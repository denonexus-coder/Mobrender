// MobileGlues - gl/mg_persistent_memory.h
// Persistent-mapped ring allocator + explicit-flush range coalescing.
// SPDX-License-Identifier: LGPL-2.1-only
//
// One buffer is created with glBufferStorageEXT(PERSISTENT|WRITE[|COHERENT]) and
// mapped ONCE with glMapBufferRange; the pointer is kept for the buffer's whole
// life (the GE8320 probe showed a second map of an already-mapped persistent
// buffer returns NULL, so we never remap). Writes go straight to mappedPtr+off.
// Regions handed to the GPU are fenced; a region returns to the free pool only
// after its fence signals. No glFinish on the normal path, no malloc per upload.
#ifndef MG_PERSISTENT_MEMORY_H
#define MG_PERSISTENT_MEMORY_H

#include "mg_buffer_dispatch.h"
#include "mg_fence.h"
#include <stdint.h>
#include <stddef.h>
#include <vector>

// Explicit-flush (MODE_EXPLICIT_FLUSH) semantics: CPU write -> record dirty
// range -> coalesce -> glFlushMappedBufferRange -> submit. Adjacent/overlapping
// ranges merge so thousands of small uploads become a handful of flush calls.
class MGFlushRange {
public:
    void reset() { ranges_.clear(); }
    bool empty() const { return ranges_.empty(); }
    // Record a dirty [offset, offset+size) span (clamped to [0,cap) by caller).
    void add(size_t offset, size_t size);
    // Flush every merged span against `target` (already bound), then clear.
    void flush(mg_enum target);
    size_t range_count() const { return ranges_.size(); }
private:
    struct Span { size_t begin; size_t end; };
    std::vector<Span> ranges_;
};

struct MGInFlightRegion {
    size_t  begin;
    size_t  end;
    mg_sync fence;
    uint64_t frame;
};

struct MGRingAlloc {
    uint8_t* ptr;     // mappedPtr + offset (null on failure)
    size_t   offset;  // byte offset inside the ring buffer
    size_t   size;    // granted size (== requested, aligned up)
};

class MGPersistentMemory {
public:
    MGPersistentMemory() = default;
    ~MGPersistentMemory();
    MGPersistentMemory(const MGPersistentMemory&) = delete;
    MGPersistentMemory& operator=(const MGPersistentMemory&) = delete;

    // Create + persistently map `capacity` bytes. `coherent` selects
    // PERSISTENT|COHERENT (no explicit flush) vs PERSISTENT (explicit flush).
    // Returns false if the backend is unavailable or the map returned NULL.
    bool create(size_t capacity, bool coherent, uint32_t alignment = 64);
    void destroy();
    bool valid() const { return mapped_ != nullptr; }

    mg_uint  buffer() const { return buffer_; }
    uint8_t* mapped() const { return mapped_; }
    size_t   capacity() const { return capacity_; }
    bool     coherent() const { return coherent_; }

    // Reserve a contiguous block. Retires finished regions first; tries free
    // space, then ring advance, then (if allowed) grow, and only as a last
    // resort a bounded fence wait. Returns {null,...} if it genuinely cannot.
    MGRingAlloc reserve(size_t size);

    // Mark [offset,offset+size) dirty (explicit-flush mode only; no-op when
    // coherent). Call after writing through the returned pointer.
    void mark_dirty(size_t offset, size_t size);

    // Flush all accumulated dirty ranges (explicit-flush mode only).
    void flush_pending();

    // Fence everything reserved since the last submit so the GPU can be told
    // which bytes are now its own. Call once per batch/frame after flush.
    void submit();

    // Poll fences and move completed regions back to the free pool.
    void recycle();

    void set_max_capacity(size_t m) { max_capacity_ = m; }
    void set_frame(uint64_t f) { frame_ = f; }

    // Swap all internals with another instance (used by arena resize). After
    // the swap each object owns the other's GL buffer; neither frees twice.
    void swap(MGPersistentMemory& o);

private:
    bool  grow(size_t needed);
    void  bind_copy_write();
    size_t align_up(size_t v) const { return (v + (alignment_ - 1)) & ~(size_t)(alignment_ - 1); }

    mg_uint  buffer_ = 0;
    uint8_t* mapped_ = nullptr;
    size_t   capacity_ = 0;
    size_t   max_capacity_ = 0;      // 0 == no cap
    uint32_t alignment_ = 64;
    bool     coherent_ = false;

    size_t   head_ = 0;              // next free write cursor
    size_t   pending_begin_ = 0;     // start of the not-yet-submitted span
    size_t   pending_end_ = 0;       // end of the not-yet-submitted span
    bool     has_pending_ = false;
    uint64_t frame_ = 0;

    std::vector<MGInFlightRegion> inflight_;  // FIFO, oldest at front
    MGFlushRange flush_;
};

#endif // MG_PERSISTENT_MEMORY_H
