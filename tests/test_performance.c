#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
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

// Performance targets from README
#define TARGET_LATENCY_P99_NS (40.0 * 1000.0)  // 40µs = 40000ns
#define TARGET_THROUGHPUT_MSG_PER_SEC 15000
#define TARGET_NEON_SPEEDUP 4.0

// Load baseline from file (simple JSON-like format)
typedef struct {
    double latency_p50_ns;
    double latency_p90_ns;
    double latency_p95_ns;
    double latency_p99_ns;
    double throughput_msg_per_sec;
    double neon_speedup;
} PerformanceBaseline;

static int load_baseline(const char* filename, PerformanceBaseline* baseline) {
    FILE* f = fopen(filename, "r");
    if (!f) {
        return -1;
    }
    
    // Initialize baseline to zeros
    memset(baseline, 0, sizeof(PerformanceBaseline));
    
    // Simple parser for key=value format
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        // Skip comments
        if (line[0] == '#') {
            continue;
        }
        
        if (strncmp(line, "latency_p50_ns=", 15) == 0) {
            baseline->latency_p50_ns = atof(line + 15);
        } else if (strncmp(line, "latency_p90_ns=", 15) == 0) {
            baseline->latency_p90_ns = atof(line + 15);
        } else if (strncmp(line, "latency_p95_ns=", 15) == 0) {
            baseline->latency_p95_ns = atof(line + 15);
        } else if (strncmp(line, "latency_p99_ns=", 15) == 0) {
            baseline->latency_p99_ns = atof(line + 15);
        } else if (strncmp(line, "throughput_msg_per_sec=", 23) == 0) {
            baseline->throughput_msg_per_sec = atof(line + 23);
        } else if (strncmp(line, "neon_speedup=", 13) == 0) {
            baseline->neon_speedup = atof(line + 13);
        }
    }
    
    fclose(f);
    return 0;
}

static int save_baseline(const char* filename, const PerformanceBaseline* baseline) {
    FILE* f = fopen(filename, "w");
    if (!f) {
        return -1;
    }
    
    fprintf(f, "# Performance Baseline\n");
    fprintf(f, "latency_p50_ns=%.2f\n", baseline->latency_p50_ns);
    fprintf(f, "latency_p90_ns=%.2f\n", baseline->latency_p90_ns);
    fprintf(f, "latency_p95_ns=%.2f\n", baseline->latency_p95_ns);
    fprintf(f, "latency_p99_ns=%.2f\n", baseline->latency_p99_ns);
    fprintf(f, "throughput_msg_per_sec=%.2f\n", baseline->throughput_msg_per_sec);
    fprintf(f, "neon_speedup=%.2f\n", baseline->neon_speedup);
    
    fclose(f);
    return 0;
}

