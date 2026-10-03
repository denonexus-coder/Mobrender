// MobileGlues - gl/mg_buffer_storage.h
// Public surface of the Buffer Storage translation layer. buffer.cpp calls into
// this; everything below (flags, dispatch, ring, queue, arena, fences) is wired
// together here. ARB frontend in, EXT backend out, persistent-mapped and fenced.
// SPDX-License-Identifier: LGPL-2.1-only
#ifndef MG_BUFFER_STORAGE_H
#define MG_BUFFER_STORAGE_H

#include "mg_buffer_dispatch.h"
#include <stdint.h>
#include <stddef.h>

// Per-buffer native state, keyed by the application-facing GL buffer name.
struct MGBufferStorage {
    mg_uint  buffer;            // application-facing name (what Sodium sees)
    mg_uint  driver_buffer;     // real driver name (already bound by caller)
    size_t   size;

    uint32_t storageFlagsARB;
    uint32_t storageFlagsEXT;
    uint32_t mapFlags;

    uint8_t* mappedPtr;         // valid for the whole life of the mapping
    size_t   mappedOffset;
    size_t   mappedLength;

    uint8_t* shadowPtr;         // CPU backing when native persistent map fails / fallback
    size_t   shadowSize;

    bool     immutable;
    bool     persistent;
    bool     coherent;
    bool     clientStorage;
    bool     mapped;
    bool     fallbackMutable;   // true: buffer from glBufferData path (no EXT)

    uint64_t generation;
};

// Internal flush policy. Default is EXPLICIT_FLUSH for maximum predictability.
enum MGBufferStorageMode {
    MG_MODE_EXPLICIT_FLUSH = 0,
    MG_MODE_COHERENT,
    MG_MODE_AUTO
};

// Benchmark staging. BASELINE forwards nothing special; higher tiers enable the
// ring / persistent-arena / adaptive paths so each can be measured in isolation.
enum MGBenchMode {
    MG_BENCH_BASELINE = 0,
    MG_BENCH_EXT_BACKEND,
    MG_BENCH_P1_RING,
    MG_BENCH_P2_PERSISTENT_ARENA,
    MG_BENCH_P3_ADAPTIVE
};

#ifdef __cplusplus
extern "C" {
#endif

// One-time init. `resolve` is an eglGetProcAddress-style resolver; pass null if
// the dispatch table was already installed via mg_bs_set_dispatch(). Reads env
// toggles (MG_BUFFER_STORAGE_DEBUG, MG_BS_MODE, MG_BS_BENCH, MG_BS_CLIENT_STORAGE).
void mg_bs_init(MGPFN_Resolver resolve);
bool mg_bs_available(void);     // true when the EXT backend can be used

void mg_bs_set_mode(enum MGBufferStorageMode m);
enum MGBufferStorageMode mg_bs_get_mode(void);
void mg_bs_set_bench(enum MGBenchMode m);
enum MGBenchMode mg_bs_get_bench(void);

// --- entry points the GL frontend (gl/buffer.cpp) forwards to ---------------
// The caller has already bound `driver_buffer` to `target`. Returns true when
// the call was serviced natively; false means "fall back to the old path".
bool mg_bs_buffer_storage(mg_enum target, mg_uint app_name, mg_uint driver_buffer,
                          mg_sizeiptr size, uint32_t arb_flags, const void* data);

// Map. If the buffer is persistent and already mapped, returns the STORED
// pointer (offset-adjusted) WITHOUT remapping -- the GE8320 driver returns NULL
// on a second map of an already-mapped persistent buffer. *handled tells the
// caller whether this layer owns the call.
void* mg_bs_map_buffer_range(mg_enum target, mg_uint app_name, mg_intptr offset,
                             mg_sizeiptr length, uint32_t arb_access, bool* handled);

// Record/queue an explicit flush. Coalesces per buffer. Returns true if owned.
bool mg_bs_flush_mapped_range(mg_enum target, mg_uint app_name, mg_intptr offset, mg_sizeiptr length);

// Unmap. Returns true if this layer handled it (persistent buffers stay mapped
// and report success without actually unmapping).
bool mg_bs_unmap(mg_enum target, mg_uint app_name, bool* out_result);

// Buffer deletion cleanup: fences, mapping, metadata. Call before the real
// glDeleteBuffers so the driver name is still valid.
void mg_bs_delete(mg_uint app_name);

// Lookup (null if not tracked).
MGBufferStorage* mg_bs_lookup(mg_uint app_name);

// Registra um buffer que caiu no fallback glBufferData (sem GL_EXT_buffer_storage).
// O shadow path serve o map persistente com CPU backing e promoção por glBufferSubData.
void mg_bs_register_fallback(mg_uint app_name, mg_sizeiptr size);

#ifdef __cplusplus
}
#endif

#endif // MG_BUFFER_STORAGE_H
