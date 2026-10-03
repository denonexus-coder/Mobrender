// MobileGlues - gl/getter.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1:
//   https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
// SPDX-License-Identifier: LGPL-2.1-only
// End of Source File Header

#include "getter.h"
#include "enable.h"
#include "../egl/context.h"
#include "buffer.h"
#include "texture.h"
#include <string>
#include <format>
#include <vector>
#include <random>
#include "FSR1/FSR1.h"
#include "log.h"
#include "mg.h"
#include "pixel.h"
#include "random_string_gen.h"
#include "../config/settings.h"

#define DEBUG 0

Version GLVersion;

namespace {
// See mg_set_gl_error in gl/mg.h for why this exists and why it is per thread.
thread_local GLenum g_frontend_error = GL_NO_ERROR;
} // namespace

void mg_set_gl_error(GLenum error) {
    if (error == GL_NO_ERROR) return;
    // First error wins. A later, vaguer failure must not paper over the one that
    // actually explains what the application did wrong.
    if (g_frontend_error != GL_NO_ERROR) return;
    g_frontend_error = error;
    LOG_D("MobileGlues raised %s", glEnumToString(error))
}

void glGetIntegerv(GLenum pname, GLint* params) {
    LOG()
    LOG_D("glGetIntegerv, pname: %s", glEnumToString(pname))
    switch (pname) {
    case GL_NUM_EXTENSIONS + GL_BACKEND_GETTER_MG:
        GLES.glGetIntegerv(pname - GL_BACKEND_GETTER_MG, params);
        return;
    case GL_CONTEXT_PROFILE_MASK:
        (*params) = GL_CONTEXT_CORE_PROFILE_BIT;
        break;
    case GL_NUM_EXTENSIONS:
        static GLint num_extensions = -1;
        if (num_extensions == -1) {
            const GLubyte* ext_str = glGetString(GL_EXTENSIONS);
            if (ext_str) {
                std::string copy_str((const char*)ext_str);
                std::string token;
                size_t pos = 0;
                num_extensions = 0;
                while ((pos = copy_str.find(' ')) != std::string::npos) {
                    num_extensions++;
                    copy_str.erase(0, pos + 1);
                }
                if (!copy_str.empty()) num_extensions++; // Count the last token
            } else {
                num_extensions = 0;
            }
        }
        (*params) = num_extensions;
        break;
    case GL_MAJOR_VERSION:
        (*params) = 4;
        break;
    case GL_MINOR_VERSION:
        (*params) = 4;
        break;
    case GL_MAX_TEXTURE_IMAGE_UNITS: {
        int es_params = 16;
        GLES.glGetIntegerv(pname, &es_params);
        CHECK_GL_ERROR(*params) = es_params * 2;
        // Why is the real GL_MAX_TEXTURE_IMAGE_UNITS bigger than what GLES.glGetIntegerv returns?
        break;
    }
    case GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS: {
        // This is the bound on glActiveTexture, and drivers report generously --
        // Mali-G77 says 96. Forwarding it unchanged promised more units than the
        // layer keeps bindings for, and the units past the end were accepted by
        // the driver but dropped here, so a glBindTexture after one of them went
        // to the wrong unit with nothing reporting it. Promise only what
        // gl/texture.cpp can actually track.
        int es_params = 32;
        GLES.glGetIntegerv(pname, &es_params);
        CHECK_GL_ERROR
        const int tracked = mg_max_texture_units();
        (*params) = es_params < tracked ? es_params : tracked;
        break;
    }
    case GL_CONTEXT_FLAGS: {
        // Reported from what the context was actually created with. Claiming
        // flags the application never asked for -- as this did before it was
        // reduced to 0 -- makes a loader believe it has a debug or robust context
        // that does not behave like one.
        (*params) = g_current_ctx ? g_current_ctx->context_flags : 0;
        break;
    }
    case GL_ARRAY_BUFFER_BINDING:
    case GL_ATOMIC_COUNTER_BUFFER_BINDING:
    case GL_COPY_READ_BUFFER_BINDING:
    case GL_COPY_WRITE_BUFFER_BINDING:
    case GL_DRAW_INDIRECT_BUFFER_BINDING:
    case GL_DISPATCH_INDIRECT_BUFFER_BINDING:
    case GL_ELEMENT_ARRAY_BUFFER_BINDING:
    case GL_PIXEL_PACK_BUFFER_BINDING:
    case GL_PIXEL_UNPACK_BUFFER_BINDING:
    case GL_SHADER_STORAGE_BUFFER_BINDING:
    case GL_TRANSFORM_FEEDBACK_BUFFER_BINDING:
    case GL_UNIFORM_BUFFER_BINDING:
        (*params) = (int)find_bound_buffer(pname);
        LOG_D("  -> %d", *params)
        break;
    case GL_VERTEX_ARRAY_BINDING:
        (*params) = (int)find_bound_array();
        break;
    // GL_FRAMEBUFFER_BINDING is the same enum as GL_DRAW_FRAMEBUFFER_BINDING.
    case GL_DRAW_FRAMEBUFFER_BINDING: {
        GLES.glGetIntegerv(pname, params);
        // Hide the FSR1 redirect. While FSR1 is on, the application's framebuffer
        // 0 really is g_renderFBO (gl/framebuffer.cpp), and handing that name back
        // let the application save it and bind it again later -- by which point a
        // resolution change may have deleted and recreated the target, so the
        // restore named a dead framebuffer and stuck. Answering 0 means a restore
        // goes back through the redirect and lands wherever it currently points.
        if (FSR1_Context::g_renderFBO != 0 && *params == (GLint)FSR1_Context::g_renderFBO) *params = 0;
        LOG_D("  -> %d", *params)
        break;
    }
    default: {
        // The enable table owns every enable capability and the handful of limits
        // that describe it, so glGetIntegerv can never disagree with glIsEnabled.
        GLboolean enabled = GL_FALSE;
        GLint ival = 0;
        if (mg_enable_query(pname, &enabled)) {
            (*params) = enabled ? 1 : 0;
            break;
        }
        if (mg_enable_query_int(pname, &ival)) {
            (*params) = ival;
            break;
        }
        // The pixel-store parameters GLES has no answer for. Forwarding them
        // returned GL_INVALID_ENUM and left *params exactly as the caller left it.
        if (mg_pixel_store_query_int(pname, params)) {
            LOG_D("  -> %d", *params)
            break;
        }
        GLES.glGetIntegerv(pname, params);
        LOG_D("  -> %d", *params)
        CHECK_GL_ERROR
    }
    }
}

