// MobileGlues - config/configdraw.cpp
// Draw modes configuration system with strict mode selection and no silent fallback.
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1
// SPDX-License-Identifier: LGPL-2.1-only

#include "configdraw.h"
#include "config.h"
#include "cJSON.h"
#include "../gl/log.h"

#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace {

static std::string mode_to_string(DrawModeType type) {
    switch (type) {
    case DrawModeType::DrawElements: return "drawElements";
    case DrawModeType::DrawElementsBaseVertex: return "drawElementsBaseVertex";
    case DrawModeType::MultiDrawIndirect: return "multiDrawIndirect";
    case DrawModeType::MultiDrawMultiIndirect: return "multiDrawMultiIndirect";
    case DrawModeType::MultiDrawBaseVertex: return "multiDrawBaseVertex";
    case DrawModeType::MultiDrawArrays: return "multiDrawArrays";
    case DrawModeType::ComputeShaderFusion: return "computeShaderFusion";
    case DrawModeType::FastPath: return "fastPath";
    case DrawModeType::FunnelEngine: return "funnelEngine";
    case DrawModeType::SodiumTierS: return "sodiumTierS";
    case DrawModeType::SodiumTierU: return "sodiumTierU";
    default: return "unknown";
    }
}

static DrawModeType string_to_mode(const std::string& name) {
    if (name == "drawElements") return DrawModeType::DrawElements;
    if (name == "drawElementsBaseVertex") return DrawModeType::DrawElementsBaseVertex;
    if (name == "multiDrawIndirect") return DrawModeType::MultiDrawIndirect;
    if (name == "multiDrawMultiIndirect") return DrawModeType::MultiDrawMultiIndirect;
    if (name == "multiDrawBaseVertex") return DrawModeType::MultiDrawBaseVertex;
    if (name == "multiDrawArrays") return DrawModeType::MultiDrawArrays;
    if (name == "computeShaderFusion") return DrawModeType::ComputeShaderFusion;
    if (name == "fastPath") return DrawModeType::FastPath;
    if (name == "funnelEngine") return DrawModeType::FunnelEngine;
    if (name == "sodiumTierS") return DrawModeType::SodiumTierS;
    if (name == "sodiumTierU") return DrawModeType::SodiumTierU;
    return DrawModeType::DrawElements;
}

static std::string default_config_path() {
    const char* dir = mg_directory_path ? mg_directory_path : "/sdcard/MG";
    return std::string(dir) + "/configdraw.json";
}

static bool path_exists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

static void ensure_parent_dir(const std::string& path) {
    const std::string::size_type slash = path.find_last_of('/');
    if (slash == std::string::npos) return;
    const std::string dir = path.substr(0, slash);
    if (dir.empty()) return;
    if (!path_exists(dir)) {
        mkdir(dir.c_str(), 0755);
    }
}

static bool get_bool(cJSON* obj, const char* name, bool default_value) {
    if (!obj) return default_value;
    cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, name);
    if (!item || !cJSON_IsBool(item)) return default_value;
    return cJSON_IsTrue(item);
}

static int get_int(cJSON* obj, const char* name, int default_value) {
    if (!obj) return default_value;
    cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, name);
    if (!item || !cJSON_IsNumber(item)) return default_value;
    return item->valueint;
}

static const char* get_string(cJSON* obj, const char* name, const char* default_value) {
    if (!obj) return default_value;
    cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, name);
    if (!item || !cJSON_IsString(item)) return default_value;
    return item->valuestring;
}

} // namespace

DrawModeConfigManager* g_drawModeConfig = nullptr;

DrawModeConfigManager::DrawModeConfigManager() : sodiumDetected(false), activePrimaryMode(DrawModeType::DrawElements) {
    initializeDefaults();
}

DrawModeConfigManager::~DrawModeConfigManager() = default;

