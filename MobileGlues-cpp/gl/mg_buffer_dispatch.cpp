// MobileGlues - gl/mg_buffer_dispatch.cpp
// SPDX-License-Identifier: LGPL-2.1-only
#include "mg_buffer_dispatch.h"
#include <string.h>

namespace {
MGBufferDispatch g_disp;      // zero-initialised (all null) at load time
int g_ready = 0;

int compute_ready(const MGBufferDispatch& d) {
    return d.BufferStorageEXT && d.MapBufferRange && d.FenceSync &&
           d.ClientWaitSync && d.DeleteSync && d.CopyBufferSubData ? 1 : 0;
}
} // namespace

extern "C" void mg_bs_set_dispatch(const MGBufferDispatch* table) {
    if (!table) { memset(&g_disp, 0, sizeof(g_disp)); g_ready = 0; return; }
    g_disp = *table;
    g_ready = compute_ready(g_disp);
}

extern "C" int mg_bs_init_dispatch(MGPFN_Resolver resolve) {
    if (!resolve) return 0;
    MGBufferDispatch d;
    memset(&d, 0, sizeof(d));
    d.BufferStorageEXT       = (MGPFN_BufferStorageEXT)      resolve("glBufferStorageEXT");
    d.MapBufferRange         = (MGPFN_MapBufferRange)        resolve("glMapBufferRange");
    d.FlushMappedBufferRange = (MGPFN_FlushMappedBufferRange)resolve("glFlushMappedBufferRange");
    d.UnmapBuffer            = (MGPFN_UnmapBuffer)           resolve("glUnmapBuffer");
    d.BindBuffer             = (MGPFN_BindBuffer)            resolve("glBindBuffer");
    d.GenBuffers             = (MGPFN_GenBuffers)            resolve("glGenBuffers");
    d.DeleteBuffers          = (MGPFN_DeleteBuffers)         resolve("glDeleteBuffers");
    d.CopyBufferSubData      = (MGPFN_CopyBufferSubData)     resolve("glCopyBufferSubData");
    d.BufferSubData          = (MGPFN_BufferSubData)          resolve("glBufferSubData");
    d.FenceSync              = (MGPFN_FenceSync)             resolve("glFenceSync");
    d.ClientWaitSync         = (MGPFN_ClientWaitSync)        resolve("glClientWaitSync");
    d.DeleteSync             = (MGPFN_DeleteSync)            resolve("glDeleteSync");
    d.GetError               = (MGPFN_GetError)              resolve("glGetError");
    d.Finish                 = (MGPFN_Finish)                resolve("glFinish");
    g_disp = d;
    g_ready = compute_ready(g_disp);
    return g_ready;
}

extern "C" const MGBufferDispatch* mg_bs_dispatch(void) { return &g_disp; }
extern "C" int mg_bs_dispatch_ready(void) { return g_ready; }