GLenum glGetError() {
    LOG()
    // Both are consumed whether or not they get reported: leaving either latched
    // would hand it to a later, unrelated glGetError.
    const GLenum backend = GLES.glGetError();
    const GLenum frontend = g_frontend_error;
    g_frontend_error = GL_NO_ERROR;

    // GL_NO_ERROR, always, in every configuration and whatever ignoreError says.
    //
    // Deliberate, and not the same thing as not knowing. This layer emulates
    // enough of desktop GL on top of GLES that a faithfully forwarded error is
    // more often an artefact of how a call had to be translated than something the
    // application got wrong -- and hosts treat errors as fatal or fall back to
    // slower paths on them. One example from inside this very library:
    // gl/buffer.cpp's glMapBuffer asks glGetError and returns nullptr if it is
    // not clear, a branch that only stays dead because of this.
    //
    // What the errors are still for is the log. Every path that raises one names
    // itself right next to the call, so a quiet failure is diagnosable from a
    // logcat even though the application will never be told.
    const GLenum swallowed = frontend != GL_NO_ERROR ? frontend : backend;
    if (swallowed != GL_NO_ERROR) {
        LOG_W("glGetError -> %s, reported to the application as GL_NO_ERROR", glEnumToString(swallowed))
    }
    return GL_NO_ERROR;
}

static std::string es_ext;
std::string GetExtensionsList() {
    return es_ext;
}

