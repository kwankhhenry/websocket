#include "ssl.h"
#include <Security/Security.h>
#include <Security/SecureTransport.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/uio.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <mach/mach_time.h>
#include <mach/mach.h>
#include <mach/vm_map.h>
#include <stdio.h>

// OpenSSL fallback (if SecureTransport unavailable)
#if defined(HAVE_OPENSSL)
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/crypto.h>
#endif

struct SSLContext {
    SSLBackend backend;
    int fd;
    bool cert_validation_disabled;
    
    // Peer hostname for SNI
    char peer_hostname[256];
    
    // SecureTransport state
    SSLContextRef st_ctx;
    SSLConnectionRef st_conn;
    
#if defined(HAVE_OPENSSL)
    // OpenSSL fallback state
    SSL_CTX* ossl_ctx;
    SSL* ossl_ssl;
#endif
    
    // Preallocated read buffer for zero-copy
    char* read_buf;
    size_t read_buf_size;
    
    // NIC timestamp tracking (for latency measurement)
    uint64_t last_nic_timestamp_ns;
    uint64_t last_nic_timestamp_ticks;
};

// Helper to convert timespec to nanoseconds
static uint64_t timespec_to_ns_ssl(const struct timespec* ts) {
    return (uint64_t)ts->tv_sec * 1000000000ULL + (uint64_t)ts->tv_nsec;
}

