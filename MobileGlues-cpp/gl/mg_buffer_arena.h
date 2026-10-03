// MobileGlues - gl/mg_buffer_arena.h
// P2: persistent final arena. The final buffer is itself created with
// glBufferStorageEXT + persistently mapped, so the CPU writes straight into the
// GPU-visible arena and the staging -> final glCopyBufferSubData disappears when
// it is safe to skip it. First-fit allocator with free-block coalescing,
// adaptive growth and separate resize-copy accounting.
// SPDX-License-Identifier: LGPL-2.1-only
#ifndef MG_BUFFER_ARENA_H
#define MG_BUFFER_ARENA_H

#include "mg_buffer_dispatch.h"
#include "mg_persistent_memory.h"
#include <stdint.h>
#include <stddef.h>
#include <vector>

struct MGArenaBlock {
    size_t   offset;
    size_t   size;
    bool     free;
    uint64_t generation;   // bumped on free, lets stale handles be detected
};

class MGBufferArena {
public:
    bool create(size_t initial_capacity, bool coherent, uint32_t alignment = 256);
    void destroy();
    bool valid() const { return mem_.valid(); }

    mg_uint  buffer() const { return mem_.buffer(); }
    uint8_t* mapped() const { return mem_.mapped(); }
    size_t   capacity() const { return mem_.capacity(); }

    // Allocate `size` bytes. Returns offset within the arena (SIZE_MAX on
    // failure). *out_ptr receives the writable mapped address when non-null.
    size_t   allocate(size_t size, uint8_t** out_ptr, uint64_t* out_generation);
    void     free_block(size_t offset);

    // Grow the arena. Preserves live contents. The grow copy is a resize copy,
    // counted separately from upload copies (copyResizeBytes).
    bool     grow(size_t needed);

    // Fragmentation ratio in [0,1]: 1 - (largest_free / total_free).
    double   fragmentation() const;
    size_t   total_free() const;

    void set_max_capacity(size_t m) { max_capacity_ = m; }

private:
    void     coalesce();
    size_t   align_up(size_t v) const { return (v + (align_ - 1)) & ~(size_t)(align_ - 1); }

    MGPersistentMemory       mem_;
    std::vector<MGArenaBlock> blocks_;   // ordered by offset
    uint32_t align_ = 256;
    size_t   max_capacity_ = 0;
    uint64_t generation_ = 1;
};

#endif // MG_BUFFER_ARENA_H
