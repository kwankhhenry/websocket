#include "ringbuffer.h"
#include <sys/mman.h>
#include <mach/mach.h>
#include <mach/vm_map.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

int ringbuffer_init(RingBuffer* rb) {
    if (!rb) {
        return -1;
    }
    
    // Allocate 8MB with 128-byte alignment using vm_allocate
    vm_address_t addr = 0;
    vm_size_t size = RINGBUFFER_SIZE;
    
    // Request aligned allocation (manual alignment after allocation)
    kern_return_t kr = vm_allocate(
        mach_task_self(),
        &addr,
        size + RINGBUFFER_ALIGNMENT,  // Allocate extra for alignment
        VM_FLAGS_ANYWHERE | VM_FLAGS_PURGABLE
    );
    
    if (kr != KERN_SUCCESS) {
        return -1;
    }
    
    // Align to 128-byte boundary
    uintptr_t aligned_addr = ((uintptr_t)addr + RINGBUFFER_ALIGNMENT - 1) & ~(RINGBUFFER_ALIGNMENT - 1);
    if (aligned_addr != (uintptr_t)addr) {
        // Deallocate original and reallocate if needed
        // For simplicity, we'll use the address as-is if close enough
        // In production, you'd want proper alignment handling
        vm_deallocate(mach_task_self(), addr, size + RINGBUFFER_ALIGNMENT);
        return -1;  // Simplified: require natural alignment
    }
    
    rb->buf = (char*)addr;
    rb->size = size;
    rb->read_ptr = 0;
    rb->write_ptr = 0;
    rb->locked = 0;
    
    // Lock buffer in physical memory to prevent page faults
    if (mlock(rb->buf, rb->size) != 0) {
        vm_deallocate(mach_task_self(), addr, size);
        return -1;
    }
    
    rb->locked = 1;
    return 0;
}

void ringbuffer_cleanup(RingBuffer* rb) {
    if (!rb || !rb->buf) {
        return;
    }
    
    if (rb->locked) {
        munlock(rb->buf, rb->size);
    }
    
    vm_deallocate(mach_task_self(), (vm_address_t)rb->buf, rb->size);
    rb->buf = NULL;
    rb->size = 0;
    rb->read_ptr = 0;
    rb->write_ptr = 0;
    rb->locked = 0;
}

size_t ringbuffer_write(RingBuffer* rb, const void* data, size_t len) {
    if (!rb || !data || len == 0) {
        return 0;
    }
    
    size_t available = ringbuffer_writeable(rb);
    if (available == 0) {
        return 0;
    }
    
    size_t to_write = (len < available) ? len : available;
    size_t wp = rb->write_ptr;
    
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
    
    // Update write pointer atomically (memory barrier ensures visibility)
    __sync_synchronize();
    rb->write_ptr = (wp + to_write) % rb->size;
    
    return to_write;
}

size_t ringbuffer_read(RingBuffer* rb, void* data, size_t len) {
    if (!rb || !data || len == 0) {
        return 0;
    }
    
    size_t available = ringbuffer_readable(rb);
    if (available == 0) {
        return 0;
    }
    
    size_t to_read = (len < available) ? len : available;
    size_t rp = rb->read_ptr;
    
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
    
    // Update read pointer atomically
    __sync_synchronize();
    rb->read_ptr = (rp + to_read) % rb->size;
    
    return to_read;
}

size_t ringbuffer_writeable(RingBuffer* rb) {
    if (!rb) {
        return 0;
    }
    
    size_t rp = rb->read_ptr;
    size_t wp = rb->write_ptr;
    
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
    
    size_t rp = rb->read_ptr;
    size_t wp = rb->write_ptr;
    
    if (wp >= rp) {
        return wp - rp;
    } else {
        return rb->size - rp + wp;
    }
}

void ringbuffer_reset(RingBuffer* rb) {
    if (rb) {
        __sync_synchronize();
        rb->read_ptr = 0;
        rb->write_ptr = 0;
    }
}

