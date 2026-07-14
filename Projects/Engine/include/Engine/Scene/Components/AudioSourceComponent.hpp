// FBZZ Engine
// AudioSourceComponent.hpp | fbzz::scene
// GameObject に音声再生設定を持たせるコンポーネント
// AudioSystem が走査して BGM / SE の再生命令へ変換する。
// Transform と AudioListenerComponent を使った距離減衰・左右パンにも対応する。
#pragma once
#include <Engine/Scene/Script.hpp>
#include <cstdint>
#include <string>

namespace fbzz::scene {

struct AudioSourceComponent {
    std::string clipPath;
    bool        playOnAwake = false;
    bool        loop        = false;
    float       volume      = 1.0f;
    float       pitch       = 1.0f;
    // 0 は 2D、1 は完全な 3D。中間値は距離減衰とパンを線形にブレンドする。
    float       spatialBlend = 0.0f;
    float       minDistance  = 1.0f;
    float       maxDistance  = 50.0f;
    float       rolloffFactor = 1.0f;
    bool        enabled     = true;
    bool        m_played       = false; // PlayOnAwake が発火済みかを追跡し、重複再生を防ぐ
    bool        m_isPlaying    = false; // 現在再生中
    bool        m_isPaused     = false; // 一時停止中
    bool        m_pendingPlay  = false; // 次フレームに再生要求
    bool        m_pendingStop  = false; // 次フレームに停止要求
    bool        m_pendingPause = false; // 次フレームに一時停止要求
    uint32_t    m_voiceId      = 0;     // AudioManager が返す非所有 voice ハンドル
    std::string m_oneShotPath;
    bool        m_pendingOneShot = false; // 次フレームに one-shot 再生要求

    const char* GetTypeName() const { return "Audio Source"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.Field("clipPath", clipPath);
        r.Field("playOnAwake", playOnAwake);
        r.Field("loop", loop);
        r.FloatRange("volume", volume, 0.0f, 1.0f);
        r.FloatRange("pitch", pitch, 0.01f, 4.0f);
        r.FloatRange("spatialBlend", spatialBlend, 0.0f, 1.0f);
        r.FloatRange("minDistance", minDistance, 0.0f, 100000.0f);
        r.FloatRange("maxDistance", maxDistance, 0.001f, 100000.0f);
        r.FloatRange("rolloffFactor", rolloffFactor, 0.01f, 8.0f);
    }
};

} // namespace fbzz::scene
