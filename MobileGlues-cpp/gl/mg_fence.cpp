// MobileGlues - gl/mg_fence.cpp
// SPDX-License-Identifier: LGPL-2.1-only
#include "mg_fence.h"
#include "mg_stats.h"

extern "C" mg_sync MGFenceCreate(void) {
    const MGBufferDispatch* d = mg_bs_dispatch();
    if (!d->FenceSync) return nullptr;
    mg_sync s = d->FenceSync(MG_SYNC_GPU_COMMANDS_COMPLETE, 0u);
    if (s) MG_STAT_INC(fenceCreates);
    return s;
}

static MGFenceState translate_wait(mg_enum r) {
    switch (r) {
        case MG_ALREADY_SIGNALED:
        case MG_CONDITION_SATISFIED:
            return MG_FENCE_DONE;
        case MG_TIMEOUT_EXPIRED:
            MG_STAT_INC(fenceTimeouts);
            return MG_FENCE_BUSY;
        case MG_WAIT_FAILED:
        default:
            MG_STAT_INC(fenceFailures);
            return MG_FENCE_DEFENSIVE;
    }
}

extern "C" MGFenceState MGFencePoll(mg_sync fence) {
    if (!fence) return MG_FENCE_DONE;
    const MGBufferDispatch* d = mg_bs_dispatch();
    if (!d->ClientWaitSync) return MG_FENCE_DEFENSIVE;
    MG_STAT_INC(fenceWaits);
    // Flush the command stream so a fence created this frame can actually
    // signal; without the flush bit a never-flushed context can spin forever.
    return translate_wait(d->ClientWaitSync(fence, MG_SYNC_FLUSH_COMMANDS_BIT, 0));
}

extern "C" MGFenceState MGFenceWait(mg_sync fence, mg_uint64 timeout_ns) {
    if (!fence) return MG_FENCE_DONE;
    if (timeout_ns == 0) return MGFencePoll(fence);
    const MGBufferDispatch* d = mg_bs_dispatch();
    if (!d->ClientWaitSync) return MG_FENCE_DEFENSIVE;
    MG_STAT_INC(fenceWaits);
    return translate_wait(d->ClientWaitSync(fence, MG_SYNC_FLUSH_COMMANDS_BIT, timeout_ns));
}

extern "C" void MGDeleteSyncSafe(mg_sync fence) {
    if (!fence) return;
    const MGBufferDispatch* d = mg_bs_dispatch();
    if (!d->DeleteSync) return;
    // Clear any pre-existing latched error so we can attribute one to us.
    if (d->GetError) (void)d->GetError();
    d->DeleteSync(fence);
    // The GE8320 driver can answer GL_INVALID_OPERATION if the sync was already
    // completed/retired. Consume it so it never surfaces on an unrelated call.
    if (d->GetError) (void)d->GetError();
}