// ---------------------------------------------------------------------------
// Capability-driven ARB exposure
//
// Every GL_ARB_* string this layer advertises must be backed by a real entry
// point on the host GLES driver. The extension string alone proves nothing on
// Android (the EGL wrapper resolves stubs), and a resolved symbol alone proves
// nothing either (drivers can export a symbol that draws nothing and raises no
// error). Both checks together are the same standard config/settings.cpp
// already applies to the multidraw backends.
//
// g_gles_caps is filled by init_gles_target() before InitGLESBaseExtensions()
// runs, so the caps are already valid here.
// ---------------------------------------------------------------------------

// One ARB string + the predicate that proves the host can serve it.
struct mg_arb_entry_t {
    const char* arb_name;
    bool (*supported)();
};

static bool mg_cap_es31() {
    return g_gles_caps.major > 3 || (g_gles_caps.major == 3 && g_gles_caps.minor >= 1);
}

static bool mg_cap_es32() {
    return g_gles_caps.major > 3 || (g_gles_caps.major == 3 && g_gles_caps.minor >= 2);
}

// Helpers used internally (same predicates, aliased for readability in lambdas).
static bool mg_has_es31() { return mg_cap_es31(); }
static bool mg_has_es32() { return mg_cap_es32(); }

// glBufferStorageEXT: GL_EXT_buffer_storage (flags map 1:1 to GL_ARB_buffer_storage)
static bool mg_cap_buffer_storage() {
    return g_gles_caps.GL_EXT_buffer_storage && GLES.glBufferStorageEXT != nullptr;
}

// glMapBufferRange / glFlushMappedBufferRange: GLES 3.0 core, identical semantics
static bool mg_cap_map_buffer_range() {
    return GLES.glMapBufferRange != nullptr && GLES.glFlushMappedBufferRange != nullptr;
}

// glFenceSync / glClientWaitSync / glDeleteSync: GLES 3.0 core
static bool mg_cap_sync() {
    return GLES.glFenceSync != nullptr && GLES.glClientWaitSync != nullptr &&
           GLES.glDeleteSync != nullptr;
}

// glCopyBufferSubData: GLES 3.0 core (Sodium arena compaction, GPU->GPU)
static bool mg_cap_copy_buffer() {
    return GLES.glCopyBufferSubData != nullptr;
}

// glDrawElementsBaseVertex / glMultiDrawElementsBaseVertex: EXT/OES form
static bool mg_cap_draw_elements_base_vertex() {
    return (g_gles_caps.GL_EXT_draw_elements_base_vertex ||
            g_gles_caps.GL_OES_draw_elements_base_vertex) &&
           GLES.glDrawElementsBaseVertex != nullptr;
}

// VAOs: core since GLES 3.0 (GL_OES_vertex_array_object on this device)
static bool mg_cap_vao() {
    return GLES.glGenVertexArrays != nullptr && GLES.glBindVertexArray != nullptr;
}

// glVertexAttribBinding / glVertexBindVertexBuffer: GLES 3.0 core
static bool mg_cap_attrib_binding() {
    return GLES.glVertexAttribBinding != nullptr && GLES.glBindVertexBuffer != nullptr;
}

// glDrawElementsInstanced / glVertexAttribDivisor: GLES 3.0 core
static bool mg_cap_instanced() {
    return GLES.glDrawElementsInstanced != nullptr && GLES.glVertexAttribDivisor != nullptr;
}

// SSBO: GLES 3.1 core
static bool mg_cap_ssbo() {
    return mg_has_es31() && GLES.glShaderStorageBlockBinding != nullptr;
}

// glDispatchCompute: GLES 3.1 core
static bool mg_cap_compute() {
    return mg_has_es31() && GLES.glDispatchCompute != nullptr;
}

// glClearTexImage / glClearTexSubImage: GL_EXT_clear_texture
static bool mg_cap_clear_texture() {
    return g_gles_caps.GL_EXT_clear_texture && GLES.glClearTexImageEXT != nullptr;
}

