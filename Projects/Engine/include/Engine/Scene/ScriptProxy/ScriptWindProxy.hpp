/// @file    ScriptWindProxy.hpp
/// @brief   Script から環境流 (SceneEnvironment) を操作するプロキシ。
/// @author  Hasegawa Jin
/// @date    2026-07-15
#pragma once
#include <Engine/Api.hpp>

#include <Math/Vector3.hpp>

namespace fbzz::scene {

class Script;

struct ScriptWindProxy {
    Script* script = nullptr;

    FBZZ_ENGINE_API void SetEnabled(bool enabled) const;
    FBZZ_ENGINE_API void SetDirection(const math::Vector3& direction) const;
    /// @note 単位は流速 [m/s] になった (旧 ForceField の加速度ではない)。名前は語彙を保つため据え置き。
    FBZZ_ENGINE_API void SetStrength(float strength) const;
    FBZZ_ENGINE_API void SetTurbulence(float turbulence) const;
    FBZZ_ENGINE_API void SetPulseFrequency(float frequency) const;
};

} /// @note namespace fbzz::scene
