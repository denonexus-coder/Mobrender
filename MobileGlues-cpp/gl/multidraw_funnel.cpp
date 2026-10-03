// MobileGlues - gl/multidraw_funnel.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1:
//   https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
// SPDX-License-Identifier: LGPL-2.1-only

#include "multidraw_funnel.h"
#include "multidraw_fast.hpp"
#include "multidraw_sodium_decl.hpp"
#include "buffer.h"
#include "enable.h"
#include "restart.h"
#include "../config/settings.h"
#include "../egl/context.h"

#include <cstdlib>
#include <cstring>
#include <vector>

#define DEBUG 0

std::atomic<uint64_t> g_mg_funnel_calls{0};
std::atomic<uint64_t> g_mg_funnel_merges{0};

void prepareForDraw();

namespace {

struct mg_funnel_run_t {
    GLsizei count;
    const void* off;
    GLint bv;
};

// ── Adaptive detection state ───────────────────────────────────────────
//
// PROBING: every qualifying batch is scanned. One merge inside 64 calls
//   promotes to ACTIVE -- the shape only Sodium emits has been observed.
//   64 calls with zero merges demote to INACTIVE.
// INACTIVE: the scan is skipped entirely (one compare-and-return); every
//   1024 calls one batch is scanned again, so switching renderers or mods
//   mid-session is picked back up.
// ACTIVE: if a 512-call window produces no merges at all, the shape went
//   away; back to PROBING rather than burning the scan on every call.
//
// Per-context: like every probe latch in multidraw.cpp, the shape is a fact
// about what is currently being drawn, and a context switch can change it.
constexpr uint32_t MG_FUNNEL_PROBE_WINDOW     = 64;
constexpr uint32_t MG_FUNNEL_REPROBE_EVERY     = 1024;
constexpr uint32_t MG_FUNNEL_ACTIVE_WINDOW     = 512;

struct mg_funnel_ctx_t {
    mg_funnel_state_t state = mg_funnel_state_t::Probing;
    uint32_t probe_seen  = 0; // calls scanned since entering PROBING
    uint32_t probe_hits  = 0;
    uint32_t idle_seen   = 0; // ACTIVE calls since the last merge
    uint64_t since_reprobe = 0; // INACTIVE calls since the last re-probe
};
thread_local mg_funnel_ctx_t g_funnel; // one current context per thread

// Tracked like every other scratch in this module. It is not a GL object
// name, so -- unlike those -- it survives a context change; only the shape
// detection restarts.
bool mg_funnel_env_off() {
    static bool resolved = false;
    static bool off = false;
    if (!resolved) {
        resolved = true;
        const char* e = getenv("MG_FUNNEL_OFF");
        off = (e != nullptr && e[0] == '1');
        if (off) LOG_W_FORCE("multidraw funnel: disabled by MG_FUNNEL_OFF")
    }
    return off;
}

GLsizei mg_funnel_index_size(GLenum type) {
    switch (type) {
    case GL_UNSIGNED_INT:
        return 4;
    case GL_UNSIGNED_SHORT:
        return 2;
    case GL_UNSIGNED_BYTE:
        return 1;
    default:
        return 0;
    }
}

// Quick reject before any state bookkeeping: the entry shape every backend of
// the funnel needs. Checked per call so a non-qualifying batch never touches
// the detection counters, whatever state we are in.
bool mg_funnel_shape_qualifies(GLenum mode, GLsizei primcount, GLenum type,
                               const GLsizei* counts, const void* const* indices,
                               const GLint* basevertex) {
    if (mode != GL_TRIANGLES) return false;
    if (primcount < 2) return false;
    if (mg_funnel_index_size(type) == 0) return false;
    if (!counts || !indices || !basevertex) return false;
    if (!GLES.glDrawElementsBaseVertex) return false;
    // Per-index semantics this engine does not rewrite belong to the
    // rewriting backend in multidraw.cpp.
    if (mg_primitive_restart_enabled()) return false;
    return true;
}

// The scan itself. Returns merges found (0 = not the Sodium shape).
GLsizei mg_funnel_scan(GLsizei* counts, GLenum type, const void* const* indices,
                       GLsizei primcount, const GLint* basevertex,
                       std::vector<mg_funnel_run_t>& runs) {
    const GLsizei s = mg_funnel_index_size(type);
    runs.clear();

    // The fused run reads more pattern than any single sub-draw, so it is
    // bounded by the real element-array-buffer size, queried lazily once per
    // call and only once a merge candidate exists.
    GLint64 pattern_indices = -1;

    GLsizei merges = 0;
    for (GLsizei i = 0; i < primcount; ++i) {
        const GLsizei c = counts[i];
        if (c <= 0) continue;
        const GLint bv = basevertex[i];
        const void* off = indices[i];

        bool merged = false;
        if (!runs.empty()) {
            mg_funnel_run_t& r = runs.back();
            // Merge rule, proven geometrically: same shared index pattern from
            // the same offset, and the next section's vertices sit exactly
            // count * 4/6 further on (6 indices per 4 vertices of the quad
            // pattern). Under these conditions the fused draw issues, for the
            // absorbed half, pattern entries [count .. count+c] with base bv --
            // the same vertices the sub-draw would have drawn itself.
            if (off == r.off && (r.count % 6) == 0 && (c % 6) == 0) {
                const int64_t dbv = static_cast<int64_t>(bv) - static_cast<int64_t>(r.bv);
                if (dbv > 0 && dbv * 3 == static_cast<int64_t>(r.count) * 2) {
                    if (pattern_indices < 0) {
                        pattern_indices = 0;
                        const GLuint ibo = mg_driver_bound_buffer(GL_ELEMENT_ARRAY_BUFFER);
                        if (ibo != 0) {
                            GLint sz = 0;
                            GLES.glGetBufferParameteriv(GL_ELEMENT_ARRAY_BUFFER, GL_BUFFER_SIZE, &sz);
                            if (sz > 0) pattern_indices = static_cast<GLint64>(sz) / s;
                        }
                    }
                    if (pattern_indices > 0 &&
                        static_cast<GLint64>(r.count) + static_cast<GLint64>(c) <= pattern_indices) {
                        r.count += c;
                        ++merges;
                        merged = true;
                    }
                }
            }
        }
        if (!merged) runs.push_back(mg_funnel_run_t{c, off, bv});
    }
    return merges;
}

} // namespace

