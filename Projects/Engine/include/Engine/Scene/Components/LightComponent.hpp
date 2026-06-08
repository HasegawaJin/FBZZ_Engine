// FBZZ Engine
// LightComponent.hpp | fbzz::scene
// ライト情報を持つコンポーネント
// 方向と位置は Transform から取り、色や強度などの発光設定だけを持つ。
// RenderSystem が LightSystem へ集約して GPU へ送る。
#pragma once
#include <Engine/Scene/Script.hpp>
#include <Math/Vector3.hpp>

namespace fbzz::scene {

struct LightComponent {
    enum class Type { Directional, Point, Spot };

    Type          type      = Type::Directional;
    math::Vector3 color     = { 1.0f, 1.0f, 1.0f };
    float         intensity = 1.0f;
    float         range     = 10.0f;    // Point / Spot のみ
    float         innerCone = 15.0f;    // Spot のみ (degrees)
    float         outerCone = 30.0f;    // Spot のみ (degrees)
    bool          enabled   = true;

    const char* GetTypeName() const { return "Light"; }
    void Reflect(IReflector& r)
    {
        int typeValue = static_cast<int>(type);
        r.Field("type", typeValue);
        if (typeValue < 0) typeValue = 0;
        if (typeValue > 2) typeValue = 2;
        type = static_cast<Type>(typeValue);
        r.Field("enabled", enabled);
        r.Field("color", color);
        r.Field("intensity", intensity);
        r.Field("range", range);
        r.Field("innerCone", innerCone);
        r.Field("outerCone", outerCone);
    }
    // Directional / Spot の方向 → Transform::Forward()
    // Point / Spot の位置      → Transform::position
};

} // namespace fbzz::scene
