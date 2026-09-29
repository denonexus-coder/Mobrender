#ifndef MG_SHADER_CACHE_H
#define MG_SHADER_CACHE_H

// MobileGlues ESSL Shader Cache V2
// Caches ESSL (vertex + fragment pair) AFTER SPIRV-Cross conversion,
// keyed per-program with per-provider namespacing.
// All disk I/O is asynchronous. The render thread is never blocked by writes.

#include <string>
#include <cstdint>
#include <GL/gl.h>

// Lifecycle — call once at startup/shutdown
void mg_init_shader_cache();
void mg_destroy_shader_cache();

// Called from glShaderSource: stores raw GLSL + shader type for later use
void mg_cache_register_glsl(GLuint shader, GLenum type, const std::string& glsl);

// Called from glAttachShader: maps shader -> program
void mg_cache_attach(GLuint program, GLuint shader);

// Called at the START of glLinkProgram.
// Returns true (HIT) and populates out_v_essl / out_f_essl from L0 or disk.
// Returns false (MISS) — caller must run GLSL->SPIR-V->SPIRV-Cross normally.
bool mg_cache_lookup_program(GLuint program, std::string& out_v_essl, std::string& out_f_essl);

// Called after a MISS, once SPIRV-Cross produced both ESSLs.
// Updates L0 and enqueues async disk write (atomic rename).
void mg_cache_save_program(GLuint program, const std::string& v_essl, const std::string& f_essl);

// Recupera o GLSL registrado para o path de MISS
std::string mg_cache_get_glsl(GLuint shader);

#endif // MG_SHADER_CACHE_H
