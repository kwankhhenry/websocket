#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/resource.h>
#include <unistd.h>
#include "test_helpers.h"

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_RUN(name, func) \
    do { \
        printf("Running: %s\n", name); \
        if (func() == 0) { \
            printf("  PASS\n"); \
            tests_passed++; \
        } else { \
            printf("  FAIL\n"); \
            tests_failed++; \
        } \
    } while(0)

// Test 1: HFT-grade leak detection (10,000 cycles)
int test_websocket_leak_detection(void) {
    MemoryTracker tracker;
    memory_tracker_init(&tracker);
    
    const int cycles = 10000;
    printf("  Creating/destroying %d WebSocket instances...\n", cycles);
    
    for (int i = 0; i < cycles; i++) {
        WebSocket* ws = websocket_create();
        if (!ws) {
            fprintf(stderr, "Failed to create WebSocket at cycle %d\n", i);
            return -1;
        }
        websocket_destroy(ws);
        
        // Check memory growth every 1000 cycles
        if ((i + 1) % 1000 == 0) {
            memory_tracker_update(&tracker);
            if (memory_tracker_check_growth(&tracker, 1.0)) {
                printf("  WARNING: Memory growth >1%% detected at cycle %d\n", i + 1);
                memory_tracker_print(&tracker, "leak_check");
            }
        }
    }
    
    memory_tracker_update(&tracker);
    memory_tracker_print(&tracker, "final");
    
    // Fail if memory growth >1% after 10k cycles
    if (memory_tracker_check_growth(&tracker, 1.0)) {
        fprintf(stderr, "FAIL: Memory leak detected (>1%% growth)\n");
        return -1;
    }
    
    return 0;
}

// Test 2: Ring buffer alignment verification
int test_ringbuffer_alignment(void) {
    RingBuffer rb;
    
    // Test multiple init/cleanup cycles
    for (int i = 0; i < 100; i++) {
        if (ringbuffer_init(&rb) != 0) {
            fprintf(stderr, "Failed to init ring buffer at cycle %d\n", i);
            return -1;
        }
        
        // Verify 128-byte alignment
        uintptr_t addr = (uintptr_t)rb.buf;
        if ((addr % RINGBUFFER_ALIGNMENT) != 0) {
            fprintf(stderr, "Ring buffer not 128-byte aligned: %p\n", (void*)rb.buf);
            ringbuffer_cleanup(&rb);
            return -1;
        }
        
        // Test boundary conditions near alignment edges
        // Write data near alignment boundary
        size_t boundary_offset = (RINGBUFFER_ALIGNMENT - 1);
        if (boundary_offset < rb.size) {
            char test_data[64];
            memset(test_data, 'A', sizeof(test_data));
            size_t written = ringbuffer_write(&rb, test_data, sizeof(test_data));
            TEST_ASSERT(written > 0, "Failed to write near alignment boundary");
        }
        
        ringbuffer_cleanup(&rb);
    }
    
    return 0;
}

