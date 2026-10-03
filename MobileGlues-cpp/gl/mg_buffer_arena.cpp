// MobileGlues - gl/mg_buffer_arena.cpp
// SPDX-License-Identifier: LGPL-2.1-only
#include "mg_buffer_arena.h"
#include "mg_stats.h"
#include <algorithm>
#include <string.h>

bool MGBufferArena::create(size_t initial_capacity, bool coherent, uint32_t alignment) {
    align_ = alignment ? alignment : 256;
    size_t cap = align_up(initial_capacity ? initial_capacity : align_);
    if (max_capacity_) mem_.set_max_capacity(max_capacity_);
    if (!mem_.create(cap, coherent, align_)) return false;
    blocks_.clear();
    blocks_.push_back(MGArenaBlock{0, mem_.capacity(), true, generation_});
    return true;
}

void MGBufferArena::destroy() { mem_.destroy(); blocks_.clear(); }

size_t MGBufferArena::allocate(size_t size, uint8_t** out_ptr, uint64_t* out_generation) {
    if (!valid() || size == 0) return SIZE_MAX;
    size_t need = align_up(size);
    for (size_t i = 0; i < blocks_.size(); ++i) {
        MGArenaBlock& b = blocks_[i];
        if (b.free && b.size >= need) {
            size_t off = b.offset;
            if (b.size > need) {
                MGArenaBlock rem{b.offset + need, b.size - need, true, generation_};
                b.size = need; b.free = false; b.generation = ++generation_;
                blocks_.insert(blocks_.begin() + (long)i + 1, rem);
            } else {
                b.free = false; b.generation = ++generation_;
            }
            if (out_ptr) *out_ptr = mem_.mapped() + off;
            if (out_generation) *out_generation = blocks_[i].generation;
            return off;
        }
    }
    // No fit: grow and retry once.
    if (grow(mem_.capacity() + need)) return allocate(size, out_ptr, out_generation);
    return SIZE_MAX;
}

void MGBufferArena::free_block(size_t offset) {
    for (auto& b : blocks_) {
        if (!b.free && b.offset == offset) {
            b.free = true;
            b.generation = ++generation_;
            coalesce();
            return;
        }
    }
}

void MGBufferArena::coalesce() {
    std::sort(blocks_.begin(), blocks_.end(),
              [](const MGArenaBlock& a, const MGArenaBlock& b) { return a.offset < b.offset; });
    for (size_t i = 0; i + 1 < blocks_.size();) {
        if (blocks_[i].free && blocks_[i + 1].free &&
            blocks_[i].offset + blocks_[i].size == blocks_[i + 1].offset) {
            blocks_[i].size += blocks_[i + 1].size;
            blocks_.erase(blocks_.begin() + (long)i + 1);
        } else {
            ++i;
        }
    }
}

size_t MGBufferArena::total_free() const {
    size_t f = 0;
    for (const auto& b : blocks_) if (b.free) f += b.size;
    return f;
}

double MGBufferArena::fragmentation() const {
    size_t total = 0, largest = 0;
    for (const auto& b : blocks_) if (b.free) { total += b.size; if (b.size > largest) largest = b.size; }
    if (total == 0) return 0.0;
    return 1.0 - (double)largest / (double)total;
}

bool MGBufferArena::grow(size_t needed) {
    size_t old_cap = mem_.capacity();
    // Adaptive growth: double, but never below `needed`.
    size_t target = old_cap ? old_cap : align_;
    while (target < needed) target *= 2;
    if (max_capacity_ && target > max_capacity_) {
        if (needed > max_capacity_) return false;
        target = max_capacity_;
    }
    if (target <= old_cap) return false;

    // Snapshot the live arena contents, build a larger persistent buffer, copy
    // them in, then swap it in. The grow copy is a RESIZE copy, accounted
    // separately from upload copies.
    std::vector<uint8_t> snapshot;
    if (mem_.mapped() && old_cap) {
        snapshot.resize(old_cap);
        memcpy(snapshot.data(), mem_.mapped(), old_cap);
    }

    bool coherent = mem_.coherent();
    MGPersistentMemory grown;
    grown.set_max_capacity(max_capacity_);
    if (!grown.create(target, coherent, align_)) return false;
    if (!snapshot.empty()) memcpy(grown.mapped(), snapshot.data(), snapshot.size());
    MG_STAT_INC(copyResizeCalls);
    MG_STAT_ADD(copyResizeBytes, snapshot.size());

    mem_.swap(grown);   // mem_ now owns the larger buffer; grown frees the old

    blocks_.push_back(MGArenaBlock{old_cap, target - old_cap, true, generation_});
    coalesce();
    return true;
}