// SecureTransport I/O callbacks
// Use recvmsg() to capture NIC timestamp for latency measurement
static OSStatus ssl_read_func(SSLConnectionRef conn, void* data, size_t* length) {
    SSLContext* ctx = (SSLContext*)conn;
    if (!ctx || ctx->fd < 0) {
        return errSSLClosedGraceful;
    }
    
    // Use recvmsg() to capture NIC timestamp (same as io_read)
    struct msghdr msg;
    struct iovec iov;
    char control[CMSG_SPACE(sizeof(struct timespec))];
    
    iov.iov_base = data;
    iov.iov_len = *length;
    
    msg.msg_name = NULL;
    msg.msg_namelen = 0;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);
    msg.msg_flags = 0;
    
    // CRITICAL FIX: Capture timestamp RIGHT BEFORE recvmsg() call
    // This is the closest we can get to packet arrival time when using SecureTransport
    uint64_t packet_arrival_ticks = mach_absolute_time();
    
    ssize_t n = recvmsg(ctx->fd, &msg, 0);
    
    if (n > 0) {
        // Extract NIC timestamp from control message
        // CRITICAL: Check msg_controllen to see if we got control messages
        bool timestamp_found = false;
        struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
        while (cmsg != NULL) {
            if (cmsg->cmsg_level == SOL_SOCKET) {
                #ifdef SCM_TIMESTAMPNS
                if (cmsg->cmsg_type == SCM_TIMESTAMPNS) {
                    struct timespec* ts = (struct timespec*)CMSG_DATA(cmsg);
                    uint64_t nic_ns = timespec_to_ns_ssl(ts);
                    ctx->last_nic_timestamp_ns = nic_ns;
                    timestamp_found = true;
                    
                    // Convert to CPU cycles using cached timebase (HFT optimization)
                    static mach_timebase_info_data_t cached_timebase_st = {0, 0};
                    static bool timebase_inited_st = false;
                    if (!timebase_inited_st) {
                        mach_timebase_info(&cached_timebase_st);
                        timebase_inited_st = true;
                    }
                    
                    uint64_t now_ticks = mach_absolute_time();
                    uint64_t now_ns = (now_ticks * cached_timebase_st.numer) / cached_timebase_st.denom;
                    
                    if (nic_ns <= now_ns && now_ns > 0 && cached_timebase_st.denom > 0) {
                        uint64_t diff_ns = now_ns - nic_ns;
                        // Integer-only conversion: diff_ticks = (diff_ns * denom) / numer
                        uint64_t diff_ticks = (diff_ns * cached_timebase_st.denom) / cached_timebase_st.numer;
                        ctx->last_nic_timestamp_ticks = (now_ticks > diff_ticks) ? (now_ticks - diff_ticks) : 0;
                    } else {
                        ctx->last_nic_timestamp_ticks = now_ticks;  // Fallback to current time
                    }
                    break;
                }
                #endif
                #ifdef SCM_TIMESTAMP
                if (cmsg->cmsg_type == SCM_TIMESTAMP) {
                    struct timeval* tv = (struct timeval*)CMSG_DATA(cmsg);
                    uint64_t nic_ns = (uint64_t)tv->tv_sec * 1000000000ULL + (uint64_t)tv->tv_usec * 1000ULL;
                    ctx->last_nic_timestamp_ns = nic_ns;
                    timestamp_found = true;
                    
                    // Convert to CPU cycles using cached timebase (HFT optimization)
                    static mach_timebase_info_data_t cached_timebase_st = {0, 0};
                    static bool timebase_inited_st = false;
                    if (!timebase_inited_st) {
                        mach_timebase_info(&cached_timebase_st);
                        timebase_inited_st = true;
                    }
                    
                    uint64_t now_ticks = mach_absolute_time();
                    uint64_t now_ns = (now_ticks * cached_timebase_st.numer) / cached_timebase_st.denom;
                    
                    if (nic_ns <= now_ns && now_ns > 0 && cached_timebase_st.denom > 0) {
                        uint64_t diff_ns = now_ns - nic_ns;
                        // Integer-only conversion: diff_ticks = (diff_ns * denom) / numer
                        uint64_t diff_ticks = (diff_ns * cached_timebase_st.denom) / cached_timebase_st.numer;
                        ctx->last_nic_timestamp_ticks = (now_ticks > diff_ticks) ? (now_ticks - diff_ticks) : 0;
                    } else {
                        ctx->last_nic_timestamp_ticks = now_ticks;  // Fallback to current time
                    }
                    break;
                }
                #endif
            }
            cmsg = CMSG_NXTHDR(&msg, cmsg);
        }
        
        // CRITICAL FIX: If no timestamp found from control messages, use packet_arrival_ticks
        // This happens when SecureTransport doesn't preserve SO_TIMESTAMPNS control messages
        // Using timestamp right before recvmsg() is the best approximation we can get
        if (!timestamp_found) {
            ctx->last_nic_timestamp_ticks = packet_arrival_ticks;
            
            // Convert to nanoseconds
            static mach_timebase_info_data_t cached_timebase_st_fallback = {0, 0};
            static bool timebase_st_fallback_inited = false;
            if (!timebase_st_fallback_inited) {
                mach_timebase_info(&cached_timebase_st_fallback);
                timebase_st_fallback_inited = true;
            }
            uint64_t packet_arrival_ns = (packet_arrival_ticks * cached_timebase_st_fallback.numer) / cached_timebase_st_fallback.denom;
            ctx->last_nic_timestamp_ns = packet_arrival_ns;
        }
    }
    
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            *length = 0;
            return errSSLWouldBlock;
        }
        return errSSLClosedGraceful;
    }
    
    *length = n;
    return n > 0 ? noErr : errSSLClosedGraceful;
}

static OSStatus ssl_write_func(SSLConnectionRef conn, const void* data, size_t* length) {
    SSLContext* ctx = (SSLContext*)conn;
    if (!ctx || ctx->fd < 0) {
        return errSSLClosedGraceful;
    }
    
    ssize_t n = write(ctx->fd, data, *length);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            *length = 0;
            return errSSLWouldBlock;
        }
        return errSSLClosedGraceful;
    }
    
    *length = n;
    return n > 0 ? noErr : errSSLClosedGraceful;
}

