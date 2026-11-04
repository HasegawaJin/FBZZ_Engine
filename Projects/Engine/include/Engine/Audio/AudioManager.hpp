/// @file    AudioManager.hpp
/// @brief   クリップの読み込み・合成・寿命管理とミキサーバスの高レベル API。
/// @author  Hasegawa Jin
/// @date    2025-01-01
#pragma once
#include <Engine/Api.hpp>
#include "AudioBus.hpp"
#include "IAudioDevice.hpp"
#include "SynthSpec.hpp"
#include "VoiceLifetime.hpp"
#include <memory>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace fbzz::audio
{

/// @note IAudioDevice に実再生を委譲し、上位はパスかクリップハンドルだけを扱う。

/// @note クリップは 2 系統ある:
/// @note - パス由来 (.wav / .mp3 / .ogg / .flac / .synth) — 一度読んだら Shutdown まで保持
/// @note - 実行時生成 (SynthSpec) — 参照と再生が両方 0 になった時点で解放

/// @note スレッド規約: すべてメインスレッド専用。AudioSystem と同じフェーズから呼ぶこと。
class AudioManager
{
public:
    /// @note クリップハンドル。0 は無効。
    using ClipId = uint32_t;

    /// @note device の所有権は呼び出し元が保持する。Init より前に渡すこと。
    FBZZ_ENGINE_API explicit AudioManager(IAudioDevice& device);

    FBZZ_ENGINE_API bool Init();
    FBZZ_ENGINE_API void Shutdown();

    /// @note フェードを進め、終了した voice を回収し、参照の切れた生成クリップを解放する。
    /// @note Application が毎フレーム呼ぶ。呼ばないとフェードが進まず、生成クリップも残る。
    FBZZ_ENGINE_API void Update(float dt);

    /// @note 同時に鳴らせる voice の上限。超えた状態で鳴らそうとすると、より優先度の低い
    /// @note 音を畳んで場所を空ける (空けられなければ新しい音の方を捨てる)。
    FBZZ_ENGINE_API void SetVoiceLimit(size_t limit);
    [[nodiscard]] size_t VoiceLimit() const { return m_voiceLimit; }
    [[nodiscard]] size_t ActiveVoiceCount() const { return m_voices.size(); }

    /// @name ミキサーバス
    /// @{
    /// @note バス構成を作り直す。再生中の音はすべて止まる。
    /// @note 送り先の submix を破棄するため。構成変更は設定画面やプロジェクト読み込みの境界でしか起きない想定。
    FBZZ_ENGINE_API void ApplyBusLayout(const std::vector<BusDesc>& buses);
    /// @note 大文字小文字を無視して引く。見つからなければ kMasterBus。
    [[nodiscard]] FBZZ_ENGINE_API BusIndex FindBus(std::string_view name) const;
    FBZZ_ENGINE_API void  SetBusVolume(std::string_view name, float volume);
    [[nodiscard]] FBZZ_ENGINE_API float GetBusVolume(std::string_view name) const;
    FBZZ_ENGINE_API void  SetBusLowPass(std::string_view name, float normalizedCutoff);
    [[nodiscard]] const std::vector<BusDesc>& BusLayout() const { return m_busLayout; }

    /// @note AudioReverbZone の合成結果を、reverb を有効にした全バスへ流す。
    /// @note AudioSystem が毎フレーム呼ぶ。値が変わったフレームだけデバイスへ届く。
    FBZZ_ENGINE_API void SetEnvironmentReverb(float wet, float decaySeconds, float highFrequencyRatio);
    /// @}

    /// @name クリップ
    /// @{
    /// @note パスからクリップを得る (読み込み済みなら再利用)。失敗時は 0。
    [[nodiscard]] FBZZ_ENGINE_API ClipId AcquireClip(const std::string& path);
    /// @note spec を合成してクリップを得る。同じ spec は同じハンドルを返す。
    /// @note 呼び出し側は不要になったら ReleaseClip すること。
    [[nodiscard]] FBZZ_ENGINE_API ClipId AcquireGeneratedClip(const SynthSpec& spec);
    /// @note 生成クリップの参照を 1 つ増やす。ハンドルを別の持ち主へ渡すときに使う。
    FBZZ_ENGINE_API void AddClipRef(ClipId clip);
    /// @note 生成クリップの参照を 1 つ手放す。再生中なら終わるまで実体は残る。
    FBZZ_ENGINE_API void ReleaseClip(ClipId clip);
    /// @}

    /// @name 再生
    /// @{
    /// @note 鳴らし始めるときだけ決まる設定。既定のままなら従来どおりの即時再生。
    struct PlayParams
    {
        /// @note 上限に達したとき、これより低い優先度の音を畳んで場所を空ける。
        /// @note 同値なら古い方が畳まれる。ループ音は畳まれない。
        int   priority      = 0;
        /// @note 0 から鳴らし始めて指定秒で通常音量まで上げる。0 なら即座に本来の音量。
        float fadeInSeconds = 0.0f;
        /// @brief true なら PCM 全体をキャッシュせず分割供給する。.synth は生成クリップを使う。
        bool streaming = false;
    };

    /// @note Scene の AudioSourceComponent が所有する voice を個別に操作する低レベル API。
    /// @note 複数の 3D 音源を同時追跡するため voiceId を返す。共有状態では足りない。
    [[nodiscard]] FBZZ_ENGINE_API uint32_t PlayVoice(const std::string& path, bool loop, BusIndex bus,
                                     const PlayParams& params = {});
    [[nodiscard]] FBZZ_ENGINE_API uint32_t PlayClipVoice(ClipId clip, bool loop, BusIndex bus,
                                         const PlayParams& params = {});

    /// @note 既定の短いフェードを掛けてから畳む。
    /// @note 波形の途中で切ると段差が生まれ「プツッ」と鳴るため即断しない。呼んだ側から見た voiceId はこの時点で無効になり、実体の後始末は Update が引き取る (フェード中も PCM は保持される)。
    FBZZ_ENGINE_API void StopVoice(uint32_t voiceId);
    /// @note フェードを挟まず即座に畳む。ライフサイクル境界とボイススティール用。
    FBZZ_ENGINE_API void StopVoiceImmediate(uint32_t voiceId);
    /// @note 現在のフェードゲインから toGain まで動かす。SetVoiceVolume が毎フレーム
    /// @note 別の音量を書き込んでも、フェードは掛かったまま残る (両者は掛け算)。
    FBZZ_ENGINE_API void FadeVoice(uint32_t voiceId, float toGain, float seconds);
    /// @note フェードし切ったところで畳む。voiceId はこの時点で呼び出し側から見て無効。
    FBZZ_ENGINE_API void FadeOutAndStop(uint32_t voiceId, float seconds);

    /// @note 再生位置を保ったまま止める / 続きから再開する。
    FBZZ_ENGINE_API void PauseVoice(uint32_t voiceId);
    FBZZ_ENGINE_API void ResumeVoice(uint32_t voiceId);
    /// @note EditorのPlay終了など、Scene Audioを一括停止するライフサイクル境界で使用する。
    FBZZ_ENGINE_API void StopAllVoices();
    /// @note 所有トークンの失効後、次の Update で停止する。AudioSource が消えた後も回収できる。
    FBZZ_ENGINE_API void BindVoiceLifetime(uint32_t voiceId, const std::shared_ptr<VoiceLifetime>& lifetime);
    /// @note 上位が意図する音量。実際にデバイスへ届くのは、これにフェードゲインを掛けた値。
    FBZZ_ENGINE_API void SetVoiceVolume(uint32_t voiceId, float volume);
    FBZZ_ENGINE_API void SetVoicePitch(uint32_t voiceId, float pitch);
    FBZZ_ENGINE_API void SetVoicePan(uint32_t voiceId, float pan);
    /// @brief 空文字または存在しないバス名は補助センドを解除する。
    FBZZ_ENGINE_API void SetVoiceSend(uint32_t voiceId, std::string_view busName, float level);
    FBZZ_ENGINE_API void SetVoiceLowPass(uint32_t voiceId, float normalizedCutoff);
    [[nodiscard]] FBZZ_ENGINE_API bool IsVoicePlaying(uint32_t voiceId);
    /// @}

    /// @name BGM / SE のショートハンド
    /// @note それぞれ "BGM" / "SE" バスへ流す。バスが無ければ Master。
    /// @{
    /// @param fadeSeconds 0 より大きいと、鳴っていた BGM を消しながら新しい BGM を
    /// @note 立ち上げる (クロスフェード)。曲の切り替わりで音が途切れない。
    FBZZ_ENGINE_API void PlayBGM(const std::string& path, bool loop = true, float fadeSeconds = 0.0f);
    FBZZ_ENGINE_API void StopBGM(float fadeSeconds = 0.0f);
    FBZZ_ENGINE_API void PlaySE(const std::string& path);
    FBZZ_ENGINE_API void SetBGMVolume(float volume);  ///< @note 0.0f ~ 1.0f
    FBZZ_ENGINE_API void SetSEVolume(float volume);
    [[nodiscard]] FBZZ_ENGINE_API float GetBGMVolume() const;
    [[nodiscard]] FBZZ_ENGINE_API float GetSEVolume() const;
    [[nodiscard]] bool  IsBGMPlaying() const { return m_bgmVoiceId != 0; }
    /// @}

    /// @name 位置を指定した使い捨て再生
    /// @note 距離減衰とパンの計算は Listener を知っている AudioSystem が行う。
    /// @note 受聴点の選び方 (priority/有効判定) が AudioSystem にあり、二重に書くと選ばれる Listener がずれる。
    /// @{
    struct PositionalRequest
    {
        std::string path;              ///< @note 空なら clip を使う
        ClipId      clip = 0;          ///< @note 参照を 1 つ握った状態で積まれる
        /// @note audio モジュールは Math へ依存していないため math::Vector3 を使わない。3 つの float で足りるものに依存を増やさない。
        float x = 0.0f, y = 0.0f, z = 0.0f;
        float volume      = 1.0f;
        float minDistance = 1.0f;
        float maxDistance = 50.0f;
        float rolloff     = 1.0f;
        float airAbsorption = 0.0f;
        int   priority      = 0;
        BusIndex bus = kMasterBus;
    };
    FBZZ_ENGINE_API void QueuePositional(PositionalRequest request);
    /// @note 溜まっている要求を引き取る (呼ぶと空になる)。AudioSystem が毎フレーム呼ぶ。
    [[nodiscard]] FBZZ_ENGINE_API std::vector<PositionalRequest> TakePositional();
    /// @}

    /// @note 読み込み済みクリップの中身を覗く。波形表示など、再生以外の用途向け。
    struct ClipInfo
    {
        WaveFormat     fmt{};
        const uint8_t* pcm   = nullptr;   ///< @note 次にこのクリップが解放されるまで有効
        size_t         bytes = 0;
        float          durationSeconds = 0.0f;
    };
    [[nodiscard]] FBZZ_ENGINE_API bool DescribeClip(ClipId clip, ClipInfo& out) const;

private:
    struct ClipEntry
    {
        WaveFormat           fmt;
        std::vector<uint8_t> pcm;
        int                  refCount   = 0;
        int                  voiceCount = 0;
        bool                 persistent = false;   ///< @note パス由来。Shutdown まで保持し、参照が 0 でも捨てない。
        uint64_t             specHash   = 0;        ///< @note 生成クリップの重複排除キー (persistent なら 0)。
    };

    /// @note 鳴っている 1 本。デバイスへ送る音量は volume * fadeGain。
    /// @note AudioSystem は毎フレーム距離減衰から音量を計算し直して書き込むため、フェードは別変数に分ける。同じ変数へ書くと次フレームで消える。
    struct VoiceState
    {
        std::weak_ptr<VoiceLifetime> lifetime;
        bool lifetimeBound = false;
        ClipId   clip      = 0;
        float    volume    = 1.0f;
        float    fadeGain  = 1.0f;
        float    fadeFrom  = 1.0f;
        float    fadeTo    = 1.0f;
        float    elapsed   = 0.0f;
        float    duration  = 0.0f;   ///< @note 0 ならフェード中でない
        bool     stopAtEnd = false;
        bool     loop      = false;
        int      priority  = 0;
        uint64_t sequence  = 0;      ///< @note 同優先度なら小さい方 (古い方) から畳む
    };

    /// @note 上限に達していれば 1 本畳んで場所を空ける。
    /// @return 場所を空けられたか。false なら新しい音を鳴らさない。
    [[nodiscard]] FBZZ_ENGINE_API bool MakeRoomForVoice(int priority);
    FBZZ_ENGINE_API void ApplyVoiceGain(uint32_t voiceId, const VoiceState& state);

    [[nodiscard]] FBZZ_ENGINE_API ClipId InternClip(const WaveFormat& fmt, std::vector<uint8_t>&& pcm,
                                    bool persistent, uint64_t specHash);
    [[nodiscard]] FBZZ_ENGINE_API bool LoadClipData(const std::string& path,
                                    WaveFormat& outFmt, std::vector<uint8_t>& outPcm);
    [[nodiscard]] FBZZ_ENGINE_API bool LoadWav(const std::string& path,
                               WaveFormat& outFmt, std::vector<uint8_t>& outPcm);
    [[nodiscard]] FBZZ_ENGINE_API bool LoadWithMediaFoundation(const std::string& path,
                                               WaveFormat& outFmt, std::vector<uint8_t>& outPcm);
    [[nodiscard]] FBZZ_ENGINE_API bool LoadSynth(const std::string& path,
                                 WaveFormat& outFmt, std::vector<uint8_t>& outPcm);

    FBZZ_ENGINE_API void ForgetVoice(uint32_t voiceId);
    FBZZ_ENGINE_API void DropClipIfUnused(ClipId clip);

    IAudioDevice&        m_device;
    std::vector<BusDesc> m_busLayout;

    uint32_t m_bgmVoiceId = 0;

    ClipId                                  m_nextClipId = 1;
    std::unordered_map<ClipId, ClipEntry>   m_clips;
    std::unordered_map<std::string, ClipId> m_pathToClip;
    std::unordered_map<uint64_t, ClipId>    m_specToClip;
    std::unordered_map<uint32_t, VoiceState> m_voices;   ///< @note 鳴っている voice。終了を検出してクリップの参照を戻すためにも要る。
    uint64_t                                m_nextVoiceSequence = 1;
    size_t                                  m_voiceLimit = 48;
    float                                   m_reverbWet     = -1.0f;   ///< @note reverb 有効バスへ最後に送った値。同じ値を送り続けないための記録。
    float                                   m_reverbDecay   = 0.0f;
    float                                   m_reverbHfRatio = 1.0f;
    size_t                                  m_generatedBytes = 0;
    std::vector<PositionalRequest>          m_positional;
};

} /// @note namespace fbzz::audio
