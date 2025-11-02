#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include "test_helpers.h"

static int tests_passed = 0;
static int tests_failed = 0;
static volatile int stop_test = 0;

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

// Signal handler
void sigint_handler(int sig) {
    (void)sig;
    stop_test = 1;
}

// Callback context for extended Binance test
static struct {
    size_t message_count;
    size_t reconnect_count;
    PerformanceMetrics* metrics;
} ext_binance_ctx = {0};

static void ext_binance_message_callback(WebSocket* w, const uint8_t* data, size_t len, void* user_data) {
    (void)w;
    (void)data;
    (void)len;
    (void)user_data;
    ext_binance_ctx.message_count++;
    
    // Record latency occasionally
    if (ext_binance_ctx.metrics && 
        ext_binance_ctx.message_count % 100 == 0 && 
        ext_binance_ctx.metrics->count < ext_binance_ctx.metrics->capacity) {
        uint64_t latency = 1000;  // Placeholder - would measure actual latency
        performance_metrics_record(ext_binance_ctx.metrics, latency);
    }
}

static void ext_binance_error_callback(WebSocket* w, int error_code, const char* error_msg, void* user_data) {
    (void)w;
    (void)error_code;
    (void)error_msg;
    (void)user_data;
    ext_binance_ctx.reconnect_count++;
    printf("  Error occurred, reconnect count: %zu\n", ext_binance_ctx.reconnect_count);
}

static size_t spike_message_count = 0;
static size_t messages_in_window = 0;

static void spike_message_callback(WebSocket* w, const uint8_t* data, size_t len, void* user_data) {
    (void)w;
    (void)data;
    (void)len;
    (void)user_data;
    spike_message_count++;
    messages_in_window++;
}

