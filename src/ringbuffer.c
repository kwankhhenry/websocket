#include "ringbuffer.h"
#include <sys/mman.h>
#include <mach/mach.h>
#include <mach/vm_map.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <stdatomic.h>

int ringbuffer_init(RingBuffer* rb) {
    if (!rb) {
        return -1;
    }
    
    // Allocate 8MB with 128-byte alignment using vm_allocate
    vm_address_t addr = 0;
    vm_size_t total_size = RINGBUFFER_SIZE + RINGBUFFER_ALIGNMENT;  // Extra for alignment
    
    // Request aligned allocation (manual alignment after allocation)
    kern_return_t kr = vm_allocate(
        mach_task_self(),
        &addr,
        total_size,
        VM_FLAGS_ANYWHERE | VM_FLAGS_PURGABLE
    );
    
    if (kr != KERN_SUCCESS) {
        return -1;
    }
    
    // Safe alignment (no deallocation of valid memory)
    uintptr_t aligned_addr = ((uintptr_t)addr + RINGBUFFER_ALIGNMENT - 1) & ~(RINGBUFFER_ALIGNMENT - 1);
    rb->buf = (char*)aligned_addr;
    rb->size = RINGBUFFER_SIZE;
    rb->read_ptr = 0;
    rb->write_ptr = 0;
    rb->locked = 0;
    
    // Lock memory (skip if mlock fails to avoid crash)
    if (mlock(rb->buf, rb->size) == 0) {
        rb->locked = 1;
    }
    
    return 0;
}

void ringbuffer_cleanup(RingBuffer* rb) {
    if (!rb || !rb->buf) {
        return;
    }
    
    if (rb->locked) {
        munlock(rb->buf, rb->size);
    }
    
    // Deallocate original allocated address (not aligned addr)
    vm_address_t orig_addr = (vm_address_t)((uintptr_t)rb->buf & ~(RINGBUFFER_ALIGNMENT - 1));
    vm_deallocate(mach_task_self(), orig_addr, RINGBUFFER_SIZE + RINGBUFFER_ALIGNMENT);
    
    memset(rb, 0, sizeof(RingBuffer));
}

size_t ringbuffer_write(RingBuffer* rb, const void* data, size_t len) {
    if (!rb || !data || len == 0) {
        return 0;
    }
    
    // SPSC lock-free: acquire read pointer with ACQUIRE semantics
    size_t rp = __atomic_load_n(&rb->read_ptr, __ATOMIC_ACQUIRE);
    size_t wp = rb->write_ptr;
    
    // Calculate available space
    size_t available;
    if (wp >= rp) {
        available = rb->size - wp + rp;
        if (available > 1) available -= 1;  // Reserve one byte to distinguish full from empty
        else available = 0;
    } else {
        available = rp - wp - 1;
    }
    
    if (available == 0) {
        return 0;
    }
    
    size_t to_write = (len < available) ? len : available;
    
    // Handle wrap-around case
    if (wp + to_write <= rb->size) {
        // Single contiguous write
        memcpy(rb->buf + wp, data, to_write);
    } else {
        // Wrap-around: write to end, then to beginning
        size_t first_part = rb->size - wp;
        memcpy(rb->buf + wp, data, first_part);
        memcpy(rb->buf, (const char*)data + first_part, to_write - first_part);
    }
    
    // Update write pointer atomically with RELEASE semantics (SPSC optimization)
    size_t new_wp = (wp + to_write) % rb->size;
    __atomic_store_n(&rb->write_ptr, new_wp, __ATOMIC_RELEASE);
    
    return to_write;
}

size_t ringbuffer_read(RingBuffer* rb, void* data, size_t len) {
    if (!rb || !data || len == 0) {
        return 0;
    }
    
    // SPSC lock-free: acquire write pointer with ACQUIRE semantics
    size_t wp = __atomic_load_n(&rb->write_ptr, __ATOMIC_ACQUIRE);
    size_t rp = rb->read_ptr;
    
    // Calculate available data
    size_t available;
    if (wp >= rp) {
        available = wp - rp;
    } else {
        available = rb->size - rp + wp;
    }
    
    if (available == 0) {
        return 0;
    }
    
    size_t to_read = (len < available) ? len : available;
    
    // Handle wrap-around case
    if (rp + to_read <= rb->size) {
        // Single contiguous read
        memcpy(data, rb->buf + rp, to_read);
    } else {
        // Wrap-around: read from end, then from beginning
        size_t first_part = rb->size - rp;
        memcpy(data, rb->buf + rp, first_part);
        memcpy((char*)data + first_part, rb->buf, to_read - first_part);
    }
    
    // Update read pointer atomically with RELEASE semantics (SPSC optimization)
    size_t new_rp = (rp + to_read) % rb->size;
    __atomic_store_n(&rb->read_ptr, new_rp, __ATOMIC_RELEASE);
    
    return to_read;
}

