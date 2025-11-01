#include "parser_neon.h"
#include <string.h>
#include <arpa/inet.h>

// Neon-accelerated WebSocket frame parsing
int neon_parse_ws_frame(const uint8_t* data, size_t len, WSFrame* frame) {
    if (!data || !frame || len < 2) {
        return -1;  // Need at least 2 bytes for basic header
    }
    
    memset(frame, 0, sizeof(WSFrame));
    
    // Extract basic header fields
    // Byte 0: FIN(7) RSV1-3(6-4) OPCODE(3-0)
    // Byte 1: MASK(7) PAYLOAD_LEN(6-0)
    uint8_t byte0 = data[0];
    uint8_t byte1 = data[1];
    
    frame->fin = (byte0 >> 7) & 1;
    frame->opcode = byte0 & 0x0F;
    frame->mask = (byte1 >> 7) & 1;
    
    uint64_t payload_len = byte1 & 0x7F;
    
    size_t header_bytes = 2;
    
    // Determine payload length type
    if (payload_len == 126) {
        // 16-bit length
        if (len < 4) {
            return 1;  // Need more data
        }
        header_bytes = 4;
        uint16_t len16;
        memcpy(&len16, data + 2, 2);
        frame->payload_len = ntohs(len16);
        frame->payload_len_type = 16;
    } else if (payload_len == 127) {
        // 64-bit length
        if (len < 10) {
            return 1;  // Need more data
        }
        header_bytes = 10;
        uint64_t len64;
        memcpy(&len64, data + 2, 8);
        // Network byte order (big-endian) to host
        frame->payload_len = __builtin_bswap64(len64);
        frame->payload_len_type = 64;
    } else {
        // 7-bit length
        frame->payload_len = payload_len;
        frame->payload_len_type = 7;
    }
    
    // Extract masking key if present
    if (frame->mask) {
        if (len < header_bytes + 4) {
            return 1;  // Need more data
        }
        memcpy(&frame->masking_key, data + header_bytes, 4);
        header_bytes += 4;
    }
    
    frame->header_size = header_bytes;
    
    // Check if we have full frame
    size_t total_frame_size = header_bytes + frame->payload_len;
    if (len < total_frame_size) {
        return 1;  // Need more data
    }
    
    frame->payload = data + header_bytes;
    
    return 0;  // Success
}

// Neon-accelerated JSON field extraction
// Uses vtbl (table lookup) for parallel delimiter scanning
int neon_parse_json_market_data(const char* json, size_t json_len,
                                const char* field_name,
                                char* value_buf, size_t value_buf_size,
                                size_t* value_len) {
    if (!json || !field_name || !value_buf || !value_len) {
        return -1;
    }
    
    // Build search pattern for field name with quotes: "field_name":
    char search_pattern[256];
    size_t pattern_len = snprintf(search_pattern, sizeof(search_pattern), "\"%s\":", field_name);
    
    if (pattern_len >= sizeof(search_pattern)) {
        return -1;
    }
    
    // Use Neon to scan for pattern in parallel
    // We'll search 16 bytes at a time
    size_t i = 0;
    bool found = false;
    size_t field_start = 0;
    
    while (i + 16 <= json_len && !found) {
        // Load 16 bytes
        uint8x16_t chunk = vld1q_u8((const uint8_t*)(json + i));
        
        // Create pattern vector (replicate first byte of pattern)
        uint8x16_t pattern_vec = vdupq_n_u8(search_pattern[0]);
        
        // Compare
        uint8x16_t cmp = vceqq_u8(chunk, pattern_vec);
        
        // Check for matches (Neon comparison requires constant indices)
        uint8_t cmp_array[16];
        vst1q_u8(cmp_array, cmp);
        for (size_t j = 0; j < 16 && (i + j) < json_len; j++) {
            if (cmp_array[j]) {
                // Potential match - verify full pattern
                if (i + j + pattern_len <= json_len) {
                    if (memcmp(json + i + j, search_pattern, pattern_len) == 0) {
                        field_start = i + j + pattern_len;
                        found = true;
                        break;
                    }
                }
            }
        }
        
        i += 16;
    }
    
    // Fallback: scalar search for remaining bytes
    if (!found) {
        for (; i < json_len - pattern_len; i++) {
            if (memcmp(json + i, search_pattern, pattern_len) == 0) {
                field_start = i + pattern_len;
                found = true;
                break;
            }
        }
    }
    
    if (!found) {
        return -1;
    }
    
    // Skip whitespace after colon
    while (field_start < json_len && (json[field_start] == ' ' || json[field_start] == '\t')) {
        field_start++;
    }
    
    // Extract value (handle quotes, numbers, etc.)
    size_t value_start = field_start;
    size_t value_end = value_start;
    
    // Determine value type and extract accordingly
    if (value_start < json_len) {
        if (json[value_start] == '"') {
            // String value
            value_start++;  // Skip opening quote
            value_end = value_start;
            while (value_end < json_len && json[value_end] != '"' && json[value_end] != '\0') {
                // Handle escape sequences
                if (json[value_end] == '\\' && value_end + 1 < json_len) {
                    value_end += 2;
                } else {
                    value_end++;
                }
            }
        } else {
            // Number or boolean (extract until comma, }, or whitespace)
            value_end = value_start;
            while (value_end < json_len &&
                   json[value_end] != ',' &&
                   json[value_end] != '}' &&
                   json[value_end] != ']' &&
                   json[value_end] != ' ' &&
                   json[value_end] != '\t' &&
                   json[value_end] != '\n' &&
                   json[value_end] != '\r') {
                value_end++;
            }
        }
    }
    
    size_t extracted_len = value_end - value_start;
    if (extracted_len >= value_buf_size) {
        extracted_len = value_buf_size - 1;
    }
    
    memcpy(value_buf, json + value_start, extracted_len);
    value_buf[extracted_len] = '\0';
    *value_len = extracted_len;
    
    return 0;
}

