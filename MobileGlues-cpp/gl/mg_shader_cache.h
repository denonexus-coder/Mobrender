#ifndef MG_SHADER_CACHE_H
#define MG_SHADER_CACHE_H

// MobileGlues ESSL Shader Cache V3
//
// Stores ESSL ONLY — never GLSL.
//
// Program pair cache:
//   key   = combine_hashes(hash(vertex_essl), hash(fragment_essl))
//   value = { vertex_essl, fragment_essl }
//
// Per-shader ESSL map:
//   shader_id → essl string  (populated by glShaderSource after conversion)
//   used at glLinkProgram to assemble the V+F pair.
//
// All disk I/O is asynchronous. The render thread is never blocked by writes.

#include <string>
#include <cstdint>
#include <GL/gl.h>

// ──────────────────────────────────────────────────────────────
// Lifecycle — call once at startup/shutdown
// ──────────────────────────────────────────────────────────────
void mg_init_shader_cache();
void mg_destroy_shader_cache();

// ──────────────────────────────────────────────────────────────
// Per-shader ESSL storage
// Called from glShaderSource after GLSL→ESSL conversion.
// Stores ESSL keyed by hash(ESSL) (L0+disk) and by shader GLuint (for glLinkProgram).
// Also records shader_type (GL_VERTEX_SHADER / GL_FRAGMENT_SHADER) for pair assembly.
// ──────────────────────────────────────────────────────────────
__attribute__((noinline))
void mg_shader_cache_save_essl(GLuint shader, GLenum shader_type, const std::string& essl);

// Returns the ESSL stored for a given shader ID.
// Called from glLinkProgram MISS path to build the V+F pair.
std::string mg_cache_get_essl(GLuint shader);

// ──────────────────────────────────────────────────────────────
// Program pair attachment tracking
// Called from glAttachShader: maps program → attached shaders.
// ──────────────────────────────────────────────────────────────
void mg_cache_attach(GLuint program, GLuint shader);

// ──────────────────────────────────────────────────────────────
// Program pair cache lookup
// Called at the START of glLinkProgram.
// Identifies vertex+fragment ESSL from attached shaders, builds key from ESSL content,
// checks L0 (in-process) then L1 (disk).
// Returns true (HIT)  → out_v_essl / out_f_essl populated; caller feeds driver normally.
// Returns false (MISS) → caller uses existing ESSL, calls mg_cache_save_program.
// ──────────────────────────────────────────────────────────────
bool mg_cache_lookup_program(GLuint program, std::string& out_v_essl, std::string& out_f_essl);

// ──────────────────────────────────────────────────────────────
// Program pair cache save
// Called after a MISS, once both ESSL sources are known.
// key = combine_hashes(hash(v_essl), hash(f_essl)) — ESSL-based, never GLSL.
// Updates L0 immediately and enqueues async disk write (atomic rename).
// ──────────────────────────────────────────────────────────────
void mg_cache_save_program(GLuint program, const std::string& v_essl, const std::string& f_essl);

// ──────────────────────────────────────────────────────────────
// Per-shader ESSL disk lookup (optional fast path, by ESSL hash)
// Returns true → out_essl populated from L0 or disk.
// Returns false → MISS; caller must convert and call mg_shader_cache_save_essl.
// ──────────────────────────────────────────────────────────────
[[nodiscard]]
__attribute__((noinline))
bool mg_shader_cache_lookup_essl_by_hash(uint64_t essl_hash, std::string& out_essl);

#endif // MG_SHADER_CACHE_H
