#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <sys/time.h>
#include "test_helpers.h"

static int tests_passed = 0;
static int tests_failed = 0;
static volatile int stop_stress_test = 0;

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

// Signal handler for graceful shutdown
void sigint_handler(int sig) {
    (void)sig;
    stop_stress_test = 1;
}

// Test 1: High-throughput message processing
int test_high_throughput_processing(void) {
    const size_t num_messages = 100000;  // 100K messages
    const size_t payload_sizes[] = {64, 256, 1024, 4096, 16384, 65536};  // Various sizes
    const int num_sizes = sizeof(payload_sizes) / sizeof(payload_sizes[0]);
    
    RingBuffer rb;
    if (ringbuffer_init(&rb) != 0) {
        fprintf(stderr, "Failed to init ring buffer\n");
        return -1;
    }
    
    MemoryTracker tracker;
    memory_tracker_init(&tracker);
    
    PerformanceMetrics metrics;
    if (performance_metrics_init(&metrics, num_messages) != 0) {
        ringbuffer_cleanup(&rb);
        return -1;
    }
    
    printf("  Processing %zu messages with varying payload sizes...\n", num_messages);
    
    size_t total_bytes = 0;
    uint64_t start_cycles = arm_cycle_count();
    
    for (size_t i = 0; i < num_messages; i++) {
        size_t payload_size = payload_sizes[i % num_sizes];
        
        // Generate frame
        size_t frame_size = payload_size + 14;  // Max header size
        uint8_t* frame = (uint8_t*)malloc(frame_size);
        if (!frame) {
            fprintf(stderr, "Out of memory at message %zu\n", i);
            break;
        }
        generate_test_frame(frame, payload_size, true);
        
        // Measure write latency
        uint64_t write_start = arm_cycle_count();
        size_t written = ringbuffer_write(&rb, frame, frame_size);
        uint64_t write_end = arm_cycle_count();
        
        if (written > 0) {
            total_bytes += written;
            performance_metrics_record(&metrics, write_end - write_start);
            
            // Read to prevent buffer saturation
            if (ringbuffer_readable(&rb) > 0) {
                char read_buf[65536];
                ringbuffer_read(&rb, read_buf, sizeof(read_buf));
            }
        }
        
        free(frame);
        
        // Memory check every 10k messages
        if ((i + 1) % 10000 == 0) {
            memory_tracker_update(&tracker);
            if (memory_tracker_check_growth(&tracker, 5.0)) {
                printf("  WARNING: Memory growth >5%% at message %zu\n", i + 1);
            }
        }
    }
    
    uint64_t end_cycles = arm_cycle_count();
    uint64_t total_cycles = end_cycles - start_cycles;
    double total_time_ns = arm_cycles_to_ns(total_cycles);
    double throughput_msg_per_sec = (num_messages * 1e9) / total_time_ns;
    double throughput_mbps = (total_bytes * 8.0) / (total_time_ns / 1e9) / 1e6;
    
    uint64_t p50, p90, p95, p99, p999;
    performance_metrics_get_stats(&metrics, &p50, &p90, &p95, &p99, &p999);
    
    printf("  Throughput: %.2f msg/sec, %.2f Mbps\n", throughput_msg_per_sec, throughput_mbps);
    printf("  Latency P50: %.2f ns, P90: %.2f ns, P95: %.2f ns, P99: %.2f ns, P99.9: %.2f ns\n",
           arm_cycles_to_ns(p50), arm_cycles_to_ns(p90), arm_cycles_to_ns(p95),
           arm_cycles_to_ns(p99), arm_cycles_to_ns(p999));
    
    memory_tracker_print(&tracker, "high_throughput");
    
    // Verify throughput target (15K msg/sec minimum)
    TEST_ASSERT(throughput_msg_per_sec >= 15000, "Throughput below target");
    
    performance_metrics_cleanup(&metrics);
    ringbuffer_cleanup(&rb);
    
    return 0;
}