void DrawModeConfigManager::initializeDefaults() {
    modes.clear();

    auto add = [&](DrawModeType type, const std::string& name, bool enabled,
                   const std::string& description, bool sodiumOnly, bool requiresExtension,
                   const std::string& requiredExtension, bool requiresComputeShader,
                   bool isOptimizationLayer) {
        DrawModeConfig cfg;
        cfg.type = type;
        cfg.name = name;
        cfg.enabled = enabled;
        cfg.params.useRestartEmulation = false;
        cfg.params.maxBatchSize = 0;
        cfg.params.validateOnEveryCall = false;
        cfg.meta.description = description;
        cfg.meta.sodiumOnly = sodiumOnly;
        cfg.meta.requiresExtension = requiresExtension;
        cfg.meta.requiredExtension = requiredExtension;
        cfg.meta.requiresComputeShader = requiresComputeShader;
        cfg.meta.isOptimizationLayer = isOptimizationLayer;
        modes[type] = cfg;
    };

    add(DrawModeType::DrawElements, "drawElements", true,
        "Basic glDrawElements per-draw loop", false, false, "", false, false);
    add(DrawModeType::DrawElementsBaseVertex, "drawElementsBaseVertex", false,
        "glDrawElementsBaseVertex per-draw", false, true,
        "GL_OES_draw_elements_base_vertex | GL_EXT_draw_elements_base_vertex", false, false);
    add(DrawModeType::MultiDrawIndirect, "multiDrawIndirect", false,
        "glDrawElementsIndirect per-draw", false, false, "", false, false);
    add(DrawModeType::MultiDrawMultiIndirect, "multiDrawMultiIndirect", false,
        "glMultiDrawElementsIndirectEXT", false, true, "GL_EXT_multi_draw_indirect", false, false);
    add(DrawModeType::MultiDrawBaseVertex, "multiDrawBaseVertex", false,
        "glMultiDrawElementsBaseVertexEXT", false, true,
        "GL_OES_draw_elements_base_vertex | GL_EXT_draw_elements_base_vertex", false, false);
    add(DrawModeType::MultiDrawArrays, "multiDrawArrays", false,
        "glMultiDrawArraysEXT", false, true, "GL_EXT_multi_draw_arrays | GL_ANGLE_multi_draw", false, false);
    add(DrawModeType::ComputeShaderFusion, "computeShaderFusion", false,
        "Compute shader fusion", false, false, "", true, false);
    add(DrawModeType::FastPath, "fastPath", true,
        "Generic fast-path optimization layer", false, false, "", false, true);
    add(DrawModeType::FunnelEngine, "funnelEngine", false,
        "Sodium funnel optimization layer", false, false, "", false, true);
    add(DrawModeType::SodiumTierS, "sodiumTierS", false,
        "Sodium shared IBO path", true, true,
        "GL_OES_draw_elements_base_vertex | GL_EXT_draw_elements_base_vertex", false, false);
    add(DrawModeType::SodiumTierU, "sodiumTierU", false,
        "Sodium micro-batch unroll", true, false, "", false, false);

    activePrimaryMode = DrawModeType::DrawElements;
    lastError.clear();
}

const DrawModeConfig* DrawModeConfigManager::getMode(DrawModeType type) const {
    auto it = modes.find(type);
    if (it == modes.end()) return nullptr;
    return &it->second;
}

const DrawModeConfig* DrawModeConfigManager::getModeByName(const std::string& name) const {
    for (const auto& it : modes) {
        if (it.second.name == name) return &it.second;
    }
    return nullptr;
}

void DrawModeConfigManager::enableMode(DrawModeType type) {
    auto it = modes.find(type);
    if (it == modes.end()) return;
    it->second.enabled = true;
}

void DrawModeConfigManager::disableMode(DrawModeType type) {
    auto it = modes.find(type);
    if (it == modes.end()) return;
    it->second.enabled = false;
}

bool DrawModeConfigManager::isModeEnabled(DrawModeType type) const {
    const DrawModeConfig* cfg = getMode(type);
    if (!cfg) return false;
    return cfg->enabled;
}

void DrawModeConfigManager::setSodiumDetected(bool detected) {
    sodiumDetected = detected;
    if (detected) {
        enableSodiumExclusiveMode();
    }
}

bool DrawModeConfigManager::isSodiumDetected() const {
    return sodiumDetected;
}