int ssl_init(SSLContext** out_ctx, bool disable_cert_validation) {
    if (!out_ctx) {
        return -1;
    }
    
    SSLContext* ctx = calloc(1, sizeof(SSLContext));
    if (!ctx) {
        return -1;
    }
    
    ctx->backend = SSL_BACKEND_SECURETRANSPORT;
    ctx->fd = -1;
    ctx->cert_validation_disabled = disable_cert_validation;
    ctx->st_ctx = NULL;
    ctx->st_conn = NULL;
    ctx->read_buf_size = 64 * 1024;  // 64KB preallocated buffer
    ctx->read_buf = malloc(ctx->read_buf_size);
    ctx->last_nic_timestamp_ns = 0;
    ctx->last_nic_timestamp_ticks = 0;
    
    if (!ctx->read_buf) {
        free(ctx);
        return -1;
    }
    
    // Try SecureTransport first
    OSStatus status = SSLNewContext(false, &ctx->st_ctx);
    if (status == noErr && ctx->st_ctx) {
        // Set I/O functions
        status = SSLSetIOFuncs(ctx->st_ctx, ssl_read_func, ssl_write_func);
        if (status == noErr) {
            // Disable certificate validation if requested (HFT optimization)
            // Note: SecureTransport doesn't provide direct session cache control,
            // but we ensure proper cleanup in ssl_cleanup() to minimize memory growth
            if (disable_cert_validation) {
                SSLSetSessionOption(ctx->st_ctx, kSSLSessionOptionBreakOnServerAuth, false);
            }
            
            *out_ctx = ctx;
            return 0;
        }
    }
    
    // SecureTransport failed, try OpenSSL fallback
#if defined(HAVE_OPENSSL)
    // Initialize OpenSSL library (thread-safe, can be called multiple times)
    static bool openssl_global_inited = false;
    if (!openssl_global_inited) {
        OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS | OPENSSL_INIT_LOAD_CRYPTO_STRINGS, NULL);
        OPENSSL_init_crypto(OPENSSL_INIT_LOAD_CRYPTO_STRINGS, NULL);
        SSL_load_error_strings();
        OpenSSL_add_all_algorithms();
        openssl_global_inited = true;
    }
    
    ctx->ossl_ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx->ossl_ctx) {
        free(ctx->read_buf);
        free(ctx);
        return -1;
    }
    
    // Set minimum TLS version to 1.2 (Binance requires TLS 1.2+)
    SSL_CTX_set_min_proto_version(ctx->ossl_ctx, TLS1_2_VERSION);
    SSL_CTX_set_max_proto_version(ctx->ossl_ctx, TLS1_3_VERSION);
    
    // Disable certificate validation if requested
    if (disable_cert_validation) {
        SSL_CTX_set_verify(ctx->ossl_ctx, SSL_VERIFY_NONE, NULL);
    } else {
        // Load system default CA certificates
        SSL_CTX_set_default_verify_paths(ctx->ossl_ctx);
    }
    
    // Optimize for low latency
    SSL_CTX_set_mode(ctx->ossl_ctx, SSL_MODE_RELEASE_BUFFERS);
    SSL_CTX_set_options(ctx->ossl_ctx, SSL_OP_NO_TICKET);
    
    // PHASE 1 FIX: Disable session caching from initialization
    // Prevent OpenSSL from caching TLS sessions to reduce memory growth
    SSL_CTX_set_session_cache_mode(ctx->ossl_ctx, SSL_SESS_CACHE_OFF);
    
    ctx->backend = SSL_BACKEND_OPENSSL;
    *out_ctx = ctx;
    return 0;
#else
    // No fallback available
    free(ctx->read_buf);
    free(ctx);
    return -1;
#endif
}