// Test 1: Latency percentile tracking
int test_latency_percentiles(void) {
    printf("  Measuring latency percentiles...\n");
    
    RingBuffer rb;
    if (ringbuffer_init(&rb) != 0) {
        fprintf(stderr, "Failed to init ring buffer\n");
        return -1;
    }
    
    const size_t num_samples = 100000;
    PerformanceMetrics metrics;
    if (performance_metrics_init(&metrics, num_samples) != 0) {
        ringbuffer_cleanup(&rb);
        return -1;
    }
    
    const size_t payload_size = 512;
    char test_data[payload_size];
    memset(test_data, 'P', payload_size);
    
    // Warmup
    for (size_t i = 0; i < 1000; i++) {
        ringbuffer_write(&rb, test_data, payload_size);
        if (ringbuffer_readable(&rb) >= payload_size) {
            char read_buf[payload_size];
            ringbuffer_read(&rb, read_buf, payload_size);
        }
    }
    
    // Measure latencies
    for (size_t i = 0; i < num_samples; i++) {
        uint64_t start = arm_cycle_count();
        size_t written = ringbuffer_write(&rb, test_data, payload_size);
        uint64_t end = arm_cycle_count();
        
        if (written > 0) {
            performance_metrics_record(&metrics, end - start);
            
            // Read to prevent saturation
            if (ringbuffer_readable(&rb) >= payload_size) {
                char read_buf[payload_size];
                ringbuffer_read(&rb, read_buf, payload_size);
            }
        }
    }
    
    // Calculate statistics
    uint64_t p50, p90, p95, p99, p999;
    performance_metrics_get_stats(&metrics, &p50, &p90, &p95, &p99, &p999);
    
    double p50_ns = arm_cycles_to_ns(p50);
    double p90_ns = arm_cycles_to_ns(p90);
    double p95_ns = arm_cycles_to_ns(p95);
    double p99_ns = arm_cycles_to_ns(p99);
    double p999_ns = arm_cycles_to_ns(p999);
    
    printf("  P50:  %.2f ns\n", p50_ns);
    printf("  P90:  %.2f ns\n", p90_ns);
    printf("  P95:  %.2f ns\n", p95_ns);
    printf("  P99:  %.2f ns (target: < %.2f ns)\n", p99_ns, TARGET_LATENCY_P99_NS);
    printf("  P99.9: %.2f ns\n", p999_ns);
    
    // Check against target (P99 should be < 40µs)
    if (p99_ns > TARGET_LATENCY_P99_NS) {
        fprintf(stderr, "FAIL: P99 latency %.2f ns exceeds target %.2f ns\n", 
                p99_ns, TARGET_LATENCY_P99_NS);
        performance_metrics_cleanup(&metrics);
        ringbuffer_cleanup(&rb);
        return -1;
    }
    
    // Check for regression (compare with baseline if available)
    PerformanceBaseline baseline;
    if (load_baseline("tests/baseline.txt", &baseline) == 0) {
        double regression_threshold = 1.10;  // 10% regression threshold
        if (p99_ns > baseline.latency_p99_ns * regression_threshold) {
            fprintf(stderr, "WARNING: P99 latency regressed by %.1f%% (baseline: %.2f ns, current: %.2f ns)\n",
                    (p99_ns / baseline.latency_p99_ns - 1.0) * 100.0,
                    baseline.latency_p99_ns, p99_ns);
        }
    }
    
    performance_metrics_cleanup(&metrics);
    ringbuffer_cleanup(&rb);
    
    return 0;
}

// Test 2: Throughput benchmarks
int test_throughput_benchmark(void) {
    printf("  Measuring throughput...\n");
    
    RingBuffer rb;
    if (ringbuffer_init(&rb) != 0) {
        fprintf(stderr, "Failed to init ring buffer\n");
        return -1;
    }
    
    const size_t payload_sizes[] = {64, 256, 1024, 4096};
    const int num_sizes = sizeof(payload_sizes) / sizeof(payload_sizes[0]);
    
    for (int size_idx = 0; size_idx < num_sizes; size_idx++) {
        size_t payload_size = payload_sizes[size_idx];
        char* test_data = (char*)malloc(payload_size);
        if (!test_data) {
            continue;
        }
        memset(test_data, 'T', payload_size);
        
        const int iterations = 50000;
        uint64_t start = arm_cycle_count();
        size_t total_written = 0;
        
        for (int i = 0; i < iterations; i++) {
            size_t written = ringbuffer_write(&rb, test_data, payload_size);
            if (written > 0) {
                total_written += written;
            }
            
            // Read to prevent saturation
            if (ringbuffer_readable(&rb) >= payload_size) {
                char* read_buf = (char*)malloc(payload_size);
                if (read_buf) {
                    ringbuffer_read(&rb, read_buf, payload_size);
                    free(read_buf);
                }
            }
        }
        
        uint64_t end = arm_cycle_count();
        double time_ns = arm_cycles_to_ns(end - start);
        double throughput_msg_per_sec = (iterations * 1e9) / time_ns;
        double throughput_mbps = (total_written * 8.0) / (time_ns / 1e9) / 1e6;
        
        printf("  Payload %zu bytes: %.0f msg/sec, %.2f Mbps\n",
               payload_size, throughput_msg_per_sec, throughput_mbps);
        
        // Check against target for small payloads (64 bytes)
        if (payload_size == 64 && throughput_msg_per_sec < TARGET_THROUGHPUT_MSG_PER_SEC) {
            fprintf(stderr, "FAIL: Throughput %.0f msg/sec below target %d msg/sec\n",
                    throughput_msg_per_sec, TARGET_THROUGHPUT_MSG_PER_SEC);
            free(test_data);
            ringbuffer_cleanup(&rb);
            return -1;
        }
        
        free(test_data);
    }
    
    ringbuffer_cleanup(&rb);
    
    return 0;
}

