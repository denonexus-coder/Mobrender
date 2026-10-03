// MobileGlues - gl/mg_buffer_storage.cpp
// SPDX-License-Identifier: LGPL-2.1-only
//
// Layer de tradução GL_ARB_buffer_storage → GL_EXT_buffer_storage.
// Cada path activo emite um LOG_D para diagnóstico; em release os logs
// desaparecem pelo pré-processador do MobileGlues (GLOBAL_DEBUG / DEBUG).
//
#include "mg_buffer_storage.h"
#include "mg_buffer_storage_flags.h"
#include "mg_stats.h"
#include "buffer.h"
#define DEBUG 0
#include "../gl/log.h"   // LOG_D / MG_WARN_ONCE do MobileGlues
#include <stdlib.h>
#include <string.h>
#include <vector>

// TAG usado em todos os LOG_D deste módulo.
#define MG_BS_TAG "MG_BS"

namespace {

// Registry keyed directly by the application buffer name. The GL context is
// thread-confined (one GL thread); no mutex on the hot path.
struct Registry {
    std::vector<MGBufferStorage> slots;
    std::vector<char>            used;

    MGBufferStorage* get(mg_uint name) {
        if (name < used.size() && used[name]) return &slots[name];
        return nullptr;
    }
    MGBufferStorage* ensure(mg_uint name) {
        if (name >= slots.size()) {
            slots.resize(name + 1);
            used.resize(name + 1, 0);
        }
        if (!used[name]) {
            memset(&slots[name], 0, sizeof(MGBufferStorage));
            slots[name].buffer = name;
            used[name] = 1;
        }
        return &slots[name];
    }
    void erase(mg_uint name) {
        if (name < used.size()) {
            used[name] = 0;
            memset(&slots[name], 0, sizeof(MGBufferStorage));
        }
    }
};

Registry            g_reg;
MGBufferStorageMode g_mode  = MG_MODE_EXPLICIT_FLUSH;
MGBenchMode         g_bench = MG_BENCH_P1_RING;
int                 g_client_storage = 0;   // -1 off, 0 auto, 1 on
bool                g_inited = false;
uint64_t            g_generation = 1;

int env_int(const char* k, int dflt) {
    const char* v = getenv(k);
    if (!v || !*v) return dflt;
    return (int)strtol(v, nullptr, 10);
}

} // namespace

// ---------------------------------------------------------------------------
// Init / mode control
// ---------------------------------------------------------------------------

extern "C" void mg_bs_init(MGPFN_Resolver resolve) {
    if (resolve) mg_bs_init_dispatch(resolve);
    switch (env_int("MG_BS_MODE", -999)) {
        case 0: g_mode = MG_MODE_EXPLICIT_FLUSH; break;
        case 1: g_mode = MG_MODE_COHERENT;       break;
        case 2: g_mode = MG_MODE_AUTO;           break;
        default: break;
    }
    int b = env_int("MG_BS_BENCH", -999);
    if (b >= MG_BENCH_BASELINE && b <= MG_BENCH_P3_ADAPTIVE)
        g_bench = (MGBenchMode)b;
    g_client_storage = env_int("MG_BS_CLIENT_STORAGE", 0);
    g_inited = true;

    LOG_D("[%s] init: mode=%d bench=%d client_storage=%d dispatch_ready=%d",
          MG_BS_TAG, (int)g_mode, (int)g_bench, g_client_storage,
          mg_bs_dispatch_ready());
}

extern "C" bool mg_bs_available(void) {
    return g_bench != MG_BENCH_BASELINE && mg_bs_dispatch_ready();
}

extern "C" void mg_bs_set_mode(MGBufferStorageMode m) { g_mode = m; }
extern "C" MGBufferStorageMode mg_bs_get_mode(void)   { return g_mode; }
extern "C" void mg_bs_set_bench(MGBenchMode m)        { g_bench = m; }
extern "C" MGBenchMode mg_bs_get_bench(void)          { return g_bench; }
extern "C" MGBufferStorage* mg_bs_lookup(mg_uint name){ return g_reg.get(name); }

