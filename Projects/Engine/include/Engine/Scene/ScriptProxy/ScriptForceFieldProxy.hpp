/// @file    ScriptForceFieldProxy.hpp
/// @brief   Script から ForceField (ベクトルフィールド) を操作するプロキシ。
/// @author  Hasegawa Jin
/// @date    2026-07-15

#pragma once

#include <Math/Vector3.hpp>
#include <cstdint>

namespace fbzz::scene {

class Script;

enum class ScriptForceFieldType : uint8_t {
    WIND = 0,
    ATTRACT,
    REPULSE,
    VORTEX,
    TURBULENCE,
    DRAG
};

struct ScriptForceFieldProxy {
    Script* script = nullptr;

    void SetEnabled(bool enabled) const;
    void SetType(ScriptForceFieldType type) const;
    void SetStrength(float strength) const;
    void SetRadius(float radius, float falloffPower = 2.0f) const;
    void SetDirection(const math::Vector3& direction) const;
    void SetTurbulence(float frequency, float speed) const;

    /// この力場を受け取るエミッターを絞る。ParticleEmitter 側の
    /// ScriptParticleProxy::SetForceFieldChannels と 1 ビットでも重なった相手にだけ作用する。
    /// 既定は全ビット ON (シーン内の全エミッターへ一律に効く)。
    void SetChannels(uint32_t channels) const;

    [[nodiscard]] bool  IsEnabled() const;
    [[nodiscard]] float GetStrength() const;
    [[nodiscard]] float GetRadius() const;
    [[nodiscard]] ScriptForceFieldType GetType() const;
    [[nodiscard]] uint32_t GetChannels() const;
};

} // namespace fbzz::scene