// Test 2: Rapid connect/disconnect cycles
int test_rapid_connect_disconnect(void) {
    const int cycles = 1000;
    MemoryTracker tracker;
    memory_tracker_init(&tracker);
    
    printf("  Running %d rapid connect/disconnect cycles...\n", cycles);
    
    int successful_connects = 0;
    int failed_connects = 0;
    
    for (int i = 0; i < cycles; i++) {
        WebSocket* ws = websocket_create();
        if (!ws) {
            failed_connects++;
            continue;
        }
        
        // Try to connect to a test server (this will likely fail, but tests cleanup)
        // In a real test, use a mock server or skip actual connection
        int connect_result = websocket_connect(ws, "wss://echo.websocket.org", true);
        if (connect_result == 0) {
            successful_connects++;
            
            // Wait briefly for connection state
            for (int j = 0; j < 10; j++) {
                websocket_process(ws);
                usleep(10000);  // 10ms
                if (websocket_get_state(ws) == WS_STATE_CONNECTED) {
                    break;
                }
            }
        }
        
        // Close and destroy
        websocket_close(ws);
        websocket_destroy(ws);
        
        // Small delay to avoid resource exhaustion
        if ((i + 1) % 100 == 0) {
            usleep(10000);  // 10ms every 100 cycles
            memory_tracker_update(&tracker);
            
            // PHASE 1: After SSL cleanup fixes, memory growth should be much lower
            if (memory_tracker_check_growth(&tracker, 10.0)) {
                printf("  WARNING: Memory growth >10%% at cycle %d\n", i + 1);
                memory_tracker_print(&tracker, "rapid_cycles");
            }
        }
    }
    
    printf("  Successful connects: %d, Failed: %d\n", successful_connects, failed_connects);
    memory_tracker_print(&tracker, "final");
    
    // PHASE 1 FIX: Reduced threshold from 50% to 20% after SSL session cache fixes
    // With disabled session caching and proper SSL cleanup, memory growth should be minimal
    // Some growth is still expected due to system allocator pooling, but should be <20%
    if (memory_tracker_check_growth(&tracker, 20.0)) {
        fprintf(stderr, "FAIL: Excessive memory growth in rapid cycles (>20%%)\n");
        fprintf(stderr, "  Expected <20%% after SSL cleanup fixes\n");
        return -1;
    }
    
    return 0;
}

// Test 3: Ring buffer saturation
int test_ringbuffer_saturation(void) {
    RingBuffer rb;
    if (ringbuffer_init(&rb) != 0) {
        fprintf(stderr, "Failed to init ring buffer\n");
        return -1;
    }
    
    printf("  Testing ring buffer saturation and backpressure...\n");
    
    // Fill buffer to capacity
    size_t writeable = ringbuffer_writeable(&rb);
    char* fill_data = (char*)malloc(writeable);
    if (!fill_data) {
        ringbuffer_cleanup(&rb);
        return -1;
    }
    memset(fill_data, 'X', writeable);
    
    // Measure write time during saturation
    uint64_t start = arm_cycle_count();
    size_t written = ringbuffer_write(&rb, fill_data, writeable);
    uint64_t end = arm_cycle_count();
    
    printf("  Wrote %zu bytes in %.2f ns\n", written, arm_cycles_to_ns(end - start));
    
    // Try to write more (should fail or return 0)
    char extra_data[1024];
    memset(extra_data, 'Y', sizeof(extra_data));
    size_t extra_written = ringbuffer_write(&rb, extra_data, sizeof(extra_data));
    TEST_ASSERT(extra_written == 0, "Should not write when buffer is full");
    
    // Measure read performance during saturation
    size_t readable = ringbuffer_readable(&rb);
    char* read_buf = (char*)malloc(readable);
    if (!read_buf) {
        free(fill_data);
        ringbuffer_cleanup(&rb);
        return -1;
    }
    
    start = arm_cycle_count();
    size_t read = ringbuffer_read(&rb, read_buf, readable);
    end = arm_cycle_count();
    
    printf("  Read %zu bytes in %.2f ns\n", read, arm_cycles_to_ns(end - start));
    
    // Verify data integrity
    TEST_ASSERT(read == readable, "Read amount mismatch");
    TEST_ASSERT(memcmp(fill_data, read_buf, read) == 0, "Data corruption detected");
    
    free(fill_data);
    free(read_buf);
    ringbuffer_cleanup(&rb);
    
    return 0;
}