// glClipControl: GL_EXT_clip_control
static bool mg_cap_clip_control() {
    return g_gles_caps.GL_EXT_clip_control && GLES.glClipControlEXT != nullptr;
}

// CLAMP_TO_BORDER + border color: EXT/OES_texture_border_clamp
static bool mg_cap_texture_border_clamp() {
    return (g_gles_caps.GL_EXT_texture_border_clamp ||
            g_gles_caps.GL_OES_texture_border_clamp) &&
           GLES.glSamplerParameterfv != nullptr;
}

// glTexBuffer: GL_EXT/OES_texture_buffer
static bool mg_cap_texture_buffer() {
    return (g_gles_caps.GL_EXT_texture_buffer || g_gles_caps.GL_OES_texture_buffer) &&
           GLES.glTexBufferEXT != nullptr;
}

// glProgramBinary: core + IMG_program_binary (native driver shader cache)
static bool mg_cap_program_binary() {
    return GLES.glProgramBinary != nullptr &&
           (g_gles_caps.GL_OES_get_program_binary || g_gles_caps.GL_IMG_program_binary);
}

// glBlendFuncSeparatei: GL_EXT/OES_draw_buffers_indexed
static bool mg_cap_draw_buffers_indexed() {
    return (g_gles_caps.GL_EXT_draw_buffers_indexed ||
            g_gles_caps.GL_OES_draw_buffers_indexed) &&
           GLES.glBlendFuncSeparatei != nullptr;
}

// glMinSampleShading: GL_OES_sample_shading
static bool mg_cap_sample_shading() {
    return g_gles_caps.GL_OES_sample_shading && GLES.glMinSampleShadingOES != nullptr;
}

// glGetGraphicsResetStatus: EXT/KHR_robustness
static bool mg_cap_robustness() {
    return (g_gles_caps.mg_GL_EXT_robustness) &&
           GLES.glGetGraphicsResetStatus != nullptr;
}

// GLES 3.0 core items that always hold on an ES 3.x host: FBO, samplers,
// MRT, separate shaders, program interface query, transform feedback.
static bool mg_cap_es30() { return g_gles_caps.major >= 3; }

// Multisample storage: OES_texture_storage_multisample_2d_array
static bool mg_cap_texture_multisample() {
    return g_gles_caps.GL_OES_texture_storage_multisample_2d_array &&
           GLES.glTexStorage3DMultisample != nullptr;
}

// Occlusion (boolean) query: GL_EXT_occlusion_query_boolean
static bool mg_cap_occlusion_query() {
    return g_gles_caps.GL_EXT_occlusion_query_boolean && GLES.glBeginQuery != nullptr;
}

// glMemoryBarrier + image load/store: GLES 3.1 core
static bool mg_cap_image_load_store() {
    return mg_has_es31() && GLES.glMemoryBarrier != nullptr;
}

// Tessellation / geometry: EXT/OES on this IMG driver
static bool mg_cap_tessellation() {
    return (g_gles_caps.GL_EXT_tessellation_shader ||
            g_gles_caps.GL_OES_tessellation_shader) &&
           GLES.glPatchParameteri != nullptr;
}

static bool mg_cap_geometry() {
    return (g_gles_caps.GL_EXT_geometry_shader || g_gles_caps.GL_OES_geometry_shader);
}

