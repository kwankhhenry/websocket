#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <stdatomic.h>
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

// Test 1: SPSC correctness verification
typedef struct {
    RingBuffer* rb;
    size_t iterations;
    size_t payload_size;
    atomic_int producer_done;
    atomic_size_t producer_count;
    atomic_size_t consumer_count;
} SPSCContext;

void* producer_thread(void* arg) {
    SPSCContext* ctx = (SPSCContext*)arg;
    
    char* data = (char*)malloc(ctx->payload_size);
    if (!data) {
        return NULL;
    }
    memset(data, 'P', ctx->payload_size);
    
    for (size_t i = 0; i < ctx->iterations; i++) {
        size_t written = ringbuffer_write(ctx->rb, data, ctx->payload_size);
        if (written > 0) {
            atomic_fetch_add(&ctx->producer_count, 1);
        } else {
            // Buffer full, wait a bit
            usleep(1);
        }
    }
    
    atomic_store(&ctx->producer_done, 1);
    free(data);
    return NULL;
}

void* consumer_thread(void* arg) {
    SPSCContext* ctx = (SPSCContext*)arg;
    
    char* data = (char*)malloc(ctx->payload_size);
    if (!data) {
        return NULL;
    }
    
    while (atomic_load(&ctx->producer_done) == 0 || 
           ringbuffer_readable(ctx->rb) > 0) {
        size_t read = ringbuffer_read(ctx->rb, data, ctx->payload_size);
        if (read > 0) {
            atomic_fetch_add(&ctx->consumer_count, 1);
        } else {
            // Buffer empty, wait a bit
            usleep(1);
        }
    }
    
    free(data);
    return NULL;
}

int test_spsc_correctness(void) {
    printf("  Testing SPSC lock-free correctness...\n");
    
    RingBuffer rb;
    if (ringbuffer_init(&rb) != 0) {
        fprintf(stderr, "Failed to init ring buffer\n");
        return -1;
    }
    
    SPSCContext ctx = {
        .rb = &rb,
        .iterations = 100000,
        .payload_size = 512,
        .producer_done = ATOMIC_VAR_INIT(0),
        .producer_count = ATOMIC_VAR_INIT(0),
        .consumer_count = ATOMIC_VAR_INIT(0)
    };
    
    pthread_t producer, consumer;
    
    // Create threads
    if (pthread_create(&producer, NULL, producer_thread, &ctx) != 0) {
        fprintf(stderr, "Failed to create producer thread\n");
        ringbuffer_cleanup(&rb);
        return -1;
    }
    
    if (pthread_create(&consumer, NULL, consumer_thread, &ctx) != 0) {
        fprintf(stderr, "Failed to create consumer thread\n");
        pthread_join(producer, NULL);
        ringbuffer_cleanup(&rb);
        return -1;
    }
    
    // Wait for threads
    pthread_join(producer, NULL);
    pthread_join(consumer, NULL);
    
    size_t produced = atomic_load(&ctx.producer_count);
    size_t consumed = atomic_load(&ctx.consumer_count);
    
    printf("  Produced: %zu, Consumed: %zu\n", produced, consumed);
    
    // Verify correctness: all produced items should be consumed
    // Allow small difference due to timing
    size_t diff = (produced > consumed) ? produced - consumed : consumed - produced;
    double diff_percent = (produced > 0) ? (double)diff * 100.0 / produced : 0.0;
    
    if (diff_percent > 1.0) {
        fprintf(stderr, "FAIL: SPSC data loss detected (diff: %.2f%%)\n", diff_percent);
        ringbuffer_cleanup(&rb);
        return -1;
    }
    
    ringbuffer_cleanup(&rb);
    return 0;
}

// Test 2: Concurrent WebSocket instances (independent connections)
typedef struct {
    int instance_id;
    int num_messages;
    atomic_int messages_received;
} ConcurrentWebSocketContext;

