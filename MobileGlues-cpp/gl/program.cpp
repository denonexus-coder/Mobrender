// MobileGlues - gl/program.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1:
//   https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
// SPDX-License-Identifier: LGPL-2.1-only
// End of Source File Header

#include <regex.h>
#include "GL/glext.h"
#include "GLES3/gl32.h"
#include "log.h"
#include "shader.h"
#include "program.h"
#include "mg_shader_cache.h"
#include "glsl/glsl_for_es.h"
#include <regex>
#include <cstring>
#include <iostream>
#include "../config/settings.h"
#include "drawing.h"

#define DEBUG 0

extern UnorderedMap<GLuint, bool> shader_map_is_sampler_buffer_emulated;
UnorderedMap<GLuint, bool> program_map_is_sampler_buffer_emulated;

enum class ShouldGenerateFSState : int {
    Never = 0,
    Maybe = 1,
    Unknown = 2
};

bool is_direct_shader(const char* glsl);

UnorderedMap<GLuint, ShouldGenerateFSState> program_map_should_generate_fs;

std::string updateLayoutLocation(const std::string& esslSource, GLuint color, const char* name) {
    const std::string& shaderCode = esslSource;

    std::string pattern = std::string(R"((layout\s*$[^)]*location\s*=\s*\d+[^)]*$\s*)?)") +
                          R"(out\s+((?:highp|mediump|lowp|\w+\s+)*\w+)\s+)" + name + R"(\s*;)";

    std::string replacement = "layout (location = " + std::to_string(color) + ") out $2 " + name + ";";

    std::regex reg(pattern);
    std::string modifiedCode = std::regex_replace(shaderCode, reg, replacement);

    return modifiedCode;
}

struct FragBinding { GLuint color; std::string name; };
static UnorderedMap<GLuint, std::vector<FragBinding>> g_frag_bindings;

void glBindFragDataLocation(GLuint program, GLuint color, const GLchar* name) {
    LOG()
    LOG_D("glBindFragDataLocation(%d, %d, %s)", program, color, name)

    if (strlen(name) > 8 && strncmp(name, "outColor", 8) == 0) {
        const char* numberStr = name + 8;
        bool isNumber = true;
        for (int i = 0; numberStr[i] != '\0'; ++i) {
            if (!isdigit(numberStr[i])) {
                isNumber = false;
                break;
            }
        }

        if (isNumber) {
            unsigned int extractedColor = static_cast<unsigned int>(std::stoul(numberStr));
            if (extractedColor == color) {
                LOG_D("Find outColor* with color *, skipping")
                return;
            }
        }
    }

    g_frag_bindings[program].push_back({color, std::string(name)});
}

static std::string DefaultFSSource;
static unsigned CurrentDefaultFSSourceVersion = 0; // the version (hardware->es_version) may change during runtime

void GenerateDefaultFSSource() {
    if (CurrentDefaultFSSourceVersion != hardware->es_version) {
        CurrentDefaultFSSourceVersion = hardware->es_version;
        std::ostringstream ss;
        ss << "#version " << CurrentDefaultFSSourceVersion << " es\n";
        ss << "precision mediump float;\n\n";
        ss << "out vec4 fragColor;\n\n";
        ss << "void main() {\n";
        ss << "    fragColor = vec4(1.0, 1.0, 1.0, 1.0);\n";
        ss << "}\n";

        DefaultFSSource = ss.str();
    }
}

