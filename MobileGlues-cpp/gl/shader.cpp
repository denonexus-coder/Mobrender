// MobileGlues - gl/shader.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1:
//   https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
// SPDX-License-Identifier: LGPL-2.1-only
// End of Source File Header

#include <cctype>
#include "shader.h"

#include <GL/gl.h>
#include "log.h"
#include "program.h"
#include "../gles/loader.h"
#include "../includes.h"
#include "glsl/glsl_for_es.h"
#include "../config/settings.h"
#include "FSR1/FSR1.h"
#include "mg_shader_cache.h"
#include <mutex>

#define DEBUG 0

struct shader_t shaderInfo;

UnorderedMap<GLuint, bool> shader_map_is_sampler_buffer_emulated;

bool can_run_essl3(unsigned int esversion, const char* glsl) {
    // Find the version string, ignoring leading whitespace or comments
    const char* version_pos = strstr(glsl, "#version");
    if (!version_pos) return false;

    if (strncmp(version_pos, "#version 100", 12) == 0) {
        return true;
    }

    unsigned int glsl_version = 0;
    if (strncmp(version_pos, "#version 300 es", 15) == 0) {
        glsl_version = 300;
    } else if (strncmp(version_pos, "#version 310 es", 15) == 0) {
        glsl_version = 310;
    } else if (strncmp(version_pos, "#version 320 es", 15) == 0) {
        glsl_version = 320;
    } else {
        return false;
    }
    return esversion >= glsl_version;
}

bool is_direct_shader(const char* glsl) {
    bool es3_ability = can_run_essl3(hardware->es_version, glsl);
    return es3_ability;
}

bool check_if_sampler_buffer_used(std::string str) {
    return str.find("samplerBuffer") != std::string::npos;
}

static std::once_flag g_cache_init_flag;


#include "mg_shader_cache.h"
#include <mutex>


void glShaderSource(GLuint shader, GLsizei count, const GLchar* const* string, const GLint* length) {
    std::call_once(g_cache_init_flag, []{
        mg_init_shader_cache();
    });
    LOG()
    
    size_t l = 0;
    for (int i = 0; i < count; i++)
        l += (length && length[i] >= 0) ? length[i] : strlen(string[i]);
    std::string glsl_src;
    glsl_src.reserve(l + 1);
    if (length) {
        for (int i = 0; i < count; i++) {
            if (length[i] >= 0) glsl_src += std::string_view(string[i], length[i]);
            else glsl_src += string[i];
        }
    } else {
        for (int i = 0; i < count; i++) glsl_src += string[i];
    }
    
    GLint shaderType = 0;
    GLES.glGetShaderiv(shader, GL_SHADER_TYPE, &shaderType);
    mg_cache_register_glsl(shader, shaderType, glsl_src);
    
    // PROTEJA O glShaderSource()
    // Impedir que o GLSL desktop alcance o driver prematuramente
    if (glsl_src.find("#version 330") != std::string::npos || !is_direct_shader(glsl_src.c_str())) {
        LOG_E("[MGShaderCache] PROTECT: Impedindo GLSL 330 de chegar ao driver. Traducao movida para glLinkProgram.");
        const char* dummy = "#version 320 es\nvoid main() {}";
        GLES.glShaderSource(shader, 1, &dummy, nullptr);
        CHECK_GL_ERROR
        return;
    }

    GLES.glShaderSource(shader, count, string, length);
    CHECK_GL_ERROR
}

void glGetShaderiv(GLuint shader, GLenum pname, GLint* params) {
    LOG()
    GLES.glGetShaderiv(shader, pname, params);
    if (global_settings.ignore_error >= IgnoreErrorLevel::Partial && pname == GL_COMPILE_STATUS && !*params) {
        GLchar infoLog[512];
        GLES.glGetShaderInfoLog(shader, 512, nullptr, infoLog);
        LOG_W_FORCE("Shader %d compilation failed: \n%s", shader, infoLog)
        LOG_W_FORCE("Now try to cheat.")
        *params = GL_TRUE;
    }
    CHECK_GL_ERROR
}

GLuint glCreateShader(GLenum shaderType) {
    if (global_settings.fsr1_setting != FSR1_Quality_Preset::Disabled && !fsrInitialized) {
        InitFSRResources();
    }

    LOG()
    LOG_D("glCreateShader(%s)", glEnumToString(shaderType))
    GLuint shader = GLES.glCreateShader(shaderType);
    if (shader != 0 && hardware->emulate_texture_buffer) shader_map_is_sampler_buffer_emulated[shader] = false;
    CHECK_GL_ERROR
    return shader;
}
