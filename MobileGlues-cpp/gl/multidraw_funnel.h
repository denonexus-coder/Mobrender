// MobileGlues - gl/multidraw_funnel.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1:
//   https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
// SPDX-License-Identifier: LGPL-2.1-only

#pragma once

#include "multidraw.h"

#include <atomic>
#include <cstdint>

// Funnel draw engine with Sodium-shape adaptation.
//
// Detection is by batch geometry, not by mod name: the merge rule itself proves,
// per call, that the batch is a contiguous shared-index quad pattern (what
// Sodium emits). The state machine below latches that proof into a cheap
// routing decision so non-Sodium workloads never pay the scan cost.
extern std::atomic<uint64_t> g_mg_funnel_calls;
extern std::atomic<uint64_t> g_mg_funnel_merges;

enum class mg_funnel_state_t : uint8_t {
    Off = 0,      // MG_FUNNEL_OFF=1: hard off until process restart
    Probing = 1,  // scanning every qualifying batch for the Sodium shape
    Active = 2,   // shape confirmed recently; funnel is the primary path
    Inactive = 3, // shape not seen in the probe window; re-probe periodically
};

mg_funnel_state_t mg_funnel_current_state();

// Returns true when the batch was served by the funnel. False means "not my
// shape": the caller proceeds through every pre-existing backend unchanged.
bool mg_multidraw_funnel_elements_bv(GLenum mode, GLsizei* counts, GLenum type,
                                      const void* const* indices, GLsizei primcount,
                                      const GLint* basevertex);
