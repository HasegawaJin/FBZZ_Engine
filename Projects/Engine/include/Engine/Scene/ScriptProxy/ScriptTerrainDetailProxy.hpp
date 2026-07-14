// FBZZ Engine
// ScriptTerrainDetailProxy.hpp | fbzz::scene
// Script から Terrain Detail レイヤーを操作し再ベイクを要求するプロキシ
#pragma once

#include <cstddef>

namespace fbzz::scene {

class Script;

struct ScriptTerrainDetailProxy {
    Script* script = nullptr;

    void SetEnabled(bool enabled) const;
    [[nodiscard]] int GetLayerCount() const;
    bool SetDensity(size_t layerIndex, float density) const;
    bool SetScaleRange(size_t layerIndex, float minScale, float maxScale) const;
    bool SetDrawDistance(size_t layerIndex, float fadeStartDistance, float drawDistance) const;
    bool SetWind(size_t layerIndex, float strength, float frequency) const;
    void RequestBake() const;
};

} // namespace fbzz::scene
