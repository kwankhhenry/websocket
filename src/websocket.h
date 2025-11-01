#ifndef WEBSOCKET_H
#define WEBSOCKET_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// WebSocket state machine
typedef enum {
    WS_STATE_CONNECTING,
    WS_STATE_CONNECTED,
    WS_STATE_CLOSING,
    WS_STATE_CLOSED
} WSState;

// WebSocket frame opcodes (RFC 6455)
#define WS_OPCODE_CONT  0x0
#define WS_OPCODE_TEXT  0x1
#define WS_OPCODE_BINARY 0x2
#define WS_OPCODE_CLOSE 0x8
#define WS_OPCODE_PING  0x9
#define WS_OPCODE_PONG  0xA

// Forward declarations
typedef struct WebSocket WebSocket;

// Message callback type
// Called when a complete WebSocket message is received
typedef void (*ws_on_message_t)(WebSocket* ws, const uint8_t* data, size_t len, void* user_data);

// Error callback type
typedef void (*ws_on_error_t)(WebSocket* ws, int error_code, const char* error_msg, void* user_data);

// Create WebSocket instance
// Returns NULL on error
WebSocket* websocket_create(void);

// Cleanup WebSocket instance
void websocket_destroy(WebSocket* ws);

// Set message callback
void websocket_set_on_message(WebSocket* ws, ws_on_message_t callback, void* user_data);

// Set error callback
void websocket_set_on_error(WebSocket* ws, ws_on_error_t callback, void* user_data);

// Connect to WebSocket server
// url: e.g., "wss://stream.binance.com:9443/ws/btcusdt@ticker"
// disable_cert_validation: true for HFT optimization (skip cert validation)
// Returns 0 on success, -1 on error
int websocket_connect(WebSocket* ws, const char* url, bool disable_cert_validation);

// Send message (TEXT or BINARY)
// Returns 0 on success, -1 on error
int websocket_send(WebSocket* ws, const uint8_t* data, size_t len, bool is_text);

// Close WebSocket connection
// Returns 0 on success, -1 on error
int websocket_close(WebSocket* ws);

// Process I/O events (call this in your event loop)
// Returns number of events processed, -1 on error
int websocket_process(WebSocket* ws);

// Get current state
WSState websocket_get_state(WebSocket* ws);

// Get user data pointer
void* websocket_get_user_data(WebSocket* ws);

// Get last captured NIC timestamp (kernel-level, from SO_TIMESTAMP_OLD)
// Returns timestamp in nanoseconds (0 if not available)
// Note: For SSL/TLS, timestamps may not be available if SecureTransport reads internally
uint64_t websocket_get_last_nic_timestamp_ns(WebSocket* ws);

// Get last NIC timestamp in CPU cycles
uint64_t websocket_get_last_nic_timestamp_ticks(WebSocket* ws);

#endif // WEBSOCKET_H

