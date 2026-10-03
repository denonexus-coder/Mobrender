// MobileGlues - config/configdraw.h
// Draw modes configuration system - strict, no fallback
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1
// SPDX-License-Identifier: LGPL-2.1-only

#pragma once

#include <string>
#include <vector>
#include <map>
#include <cstdint>

// ============================================================================
// Draw Mode Types
// ============================================================================

enum class DrawModeType {
    DrawElements,              // glDrawElements - basic draw loop
    DrawElementsBaseVertex,    // glDrawElementsBaseVertex - with base vertex offset
    MultiDrawIndirect,         // glDrawElementsIndirect per-draw
    MultiDrawMultiIndirect,    // glMultiDrawElementsIndirectEXT - single GPU call
    MultiDrawBaseVertex,       // glMultiDrawElementsBaseVertexEXT - EXT native
    MultiDrawArrays,           // glMultiDrawArraysEXT
    ComputeShaderFusion,       // Compute shader index fusion
    FastPath,                  // Frustum culling + contiguous merge
    FunnelEngine,              // Sodium terrain: merge contiguous basevertex runs
    SodiumTierS,               // Sodium TIER S: shared-IBO detector
    SodiumTierU,               // Sodium TIER U: micro-batch unroll
};

struct DrawModeConfig {
    DrawModeType type;
    std::string name;
    bool enabled;
    
    struct {
        bool useRestartEmulation;
        int maxBatchSize;
        bool validateOnEveryCall;
    } params;
    
    struct {
        std::string description;
        bool sodiumOnly;
        bool requiresExtension;
        std::string requiredExtension;
        bool requiresComputeShader;
        bool isOptimizationLayer;
    } meta;
};

// ============================================================================
// Configuration Manager
// ============================================================================

class DrawModeConfigManager {
public:
    DrawModeConfigManager();
    ~DrawModeConfigManager();
    
    bool loadFromJSON(const std::string& configPath);
    bool saveToJSON(const std::string& configPath);
    
    const DrawModeConfig* getMode(DrawModeType type) const;
    const DrawModeConfig* getModeByName(const std::string& name) const;
    
    void enableMode(DrawModeType type);
    void disableMode(DrawModeType type);
    bool isModeEnabled(DrawModeType type) const;
    
    void setSodiumDetected(bool detected);
    bool isSodiumDetected() const;
    
    std::vector<DrawModeType> getEnabledModes() const;
    DrawModeType getActiveMode() const;
    DrawModeType getActiveModeForEntryPoint(const std::string& entryPoint) const;
    
    bool validate() const;
    std::string getLastError() const;
    std::string dumpConfig() const;
    
private:
    std::map<DrawModeType, DrawModeConfig> modes;
    std::string lastError;
    bool sodiumDetected;
    DrawModeType activePrimaryMode;
    
    void initializeDefaults();
    void enableSodiumExclusiveMode();
};

extern DrawModeConfigManager* g_drawModeConfig;
