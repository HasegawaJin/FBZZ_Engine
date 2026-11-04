/// @file    ScriptLightProxy.hpp
/// @brief   Script から LightComponent を操作するショートハンド。
/// @author  Hasegawa Jin
/// @date    2026-06-01
#pragma once
#include <Engine/Api.hpp>

#include <Math/Vector3.hpp>

namespace fbzz::scene {

class Script;

/// @note LightComponent::Type と値を一致させること。DLL 境界を越えるので順序の変更は不可。
enum class LightType { Directional, Point, Spot, Area, Sphere, Tube };

struct ScriptLightProxy {
    Script* script = nullptr;

    FBZZ_ENGINE_API void SetColor(const math::Vector3& color) const;
    FBZZ_ENGINE_API void SetType(LightType type) const;
    FBZZ_ENGINE_API void SetIntensity(float intensity) const;
    FBZZ_ENGINE_API void SetRange(float range) const;
    FBZZ_ENGINE_API void SetInnerCone(float degrees) const;
    FBZZ_ENGINE_API void SetOuterCone(float degrees) const;
    FBZZ_ENGINE_API void SetEnabled(bool enabled) const;

    /// @name 読み取り
    /// @note オーサリング値を起点に増減させる演出 (点滅・フェード・被弾時のフラッシュ) は、
    /// @note 元の値を Script 側に控えておかなくてもここから読める。LightComponent が
    /// @note 無ければ既定値を返す。
    /// @{
    [[nodiscard]] FBZZ_ENGINE_API math::Vector3 GetColor() const;
    [[nodiscard]] FBZZ_ENGINE_API LightType     GetType() const;
    [[nodiscard]] FBZZ_ENGINE_API float         GetIntensity() const;
    [[nodiscard]] FBZZ_ENGINE_API float         GetRange() const;
    [[nodiscard]] FBZZ_ENGINE_API float         GetInnerCone() const;
    [[nodiscard]] FBZZ_ENGINE_API float         GetOuterCone() const;
    [[nodiscard]] FBZZ_ENGINE_API bool          IsEnabled() const;
    /// @}
};

} /// @note namespace fbzz::scene
