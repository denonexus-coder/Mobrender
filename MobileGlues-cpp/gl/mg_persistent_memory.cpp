// MobileGlues - gl/mg_persistent_memory.cpp
// SPDX-License-Identifier: LGPL-2.1-only
#include "mg_persistent_memory.h"
#include "mg_buffer_storage_flags.h"
#include "mg_stats.h"
#include <algorithm>
#include <string.h>

// ---------------------------------------------------------------------------
// MGFlushRange: merge adjacent/overlapping dirty spans, then one flush each.
// ---------------------------------------------------------------------------
void MGFlushRange::add(size_t offset, size_t size) {
    if (size == 0) return;
    Span s{offset, offset + size};
    // Insert keeping the vector sorted by begin, merging as we go.
    ranges_.push_back(s);
    std::sort(ranges_.begin(), ranges_.end(),
              [](const Span& a, const Span& b) { return a.begin < b.begin; });
    std::vector<Span> merged;
    merged.reserve(ranges_.size());
    for (const Span& r : ranges_) {
        if (!merged.empty() && r.begin <= merged.back().end) {
            if (r.end > merged.back().end) merged.back().end = r.end;
        } else {
            merged.push_back(r);
        }
    }
    ranges_.swap(merged);
}

void MGFlushRange::flush(mg_enum target) {
    const MGBufferDispatch* d = mg_bs_dispatch();
    if (!d->FlushMappedBufferRange) { ranges_.clear(); return; }
    for (const Span& r : ranges_) {
        d->FlushMappedBufferRange(target, (mg_intptr)r.begin, (mg_sizeiptr)(r.end - r.begin));
        MG_STAT_INC(flushCalls);
        MG_STAT_ADD(flushBytes, r.end - r.begin);
    }
    ranges_.clear();
}

// ---------------------------------------------------------------------------
// MGPersistentMemory
// ---------------------------------------------------------------------------
MGPersistentMemory::~MGPersistentMemory() { destroy(); }

void MGPersistentMemory::swap(MGPersistentMemory& o) {
    std::swap(buffer_, o.buffer_);
    std::swap(mapped_, o.mapped_);
    std::swap(capacity_, o.capacity_);
    std::swap(max_capacity_, o.max_capacity_);
    std::swap(alignment_, o.alignment_);
    std::swap(coherent_, o.coherent_);
    std::swap(head_, o.head_);
    std::swap(pending_begin_, o.pending_begin_);
    std::swap(pending_end_, o.pending_end_);
    std::swap(has_pending_, o.has_pending_);
    std::swap(frame_, o.frame_);
    inflight_.swap(o.inflight_);
    std::swap(flush_, o.flush_);
}

void MGPersistentMemory::bind_copy_write() {
    const MGBufferDispatch* d = mg_bs_dispatch();
    if (d->BindBuffer) d->BindBuffer(MG_COPY_WRITE_BUFFER, buffer_);
}

bool MGPersistentMemory::create(size_t capacity, bool coherent, uint32_t alignment) {
    if (!mg_bs_dispatch_ready()) return false;
    if (mapped_) destroy();
    const MGBufferDispatch* d = mg_bs_dispatch();
    alignment_ = alignment ? alignment : 64;
    capacity_  = align_up(capacity ? capacity : alignment_);
    coherent_  = coherent;

    d->GenBuffers(1, &buffer_);
    d->BindBuffer(MG_COPY_WRITE_BUFFER, buffer_);

    uint32_t arb = MG_MAP_WRITE_BIT | MG_ARB_MAP_PERSISTENT_BIT;
    if (coherent_) arb |= MG_ARB_MAP_COHERENT_BIT;
    uint32_t dropped = 0;
    uint32_t ext = translateStorageFlagsARBtoEXT(arb, &dropped);
    d->BufferStorageEXT(MG_COPY_WRITE_BUFFER, (mg_sizeiptr)capacity_, nullptr, ext);
    MG_STAT_INC(bufferStorageCalls);

    uint32_t marb = MG_MAP_WRITE_BIT | MG_ARB_MAP_PERSISTENT_BIT;
    if (coherent_) marb |= MG_ARB_MAP_COHERENT_BIT;
    else           marb |= MG_MAP_FLUSH_EXPLICIT_BIT;
    uint32_t mext = translateMapFlagsARBtoBackend(marb, &dropped);
    mapped_ = (uint8_t*)d->MapBufferRange(MG_COPY_WRITE_BUFFER, 0, (mg_sizeiptr)capacity_, mext);
    MG_STAT_INC(mapCalls);
    if (!mapped_) { destroy(); return false; }

    MG_STAT_INC(persistentBuffers);
    MG_STAT_ADD(persistentBytes, capacity_);
    head_ = 0; pending_begin_ = 0; pending_end_ = 0; has_pending_ = false;
    inflight_.clear(); flush_.reset();
    return true;
}