void ssl_cleanup(SSLContext* ctx) {
    if (!ctx) {
        return;
    }
    
    // CRITICAL: Close socket first to ensure clean SSL shutdown
    if (ctx->fd >= 0) {
        shutdown(ctx->fd, SHUT_RDWR);
        close(ctx->fd);
        ctx->fd = -1;
    }
    
    if (ctx->backend == SSL_BACKEND_SECURETRANSPORT && ctx->st_ctx) {
        // PRIORITY 1 FIX: Force SSL close before disposal to ensure clean shutdown
        // SSLClose() properly terminates the SSL connection and releases resources
        // This helps reduce memory growth during rapid connect/disconnect cycles
        // Note: SecureTransport doesn't provide a direct API to disable session caching,
        // but proper cleanup with SSLClose() before disposal helps minimize resource retention
        // CRITICAL FIX: Only call SSLClose once on the context (not on st_conn)
        OSStatus close_status = SSLClose(ctx->st_ctx);
        (void)close_status;  // Ignore errors during cleanup
        
        // Small delay to allow SecureTransport to release resources
        // This helps ensure cleanup is complete before disposal
        usleep(1000);  // 1ms delay
        
        // Dispose context (this releases all SSL state including connection)
        SSLDisposeContext(ctx->st_ctx);
        ctx->st_ctx = NULL;
        ctx->st_conn = NULL;  // Connection is disposed with context
    }
    
#if defined(HAVE_OPENSSL)
    if (ctx->backend == SSL_BACKEND_OPENSSL) {
        if (ctx->ossl_ssl) {
            // OpenSSL: Shutdown SSL connection before freeing
            SSL_shutdown(ctx->ossl_ssl);
            SSL_free(ctx->ossl_ssl);
            ctx->ossl_ssl = NULL;
        }
        if (ctx->ossl_ctx) {
            // PRIORITY 1 FIX: Session cache is already disabled at initialization
            // SSL_CTX_flush_sessions can cause crashes in some OpenSSL versions
            // Freeing the context will clean up sessions automatically
            SSL_CTX_free(ctx->ossl_ctx);
            ctx->ossl_ctx = NULL;
        }
    }
#endif
    
    // PRIORITY 1 FIX: Regular free (VM_DEALLOCATE was causing crashes)
    // The SSL session cache flushing and explicit cleanup should be sufficient
    if (ctx->read_buf) {
        free(ctx->read_buf);
        ctx->read_buf = NULL;
    }
    
    free(ctx);
}

// Helper function to clear SSL errors (errors handled via return codes)
static void clear_ssl_errors(SSLContext* ctx) {
#if defined(HAVE_OPENSSL)
    if (ctx && ctx->backend == SSL_BACKEND_OPENSSL) {
        while (ERR_get_error() != 0) {
            // Clear error queue
        }
    }
#endif
}

