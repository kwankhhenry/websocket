#ifndef TEST_HELPERS_H
#define TEST_HELPERS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <sys/resource.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <mach/mach.h>
#include <mach/vm_statistics.h>
#include <mach/mach_host.h>
#include "../src/ringbuffer.h"
#include "../src/parser_neon.h"
#include "../src/websocket.h"
#include "../src/os_macos.h"

// Test assertion macro
#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "FAIL [%s:%d]: %s\n", __FILE__, __LINE__, msg); \
            return -1; \
        } \
    } while(0)

// Test assertion that doesn't return
#define TEST_CHECK(cond, msg) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "CHECK FAIL [%s:%d]: %s\n", __FILE__, __LINE__, msg); \
        } \
    } while(0)

// Memory tracking structure
typedef struct {
    size_t initial_rss;
    size_t initial_heap;
    size_t peak_rss;
    size_t peak_heap;
    size_t allocation_count;
    size_t deallocation_count;
} MemoryTracker;

// Initialize memory tracker
static inline void memory_tracker_init(MemoryTracker* tracker) {
    struct task_basic_info info;
    mach_msg_type_number_t size = sizeof(info);
    task_info(mach_task_self(), TASK_BASIC_INFO, (task_info_t)&info, &size);
    
    tracker->initial_rss = info.resident_size;
    tracker->initial_heap = 0;  // Heap tracking requires malloc interposition
    tracker->peak_rss = tracker->initial_rss;
    tracker->peak_heap = 0;
    tracker->allocation_count = 0;
    tracker->deallocation_count = 0;
}

// Get current RSS
static inline size_t get_current_rss(void) {
    struct task_basic_info info;
    mach_msg_type_number_t size = sizeof(info);
    if (task_info(mach_task_self(), TASK_BASIC_INFO, (task_info_t)&info, &size) == KERN_SUCCESS) {
        return info.resident_size;
    }
    return 0;
}

// Get current heap size (approximate)
static inline size_t get_current_heap(void) {
    vm_size_t page_size;
    vm_statistics_data_t vm_stat;
    mach_msg_type_number_t host_size = sizeof(vm_stat) / sizeof(natural_t);
    
    host_page_size(mach_host_self(), &page_size);
    if (host_statistics(mach_host_self(), HOST_VM_INFO, (host_info_t)&vm_stat, &host_size) == KERN_SUCCESS) {
        return (vm_stat.active_count + vm_stat.inactive_count + vm_stat.wire_count) * page_size;
    }
    return 0;
}

// Update memory tracker
static inline void memory_tracker_update(MemoryTracker* tracker) {
    size_t current_rss = get_current_rss();
    size_t current_heap = get_current_heap();
    
    if (current_rss > tracker->peak_rss) {
        tracker->peak_rss = current_rss;
    }
    if (current_heap > tracker->peak_heap) {
        tracker->peak_heap = current_heap;
    }
}

// Check memory growth (returns true if growth exceeds threshold)
static inline bool memory_tracker_check_growth(MemoryTracker* tracker, double threshold_percent) {
    memory_tracker_update(tracker);
    size_t current_rss = get_current_rss();
    size_t growth = current_rss > tracker->initial_rss ? current_rss - tracker->initial_rss : 0;
    double growth_percent = tracker->initial_rss > 0 ? (double)growth * 100.0 / tracker->initial_rss : 0.0;
    return growth_percent > threshold_percent;
}

// Print memory tracker stats
static inline void memory_tracker_print(const MemoryTracker* tracker, const char* label) {
    size_t current_rss = get_current_rss();
    size_t growth = current_rss > tracker->initial_rss ? current_rss - tracker->initial_rss : 0;
    double growth_percent = tracker->initial_rss > 0 ? (double)growth * 100.0 / tracker->initial_rss : 0.0;
    
    printf("Memory [%s]: RSS=%zu KB (growth: +%zu KB, +%.2f%%), Peak RSS=%zu KB\n",
           label, current_rss / 1024, growth / 1024, growth_percent, tracker->peak_rss / 1024);
}

// Performance metrics structure
typedef struct {
    uint64_t* latencies;      // Array of latency measurements (CPU cycles)
    size_t capacity;
    size_t count;
    uint64_t start_time;
    uint64_t end_time;
} PerformanceMetrics;

// Initialize performance metrics
static inline int performance_metrics_init(PerformanceMetrics* metrics, size_t capacity) {
    metrics->latencies = (uint64_t*)calloc(capacity, sizeof(uint64_t));
    if (!metrics->latencies) {
        return -1;
    }
    metrics->capacity = capacity;
    metrics->count = 0;
    metrics->start_time = 0;
    metrics->end_time = 0;
    return 0;
}

// Record a latency measurement
static inline void performance_metrics_record(PerformanceMetrics* metrics, uint64_t latency_cycles) {
    if (metrics->count < metrics->capacity) {
        metrics->latencies[metrics->count++] = latency_cycles;
    }
}