void MGPersistentMemory::destroy() {
    const MGBufferDispatch* d = mg_bs_dispatch();
    for (auto& r : inflight_) MGDeleteSyncSafe(r.fence);
    inflight_.clear();
    if (buffer_ && d->UnmapBuffer && mapped_) {
        if (d->BindBuffer) d->BindBuffer(MG_COPY_WRITE_BUFFER, buffer_);
        d->UnmapBuffer(MG_COPY_WRITE_BUFFER);
        MG_STAT_INC(unmapCalls);
    }
    if (buffer_ && d->DeleteBuffers) d->DeleteBuffers(1, &buffer_);
    buffer_ = 0; mapped_ = nullptr; capacity_ = 0;
    head_ = 0; has_pending_ = false; flush_.reset();
}

// true if [begin,end) fits in the buffer and overlaps no in-flight region.
static inline bool overlaps(size_t b0, size_t e0, size_t b1, size_t e1) {
    return b0 < e1 && b1 < e0;
}

MGRingAlloc MGPersistentMemory::reserve(size_t size) {
    MGRingAlloc fail{nullptr, 0, 0};
    if (!mapped_) return fail;
    size_t na = align_up(size ? size : 1);
    if (na > capacity_) { if (!grow(na)) return fail; }

    for (int attempt = 0; attempt < 3; ++attempt) {
        recycle();
        // Try at head_ without wrapping.
        if (head_ + na <= capacity_) {
            bool clash = false;
            for (const auto& r : inflight_)
                if (overlaps(head_, head_ + na, r.begin, r.end)) { clash = true; break; }
            if (!clash) {
                if (!has_pending_) { pending_begin_ = head_; has_pending_ = true; }
                MGRingAlloc a{mapped_ + head_, head_, na};
                head_ += na;
                pending_end_ = head_;
                return a;
            }
        } else {
            // Would overrun the end: fence what we have and wrap to 0.
            submit();
            head_ = 0;
            MG_STAT_INC(ringWraps);
            bool clash = false;
            for (const auto& r : inflight_)
                if (overlaps(0, na, r.begin, r.end)) { clash = true; break; }
            if (!clash && na <= capacity_) {
                pending_begin_ = 0; has_pending_ = true;
                MGRingAlloc a{mapped_, 0, na};
                head_ = na; pending_end_ = na;
                return a;
            }
        }
        // Pressure: try to grow (bounded), else wait on the oldest fence.
        if (max_capacity_ == 0 || capacity_ * 2 <= max_capacity_) {
            if (grow(capacity_ + na)) continue;
        }
        if (!inflight_.empty()) {
            MG_STAT_INC(ringStalls);
            MGFenceWait(inflight_.front().fence, 2000000ull /* 2 ms */);
            recycle();
        } else {
            break;
        }
    }
    return fail;
}

void MGPersistentMemory::mark_dirty(size_t offset, size_t size) {
    if (coherent_ || size == 0) return;
    if (offset >= capacity_) return;
    if (offset + size > capacity_) size = capacity_ - offset;
    flush_.add(offset, size);
}

void MGPersistentMemory::flush_pending() {
    if (coherent_ || flush_.empty()) return;
    bind_copy_write();
    flush_.flush(MG_COPY_WRITE_BUFFER);
}

void MGPersistentMemory::submit() {
    if (!has_pending_) return;
    mg_sync f = MGFenceCreate();
    inflight_.push_back(MGInFlightRegion{pending_begin_, pending_end_, f, frame_});
    has_pending_ = false;
    pending_begin_ = head_;
    pending_end_ = head_;
}

void MGPersistentMemory::recycle() {
    while (!inflight_.empty()) {
        MGInFlightRegion& r = inflight_.front();
        MGFenceState st = MGFencePoll(r.fence);
        if (st == MG_FENCE_BUSY) break;         // oldest still running; stop
        // DONE or DEFENSIVE: reclaim (defensive is pathological, documented).
        MGDeleteSyncSafe(r.fence);
        inflight_.erase(inflight_.begin());
    }
}

bool MGPersistentMemory::grow(size_t needed) {
    const MGBufferDispatch* d = mg_bs_dispatch();
    size_t target = capacity_;
    while (target < needed) target *= 2;
    if (max_capacity_ && target > max_capacity_) {
        if (needed > max_capacity_) return false;
        target = max_capacity_;
    }
    if (target == capacity_) return false;

    // Growing a live ring is only safe once the GPU is done with it. Drain.
    submit();
    for (int i = 0; i < 64 && !inflight_.empty(); ++i) {
        if (!inflight_.empty()) MGFenceWait(inflight_.front().fence, 4000000ull);
        recycle();
    }
    if (!inflight_.empty()) return false;  // still busy: refuse rather than risk

    size_t live = head_;                   // bytes currently written [0,head_)
    MGPersistentMemory next;
    next.alignment_ = alignment_;
    next.max_capacity_ = max_capacity_;
    if (!next.create(target, coherent_, alignment_)) return false;
    if (live && mapped_ && next.mapped_) memcpy(next.mapped_, mapped_, live);

    // Swap guts; `next` dtor tears down our old buffer.
    std::swap(buffer_, next.buffer_);
    std::swap(mapped_, next.mapped_);
    std::swap(capacity_, next.capacity_);
    coherent_ = next.coherent_;
    head_ = live; pending_begin_ = live; pending_end_ = live; has_pending_ = false;
    flush_.reset();
    (void)d;
    return true;
}
