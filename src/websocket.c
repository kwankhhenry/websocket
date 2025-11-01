#include "websocket.h"
#include "internal.h"
#include "ringbuffer.h"
#include "io_kqueue.h"
#include "ssl.h"
#include "parser_neon.h"
#include "os_macos.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <errno.h>
#include <openssl/sha.h>
#include <openssl/bio.h>
#include <openssl/evp.h>

// Precomputed HTTP upgrade headers (const to avoid ARC overhead if objc interop needed)
const char* const ws_http_upgrade_template = 
    "GET %s HTTP/1.1\r\n"
    "Host: %s\r\n"
    "Upgrade: websocket\r\n"
    "Connection: Upgrade\r\n"
    "Sec-WebSocket-Key: %s\r\n"
    "Sec-WebSocket-Version: 13\r\n"
    "\r\n";

struct WebSocket {
    WSState state;
    int fd;
    SSLContext* ssl_ctx;
    IOContext* io_ctx;
    
    // Ring buffers
    RingBuffer rx_ring;
    RingBuffer tx_ring;
    
    // Callbacks
    ws_on_message_t on_message;
    ws_on_error_t on_error;
    void* callback_user_data;
    void* user_data;
    
    // Connection state
    char* url;
    char* host;
    char* path;
    int port;
    bool use_ssl;
    bool cert_validation_disabled;
    
    // Handshake state
    char sec_websocket_key[25];  // Base64-encoded 16-byte key + null terminator
    
    // I/O socket index
    int socket_index;
};

// Generate WebSocket key for handshake
static void generate_websocket_key(char* key_out) {
    uint8_t random_bytes[16];
    
    // Generate random bytes
    FILE* urandom = fopen("/dev/urandom", "r");
    if (urandom) {
        fread(random_bytes, 1, 16, urandom);
        fclose(urandom);
    } else {
        // Fallback: use simple PRNG
        for (int i = 0; i < 16; i++) {
            random_bytes[i] = (uint8_t)(rand() & 0xFF);
        }
    }
    
    // Base64 encode using EVP
    EVP_ENCODE_CTX* ctx = EVP_ENCODE_CTX_new();
    if (!ctx) {
        return;
    }
    
    unsigned char encoded[32];
    int out_len = 0;
    
    EVP_EncodeInit(ctx);
    EVP_EncodeUpdate(ctx, encoded, &out_len, random_bytes, 16);
    int final_len = 0;
    EVP_EncodeFinal(ctx, encoded + out_len, &final_len);
    
    size_t total_len = out_len + final_len;
    if (total_len > 0 && total_len < 25) {
        // Remove newline if present
        if (encoded[total_len - 1] == '\n') {
            total_len--;
        }
        memcpy(key_out, encoded, total_len);
        key_out[total_len] = '\0';
    }
    
    EVP_ENCODE_CTX_free(ctx);
}

// Parse URL into components
static int parse_url(const char* url, char** host_out, char** path_out, int* port_out, bool* use_ssl_out) {
    if (!url || !host_out || !path_out || !port_out || !use_ssl_out) {
        return -1;
    }
    
    *use_ssl_out = false;
    
    // Check protocol
    if (strncmp(url, "wss://", 6) == 0) {
        *use_ssl_out = true;
        url += 6;
    } else if (strncmp(url, "ws://", 5) == 0) {
        url += 5;
    } else {
        return -1;  // Invalid protocol
    }
    
    // Find host end: look for port colon first, then path slash
    const char* host_start = url;
    const char* colon = strchr(host_start, ':');
    const char* slash = strchr(host_start, '/');
    const char* question = strchr(host_start, '?');
    
    // Host ends at first of: port colon, path slash, query param, or end of string
    const char* host_end = host_start + strlen(host_start);
    if (slash && slash < host_end) host_end = slash;
    if (question && question < host_end) host_end = question;
    
    // Extract hostname (without port)
    if (colon && colon < host_end) {
        // Port is present, extract hostname up to colon
        size_t host_len = colon - host_start;
        *host_out = malloc(host_len + 1);
        if (!*host_out) {
            return -1;
        }
        memcpy(*host_out, host_start, host_len);
        (*host_out)[host_len] = '\0';
        
        // Parse port (between colon and host_end)
        const char* port_start = colon + 1;
        size_t port_len = (host_end < port_start + 20) ? (host_end - port_start) : 20;
        char port_buf[21] = {0};
        if (port_len > 0) {
            memcpy(port_buf, port_start, port_len > 20 ? 20 : port_len);
            *port_out = atoi(port_buf);
        } else {
            *port_out = *use_ssl_out ? 443 : 80;
        }
    } else {
        // No port, extract hostname up to host_end
        size_t host_len = host_end - host_start;
        *host_out = malloc(host_len + 1);
        if (!*host_out) {
            return -1;
        }
        memcpy(*host_out, host_start, host_len);
        (*host_out)[host_len] = '\0';
        
        // Default port
        *port_out = *use_ssl_out ? 443 : 80;
    }
    
    // Parse path (include everything after host, including query strings)
    if (*host_end == '/' || *host_end == '?') {
        *path_out = strdup(host_end);
    } else {
        *path_out = strdup("/");
    }
    
    return 0;
}