// The full capability table. Order matters only for readability.
static const mg_arb_entry_t mg_arb_table[] = {
    // ── Tier 1: pass-through, the Sodium hot path ─────────────────────────
    {"GL_ARB_buffer_storage",            mg_cap_buffer_storage},
    {"GL_ARB_map_buffer_range",          mg_cap_map_buffer_range},
    {"GL_ARB_sync",                      mg_cap_sync},
    {"GL_ARB_copy_buffer",               mg_cap_copy_buffer},
    {"GL_ARB_vertex_array_object",       mg_cap_vao},
    {"GL_ARB_vertex_attrib_binding",     mg_cap_attrib_binding},
    {"GL_ARB_draw_elements_base_vertex", mg_cap_draw_elements_base_vertex},
    {"GL_ARB_vertex_buffer_object",     mg_cap_es30},
    // ── Tier 1: general GL 3.x surface, all core GLES 3.0 ─────────────────
    {"GL_ARB_framebuffer_object",       mg_cap_es30},
    {"GL_ARB_sampler_objects",          mg_cap_es30},
    {"GL_ARB_draw_instanced",           mg_cap_instanced},
    {"GL_ARB_instanced_arrays",         mg_cap_instanced},
    {"GL_ARB_separate_shader_objects",  mg_cap_es30},
    {"GL_ARB_program_interface_query",  mg_cap_es30},
    {"GL_ARB_shader_bit_encoding",      mg_cap_es30},
    {"GL_ARB_explicit_attrib_location", mg_cap_es30},
    {"GL_ARB_explicit_uniform_location", mg_cap_es31},
    {"GL_ARB_arrays_of_arrays",         mg_cap_es32},
    // ── Tier 2: compute / image / misc with real equivalents ───────────────
    {"GL_ARB_shader_storage_buffer_object", mg_cap_ssbo},
    {"GL_ARB_shader_image_load_store",     mg_cap_image_load_store},
    {"GL_ARB_compute_shader",             mg_cap_compute},
    {"GL_ARB_shader_atomic_counters",     mg_cap_compute},
    {"GL_ARB_clear_texture",               mg_cap_clear_texture},
    {"GL_ARB_clip_control",               mg_cap_clip_control},
    {"GL_ARB_texture_border_clamp",       mg_cap_texture_border_clamp},
    {"GL_ARB_texture_buffer_object",      mg_cap_texture_buffer},
    {"GL_ARB_texture_multisample",        mg_cap_texture_multisample},
    {"GL_ARB_sample_shading",             mg_cap_sample_shading},
    {"GL_ARB_draw_buffers",               mg_cap_es30},
    {"GL_ARB_blend_func_extended",        mg_cap_draw_buffers_indexed},
    {"GL_ARB_robustness",                 mg_cap_robustness},
    {"GL_ARB_occlusion_query2",           mg_cap_occlusion_query},
    {"GL_ARB_tessellation_shader",        mg_cap_tessellation},
    {"GL_ARB_geometry_shader4",           mg_cap_geometry},
    {"GL_ARB_gpu_shader5",               mg_cap_es31},
    {"GL_ARB_get_program_binary",        mg_cap_program_binary},
    {"GL_KHR_debug",                     nullptr}, // handled below
    // Legacy strings the shader translator still references; harmless.
    {"GL_ARB_ES2_compatibility",         []() { return true; }},
    {"GL_ARB_ES3_compatibility",         []() { return true; }},
    {"GL_ARB_shading_language_100",      []() { return true; }},
    {"GL_ARB_fragment_program",         []() { return true; }},
    {"GL_ARB_imaging",                   []() { return true; }},
    {"GL_ARB_depth_texture",             []() { return true; }},
    {"GL_ARB_draw_buffers_blend",        mg_cap_draw_buffers_indexed},
    {"GL_EXT_vertex_array",             mg_cap_vao},
    {"GL_EXT_packed_depth_stencil",     mg_cap_es30},
    {"GL_EXT_depth_texture",            mg_cap_es30},
};

