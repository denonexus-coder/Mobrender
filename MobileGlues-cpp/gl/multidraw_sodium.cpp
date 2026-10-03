// MobileGlues - gl/multidraw_sodium.cpp
// Direct Sodium terrain path: glMultiDrawElementsBaseVertex in one driver call.
// Copyright (c) 2025-2026 MobileGL-Dev
// SPDX-License-Identifier: LGPL-2.1-only

#include "multidraw.h"
#include "../gles/loader.h"
#include "log.h"
#include <cstdint>
#include <dlfcn.h>

#define DEBUG 0

// ---------------------------------------------------------------------------
// Fixed thresholds. No runtime benchmarking: these are compile-time constants
// calibrated per device family. On Rogue (GE8320) every driver entry point
// carries non-trivial front-end validation, so:
//   primcount <= 3 : the EXT call's own validation cost exceeds a scalar loop
//   primcount >= 4 : one EXT call beats N scalar calls, always
// ---------------------------------------------------------------------------
#ifndef MG_MD_UNROLL_MAX
#define MG_MD_UNROLL_MAX 3
#endif
#ifndef MG_MD_EXT_MIN
#define MG_MD_EXT_MIN 4
#endif
#ifndef MG_MD_SHARED_SCAN_MAX
#define MG_MD_SHARED_SCAN_MAX 4096
#endif

// gles is the real driver library handle (gles/loader.cpp). Resolving through
// eglGetProcAddress here would return MobileGlues' own exported alias and
// recurse; the same trap gl/multidraw.cpp documents for glMultiDrawArraysEXT.
extern "C" void* gles;

typedef void (GLAPIENTRY* mg_pfn_mdebv_ext)(
    GLenum, const GLsizei*, GLenum, const void* const*, GLsizei, const GLint*);

static mg_pfn_mdebv_ext mg_sodium_mdebv_fn() {
    static mg_pfn_mdebv_ext fn = nullptr;
    static bool resolved = false;
    if (!resolved) {
        resolved = true;
        const bool real_handle =
            gles != nullptr && gles != reinterpret_cast<void*>(~(uintptr_t)0);
        if (real_handle) {
            fn = reinterpret_cast<mg_pfn_mdebv_ext>(
                dlsym(gles, "glMultiDrawElementsBaseVertexEXT"));
            if (!fn) {
                fn = reinterpret_cast<mg_pfn_mdebv_ext>(
                    dlsym(gles, "glMultiDrawElementsBaseVertexOES"));
            }
        }
        LOG_D("sodium_fast: glMultiDrawElementsBaseVertexEXT=%p", (void*)fn);
    }
    return fn;
}

// Latch set by multidraw.cpp's probe: when the first real EXT call fails on
// the host (stub driver), the dispatcher falls back to basevertex and this
// path must stop trying. Exported so multidraw.cpp can clear it from
// multidraw_check_context().
static bool mg_sodium_ext_broken = false;

extern "C" void mg_multidraw_sodium_invalidate() {
    mg_sodium_ext_broken = false;
}

// ---------------------------------------------------------------------------
// TIER S — Shared-IBO detector.
//
// Sodium's opaque terrain batches (RenderRegion -> MultiDrawBatch) use the
// shared quad index buffer: every indices[i] is the same byte offset into one
// IBO, while count[i] and basevertex[i] vary per section. Detect that shape
// with one comparison per sub-draw (abort on first divergence) and hand the
// whole batch to the host in a single glMultiDrawElementsBaseVertexEXT call.
//
// Nothing is copied, no index is rebased, no basevertex is recomputed, no
// buffer is looked up: mode/type/VAO/VBO/IBO/program/uniforms are already
// bound and validated by the time this runs.
// ---------------------------------------------------------------------------
static bool mg_sodium_tierS(GLenum mode, const GLsizei* counts, GLenum type,
                            const void* const* indices, GLsizei primcount,
                            const GLint* basevertex) {
    if (mg_sodium_ext_broken) return false;
    if (primcount < MG_MD_EXT_MIN || primcount > MG_MD_SHARED_SCAN_MAX) return false;
    if (!basevertex || !counts || !indices) return false;

    const void* const first = indices[0];
    for (GLsizei i = 1; i < primcount; ++i) {
        if (indices[i] != first) return false; // diverged: not the Sodium shape
    }

    mg_pfn_mdebv_ext fn = mg_sodium_mdebv_fn();
    if (!fn) return false;

    // Drain any stale error so the probe below reads only this call's result.
    for (int i = 0; i < 16 && GLES.glGetError() != GL_NO_ERROR; ++i) {
    }

    fn(mode, counts, type, indices, primcount, basevertex);

    // Probe the first call only. A stub driver accepts the call, draws nothing
    // and raises no error (see multidraw.cpp); on a real Rogue driver this
    // reads GL_NO_ERROR and the latch never trips. On any error, latch off and
    // let the caller fall through to the normal backend chain.
    if (GLES.glGetError() != GL_NO_ERROR) {
        mg_sodium_ext_broken = true;
        LOG_D("sodium_fast: EXT call errored on probe, latching off");
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// TIER U — micro-batch unroll.
//
// primcount <= MG_MD_UNROLL_MAX: scalar glDrawElementsBaseVertex calls are
// cheaper than one EXT dispatch, because the host front-end validates the
// EXT call's arrays and then loops internally anyway.
// ---------------------------------------------------------------------------
static bool mg_sodium_tierU(GLenum mode, const GLsizei* counts, GLenum type,
                            const void* const* indices, GLsizei primcount,
                            const GLint* basevertex) {
    if (primcount > MG_MD_UNROLL_MAX || primcount < 1) return false;
    if (!counts || !indices) return false;

    if (basevertex) {
        for (GLsizei i = 0; i < primcount; ++i) {
            if (counts[i] > 0) {
                GLES.glDrawElementsBaseVertex(mode, counts[i], type, indices[i], basevertex[i]);
            }
        }
    } else {
        for (GLsizei i = 0; i < primcount; ++i) {
            if (counts[i] > 0) {
                GLES.glDrawElements(mode, counts[i], type, indices[i]);
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Public entry. Called by the glMultiDrawElementsBaseVertex dispatcher before
// the generic fast path, because the Sodium shape is the common case in this
// workload and the generic frustum/contiguous tiers cannot serve it (they need
// uniform basevertex / bboxes, and Sodium provides neither).
//
// Returns true when the draw was emitted (including legal no-ops).
// ---------------------------------------------------------------------------
extern "C" bool mg_multidraw_sodium_bv(GLenum mode, const GLsizei* counts, GLenum type,
                                       const void* const* indices, GLsizei primcount,
                                       const GLint* basevertex) {
    if (primcount < 1) return true;  // legal no-op, consumed here
    if (!counts || !indices) return false; // let the dispatcher's gate warn

    if (mg_sodium_tierS(mode, counts, type, indices, primcount, basevertex)) {
        return true;
    }
    if (mg_sodium_ext_broken) return false; // hand the whole call to the chain

    return mg_sodium_tierU(mode, counts, type, indices, primcount, basevertex);
}
