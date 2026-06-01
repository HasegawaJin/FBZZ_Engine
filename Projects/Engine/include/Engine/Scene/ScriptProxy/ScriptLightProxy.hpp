// FBZZ Engine
// ScriptLightProxy.hpp | fbzz::scene
// Script から LightComponent を操作するショートハンド
#pragma once

#include <Math/Vector3.hpp>

namespace fbzz::scene {

class Script;

struct ScriptLightProxy {
    Script* script = nullptr;

    void SetColor(const math::Vector3& color) const;
    void SetIntensity(float intensity) const;
    void SetRange(float range) const;
    void SetEnabled(bool enabled) const;
};

} // namespace fbzz::scene