static UnorderedMap<unsigned, GLuint> DefaultFSMap; // essl version <-> shader id
void glLinkProgram(GLuint program) {
    LOG()
    LOG_D("glLinkProgram(%d)", program)

    // ── V2 Cache: L0 / disk lookup ──────────────────────────────────────────
    // Try to get previously cached ESSL for this vertex+fragment pair.
    // On HIT we skip GLSL→SPIR-V→SPIRV-Cross entirely.
    {
        std::string v_essl, f_essl;
        if (mg_cache_lookup_program(program, v_essl, f_essl)) {
            // Apply the cached ESSL directly to the driver shaders
            // We need to find the shader IDs attached to this program
            GLint attached_count = 0;
            GLES.glGetProgramiv(program, GL_ATTACHED_SHADERS, &attached_count);
            if (attached_count >= 2) {
                std::vector<GLuint> attached(attached_count);
                GLES.glGetAttachedShaders(program, attached_count, nullptr, attached.data());
                for (GLuint sh : attached) {
                    GLint type = 0;
                    GLES.glGetShaderiv(sh, GL_SHADER_TYPE, &type);
                    if (type == GL_VERTEX_SHADER) {
                        const char* src = v_essl.c_str();
                        GLES.glShaderSource(sh, 1, &src, nullptr);
                        GLES.glCompileShader(sh);
                    } else if (type == GL_FRAGMENT_SHADER) {
                        const char* src = f_essl.c_str();
                        GLES.glShaderSource(sh, 1, &src, nullptr);
                        GLES.glCompileShader(sh);
                    }
                }
                // Clear legacy shaderInfo state (it's been superseded by cache)
                shaderInfo.id = 0;
                shaderInfo.converted = "";
                shaderInfo.frag_data_changed_converted.clear();
                shaderInfo.frag_data_changed = 0;
                GLES.glLinkProgram(program);
                CHECK_GL_ERROR
                return;
            }
            // Fallthrough: not enough attached shaders, do normal pipeline
        }
    }
    // ── End V2 Cache lookup ─────────────────────────────────────────────────

    // MISS path: Executar o pipeline de traducao (GLSL -> SPIRV -> SPIRV-Cross -> ESSL)
    LOG_D("[MGShaderCache][TRACE] program=%u cache=MISS", program);
    std::string captured_v_essl, captured_f_essl;
    
    GLint attached_count = 0;
    GLES.glGetProgramiv(program, GL_ATTACHED_SHADERS, &attached_count);
    if (attached_count > 0) {
        std::vector<GLuint> attached(attached_count);
        GLES.glGetAttachedShaders(program, attached_count, nullptr, attached.data());
        for (GLuint sh : attached) {
            GLint type = 0;
            GLES.glGetShaderiv(sh, GL_SHADER_TYPE, &type);
            
            std::string raw_glsl = mg_cache_get_glsl(sh);
            if (raw_glsl.empty()) continue;

            if (!is_direct_shader(raw_glsl.c_str())) {
                LOG_D("[MGShaderCache][TRACE] stage=%s program=%u cache=MISS translation=GLSL->SPIRV->SPIRVCROSS->ESSL input_version=330", 
                      (type == GL_VERTEX_SHADER ? "VERTEX" : "FRAGMENT"), program);
                
                int errc = 0;
                int glsl_ver = getGLSLVersion(raw_glsl.c_str());
                std::string essl = GLSLtoGLSLES(raw_glsl.c_str(), type, hardware->es_version, glsl_ver, errc);
                
                if (errc >= 0 && !essl.empty()) {
                    if (type == GL_VERTEX_SHADER) captured_v_essl = essl;
                    if (type == GL_FRAGMENT_SHADER) {
                        captured_f_essl = essl;
                        // Apply frag data bindings for fragment shader
                        for (const auto& b : g_frag_bindings[program]) {
                            essl = updateLayoutLocation(essl, b.color, b.name.c_str());
                        }
                    }

                    const char* src = essl.c_str();
                    GLES.glShaderSource(sh, 1, &src, nullptr);
                    GLES.glCompileShader(sh);
                } else {
                    LOG_E("Failed to translate GLSL for shader %u", sh);
                }
            } else {
                if (type == GL_VERTEX_SHADER) captured_v_essl = raw_glsl;
                if (type == GL_FRAGMENT_SHADER) captured_f_essl = raw_glsl;
            }
        }
    }

    shaderInfo.id = 0;
    shaderInfo.converted = "";
    shaderInfo.frag_data_changed_converted.clear();
    shaderInfo.frag_data_changed = 0;

    // Generate default fragment shader if needed (vertex-only program)
    if (program_map_should_generate_fs[program] == ShouldGenerateFSState::Maybe) {
        GenerateDefaultFSSource();
        GLuint& default_fs = DefaultFSMap[CurrentDefaultFSSourceVersion];
        if (!default_fs) {
            default_fs = GLES.glCreateShader(GL_FRAGMENT_SHADER);
            const char* src = DefaultFSSource.c_str();
            GLES.glShaderSource(default_fs, 1, &src, nullptr);

            GLES.glCompileShader(default_fs);

            GLint success = 0;
            GLES.glGetShaderiv(default_fs, GL_COMPILE_STATUS, &success);
            if (!success) {
                GLint logLength = 0;
                GLES.glGetShaderiv(default_fs, GL_INFO_LOG_LENGTH, &logLength);
                std::vector<char> log(logLength);
                GLES.glGetShaderInfoLog(default_fs, logLength, nullptr, log.data());
                LOG_E("Default fragment shader compile error for program %u :\n%s\n", program, log.data());
                GLES.glDeleteShader(default_fs);
                default_fs = 0;
            }
        }

        if (default_fs) {
            LOG_D("Try to attach missing default FS for program %u...", program);
            GLES.glAttachShader(program, default_fs);
        }
    }

    GLES.glLinkProgram(program);
    CHECK_GL_ERROR

    // ── V2 Cache: save after successful MISS + link ─────────────────────────
    // Save vertex+fragment ESSL pair so next run is a cache hit.
    // Only save when we actually have ESSL content (normal pipeline ran).
    // Vertex ESSL: retrieve from driver-compiled source via glGetShaderSource.
    if (!captured_f_essl.empty()) {
        GLint attached_count = 0;
        GLES.glGetProgramiv(program, GL_ATTACHED_SHADERS, &attached_count);
        if (attached_count >= 2) {
            std::vector<GLuint> attached(attached_count);
            GLES.glGetAttachedShaders(program, attached_count, nullptr, attached.data());
            for (GLuint sh : attached) {
                GLint type = 0;
                GLES.glGetShaderiv(sh, GL_SHADER_TYPE, &type);
                if (type == GL_VERTEX_SHADER && captured_v_essl.empty()) {
                    GLint src_len = 0;
                    GLES.glGetShaderiv(sh, GL_SHADER_SOURCE_LENGTH, &src_len);
                    if (src_len > 1) {
                        captured_v_essl.resize(src_len - 1);
                        GLES.glGetShaderSource(sh, src_len, nullptr, &captured_v_essl[0]);
                    }
                }
            }
        }
        if (!captured_v_essl.empty()) {
            mg_cache_save_program(program, captured_v_essl, captured_f_essl);
        }
    }
    // ── End V2 Cache save ───────────────────────────────────────────────────
}


