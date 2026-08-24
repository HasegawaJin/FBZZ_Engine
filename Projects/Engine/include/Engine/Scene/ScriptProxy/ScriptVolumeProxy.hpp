// FBZZ Engine
// ScriptVolumeProxy.hpp | fbzz::scene
// Script から VolumeComponent (重力・渦・爆風・時間減速など) を操作するショートハンド。
#pragma once

#include <Math/Vector3.hpp>

namespace fbzz::scene {

class Script;

struct ScriptVolumeProxy {
    Script* script = nullptr;

    void SetEnabled(bool enabled) const;

    // カスタム重力加速度 (VolumeType::Gravity のとき有効)。
    void SetGravity(const math::Vector3& gravity) const;
    // 渦の回転強さ (VolumeType::Vortex)。
    void SetSwirlStrength(float strength) const;
    // 浮力 (VolumeType::Buoyancy)。
    void SetBuoyancy(float buoyancy) const;
    // 爆発インパルス (VolumeType::Explosion)。
    void SetExplosionImpulse(float impulse) const;
    // 時間スケール (VolumeType::TimeDilation)。1.0 で等倍、0 で停止。
    void SetTimeScale(float scale) const;

    // 有効期間 [秒]。負値で無限継続。設定後は経過時間 (elapsed) をリセットしない。
    void SetDuration(float seconds) const;
    // elapsed を 0 にリセットして有効期間タイマーを巻き戻す。
    void ResetElapsed() const;

    [[nodiscard]] bool  IsEnabled() const;
    [[nodiscard]] float GetTimeScale() const;
    [[nodiscard]] float GetDuration() const;
    // 有効化からの経過秒。duration が負 (無限) でも増え続ける。
    [[nodiscard]] float GetElapsed() const;
};

} // namespace fbzz::scene
