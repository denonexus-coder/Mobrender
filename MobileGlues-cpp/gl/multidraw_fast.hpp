// MobileGlues - gl/multidraw_fast.hpp
// Multi-draw acelerado para PowerVR Rogue GE8320 (e qualquer GLES 3.x)
// SPDX-License-Identifier: LGPL-2.1-only
#ifndef MOBILEGLUES_MULTIDRAW_FAST_HPP
#define MOBILEGLUES_MULTIDRAW_FAST_HPP

#include <GLES3/gl32.h>
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

// Bounding sphere em espaço de mundo (ou local — desde que o frustum
// seja aplicado no mesmo espaço).
struct mg_bbox_t {
    float cx, cy, cz;   // centro
    float r;            // raio
};

// O caller (Sodium/vanilla) fornece bboxes antes de cada chamada de
// multi-draw. Se não fornecer, frustum culling é desabilitado e só o
// "contiguous merge" roda. Thread-local.
void mg_multidraw_set_bboxes(const mg_bbox_t* bboxes, GLsizei n);

// Define o frustum atual em espaço de mundo (por frame).
// Planos na ordem: left, right, bottom, top, near, far.
//   planes[i] = (a, b, c, d) tal que ax+by+cz+d >= 0 = "dentro".
// Passar NULL desabilita.
void mg_multidraw_set_frustum(const float planes[6][4]);
void mg_multidraw_clear_frustum();

// Estatísticas (debug).
void mg_multidraw_get_stats(uint64_t* called, uint64_t* t0_hits,
                            uint64_t* t1_hits, uint64_t* fallback,
                            uint64_t* culled);

// Entry points rápidos. Retornam true se EMITIRAM o draw.
// Retornam false = caller deve usar o caminho vanilla (loop escalar).
// Não fazem fallback interno — o caller decide.
bool mg_multidraw_fast_elements(
    GLenum mode, const GLsizei* counts, GLenum type,
    const void* const* indices, GLsizei primcount);

bool mg_multidraw_fast_elements_bv(
    GLenum mode, const GLsizei* counts, GLenum type,
    const void* const* indices, GLsizei primcount, const GLint* basevertex);

bool mg_multidraw_fast_arrays(
    GLenum mode, const GLint* first, const GLsizei* counts, GLsizei drawcount);

#ifdef __cplusplus
}
#endif
#endif // MOBILEGLUES_MULTIDRAW_FAST_HPP
