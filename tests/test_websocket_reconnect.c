#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "../src/websocket.h"
#include "../src/os_macos.h"

static int message_count = 0;
static int reconnect_count = 0;
static time_t start_time = 0;

void on_message(WebSocket* ws, const uint8_t* data, size_t len, void* user_data) {
    message_count++;
    
    // Print first 3 messages and every 50th message
    if (message_count <= 3 || message_count % 50 == 0) {
        char msg_preview[128];
        size_t preview_len = len < 127 ? len : 127;
        memcpy(msg_preview, data, preview_len);
        msg_preview[preview_len] = '\0';
        
        time_t now = time(NULL);
        int elapsed = (int)(now - start_time);
        printf("[%03ds] Message #%d received (%zu bytes): %.80s...\n", 
               elapsed, message_count, len, msg_preview);
    }
}

void on_error(WebSocket* ws, int error_code, const char* error_msg, void* user_data) {
    fprintf(stderr, "WebSocket error %d: %s\n", error_code, error_msg ? error_msg : "Unknown");
}

// Test auto-reconnect functionality
int test_reconnect(WebSocket** ws_ptr, const char* url) {
    WebSocket* ws = *ws_ptr;
    
    // Close existing connection
    if (ws) {
        printf("Closing existing connection...\n");
        websocket_close(ws);
        websocket_destroy(ws);
        usleep(500000);  // 500ms delay
    }
    
    reconnect_count++;
    printf("\n=== RECONNECT ATTEMPT #%d ===\n", reconnect_count);
    
    // Create new connection
    ws = websocket_create();
    if (!ws) {
        fprintf(stderr, "ERROR: Failed to create WebSocket for reconnect\n");
        return -1;
    }
    
    // Set callbacks
    websocket_set_on_message(ws, on_message, NULL);
    websocket_set_on_error(ws, on_error, NULL);
    
    // Connect
    printf("Connecting to %s...\n", url);
    int connect_result = websocket_connect(ws, url, true);
    if (connect_result != 0) {
        // PRIORITY 1 FIX: Better error reporting with context
        WSReconnectState state;
        websocket_get_reconnect_state(ws, &state);
        
        fprintf(stderr, "ERROR: Reconnection failed (result=%d, reconnect_attempts=%d)\n", 
                connect_result, state.reconnect_attempts);
        
        // Only fail if this is not an expected backoff scenario
        // Initial connect failures are expected, retries should handle it
        if (state.reconnect_attempts >= 5) {
            fprintf(stderr, "  Failed after %d reconnect attempts\n", state.reconnect_attempts);
            websocket_destroy(ws);
            return -1;
        }
        
        // For initial failures or backoff scenarios, continue to handshake wait
        // The handshake wait will handle the actual connection verification
        printf("  Retrying connection (attempt %d)...\n", state.reconnect_attempts + 1);
    }
    
    // Wait for handshake
    printf("Waiting for handshake...\n");
    int handshake_waits = 0;
    while (websocket_get_state(ws) == WS_STATE_CONNECTING && handshake_waits < 100) {
        websocket_process(ws);
        usleep(100000);  // 100ms
        handshake_waits++;
        if (handshake_waits % 10 == 0) {
            printf("  Waiting... (%d/100)\n", handshake_waits);
        }
    }
    
    if (websocket_get_state(ws) != WS_STATE_CONNECTED) {
        fprintf(stderr, "ERROR: Reconnection handshake failed (state=%d)\n", 
                (int)websocket_get_state(ws));
        websocket_destroy(ws);
        return -1;
    }
    
    printf("✅ Reconnected successfully! (state=%d)\n", (int)websocket_get_state(ws));
    
    // Process a few times to stabilize
    for (int i = 0; i < 10; i++) {
        websocket_process(ws);
        usleep(50000);  // 50ms
    }
    
    *ws_ptr = ws;
    return 0;
}

