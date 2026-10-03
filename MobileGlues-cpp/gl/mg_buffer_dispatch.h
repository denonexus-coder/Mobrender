// MobileGlues - gl/mg_buffer_dispatch.h
// Dispatch cache for the Buffer Storage backend (zero eglGetProcAddress on hot path)
// Copyright (c) 2025-2026 MobileGL-Dev
// SPDX-License-Identifier: LGPL-2.1-only
//
// The entry points are resolved ONCE at init and cached here. The hot path never
// calls eglGetProcAddress again. Portable scalar aliases are used instead of the
// GL headers so this module compiles standalone AND drops straight into the
// MobileGlues tree: on ARM64/LP64 GLenum/GLuint/GLbitfield == unsigned int,
// GLsizeiptr/GLintptr == ptrdiff_t, GLsync == void*, GLuint64 == uint64_t, so
// the host's real GL function pointers are ABI-compatible with these typedefs.
#ifndef MG_BUFFER_DISPATCH_H
#define MG_BUFFER_DISPATCH_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned int mg_enum;
typedef unsigned int mg_uint;
typedef unsigned int mg_bitfield;
typedef int          mg_sizei;
typedef ptrdiff_t    mg_sizeiptr;
typedef ptrdiff_t    mg_intptr;
typedef void*        mg_sync;
typedef uint64_t     mg_uint64;
typedef unsigned char mg_boolean;

// --- sync tokens (core GLES 3.0, shared values with desktop) -----------------
#define MG_SYNC_GPU_COMMANDS_COMPLETE   0x9117u
#define MG_SYNC_FLUSH_COMMANDS_BIT      0x00000001u
#define MG_ALREADY_SIGNALED             0x911Au
#define MG_TIMEOUT_EXPIRED              0x911Bu
#define MG_CONDITION_SATISFIED          0x911Cu
#define MG_WAIT_FAILED                  0x911Du
#define MG_NO_ERROR                     0u
#define MG_INVALID_OPERATION            0x0502u

// Buffer bind targets we use internally for copies / binds.
#define MG_ARRAY_BUFFER                 0x8892u
#define MG_COPY_READ_BUFFER             0x8F36u
#define MG_COPY_WRITE_BUFFER            0x8F37u

typedef void      (*MGPFN_BufferStorageEXT)(mg_enum, mg_sizeiptr, const void*, mg_bitfield);
typedef void*     (*MGPFN_MapBufferRange)(mg_enum, mg_intptr, mg_sizeiptr, mg_bitfield);
typedef void      (*MGPFN_FlushMappedBufferRange)(mg_enum, mg_intptr, mg_sizeiptr);
typedef mg_boolean(*MGPFN_UnmapBuffer)(mg_enum);
typedef void      (*MGPFN_BindBuffer)(mg_enum, mg_uint);
typedef void      (*MGPFN_GenBuffers)(mg_sizei, mg_uint*);
typedef void      (*MGPFN_DeleteBuffers)(mg_sizei, const mg_uint*);
typedef void      (*MGPFN_CopyBufferSubData)(mg_enum, mg_enum, mg_intptr, mg_intptr, mg_sizeiptr);
typedef void      (*MGPFN_BufferSubData)(mg_enum, mg_intptr, mg_sizeiptr, const void*);
typedef mg_sync   (*MGPFN_FenceSync)(mg_enum, mg_bitfield);
typedef mg_enum   (*MGPFN_ClientWaitSync)(mg_sync, mg_bitfield, mg_uint64);
typedef void      (*MGPFN_DeleteSync)(mg_sync);
typedef mg_enum   (*MGPFN_GetError)(void);
typedef void      (*MGPFN_Finish)(void);

// Cached dispatch table. Any pointer may be null if the host lacks it; callers
// must check (helpers in mg_buffer_storage.cpp do).
typedef struct MGBufferDispatch {
    MGPFN_BufferStorageEXT        BufferStorageEXT;
    MGPFN_MapBufferRange          MapBufferRange;
    MGPFN_FlushMappedBufferRange  FlushMappedBufferRange;
    MGPFN_UnmapBuffer             UnmapBuffer;
    MGPFN_BindBuffer              BindBuffer;
    MGPFN_GenBuffers              GenBuffers;
    MGPFN_DeleteBuffers           DeleteBuffers;
    MGPFN_CopyBufferSubData       CopyBufferSubData;
    MGPFN_BufferSubData           BufferSubData;      // shadow fallback promotion
    MGPFN_FenceSync               FenceSync;
    MGPFN_ClientWaitSync          ClientWaitSync;
    MGPFN_DeleteSync              DeleteSync;
    MGPFN_GetError                GetError;
    MGPFN_Finish                  Finish;
} MGBufferDispatch;

// Install a fully-populated dispatch table (the MobileGlues integration path:
// fill from the global GLES struct and pass &table). Copies by value.
void mg_bs_set_dispatch(const MGBufferDispatch* table);

// Alternative init via an eglGetProcAddress-style resolver (standalone / tools).
// Returns 1 if the mandatory entry points (BufferStorageEXT, MapBufferRange,
// FenceSync, ClientWaitSync, DeleteSync, CopyBufferSubData) all resolved.
typedef void* (*MGPFN_Resolver)(const char* name);
int  mg_bs_init_dispatch(MGPFN_Resolver resolve);

// Access the cached table (never null after init; zero-filled before).
const MGBufferDispatch* mg_bs_dispatch(void);

// True when every mandatory entry point is present.
int  mg_bs_dispatch_ready(void);

#ifdef __cplusplus
}
#endif

#endif // MG_BUFFER_DISPATCH_H