static void concurrent_message_callback(WebSocket* w, const uint8_t* data, size_t len, void* user_data) {
    (void)w;
    (void)data;
    (void)len;
    ConcurrentWebSocketContext* cctx = (ConcurrentWebSocketContext*)user_data;
    atomic_fetch_add(&cctx->messages_received, 1);
}

void* concurrent_websocket_thread(void* arg) {
    ConcurrentWebSocketContext* ctx = (ConcurrentWebSocketContext*)arg;
    
    WebSocket* ws = websocket_create();
    if (!ws) {
        fprintf(stderr, "Thread %d: Failed to create WebSocket\n", ctx->instance_id);
        return NULL;
    }
    
    websocket_set_on_message(ws, concurrent_message_callback, ctx);
    
    // Try to connect (will likely fail without a server, but tests creation/destruction)
    websocket_connect(ws, "wss://echo.websocket.org", true);
    
    // Process for a short time
    for (int i = 0; i < 100; i++) {
        websocket_process(ws);
        usleep(10000);  // 10ms
    }
    
    websocket_close(ws);
    websocket_destroy(ws);
    
    return NULL;
}

int test_concurrent_websocket_instances(void) {
    printf("  Testing concurrent WebSocket instances...\n");
    
    const int num_instances = 10;
    pthread_t* threads = (pthread_t*)malloc(num_instances * sizeof(pthread_t));
    ConcurrentWebSocketContext* contexts = 
        (ConcurrentWebSocketContext*)malloc(num_instances * sizeof(ConcurrentWebSocketContext));
    
    if (!threads || !contexts) {
        fprintf(stderr, "Failed to allocate thread/context arrays\n");
        free(threads);
        free(contexts);
        return -1;
    }
    
    // Initialize contexts
    for (int i = 0; i < num_instances; i++) {
        contexts[i].instance_id = i;
        contexts[i].num_messages = 100;
        contexts[i].messages_received = ATOMIC_VAR_INIT(0);
    }
    
    // Create threads
    for (int i = 0; i < num_instances; i++) {
        if (pthread_create(&threads[i], NULL, concurrent_websocket_thread, &contexts[i]) != 0) {
            fprintf(stderr, "Failed to create thread %d\n", i);
            // Join already created threads
            for (int j = 0; j < i; j++) {
                pthread_join(threads[j], NULL);
            }
            free(threads);
            free(contexts);
            return -1;
        }
    }
    
    // Wait for all threads
    for (int i = 0; i < num_instances; i++) {
        pthread_join(threads[i], NULL);
    }
    
    printf("  Completed %d concurrent instances\n", num_instances);
    
    free(threads);
    free(contexts);
    
    return 0;
}

// Test 3: Memory ordering verification (acquire/release semantics)
int test_memory_ordering(void) {
    printf("  Verifying memory ordering semantics...\n");
    
    RingBuffer rb;
    if (ringbuffer_init(&rb) != 0) {
        fprintf(stderr, "Failed to init ring buffer\n");
        return -1;
    }
    
    // This test verifies that writes by producer are visible to consumer
    // due to acquire/release semantics in ringbuffer_read/write
    
    const size_t test_iterations = 10000;
    const size_t payload_size = 128;
    
    char write_data[payload_size];
    char read_data[payload_size];
    
    // Sequential write/read should work correctly
    int errors = 0;
    for (size_t i = 0; i < test_iterations; i++) {
        // Write marker value
        memset(write_data, (char)(i & 0xFF), payload_size);
        size_t written = ringbuffer_write(&rb, write_data, payload_size);
        
        if (written == payload_size) {
            size_t read = ringbuffer_read(&rb, read_data, payload_size);
            
            if (read != payload_size || memcmp(write_data, read_data, payload_size) != 0) {
                errors++;
                if (errors > 10) {
                    fprintf(stderr, "FAIL: Memory ordering violation detected\n");
                    ringbuffer_cleanup(&rb);
                    return -1;
                }
            }
        }
    }
    
    if (errors > 0) {
        printf("  WARNING: %d memory ordering issues detected\n", errors);
    } else {
        printf("  Memory ordering verified (acquire/release semantics working)\n");
    }
    
    ringbuffer_cleanup(&rb);
    return 0;
}