// Test 1: Extended Binance connection (1+ hours)
int test_extended_binance_connection(void) {
    signal(SIGINT, sigint_handler);
    
    const int duration_minutes = getenv("INTEGRATION_DURATION") ?
                                 atoi(getenv("INTEGRATION_DURATION")) : 60;  // 1 hour default
    
    printf("  Running extended Binance connection test for %d minutes...\n", duration_minutes);
    printf("  (Press Ctrl+C to stop early)\n");
    
    const char* url = "wss://stream.binance.com:443/stream?streams=btcusdt@trade&timeUnit=MICROSECOND";
    
    WebSocket* ws = websocket_create();
    if (!ws) {
        fprintf(stderr, "Failed to create WebSocket\n");
        return -1;
    }
    
    MemoryTracker tracker;
    memory_tracker_init(&tracker);
    
    PerformanceMetrics metrics;
    if (performance_metrics_init(&metrics, 100000) != 0) {
        websocket_destroy(ws);
        return -1;
    }
    
    ext_binance_ctx.message_count = 0;
    ext_binance_ctx.reconnect_count = 0;
    ext_binance_ctx.metrics = &metrics;
    
    time_t start_time = time(NULL);
    const int duration_seconds = duration_minutes * 60;
    
    websocket_set_on_message(ws, ext_binance_message_callback, NULL);
    websocket_set_on_error(ws, ext_binance_error_callback, NULL);
    
    printf("  Connecting to Binance...\n");
    if (websocket_connect(ws, url, true) != 0) {
        fprintf(stderr, "Failed to connect\n");
        websocket_destroy(ws);
        return -1;
    }
    
    // Wait for handshake
    int handshake_waits = 0;
    while (websocket_get_state(ws) == WS_STATE_CONNECTING && handshake_waits < 200) {
        websocket_process(ws);
        if (websocket_get_state(ws) == WS_STATE_CONNECTED) {
            break;
        }
        usleep(100000);  // 100ms
        handshake_waits++;
    }
    
    if (websocket_get_state(ws) != WS_STATE_CONNECTED) {
        fprintf(stderr, "Handshake failed\n");
        websocket_destroy(ws);
        return -1;
    }
    
    printf("  Connected! Running for %d minutes...\n", duration_minutes);
    
    // Main loop
    time_t last_status_time = start_time;
    
    while (!stop_test) {
        time_t current_time = time(NULL);
        int elapsed = (int)(current_time - start_time);
        
        if (elapsed >= duration_seconds) {
            break;
        }
        
        // Process events
        websocket_process(ws);
        
        // Auto-reconnect if connection closed
        if (websocket_get_state(ws) == WS_STATE_CLOSED || 
            websocket_get_state(ws) == WS_STATE_CLOSING) {
            printf("  Connection closed, reconnecting...\n");
            
            websocket_close(ws);
            websocket_destroy(ws);
            usleep(1000000);  // 1 second
            
            ws = websocket_create();
            if (!ws) {
                fprintf(stderr, "Failed to recreate WebSocket\n");
                break;
            }
            
            websocket_set_on_message(ws, ext_binance_message_callback, NULL);
            websocket_set_on_error(ws, ext_binance_error_callback, NULL);
            
            if (websocket_connect(ws, url, true) == 0) {
                handshake_waits = 0;
                while (websocket_get_state(ws) == WS_STATE_CONNECTING && handshake_waits < 200) {
                    websocket_process(ws);
                    if (websocket_get_state(ws) == WS_STATE_CONNECTED) {
                        break;
                    }
                    usleep(100000);
                    handshake_waits++;
                }
                
                if (websocket_get_state(ws) == WS_STATE_CONNECTED) {
                    printf("  Reconnected successfully\n");
                }
            }
        }
        
        // Status update every minute
        if (current_time - last_status_time >= 60) {
            int minutes = elapsed / 60;
            int seconds = elapsed % 60;
            double msg_per_sec = ext_binance_ctx.message_count / (elapsed > 0 ? elapsed : 1);
            
            printf("  [%02d:%02d] Messages: %zu (%.1f msg/sec), Reconnects: %zu\n",
                   minutes, seconds, ext_binance_ctx.message_count, msg_per_sec, ext_binance_ctx.reconnect_count);
            
            memory_tracker_update(&tracker);
            if (memory_tracker_check_growth(&tracker, 10.0)) {
                printf("  WARNING: Memory growth >10%% detected\n");
                memory_tracker_print(&tracker, "status");
            }
            
            last_status_time = current_time;
        }
        
        usleep(10000);  // 10ms
    }
    
    time_t end_time = time(NULL);
    int total_elapsed = (int)(end_time - start_time);
    
    printf("\n  Test completed:\n");
    printf("    Duration: %d seconds\n", total_elapsed);
    printf("    Messages: %zu\n", ext_binance_ctx.message_count);
    printf("    Reconnects: %zu\n", ext_binance_ctx.reconnect_count);
    double avg_rate = total_elapsed > 0 ? (double)ext_binance_ctx.message_count / total_elapsed : 0.0;
    printf("    Avg rate: %.1f msg/sec\n", avg_rate);
    
    memory_tracker_print(&tracker, "final");
    
    // Check for memory leaks
    if (memory_tracker_check_growth(&tracker, 20.0)) {
        fprintf(stderr, "FAIL: Memory leak detected in extended test\n");
        performance_metrics_cleanup(&metrics);
        websocket_close(ws);
        websocket_destroy(ws);
        return -1;
    }
    
    performance_metrics_cleanup(&metrics);
    websocket_close(ws);
    websocket_destroy(ws);
    
    return 0;
}

