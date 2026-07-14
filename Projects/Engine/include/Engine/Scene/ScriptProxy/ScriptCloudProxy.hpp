// FBZZ Engine
// ScriptCloudProxy.hpp | fbzz::scene
// Script から VolumetricCloudComponent を操作するプロキシ
#pragma once

#include <Math/Vector2.hpp>
#include <Math/Vector3.hpp>

namespace fbzz::scene {

class Script;

struct ScriptCloudProxy {
    Script* script = nullptr;

    void SetEnabled(bool enabled) const;
    void SetLayer(float bottomHeight, float thickness) const;
    void SetCoverage(float coverage, float density) const;
    void SetWind(const math::Vector2& direction, float speed) const;
    void SetLighting(float absorption, float ambientStrength,
                     float silverLining, const math::Vector3& albedo) const;
    void SetQuality(int stepCount, float maxDistance) const;
};

} // namespace fbzz::scene
