#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "../src/parser_neon.h"

#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "FAIL: %s\n", msg); \
            exit(1); \
        } \
    } while(0)

// Test WebSocket frame parsing (basic)
void test_ws_frame_basic(void) {
    // Simple TEXT frame: FIN=1, OPCODE=1 (TEXT), MASK=1, PAYLOAD_LEN=5
    uint8_t frame[] = {
        0x81, 0x85,  // Header: FIN|TEXT, MASK|LEN=5
        0x37, 0xfa, 0x21, 0x3d,  // Masking key
        0x7f, 0x9f, 0x4d, 0x51, 0x58  // Masked payload "Hello" (0x48^0x37, 0x65^0xfa, ...)
    };
    
    WSFrame parsed;
    memset(&parsed, 0, sizeof(parsed));
    int result = neon_parse_ws_frame(frame, sizeof(frame), &parsed);
    
    TEST_ASSERT(result == 0, "Frame parse failed");
    TEST_ASSERT(parsed.fin == 1, "FIN bit incorrect");
    TEST_ASSERT(parsed.opcode == 1, "Opcode incorrect");
    TEST_ASSERT(parsed.mask == 1, "Mask bit incorrect");
    TEST_ASSERT(parsed.payload_len == 5, "Payload length incorrect");
    
    printf("PASS: Basic WebSocket frame parsing test\n");
}

// Test WebSocket frame parsing (16-bit length)
void test_ws_frame_16bit(void) {
    // Frame with 16-bit length (126 = use 16-bit)
    uint8_t frame[264];  // 2 header + 2 length + 4 mask + 256 payload
    frame[0] = 0x82;  // FIN|BINARY
    frame[1] = 0xFE;  // MASK|126
    frame[2] = 0x01;  // Length MSB (256 bytes)
    frame[3] = 0x00;  // Length LSB
    frame[4] = 0x00;  // Mask key
    frame[5] = 0x00;
    frame[6] = 0x00;
    frame[7] = 0x00;
    memset(frame + 8, 0xAA, 256);
    
    WSFrame parsed;
    memset(&parsed, 0, sizeof(parsed));
    int result = neon_parse_ws_frame(frame, sizeof(frame), &parsed);
    
    TEST_ASSERT(result == 0, "Frame parse failed");
    TEST_ASSERT(parsed.payload_len == 256, "16-bit payload length incorrect");
    
    printf("PASS: 16-bit length WebSocket frame parsing test\n");
}

// Test JSON parsing (Neon)
void test_json_parsing(void) {
    const char* json = "{\"symbol\":\"BTCUSDT\",\"price\":\"50000.50\",\"quantity\":\"0.001\"}";
    
    char price_buf[64];
    size_t price_len = 0;
    
    int result = neon_parse_json_market_data(json, strlen(json),
                                             "price",
                                             price_buf, sizeof(price_buf),
                                             &price_len);
    
    TEST_ASSERT(result == 0, "JSON parse failed");
    TEST_ASSERT(price_len > 0, "Price value not found");
    TEST_ASSERT(strcmp(price_buf, "50000.50") == 0, "Price value incorrect");
    
    printf("PASS: JSON parsing test\n");
}

// Test JSON batch extraction
void test_json_batch_extraction(void) {
    const char* json = "{\"symbol\":\"BTCUSDT\",\"price\":\"50000.50\",\"quantity\":\"0.001\"}";
    
    JSONField fields[3];
    fields[0].field_name = "symbol";
    fields[1].field_name = "price";
    fields[2].field_name = "quantity";
    
    int found = neon_extract_json_fields(json, strlen(json), fields, 3);
    
    TEST_ASSERT(found == 3, "Batch extraction failed");
    TEST_ASSERT(fields[0].found == true, "Symbol field not found");
    TEST_ASSERT(fields[1].found == true, "Price field not found");
    TEST_ASSERT(fields[2].found == true, "Quantity field not found");
    
    printf("PASS: JSON batch extraction test\n");
}

// Test Neon vs scalar comparison
void test_neon_vs_scalar(void) {
    // Create test frame
    uint8_t frame[100];
    frame[0] = 0x81;  // FIN|TEXT
    frame[1] = 60;    // No mask, 60 bytes
    memset(frame + 2, 'A', 60);
    
    WSFrame neon_result, scalar_result;
    memset(&neon_result, 0, sizeof(neon_result));
    memset(&scalar_result, 0, sizeof(scalar_result));
    
    int neon_ret = neon_parse_ws_frame(frame, sizeof(frame), &neon_result);
    int scalar_ret = scalar_parse_ws_frame(frame, sizeof(frame), &scalar_result);
    
    TEST_ASSERT(neon_ret == scalar_ret, "Neon and scalar results differ");
    TEST_ASSERT(neon_result.opcode == scalar_result.opcode, "Opcode mismatch");
    TEST_ASSERT(neon_result.payload_len == scalar_result.payload_len, "Length mismatch");
    
    printf("PASS: Neon vs scalar comparison test\n");
}

// Test endianness (ARM is little-endian)
void test_endianness(void) {
    // Frame with 16-bit length
    uint8_t frame[520];  // 2 header + 2 length + 4 mask + 512 payload
    frame[0] = 0x82;
    frame[1] = 0xFE;  // 16-bit length
    frame[2] = 0x02;  // MSB (512 = 0x0200 in network byte order)
    frame[3] = 0x00;  // LSB
    frame[4] = 0x00;  // Mask key
    frame[5] = 0x00;
    frame[6] = 0x00;
    frame[7] = 0x00;
    memset(frame + 8, 'X', 512);
    
    WSFrame parsed;
    memset(&parsed, 0, sizeof(parsed));
    int result = neon_parse_ws_frame(frame, sizeof(frame), &parsed);
    
    TEST_ASSERT(result == 0, "Frame parse failed");
    TEST_ASSERT(parsed.payload_len == 512, "Endianness conversion failed");
    
    printf("PASS: Endianness test\n");
}

int main(void) {
    printf("Running Neon parser unit tests...\n\n");
    
    test_ws_frame_basic();
    test_ws_frame_16bit();
    test_json_parsing();
    test_json_batch_extraction();
    test_neon_vs_scalar();
    test_endianness();
    
    printf("\nAll Neon parser tests passed!\n");
    return 0;
}