void glGetProgramiv(GLuint program, GLenum pname, GLint* params) {
    LOG()
    GLES.glGetProgramiv(program, pname, params);
    if (global_settings.ignore_error >= IgnoreErrorLevel::Partial &&
        (pname == GL_LINK_STATUS || pname == GL_VALIDATE_STATUS) && !*params) {
        GLchar infoLog[512];
        GLES.glGetProgramInfoLog(program, 512, nullptr, infoLog);

        LOG_W_FORCE("Program %d linking failed: \n%s", program, infoLog);
        LOG_W_FORCE("Now try to cheat.");
        *params = GL_TRUE;
    }
    CHECK_GL_ERROR
}

void glUseProgram(GLuint program) {
    LOG()
    LOG_D("glUseProgram(%d)", program)
    if (program != gl_state->current_program) {
        gl_state->current_program = program;
        GLES.glUseProgram(program);
        CHECK_GL_ERROR
    }
}

void glAttachShader(GLuint program, GLuint shader) {
    LOG()
    LOG_D("glAttachShader(%u, %u)", program, shader)
    if (hardware->emulate_texture_buffer && shader_map_is_sampler_buffer_emulated[shader])
        program_map_is_sampler_buffer_emulated[program] = true;

    GLint type = 0;
    GLES.glGetShaderiv(shader, GL_SHADER_TYPE, &type);
    auto& should_gen_fs_map = program_map_should_generate_fs;
    if (type == GL_FRAGMENT_SHADER) {
        should_gen_fs_map[program] = ShouldGenerateFSState::Never;
    } else if (type == GL_VERTEX_SHADER) {
        auto it = should_gen_fs_map.find(program);
        if (it == should_gen_fs_map.end() || should_gen_fs_map[program] != ShouldGenerateFSState::Never) {
            should_gen_fs_map[program] = ShouldGenerateFSState::Maybe;
        }
    }

    GLES.glAttachShader(program, shader);
    mg_cache_attach(program, shader);
    CHECK_GL_ERROR
}

