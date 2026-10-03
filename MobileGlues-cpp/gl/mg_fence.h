// MobileGlues - gl/mg_fence.h
// Safe fence lifetime + non-blocking wait helpers for the ring allocator.
// SPDX-License-Identifier: LGPL-2.1-only
//
// The real GE8320 driver was observed to:
//   * return GL_ALREADY_SIGNALED / GL_CONDITION_SATISFIED / GL_TIMEOUT_EXPIRED
//     from glClientWaitSync,
//   * raise GL_INVALID_OPERATION from glDeleteSync when the sync was already
//     finished/deleted.
// So we never ignore sync errors, never double-delete, and never assume only
// two wait results exist.
#ifndef MG_FENCE_H
#define MG_FENCE_H

#include "mg_buffer_dispatch.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum MGFenceState {
    MG_FENCE_DONE = 0,   // signalled: region may be reused
    MG_FENCE_BUSY,       // still in flight: do NOT block again immediately
    MG_FENCE_DEFENSIVE   // GL_WAIT_FAILED: region not reusable until a safe point
} MGFenceState;

// Create a fence after the current command stream. Returns null on failure.
mg_sync      MGFenceCreate(void);

// Non-blocking poll (timeout 0). Translates all four documented results into an
// MGFenceState. Accepts a null sync as DONE (nothing was ever submitted).
MGFenceState MGFencePoll(mg_sync fence);

// Bounded wait (last-resort only). timeout_ns == 0 behaves like MGFencePoll.
MGFenceState MGFenceWait(mg_sync fence, mg_uint64 timeout_ns);

// Delete a sync exactly once, swallowing the driver's GL_INVALID_OPERATION and
// clearing it from the error latch so it cannot contaminate the next GL call.
// No-op on a null handle; safe against double-delete at the call site.
void         MGDeleteSyncSafe(mg_sync fence);

#ifdef __cplusplus
}
#endif

#endif // MG_FENCE_H