// Calculate percentiles
static inline uint64_t calculate_percentile(uint64_t* sorted_values, size_t count, double percentile) {
    if (count == 0) return 0;
    size_t idx = (size_t)(percentile * (count - 1));
    if (idx >= count) idx = count - 1;
    return sorted_values[idx];
}

// Compare function for qsort
static int compare_uint64(const void* a, const void* b) {
    uint64_t va = *(const uint64_t*)a;
    uint64_t vb = *(const uint64_t*)b;
    if (va < vb) return -1;
    if (va > vb) return 1;
    return 0;
}

// Get statistics from performance metrics
static inline void performance_metrics_get_stats(PerformanceMetrics* metrics, 
                                                 uint64_t* p50, uint64_t* p90, uint64_t* p95, 
                                                 uint64_t* p99, uint64_t* p999) {
    if (metrics->count == 0) {
        *p50 = *p90 = *p95 = *p99 = *p999 = 0;
        return;
    }
    
    uint64_t* sorted = (uint64_t*)malloc(metrics->count * sizeof(uint64_t));
    if (!sorted) {
        *p50 = *p90 = *p95 = *p99 = *p999 = 0;
        return;
    }
    
    memcpy(sorted, metrics->latencies, metrics->count * sizeof(uint64_t));
    qsort(sorted, metrics->count, sizeof(uint64_t), compare_uint64);
    
    *p50 = calculate_percentile(sorted, metrics->count, 0.50);
    *p90 = calculate_percentile(sorted, metrics->count, 0.90);
    *p95 = calculate_percentile(sorted, metrics->count, 0.95);
    *p99 = calculate_percentile(sorted, metrics->count, 0.99);
    *p999 = calculate_percentile(sorted, metrics->count, 0.999);
    
    free(sorted);
}

// Cleanup performance metrics
static inline void performance_metrics_cleanup(PerformanceMetrics* metrics) {
    if (metrics->latencies) {
        free(metrics->latencies);
        metrics->latencies = NULL;
    }
    metrics->capacity = 0;
    metrics->count = 0;
}

// Generate test WebSocket frame
static inline void generate_test_frame(uint8_t* buffer, size_t payload_size, bool is_binary) {
    if (payload_size < 126) {
        buffer[0] = (is_binary ? 0x82 : 0x81);  // FIN|OPCODE (BINARY or TEXT)
        buffer[1] = (uint8_t)payload_size;
        if (payload_size > 0) {
            memset(buffer + 2, 'X', payload_size);
        }
    } else if (payload_size <= 65535) {
        buffer[0] = (is_binary ? 0x82 : 0x81);
        buffer[1] = 126;
        uint16_t len16 = (uint16_t)payload_size;
        // Network byte order (big-endian)
        buffer[2] = (uint8_t)((len16 >> 8) & 0xFF);
        buffer[3] = (uint8_t)(len16 & 0xFF);
        if (payload_size > 0) {
            memset(buffer + 4, 'X', payload_size);
        }
    } else {
        buffer[0] = (is_binary ? 0x82 : 0x81);
        buffer[1] = 127;
        uint64_t len64 = (uint64_t)payload_size;
        // Network byte order (big-endian)
        for (int i = 0; i < 8; i++) {
            buffer[2 + i] = (uint8_t)((len64 >> (56 - i * 8)) & 0xFF);
        }
        if (payload_size > 0) {
            memset(buffer + 10, 'X', payload_size);
        }
    }
}

// Generate malformed WebSocket frame
static inline void generate_malformed_frame(uint8_t* buffer, int type) {
    switch (type) {
        case 0:  // Invalid opcode (reserved bits)
            buffer[0] = 0xF2;  // FIN|RSV1|RSV2|RSV3|opcode=2 (invalid)
            buffer[1] = 5;
            break;
        case 1:  // Incorrect masking (client must mask)
            buffer[0] = 0x82;
            buffer[1] = 0x05;  // No mask bit set
            break;
        case 2:  // Invalid payload length (oversized)
            buffer[0] = 0x82;
            buffer[1] = 127;  // 64-bit length
            uint64_t huge_len = UINT64_MAX;
            memcpy(buffer + 2, &huge_len, 8);
            break;
        default:
            memset(buffer, 0, 16);
            break;
    }
}

// Set resource limits (for exhaustion testing)
static inline int set_memory_limit(size_t limit_mb) {
    struct rlimit rlim;
    rlim.rlim_cur = limit_mb * 1024 * 1024;
    rlim.rlim_max = limit_mb * 1024 * 1024;
    return setrlimit(RLIMIT_AS, &rlim);
}

// Get resource limit
static inline size_t get_memory_limit(void) {
    struct rlimit rlim;
    if (getrlimit(RLIMIT_AS, &rlim) == 0) {
        return rlim.rlim_cur;
    }
    return 0;
}

#endif // TEST_HELPERS_H

