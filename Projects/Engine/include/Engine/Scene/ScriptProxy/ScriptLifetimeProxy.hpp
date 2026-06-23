// FBZZ Engine
// ScriptLifetimeProxy.hpp | fbzz::scene
// Script から LifetimeComponent を操作するショートハンド。
// 銃弾・パーティクル GO など、寿命タイマーを持つオブジェクトの残り時間を動的に制御する。
#pragma once

namespace fbzz::scene {

class Script;

struct ScriptLifetimeProxy {
    Script* script = nullptr;

    // 残り寿命 [秒] を設定する。正値で指定時間後に GO が破棄される。
    void  SetRemaining(float seconds) const;
    float GetRemaining() const;

    void SetEnabled(bool enabled) const;

    // remaining を 0 にして次フレームで即座に GO を破棄させる。
    void Kill() const;
};

} // namespace fbzz::scene
