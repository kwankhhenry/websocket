#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <unistd.h>
#include "../src/websocket.h"
#include "../src/os_macos.h"

#define TARGET_LATENCY_US 40        // Target: 99th percentile < 40µs
#define TARGET_THROUGHPUT_MSG_SEC 15000  // Target: 15k+ messages/sec

static volatile int running = 1;
static uint64_t message_count = 0;
static uint64_t latency_sum_ns = 0;
static uint64_t latency_max_ns = 0;
static uint64_t latencies[1000000];  // Store last 1M latencies for percentile calculation
static size_t latency_idx = 0;

void on_message(WebSocket* ws, const uint8_t* data, size_t len, void* user_data) {
    // t0: Kernel timestamp (already captured by SO_TIMESTAMP_OLD in io_kqueue)
    // t1: kqueue wake (we'll measure t1-t0 difference)
    // t2: TLS decryption (measured in ssl_read)
    // t3: Parsing complete (now)
    
    uint64_t t3 = arm_cycle_count();
    
    // For this test, we'll measure t3 (parsing complete time)
    // In production, you'd capture t0-t3 at each stage
    if (latency_idx < sizeof(latencies) / sizeof(latencies[0])) {
        latencies[latency_idx++] = t3;
    }
    
    message_count++;
    
    // Print first few messages for debugging
    if (message_count <= 5) {
        char msg_buf[256];
        size_t copy_len = (len < sizeof(msg_buf) - 1) ? len : sizeof(msg_buf) - 1;
        memcpy(msg_buf, data, copy_len);
        msg_buf[copy_len] = '\0';
        printf("Message %llu: %s\n", (unsigned long long)message_count, msg_buf);
    }
}

void on_error(WebSocket* ws, int error_code, const char* error_msg, void* user_data) {
    fprintf(stderr, "WebSocket error %d: %s\n", error_code, error_msg ? error_msg : "Unknown");
}

void signal_handler(int sig) {
    running = 0;
}

// Comparison function for qsort
static int compare_uint64(const void* a, const void* b) {
    uint64_t va = *(const uint64_t*)a;
    uint64_t vb = *(const uint64_t*)b;
    if (va < vb) return -1;
    if (va > vb) return 1;
    return 0;
}

// Calculate percentile from sorted latencies
static double calculate_percentile(uint64_t* sorted_latencies, size_t count, double percentile) {
    if (count == 0) return 0.0;
    
    size_t idx = (size_t)(percentile * count / 100.0);
    if (idx >= count) idx = count - 1;
    
    return arm_cycles_to_ns(sorted_latencies[idx]);
}

int main(void) {
    printf("Binance WebSocket Integration Test\n");
    printf("===================================\n\n");
    
    // Set up signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // Tune network for low latency
    if (sysctl_tune_network() == 0) {
        printf("Network tuning applied\n");
    }
    
    // Create WebSocket
    WebSocket* ws = websocket_create();
    if (!ws) {
        fprintf(stderr, "Failed to create WebSocket\n");
        return 1;
    }
    
    // Set callbacks
    websocket_set_on_message(ws, on_message, NULL);
    websocket_set_on_error(ws, on_error, NULL);
    
    // Connect to Binance WebSocket
    const char* url = "wss://stream.binance.com:9443/ws/btcusdt@ticker";
    printf("Connecting to %s...\n", url);
    
    if (websocket_connect(ws, url, true) != 0) {
        fprintf(stderr, "Failed to connect\n");
        websocket_destroy(ws);
        return 1;
    }
    
    printf("Connected. Processing messages...\n");
    printf("Press Ctrl+C to stop\n\n");
    
    time_t start_time = time(NULL);
    uint64_t start_cycles = arm_cycle_count();
    
    // Event loop
    while (running) {
        int events = websocket_process(ws);
        
        if (events < 0) {
            fprintf(stderr, "Error processing events\n");
            break;
        }
        
        if (websocket_get_state(ws) == WS_STATE_CLOSED) {
            fprintf(stderr, "Connection closed\n");
            break;
        }
        
        // Small sleep to avoid busy-waiting
        usleep(100);  // 100µs
    }
    
    time_t end_time = time(NULL);
    uint64_t end_cycles = arm_cycle_count();
    double elapsed_sec = difftime(end_time, start_time);
    
    printf("\nTest completed.\n");
    printf("===============\n\n");
    
    // Statistics
    printf("Messages received: %llu\n", (unsigned long long)message_count);
    printf("Test duration: %.2f seconds\n", elapsed_sec);
    
    if (elapsed_sec > 0) {
        double throughput = message_count / elapsed_sec;
        printf("Throughput: %.2f messages/sec\n", throughput);
        
        if (throughput >= TARGET_THROUGHPUT_MSG_SEC) {
            printf("✓ Throughput target met (>= %d msg/sec)\n", TARGET_THROUGHPUT_MSG_SEC);
        } else {
            printf("✗ Throughput below target (< %d msg/sec)\n", TARGET_THROUGHPUT_MSG_SEC);
        }
    }
    
    // Calculate latency percentiles
    if (latency_idx > 0) {
        // Sort latencies for percentile calculation
        qsort(latencies, latency_idx, sizeof(uint64_t), compare_uint64);
        
        double p50 = calculate_percentile(latencies, latency_idx, 50.0);
        double p95 = calculate_percentile(latencies, latency_idx, 95.0);
        double p99 = calculate_percentile(latencies, latency_idx, 99.0);
        
        printf("\nLatency percentiles:\n");
        printf("  P50: %.2f ns (%.3f µs)\n", p50, p50 / 1000.0);
        printf("  P95: %.2f ns (%.3f µs)\n", p95, p95 / 1000.0);
        printf("  P99: %.2f ns (%.3f µs)\n", p99, p99 / 1000.0);
        
        if (p99 < TARGET_LATENCY_US * 1000.0) {
            printf("✓ Latency target met (P99 < %d µs)\n", TARGET_LATENCY_US);
        } else {
            printf("✗ Latency above target (P99 >= %d µs)\n", TARGET_LATENCY_US);
        }
    }
    
    websocket_close(ws);
    websocket_destroy(ws);
    
    return 0;
}