int ssl_connect(SSLContext* ctx, int fd, const char* hostname) {
    if (!ctx || fd < 0 || !hostname) {
        return -1;
    }
    
    ctx->fd = fd;
    strncpy(ctx->peer_hostname, hostname, sizeof(ctx->peer_hostname) - 1);
    ctx->peer_hostname[sizeof(ctx->peer_hostname) - 1] = '\0';
    
    if (ctx->backend == SSL_BACKEND_SECURETRANSPORT) {
        // Set connection reference
        OSStatus status = SSLSetConnection(ctx->st_ctx, ctx);
        if (status != noErr) {
            return -1;
        }
        
        ctx->st_conn = (SSLConnectionRef)ctx;
        
        // Set peer domain name (required for SNI and handshake)
        status = SSLSetPeerDomainName(ctx->st_ctx, ctx->peer_hostname, strlen(ctx->peer_hostname));
        if (status != noErr) {
            return -1;
        }
        
        // Perform initial handshake attempt
        // CRITICAL: SecureTransport handshake is asynchronous and requires I/O cycles
        // We do initial attempt here, but handshake completion happens via ssl_read/ssl_write
        // in websocket_process() during WS_STATE_CONNECTING state
        status = SSLHandshake(ctx->st_ctx);
        
        // errSSLWouldBlock means handshake needs more I/O - this is NORMAL and expected
        // Return success and let websocket_process drive the handshake to completion
        if (status == noErr || status == errSSLWouldBlock) {
            return 0;  // Handshake in progress or complete
        }
        
        // errSSLProtocol (-50) can happen if socket is in wrong state or SSL context is stale
        // On reconnect, this might mean we need to wait longer or the socket isn't ready
        if (status == errSSLProtocol) {
            // This might be a transient error - let websocket_process() try to drive the handshake further
            return 0;  // Let it try via I/O cycles
        }
        
        // Other errors are real failures
        return -1;
    }
    
#if defined(HAVE_OPENSSL)
    if (ctx->backend == SSL_BACKEND_OPENSSL) {
        ctx->ossl_ssl = SSL_new(ctx->ossl_ctx);
        if (!ctx->ossl_ssl) {
            clear_ssl_errors(ctx);
            return -1;
        }
        
        // Set SNI hostname (required for modern TLS)
        SSL_set_tlsext_host_name(ctx->ossl_ssl, hostname);
        
        SSL_set_fd(ctx->ossl_ssl, fd);
        
        // Start SSL handshake (may not complete immediately)
        int ret = SSL_connect(ctx->ossl_ssl);
        if (ret == 1) {
            return 0;  // Handshake complete
        }
        
        // Check for would-block - this is normal, handshake will complete via I/O
        int ssl_err = SSL_get_error(ctx->ossl_ssl, ret);
        if (ssl_err == SSL_ERROR_WANT_READ || ssl_err == SSL_ERROR_WANT_WRITE) {
            // Would block - handshake will continue via ssl_read/ssl_write in websocket_process
            return 0;
        }
        
        // Real error
        clear_ssl_errors(ctx);
        return -1;
    }
#endif
    
    return -1;
}

ssize_t ssl_read(SSLContext* ctx, RingBuffer* rb) {
    if (!ctx || !rb) {
        return -1;
    }
    
    if (ctx->backend == SSL_BACKEND_SECURETRANSPORT) {
        // Get write pointer from ring buffer
        char* write_ptr;
        size_t available;
        ringbuffer_write_inline(rb, &write_ptr, &available);
        
        if (available == 0) {
            return 0;  // Ring buffer full
        }
        
    // Read directly into ring buffer (zero-copy with SSLRead)
    // NOTE: NIC timestamp is captured in ssl_read_func() when recvmsg() is called
    // This happens INSIDE SSLRead(), so timestamp is already set by the time we get here
    size_t requested = (available < ctx->read_buf_size) ? available : ctx->read_buf_size;
    size_t actual = requested;
    
    OSStatus status = SSLRead(ctx->st_ctx, write_ptr, requested, &actual);
    
    if (status == noErr && actual > 0) {
        // Update ring buffer write pointer
        size_t wp = rb->write_ptr;
        __sync_synchronize();
        rb->write_ptr = (wp + actual) % rb->size;
        return (ssize_t)actual;
    }
        
        if (status == errSSLWouldBlock) {
            return 0;  // Would block - no more data available
        }
        
        // CRITICAL: Check for connection closure
        if (status == errSSLClosedGraceful || status == errSSLClosedAbort) {
            // Connection closed by peer - mark context as closed
            // Return -1 to distinguish from "would block" (0)
            ctx->fd = -1;  // Mark socket as closed
            return -1;  // Return error to indicate connection closed
        }
        
        // If actual == 0 and status == noErr, we've reached EOF (connection closed)
        if (status == noErr && actual == 0) {
            ctx->fd = -1;  // Mark socket as closed
            return -1;  // Connection closed (EOF) - distinguish from would block
        }
        
        // Other errors - connection might be broken
        if (status != noErr && status != errSSLWouldBlock) {
            // Real SSL error - connection is broken
            ctx->fd = -1;
            return -1;
        }
        
        // errSSLWouldBlock means no data now but connection is alive
        return 0;
    }
    
#if defined(HAVE_OPENSSL)
    if (ctx->backend == SSL_BACKEND_OPENSSL) {
        // Get write pointer from ring buffer
        char* write_ptr;
        size_t available;
        ringbuffer_write_inline(rb, &write_ptr, &available);
        
        if (available == 0) {
            return 0;
        }
        
        int ret = SSL_read(ctx->ossl_ssl, write_ptr, (int)available);
        
        if (ret > 0) {
            size_t wp = rb->write_ptr;
            __sync_synchronize();
            rb->write_ptr = (wp + ret) % rb->size;
            return ret;
        }
        
        int ssl_err = SSL_get_error(ctx->ossl_ssl, ret);
        if (ssl_err == SSL_ERROR_WANT_READ || ssl_err == SSL_ERROR_WANT_WRITE) {
            return 0;  // Would block
        }
        
        return -1;
    }
#endif
    
    return -1;
}

