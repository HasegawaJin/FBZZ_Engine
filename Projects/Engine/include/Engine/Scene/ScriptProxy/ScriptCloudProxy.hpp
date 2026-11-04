/// @file    ScriptCloudProxy.hpp
/// @brief   Script から VolumetricCloudComponent を操作するプロキシ。
/// @author  Hasegawa Jin
/// @date    2026-07-15
#pragma once
#include <Engine/Api.hpp>

#include <Math/Vector2.hpp>
#include <Math/Vector3.hpp>

namespace fbzz::scene {

class Script;

struct ScriptCloudProxy {
    Script* script = nullptr;

    FBZZ_ENGINE_API void SetEnabled(bool enabled) const;
    FBZZ_ENGINE_API void SetLayer(float bottomHeight, float thickness) const;
    FBZZ_ENGINE_API void SetCoverage(float coverage, float density) const;
    FBZZ_ENGINE_API void SetWind(const math::Vector2& direction, float speed) const;
    FBZZ_ENGINE_API void SetLighting(float absorption, float ambientStrength,
                     float silverLining, const math::Vector3& albedo) const;
    FBZZ_ENGINE_API void SetQuality(int stepCount, float maxDistance) const;
};

} /// @note namespace fbzz::scene
