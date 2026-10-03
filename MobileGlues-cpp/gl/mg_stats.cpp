// MobileGlues - gl/mg_stats.cpp
// SPDX-License-Identifier: LGPL-2.1-only
#include "mg_stats.h"
#include <string.h>

namespace { MGBufferStats g_stats; }

extern "C" MGBufferStats* mg_bs_stats(void) { return &g_stats; }
extern "C" void mg_bs_stats_reset(void) { memset(&g_stats, 0, sizeof(g_stats)); }