mg_funnel_state_t mg_funnel_current_state() {
    return g_funnel.state;
}

bool mg_multidraw_funnel_elements_bv(GLenum mode, GLsizei* counts, GLenum type,
                                     const void* const* indices, GLsizei primcount,
                                     const GLint* basevertex) {
    if (mg_funnel_env_off()) {
        g_funnel.state = mg_funnel_state_t::Off;
        return false;
    }
    if (!mg_funnel_shape_qualifies(mode, primcount, type, counts, indices, basevertex))
        return false;

    static thread_local std::vector<mg_funnel_run_t> runs;

    GLsizei merges_this_batch = 0;
    switch (g_funnel.state) {
    case mg_funnel_state_t::Off:
        return false;

    case mg_funnel_state_t::Probing: {
        const GLsizei merges = mg_funnel_scan(counts, type, indices, primcount, basevertex, runs);
        ++g_funnel.probe_seen;
        if (merges > 0) {
            merges_this_batch = merges;
            if (++g_funnel.probe_hits >= 1) {
                g_funnel.state = mg_funnel_state_t::Active;
                g_funnel.idle_seen = 0;
                LOG_D("multidraw funnel: sodium shape confirmed, funnel ACTIVE")
            }
            break; // serve this batch below
        }
        if (g_funnel.probe_seen >= MG_FUNNEL_PROBE_WINDOW) {
            g_funnel.state = mg_funnel_state_t::Inactive;
            g_funnel.since_reprobe = 0;
            LOG_D("multidraw funnel: no sodium shape in %u calls, INACTIVE", MG_FUNNEL_PROBE_WINDOW)
            return false;
        }
        return false; // scanned, nothing mergeable yet: hand the batch back
    }

    case mg_funnel_state_t::Active: {
        const GLsizei merges = mg_funnel_scan(counts, type, indices, primcount, basevertex, runs);
        if (merges > 0) {
            merges_this_batch = merges;
            g_funnel.idle_seen = 0;
        } else if (++g_funnel.idle_seen >= MG_FUNNEL_ACTIVE_WINDOW) {
            // The shape went away (render distance 2 chunks, world switch,
            // Sodium disabled mid-session): stop scanning every call, but
            // keep watching.
            g_funnel.state = mg_funnel_state_t::Probing;
            g_funnel.probe_seen = 0;
            g_funnel.probe_hits = 0;
            LOG_D("multidraw funnel: shape lost, re-probing")
            return false;
        }
        if (merges == 0) return false;
        break; // serve below
    }

    case mg_funnel_state_t::Inactive:
        // One compare-and-return most of the time; a periodic single-batch
        // re-probe so a mid-session switch to Sodium is picked up.
        if (++g_funnel.since_reprobe >= MG_FUNNEL_REPROBE_EVERY) {
            g_funnel.since_reprobe = 0;
            const GLsizei merges = mg_funnel_scan(counts, type, indices, primcount, basevertex, runs);
            if (merges > 0) {
                merges_this_batch = merges;
                g_funnel.state = mg_funnel_state_t::Active;
                g_funnel.idle_seen = 0;
                LOG_D("multidraw funnel: sodium shape found on re-probe, funnel ACTIVE")
                break; // serve below
            }
        }
        return false;
    }

    prepareForDraw();
    for (const mg_funnel_run_t& r : runs) {
        GLES.glDrawElementsBaseVertex(mode, r.count, type, r.off, r.bv);
    }

    g_mg_funnel_calls.fetch_add(1, std::memory_order_relaxed);
    g_mg_funnel_merges.fetch_add(static_cast<uint64_t>(merges_this_batch), std::memory_order_relaxed);
    CHECK_GL_ERROR
    return true;
}
