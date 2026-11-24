/// @file    ScriptLifetimeProxy.hpp
/// @brief   Script から LifetimeComponent を操作するショートハンド。
/// @author  Hasegawa Jin
/// @date    2026-06-23

/// @note 銃弾・パーティクル GO など、寿命タイマーを持つオブジェクトの残り時間を動的に制御する。
#pragma once
#include <Engine/Api.hpp>

namespace fbzz::scene {

class Script;

struct ScriptLifetimeProxy {
    Script* script = nullptr;

    /// @brief 残り寿命 [秒] を設定する。正値で指定時間後に GO が破棄される。
    FBZZ_ENGINE_API void  SetRemaining(float seconds) const;
    FBZZ_ENGINE_API float GetRemaining() const;

    FBZZ_ENGINE_API void SetEnabled(bool enabled) const;
    FBZZ_ENGINE_API bool IsEnabled() const;
    /// @return LifetimeComponent 自体が付いているか。付いていない GO では GetRemaining() が
    /// @note 0 を返すため、「寿命 0」と「寿命を持たない」の区別に要る。
    FBZZ_ENGINE_API bool HasLifetime() const;

    /// @brief remaining を 0 にして次フレームで即座に GO を破棄させる。
    FBZZ_ENGINE_API void Kill() const;
};

} /// @note namespace fbzz::scene
