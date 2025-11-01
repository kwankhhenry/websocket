#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "../src/io_kqueue.h"
#include "../src/ringbuffer.h"

#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "FAIL: %s\n", msg); \
            exit(1); \
        } \
    } while(0)

// Test kqueue initialization
void test_kqueue_init(void) {
    IOContext ctx;
    TEST_ASSERT(io_init(&ctx) == 0, "kqueue init failed");
    TEST_ASSERT(ctx.kq >= 0, "kqueue fd invalid");
    TEST_ASSERT(ctx.socket_count == 0, "Socket count should be 0");
    
    io_cleanup(&ctx);
    printf("PASS: kqueue initialization test\n");
}

// Test socket addition
void test_socket_addition(void) {
    IOContext ctx;
    TEST_ASSERT(io_init(&ctx) == 0, "kqueue init failed");
    
    RingBuffer rx_ring, tx_ring;
    TEST_ASSERT(ringbuffer_init(&rx_ring) == 0, "RX ring init failed");
    TEST_ASSERT(ringbuffer_init(&tx_ring) == 0, "TX ring init failed");
    
    // Create a test socket (pair for testing)
    int fds[2];
    TEST_ASSERT(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "socketpair failed");
    
    int idx = io_add_socket(&ctx, fds[0], &rx_ring, &tx_ring, NULL);
    TEST_ASSERT(idx >= 0, "Socket addition failed");
    TEST_ASSERT(ctx.socket_count == 1, "Socket count incorrect");
    
    io_remove_socket(&ctx, fds[0]);
    close(fds[0]);
    close(fds[1]);
    
    ringbuffer_cleanup(&rx_ring);
    ringbuffer_cleanup(&tx_ring);
    io_cleanup(&ctx);
    
    printf("PASS: Socket addition test\n");
}

// Test non-blocking I/O
void test_non_blocking_io(void) {
    IOContext ctx;
    TEST_ASSERT(io_init(&ctx) == 0, "kqueue init failed");
    
    RingBuffer rx_ring, tx_ring;
    TEST_ASSERT(ringbuffer_init(&rx_ring) == 0, "RX ring init failed");
    TEST_ASSERT(ringbuffer_init(&tx_ring) == 0, "TX ring init failed");
    
    int fds[2];
    TEST_ASSERT(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "socketpair failed");
    
    int idx = io_add_socket(&ctx, fds[0], &rx_ring, &tx_ring, NULL);
    TEST_ASSERT(idx >= 0, "Socket addition failed");
    
    // Verify socket is non-blocking
    int flags = fcntl(fds[0], F_GETFL, 0);
    TEST_ASSERT((flags & O_NONBLOCK) != 0, "Socket should be non-blocking");
    
    // Poll (should return 0 events initially)
    struct kevent events[10];
    int event_count = io_poll(&ctx, events, 10);
    TEST_ASSERT(event_count >= 0, "Poll failed");
    
    io_remove_socket(&ctx, fds[0]);
    close(fds[0]);
    close(fds[1]);
    
    ringbuffer_cleanup(&rx_ring);
    ringbuffer_cleanup(&tx_ring);
    io_cleanup(&ctx);
    
    printf("PASS: Non-blocking I/O test\n");
}

// Test event ordering (kqueue edge-triggered behavior)
void test_event_ordering(void) {
    IOContext ctx;
    TEST_ASSERT(io_init(&ctx) == 0, "kqueue init failed");
    
    RingBuffer rx_ring, tx_ring;
    TEST_ASSERT(ringbuffer_init(&rx_ring) == 0, "RX ring init failed");
    TEST_ASSERT(ringbuffer_init(&tx_ring) == 0, "TX ring init failed");
    
    int fds[2];
    TEST_ASSERT(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "socketpair failed");
    
    int idx = io_add_socket(&ctx, fds[0], &rx_ring, &tx_ring, NULL);
    TEST_ASSERT(idx >= 0, "Socket addition failed");
    
    // Write data to trigger read event
    const char* test_data = "Hello, World!";
    send(fds[1], test_data, strlen(test_data), 0);
    
    // Poll for events
    struct kevent events[10];
    int event_count = io_poll(&ctx, events, 10);
    
    // Should have at least one read event
    TEST_ASSERT(event_count > 0, "No events received");
    
    bool found_read = false;
    for (int i = 0; i < event_count; i++) {
        if (events[i].filter == EVFILT_READ) {
            found_read = true;
            break;
        }
    }
    TEST_ASSERT(found_read, "Read event not found");
    
    io_remove_socket(&ctx, fds[0]);
    close(fds[0]);
    close(fds[1]);
    
    ringbuffer_cleanup(&rx_ring);
    ringbuffer_cleanup(&tx_ring);
    io_cleanup(&ctx);
    
    printf("PASS: Event ordering test\n");
}

// Test zero-copy read
void test_zero_copy_read(void) {
    IOContext ctx;
    TEST_ASSERT(io_init(&ctx) == 0, "kqueue init failed");
    
    RingBuffer rx_ring, tx_ring;
    TEST_ASSERT(ringbuffer_init(&rx_ring) == 0, "RX ring init failed");
    TEST_ASSERT(ringbuffer_init(&tx_ring) == 0, "TX ring init failed");
    
    int fds[2];
    TEST_ASSERT(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "socketpair failed");
    
    int idx = io_add_socket(&ctx, fds[0], &rx_ring, &tx_ring, NULL);
    TEST_ASSERT(idx >= 0, "Socket addition failed");
    
    // Write test data
    const char* test_data = "Zero-copy test data";
    size_t data_len = strlen(test_data);
    send(fds[1], test_data, data_len, 0);
    
    // Read directly into ring buffer
    ssize_t read_bytes = io_read(&ctx, idx);
    TEST_ASSERT(read_bytes > 0, "Zero-copy read failed");
    
    // Verify data in ring buffer
    size_t readable = ringbuffer_readable(&rx_ring);
    TEST_ASSERT(readable >= data_len, "Data not in ring buffer");
    
    char read_buf[256];
    size_t read = ringbuffer_read(&rx_ring, read_buf, sizeof(read_buf));
    TEST_ASSERT(read >= data_len, "Ring buffer read failed");
    read_buf[read] = '\0';
    TEST_ASSERT(memcmp(read_buf, test_data, data_len) == 0, "Data mismatch");
    
    io_remove_socket(&ctx, fds[0]);
    close(fds[0]);
    close(fds[1]);
    
    ringbuffer_cleanup(&rx_ring);
    ringbuffer_cleanup(&tx_ring);
    io_cleanup(&ctx);
    
    printf("PASS: Zero-copy read test\n");
}

int main(void) {
    printf("Running kqueue unit tests...\n\n");
    
    test_kqueue_init();
    test_socket_addition();
    test_non_blocking_io();
    test_event_ordering();
    test_zero_copy_read();
    
    printf("\nAll kqueue tests passed!\n");
    return 0;
}

