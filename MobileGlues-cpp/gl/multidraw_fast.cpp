// MobileGlues - gl/multidraw_fast.cpp
// SPDX-License-Identifier: LGPL-2.1-only
//
// Estratégia (ordem de aplicação por chamada):
//   TIER 0: Frustum culling  — precisa de bboxes + frustum (API de cooperação)
//                              ganho medido: 10-16× no GE8320
//   TIER 1: Contiguous merge — detecta ranges contíguos no EBO/VBO
//                              ganho medido: 2-5×
//   FALLBACK: retorna false  — caller usa loop escalar (mg_glMultiDrawElements_drawelements)
//
// NOTA: "scratch coalesce" foi DELIBERADAMENTE omitido. Bench no GE8320
// mostrou que memcpy + glBufferSubData custa MAIS que o dispatch economizado
// (0.5× em modo espalhado, 1000 sub-draws × 256 índices).
//
// Este arquivo não depende de bench/multidraw_bench.cpp nem de nenhuma
// lógica de seleção de backend. É um fast-path pré-dispatcher.

#include "multidraw_fast.hpp"

// Pulls GLES (struct com todos os ponteiros de função), LOG_D, LOG_W_FORCE etc.
#include "../gles/loader.h"
#include "log.h"

#include <cstring>
#include <cstdlib>
#include <cmath>
#include <vector>

// ═══════════════════════════════════════════════════════════════════════════
// Estado thread-local
// ═══════════════════════════════════════════════════════════════════════════

namespace {

struct Frustum {
    float planes[6][4];
    bool  valid = false;

    // Sphere-frustum conservative test.
    // Returns false se a esfera está completamente FORA de algum plano.
    inline bool contains(float cx, float cy, float cz, float r) const {
        for (int i = 0; i < 6; ++i) {
            const float d = planes[i][0]*cx + planes[i][1]*cy
                          + planes[i][2]*cz + planes[i][3];
            if (d < -r) return false;
        }
        return true;
    }
};

thread_local const mg_bbox_t* tl_bboxes   = nullptr;
thread_local GLsizei          tl_bboxes_n = 0;
thread_local Frustum          tl_frustum;

thread_local struct Stats {
    uint64_t called        = 0;
    uint64_t tier0_hits    = 0;
    uint64_t tier1_hits    = 0;
    uint64_t fallback_hits = 0;
    uint64_t culled_draws  = 0;
} tl_stats;

// Buffers de trabalho por thread — evita malloc a cada chamada.
thread_local std::vector<GLsizei>     tl_k_counts;
thread_local std::vector<const void*> tl_k_offsets;
thread_local std::vector<GLint>       tl_k_bv;

inline size_t index_size_of(GLenum t) {
    switch (t) {
        case GL_UNSIGNED_BYTE:  return 1;
        case GL_UNSIGNED_SHORT: return 2;
        case GL_UNSIGNED_INT:   return 4;
        default:                return 0;
    }
}

} // anonymous namespace

// ═══════════════════════════════════════════════════════════════════════════
// API de cooperação
// ═══════════════════════════════════════════════════════════════════════════

extern "C" void mg_multidraw_set_bboxes(const mg_bbox_t* bboxes, GLsizei n) {
    tl_bboxes   = bboxes;
    tl_bboxes_n = n;
}

extern "C" void mg_multidraw_set_frustum(const float planes[6][4]) {
    if (!planes) {
        tl_frustum.valid = false;
        return;
    }
    for (int i = 0; i < 6; ++i) {
        const float a = planes[i][0], b = planes[i][1];
        const float c = planes[i][2], d = planes[i][3];
        const float len = std::sqrt(a*a + b*b + c*c);
        if (len < 1e-6f) {
            tl_frustum.valid = false;
            return;
        }
        tl_frustum.planes[i][0] = a / len;
        tl_frustum.planes[i][1] = b / len;
        tl_frustum.planes[i][2] = c / len;
        tl_frustum.planes[i][3] = d / len;
    }
    tl_frustum.valid = true;
}

extern "C" void mg_multidraw_clear_frustum() {
    tl_frustum.valid = false;
}

