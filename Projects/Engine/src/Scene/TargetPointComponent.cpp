/// @file    TargetPointComponent.cpp
/// @brief   TargetPoint の標準 Inspector 反射。
/// @author  Hasegawa Jin
/// @date    2026-10-08
#include <Engine/Scene/Components/TargetPointComponent.hpp>

#include <Engine/Scene/Script.hpp>
#include <algorithm>
#include <cmath>

namespace fbzz::scene {

void TargetPointComponent::Reflect(IReflector& reflector)
{
    reflector.Field("visible", visible);
    reflector.Field("color", color);
    reflector.FloatRange("screenSize", screenSize, 3.0f, 64.0f);
    screenSize = std::isfinite(screenSize) ? (std::clamp)(screenSize, 3.0f, 64.0f) : 9.0f;
}

} /// @note namespace fbzz::scene