// Perform TCP connect
static int tcp_connect(const char* host, int port) {
    struct addrinfo hints, *result, *rp;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);
    
    int gai_err = getaddrinfo(host, port_str, &hints, &result);
    if (gai_err != 0) {
        return -1;
    }
    
    int fd = -1;
    for (rp = result; rp != NULL; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd == -1) {
            continue;
        }
        
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) {
            break;  // Success
        }
        close(fd);
        fd = -1;
    }
    
    
    freeaddrinfo(result);
    return fd;
}

// Send HTTP upgrade request
static int send_handshake(WebSocket* ws) {
    char handshake[WS_HTTP_UPGRADE_HEADER_SIZE];
    
    // Generate WebSocket key
    generate_websocket_key(ws->sec_websocket_key);
    
    // Build handshake
    snprintf(handshake, sizeof(handshake), ws_http_upgrade_template,
             ws->path, ws->host, ws->sec_websocket_key);
    
    size_t handshake_len = strlen(handshake);
    
    // Write to tx_ring and send
    size_t written = ringbuffer_write(&ws->tx_ring, handshake, handshake_len);
    if (written != handshake_len) {
        return -1;
    }
    
    if (ws->use_ssl) {
        ssl_write(ws->ssl_ctx, &ws->tx_ring);
    } else {
        io_write(ws->io_ctx, ws->socket_index);
    }
    
    return 0;
}

// Parse HTTP response and verify handshake
static int parse_handshake_response(WebSocket* ws) {
    // Read from rx_ring
    char response[WS_MAX_HEADER_SIZE];
    size_t read = ringbuffer_read(&ws->rx_ring, response, sizeof(response) - 1);
    if (read == 0) {
        return 1;  // Need more data
    }
    
    response[read] = '\0';
    
    // Try to find HTTP response even if there's binary data first
    const char* http_start = strstr(response, "HTTP");
    if (http_start && http_start != response) {
        // Shift response to start at HTTP
        size_t offset = http_start - response;
        memmove(response, http_start, read - offset);
        response[read - offset] = '\0';
        read = read - offset;
    }
    
    // Check for "HTTP/1.1 101 Switching Protocols"
    if (strstr(response, "HTTP/1.1 101") == NULL && 
        strstr(response, "HTTP/1.0 101") == NULL) {
        return -1;  // Invalid response
    }
    
    // Check for "Upgrade: websocket"
    if (strstr(response, "Upgrade: websocket") == NULL) {
        return -1;
    }
    
    // Check for "Connection: Upgrade"
    if (strstr(response, "Connection: Upgrade") == NULL &&
        strstr(response, "Connection: upgrade") == NULL) {
        return -1;
    }
    
    // For HFT, we can skip Sec-WebSocket-Accept validation if needed
    // But we'll do basic validation
    if (strstr(response, "Sec-WebSocket-Accept") == NULL) {
        // Optional: could validate the accept key here
    }
    
    return 0;
}