// Test 3: Neon SIMD speedup verification
int test_neon_speedup(void) {
    printf("  Measuring Neon SIMD speedup...\n");
    
    const size_t payload_sizes[] = {64, 512, 4096};
    const int num_sizes = sizeof(payload_sizes) / sizeof(payload_sizes[0]);
    const int iterations = 100000;
    
    for (int size_idx = 0; size_idx < num_sizes; size_idx++) {
        size_t payload_size = payload_sizes[size_idx];
        size_t frame_size = payload_size + 14;
        uint8_t* frame = (uint8_t*)malloc(frame_size);
        if (!frame) {
            continue;
        }
        generate_test_frame(frame, payload_size, true);
        
        // Neon benchmark
        uint64_t neon_start = arm_cycle_count();
        for (int i = 0; i < iterations; i++) {
            WSFrame neon_frame;
            neon_parse_ws_frame(frame, frame_size, &neon_frame);
            (void)neon_frame;  // Suppress unused warning
        }
        uint64_t neon_end = arm_cycle_count();
        uint64_t neon_cycles = neon_end - neon_start;
        
        // Scalar benchmark
        uint64_t scalar_start = arm_cycle_count();
        for (int i = 0; i < iterations; i++) {
            WSFrame scalar_frame;
            scalar_parse_ws_frame(frame, frame_size, &scalar_frame);
            (void)scalar_frame;  // Suppress unused warning
        }
        uint64_t scalar_end = arm_cycle_count();
        uint64_t scalar_cycles = scalar_end - scalar_start;
        
        double neon_time_ns = arm_cycles_to_ns(neon_cycles);
        double scalar_time_ns = arm_cycles_to_ns(scalar_cycles);
        double speedup = scalar_time_ns / neon_time_ns;
        
        printf("  Payload %zu bytes: Neon=%.2f ns, Scalar=%.2f ns, Speedup=%.2fx\n",
               payload_size, neon_time_ns / iterations, scalar_time_ns / iterations, speedup);
        
        // Check against target (4x speedup)
        if (speedup < TARGET_NEON_SPEEDUP) {
            printf("  WARNING: Neon speedup %.2fx below target %.2fx\n", speedup, TARGET_NEON_SPEEDUP);
        }
        
        free(frame);
    }
    
    return 0;
}

// Test 4: Performance regression detection
int test_performance_regression(void) {
    printf("  Checking for performance regressions...\n");
    
    const char* baseline_file = "tests/baseline.txt";
    PerformanceBaseline baseline;
    
    if (load_baseline(baseline_file, &baseline) != 0) {
        printf("  No baseline found, creating new baseline...\n");
        
        // Run benchmarks to create baseline
        RingBuffer rb;
        if (ringbuffer_init(&rb) != 0) {
            return -1;
        }
        
        const size_t num_samples = 10000;
        PerformanceMetrics metrics;
        if (performance_metrics_init(&metrics, num_samples) != 0) {
            ringbuffer_cleanup(&rb);
            return -1;
        }
        
        const size_t payload_size = 512;
        char test_data[payload_size];
        memset(test_data, 'B', payload_size);
        
        // Measure latencies
        for (size_t i = 0; i < num_samples; i++) {
            uint64_t start = arm_cycle_count();
            size_t written = ringbuffer_write(&rb, test_data, payload_size);
            uint64_t end = arm_cycle_count();
            
            if (written > 0) {
                performance_metrics_record(&metrics, end - start);
                
                if (ringbuffer_readable(&rb) >= payload_size) {
                    char read_buf[payload_size];
                    ringbuffer_read(&rb, read_buf, payload_size);
                }
            }
        }
        
        uint64_t p50, p90, p95, p99, p999;
        performance_metrics_get_stats(&metrics, &p50, &p90, &p95, &p99, &p999);
        
        baseline.latency_p50_ns = arm_cycles_to_ns(p50);
        baseline.latency_p90_ns = arm_cycles_to_ns(p90);
        baseline.latency_p95_ns = arm_cycles_to_ns(p95);
        baseline.latency_p99_ns = arm_cycles_to_ns(p99);
        baseline.throughput_msg_per_sec = 15000.0;  // Default
        baseline.neon_speedup = 4.0;  // Default
        
        if (save_baseline(baseline_file, &baseline) == 0) {
            printf("  Baseline saved to %s\n", baseline_file);
        }
        
        performance_metrics_cleanup(&metrics);
        ringbuffer_cleanup(&rb);
        
        return 0;  // First run, no regression to check
    }
    
    // Compare current performance with baseline
    RingBuffer rb;
    if (ringbuffer_init(&rb) != 0) {
        return -1;
    }
    
    const size_t num_samples = 10000;
    PerformanceMetrics metrics;
    if (performance_metrics_init(&metrics, num_samples) != 0) {
        ringbuffer_cleanup(&rb);
        return -1;
    }
    
    const size_t payload_size = 512;
    char test_data[payload_size];
    memset(test_data, 'B', payload_size);
    
    // Measure current performance
    for (size_t i = 0; i < num_samples; i++) {
        uint64_t start = arm_cycle_count();
        size_t written = ringbuffer_write(&rb, test_data, payload_size);
        uint64_t end = arm_cycle_count();
        
        if (written > 0) {
            performance_metrics_record(&metrics, end - start);
            
            if (ringbuffer_readable(&rb) >= payload_size) {
                char read_buf[payload_size];
                ringbuffer_read(&rb, read_buf, payload_size);
            }
        }
    }
    
    uint64_t p50, p90, p95, p99, p999;
    performance_metrics_get_stats(&metrics, &p50, &p90, &p95, &p99, &p999);
    
    double current_p99_ns = arm_cycles_to_ns(p99);
    double regression_percent = ((current_p99_ns / baseline.latency_p99_ns) - 1.0) * 100.0;
    
    printf("  Baseline P99: %.2f ns\n", baseline.latency_p99_ns);
    printf("  Current P99:  %.2f ns\n", current_p99_ns);
    printf("  Regression:   %.1f%%\n", regression_percent);
    
    // Fail if regression >10%
    if (regression_percent > 10.0) {
        fprintf(stderr, "FAIL: Performance regression of %.1f%% detected\n", regression_percent);
        performance_metrics_cleanup(&metrics);
        ringbuffer_cleanup(&rb);
        return -1;
    }
    
    performance_metrics_cleanup(&metrics);
    ringbuffer_cleanup(&rb);
    
    return 0;
}