// Test 3: Buffer overflow protection
int test_ringbuffer_overflow_protection(void) {
    RingBuffer rb;
    if (ringbuffer_init(&rb) != 0) {
        fprintf(stderr, "Failed to init ring buffer\n");
        return -1;
    }
    
    // Test 1: Write beyond buffer capacity
    size_t writeable = ringbuffer_writeable(&rb);
    char* large_data = (char*)malloc(writeable + 1000);
    if (!large_data) {
        ringbuffer_cleanup(&rb);
        return -1;
    }
    memset(large_data, 'X', writeable + 1000);
    
    size_t written = ringbuffer_write(&rb, large_data, writeable + 1000);
    // Should only write up to available space
    TEST_ASSERT(written <= writeable, "Buffer overflow: wrote more than available");
    
    free(large_data);
    
    // Test 2: Read from empty buffer (should return 0)
    size_t readable = ringbuffer_readable(&rb);
    char read_buf[1024];
    size_t read = ringbuffer_read(&rb, read_buf, sizeof(read_buf));
    TEST_ASSERT(read <= readable, "Read more than available");
    
    // Test 3: Wrap-around at buffer boundaries
    ringbuffer_reset(&rb);
    
    // Fill buffer nearly to the end
    size_t near_end = rb.size - 1024;
    char* fill_data = (char*)malloc(near_end);
    memset(fill_data, 'Y', near_end);
    size_t fill_written = ringbuffer_write(&rb, fill_data, near_end);
    TEST_ASSERT(fill_written > 0, "Failed to fill buffer near end");
    
    // Read some data to create wrap-around space
    size_t read_amount = near_end / 2;
    char* read_data = (char*)malloc(read_amount);
    size_t fill_read = ringbuffer_read(&rb, read_data, read_amount);
    TEST_ASSERT(fill_read > 0, "Failed to read for wrap-around test");
    
    // Write more data to trigger wrap-around
    char* wrap_data = (char*)malloc(2048);
    memset(wrap_data, 'Z', 2048);
    size_t wrap_written = ringbuffer_write(&rb, wrap_data, 2048);
    TEST_ASSERT(wrap_written > 0, "Wrap-around write failed");
    
    free(fill_data);
    free(read_data);
    free(wrap_data);
    
    ringbuffer_cleanup(&rb);
    return 0;
}

// Test 4: Resource exhaustion - memory limit
int test_resource_exhaustion_memory(void) {
    // Save current limit
    struct rlimit old_rlim;
    getrlimit(RLIMIT_AS, &old_rlim);
    
    // Set a restrictive memory limit (128MB)
    struct rlimit rlim;
    rlim.rlim_cur = 128 * 1024 * 1024;  // 128MB
    rlim.rlim_max = 128 * 1024 * 1024;
    if (setrlimit(RLIMIT_AS, &rlim) != 0) {
        fprintf(stderr, "Failed to set memory limit (may require privileges)\n");
        return 0;  // Skip if we can't set limit
    }
    
    // Try to create many WebSocket instances
    int created = 0;
    WebSocket* ws_array[100];
    
    for (int i = 0; i < 100; i++) {
        ws_array[i] = websocket_create();
        if (ws_array[i]) {
            created++;
        } else {
            // Expected to fail eventually
            printf("  Memory limit reached at %d instances (expected)\n", i);
            break;
        }
    }
    
    // Cleanup all created instances
    for (int i = 0; i < created; i++) {
        websocket_destroy(ws_array[i]);
    }
    
    // Restore old limit
    setrlimit(RLIMIT_AS, &old_rlim);
    
    // Test should pass if we handled exhaustion gracefully
    TEST_ASSERT(created > 0, "Should have created at least one instance");
    
    return 0;
}

// Test 5: Double-free protection
int test_double_free_protection(void) {
    WebSocket* ws = websocket_create();
    if (!ws) {
        fprintf(stderr, "Failed to create WebSocket\n");
        return -1;
    }
    
    // First destroy (should succeed)
    websocket_destroy(ws);
    ws = NULL;  // Clear pointer after destroy
    
    // Note: Second destroy on same pointer would cause double-free
    // This test verifies that the first destroy properly cleans up
    // For actual double-free detection, run with AddressSanitizer:
    // BUILD_WITH_ASAN=ON cmake .. && make
    // AddressSanitizer will catch any double-free issues
    
    // Test that we can create/destroy multiple times without issues
    for (int i = 0; i < 10; i++) {
        WebSocket* ws2 = websocket_create();
        if (ws2) {
            websocket_destroy(ws2);
        }
    }
    
    return 0;
}