// ---------------------------------------------------------------------------
// mg_bs_register_fallback
// Chamado quando glBufferStorage recebeu !glBufferStorageEXT e usou glBufferData.
// Permite que o map persistente subsequente seja servido via shadow de CPU.
// ---------------------------------------------------------------------------
extern "C" void mg_bs_register_fallback(mg_uint app_name, mg_sizeiptr size) {
    MGBufferStorage* s = g_reg.ensure(app_name);
    // Não sobrescreve um registo nativo já criado por mg_bs_buffer_storage.
    if (s->immutable) {
        LOG_D("[%s] register_fallback buf=%u: já é imutável nativo, ignorado", MG_BS_TAG, app_name);
        return;
    }
    s->size           = (size_t)size;
    s->immutable      = false;
    s->persistent     = false;
    s->fallbackMutable = true;
    s->mapped         = false;
    s->mappedPtr      = nullptr;
    s->shadowPtr      = nullptr;
    s->shadowSize     = 0;
    s->generation     = ++g_generation;
    LOG_D("[%s] register_fallback buf=%u size=%zu (shadow CPU path activado)", MG_BS_TAG, app_name, (size_t)size);
}

// ---------------------------------------------------------------------------
// mg_bs_buffer_storage — CORREÇÃO 1: data passado directamente ao EXT
// Buffers imutáveis sem GL_DYNAMIC_STORAGE_BIT rejeitam glBufferSubData
// com GL_INVALID_OPERATION pela spec; passamos data aqui e ponto final.
// ---------------------------------------------------------------------------
extern "C" bool mg_bs_buffer_storage(mg_enum target, mg_uint app_name, mg_uint driver_buffer,
                                     mg_sizeiptr size, uint32_t arb_flags, const void* data) {
    if (!mg_bs_available()) return false;
    const MGBufferDispatch* d = mg_bs_dispatch();
    if (!d->BufferStorageEXT) return false;

    uint32_t dropped = 0;
    uint32_t ext = translateStorageFlagsARBtoEXT(arb_flags, &dropped);

    if (g_client_storage == 1)       ext |= MG_EXT_CLIENT_STORAGE_BIT;
    else if (g_client_storage == -1) ext &= ~MG_EXT_CLIENT_STORAGE_BIT;

    if (g_mode == MG_MODE_COHERENT && (ext & MG_EXT_MAP_PERSISTENT_BIT))
        ext |= MG_EXT_MAP_COHERENT_BIT;

    // CORREÇÃO 1: data vai directamente no StorageEXT.
    d->BufferStorageEXT(target, size, data, ext);
    MG_STAT_INC(bufferStorageCalls);

    LOG_D("[%s] glBufferStorageEXT buf=%u size=%zu arb_flags=0x%x ext_flags=0x%x "
          "persistent=%d coherent=%d clientStorage=%d dropped_bits=0x%x "
          "data=%s — GL_EXT_buffer_storage activo",
          MG_BS_TAG, app_name, (size_t)size, arb_flags, ext,
          (int)!!(ext & MG_EXT_MAP_PERSISTENT_BIT),
          (int)!!(ext & MG_EXT_MAP_COHERENT_BIT),
          (int)!!(ext & MG_EXT_CLIENT_STORAGE_BIT),
          dropped,
          data ? "presente" : "null");

    MGBufferStorage* s = g_reg.ensure(app_name);
    s->driver_buffer   = driver_buffer;
    s->size            = (size_t)size;
    s->storageFlagsARB = arb_flags;
    s->storageFlagsEXT = ext;
    s->immutable       = true;
    s->persistent      = (ext & MG_EXT_MAP_PERSISTENT_BIT) != 0;
    s->coherent        = (ext & MG_EXT_MAP_COHERENT_BIT) != 0;
    s->clientStorage   = (ext & MG_EXT_CLIENT_STORAGE_BIT) != 0;
    s->mapped          = false;
    s->mappedPtr       = nullptr;
    s->shadowPtr       = nullptr;
    s->shadowSize      = 0;
    s->fallbackMutable = false;
    s->generation      = ++g_generation;
    return true;
}

