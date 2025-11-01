#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include "../src/websocket.h"
#include "../src/parser_neon.h"
#include "../src/os_macos.h"

static volatile int running = 1;

void on_message(WebSocket* ws, const uint8_t* data, size_t len, void* user_data) {
    // Parse JSON ticker data
    char symbol_buf[32];
    char price_buf[32];
    char volume_buf[32];
    size_t symbol_len = 0, price_len = 0, volume_len = 0;
    
    // Extract fields using Neon-accelerated JSON parsing
    neon_parse_json_market_data((const char*)data, len, "s", symbol_buf, sizeof(symbol_buf), &symbol_len);
    neon_parse_json_market_data((const char*)data, len, "c", price_buf, sizeof(price_buf), &price_len);
    neon_parse_json_market_data((const char*)data, len, "v", volume_buf, sizeof(volume_buf), &volume_len);
    
    if (symbol_len > 0 && price_len > 0) {
        printf("[%s] Price: %s", symbol_buf, price_buf);
        if (volume_len > 0) {
            printf(" | Volume: %s", volume_buf);
        }
        printf("\n");
    }
}

void on_error(WebSocket* ws, int error_code, const char* error_msg, void* user_data) {
    fprintf(stderr, "Error %d: %s\n", error_code, error_msg ? error_msg : "Unknown error");
}

void signal_handler(int sig) {
    running = 0;
}

int main(int argc, char* argv[]) {
    const char* symbol = "btcusdt";
    if (argc > 1) {
        symbol = argv[1];
    }
    
    printf("Binance Ticker Subscription Example\n");
    printf("====================================\n\n");
    printf("Subscribing to %s ticker stream...\n\n", symbol);
    
    // Set up signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // Tune network for low latency (optional)
    sysctl_tune_network();
    
    // Create WebSocket
    WebSocket* ws = websocket_create();
    if (!ws) {
        fprintf(stderr, "Failed to create WebSocket\n");
        return 1;
    }
    
    // Set callbacks
    websocket_set_on_message(ws, on_message, NULL);
    websocket_set_on_error(ws, on_error, NULL);
    
    // Build WebSocket URL
    char url[256];
    snprintf(url, sizeof(url), "wss://stream.binance.com:9443/ws/%s@ticker", symbol);
    
    // Connect (disable cert validation for HFT optimization)
    if (websocket_connect(ws, url, true) != 0) {
        fprintf(stderr, "Failed to connect to %s\n", url);
        websocket_destroy(ws);
        return 1;
    }
    
    printf("Connected! Receiving ticker updates...\n");
    printf("Press Ctrl+C to stop\n\n");
    
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
        usleep(100);
    }
    
    printf("\nDisconnecting...\n");
    websocket_close(ws);
    websocket_destroy(ws);
    
    return 0;
}