void InitGLESBaseExtensions() {
    std::vector<std::string> extensions;

    if (global_settings.hide_mg_env_level == HideMGEnvLevel::Disabled) {
        extensions.push_back("GL_MG_mobileglues");
        extensions.push_back("GL_MG_backend_string_getter_access");
        extensions.push_back("GL_MG_settings_string_dump");
    }

    // Capability-driven ARB exposure: a string is advertised only when the
    // host driver can actually serve every entry point behind it. This is what
    // lets Sodium take its native fast paths (glBufferStorage -> persistent
    // mapped staging, glMultiDrawElementsBaseVertex -> EXT) instead of
    // emulated ones, and what stops mods from entering desktop paths this
    // layer would then have to fake.
    for (const auto& entry : mg_arb_table) {
        if (entry.supported == nullptr) continue; // reserved rows
        if (entry.supported()) {
            extensions.push_back(entry.arb_name);
        } else {
            LOG_D("extensions: %s not advertised (host capability missing)", entry.arb_name);
        }
    }

    // GL_KHR_debug is only honest when the host exposes it; the log macros do
    // not depend on it.
    if (g_gles_caps.mg_GL_KHR_debug) {
        extensions.push_back("GL_KHR_debug");
    }

    // Emulated extensions for Iris / Vanilla Minecraft compatibility
    extensions.push_back("GL_EXT_texture_filter_anisotropic");
    extensions.push_back("GL_KHR_parallel_shader_compile");

    if (global_settings.hide_mg_env_level >= HideMGEnvLevel::Level1) {
        for (int i = (int)extensions.size() - 1; i > 0; --i) {
            int j = rand() % (i + 1);
            std::swap(extensions[i], extensions[j]);
        }
    }

    es_ext.clear();
    for (const auto& ext : extensions) {
        es_ext += ext;
        es_ext += " ";
    }
}

void AppendExtension(const char* ext) {
    es_ext += ext;
    es_ext += ' ';
}

std::string getBeforeThirdSpace(const std::string& str) {
    int spaceCount = 0;
    size_t endPos = 0;
    for (size_t i = 0; i < str.length(); ++i) {
        if (str[i] == ' ') {
            spaceCount++;
            if (spaceCount == 3) {
                endPos = i;
                break;
            }
        }
        if (spaceCount < 3) endPos = str.length();
    }

    return str.substr(0, endPos);
}

std::string getGpuName() {
    std::string gpuName = std::string((char*)GLES.glGetString(GL_RENDERER));

    if (gpuName.empty()) {
        return "<unknown>";
    }

    // MetalANGLE, ANGLE (Metal Renderer: Apple * GPU)
    if (gpuName.find("MetalANGLE, ANGLE") != std::string::npos) {
        if (gpuName.length() < 25) {
            return gpuName;
        }

        std::string gpu = gpuName.substr(23, gpuName.length() - 24);
        std::string formattedGpuName = gpu + " | MetalANGLE | Metal";
        return formattedGpuName;
    }

    // Vulkan ANGLE
    if (gpuName.rfind("ANGLE", 0) == 0 && gpuName.find("Vulkan") != std::string::npos) {
        size_t firstParen = gpuName.find('(');
        size_t secondParen = gpuName.find('(', firstParen + 1);
        size_t lastParen = gpuName.rfind('(');

        std::string gpu = gpuName.substr(secondParen + 1, lastParen - secondParen - 2);

        size_t vulkanStart = gpuName.find("Vulkan ");
        size_t vulkanEnd = gpuName.find(' ', vulkanStart + 7);
        std::string vulkanVersion = gpuName.substr(vulkanStart + 7, vulkanEnd - (vulkanStart + 7));

        std::string formattedGpuName = gpu + " | ANGLE | Vulkan " + vulkanVersion;

        return formattedGpuName;
    }

    return gpuName;
}

void set_es_version() {
    std::string ESVersionStr = getBeforeThirdSpace(std::string((const char*)GLES.glGetString(GL_VERSION)));
    int major, minor;

    if (sscanf(ESVersionStr.c_str(), "OpenGL ES %d.%d", &major, &minor) == 2) {
        hardware->es_version = major * 100 + minor * 10;
    } else {
        hardware->es_version = 300;
    }
    LOG_I("OpenGL ES Version: %s (%d)", ESVersionStr.c_str(), hardware->es_version)
    if (hardware->es_version < 300) {
        LOG_I("OpenGL ES version is lower than 3.0! This version is not supported!")
    }
}

std::string getGLESName() {
    return getBeforeThirdSpace(std::string((char*)GLES.glGetString(GL_VERSION)));
}