std::vector<DrawModeType> DrawModeConfigManager::getEnabledModes() const {
    std::vector<DrawModeType> result;
    for (const auto& it : modes) {
        if (it.second.enabled) result.push_back(it.first);
    }
    return result;
}

DrawModeType DrawModeConfigManager::getActiveMode() const {
    return activePrimaryMode;
}

DrawModeType DrawModeConfigManager::getActiveModeForEntryPoint(const std::string& entryPoint) const {
    (void)entryPoint;
    return activePrimaryMode;
}

void DrawModeConfigManager::enableSodiumExclusiveMode() {
    for (auto& it : modes) {
        it.second.enabled = false;
    }
    auto* funnel = getMode(DrawModeType::FunnelEngine);
    if (funnel) funnel->enabled = true;
    auto* fast = getMode(DrawModeType::FastPath);
    if (fast) fast->enabled = true;
    activePrimaryMode = DrawModeType::FunnelEngine;
}

bool DrawModeConfigManager::validate() const {
    if (sodiumDetected) {
        return getMode(DrawModeType::FunnelEngine) && getMode(DrawModeType::FunnelEngine)->enabled;
    }

    const DrawModeConfig* primary = getMode(activePrimaryMode);
    if (primary && primary->enabled) {
        return true;
    }
    return true;
}

std::string DrawModeConfigManager::getLastError() const {
    return lastError;
}

std::string DrawModeConfigManager::dumpConfig() const {
    std::stringstream ss;
    ss << "sodiumDetected=" << (sodiumDetected ? "true" : "false") << "\n";
    ss << "activePrimaryMode=" << mode_to_string(activePrimaryMode) << "\n";
    for (const auto& it : modes) {
        ss << it.second.name << "=" << (it.second.enabled ? "enabled" : "disabled") << "\n";
    }
    return ss.str();
}

bool DrawModeConfigManager::loadFromJSON(const std::string& configPath) {
    const std::string path = configPath.empty() ? default_config_path() : configPath;
    ensure_parent_dir(path);

    if (!path_exists(path)) {
        return saveToJSON(path);
    }

    std::ifstream in(path.c_str(), std::ios::binary);
    if (!in.is_open()) {
        lastError = "Unable to open draw config file: " + path;
        LOG_E("draw config: unable to open %s", path.c_str())
        return false;
    }

    std::ostringstream buffer;
    buffer << in.rdbuf();
    const std::string jsonText = buffer.str();

    cJSON* root = cJSON_Parse(jsonText.c_str());
    if (!root) {
        lastError = "Invalid JSON in draw config";
        LOG_E("draw config: invalid JSON in %s", path.c_str())
        return false;
    }

    cJSON* sodium = cJSON_GetObjectItemCaseSensitive(root, "sodiumDetected");
    if (cJSON_IsBool(sodium)) {
        sodiumDetected = cJSON_IsTrue(sodium);
    }

    cJSON* modesNode = cJSON_GetObjectItemCaseSensitive(root, "drawModes");
    if (modesNode && cJSON_IsObject(modesNode)) {
        cJSON* child = nullptr;
        cJSON_ArrayForEach(child, modesNode) {
            const std::string name = child->string ? child->string : "";
            const DrawModeType type = string_to_mode(name);
            auto it = modes.find(type);
            if (it == modes.end()) continue;

            DrawModeConfig& cfg = it->second;
            cfg.enabled = get_bool(child, "enabled", cfg.enabled);
            cfg.meta.sodiumOnly = get_bool(child, "sodiumOnly", cfg.meta.sodiumOnly);
            cfg.meta.requiresExtension = get_bool(child, "requiresExtension", cfg.meta.requiresExtension);
            cfg.meta.requiredExtension = get_string(child, "requiredExtension", cfg.meta.requiredExtension.c_str());
            cfg.meta.requiresComputeShader = get_bool(child, "requiresComputeShader", cfg.meta.requiresComputeShader);
            cfg.meta.isOptimizationLayer = get_bool(child, "isOptimizationLayer", cfg.meta.isOptimizationLayer);
            const char* desc = get_string(child, "description", cfg.meta.description.c_str());
            cfg.meta.description = desc ? desc : cfg.meta.description;

            cJSON* params = cJSON_GetObjectItemCaseSensitive(child, "params");
            if (params) {
                cfg.params.useRestartEmulation = get_bool(params, "useRestartEmulation", cfg.params.useRestartEmulation);
                cfg.params.maxBatchSize = get_int(params, "maxBatchSize", cfg.params.maxBatchSize);
                cfg.params.validateOnEveryCall = get_bool(params, "validateOnEveryCall", cfg.params.validateOnEveryCall);
            }
        }
    }

    cJSON* strategy = cJSON_GetObjectItemCaseSensitive(root, "modeSelectionStrategy");
    if (strategy && cJSON_IsObject(strategy)) {
        const char* primaryMode = get_string(strategy, "primaryMode", mode_to_string(activePrimaryMode).c_str());
        activePrimaryMode = string_to_mode(primaryMode ? primaryMode : mode_to_string(activePrimaryMode));
    }

    cJSON_Delete(root);

    if (sodiumDetected) {
        enableSodiumExclusiveMode();
    }

    validate();
    return true;
}