// Test 2: Reconnection stress test
int test_reconnection_stress(void) {
    printf("  Testing reconnection stress...\n");
    
    const char* url = "wss://stream.binance.com:443/stream?streams=btcusdt@trade&timeUnit=MICROSECOND";
    const int num_reconnects = 20;
    
    MemoryTracker tracker;
    memory_tracker_init(&tracker);
    
    int successful_reconnects = 0;
    
    for (int i = 0; i < num_reconnects; i++) {
        WebSocket* ws = websocket_create();
        if (!ws) {
            fprintf(stderr, "Failed to create WebSocket at reconnect %d\n", i);
            continue;
        }
        
        printf("  Reconnect attempt %d/%d...\n", i + 1, num_reconnects);
        
        if (websocket_connect(ws, url, true) != 0) {
            websocket_destroy(ws);
            continue;
        }
        
        // Wait for connection
        int handshake_waits = 0;
        while (websocket_get_state(ws) == WS_STATE_CONNECTING && handshake_waits < 100) {
            websocket_process(ws);
            if (websocket_get_state(ws) == WS_STATE_CONNECTED) {
                successful_reconnects++;
                break;
            }
            usleep(100000);  // 100ms
            handshake_waits++;
        }
        
        // Close immediately
        websocket_close(ws);
        websocket_destroy(ws);
        
        // Small delay between reconnects
        usleep(200000);  // 200ms
        
        // Memory check every 5 reconnects
        if ((i + 1) % 5 == 0) {
            memory_tracker_update(&tracker);
            if (memory_tracker_check_growth(&tracker, 15.0)) {
                printf("  WARNING: Memory growth >15%% at reconnect %d\n", i + 1);
            }
        }
    }
    
    printf("  Successful reconnects: %d/%d\n", successful_reconnects, num_reconnects);
    memory_tracker_print(&tracker, "reconnection_stress");
    
    // Check for memory leaks
    if (memory_tracker_check_growth(&tracker, 25.0)) {
        fprintf(stderr, "FAIL: Memory leak in reconnection stress test\n");
        return -1;
    }
    
    // Should have at least 50% success rate
    if (successful_reconnects < num_reconnects / 2) {
        fprintf(stderr, "FAIL: Reconnection success rate too low (%d/%d)\n",
                successful_reconnects, num_reconnects);
        return -1;
    }
    
    return 0;
}

// Test 3: Message rate spike handling
int test_message_rate_spike(void) {
    printf("  Testing message rate spike handling...\n");
    
    const char* url = "wss://stream.binance.com:443/stream?streams=btcusdt@trade&timeUnit=MICROSECOND";
    
    WebSocket* ws = websocket_create();
    if (!ws) {
        fprintf(stderr, "Failed to create WebSocket\n");
        return -1;
    }
    
    static size_t peak_message_rate = 0;
    spike_message_count = 0;
    peak_message_rate = 0;
    messages_in_window = 0;
    
    time_t last_rate_check = time(NULL);
    
    websocket_set_on_message(ws, spike_message_callback, NULL);
    
    if (websocket_connect(ws, url, true) != 0) {
        websocket_destroy(ws);
        return -1;
    }
    
    // Wait for handshake
    int handshake_waits = 0;
    while (websocket_get_state(ws) == WS_STATE_CONNECTING && handshake_waits < 200) {
        websocket_process(ws);
        if (websocket_get_state(ws) == WS_STATE_CONNECTED) {
            break;
        }
        usleep(100000);
        handshake_waits++;
    }
    
    if (websocket_get_state(ws) != WS_STATE_CONNECTED) {
        websocket_close(ws);
        websocket_destroy(ws);
        return -1;
    }
    
    printf("  Connected, monitoring message rate for 60 seconds...\n");
    
    time_t start_time = time(NULL);
    const int test_duration = 60;  // 60 seconds
    
    while (!stop_test) {
        time_t current_time = time(NULL);
        int elapsed = (int)(current_time - start_time);
        
        if (elapsed >= test_duration) {
            break;
        }
        
        websocket_process(ws);
        
        // Check message rate every second
        if (current_time != last_rate_check) {
            if (messages_in_window > peak_message_rate) {
                peak_message_rate = messages_in_window;
            }
            messages_in_window = 0;
            last_rate_check = current_time;
        }
        
        if (websocket_get_state(ws) != WS_STATE_CONNECTED) {
            break;
        }
        
        usleep(1000);  // 1ms
    }
    
    printf("  Total messages: %zu\n", spike_message_count);
    printf("  Peak message rate: %zu msg/sec\n", peak_message_rate);
    double avg_rate2 = test_duration > 0 ? (double)spike_message_count / test_duration : 0.0;
    printf("  Average rate: %.1f msg/sec\n", avg_rate2);
    
    websocket_close(ws);
    websocket_destroy(ws);
    
    // Should handle at least 100 msg/sec
    if (peak_message_rate < 100 && spike_message_count > 0) {
        printf("  WARNING: Peak message rate %zu msg/sec seems low\n", peak_message_rate);
    }
    
    return 0;
}

