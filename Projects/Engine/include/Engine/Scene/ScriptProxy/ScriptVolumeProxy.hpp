/// @file    ScriptVolumeProxy.hpp
/// @brief   Script から VolumeComponent (重力・渦・爆風・時間減速など) を操作するショートハンド。
/// @author  Hasegawa Jin
/// @date    2026-06-23
#pragma once
#include <Engine/Api.hpp>

#include <Math/Vector3.hpp>

namespace fbzz::scene {

class Script;

struct ScriptVolumeProxy {
    Script* script = nullptr;

    FBZZ_ENGINE_API void SetEnabled(bool enabled) const;

    /// @note カスタム重力加速度 (VolumeType::Gravity のとき有効)。
    FBZZ_ENGINE_API void SetGravity(const math::Vector3& gravity) const;
    /// @note 渦の回転強さ (VolumeType::Vortex)。
    FBZZ_ENGINE_API void SetSwirlStrength(float strength) const;
    /// @note 爆発インパルス (VolumeType::Explosion)。
    FBZZ_ENGINE_API void SetExplosionImpulse(float impulse) const;
    /// @note 時間スケール (VolumeType::TimeDilation)。1.0 で等倍、0 で停止。
    FBZZ_ENGINE_API void SetTimeScale(float scale) const;

    /// @note 有効期間 [秒]。負値で無限継続。設定後は経過時間 (elapsed) をリセットしない。
    FBZZ_ENGINE_API void SetDuration(float seconds) const;
    /// @note elapsed を 0 にリセットして有効期間タイマーを巻き戻す。
    FBZZ_ENGINE_API void ResetElapsed() const;

    [[nodiscard]] FBZZ_ENGINE_API bool  IsEnabled() const;
    [[nodiscard]] FBZZ_ENGINE_API float GetTimeScale() const;
    [[nodiscard]] FBZZ_ENGINE_API float GetDuration() const;
    /// @note 有効化からの経過秒。duration が負 (無限) でも増え続ける。
    [[nodiscard]] FBZZ_ENGINE_API float GetElapsed() const;
};

} /// @note namespace fbzz::scene