bool DrawModeConfigManager::saveToJSON(const std::string& configPath) {
    const std::string path = configPath.empty() ? default_config_path() : configPath;
    ensure_parent_dir(path);

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "version", "1.0");
    cJSON_AddStringToObject(root, "comment", "MobileGlues Draw Modes Configuration - strict, no fallback");
    cJSON_AddBoolToObject(root, "sodiumDetected", sodiumDetected);

    cJSON* drawModes = cJSON_CreateObject();
    for (const auto& it : modes) {
        const DrawModeConfig& cfg = it.second;
        cJSON* mode = cJSON_CreateObject();
        cJSON_AddBoolToObject(mode, "enabled", cfg.enabled);
        cJSON_AddStringToObject(mode, "description", cfg.meta.description.c_str());
        cJSON_AddBoolToObject(mode, "sodiumOnly", cfg.meta.sodiumOnly);
        cJSON_AddBoolToObject(mode, "requiresExtension", cfg.meta.requiresExtension);
        cJSON_AddStringToObject(mode, "requiredExtension", cfg.meta.requiredExtension.c_str());
        cJSON_AddBoolToObject(mode, "requiresComputeShader", cfg.meta.requiresComputeShader);
        cJSON_AddBoolToObject(mode, "isOptimizationLayer", cfg.meta.isOptimizationLayer);

        cJSON* params = cJSON_CreateObject();
        cJSON_AddBoolToObject(params, "useRestartEmulation", cfg.params.useRestartEmulation);
        cJSON_AddNumberToObject(params, "maxBatchSize", cfg.params.maxBatchSize);
        cJSON_AddBoolToObject(params, "validateOnEveryCall", cfg.params.validateOnEveryCall);
        cJSON_AddItemToObject(mode, "params", params);

        cJSON_AddItemToObject(drawModes, cfg.name.c_str(), mode);
    }
    cJSON_AddItemToObject(root, "drawModes", drawModes);

    cJSON* strategy = cJSON_CreateObject();
    cJSON_AddStringToObject(strategy, "primaryMode", mode_to_string(activePrimaryMode).c_str());
    cJSON_AddBoolToObject(strategy, "strictMode", true);
    cJSON_AddBoolToObject(strategy, "enableOptimizationLayers", true);
    cJSON_AddItemToObject(root, "modeSelectionStrategy", strategy);

    char* json = cJSON_Print(root);
    if (!json) {
        cJSON_Delete(root);
        lastError = "Unable to generate JSON for draw config";
        return false;
    }

    std::ofstream out(path.c_str(), std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        cJSON_free(json);
        cJSON_Delete(root);
        lastError = "Unable to write draw config: " + path;
        LOG_E("draw config: unable to write %s", path.c_str())
        return false;
    }

    out << json;
    out.flush();
    out.close();

    cJSON_free(json);
    cJSON_Delete(root);
    return true;
}

// Global instance is created in settings.cpp after init config paths.
