// FBZZ Engine
// ScriptSunMoonProxy.hpp | fbzz::scene
// Script から太陽・月ディスクの描画設定を操作するプロキシ
#pragma once

#include <Math/Vector3.hpp>

namespace fbzz::scene {

class Script;

struct ScriptSunMoonProxy {
    Script* script = nullptr;

    void SetEnabled(bool enabled) const;
    void SetSun(bool enabled, float intensity) const;
    void SetMoon(bool enabled, float size, float brightness,
                 const math::Vector3& color) const;
};

} // namespace fbzz::scene
