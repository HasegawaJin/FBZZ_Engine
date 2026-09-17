/// @file    ScriptLifetimeProxy.hpp
/// @brief   Script から LifetimeComponent を操作するショートハンド。
/// @author  Hasegawa Jin
/// @date    2026-06-23
///
/// 銃弾・パーティクル GO など、寿命タイマーを持つオブジェクトの残り時間を動的に制御する。
#pragma once

namespace fbzz::scene {

class Script;

struct ScriptLifetimeProxy {
    Script* script = nullptr;

    /// @brief 残り寿命 [秒] を設定する。正値で指定時間後に GO が破棄される。
    void  SetRemaining(float seconds) const;
    float GetRemaining() const;

    void SetEnabled(bool enabled) const;
    bool IsEnabled() const;
    /// @return LifetimeComponent 自体が付いているか。付いていない GO では GetRemaining() が
    ///         0 を返すため、「寿命 0」と「寿命を持たない」の区別に要る。
    bool HasLifetime() const;

    /// @brief remaining を 0 にして次フレームで即座に GO を破棄させる。
    void Kill() const;
};

} // namespace fbzz::scene