static std::string rendererString;
static std::string vendorString;
static std::string versionString;
const GLubyte* glGetString(GLenum name) {
    LOG()
    LOG_D("glGetString, %s", glEnumToString(name))
    switch (name) {
    case GL_VENDOR: {
        if (vendorString.empty()) {
            if (global_settings.hide_mg_env_level == HideMGEnvLevel::Disabled) {
                std::string vendor = "Swung0x48, BZLZHH, Tungsten";
                vendorString = vendor;
            } else {
                const char choices[] = "AINM";
                vendorString = choices[rand() % 4];

                RandomStringOptions randStrOpts;
                randStrOpts.includeDigits = false;
                randStrOpts.minLength = 3;
                randStrOpts.maxLength = 8;
                randStrOpts.includeLowercase = false;
                randStrOpts.includeUppercase = false;
                randStrOpts.customChars = "IMenaNtMseAVlD";
                vendorString += GenerateRandomString(randStrOpts);
            }
        }
        return (const GLubyte*)vendorString.c_str();
    }
    case GL_VERSION: {
        return (const GLubyte*)"4.4.0 Core Profile";
    }
    case GL_RENDERER: {
        if (rendererString == std::string("")) {
            if (global_settings.hide_mg_env_level == HideMGEnvLevel::Disabled) {
                std::string gpuName = getGpuName();
                std::string glesName = getGLESName();
                rendererString = std::string(gpuName) + " | " + std::string(glesName);
            } else {
                const char choices[] = "AINM";
                rendererString = choices[rand() % 4];

                RandomStringOptions randStrOpts;
                randStrOpts.includeDigits = true;
                randStrOpts.minLength = 6;
                randStrOpts.maxLength = 12;
                randStrOpts.includeLowercase = false;
                randStrOpts.includeUppercase = false;
                randStrOpts.customChars = "IRMenaNtfsoerAceVlDG";
                rendererString += GenerateRandomString(randStrOpts);

                int junkInfoTime = rand() % 3 + 1;
                for (int i = 0; i < junkInfoTime; ++i) {
                    rendererString += " ";
                    RandomStringOptions randStrOpts2;
                    randStrOpts2.minLength = 3;
                    randStrOpts2.maxLength = 6;
                    randStrOpts2.includeLowercase = false;
                    randStrOpts2.includeUppercase = false;
                    randStrOpts2.customChars = "IRenaNtfsoerAcieVDcsG";
                    rendererString += GenerateRandomString(randStrOpts2);
                }
            }
        }
        return (const GLubyte*)rendererString.c_str();
    }
    case GL_SHADING_LANGUAGE_VERSION: {
        static std::string shadingLangString;

        if (shadingLangString.empty()) {
            std::string baseVer;
            if (hardware->es_version < 310) {
                baseVer = "4.00";
            } else {
                baseVer = "4.60";
            }

            if (global_settings.hide_mg_env_level >= HideMGEnvLevel::Level1) {
                shadingLangString = baseVer;

                int junkCount = rand() % 2 + 1;
                for (int i = 0; i < junkCount; ++i) {
                    shadingLangString += " ";
                    RandomStringOptions junkOpts;
                    junkOpts.minLength = 2;
                    junkOpts.maxLength = 5;
                    junkOpts.includeLowercase = false;
                    junkOpts.includeUppercase = false;
                    junkOpts.customChars = "IAneNDtVsaMIl";
                    shadingLangString += GenerateRandomString(junkOpts);
                }
            } else {
                shadingLangString = baseVer + " MobileGlues with glslang and SPIRV-Cross";
            }
        }

        return reinterpret_cast<const GLubyte*>(shadingLangString.c_str());
    }
    case GL_EXTENSIONS: {
        // GetExtensionsList() returns by value, so assigning it on every call freed the buffer handed
        // out last time and dangled every pointer already returned. Build it once, like the strings above.
        static std::string extensionsString;

        if (extensionsString.empty()) {
            extensionsString = GetExtensionsList();
        }

        return (const GLubyte*)extensionsString.c_str();
    }
    case GL_SETTINGS_MG: {
        if (global_settings.hide_mg_env_level >= HideMGEnvLevel::Level1) return GLES.glGetString(name);

        static char* settings_string = nullptr;
        std::string tmp = dump_settings_string("  ");
        settings_string = strdup(tmp.c_str());
        return reinterpret_cast<const GLubyte*>(settings_string);
    }
    case GL_VERSION + GL_BACKEND_GETTER_MG:
    case GL_VENDOR + GL_BACKEND_GETTER_MG:
    case GL_RENDERER + GL_BACKEND_GETTER_MG:
    case GL_EXTENSIONS + GL_BACKEND_GETTER_MG:
    case GL_SHADING_LANGUAGE_VERSION + GL_BACKEND_GETTER_MG:
        if (global_settings.hide_mg_env_level == HideMGEnvLevel::Disabled)
            return GLES.glGetString(name - GL_BACKEND_GETTER_MG);
        else
            return GLES.glGetString(name);
    default:
        return GLES.glGetString(name);
    }
}