// Process received WebSocket frames
static void process_frames(WebSocket* ws) {
    // Read available data from rx_ring
    char* data;
    size_t len;
    ringbuffer_read_inline(&ws->rx_ring, &data, &len);
    
    if (len == 0) {
        return;
    }
    
    // Parse frames
    size_t offset = 0;
    size_t total_consumed = 0;
    int parse_attempts = 0;
    const int max_parse_attempts = len;  // Prevent infinite loops
    
    while (offset < len && parse_attempts < max_parse_attempts) {
        parse_attempts++;
        WSFrame frame;
        int result = neon_parse_ws_frame((const uint8_t*)(data + offset), len - offset, &frame);
        
        if (result == 1) {
            // Need more data - keep what we have in buffer
            break;
        }
        
        if (result != 0) {
            // Parse error - skip one byte and try again (to recover from corruption)
            if (offset + 1 >= len) break;
            offset++;
            continue;
        }
        
        // Validate frame size to prevent infinite loops
        if (frame.header_size + frame.payload_len == 0) {
            offset++;
            continue;
        }
        
        if (frame.header_size + frame.payload_len > len - offset) {
            // Frame size exceeds available data - wait for more
            break;
        }
        
        // Process frame
        if (frame.opcode == WS_OPCODE_TEXT || frame.opcode == WS_OPCODE_BINARY) {
            // Server-to-client frames should NOT be masked per RFC 6455
            // But handle both cases for robustness
            uint8_t* payload = (uint8_t*)frame.payload;
            if (frame.mask) {
                // Unmask if present (shouldn't happen for server frames)
                for (size_t i = 0; i < frame.payload_len; i++) {
                    payload[i] ^= ((uint8_t*)(&frame.masking_key))[i % 4];
                }
            }
            
            // Call callback directly with payload pointer
            // Note: Callback should copy data if it needs to hold it, since ringbuffer may advance
            if (ws->on_message) {
                // Store frame info in user data if needed, or pass via a separate mechanism
                // For now, callback receives payload which is JSON (not a WebSocket frame)
                ws->on_message(ws, payload, frame.payload_len, ws->callback_user_data);
            }
        } else if (frame.opcode == WS_OPCODE_CLOSE) {
            ws->state = WS_STATE_CLOSING;
        }
        
        size_t frame_size = frame.header_size + frame.payload_len;
        offset += frame_size;
        total_consumed += frame_size;
    }
    
    // Advance ring buffer read pointer for consumed data
    if (total_consumed > 0) {
        // Manually advance read pointer (faster than calling ringbuffer_read with NULL)
        size_t rp = ws->rx_ring.read_ptr;
        size_t new_rp = (rp + total_consumed) % ws->rx_ring.size;
        __sync_synchronize();
        ws->rx_ring.read_ptr = new_rp;
    }
}

WebSocket* websocket_create(void) {
    WebSocket* ws = calloc(1, sizeof(WebSocket));
    if (!ws) {
        return NULL;
    }
    
    ws->state = WS_STATE_CONNECTING;
    ws->fd = -1;
    ws->socket_index = -1;
    
    return ws;
}

void websocket_destroy(WebSocket* ws) {
    if (!ws) {
        return;
    }
    
    websocket_close(ws);
    
    if (ws->ssl_ctx) {
        ssl_cleanup(ws->ssl_ctx);
    }
    
    if (ws->io_ctx) {
        io_cleanup(ws->io_ctx);
        free(ws->io_ctx);
    }
    
    ringbuffer_cleanup(&ws->rx_ring);
    ringbuffer_cleanup(&ws->tx_ring);
    
    free(ws->url);
    free(ws->host);
    free(ws->path);
    free(ws);
}

void websocket_set_on_message(WebSocket* ws, ws_on_message_t callback, void* user_data) {
    if (ws) {
        ws->on_message = callback;
        ws->callback_user_data = user_data;
    }
}

void websocket_set_on_error(WebSocket* ws, ws_on_error_t callback, void* user_data) {
    if (ws) {
        ws->on_error = callback;
        ws->callback_user_data = user_data;
    }
}

