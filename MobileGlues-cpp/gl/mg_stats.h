// MobileGlues - gl/mg_stats.h
// Internal, switchable counters for the Buffer Storage layer.
// SPDX-License-Identifier: LGPL-2.1-only
//
// All counting goes through MG_STAT(). When MG_BS_STATS is 0 the macro expands
// to nothing, so release builds carry zero overhead on the hot path. Enable with
// -DMG_BS_STATS=1 (or the env toggle wired in mg_buffer_storage.cpp).
#ifndef MG_STATS_H
#define MG_STATS_H

#include <stdint.h>

#ifndef MG_BS_STATS
#define MG_BS_STATS 0
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MGBufferStats {
    uint64_t bufferStorageCalls;
    uint64_t mapCalls;
    uint64_t unmapCalls;
    uint64_t persistentBuffers;
    uint64_t persistentBytes;
    uint64_t uploadCalls;
    uint64_t uploadBytes;
    uint64_t flushCalls;
    uint64_t flushBytes;
    uint64_t coalescedUploads;
    uint64_t copyUploadCalls;   // staging -> final arena
    uint64_t copyUploadBytes;
    uint64_t copyResizeCalls;   // old arena -> new arena
    uint64_t copyResizeBytes;
    uint64_t fenceCreates;
    uint64_t fenceWaits;
    uint64_t fenceTimeouts;
    uint64_t fenceFailures;
    uint64_t ringWraps;
    uint64_t ringStalls;
    uint64_t directArenaUploads;
    uint64_t stagingUploads;
    uint64_t mappedPointerReuse;
} MGBufferStats;

MGBufferStats* mg_bs_stats(void);     // live pointer (may be read any time)
void           mg_bs_stats_reset(void);

#ifdef __cplusplus
}
#endif

#if MG_BS_STATS
#define MG_STAT_ADD(field, n) (mg_bs_stats()->field += (uint64_t)(n))
#define MG_STAT_INC(field)    (mg_bs_stats()->field += 1u)
#else
#define MG_STAT_ADD(field, n) ((void)0)
#define MG_STAT_INC(field)    ((void)0)
#endif

#endif // MG_STATS_H
