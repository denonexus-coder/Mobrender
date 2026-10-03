// MobileGlues - gl/mg_upload_queue.cpp
// SPDX-License-Identifier: LGPL-2.1-only
#include "mg_upload_queue.h"
#include "mg_persistent_memory.h"
#include "mg_stats.h"
#include <string.h>

void MGUploadQueue::enqueue(const MGUpload& up) {
    if (up.size == 0) return;
    if (!items_.empty()) {
        MGUpload& p = items_.back();
        const uint8_t* psrc = (const uint8_t*)p.source;
        const uint8_t* nsrc = (const uint8_t*)up.source;
        const bool same_dst = p.destination == up.destination;
        const bool dst_contig = p.destinationOffset + p.size == up.destinationOffset;
        const bool src_contig = psrc + p.size == nsrc;
        if (same_dst && dst_contig && src_contig) {
            p.size += up.size;              // provably one contiguous span
            pending_bytes_ += up.size;
            MG_STAT_INC(coalescedUploads);
            return;
        }
    }
    items_.push_back(up);
    pending_bytes_ += up.size;
}

void MGUploadQueue::flush_to_ring(MGPersistentMemory& ring, CopyFn copy_cb) {
    if (items_.empty()) return;
    if (!copy_cb) copy_cb = [](void* d, const void* s, size_t n) { memcpy(d, s, n); };

    for (const MGUpload& up : items_) {
        MGRingAlloc a = ring.reserve(up.size);
        if (!a.ptr) continue;               // ring exhausted; caller re-tries
        copy_cb(a.ptr, up.source, up.size);
        ring.mark_dirty(a.offset, up.size);
        MG_STAT_INC(uploadCalls);
        MG_STAT_ADD(uploadBytes, up.size);
    }
    ring.flush_pending();
    ring.submit();
    clear();
}

void MGUploadQueue::clear() {
    items_.clear();
    pending_bytes_ = 0;
}