extern "C" void mg_multidraw_get_stats(uint64_t* called, uint64_t* t0,
                                        uint64_t* t1, uint64_t* fb,
                                        uint64_t* cu) {
    if (called) *called = tl_stats.called;
    if (t0)     *t0     = tl_stats.tier0_hits;
    if (t1)     *t1     = tl_stats.tier1_hits;
    if (fb)     *fb     = tl_stats.fallback_hits;
    if (cu)     *cu     = tl_stats.culled_draws;
}

// ═══════════════════════════════════════════════════════════════════════════
// TIER 0 — Frustum culling
//
// Filtra sub-draws por bounding sphere. Após filtrar, tenta colapsar o
// resultado num único draw (contíguo). Se não colapsar, emite loop sobre
// os draws que passaram no teste.
// ═══════════════════════════════════════════════════════════════════════════

static bool tier0_cull_elements(
    GLenum mode, const GLsizei* counts, GLenum type,
    const void* const* indices, GLsizei primcount, const GLint* basevertex)
{
    if (!tl_frustum.valid)                    return false;
    if (!tl_bboxes || tl_bboxes_n < primcount) return false;
    if (primcount < 2)                        return false;

    tl_k_counts.clear();
    tl_k_offsets.clear();
    if (basevertex) tl_k_bv.clear();

    GLint  common_bv   = basevertex ? basevertex[0] : 0;
    bool   bv_uniform  = true;
    GLsizei kept       = 0;

    for (GLsizei i = 0; i < primcount; ++i) {
        const mg_bbox_t& b = tl_bboxes[i];
        if (!tl_frustum.contains(b.cx, b.cy, b.cz, b.r)) continue;

        tl_k_counts.push_back(counts[i]);
        tl_k_offsets.push_back(indices[i]);
        if (basevertex) {
            tl_k_bv.push_back(basevertex[i]);
            if (basevertex[i] != common_bv) bv_uniform = false;
        }
        ++kept;
    }

    tl_stats.tier0_hits++;
    tl_stats.culled_draws += static_cast<uint64_t>(primcount - kept);

    if (kept == 0) return true; // tudo culled — nada a emitir, OK

    // Um único draw restante
    if (kept == 1) {
        if (basevertex) {
            GLES.glDrawElementsBaseVertex(mode, tl_k_counts[0], type,
                                          tl_k_offsets[0], tl_k_bv[0]);
        } else {
            GLES.glDrawElements(mode, tl_k_counts[0], type, tl_k_offsets[0]);
        }
        return true;
    }

    // Tentativa de colapso: basevertex uniforme + offsets contíguos
    const size_t es = index_size_of(type);
    if (es > 0 && bv_uniform) {
        intptr_t  prev  = reinterpret_cast<intptr_t>(tl_k_offsets[0]);
        GLsizei   total = 0;
        bool      cont  = true;
        for (size_t i = 0; i < tl_k_offsets.size(); ++i) {
            if (reinterpret_cast<intptr_t>(tl_k_offsets[i]) != prev) {
                cont = false;
                break;
            }
            prev  += static_cast<intptr_t>(tl_k_counts[i]) * static_cast<intptr_t>(es);
            total += tl_k_counts[i];
        }
        if (cont) {
            if (basevertex && common_bv != 0) {
                GLES.glDrawElementsBaseVertex(mode, total, type,
                                              tl_k_offsets[0], common_bv);
            } else {
                GLES.glDrawElements(mode, total, type, tl_k_offsets[0]);
            }
            return true;
        }
    }

    // Não colapsou — loop sobre os draws que sobreviveram ao cull
    for (size_t i = 0; i < tl_k_offsets.size(); ++i) {
        if (basevertex) {
            GLES.glDrawElementsBaseVertex(mode, tl_k_counts[i], type,
                                          tl_k_offsets[i], tl_k_bv[i]);
        } else {
            GLES.glDrawElements(mode, tl_k_counts[i], type, tl_k_offsets[i]);
        }
    }
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// TIER 1 — Contiguous merge para Elements
//
// Se todos os sub-draws formam uma sequência contínua no EBO (offset de um
// draw = offset anterior + count*elemsize) com o mesmo basevertex, emite
// um único glDrawElements cobrindo todo o range.
// ═══════════════════════════════════════════════════════════════════════════

static bool tier1_contiguous_elements(
    GLenum mode, const GLsizei* counts, GLenum type,
    const void* const* indices, GLsizei primcount, const GLint* basevertex)
{
    if (primcount < 2) return false;
    const size_t es = index_size_of(type);
    if (es == 0)       return false;
    if (counts[0] <= 0) return false;

    // Todos os basevertex têm de ser iguais para poder fundir
    const GLint common_bv = basevertex ? basevertex[0] : 0;
    if (basevertex) {
        for (GLsizei i = 1; i < primcount; ++i)
            if (basevertex[i] != common_bv) return false;
    }

    intptr_t prev_end = reinterpret_cast<intptr_t>(indices[0])
                      + static_cast<intptr_t>(counts[0]) * static_cast<intptr_t>(es);
    GLsizei total = counts[0];

    for (GLsizei i = 1; i < primcount; ++i) {
        if (counts[i] <= 0) continue; // ignora draws vazios internos
        if (reinterpret_cast<intptr_t>(indices[i]) != prev_end) return false;
        prev_end += static_cast<intptr_t>(counts[i]) * static_cast<intptr_t>(es);
        total    += counts[i];
    }

    if (basevertex && common_bv != 0) {
        GLES.glDrawElementsBaseVertex(mode, total, type, indices[0], common_bv);
    } else {
        GLES.glDrawElements(mode, total, type, indices[0]);
    }
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// TIER 1 — Contiguous merge para Arrays
// ═══════════════════════════════════════════════════════════════════════════

static bool tier1_contiguous_arrays(
    GLenum mode, const GLint* first, const GLsizei* counts, GLsizei drawcount)
{
    if (drawcount < 2)  return false;
    if (!first || !counts) return false;
    if (counts[0] <= 0) return false;

    GLint   prev_end = first[0] + counts[0];
    GLsizei total    = counts[0];

    for (GLsizei i = 1; i < drawcount; ++i) {
        if (counts[i] <= 0) continue;
        if (first[i] != prev_end) return false;
        prev_end += counts[i];
        total    += counts[i];
    }

    GLES.glDrawArrays(mode, first[0], total);
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// Entry points públicos
// ═══════════════════════════════════════════════════════════════════════════

extern "C" bool mg_multidraw_fast_elements(
    GLenum mode, const GLsizei* counts, GLenum type,
    const void* const* indices, GLsizei primcount)
{
    tl_stats.called++;
    if (primcount < 2 || !counts || !indices) return false;

    // TIER 0
    if (tier0_cull_elements(mode, counts, type, indices, primcount, nullptr))
        return true;

    // TIER 1
    if (tier1_contiguous_elements(mode, counts, type, indices, primcount, nullptr)) {
        tl_stats.tier1_hits++;
        return true;
    }

    tl_stats.fallback_hits++;
    return false;
}

extern "C" bool mg_multidraw_fast_elements_bv(
    GLenum mode, const GLsizei* counts, GLenum type,
    const void* const* indices, GLsizei primcount, const GLint* basevertex)
{
    tl_stats.called++;
    if (primcount < 2 || !counts || !indices) return false;

    // Sem basevertex — delega para a versão simples
    if (!basevertex)
        return mg_multidraw_fast_elements(mode, counts, type, indices, primcount);

    // TIER 0
    if (tier0_cull_elements(mode, counts, type, indices, primcount, basevertex))
        return true;

    // TIER 1
    if (tier1_contiguous_elements(mode, counts, type, indices, primcount, basevertex)) {
        tl_stats.tier1_hits++;
        return true;
    }

    tl_stats.fallback_hits++;
    return false;
}

extern "C" bool mg_multidraw_fast_arrays(
    GLenum mode, const GLint* first, const GLsizei* counts, GLsizei drawcount)
{
    tl_stats.called++;
    if (drawcount < 2) return false;

    if (tier1_contiguous_arrays(mode, first, counts, drawcount)) {
        tl_stats.tier1_hits++;
        return true;
    }

    tl_stats.fallback_hits++;
    return false;
}
