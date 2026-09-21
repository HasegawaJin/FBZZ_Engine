/// @file    AudioSourceComponent.hpp
/// @brief   GameObject に音声再生設定を持たせるコンポーネント。
/// @author  Hasegawa Jin
/// @date    2025-01-01
#pragma once
#include <Engine/Scene/Script.hpp>
#include <Math/Vector3.hpp>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fbzz::scene {

/// @note AudioSystem が走査して再生命令へ変換する。
/// @note Transform と AudioListenerComponent を使った距離減衰・左右パンにも対応する。
struct AudioSourceComponent {
    /// @note .wav / .mp3 / .ogg / .flac のほか、手続き効果音の .synth も指定できる
    /// @note (AudioManager が読み込み時に合成する)。
    std::string clipPath;
    bool        playOnAwake = false;
    bool        loop        = false;
    /// @brief 長尺音声を固定量のバッファで再生する。開始後のデコードは専用スレッドで行う。
    bool        streaming   = false;
    float       volume      = 1.0f;
    float       pitch       = 1.0f;
    /// @note 出力先ミキサーバス。ProjectSettings の [[audio.bus]] で定義した名前。
    /// @note 未知の名前は Master へ落ちる。
    std::string busName     = "SE";
    /// @note 0 は 2D、1 は完全な 3D。中間値は距離減衰とパンを線形にブレンドする。
    float       spatialBlend = 0.0f;
    float       minDistance  = 1.0f;
    float       maxDistance  = 50.0f;
    float       rolloffFactor = 1.0f;
    /// @note 遠ざかるほど高域を吸わせる量 (0 で無加工)。距離そのものは rolloffFactor が受け持つ。
    float       airAbsorption = 0.0f;
    /// @note 相対速度によるピッチ変化の強さ。0 で無効、1 で物理どおり。既定は 0。
    float       dopplerLevel = 0.0f;
    /// @note 同時発音数が上限に達したとき、これが低い音から畳まれる。
    int         priority = 0;
    bool        enabled     = true;
    bool        m_played       = false; ///< @note PlayOnAwake が発火済みかを追跡し、重複再生を防ぐ
    bool        m_isPlaying    = false; ///< @note 現在再生中
    bool        m_isPaused     = false; ///< @note 一時停止中
    bool        m_pendingPlay  = false; ///< @note 次フレームに再生要求
    bool        m_pendingStop  = false; ///< @note 次フレームに停止要求
    bool        m_pendingPause = false; ///< @note 次フレームに一時停止要求
    bool        m_pendingResume = false; ///< @note 次フレームに再開要求
    uint32_t    m_voiceId      = 0;     ///< @note AudioManager が返す非所有 voice ハンドル

    /// @note 前フレームのワールド座標と、それが有効かどうか。Doppler の相対速度に使う。
    /// @note 物理の速度を見ないのは、音源が RigidBody を持たない飾りであることが多いため。
    math::Vector3 m_previousPosition{};
    bool          m_hasPreviousPosition = false;

    /// @note 手続き生成クリップを主 voice として鳴らす要求。clipPath より優先する。
    /// @note 参照カウントを 1 つ持った状態で積まれ、AudioSystem が再生後に手放す。
    /// @note PCM の実体は AudioManager が抱え、誰も参照しなくなった瞬間に解放される。
    /// @note 積んでから鳴るまでの 1 フレームで解放されないよう、要求自身が参照を握る。
    uint32_t    m_pendingClipId = 0;

    /// @note 主 voice に重ねて 1 度だけ鳴らす要求。
    struct OneShotRequest {
        std::string path;              ///< @note 空なら clipId を使う
        uint32_t    clipId      = 0;   ///< @note 生成クリップ。参照を 1 つ握る
        float       volumeScale = 1.0f;
    };
    /// @note 単一スロットだと同じフレームの 2 回目の要求が上書きで消えるため列で持つ。
    std::vector<OneShotRequest> m_pendingOneShots;
    /// @note 1 フレームに積める上限。AudioSystem は SimOnly なので、Edit モードで
    /// @note 誰も引き取らないまま積み続けても膨らまないように蓋をする。
    static constexpr size_t MAX_PENDING_ONE_SHOTS = 16;

    const char* GetTypeName() const { return "Audio Source"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.Field("clipPath", clipPath);
        r.Field("playOnAwake", playOnAwake);
        r.Field("loop", loop);
        r.Field("streaming", streaming);
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
        r.BeginField("airAbsorption", "airAbsorption");
        r.FloatRange("airAbsorption", airAbsorption, 0.0f, 1.0f);
        r.Tooltip("遠ざかるほど高域を吸わせる量 (0 で無加工)");
        r.EndField();
        r.BeginField("dopplerLevel", "dopplerLevel");
        r.FloatRange("dopplerLevel", dopplerLevel, 0.0f, 1.0f);
        r.Tooltip("相対速度によるピッチ変化 (0 で無効、1 で物理どおり)");
        r.EndField();
        r.BeginField("priority", "priority");
        r.Field("priority", priority);
        r.Tooltip("同時発音数が上限に達したとき、低い音から畳まれる");
        r.EndField();
    }
};

} /// @note namespace fbzz::scene
