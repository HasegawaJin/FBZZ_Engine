// FBZZ Engine
// AudioListenerComponent.hpp | fbzz::scene
// 3D 空間オーディオの受聴点とマスター音量を定義する。
#pragma once

#include <Engine/Scene/Script.hpp>

namespace fbzz::scene {

// AudioListenerComponent — Transform の位置・向きを AudioSystem の受聴座標として使う。
// WHY: Camera と Listener を分離し、リプレイカメラや観戦カメラでも音の基準を明示的に選べるようにする。
struct AudioListenerComponent {
    bool enabled = true;
    int priority = 0;
    float volume = 1.0f;

    const char* GetTypeName() const { return "Audio Listener"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.Field("priority", priority);
        r.FloatRange("volume", volume, 0.0f, 1.0f);
    }
};

} // namespace fbzz::scene
