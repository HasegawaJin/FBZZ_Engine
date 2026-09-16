/// @file    ScriptEnvironmentProxy.hpp
/// @brief   Script からシーン全体の環境設定を操作するショートハンド。
/// @author  Hasegawa Jin
/// @date    2026-06-23
///
/// EnvironmentLightComponent / AtmosphericScatteringComponent / SkyRenderer を
/// シーン内から検索して操作するため、任意の Script から呼べる。
/// WHY: これらは通常シーンに 1 つしか存在しないグローバル設定コンポーネントであり、
/// Script が所属する GO に付いていなくても操作できることが重要。
#pragma once

#include <Math/Vector3.hpp>

namespace fbzz::scene {

class Script;

struct ScriptEnvironmentProxy {
    Script* script = nullptr;

    // --- EnvironmentLightComponent (IBL) ---
    void SetIBLEnabled(bool enabled)       const;
    void SetIBLIntensity(float intensity)  const;
    void SetIBLDiffuseScale(float scale)   const;
    void SetIBLSpecularScale(float scale)  const;

    // --- AtmosphericScatteringComponent (霧) ---
    void SetFogEnabled(bool enabled)           const;
    void SetFogDensity(float density)          const;
    void SetFogFar(float fogFar)               const;
    void SetFogColor(const math::Vector3& rgb) const;

    // --- SkyRenderer (大気散乱空) ---
    void SetSunIntensity(float intensity)  const;
    void SetMieScattering(float mie)       const;
    void SetMieG(float g)                  const;

    float GetSunIntensity()  const;
    float GetFogDensity()    const;
    bool  IsFogEnabled()     const;

    // 対応するコンポーネントがシーンに無ければ既定値 (0 / false / 黒) を返す。
    bool          IsIBLEnabled()        const;
    float         GetIBLIntensity()     const;
    float         GetIBLDiffuseScale()  const;
    float         GetIBLSpecularScale() const;
    math::Vector3 GetFogColor()         const;
    float         GetFogFar()           const;
    float         GetMieScattering()    const;
    float         GetMieG()             const;
};

} // namespace fbzz::scene