const GLubyte* glGetStringi(GLenum name, GLuint index) {
    LOG()
    if (name == GL_EXTENSIONS + GL_BACKEND_GETTER_MG && global_settings.hide_mg_env_level == HideMGEnvLevel::Disabled) {
        return GLES.glGetStringi(name - GL_BACKEND_GETTER_MG, index);
    }

    typedef struct {
        GLenum name;
        const char** parts;
        GLuint count;
    } StringCache;
    static StringCache caches[] = {{GL_EXTENSIONS, nullptr, 0},
                                   {GL_VENDOR, nullptr, 0},
                                   {GL_VERSION, nullptr, 0},
                                   {GL_SHADING_LANGUAGE_VERSION, nullptr, 0}};
    static int initialized = 0;
    if (!initialized) {
        for (auto& cache : caches) {
            GLenum target = cache.name;
            const GLubyte* str = nullptr;
            const char* delimiter = " ";

            switch (target) {
            case GL_VENDOR:
                str = glGetString(GL_VENDOR);
                delimiter = ", ";
                break;
            case GL_VERSION:
                str = glGetString(GL_VERSION);
                delimiter = " .";
                break;
            case GL_SHADING_LANGUAGE_VERSION:
                str = glGetString(GL_SHADING_LANGUAGE_VERSION);
                break;
            case GL_EXTENSIONS:
                str = glGetString(GL_EXTENSIONS);
                break;
            default:
                return GLES.glGetStringi(name, index);
            }

            if (!str) continue;

            std::string copy_str((const char*)str);
            std::string token_str;
            size_t start = 0;
            size_t end = copy_str.find_first_of(delimiter);

            while (end != std::string::npos) {
                token_str = copy_str.substr(start, end - start);
                cache.parts = (const char**)realloc(cache.parts, (cache.count + 1) * sizeof(char*));
                cache.parts[cache.count++] = strdup(token_str.c_str());
                start = end + 1;
                end = copy_str.find_first_of(delimiter, start);
            }
            token_str = copy_str.substr(start); // Get the last token
            cache.parts = (const char**)realloc(cache.parts, (cache.count + 1) * sizeof(char*));
            cache.parts[cache.count++] = strdup(token_str.c_str());
        }
        initialized = 1;
    }

    for (auto& cache : caches) {
        if (cache.name == name) {
            if (index >= cache.count) {
                return nullptr;
            }
            return (const GLubyte*)cache.parts[index];
        }
    }

    return nullptr;
}

void glGetQueryObjectiv(GLuint id, GLenum pname, GLint* params) {
    LOG()
    if (GLES.glGetQueryObjectivEXT) {
        GLES.glGetQueryObjectivEXT(id, pname, params);
        CHECK_GL_ERROR
    }
}

void glGetQueryObjecti64v(GLuint id, GLenum pname, GLint64* params) {
    LOG()
    if (GLES.glGetQueryObjecti64vEXT) {
        GLES.glGetQueryObjecti64vEXT(id, pname, params);
        CHECK_GL_ERROR
    }
}