// Test 4: Long-running connection test (configurable duration)
int test_long_running_connection(void) {
    signal(SIGINT, sigint_handler);
    
    const int duration_seconds = getenv("STRESS_DURATION") ? 
                                  atoi(getenv("STRESS_DURATION")) : 300;  // 5 minutes default
    
    printf("  Running long-running connection test for %d seconds...\n", duration_seconds);
    printf("  (Press Ctrl+C to stop early)\n");
    
    RingBuffer rb;
    if (ringbuffer_init(&rb) != 0) {
        fprintf(stderr, "Failed to init ring buffer\n");
        return -1;
    }
    
    MemoryTracker tracker;
    memory_tracker_init(&tracker);
    
    time_t start_time = time(NULL);
    size_t message_count = 0;
    size_t total_bytes = 0;
    
    const size_t payload_size = 512;
    char test_data[payload_size];
    memset(test_data, 'T', payload_size);
    
    while (!stop_stress_test) {
        time_t current_time = time(NULL);
        if (current_time - start_time >= duration_seconds) {
            break;
        }
        
        // Continuous write/read cycle
        size_t written = ringbuffer_write(&rb, test_data, payload_size);
        if (written > 0) {
            total_bytes += written;
            message_count++;
            
            // Read when buffer has data
            if (ringbuffer_readable(&rb) >= payload_size) {
                char read_buf[payload_size];
                size_t read = ringbuffer_read(&rb, read_buf, payload_size);
                TEST_ASSERT(read == payload_size, "Read size mismatch");
            }
        }
        
        // Periodic memory checks
        if (message_count % 10000 == 0) {
            memory_tracker_update(&tracker);
            
            if (memory_tracker_check_growth(&tracker, 5.0)) {
                printf("  WARNING: Memory growth >5%% at message %zu\n", message_count);
            }
            
            // Print progress every 10k messages
            if (message_count % 100000 == 0) {
                int elapsed = (int)(current_time - start_time);
                double msg_per_sec = message_count / (elapsed > 0 ? elapsed : 1);
                printf("  Progress: %zu messages (%.0f msg/sec, %d seconds elapsed)\n",
                       message_count, msg_per_sec, elapsed);
            }
        }
        
        // Small sleep to avoid 100% CPU
        usleep(10);  // 10µs
    }
    
    time_t end_time = time(NULL);
    int elapsed = (int)(end_time - start_time);
    double msg_per_sec = message_count / (elapsed > 0 ? elapsed : 1);
    
    printf("  Completed: %zu messages in %d seconds (%.0f msg/sec, %zu MB)\n",
           message_count, elapsed, msg_per_sec, total_bytes / (1024 * 1024));
    
    memory_tracker_print(&tracker, "long_running");
    
    // Verify no memory leaks
    if (memory_tracker_check_growth(&tracker, 10.0)) {
        fprintf(stderr, "FAIL: Memory leak in long-running test\n");
        ringbuffer_cleanup(&rb);
        return -1;
    }
    
    ringbuffer_cleanup(&rb);
    return 0;
}

// Test 5: Concurrent connection stress (if supported)
int test_concurrent_connections(void) {
    const int num_connections = getenv("STRESS_CONNECTIONS") ?
                                atoi(getenv("STRESS_CONNECTIONS")) : 10;
    
    printf("  Testing %d concurrent WebSocket instances...\n", num_connections);
    
    WebSocket** ws_array = (WebSocket**)calloc(num_connections, sizeof(WebSocket*));
    if (!ws_array) {
        fprintf(stderr, "Failed to allocate WebSocket array\n");
        return -1;
    }
    
    MemoryTracker tracker;
    memory_tracker_init(&tracker);
    
    // Create all connections
    int created = 0;
    for (int i = 0; i < num_connections; i++) {
        ws_array[i] = websocket_create();
        if (ws_array[i]) {
            created++;
        }
    }
    
    printf("  Created %d/%d WebSocket instances\n", created, num_connections);
    
    // Process all connections in a loop
    const int iterations = 1000;
    for (int iter = 0; iter < iterations; iter++) {
        for (int i = 0; i < created; i++) {
            if (ws_array[i]) {
                websocket_process(ws_array[i]);
            }
        }
        
        // Memory check every 100 iterations
        if (iter % 100 == 0) {
            memory_tracker_update(&tracker);
        }
    }
    
    // Cleanup all connections
    for (int i = 0; i < created; i++) {
        if (ws_array[i]) {
            websocket_close(ws_array[i]);
            websocket_destroy(ws_array[i]);
        }
    }
    
    free(ws_array);
    
    memory_tracker_print(&tracker, "concurrent");
    
    // Check for memory leaks
    if (memory_tracker_check_growth(&tracker, 15.0)) {
        fprintf(stderr, "FAIL: Memory leak in concurrent connections\n");
        return -1;
    }
    
    return 0;
}

