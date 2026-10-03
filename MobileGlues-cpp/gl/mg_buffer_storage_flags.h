// MobileGlues - gl/mg_buffer_storage_flags.h
// Buffer Storage flag translation: GL_ARB_buffer_storage -> GL_EXT_buffer_storage
// Copyright (c) 2025-2026 MobileGL-Dev
// SPDX-License-Identifier: LGPL-2.1-only
//
// RATIONALE
// ---------
// The ARB and EXT buffer-storage specs happen to share the same numeric bit
// values today, but the project rule is explicit: NEVER do `flagsEXT = flagsARB`
// by blind cast. The real PowerVR Rogue GE8320 / Imagination driver exposes the
// EXT token values below, and we translate every bit through an explicit table
// so that a future driver (or a desktop ARB value that diverges) cannot silently
// corrupt a storage/map request. Unknown bits are dropped and reported, never
// forwarded verbatim.
#ifndef MG_BUFFER_STORAGE_FLAGS_H
#define MG_BUFFER_STORAGE_FLAGS_H

#include <stdint.h>

// --- GL_ARB_buffer_storage / core GL 4.4 immutable-storage flags -------------
#ifndef MG_MAP_READ_BIT
#define MG_MAP_READ_BIT              0x0001u
#define MG_MAP_WRITE_BIT             0x0002u
#endif
// ARB immutable-storage specific bits (core GL values)
#define MG_ARB_MAP_PERSISTENT_BIT    0x0040u
#define MG_ARB_MAP_COHERENT_BIT      0x0080u
#define MG_ARB_DYNAMIC_STORAGE_BIT   0x0100u
#define MG_ARB_CLIENT_STORAGE_BIT    0x0200u

// --- map-range flags (used by glMapBufferRange) ------------------------------
#ifndef MG_MAP_INVALIDATE_RANGE_BIT
#define MG_MAP_INVALIDATE_RANGE_BIT  0x0004u
#define MG_MAP_INVALIDATE_BUFFER_BIT 0x0008u
#define MG_MAP_FLUSH_EXPLICIT_BIT    0x0010u
#define MG_MAP_UNSYNCHRONIZED_BIT    0x0020u
#endif

// --- GL_EXT_buffer_storage real token values on the GE8320 driver ------------
// (confirmed accepted by glBufferStorageEXT, all returned GL_NO_ERROR)
#define MG_EXT_MAP_PERSISTENT_BIT    0x0040u  // GL_MAP_PERSISTENT_BIT_EXT
#define MG_EXT_MAP_COHERENT_BIT      0x0080u  // GL_MAP_COHERENT_BIT_EXT
#define MG_EXT_DYNAMIC_STORAGE_BIT   0x0100u  // GL_DYNAMIC_STORAGE_BIT_EXT
#define MG_EXT_CLIENT_STORAGE_BIT    0x0200u  // GL_CLIENT_STORAGE_BIT_EXT

#ifdef __cplusplus
extern "C" {
#endif

// Explicit, bit-by-bit translation of the immutable-storage flags passed to
// glBufferStorage() (ARB semantics) into the exact flags glBufferStorageEXT()
// expects. `out_dropped` (optional) receives any bits we could not map so the
// caller can log them once. Returns the EXT flag word.
static inline uint32_t translateStorageFlagsARBtoEXT(uint32_t arb, uint32_t* out_dropped) {
    uint32_t ext = 0u;
    uint32_t known = 0u;

    if (arb & MG_MAP_READ_BIT)            { ext |= MG_MAP_READ_BIT;            known |= MG_MAP_READ_BIT; }
    if (arb & MG_MAP_WRITE_BIT)           { ext |= MG_MAP_WRITE_BIT;           known |= MG_MAP_WRITE_BIT; }
    if (arb & MG_ARB_MAP_PERSISTENT_BIT)  { ext |= MG_EXT_MAP_PERSISTENT_BIT;  known |= MG_ARB_MAP_PERSISTENT_BIT; }
    if (arb & MG_ARB_MAP_COHERENT_BIT)    { ext |= MG_EXT_MAP_COHERENT_BIT;    known |= MG_ARB_MAP_COHERENT_BIT; }
    if (arb & MG_ARB_DYNAMIC_STORAGE_BIT) { ext |= MG_EXT_DYNAMIC_STORAGE_BIT; known |= MG_ARB_DYNAMIC_STORAGE_BIT; }
    if (arb & MG_ARB_CLIENT_STORAGE_BIT)  { ext |= MG_EXT_CLIENT_STORAGE_BIT;  known |= MG_ARB_CLIENT_STORAGE_BIT; }

    // EXT_buffer_storage requires PERSISTENT whenever COHERENT is set.
    if ((ext & MG_EXT_MAP_COHERENT_BIT) && !(ext & MG_EXT_MAP_PERSISTENT_BIT))
        ext |= MG_EXT_MAP_PERSISTENT_BIT;

    if (out_dropped) *out_dropped = arb & ~known;
    return ext;
}

// Translate the flags Sodium passes to glMapBufferRange into the flags the
// backend driver accepts. The map-range bits are core-GLES 3.0 and share values,
// but PERSISTENT/COHERENT ride the EXT values, so we route them explicitly too.
static inline uint32_t translateMapFlagsARBtoBackend(uint32_t arb, uint32_t* out_dropped) {
    uint32_t be = 0u;
    uint32_t known = 0u;

    if (arb & MG_MAP_READ_BIT)             { be |= MG_MAP_READ_BIT;             known |= MG_MAP_READ_BIT; }
    if (arb & MG_MAP_WRITE_BIT)            { be |= MG_MAP_WRITE_BIT;            known |= MG_MAP_WRITE_BIT; }
    if (arb & MG_MAP_INVALIDATE_RANGE_BIT) { be |= MG_MAP_INVALIDATE_RANGE_BIT; known |= MG_MAP_INVALIDATE_RANGE_BIT; }
    if (arb & MG_MAP_INVALIDATE_BUFFER_BIT){ be |= MG_MAP_INVALIDATE_BUFFER_BIT;known |= MG_MAP_INVALIDATE_BUFFER_BIT; }
    if (arb & MG_MAP_FLUSH_EXPLICIT_BIT)   { be |= MG_MAP_FLUSH_EXPLICIT_BIT;   known |= MG_MAP_FLUSH_EXPLICIT_BIT; }
    if (arb & MG_MAP_UNSYNCHRONIZED_BIT)   { be |= MG_MAP_UNSYNCHRONIZED_BIT;   known |= MG_MAP_UNSYNCHRONIZED_BIT; }
    if (arb & MG_ARB_MAP_PERSISTENT_BIT)   { be |= MG_EXT_MAP_PERSISTENT_BIT;   known |= MG_ARB_MAP_PERSISTENT_BIT; }
    if (arb & MG_ARB_MAP_COHERENT_BIT)     { be |= MG_EXT_MAP_COHERENT_BIT;     known |= MG_ARB_MAP_COHERENT_BIT; }

    if (out_dropped) *out_dropped = arb & ~known;
    return be;
}

#ifdef __cplusplus
}
#endif

#endif // MG_BUFFER_STORAGE_FLAGS_H
