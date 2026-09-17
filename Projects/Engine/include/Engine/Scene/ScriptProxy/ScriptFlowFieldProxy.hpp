/// @file    ScriptFlowFieldProxy.hpp
/// @brief   Script から FlowField (流れの場) を操作するプロキシ。
/// @author  Hasegawa Jin
/// @date    2026-07-15

#pragma once

#include <Math/Vector3.hpp>
#include <cstdint>

namespace fbzz::scene {

class Script;

/// @note 値は FlowFieldType と一致させること。LEGACY_DRAG は読み込み専用なので出さない。
enum class ScriptFlowFieldType : uint8_t {
    UNIFORM = 0,
    SINK,
    SOURCE,
    VORTEX,
    CURL,
};

struct ScriptFlowFieldProxy {
    Script* script = nullptr;

    void SetEnabled(bool enabled) const;
    void SetType(ScriptFlowFieldType type) const;
    /// 流速の大きさ [m/s]。
    void SetSpeed(float speed) const;
    void SetRadius(float radius, float falloffPower = 2.0f) const;
    void SetDirection(const math::Vector3& direction) const;
    void SetTurbulence(float frequency, float speed) const;

    /// この場を受け取るエミッターを絞る。ParticleEmitter 側の
    /// ScriptParticleProxy::SetFlowFieldChannels と 1 ビットでも重なった相手にだけ作用する。
    /// 既定は全ビット ON (シーン内の全エミッターへ一律に効く)。
    void SetChannels(uint32_t channels) const;

    [[nodiscard]] bool  IsEnabled() const;
    [[nodiscard]] float GetSpeed() const;
    [[nodiscard]] float GetRadius() const;
    [[nodiscard]] ScriptFlowFieldType GetType() const;
    [[nodiscard]] uint32_t GetChannels() const;
};

} // namespace fbzz::scene
