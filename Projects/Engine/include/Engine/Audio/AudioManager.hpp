/// @file    AudioManager.hpp
/// @brief   クリップの読み込み・合成・寿命管理とミキサーバスの高レベル API。
/// @author  Hasegawa Jin
/// @date    2025-01-01
#pragma once
#include "AudioBus.hpp"
#include "IAudioDevice.hpp"
#include "SynthSpec.hpp"
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace fbzz::audio
{

/// IAudioDevice に実再生を委譲し、上位はパスかクリップハンドルだけを扱う。
///
/// クリップは 2 系統ある:
///   - パス由来 (.wav / .mp3 / .ogg / .flac / .synth) — 一度読んだら Shutdown まで保持
///   - 実行時生成 (SynthSpec) — 参照と再生が両方 0 になった時点で解放
///
/// スレッド規約: すべてメインスレッド専用。AudioSystem と同じフェーズから呼ぶこと。
class AudioManager
{
public:
    /// クリップハンドル。0 は無効。
    using ClipId = uint32_t;

    // device の所有権は呼び出し元が保持。Init より前に渡す
    explicit AudioManager(IAudioDevice& device);

    bool Init();
    void Shutdown();

    /// フェードを進め、終了した voice を回収し、参照の切れた生成クリップを解放する。
    /// Application が毎フレーム呼ぶ。呼ばないとフェードが進まず、生成クリップも残る。
    void Update(float dt);

    /// 同時に鳴らせる voice の上限。超えた状態で鳴らそうとすると、より優先度の低い
    /// 音を畳んで場所を空ける (空けられなければ新しい音の方を捨てる)。
    void SetVoiceLimit(size_t limit);
    [[nodiscard]] size_t VoiceLimit() const { return m_voiceLimit; }
    [[nodiscard]] size_t ActiveVoiceCount() const { return m_voices.size(); }

    /// @name ミキサーバス
    ///@{
    /// バス構成を作り直す。再生中の音はすべて止まる。
    /// WHY 止まるか: 送り先の submix を破棄するため。構成変更は設定画面や
    ///     プロジェクト読み込みの境界でしか起きない想定。
    void ApplyBusLayout(const std::vector<BusDesc>& buses);
    /// 大文字小文字を無視して引く。見つからなければ kMasterBus。
    [[nodiscard]] BusIndex FindBus(std::string_view name) const;
    void  SetBusVolume(std::string_view name, float volume);
    [[nodiscard]] float GetBusVolume(std::string_view name) const;
    void  SetBusLowPass(std::string_view name, float normalizedCutoff);
    [[nodiscard]] const std::vector<BusDesc>& BusLayout() const { return m_busLayout; }

    /// AudioReverbZone の合成結果を、reverb を有効にした全バスへ流す。
    /// AudioSystem が毎フレーム呼ぶ。値が変わったフレームだけデバイスへ届く。
    void SetEnvironmentReverb(float wet, float decaySeconds, float highFrequencyRatio);
    ///@}

    /// @name クリップ
    ///@{
    /// パスからクリップを得る (読み込み済みなら再利用)。失敗時は 0。
    [[nodiscard]] ClipId AcquireClip(const std::string& path);
    /// spec を合成してクリップを得る。同じ spec は同じハンドルを返す。
    /// 呼び出し側は不要になったら ReleaseClip すること。
    [[nodiscard]] ClipId AcquireGeneratedClip(const SynthSpec& spec);
    /// 生成クリップの参照を 1 つ増やす。ハンドルを別の持ち主へ渡すときに使う。
    void AddClipRef(ClipId clip);
    /// 生成クリップの参照を 1 つ手放す。再生中なら終わるまで実体は残る。
    void ReleaseClip(ClipId clip);
    ///@}

    /// @name 再生
    ///@{
    /// 鳴らし始めるときだけ決まる設定。既定のままなら従来どおりの即時再生。
    struct PlayParams
    {
        /// 上限に達したとき、これより低い優先度の音を畳んで場所を空ける。
        /// 同値なら古い方が畳まれる。ループ音は畳まれない。
        int   priority      = 0;
        /// 0 から鳴らし始めて指定秒で通常音量まで上げる。0 なら即座に本来の音量。
        float fadeInSeconds = 0.0f;
    };

    /// Scene の AudioSourceComponent が所有する voice を個別に操作する低レベル API。
    /// WHY voiceId を返すか: 複数の 3D 音源を同時追跡するため、共有状態では足りない。
    [[nodiscard]] uint32_t PlayVoice(const std::string& path, bool loop, BusIndex bus,
                                     const PlayParams& params = {});
    [[nodiscard]] uint32_t PlayClipVoice(ClipId clip, bool loop, BusIndex bus,
                                         const PlayParams& params = {});

    /// 既定の短いフェードを掛けてから畳む。
    /// WHY 即断しないか: 波形の途中で切ると段差が生まれ、「プツッ」と鳴る。
    ///     呼んだ側から見た voiceId はこの時点で無効になり、実体の後始末は
    ///     Update が引き取る (フェード中も PCM は保持される)。
    void StopVoice(uint32_t voiceId);
    /// フェードを挟まず即座に畳む。ライフサイクル境界とボイススティール用。
    void StopVoiceImmediate(uint32_t voiceId);
    /// 現在のフェードゲインから toGain まで動かす。SetVoiceVolume が毎フレーム
    /// 別の音量を書き込んでも、フェードは掛かったまま残る (両者は掛け算)。
    void FadeVoice(uint32_t voiceId, float toGain, float seconds);
    /// フェードし切ったところで畳む。voiceId はこの時点で呼び出し側から見て無効。
    void FadeOutAndStop(uint32_t voiceId, float seconds);

    /// 再生位置を保ったまま止める / 続きから再開する。
    void PauseVoice(uint32_t voiceId);
    void ResumeVoice(uint32_t voiceId);
    /// EditorのPlay終了など、Scene Audioを一括停止するライフサイクル境界で使用する。
    void StopAllVoices();
    /// 上位が意図する音量。実際にデバイスへ届くのは、これにフェードゲインを掛けた値。
    void SetVoiceVolume(uint32_t voiceId, float volume);
    void SetVoicePitch(uint32_t voiceId, float pitch);
    void SetVoicePan(uint32_t voiceId, float pan);
    void SetVoiceLowPass(uint32_t voiceId, float normalizedCutoff);
    [[nodiscard]] bool IsVoicePlaying(uint32_t voiceId);
    ///@}

    /// @name BGM / SE のショートハンド
    /// それぞれ "BGM" / "SE" バスへ流す。バスが無ければ Master。
    ///@{
    /// @param fadeSeconds 0 より大きいと、鳴っていた BGM を消しながら新しい BGM を
    ///        立ち上げる (クロスフェード)。曲の切り替わりで音が途切れない。
    void PlayBGM(const std::string& path, bool loop = true, float fadeSeconds = 0.0f);
    void StopBGM(float fadeSeconds = 0.0f);
    void PlaySE(const std::string& path);
    void SetBGMVolume(float volume);  // 0.0f ~ 1.0f
    void SetSEVolume(float volume);
    [[nodiscard]] float GetBGMVolume() const;
    [[nodiscard]] float GetSEVolume() const;
    [[nodiscard]] bool  IsBGMPlaying() const { return m_bgmVoiceId != 0; }
    ///@}

    /// @name 位置を指定した使い捨て再生
    /// 距離減衰とパンの計算は Listener を知っている AudioSystem が行う。
    /// WHY ここで計算しないか: 受聴点の選び方 (priority / 有効判定) が
    ///     AudioSystem にあり、二重に書くと選ばれる Listener がずれる。
    ///@{
    struct PositionalRequest
    {
        std::string path;              ///< 空なら clip を使う
        ClipId      clip = 0;          ///< 参照を 1 つ握った状態で積まれる
        // WHY math::Vector3 を使わないか: audio モジュールは Math へ依存していない。
        //     3 つの float で足りるものに依存を増やさない。
        float x = 0.0f, y = 0.0f, z = 0.0f;
        float volume      = 1.0f;
        float minDistance = 1.0f;
        float maxDistance = 50.0f;
        float rolloff     = 1.0f;
        float airAbsorption = 0.0f;
        int   priority      = 0;
        BusIndex bus = kMasterBus;
    };
    void QueuePositional(PositionalRequest request);
    /// 溜まっている要求を引き取る (呼ぶと空になる)。AudioSystem が毎フレーム呼ぶ。
    [[nodiscard]] std::vector<PositionalRequest> TakePositional();
    ///@}

    /// 読み込み済みクリップの中身を覗く。波形表示など、再生以外の用途向け。
    struct ClipInfo
    {
        WaveFormat     fmt{};
        const uint8_t* pcm   = nullptr;   ///< 次にこのクリップが解放されるまで有効
        size_t         bytes = 0;
        float          durationSeconds = 0.0f;
    };
    [[nodiscard]] bool DescribeClip(ClipId clip, ClipInfo& out) const;

private:
    struct ClipEntry
    {
        WaveFormat           fmt;
        std::vector<uint8_t> pcm;
        int                  refCount   = 0;
        int                  voiceCount = 0;
        // パス由来。Shutdown まで保持し、参照が 0 でも捨てない。
        bool                 persistent = false;
        // 生成クリップの重複排除キー (persistent なら 0)。
        uint64_t             specHash   = 0;
    };

    /// 鳴っている 1 本。デバイスへ送る音量は volume * fadeGain。
    /// WHY 2 つに分けるか: AudioSystem は毎フレーム距離減衰から音量を計算し直して
    ///     書き込む。フェードを同じ変数へ書くと、次のフレームで消える。
    struct VoiceState
    {
        ClipId   clip      = 0;
        float    volume    = 1.0f;
        float    fadeGain  = 1.0f;
        float    fadeFrom  = 1.0f;
        float    fadeTo    = 1.0f;
        float    elapsed   = 0.0f;
        float    duration  = 0.0f;   ///< 0 ならフェード中でない
        bool     stopAtEnd = false;
        bool     loop      = false;
        int      priority  = 0;
        uint64_t sequence  = 0;      ///< 同優先度なら小さい方 (古い方) から畳む
    };

    /// 上限に達していれば 1 本畳んで場所を空ける。
    /// @ret 場所を空けられたか。false なら新しい音を鳴らさない。
    [[nodiscard]] bool MakeRoomForVoice(int priority);
    void ApplyVoiceGain(uint32_t voiceId, const VoiceState& state);

    [[nodiscard]] ClipId InternClip(const WaveFormat& fmt, std::vector<uint8_t>&& pcm,
                                    bool persistent, uint64_t specHash);
    [[nodiscard]] bool LoadClipData(const std::string& path,
                                    WaveFormat& outFmt, std::vector<uint8_t>& outPcm);
    [[nodiscard]] bool LoadWav(const std::string& path,
                               WaveFormat& outFmt, std::vector<uint8_t>& outPcm);
    [[nodiscard]] bool LoadWithMediaFoundation(const std::string& path,
                                               WaveFormat& outFmt, std::vector<uint8_t>& outPcm);
    [[nodiscard]] bool LoadSynth(const std::string& path,
                                 WaveFormat& outFmt, std::vector<uint8_t>& outPcm);

    void ForgetVoice(uint32_t voiceId);
    void DropClipIfUnused(ClipId clip);

    IAudioDevice&        m_device;
    std::vector<BusDesc> m_busLayout;

    uint32_t m_bgmVoiceId = 0;

    ClipId                                  m_nextClipId = 1;
    std::unordered_map<ClipId, ClipEntry>   m_clips;
    std::unordered_map<std::string, ClipId> m_pathToClip;
    std::unordered_map<uint64_t, ClipId>    m_specToClip;
    // 鳴っている voice。終了を検出してクリップの参照を戻すためにも要る。
    std::unordered_map<uint32_t, VoiceState> m_voices;
    uint64_t                                m_nextVoiceSequence = 1;
    size_t                                  m_voiceLimit = 48;
    // reverb 有効バスへ最後に送った値。同じ値を送り続けないための記録。
    float                                   m_reverbWet     = -1.0f;
    float                                   m_reverbDecay   = 0.0f;
    float                                   m_reverbHfRatio = 1.0f;
    size_t                                  m_generatedBytes = 0;
    std::vector<PositionalRequest>          m_positional;
};

} // namespace fbzz::audio