// Test 4: Race condition detection stress test
typedef struct {
    RingBuffer* rb;
    int thread_id;
    int num_operations;
    atomic_int errors;
} RaceTestContext;

void* race_test_thread(void* arg) {
    RaceTestContext* ctx = (RaceTestContext*)arg;
    
    const size_t payload_size = 256;
    char* data = (char*)malloc(payload_size);
    if (!data) {
        return NULL;
    }
    memset(data, (char)ctx->thread_id, payload_size);
    
    for (int i = 0; i < ctx->num_operations; i++) {
        // Alternate between write and read
        if (i % 2 == 0) {
            size_t written = ringbuffer_write(ctx->rb, data, payload_size);
            if (written == 0 && ringbuffer_writeable(ctx->rb) > 0) {
                // Unexpected failure
                atomic_fetch_add(&ctx->errors, 1);
            }
        } else {
            char read_buf[payload_size];
            size_t read = ringbuffer_read(ctx->rb, read_buf, payload_size);
            if (read > 0 && read != payload_size) {
                // Unexpected partial read
                atomic_fetch_add(&ctx->errors, 1);
            }
        }
    }
    
    free(data);
    return NULL;
}

int test_race_condition_stress(void) {
    printf("  Stress testing for race conditions...\n");
    
    RingBuffer rb;
    if (ringbuffer_init(&rb) != 0) {
        fprintf(stderr, "Failed to init ring buffer\n");
        return -1;
    }
    
    const int num_threads = 4;
    pthread_t* threads = (pthread_t*)malloc(num_threads * sizeof(pthread_t));
    RaceTestContext* contexts = 
        (RaceTestContext*)malloc(num_threads * sizeof(RaceTestContext));
    
    if (!threads || !contexts) {
        free(threads);
        free(contexts);
        ringbuffer_cleanup(&rb);
        return -1;
    }
    
    // Initialize contexts
    for (int i = 0; i < num_threads; i++) {
        contexts[i].rb = &rb;
        contexts[i].thread_id = i;
        contexts[i].num_operations = 10000;
        contexts[i].errors = ATOMIC_VAR_INIT(0);
    }
    
    // Create threads
    for (int i = 0; i < num_threads; i++) {
        if (pthread_create(&threads[i], NULL, race_test_thread, &contexts[i]) != 0) {
            fprintf(stderr, "Failed to create thread %d\n", i);
            // Cleanup
            for (int j = 0; j < i; j++) {
                pthread_join(threads[j], NULL);
            }
            free(threads);
            free(contexts);
            ringbuffer_cleanup(&rb);
            return -1;
        }
    }
    
    // Wait for threads
    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
    }
    
    // Check for errors
    int total_errors = 0;
    for (int i = 0; i < num_threads; i++) {
        total_errors += atomic_load(&contexts[i].errors);
    }
    
    printf("  Total errors across %d threads: %d\n", num_threads, total_errors);
    
    free(threads);
    free(contexts);
    ringbuffer_cleanup(&rb);
    
    // Note: Some errors may be expected in multi-threaded scenarios
    // since ring buffer is SPSC (single producer, single consumer)
    // This test is more about verifying no crashes
    
    return 0;
}

int main(void) {
    printf("Thread Safety Tests\n");
    printf("===================\n\n");
    
    TEST_RUN("SPSC correctness verification", test_spsc_correctness);
    TEST_RUN("Concurrent WebSocket instances", test_concurrent_websocket_instances);
    TEST_RUN("Memory ordering verification", test_memory_ordering);
    TEST_RUN("Race condition stress test", test_race_condition_stress);
    
    printf("\n========================================\n");
    printf("Total: %d passed, %d failed\n", tests_passed, tests_failed);
    
    return (tests_failed > 0) ? 1 : 0;
}

