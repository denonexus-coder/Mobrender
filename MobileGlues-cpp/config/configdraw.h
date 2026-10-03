// MobileGlues - config/configdraw.h
// Draw modes configuration system
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1
// SPDX-License-Identifier: LGPL-2.1-only

#pragma once

#include <string>
#include <vector>
#include <map>

// ============================================================================
// Draw Mode Configuration System
// NO FALLBACKS - Each mode is individually enabled/disabled
// Direct 1:1 mapping between config and runtime behavior
// ============================================================================

enum class DrawModeType {
    // Core single-draw modes
    DrawElements,              // glDrawElements - basic draw loop
    DrawElementsBaseVertex,    // glDrawElementsBaseVertex - with base vertex offset
    
    // Multi-draw modes (batched)
    MultiDrawIndirect,         // glDrawElementsIndirect per-draw
    MultiDrawMultiIndirect,    // glMultiDrawElementsIndirectEXT - single GPU call
    MultiDrawBaseVertex,       // glMultiDrawElementsBaseVertexEXT - EXT native
    MultiDrawArrays,           // glMultiDrawArraysEXT
    
    // Compute-based
    ComputeShaderFusion,       // Compute shader index fusion
    
    // Optimization layers (can be active on top of others)
    FastPath,                  // Frustum culling + contiguous merge
    FunnelEngine,              // Sodium terrain: merge contiguous basevertex runs
    SodiumTierS,               // Sodium TIER S: shared-IBO detector (Sodium only!)
    SodiumTierU,               // Sodium TIER U: micro-batch unroll (Sodium only!)
};

struct DrawModeConfig {
    DrawModeType type;
    std::string name;           // e.g. "DrawElements", "MultiDrawIndirect"
    bool enabled;               // Master enable/disable
    
    // Mode-specific parameters
    struct {
        bool useRestartEmulation;      // GL_PRIMITIVE_RESTART handling
        int maxBatchSize;              // Limit for this mode
        bool validateOnEveryCall;      // Extra GL error checks
    } params;
    
    // Metadata
    struct {
        std::string description;       // Human-readable description
        bool sodiumOnly;               // Only works with Sodium mod
        bool requiresExtension;        // Needs specific GL extension
        std::string requiredExtension; // Which extension (if any)
        bool requiresComputeShader;    // Needs GLES 3.1+ compute
        bool isOptimizationLayer;      // Stacks on top of other modes
    } meta;
};

// ============================================================================
// Configuration Manager - Single source of truth
// ============================================================================

class DrawModeConfigManager {
public:
    DrawModeConfigManager();
    ~DrawModeConfigManager();
    
    // Initialize from JSON file
    bool loadFromJSON(const std::string& configPath);
    
    // Save current configuration to JSON
    bool saveToJSON(const std::string& configPath);
    
    // Get mode configuration
    const DrawModeConfig* getMode(DrawModeType type) const;
    const DrawModeConfig* getModeByName(const std::string& name) const;
    
    // Enable/disable modes
    void enableMode(DrawModeType type);
    void disableMode(DrawModeType type);
    bool isModeEnabled(DrawModeType type) const;
    
    // Sodium detection - auto-activates FunnelEngine and disables others
    void setSodiumDetected(bool detected);
    bool isSodiumDetected() const;
    
    // Get all enabled modes in priority order
    std::vector<DrawModeType> getEnabledModes() const;
    
    // Get the active mode for this frame (only ONE primary mode active)
    DrawModeType getActiveMode() const;
    
    // Validation
    bool validate() const;
    std::string getLastError() const;
    
private:
    std::map<DrawModeType, DrawModeConfig> modes;
    std::string lastError;
    bool sodiumDetected;
    DrawModeType activePrimaryMode;
    
    // Initialize default configurations
    void initializeDefaults();
    
    // Set Sodium exclusive mode
    void enableSodiumExclusiveMode();
};

// Global configuration manager instance
extern DrawModeConfigManager* g_drawModeConfig;

// ============================================================================
// JSON Schema (configdraw.json structure)
// ============================================================================

