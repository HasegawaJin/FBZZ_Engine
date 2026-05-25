// FBZZ Engine
// AudioSourceComponent.hpp | fbzz::scene
// GameObject に音声再生設定を持たせるコンポーネント
// AudioSystem が走査して BGM / SE の再生命令へ変換する。
// Transform 連動の 3D 音響はまだ持たせない。
#pragma once
#include <Engine/Scene/Script.hpp>
#include <string>

namespace fbzz::scene {

struct AudioSourceComponent {
    std::string clipPath;
    bool        playOnAwake = false;
    bool        loop        = false;
    float       volume      = 1.0f;
    bool        enabled     = true;
    bool        m_played    = false; // PlayOnAwake が発火済みかを追跡し、重複再生を防ぐ

    const char* GetTypeName() const { return "Audio Source"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.Field("clipPath", clipPath);
        r.Field("playOnAwake", playOnAwake);
        r.Field("loop", loop);
        r.Field("volume", volume);
    }
};

} // namespace fbzz::scene