int websocket_connect(WebSocket* ws, const char* url, bool disable_cert_validation) {
    if (!ws || !url) {
        return -1;
    }
    
    ws->url = strdup(url);
    ws->cert_validation_disabled = disable_cert_validation;
    
    // Parse URL
    if (parse_url(url, &ws->host, &ws->path, &ws->port, &ws->use_ssl) != 0) {
        return -1;
    }
    
    // Initialize ring buffers
    if (ringbuffer_init(&ws->rx_ring) != 0 || ringbuffer_init(&ws->tx_ring) != 0) {
        fprintf(stderr, "ERROR: Ring buffer init failed\n");
        goto cleanup;
    }
    
    // Initialize I/O context
    ws->io_ctx = malloc(sizeof(IOContext));
    if (!ws->io_ctx || io_init(ws->io_ctx) != 0) {
        fprintf(stderr, "ERROR: I/O context init failed\n");
        goto cleanup;
    }
    
    // Connect TCP
    ws->fd = tcp_connect(ws->host, ws->port);
    if (ws->fd < 0) {
        fprintf(stderr, "ERROR: TCP connect failed for %s:%d\n", ws->host, ws->port);
        goto cleanup;
    }
    
    // Enable SO_TIMESTAMP on socket BEFORE SSL init to capture NIC timestamps
    // This is critical for measuring real NIC→SSL latency
    int timestamp_on = 1;
    #ifdef SO_TIMESTAMPNS
    if (setsockopt(ws->fd, SOL_SOCKET, SO_TIMESTAMPNS, &timestamp_on, sizeof(timestamp_on)) < 0) {
        // Fallback to SO_TIMESTAMP if SO_TIMESTAMPNS not available
        if (setsockopt(ws->fd, SOL_SOCKET, SO_TIMESTAMP, &timestamp_on, sizeof(timestamp_on)) < 0) {
            fprintf(stderr, "WARNING: Failed to enable SO_TIMESTAMP on socket\n");
        }
    }
    #else
    if (setsockopt(ws->fd, SOL_SOCKET, SO_TIMESTAMP, &timestamp_on, sizeof(timestamp_on)) < 0) {
        fprintf(stderr, "WARNING: Failed to enable SO_TIMESTAMP on socket\n");
    }
    #endif
    
    // Initialize SSL if needed
    if (ws->use_ssl) {
        if (ssl_init(&ws->ssl_ctx, disable_cert_validation) != 0) {
            goto cleanup;
        }
        
        if (ssl_connect(ws->ssl_ctx, ws->fd, ws->host) != 0) {
            ssl_cleanup(ws->ssl_ctx);
            ws->ssl_ctx = NULL;
            goto cleanup;
        }
    }
    
    // Add socket to I/O context
    ws->socket_index = io_add_socket(ws->io_ctx, ws->fd, &ws->rx_ring, &ws->tx_ring, ws);
    if (ws->socket_index < 0) {
        if (ws->use_ssl) {
            ssl_cleanup(ws->ssl_ctx);
            ws->ssl_ctx = NULL;
        }
        goto cleanup;
    }
    
    // Send handshake
    if (send_handshake(ws) != 0) {
        fprintf(stderr, "ERROR: Failed to send WebSocket handshake\n");
        goto cleanup;
    }
    
    ws->state = WS_STATE_CONNECTING;
    return 0;

cleanup:
    // Clean up resources on error
    if (ws->fd >= 0) {
        close(ws->fd);
        ws->fd = -1;
    }
    if (ws->io_ctx) {
        io_cleanup(ws->io_ctx);
        free(ws->io_ctx);
        ws->io_ctx = NULL;
    }
    ringbuffer_cleanup(&ws->rx_ring);
    ringbuffer_cleanup(&ws->tx_ring);
    return -1;
}

int websocket_send(WebSocket* ws, const uint8_t* data, size_t len, bool is_text) {
    if (!ws || !data || len == 0 || ws->state != WS_STATE_CONNECTED) {
        return -1;
    }
    
    // Build WebSocket frame
    uint8_t frame_header[14];
    size_t header_size = 2;
    
    frame_header[0] = 0x80 | (is_text ? WS_OPCODE_TEXT : WS_OPCODE_BINARY);
    frame_header[1] = 0;  // No mask (client-to-server)
    
    if (len < 126) {
        frame_header[1] |= len;
    } else if (len < 65536) {
        frame_header[1] |= 126;
        uint16_t len16 = htons((uint16_t)len);
        memcpy(frame_header + 2, &len16, 2);
        header_size = 4;
    } else {
        frame_header[1] |= 127;
        uint64_t len64 = __builtin_bswap64(len);
        memcpy(frame_header + 2, &len64, 8);
        header_size = 10;
    }
    
    // Write frame to tx_ring
    size_t written = ringbuffer_write(&ws->tx_ring, frame_header, header_size);
    if (written != header_size) {
        return -1;
    }
    
    written = ringbuffer_write(&ws->tx_ring, data, len);
    if (written != len) {
        return -1;
    }
    
    // Send via SSL or direct I/O
    if (ws->use_ssl) {
        ssl_write(ws->ssl_ctx, &ws->tx_ring);
    } else {
        io_write(ws->io_ctx, ws->socket_index);
    }
    
    return 0;
}