/*
{
  "version": "1.0",
  "comment": "MobileGlues Draw Modes Configuration - NO FALLBACKS, DIRECT CONTROL",
  
  "sodiumDetected": false,
  "comment_sodium": "When true, FunnelEngine activates exclusively, all others disabled",
  
  "drawModes": {
    "drawElements": {
      "enabled": true,
      "description": "Basic glDrawElements per-draw loop - ALWAYS AVAILABLE FALLBACK",
      "sodiumOnly": false,
      "requiresExtension": false,
      "requiresComputeShader": false,
      "isOptimizationLayer": false,
      "params": {
        "useRestartEmulation": true,
        "maxBatchSize": 0,
        "validateOnEveryCall": false
      }
    },
    
    "drawElementsBaseVertex": {
      "enabled": true,
      "description": "glDrawElementsBaseVertex per-draw - needs GL_OES/EXT_draw_elements_base_vertex",
      "sodiumOnly": false,
      "requiresExtension": true,
      "requiredExtension": "GL_OES_draw_elements_base_vertex | GL_EXT_draw_elements_base_vertex",
      "requiresComputeShader": false,
      "isOptimizationLayer": false,
      "params": {
        "useRestartEmulation": true,
        "maxBatchSize": 0,
        "validateOnEveryCall": false
      }
    },
    
    "multiDrawIndirect": {
      "enabled": false,
      "description": "glDrawElementsIndirect per-draw - GPU command buffer, needs ES 3.1",
      "sodiumOnly": false,
      "requiresExtension": false,
      "requiresComputeShader": false,
      "isOptimizationLayer": false,
      "params": {
        "useRestartEmulation": false,
        "maxBatchSize": 4096,
        "validateOnEveryCall": false
      }
    },
    
    "multiDrawMultiIndirect": {
      "enabled": false,
      "description": "glMultiDrawElementsIndirectEXT - single driver call, batched",
      "sodiumOnly": false,
      "requiresExtension": true,
      "requiredExtension": "GL_EXT_multi_draw_indirect",
      "requiresComputeShader": false,
      "isOptimizationLayer": false,
      "params": {
        "useRestartEmulation": false,
        "maxBatchSize": 4096,
        "validateOnEveryCall": false
      }
    },
    
    "multiDrawBaseVertex": {
      "enabled": false,
      "description": "glMultiDrawElementsBaseVertexEXT - EXT native form, most optimized",
      "sodiumOnly": false,
      "requiresExtension": true,
      "requiredExtension": "GL_EXT_draw_elements_base_vertex | GL_OES_draw_elements_base_vertex",
      "requiresComputeShader": false,
      "isOptimizationLayer": false,
      "params": {
        "useRestartEmulation": true,
        "maxBatchSize": 4096,
        "validateOnEveryCall": false
      }
    },
    
    "multiDrawArrays": {
      "enabled": false,
      "description": "glMultiDrawArraysEXT - for array-based rendering",
      "sodiumOnly": false,
      "requiresExtension": true,
      "requiredExtension": "GL_EXT_multi_draw_arrays | GL_ANGLE_multi_draw",
      "requiresComputeShader": false,
      "isOptimizationLayer": false,
      "params": {
        "useRestartEmulation": false,
        "maxBatchSize": 4096,
        "validateOnEveryCall": false
      }
    },
    
    "computeShaderFusion": {
      "enabled": false,
      "description": "Fuse all sub-draws into single index stream via compute shader",
      "sodiumOnly": false,
      "requiresExtension": false,
      "requiresComputeShader": true,
      "isOptimizationLayer": false,
      "params": {
        "useRestartEmulation": false,
        "maxBatchSize": 0,
        "validateOnEveryCall": false
      }
    },
    
    "fastPath": {
      "enabled": true,
      "description": "Generic fast path - frustum culling + contiguous merge",
      "sodiumOnly": false,
      "requiresExtension": false,
      "requiresComputeShader": false,
      "isOptimizationLayer": true,
      "comment": "Can layer on top of any primary mode",
      "params": {
        "useRestartEmulation": false,
        "maxBatchSize": 0,
        "validateOnEveryCall": false
      }
    },
    
    "funnelEngine": {
      "enabled": true,
      "description": "Sodium terrain: merge contiguous basevertex runs (7.2x submission, 1.26x FPS on GE8320)",
      "sodiumOnly": false,
      "requiresExtension": false,
      "requiresComputeShader": false,
      "isOptimizationLayer": true,
      "comment": "Auto-activates when Sodium detected, disables all primary modes except FastPath",
      "params": {
        "useRestartEmulation": true,
        "maxBatchSize": 0,
        "validateOnEveryCall": false
      }
    },
    
    "sodiumTierS": {
      "enabled": true,
      "description": "Sodium TIER S - Shared-IBO detector (single glMultiDrawElementsBaseVertexEXT call)",
      "sodiumOnly": true,
      "requiresExtension": true,
      "requiredExtension": "GL_EXT_draw_elements_base_vertex | GL_OES_draw_elements_base_vertex",
      "requiresComputeShader": false,
      "isOptimizationLayer": false,
      "comment": "Only activates when Sodium mod detected. Requires all indices[i] identical",
      "params": {
        "useRestartEmulation": false,
        "maxBatchSize": 4096,
        "validateOnEveryCall": false
      }
    },
    
    "sodiumTierU": {
      "enabled": true,
      "description": "Sodium TIER U - Micro-batch unroll (primcount <= 3)",
      "sodiumOnly": true,
      "requiresExtension": false,
      "requiresComputeShader": false,
      "isOptimizationLayer": false,
      "comment": "Only activates when Sodium mod detected. For small batches",
      "params": {
        "useRestartEmulation": true,
        "maxBatchSize": 3,
        "validateOnEveryCall": false
      }
    }
  },
  
  "modeSelectionStrategy": {
    "comment": "NO AUTO-FALLBACK. Direct selection only.",
    "primaryMode": "drawElements",
    "comment_primaryMode": "Only ONE primary mode active at a time",
    "enableOptimizationLayers": true,
    "comment_optimizationLayers": "FastPath, FunnelEngine can layer on top if enabled",
    "strictMode": true,
    "comment_strictMode": "If true, crash instead of silent degradation when mode unavailable"
  }
}
*/