int main(void) {
    printf("WebSocket Auto-Reconnect Test\n");
    printf("=============================\n\n");
    
    start_time = time(NULL);
    
    const char* url = "wss://stream.binance.com:443/stream?streams=btcusdt@trade&timeUnit=MICROSECOND";
    
    // Initial connection
    printf("=== INITIAL CONNECTION ===\n");
    WebSocket* ws = websocket_create();
    if (!ws) {
        fprintf(stderr, "Failed to create WebSocket\n");
        return 1;
    }
    
    websocket_set_on_message(ws, on_message, NULL);
    websocket_set_on_error(ws, on_error, NULL);
    
    printf("Connecting to %s...\n", url);
    if (websocket_connect(ws, url, true) != 0) {
        fprintf(stderr, "Failed to connect\n");
        websocket_destroy(ws);
        return 1;
    }
    
    printf("Waiting for handshake...\n");
    int handshake_waits = 0;
    while (websocket_get_state(ws) == WS_STATE_CONNECTING && handshake_waits < 200) {  // 20 seconds total
        int events = websocket_process(ws);
        if (events < 0) {
            fprintf(stderr, "Error processing events during handshake\n");
            websocket_destroy(ws);
            return 1;
        }
        usleep(100000);  // 100ms
        handshake_waits++;
        WSState current_state = websocket_get_state(ws);
        if (handshake_waits % 10 == 0) {
            printf("  Waiting... (%d/200, state=%d, events=%d)\n", handshake_waits, (int)current_state, events);
        }
        // Check if we're connected
        if (current_state == WS_STATE_CONNECTED) {
            printf("  ✓ Handshake completed after %d waits\n", handshake_waits);
            break;
        }
        // Check if connection failed
        if (current_state == WS_STATE_CLOSED) {
            fprintf(stderr, "Connection closed during handshake (waits=%d)\n", handshake_waits);
            websocket_destroy(ws);
            return 1;
        }
    }
    
    if (websocket_get_state(ws) != WS_STATE_CONNECTED) {
        fprintf(stderr, "Handshake failed or timeout (state=%d, waits=%d)\n", 
                (int)websocket_get_state(ws), handshake_waits);
        websocket_destroy(ws);
        return 1;
    }
    
    printf("✅ Connected! (state=%d)\n\n", (int)websocket_get_state(ws));
    
    // Collect messages for 30 seconds
    printf("Collecting messages for 30 seconds...\n");
    printf("(Will then test auto-reconnect)\n\n");
    
    time_t connect_time = time(NULL);
    int last_message_count = 0;
    int no_message_count = 0;
    
    while (1) {
        time_t now = time(NULL);
        int elapsed = (int)(now - start_time);
        int connect_elapsed = (int)(now - connect_time);
        
        // Process events
        int events = websocket_process(ws);
        
        // Check connection state
        WSState state = websocket_get_state(ws);
        
        // Collect messages for first 30 seconds
        if (connect_elapsed < 30) {
            if (message_count > last_message_count) {
                last_message_count = message_count;
                no_message_count = 0;
            } else {
                no_message_count++;
            }
        } else if (connect_elapsed == 30) {
            printf("\n=== 30 seconds elapsed, testing reconnect ===\n");
            printf("Collected %d messages before reconnect\n", message_count);
            
            // Test reconnect
            if (test_reconnect(&ws, url) != 0) {
                fprintf(stderr, "Reconnect test failed\n");
                break;
            }
            
            connect_time = time(NULL);  // Reset for next phase
            last_message_count = message_count;
            printf("Continuing to collect messages after reconnect...\n\n");
        }
        
        // Check if connection closed unexpectedly
        if (state == WS_STATE_CLOSED || state == WS_STATE_CLOSING) {
            printf("\n⚠️  Connection closed (state=%d), attempting reconnect...\n", state);
            
            if (test_reconnect(&ws, url) != 0) {
                fprintf(stderr, "Reconnect failed\n");
                break;
            }
            
            connect_time = time(NULL);
            last_message_count = message_count;
        }
        
        // Test duration: 90 seconds total
        if (elapsed >= 90) {
            printf("\n=== Test duration complete (90 seconds) ===\n");
            break;
        }
        
        // Check if we're getting messages
        if (events == 0) {
            usleep(1000);  // 1ms if no events
        }
        
        // Print status every 10 seconds
        if (elapsed > 0 && elapsed % 10 == 0 && elapsed != (elapsed / 10) * 10 - 1) {
            printf("[%03ds] Status: %d messages, %d reconnects, state=%d\n", 
                   elapsed, message_count, reconnect_count, (int)state);
        }
    }
    
    printf("\n=== TEST COMPLETE ===\n");
    printf("Total messages received: %d\n", message_count);
    printf("Total reconnects: %d\n", reconnect_count);
    printf("Test duration: %d seconds\n", (int)(time(NULL) - start_time));
    
    if (message_count > 0) {
        printf("✅ Test PASSED - Messages received successfully\n");
    } else {
        printf("❌ Test FAILED - No messages received\n");
    }
    
    if (reconnect_count > 0) {
        printf("✅ Reconnect test PASSED - Reconnected %d time(s)\n", reconnect_count);
    } else {
        printf("⚠️  Reconnect test not triggered (connection stayed alive)\n");
    }
    
    websocket_close(ws);
    websocket_destroy(ws);
    
    return (message_count > 0) ? 0 : 1;
}

