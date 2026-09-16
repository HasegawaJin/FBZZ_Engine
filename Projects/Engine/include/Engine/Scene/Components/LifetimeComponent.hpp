/// @file    LifetimeComponent.hpp
/// @brief   GameObjectを自動破棄するまでの残り時間を保持する。
/// @author  Hasegawa Jin
/// @date    2026-06-10
#pragma once

#include <Engine/Scene/Script.hpp>

namespace fbzz::scene {

// LifetimeComponent — エフェクトなどの一時GameObjectへ秒単位の寿命を与える。
struct LifetimeComponent {
    float remaining = 5.0f;
    bool  enabled   = true;

    const char* GetTypeName() const { return "Lifetime"; }
    void Reflect(IReflector& reflector)
    {
        reflector.Field("enabled", enabled);
        reflector.FloatRange("remaining", remaining, 0.0f, 9999.0f);
    }
};

} // namespace fbzz::scene
