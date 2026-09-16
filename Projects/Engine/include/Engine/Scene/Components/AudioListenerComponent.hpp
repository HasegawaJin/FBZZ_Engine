/// @file    AudioListenerComponent.hpp
/// @brief   3D 空間オーディオの受聴点とマスター音量を定義する。
/// @author  Hasegawa Jin
/// @date    2026-07-15
#pragma once

#include <Engine/Scene/Script.hpp>
#include <Math/Vector3.hpp>

namespace fbzz::scene {

// Transform の位置・向きを AudioSystem の受聴座標として使う。
// Camera と分けるのは、リプレイカメラや観戦カメラでも音の基準を明示的に選べるようにするため。
struct AudioListenerComponent {
    bool enabled = true;
    // 有効な Listener が複数あるとき、最大の 1 つが受聴点になる。同値なら登録順。
    int priority = 0;
    float volume = 1.0f;

    // 前フレームのワールド座標と、それが有効かどうか。Doppler の相対速度に使う。
    // AudioSystem が毎フレーム書き込む。
    math::Vector3 m_previousPosition{};
    bool          m_hasPreviousPosition = false;

    const char* GetTypeName() const { return "Audio Listener"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.Field("priority", priority);
        r.FloatRange("volume", volume, 0.0f, 1.0f);
    }
};

} // namespace fbzz::scene