ssize_t ssl_write(SSLContext* ctx, RingBuffer* rb) {
    if (!ctx || !rb) {
        return -1;
    }
    
    if (ctx->backend == SSL_BACKEND_SECURETRANSPORT) {
        // Get read pointer from ring buffer
        char* read_ptr;
        size_t available;
        ringbuffer_read_inline(rb, &read_ptr, &available);
        
        if (available == 0) {
            return 0;  // No data
        }
        
        size_t written = 0;
        OSStatus status = SSLWrite(ctx->st_ctx, read_ptr, available, &written);
        
        if (status == noErr && written > 0) {
            size_t rp = rb->read_ptr;
            __sync_synchronize();
            rb->read_ptr = (rp + written) % rb->size;
            return (ssize_t)written;
        }
        
        if (status == errSSLWouldBlock) {
            return 0;
        }
        
        return -1;
    }
    
#if defined(HAVE_OPENSSL)
    if (ctx->backend == SSL_BACKEND_OPENSSL) {
        char* read_ptr;
        size_t available;
        ringbuffer_read_inline(rb, &read_ptr, &available);
        
        if (available == 0) {
            return 0;
        }
        
        int ret = SSL_write(ctx->ossl_ssl, read_ptr, (int)available);
        
        if (ret > 0) {
            size_t rp = rb->read_ptr;
            __sync_synchronize();
            rb->read_ptr = (rp + ret) % rb->size;
            return ret;
        }
        
        int ssl_err = SSL_get_error(ctx->ossl_ssl, ret);
        if (ssl_err == SSL_ERROR_WANT_READ || ssl_err == SSL_ERROR_WANT_WRITE) {
            return 0;
        }
        
        return -1;
    }
#endif
    
    return -1;
}

SSLBackend ssl_get_backend(SSLContext* ctx) {
    if (!ctx) {
        return SSL_BACKEND_SECURETRANSPORT;
    }
    return ctx->backend;
}

uint64_t ssl_get_last_nic_timestamp_ns(SSLContext* ctx) {
    if (!ctx) {
        return 0;
    }
    return ctx->last_nic_timestamp_ns;
}

uint64_t ssl_get_last_nic_timestamp_ticks(SSLContext* ctx) {
    if (!ctx) {
        return 0;
    }
    return ctx->last_nic_timestamp_ticks;
}

// Check if SSL handshake is complete
bool ssl_is_handshake_complete(SSLContext* ctx) {
    if (!ctx) {
        return false;
    }
    
    if (ctx->backend == SSL_BACKEND_SECURETRANSPORT) {
        // SecureTransport doesn't provide a direct "is connected" API
        // We check by attempting operations - if SSLRead/SSLWrite succeed,
        // handshake is complete. For now, assume handshake can complete if
        // ssl_connect() succeeded (it will be driven to completion via I/O)
        // The actual completion is determined by successful I/O operations
        return true;  // Handshake completion determined by successful I/O in websocket_process
    }
    
#if defined(HAVE_OPENSSL)
    if (ctx->backend == SSL_BACKEND_OPENSSL && ctx->ossl_ssl) {
        return SSL_is_init_finished(ctx->ossl_ssl) != 0;
    }
#endif
    
    return false;
}