// Test 4: Multiple symbol streams
int test_multiple_symbol_streams(void) {
    printf("  Testing multiple symbol streams...\n");
    
    const int num_streams = 3;
    const char* symbols[] = {"btcusdt", "ethusdt", "bnbusdt"};
    WebSocket* ws_array[num_streams];
    
    for (int i = 0; i < num_streams; i++) {
        ws_array[i] = websocket_create();
        if (!ws_array[i]) {
            fprintf(stderr, "Failed to create WebSocket for stream %d\n", i);
            // Cleanup already created
            for (int j = 0; j < i; j++) {
                websocket_destroy(ws_array[j]);
            }
            return -1;
        }
        
        char url[256];
        snprintf(url, sizeof(url),
                 "wss://stream.binance.com:443/stream?streams=%s@trade&timeUnit=MICROSECOND",
                 symbols[i]);
        
        printf("  Connecting to %s stream...\n", symbols[i]);
        
        if (websocket_connect(ws_array[i], url, true) != 0) {
            fprintf(stderr, "Failed to connect to %s stream\n", symbols[i]);
            websocket_destroy(ws_array[i]);
            // Cleanup
            for (int j = 0; j < i; j++) {
                websocket_destroy(ws_array[j]);
            }
            return -1;
        }
    }
    
    // Wait for all handshakes
    int all_connected = 0;
    for (int wait = 0; wait < 200 && all_connected < num_streams; wait++) {
        all_connected = 0;
        for (int i = 0; i < num_streams; i++) {
            websocket_process(ws_array[i]);
            if (websocket_get_state(ws_array[i]) == WS_STATE_CONNECTED) {
                all_connected++;
            }
        }
        usleep(100000);  // 100ms
    }
    
    printf("  Connected streams: %d/%d\n", all_connected, num_streams);
    
    // Process for a short time
    time_t start_time = time(NULL);
    const int process_duration = 30;  // 30 seconds
    
    while (!stop_test && (time(NULL) - start_time) < process_duration) {
        for (int i = 0; i < num_streams; i++) {
            websocket_process(ws_array[i]);
        }
        usleep(10000);  // 10ms
    }
    
    // Cleanup
    for (int i = 0; i < num_streams; i++) {
        websocket_close(ws_array[i]);
        websocket_destroy(ws_array[i]);
    }
    
    if (all_connected == num_streams) {
        printf("  All streams connected successfully\n");
    } else {
        printf("  WARNING: Only %d/%d streams connected\n", all_connected, num_streams);
    }
    
    return (all_connected >= num_streams / 2) ? 0 : -1;  // At least half should connect
}

int main(void) {
    printf("Integration Stress Tests\n");
    printf("=======================\n\n");
    
    // Check environment variables
    if (getenv("INTEGRATION_DURATION")) {
        printf("INTEGRATION_DURATION=%s\n", getenv("INTEGRATION_DURATION"));
    }
    printf("\n");
    
    TEST_RUN("Extended Binance connection", test_extended_binance_connection);
    TEST_RUN("Reconnection stress", test_reconnection_stress);
    TEST_RUN("Message rate spike handling", test_message_rate_spike);
    TEST_RUN("Multiple symbol streams", test_multiple_symbol_streams);
    
    printf("\n========================================\n");
    printf("Total: %d passed, %d failed\n", tests_passed, tests_failed);
    
    return (tests_failed > 0) ? 1 : 0;
}