size_t ringbuffer_writeable(RingBuffer* rb) {
    if (!rb) {
        return 0;
    }
    
    // Use atomic load for SPSC lock-free design
    size_t rp = __atomic_load_n(&rb->read_ptr, __ATOMIC_ACQUIRE);
    size_t wp = __atomic_load_n(&rb->write_ptr, __ATOMIC_RELAXED);
    
    if (wp >= rp) {
        // Write pointer ahead or equal to read pointer
        size_t available = rb->size - wp + rp;
        // Reserve one byte to distinguish full from empty
        return (available > 1) ? (available - 1) : 0;
    } else {
        // Read pointer ahead of write pointer
        return rp - wp - 1;
    }
}

size_t ringbuffer_readable(RingBuffer* rb) {
    if (!rb) {
        return 0;
    }
    
    // Use atomic load for SPSC lock-free design
    size_t rp = __atomic_load_n(&rb->read_ptr, __ATOMIC_RELAXED);
    size_t wp = __atomic_load_n(&rb->write_ptr, __ATOMIC_ACQUIRE);
    
    if (wp >= rp) {
        return wp - rp;
    } else {
        return rb->size - rp + wp;
    }
}

void ringbuffer_reset(RingBuffer* rb) {
    if (rb) {
        // Use atomic store for reset (RELEASE semantics ensure visibility)
        __atomic_store_n(&rb->read_ptr, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&rb->write_ptr, 0, __ATOMIC_RELEASE);
    }
}

// Batch write operation (HFT optimization: reduces function call overhead)
size_t ringbuffer_batch_write(RingBuffer* rb, const void** data_array, size_t* len_array, size_t count) {
    if (!rb || !data_array || !len_array || count == 0) {
        return 0;
    }
    
    // SPSC lock-free: acquire read pointer once for all chunks
    size_t rp = __atomic_load_n(&rb->read_ptr, __ATOMIC_ACQUIRE);
    size_t wp = rb->write_ptr;
    size_t total_written = 0;
    
    for (size_t i = 0; i < count; i++) {
        if (!data_array[i] || len_array[i] == 0) {
            continue;
        }
        
        // Calculate available space
        size_t available;
        if (wp >= rp) {
            available = rb->size - wp + rp;
            if (available > 1) available -= 1;
            else available = 0;
        } else {
            available = rp - wp - 1;
        }
        
        if (available == 0) {
            break;  // Buffer full, stop writing
        }
        
        size_t to_write = (len_array[i] < available) ? len_array[i] : available;
        
        // Handle wrap-around case
        if (wp + to_write <= rb->size) {
            // Single contiguous write
            memcpy(rb->buf + wp, data_array[i], to_write);
        } else {
            // Wrap-around: write to end, then to beginning
            size_t first_part = rb->size - wp;
            memcpy(rb->buf + wp, data_array[i], first_part);
            memcpy(rb->buf, (const char*)data_array[i] + first_part, to_write - first_part);
        }
        
        wp = (wp + to_write) % rb->size;
        total_written += to_write;
    }
    
    // Update write pointer atomically once for all chunks (SPSC optimization)
    if (total_written > 0) {
        __atomic_store_n(&rb->write_ptr, wp, __ATOMIC_RELEASE);
    }
    
    return total_written;
}

// Batch read operation (HFT optimization: reduces function call overhead)
size_t ringbuffer_batch_read(RingBuffer* rb, void** data_array, size_t* len_array, size_t count) {
    if (!rb || !data_array || !len_array || count == 0) {
        return 0;
    }
    
    // SPSC lock-free: acquire write pointer once for all chunks
    size_t wp = __atomic_load_n(&rb->write_ptr, __ATOMIC_ACQUIRE);
    size_t rp = rb->read_ptr;
    size_t total_read = 0;
    
    for (size_t i = 0; i < count; i++) {
        if (!data_array[i] || len_array[i] == 0) {
            continue;
        }
        
        // Calculate available data
        size_t available;
        if (wp >= rp) {
            available = wp - rp;
        } else {
            available = rb->size - rp + wp;
        }
        
        if (available == 0) {
            break;  // Buffer empty, stop reading
        }
        
        size_t to_read = (len_array[i] < available) ? len_array[i] : available;
        
        // Handle wrap-around case
        if (rp + to_read <= rb->size) {
            // Single contiguous read
            memcpy(data_array[i], rb->buf + rp, to_read);
        } else {
            // Wrap-around: read from end, then from beginning
            size_t first_part = rb->size - rp;
            memcpy(data_array[i], rb->buf + rp, first_part);
            memcpy((char*)data_array[i] + first_part, rb->buf, to_read - first_part);
        }
        
        rp = (rp + to_read) % rb->size;
        total_read += to_read;
    }
    
    // Update read pointer atomically once for all chunks (SPSC optimization)
    if (total_read > 0) {
        __atomic_store_n(&rb->read_ptr, rp, __ATOMIC_RELEASE);
    }
    
    return total_read;
}