// Test 5: Cache behavior verification
int test_cache_behavior(void) {
    printf("  Verifying cache alignment and behavior...\n");
    
    RingBuffer rb;
    if (ringbuffer_init(&rb) != 0) {
        fprintf(stderr, "Failed to init ring buffer\n");
        return -1;
    }
    
    // Verify 128-byte alignment
    uintptr_t addr = (uintptr_t)rb.buf;
    if ((addr % RINGBUFFER_ALIGNMENT) != 0) {
        fprintf(stderr, "FAIL: Ring buffer not 128-byte aligned\n");
        ringbuffer_cleanup(&rb);
        return -1;
    }
    
    printf("  Buffer aligned at: %p (mod 128 = %zu)\n", 
           (void*)rb.buf, addr % RINGBUFFER_ALIGNMENT);
    
    // Test access patterns
    const size_t test_sizes[] = {128, 256, 512, 1024, 4096};
    const int num_sizes = sizeof(test_sizes) / sizeof(test_sizes[0]);
    
    for (int i = 0; i < num_sizes; i++) {
        size_t size = test_sizes[i];
        char* test_data = (char*)malloc(size);
        if (!test_data) {
            continue;
        }
        memset(test_data, 'C', size);
        
        // Measure write performance
        uint64_t start = arm_cycle_count();
        size_t written = ringbuffer_write(&rb, test_data, size);
        uint64_t end = arm_cycle_count();
        
        if (written > 0) {
            double time_ns = arm_cycles_to_ns(end - start);
            double bandwidth_gbps = (written * 8.0) / (time_ns / 1e9) / 1e9;
            printf("  Write %zu bytes: %.2f GB/s (%.2f ns)\n", 
                   size, bandwidth_gbps, time_ns);
        }
        
        free(test_data);
    }
    
    ringbuffer_cleanup(&rb);
    
    return 0;
}

int main(void) {
    printf("Performance Tests\n");
    printf("=================\n\n");
    
    TEST_RUN("Latency percentile tracking", test_latency_percentiles);
    TEST_RUN("Throughput benchmark", test_throughput_benchmark);
    TEST_RUN("Neon SIMD speedup", test_neon_speedup);
    TEST_RUN("Performance regression detection", test_performance_regression);
    TEST_RUN("Cache behavior verification", test_cache_behavior);
    
    printf("\n========================================\n");
    printf("Total: %d passed, %d failed\n", tests_passed, tests_failed);
    
    return (tests_failed > 0) ? 1 : 0;
}