// Test 6: Message burst handling
int test_message_burst_handling(void) {
    RingBuffer rb;
    if (ringbuffer_init(&rb) != 0) {
        fprintf(stderr, "Failed to init ring buffer\n");
        return -1;
    }
    
    printf("  Testing sudden message burst (10x normal rate)...\n");
    
    const size_t normal_payload = 512;
    const size_t burst_messages = 10000;  // Burst of 10K messages
    
    // Pre-generate burst data
    char** burst_data = (char**)malloc(burst_messages * sizeof(char*));
    if (!burst_data) {
        ringbuffer_cleanup(&rb);
        return -1;
    }
    
    for (size_t i = 0; i < burst_messages; i++) {
        burst_data[i] = (char*)malloc(normal_payload);
        if (!burst_data[i]) {
            // Cleanup allocated so far
            for (size_t j = 0; j < i; j++) {
                free(burst_data[j]);
            }
            free(burst_data);
            ringbuffer_cleanup(&rb);
            return -1;
        }
        memset(burst_data[i], (char)('A' + (i % 26)), normal_payload);
    }
    
    // Simulate normal rate, then burst
    printf("  Normal rate phase...\n");
    for (size_t i = 0; i < 1000; i++) {
        char normal_data[normal_payload];
        memset(normal_data, 'N', normal_payload);
        ringbuffer_write(&rb, normal_data, normal_payload);
        
        // Read occasionally
        if (i % 100 == 0 && ringbuffer_readable(&rb) > 0) {
            char read_buf[normal_payload];
            ringbuffer_read(&rb, read_buf, normal_payload);
        }
    }
    
    // Burst phase
    printf("  Burst phase (%zu messages)...\n", burst_messages);
    uint64_t burst_start = arm_cycle_count();
    size_t burst_written = 0;
    
    for (size_t i = 0; i < burst_messages; i++) {
        size_t written = ringbuffer_write(&rb, burst_data[i], normal_payload);
        if (written > 0) {
            burst_written += written;
        }
        
        // Drain occasionally to prevent full saturation
        if (i % 1000 == 0 && ringbuffer_readable(&rb) > normal_payload * 100) {
            char drain_buf[normal_payload * 10];
            ringbuffer_read(&rb, drain_buf, sizeof(drain_buf));
        }
    }
    
    uint64_t burst_end = arm_cycle_count();
    double burst_time_ns = arm_cycles_to_ns(burst_end - burst_start);
    double burst_rate = (burst_messages * 1e9) / burst_time_ns;
    
    printf("  Burst rate: %.0f msg/sec\n", burst_rate);
    printf("  Written: %zu/%zu messages\n", burst_written / normal_payload, burst_messages);
    
    // Cleanup
    for (size_t i = 0; i < burst_messages; i++) {
        free(burst_data[i]);
    }
    free(burst_data);
    ringbuffer_cleanup(&rb);
    
    // Burst should handle at least 50K msg/sec
    TEST_ASSERT(burst_rate >= 50000, "Burst handling below expectation");
    
    return 0;
}

int main(void) {
    printf("Stress Tests\n");
    printf("============\n\n");
    
    // Check environment variables for test configuration
    if (getenv("STRESS_DURATION")) {
        printf("STRESS_DURATION=%s\n", getenv("STRESS_DURATION"));
    }
    if (getenv("STRESS_CONNECTIONS")) {
        printf("STRESS_CONNECTIONS=%s\n", getenv("STRESS_CONNECTIONS"));
    }
    printf("\n");
    
    TEST_RUN("High-throughput message processing", test_high_throughput_processing);
    TEST_RUN("Rapid connect/disconnect cycles", test_rapid_connect_disconnect);
    TEST_RUN("Ring buffer saturation", test_ringbuffer_saturation);
    TEST_RUN("Long-running connection", test_long_running_connection);
    TEST_RUN("Concurrent connections", test_concurrent_connections);
    TEST_RUN("Message burst handling", test_message_burst_handling);
    
    printf("\n========================================\n");
    printf("Total: %d passed, %d failed\n", tests_passed, tests_failed);
    
    return (tests_failed > 0) ? 1 : 0;
}

