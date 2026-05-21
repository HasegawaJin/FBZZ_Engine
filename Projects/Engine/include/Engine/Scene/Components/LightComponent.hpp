// FBZZ Engine
// LightComponent.hpp | fbzz::scene
// ライト情報を持つ Component。方向/位置は Transform から取得する
#pragma once
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
    // Directional / Spot の方向 → Transform::Forward()
    // Point / Spot の位置      → Transform::position
};

} // namespace fbzz::scene
