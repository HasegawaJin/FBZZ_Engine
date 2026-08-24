/// @file    AudioSourceComponent.hpp
/// @brief   GameObject に音声再生設定を持たせるコンポーネント。
/// @author  Hasegawa Jin
/// @date    2025-01-01
#pragma once
#include <Engine/Scene/Script.hpp>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fbzz::scene {

/// AudioSystem が走査して再生命令へ変換する。
/// Transform と AudioListenerComponent を使った距離減衰・左右パンにも対応する。
struct AudioSourceComponent {
    /// .wav / .mp3 / .ogg / .flac のほか、手続き効果音の .synth も指定できる
    /// (AudioManager が読み込み時に合成する)。
    std::string clipPath;
    bool        playOnAwake = false;
    bool        loop        = false;
    float       volume      = 1.0f;
    float       pitch       = 1.0f;
    /// 出力先ミキサーバス。ProjectSettings の [[audio.bus]] で定義した名前。
    /// 未知の名前は Master へ落ちる。
    std::string busName     = "SE";
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
    bool        m_pendingResume = false; // 次フレームに再開要求
    uint32_t    m_voiceId      = 0;     // AudioManager が返す非所有 voice ハンドル

    // 手続き生成クリップを主 voice として鳴らす要求。clipPath より優先する。
    // 参照カウントを 1 つ持った状態で積まれ、AudioSystem が再生後に手放す。
    // WHY 所有権をここへ持たせるか: 生成クリップは PCM の実体を AudioManager が抱えており、
    //     誰も参照しなくなった瞬間に解放される。要求を積んでから実際に鳴るまでの
    //     1 フレームで解放されないよう、要求そのものが参照を握る。
    uint32_t    m_pendingClipId = 0;

    /// 主 voice に重ねて 1 度だけ鳴らす要求。
    struct OneShotRequest {
        std::string path;              ///< 空なら clipId を使う
        uint32_t    clipId      = 0;   ///< 生成クリップ。参照を 1 つ握る
        float       volumeScale = 1.0f;
    };
    // WHY 1 個ではなく列で持つか: 単一スロットだと、同じフレームに 2 回要求した
    //     ぶんが上書きで消える。敵が同時に複数被弾する場面は普通に起きるので、
    //     「たまに音が抜ける」という再現しにくい形の不具合になっていた。
    std::vector<OneShotRequest> m_pendingOneShots;
    /// 1 フレームに積める上限。AudioSystem は SimOnly なので、Edit モードで
    /// 誰も引き取らないまま積み続けても膨らまないように蓋をする。
    static constexpr size_t MAX_PENDING_ONE_SHOTS = 16;

    const char* GetTypeName() const { return "Audio Source"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.Field("clipPath", clipPath);
        r.Field("playOnAwake", playOnAwake);
        r.Field("loop", loop);
        r.FloatRange("volume", volume, 0.0f, 1.0f);
        r.FloatRange("pitch", pitch, 0.01f, 4.0f);
        r.BeginField("busName", "busName");
        r.SetFieldHint(IReflector::FieldHint::AudioBus);
        r.Field("busName", busName);
        r.Tooltip("出力先ミキサーバス (Project Settings > Audio で定義)");
        r.EndField();
        r.FloatRange("spatialBlend", spatialBlend, 0.0f, 1.0f);
        r.FloatRange("minDistance", minDistance, 0.0f, 100000.0f);
        r.FloatRange("maxDistance", maxDistance, 0.001f, 100000.0f);
        r.FloatRange("rolloffFactor", rolloffFactor, 0.01f, 8.0f);
    }
};

} // namespace fbzz::scene
