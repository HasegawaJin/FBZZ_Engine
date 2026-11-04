/// @file    ScriptFlowFieldProxy.hpp
/// @brief   Script から FlowField (流れの場) を操作するプロキシ。
/// @author  Hasegawa Jin
/// @date    2026-07-15

#pragma once
#include <Engine/Api.hpp>

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

    FBZZ_ENGINE_API void SetEnabled(bool enabled) const;
    FBZZ_ENGINE_API void SetType(ScriptFlowFieldType type) const;
    /// @note 流速の大きさ [m/s]。
    FBZZ_ENGINE_API void SetSpeed(float speed) const;
    FBZZ_ENGINE_API void SetRadius(float radius, float falloffPower = 2.0f) const;
    FBZZ_ENGINE_API void SetDirection(const math::Vector3& direction) const;
    FBZZ_ENGINE_API void SetTurbulence(float frequency, float speed) const;

    /// @note この場を受け取るエミッターを絞る。ParticleEmitter 側の
    /// @note ScriptParticleProxy::SetFlowFieldChannels と 1 ビットでも重なった相手にだけ作用する。
    /// @note 既定は全ビット ON (シーン内の全エミッターへ一律に効く)。
    FBZZ_ENGINE_API void SetChannels(uint32_t channels) const;

    [[nodiscard]] FBZZ_ENGINE_API bool  IsEnabled() const;
    [[nodiscard]] FBZZ_ENGINE_API float GetSpeed() const;
    [[nodiscard]] FBZZ_ENGINE_API float GetRadius() const;
    [[nodiscard]] FBZZ_ENGINE_API ScriptFlowFieldType GetType() const;
    [[nodiscard]] FBZZ_ENGINE_API uint32_t GetChannels() const;
};

} /// @note namespace fbzz::scene
