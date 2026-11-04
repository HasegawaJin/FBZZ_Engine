/// @file    ScriptEnvironmentProxy.hpp
/// @brief   Script からシーン全体の環境設定を操作するショートハンド。
/// @author  Hasegawa Jin
/// @date    2026-06-23

/// @note EnvironmentLightComponent / AtmosphericScatteringComponent / SkyRenderer を
/// @note シーン内から検索して操作するため、任意の Script から呼べる。
/// @note これらはシーンに通常 1 つしか存在しないグローバル設定コンポーネントであり、
/// @note Script が所属する GO に付いていなくても操作できる必要がある。
#pragma once
#include <Engine/Api.hpp>

#include <Math/Vector3.hpp>

namespace fbzz::scene {

class Script;

struct ScriptEnvironmentProxy {
    Script* script = nullptr;

    /// @name EnvironmentLightComponent (IBL)
    /// @{
    FBZZ_ENGINE_API void SetIBLEnabled(bool enabled)       const;
    FBZZ_ENGINE_API void SetIBLIntensity(float intensity)  const;
    FBZZ_ENGINE_API void SetIBLDiffuseScale(float scale)   const;
    FBZZ_ENGINE_API void SetIBLSpecularScale(float scale)  const;
    /// @}

    /// @name AtmosphericScatteringComponent (霧)
    /// @{
    FBZZ_ENGINE_API void SetFogEnabled(bool enabled)           const;
    FBZZ_ENGINE_API void SetFogDensity(float density)          const;
    FBZZ_ENGINE_API void SetFogFar(float fogFar)               const;
    FBZZ_ENGINE_API void SetFogColor(const math::Vector3& rgb) const;
    /// @}

    /// @name SkyRenderer (大気散乱空)
    /// @{
    FBZZ_ENGINE_API void SetSunIntensity(float intensity)  const;
    FBZZ_ENGINE_API void SetMieScattering(float mie)       const;
    FBZZ_ENGINE_API void SetMieG(float g)                  const;
    /// @}

    FBZZ_ENGINE_API float GetSunIntensity()  const;
    FBZZ_ENGINE_API float GetFogDensity()    const;
    FBZZ_ENGINE_API bool  IsFogEnabled()     const;

    /// @note 対応するコンポーネントがシーンに無ければ既定値 (0 / false / 黒) を返す。
    FBZZ_ENGINE_API bool          IsIBLEnabled()        const;
    FBZZ_ENGINE_API float         GetIBLIntensity()     const;
    FBZZ_ENGINE_API float         GetIBLDiffuseScale()  const;
    FBZZ_ENGINE_API float         GetIBLSpecularScale() const;
    FBZZ_ENGINE_API math::Vector3 GetFogColor()         const;
    FBZZ_ENGINE_API float         GetFogFar()           const;
    FBZZ_ENGINE_API float         GetMieScattering()    const;
    FBZZ_ENGINE_API float         GetMieG()             const;
};

} /// @note namespace fbzz::scene
