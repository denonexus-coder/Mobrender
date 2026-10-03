// MobileGlues - gl/mg_upload_queue.h
// Batches small uploads and coalesces contiguous ones before touching GL.
// SPDX-License-Identifier: LGPL-2.1-only
//
// Order is preserved whenever destinations overlap, so coalescing never changes
// observable semantics. A flush drains the queue into the persistent ring (or
// directly into a destination buffer) in one pass.
#ifndef MG_UPLOAD_QUEUE_H
#define MG_UPLOAD_QUEUE_H

#include "mg_buffer_dispatch.h"
#include <stdint.h>
#include <stddef.h>
#include <vector>

struct MGUpload {
    const void* source;
    size_t      size;
    mg_uint     destination;
    size_t      destinationOffset;
};

class MGPersistentMemory;

class MGUploadQueue {
public:
    // Adaptive threshold: once queued bytes exceed this, the caller should
    // flush. Defaults tuned for Sodium's ~16 MiB staging working set.
    void   set_flush_threshold(size_t bytes) { flush_threshold_ = bytes; }
    size_t flush_threshold() const { return flush_threshold_; }
    size_t pending_bytes() const { return pending_bytes_; }
    size_t pending_count() const { return items_.size(); }
    bool   should_flush() const { return pending_bytes_ >= flush_threshold_; }

    // Enqueue one upload. Merges with the previous item when it targets the
    // same buffer at a contiguous destination offset AND the source is
    // contiguous in memory (provably safe to treat as one span).
    void enqueue(const MGUpload& up);

    // Drain: write each (merged) upload through the persistent ring, record
    // dirty ranges, then flush+submit once. `copy_cb` performs the actual
    // memcpy into the mapped pointer so the host controls the copy primitive.
    typedef void (*CopyFn)(void* dst, const void* src, size_t n);
    void flush_to_ring(MGPersistentMemory& ring, CopyFn copy_cb);

    void clear();

private:
    std::vector<MGUpload> items_;
    size_t pending_bytes_ = 0;
    size_t flush_threshold_ = 2u * 1024u * 1024u; // 2 MiB default
};

#endif // MG_UPLOAD_QUEUE_H