// ---------------------------------------------------------------------------
// mg_bs_map_buffer_range — CORREÇÃO 3: shadow fallback
// ---------------------------------------------------------------------------
extern "C" void* mg_bs_map_buffer_range(mg_enum target, mg_uint app_name, mg_intptr offset,
                                        mg_sizeiptr length, uint32_t arb_access, bool* handled) {
    if (handled) *handled = false;
    MGBufferStorage* s = g_reg.get(app_name);
    if (!s) return nullptr;
    const MGBufferDispatch* d = mg_bs_dispatch();
    if (!d->MapBufferRange) return nullptr;

    // ── Path A: já mapeado persistentemente (nativo ou shadow) ──────────────
    // Nunca remapear: o GE8320 devolve NULL no 2.º map de buffer persistente.
    if (s->persistent && !s->shadowPtr && s->mapped && s->mappedPtr) {
        if (handled) *handled = true;
        MG_STAT_INC(mappedPointerReuse);
        LOG_D("[%s] MapBufferRange buf=%u offset=%zd: reuso do ponteiro persistente nativo ptr=%p",
              MG_BS_TAG, app_name, (ssize_t)offset, (void*)(s->mappedPtr + (size_t)offset));
        return s->mappedPtr + (size_t)offset - s->mappedOffset;
    }

    // ── Path B: shadow já mapeado, também nunca remapear ────────────────────
    if (s->persistent && s->shadowPtr && s->mapped) {
        if (handled) *handled = true;
        MG_STAT_INC(mappedPointerReuse);
        LOG_D("[%s] MapBufferRange buf=%u offset=%zd: reuso do shadow CPU ptr=%p",
              MG_BS_TAG, app_name, (ssize_t)offset, (void*)(s->shadowPtr + (size_t)offset));
        return s->shadowPtr + (size_t)offset;
    }

    uint32_t dropped = 0;
    uint32_t be = translateMapFlagsARBtoBackend(arb_access, &dropped);
    if (s->coherent) be &= ~MG_MAP_FLUSH_EXPLICIT_BIT;

    // ── Path C: map nativo (buffer EXT real) ────────────────────────────────
    if (!s->fallbackMutable) {
        void* p = d->MapBufferRange(target, offset, length, be);
        MG_STAT_INC(mapCalls);
        if (p) {
            if (handled) *handled = true;
            s->mapped       = true;
            s->mappedPtr    = (uint8_t*)p - (size_t)offset;
            s->mappedOffset = 0;
            s->mappedLength = (size_t)length + (size_t)offset;
            s->mapFlags     = be;
            LOG_D("[%s] MapBufferRange buf=%u offset=%zd len=%zd flags=0x%x: map nativo OK ptr=%p",
                  MG_BS_TAG, app_name, (ssize_t)offset, (ssize_t)length, be, p);
            return p;
        }
        LOG_D("[%s] MapBufferRange buf=%u: map nativo devolveu NULL — activando shadow CPU",
              MG_BS_TAG, app_name);
    }

    // ── Path D (CORREÇÃO 3): shadow CPU ─────────────────────────────────────
    // Activa quando: (a) map nativo falhou num buffer EXT, ou
    //               (b) buffer veio do fallback glBufferData (fallbackMutable).
    // Só se o app pediu PERSISTENT ou o buffer é do fallback.
    if ((arb_access & MG_ARB_MAP_PERSISTENT_BIT) || s->fallbackMutable) {
        size_t need = (size_t)length + (size_t)offset;
        if (need == 0) return nullptr;
        if (!s->shadowPtr || s->shadowSize < need) {
            free(s->shadowPtr);
            s->shadowSize = need;
            s->shadowPtr  = (uint8_t*)malloc(s->shadowSize);
        }
        if (s->shadowPtr) {
            if (handled) *handled = true;
            s->mapped       = true;
            s->persistent   = true;   // Sodium nunca remapeia; contrato mantido
            s->mappedPtr    = s->shadowPtr;
            s->mappedOffset = 0;
            s->mappedLength = s->shadowSize;
            s->mapFlags     = be;
            MG_STAT_INC(mapCalls);
            LOG_D("[%s] MapBufferRange buf=%u offset=%zd len=%zd: shadow CPU activado ptr=%p size=%zu "
                  "(fallback=%d)",
                  MG_BS_TAG, app_name, (ssize_t)offset, (ssize_t)length,
                  (void*)(s->shadowPtr + (size_t)offset), s->shadowSize,
                  (int)s->fallbackMutable);
            return s->shadowPtr + (size_t)offset;
        }
        LOG_D("[%s] MapBufferRange buf=%u: malloc falhou para shadow — retorna NULL",
              MG_BS_TAG, app_name);
    }

    return nullptr;  // frontend cai no caminho antigo do driver
}