int websocket_close(WebSocket* ws) {
    if (!ws || ws->state == WS_STATE_CLOSED) {
        return 0;
    }
    
    if (ws->fd >= 0) {
        if (ws->socket_index >= 0) {
            io_remove_socket(ws->io_ctx, ws->fd);
        }
        close(ws->fd);
        ws->fd = -1;
    }
    
    ws->state = WS_STATE_CLOSED;
    return 0;
}

int websocket_process(WebSocket* ws) {
    if (!ws) {
        return -1;
    }
    
    struct kevent events[IO_MAX_SOCKETS];
    int event_count = io_poll(ws->io_ctx, events, IO_MAX_SOCKETS);
    
    if (event_count < 0) {
        return -1;
    }
    
    for (int i = 0; i < event_count; i++) {
        int socket_idx = (int)(intptr_t)events[i].udata;
        
        if (events[i].filter == EVFILT_READ) {
            // Read data
            // For non-SSL, io_read() captures NIC timestamp
            // For SSL, SecureTransport reads internally, so timestamp capture is limited
            if (ws->use_ssl) {
                // SSL reads from socket internally via SecureTransport
                // Timestamp capture is handled within ssl_read via recvmsg()
                ssl_read(ws->ssl_ctx, &ws->rx_ring);
            } else {
                io_read(ws->io_ctx, socket_idx);
            }
            
            // Process handshake if connecting
            if (ws->state == WS_STATE_CONNECTING) {
                // For SSL, continue handshake and ensure data flows
                if (ws->use_ssl) {
                    // Read any available SSL data first
                    ssl_read(ws->ssl_ctx, &ws->rx_ring);
                    // Then write any pending data
                    ssl_write(ws->ssl_ctx, &ws->tx_ring);
                }
                
                int result = parse_handshake_response(ws);
                if (result == 0) {
                    ws->state = WS_STATE_CONNECTED;
                } else if (result < 0) {
                    if (ws->on_error) {
                        ws->on_error(ws, -1, "Handshake failed", ws->callback_user_data);
                    }
                    websocket_close(ws);
                }
            } else if (ws->state == WS_STATE_CONNECTED) {
                // Process WebSocket frames
                process_frames(ws);
            }
        }
    }
    
    return event_count;
}

WSState websocket_get_state(WebSocket* ws) {
    return ws ? ws->state : WS_STATE_CLOSED;
}

void* websocket_get_user_data(WebSocket* ws) {
    return ws ? ws->user_data : NULL;
}

uint64_t websocket_get_last_nic_timestamp_ns(WebSocket* ws) {
    if (!ws || ws->socket_index < 0) {
        return 0;
    }
    // For SSL connections, get timestamp from SSL context
    if (ws->use_ssl && ws->ssl_ctx) {
        return ssl_get_last_nic_timestamp_ns(ws->ssl_ctx);
    }
    // For non-SSL, get from I/O context
    return io_get_last_nic_timestamp_ns(ws->io_ctx, ws->socket_index);
}

uint64_t websocket_get_last_nic_timestamp_ticks(WebSocket* ws) {
    if (!ws || ws->socket_index < 0) {
        return 0;
    }
    // For SSL connections, get timestamp from SSL context
    if (ws->use_ssl && ws->ssl_ctx) {
        return ssl_get_last_nic_timestamp_ticks(ws->ssl_ctx);
    }
    // For non-SSL, get from I/O context
    return io_get_last_nic_timestamp_ticks(ws->io_ctx, ws->socket_index);
}

