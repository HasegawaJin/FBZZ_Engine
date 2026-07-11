// FBZZ Engine
// ScriptFoliageProxy.hpp | fbzz::scene
// Script から FoliageComponent の配置・再ベイクを制御するショートハンド
#pragma once

#include <Math/Vector3.hpp>
#include <cstddef>

namespace fbzz::scene {

class Script;

struct ScriptFoliageProxy {
    Script* script = nullptr;

    void SetEnabled(bool enabled) const;
    void RequestBake(bool rebuildChildren = true) const;
    bool AddStamp(size_t speciesIndex, const math::Vector3& localPosition,
                  float rotationY = 0.0f, float scale = 1.0f) const;
    bool ClearStamps(size_t speciesIndex) const;
    bool SetDensity(size_t speciesIndex, float densityPer100SquareMeters) const;
    bool SetDrawDistance(size_t speciesIndex, float distance) const;
};

} // namespace fbzz::scene