// ---------------------------------------------------------------------------
// mg_bs_flush_mapped_range — promoção shadow → buffer real
// ---------------------------------------------------------------------------
extern "C" bool mg_bs_flush_mapped_range(mg_enum target, mg_uint app_name,
                                         mg_intptr offset, mg_sizeiptr length) {
    MGBufferStorage* s = g_reg.get(app_name);
    if (!s || !s->mapped) return false;

    const MGBufferDispatch* d = mg_bs_dispatch();

    if (s->shadowPtr) {
        // Shadow → driver via glBufferSubData (sempre legal: buffer mutável do
        // fallback, ou mapeamento nativo que falhou onde DYNAMIC_STORAGE está
        // implicitamente presente por ser mutável).
        if (!d->BufferSubData) {
            LOG_D("[%s] FlushMappedRange buf=%u: shadow presente mas BufferSubData=NULL — ignorado",
                  MG_BS_TAG, app_name);
            return false;
        }
        d->BufferSubData(target, offset, length, s->shadowPtr + (size_t)offset);
        MG_STAT_INC(flushCalls);
        MG_STAT_ADD(flushBytes, length);
        LOG_D("[%s] FlushMappedRange buf=%u offset=%zd len=%zd: shadow→driver via glBufferSubData OK",
              MG_BS_TAG, app_name, (ssize_t)offset, (ssize_t)length);
        return true;
    }

    if (s->coherent) {
        LOG_D("[%s] FlushMappedRange buf=%u: buffer coerente — flush é no-op (owned)",
              MG_BS_TAG, app_name);
        return true;
    }

    if (!d->FlushMappedBufferRange) return false;
    d->FlushMappedBufferRange(target, offset, length);
    MG_STAT_INC(flushCalls);
    MG_STAT_ADD(flushBytes, length);
    LOG_D("[%s] FlushMappedRange buf=%u offset=%zd len=%zd: flush nativo OK",
          MG_BS_TAG, app_name, (ssize_t)offset, (ssize_t)length);
    return true;
}

// ---------------------------------------------------------------------------
// mg_bs_unmap — persistente permanece mapeado (inclui shadow)
// ---------------------------------------------------------------------------
extern "C" bool mg_bs_unmap(mg_enum target, mg_uint app_name, bool* out_result) {
    MGBufferStorage* s = g_reg.get(app_name);
    if (!s || !s->mapped) return false;
    if (s->persistent) {
        // Persistente (nativo ou shadow): reporta sucesso SEM desmapear.
        if (out_result) *out_result = true;
        LOG_D("[%s] UnmapBuffer buf=%u: persistente — mantendo mapeamento (shadow=%s)",
              MG_BS_TAG, app_name, s->shadowPtr ? "sim" : "não");
        return true;
    }
    const MGBufferDispatch* d = mg_bs_dispatch();
    if (!d->UnmapBuffer) return false;
    mg_boolean r = d->UnmapBuffer(target);
    MG_STAT_INC(unmapCalls);
    s->mapped    = false;
    s->mappedPtr = nullptr;
    if (out_result) *out_result = (bool)r;
    LOG_D("[%s] UnmapBuffer buf=%u: desmapeado (resultado=%d)", MG_BS_TAG, app_name, (int)r);
    return true;
}

// ---------------------------------------------------------------------------
// mg_bs_delete — liberta shadow + limpa metadata
// ---------------------------------------------------------------------------
extern "C" void mg_bs_delete(mg_uint app_name) {
    MGBufferStorage* s = g_reg.get(app_name);
    if (!s) return;
    if (s->shadowPtr) {
        LOG_D("[%s] Delete buf=%u: libertando shadow CPU size=%zu", MG_BS_TAG, app_name, s->shadowSize);
        free(s->shadowPtr);
        s->shadowPtr  = nullptr;
        s->shadowSize = 0;
    }
    s->mapped    = false;
    s->mappedPtr = nullptr;
    LOG_D("[%s] Delete buf=%u: metadata limpo", MG_BS_TAG, app_name);
    g_reg.erase(app_name);
}
