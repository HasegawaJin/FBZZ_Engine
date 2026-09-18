/// @file    ScriptWindProxy.hpp
/// @brief   Script から環境流 (SceneEnvironment) を操作するプロキシ。
/// @author  Hasegawa Jin
/// @date    2026-07-15
#pragma once

#include <Math/Vector3.hpp>

namespace fbzz::scene {

class Script;

struct ScriptWindProxy {
    Script* script = nullptr;

    void SetEnabled(bool enabled) const;
    void SetDirection(const math::Vector3& direction) const;
    /// @note 単位は流速 [m/s] になった (旧 ForceField の加速度ではない)。名前は語彙を保つため据え置き。
    void SetStrength(float strength) const;
    void SetTurbulence(float turbulence) const;
    void SetPulseFrequency(float frequency) const;
};

} // namespace fbzz::scene
