#pragma once

#include <map>
#include <string>
#include <vector>

// -----------------------------------------------------------------------------
// Strict draw mode registry used by MobileGlues.
// Each mode may be independently enabled or disabled. No silent fallback is
// allowed: the selected mode must correspond directly to the draw path actually
// used. When Sodium is detected, the funnel path is forced and the rest are
// disabled.
// -----------------------------------------------------------------------------
enum class DrawModeType {
    DrawElements,
    DrawElementsBaseVertex,
    MultiDrawIndirect,
    MultiDrawMultiIndirect,
    MultiDrawBaseVertex,
    MultiDrawArrays,
    ComputeShaderFusion,
    FastPath,
    FunnelEngine,
    SodiumTierS,
    SodiumTierU,
};

struct DrawModeConfig {
    DrawModeType type;
    std::string name;
    bool enabled;

    struct Params {
        bool useRestartEmulation;
        int maxBatchSize;
        bool validateOnEveryCall;
    } params;

    struct Meta {
        std::string description;
        bool sodiumOnly;
        bool requiresExtension;
        std::string requiredExtension;
        bool requiresComputeShader;
        bool isOptimizationLayer;
    } meta;
};

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