// Batch field extraction using Neon
int neon_extract_json_fields(const char* json, size_t json_len,
                             JSONField* fields, size_t field_count) {
    if (!json || !fields || field_count == 0) {
        return -1;
    }
    
    int found_count = 0;
    
    // Extract each field
    for (size_t i = 0; i < field_count; i++) {
        if (!fields[i].field_name) {
            continue;
        }
        
        char value_buf[256];
        size_t value_len = 0;
        
        if (neon_parse_json_market_data(json, json_len,
                                       fields[i].field_name,
                                       value_buf, sizeof(value_buf),
                                       &value_len) == 0) {
            fields[i].found = true;
            fields[i].field_value = value_buf;
            fields[i].value_len = value_len;
            found_count++;
        } else {
            fields[i].found = false;
        }
    }
    
    return found_count;
}

// Scalar fallback implementations (for comparison/validation)
int scalar_parse_ws_frame(const uint8_t* data, size_t len, WSFrame* frame) {
    if (!data || !frame || len < 2) {
        return -1;
    }
    
    memset(frame, 0, sizeof(WSFrame));
    
    uint8_t byte0 = data[0];
    uint8_t byte1 = data[1];
    
    frame->fin = (byte0 >> 7) & 1;
    frame->opcode = byte0 & 0x0F;
    frame->mask = (byte1 >> 7) & 1;
    
    uint64_t payload_len = byte1 & 0x7F;
    size_t header_bytes = 2;
    
    if (payload_len == 126) {
        if (len < 4) {
            return 1;
        }
        header_bytes = 4;
        uint16_t len16;
        memcpy(&len16, data + 2, 2);
        frame->payload_len = ntohs(len16);
        frame->payload_len_type = 16;
    } else if (payload_len == 127) {
        if (len < 10) {
            return 1;
        }
        header_bytes = 10;
        uint64_t len64;
        memcpy(&len64, data + 2, 8);
        frame->payload_len = __builtin_bswap64(len64);
        frame->payload_len_type = 64;
    } else {
        frame->payload_len = payload_len;
        frame->payload_len_type = 7;
    }
    
    if (frame->mask) {
        if (len < header_bytes + 4) {
            return 1;
        }
        memcpy(&frame->masking_key, data + header_bytes, 4);
        header_bytes += 4;
    }
    
    frame->header_size = header_bytes;
    
    size_t total_frame_size = header_bytes + frame->payload_len;
    if (len < total_frame_size) {
        return 1;
    }
    
    frame->payload = data + header_bytes;
    
    return 0;
}

int scalar_parse_json_market_data(const char* json, size_t json_len,
                                  const char* field_name,
                                  char* value_buf, size_t value_buf_size,
                                  size_t* value_len) {
    // Simple scalar implementation without Neon
    char search_pattern[256];
    size_t pattern_len = snprintf(search_pattern, sizeof(search_pattern), "\"%s\":", field_name);
    
    if (pattern_len >= sizeof(search_pattern)) {
        return -1;
    }
    
    const char* pos = strstr(json, search_pattern);
    if (!pos) {
        return -1;
    }
    
    size_t field_start = (pos - json) + pattern_len;
    
    // Skip whitespace
    while (field_start < json_len && (json[field_start] == ' ' || json[field_start] == '\t')) {
        field_start++;
    }
    
    size_t value_start = field_start;
    size_t value_end = value_start;
    
    if (value_start < json_len) {
        if (json[value_start] == '"') {
            value_start++;
            value_end = value_start;
            while (value_end < json_len && json[value_end] != '"' && json[value_end] != '\0') {
                if (json[value_end] == '\\' && value_end + 1 < json_len) {
                    value_end += 2;
                } else {
                    value_end++;
                }
            }
        } else {
            value_end = value_start;
            while (value_end < json_len &&
                   json[value_end] != ',' &&
                   json[value_end] != '}' &&
                   json[value_end] != ']' &&
                   json[value_end] != ' ' &&
                   json[value_end] != '\t' &&
                   json[value_end] != '\n' &&
                   json[value_end] != '\r') {
                value_end++;
            }
        }
    }
    
    size_t extracted_len = value_end - value_start;
    if (extracted_len >= value_buf_size) {
        extracted_len = value_buf_size - 1;
    }
    
    memcpy(value_buf, json + value_start, extracted_len);
    value_buf[extracted_len] = '\0';
    *value_len = extracted_len;
    
    return 0;
}