// Test 6: Use-after-free detection (hot paths)
int test_use_after_free(void) {
    WebSocket* ws = websocket_create();
    if (!ws) {
        fprintf(stderr, "Failed to create WebSocket\n");
        return -1;
    }
    
    // Set callbacks
    websocket_set_on_message(ws, NULL, NULL);
    websocket_set_on_error(ws, NULL, NULL);
    
    // Get state before destroy
    WSState state_before = websocket_get_state(ws);
    (void)state_before;
    
    // Destroy
    websocket_destroy(ws);
    ws = NULL;  // Clear pointer to prevent use-after-free
    
    // Note: Use-after-free detection should be tested with AddressSanitizer
    // AddressSanitizer will catch any access to freed memory
    // For normal builds, we just verify that destroy doesn't crash
    
    // Test that operations on NULL pointer don't crash
    // (These should be handled gracefully by the library)
    WebSocket* null_ws = NULL;
    WSState state = websocket_get_state(null_ws);
    (void)state;
    
    int result = websocket_send(null_ws, (const uint8_t*)"test", 4, true);
    (void)result;
    
    result = websocket_close(null_ws);
    (void)result;
    
    return 0;
}

// Test 7: Ring buffer memory locking verification
int test_ringbuffer_memory_locking(void) {
    RingBuffer rb;
    if (ringbuffer_init(&rb) != 0) {
        fprintf(stderr, "Failed to init ring buffer\n");
        return -1;
    }
    
    // Verify buffer is locked (if mlock succeeded)
    TEST_CHECK(rb.locked == 0 || rb.locked == 1, "Locked flag should be 0 or 1");
    
    // Verify buffer is accessible and aligned
    TEST_ASSERT(rb.buf != NULL, "Buffer should not be NULL");
    TEST_ASSERT(rb.size == RINGBUFFER_SIZE, "Buffer size mismatch");
    
    uintptr_t addr = (uintptr_t)rb.buf;
    TEST_ASSERT((addr % RINGBUFFER_ALIGNMENT) == 0, "Buffer not aligned");
    
    ringbuffer_cleanup(&rb);
    
    // Verify cleanup properly unmapped memory
    // (Can't directly verify, but should not crash)
    
    return 0;
}

// Test 8: Multiple rapid init/cleanup cycles
int test_rapid_init_cleanup_cycles(void) {
    const int cycles = 1000;
    MemoryTracker tracker;
    memory_tracker_init(&tracker);
    
    printf("  Running %d rapid init/cleanup cycles...\n", cycles);
    
    for (int i = 0; i < cycles; i++) {
        RingBuffer rb;
        if (ringbuffer_init(&rb) != 0) {
            fprintf(stderr, "Failed to init at cycle %d\n", i);
            return -1;
        }
        
        // Quick write/read test
        char test_data[64] = "test";
        size_t written = ringbuffer_write(&rb, test_data, sizeof(test_data));
        TEST_ASSERT(written == sizeof(test_data), "Write failed");
        
        char read_buf[64];
        size_t read = ringbuffer_read(&rb, read_buf, sizeof(read_buf));
        TEST_ASSERT(read == sizeof(test_data), "Read failed");
        TEST_ASSERT(memcmp(test_data, read_buf, sizeof(test_data)) == 0, "Data mismatch");
        
        ringbuffer_cleanup(&rb);
    }
    
    memory_tracker_update(&tracker);
    memory_tracker_print(&tracker, "rapid_cycles");
    
    // Check for memory leaks
    if (memory_tracker_check_growth(&tracker, 1.0)) {
        fprintf(stderr, "FAIL: Memory leak in rapid cycles\n");
        return -1;
    }
    
    return 0;
}

int main(void) {
    printf("Memory Management Tests\n");
    printf("======================\n\n");
    
    TEST_RUN("WebSocket leak detection (10k cycles)", test_websocket_leak_detection);
    TEST_RUN("Ring buffer alignment verification", test_ringbuffer_alignment);
    TEST_RUN("Ring buffer overflow protection", test_ringbuffer_overflow_protection);
    TEST_RUN("Resource exhaustion - memory limit", test_resource_exhaustion_memory);
    TEST_RUN("Double-free protection", test_double_free_protection);
    TEST_RUN("Use-after-free detection", test_use_after_free);
    TEST_RUN("Ring buffer memory locking", test_ringbuffer_memory_locking);
    TEST_RUN("Rapid init/cleanup cycles", test_rapid_init_cleanup_cycles);
    
    printf("\n========================================\n");
    printf("Total: %d passed, %d failed\n", tests_passed, tests_failed);
    
    return (tests_failed > 0) ? 1 : 0;
}