extern UnorderedMap<GLuint, SamplerInfo> g_samplerCacheForSamplerBuffer;

GLuint glCreateProgram() {
    LOG()
    LOG_D("glCreateProgram")
    GLuint program = GLES.glCreateProgram();
    if (hardware->emulate_texture_buffer) {
        program_map_is_sampler_buffer_emulated[program] = false;
        if (g_samplerCacheForSamplerBuffer.find(program) != g_samplerCacheForSamplerBuffer.end()) {
            g_samplerCacheForSamplerBuffer.erase(program);
        }
    }
    program_map_should_generate_fs[program] = ShouldGenerateFSState::Unknown;

    CHECK_GL_ERROR
    return program;
}

// GL 3.1's name-only half of the active-uniform query, on top of the ES call that
// already returns the same string.
//
// It was a stub -- a no-op that wrote neither the name nor the length and, being a
// stub rather than an error, left glGetError clean. Callers got whatever was
// already in the buffer they passed.
//
// That is not a cosmetic gap. The standard way to build a name -> location map is
// to walk the active uniforms by index and ask for each name, and a caller doing
// that ended up with a map keyed on garbage: every later lookup missed, so the
// uniforms never got set and kept whatever the driver had zero-initialised them
// to. NeoForge's early loading window does exactly this, and a screenSize of
// (0, 0) turned its every vertex into a division by zero -- gl_Position came out
// non-finite, every primitive was discarded, and the window rendered black with
// nothing anywhere reporting a problem.
void glGetActiveUniformName(GLuint program, GLuint uniformIndex, GLsizei bufSize, GLsizei* length,
                            GLchar* uniformName) {
    LOG()
    LOG_D("glGetActiveUniformName(program: %u, index: %u, bufSize: %d)", program, uniformIndex, bufSize)

    if (length) *length = 0;
    if (bufSize <= 0 || uniformName == nullptr) {
        // Nothing to write. Still forwarded when bufSize is negative so the driver
        // raises the GL_INVALID_VALUE the caller is owed.
        if (bufSize < 0) GLES.glGetActiveUniform(program, uniformIndex, bufSize, nullptr, nullptr, nullptr, nullptr);
        CHECK_GL_ERROR
        return;
    }

    // Same buffer contract in both calls: at most bufSize-1 characters plus the
    // terminator, and a length that excludes it. The size and type this also
    // returns are what glGetActiveUniformsiv is for; they are discarded here.
    GLint size = 0;
    GLenum type = 0;
    GLsizei written = 0;
    uniformName[0] = '\0';
    GLES.glGetActiveUniform(program, uniformIndex, bufSize, &written, &size, &type, uniformName);
    if (length) *length = written;

    LOG_D("  -> \"%s\"", uniformName)
    CHECK_GL_ERROR
}
