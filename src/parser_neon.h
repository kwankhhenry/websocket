#ifndef PARSER_NEON_H
#define PARSER_NEON_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <arm_neon.h>

// WebSocket frame structure (RFC 6455)
typedef struct {
    uint8_t fin;           // FIN bit
    uint8_t opcode;        // Opcode (0x1=TEXT, 0x2=BINARY, 0x8=CLOSE, etc.)
    uint8_t mask;          // Mask bit
    uint8_t payload_len_type;  // Payload length type (7-bit, 16-bit, 64-bit)
    uint64_t payload_len;  // Actual payload length
    uint32_t masking_key;  // Masking key (if mask=1)
    const uint8_t* payload; // Payload pointer
    size_t header_size;    // Total header size (2-14 bytes)
} WSFrame;

// JSON field extraction (for market data)
typedef struct {
    const char* field_name;
    const char* field_value;
    size_t value_len;
    bool found;
} JSONField;

// Parse WebSocket frame using Neon SIMD
// Processes 16 bytes at a time for header validation
// Returns 0 on success, -1 on error, >0 if more data needed
int neon_parse_ws_frame(const uint8_t* data, size_t len, WSFrame* frame);

// Parse JSON market data field using Neon vtbl
// Optimized for known exchange schemas (precomputed field offsets)
// Returns 0 on success, -1 on error
int neon_parse_json_market_data(const char* json, size_t json_len, 
                                const char* field_name, 
                                char* value_buf, size_t value_buf_size,
                                size_t* value_len);

// Extract multiple fields from JSON in one pass (batch processing)
// Uses Neon for parallel delimiter scanning
int neon_extract_json_fields(const char* json, size_t json_len,
                             JSONField* fields, size_t field_count);

// Scalar (non-Neon) fallback for comparison/validation
int scalar_parse_ws_frame(const uint8_t* data, size_t len, WSFrame* frame);
int scalar_parse_json_market_data(const char* json, size_t json_len,
                                  const char* field_name,
                                  char* value_buf, size_t value_buf_size,
                                  size_t* value_len);

#endif // PARSER_NEON_H

