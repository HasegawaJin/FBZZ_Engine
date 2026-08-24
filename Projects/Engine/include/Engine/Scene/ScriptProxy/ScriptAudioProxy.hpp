/// @file    ScriptAudioProxy.hpp
/// @brief   Script から音声再生と手続き効果音の生成を行うショートハンド。
/// @author  Hasegawa Jin
/// @date    2025-01-01
///
/// 設計意図 (WHY 手続き生成をここへ置くか):
///   録音素材を 1 つも用意しなくても効果音を鳴らせるようにするため。パラメーターから
///   PCM を合成できると、1 つのプリセットを Mutate するだけで「毎回わずかに違う」
///   足音や被弾音が作れる。素材の枚数ではなくコードで音のバリエーションを増やせる。
///
///   .synth アセットとして保存した音は clipPath で参照できるので、
///   「アセットで基準を決めて、スクリプトで毎回揺らす」が基本の使い方になる:
///
///     void OnStart() override {
///         if (!audio.LoadSpec(m_shotSynth, m_shotSpec))
///             m_shotSpec = audio.MakeSpec(audio::SynthPreset::Laser);
///     }
///     void Fire() {
///         audio.PlaySynth(audio.MutateSpec(m_shotSpec, 0.12f, random.Range(0, 9999)));
///     }
///
/// 詳細は Docs/design/audio-system.md。
#pragma once

#include <Engine/Audio/SynthSpec.hpp>
#include <Engine/Scene/ScriptAssetRef.hpp>
#include <Math/Vector3.hpp>
#include <cstdint>
#include <string>
#include <string_view>

namespace fbzz::scene {

class Script;

/// 実行時に合成したクリップのハンドル。
///
/// Play を抜けると無効になる。Play→Stop では合成済み PCM をまとめて捨てるため、
/// メンバーに持ち越さず OnStart で作り直すこと。
/// WHY 捨てるか: Play の終わりでスクリプト DLL ごと作り直されるので、
///     ハンドルの持ち主が消えて誰も解放できなくなる。
struct SynthClip {
    uint32_t id = 0;
    [[nodiscard]] bool IsValid() const { return id != 0; }
};

struct ScriptAudioProxy {
    Script* script = nullptr;

    /// @name クリップ再生 (自 GameObject の AudioSource 経由)
    ///
    /// AudioSource が付いていない GameObject では、これらは何もせず
    /// Console へ一度だけ警告を出す (無言で鳴らないのを防ぐため)。
    ///@{
    /// Inspector で設定済みの clipPath を先頭から鳴らす。
    /// 「シーンでオーサリングし、スクリプトはきっかけだけ与える」用。
    void Play() const;
    void Play(std::string_view clipPath) const;
    void Play(const AudioClipRef& clip) const;
    void Stop() const;
    /// 再生位置を保ったまま止める。Resume() で続きから鳴る。
    void Pause() const;
    /// Pause() した音を続きから再開する。停止中・未再生なら何もしない。
    void Resume() const;
    void SetVolume(float v) const;
    void SetLoop(bool loop) const;

    bool  IsPlaying() const;
    float GetVolume() const;
    void  SetPitch(float pitch) const;
    float GetPitch() const;
    bool  GetLoop() const;
    /// Pause() 中は IsPlaying() が false になるため、「止めた」と「一時停止した」の
    /// 区別が要る側 (ポーズ解除で鳴らし直すか、Resume するか) はこちらも見る。
    bool  IsPaused() const;
    void  SetSpatialBlend(float blend) const;
    float GetSpatialBlend() const;
    void  Set3DDistances(float minDistance, float maxDistance, float rolloff = 1.0f) const;
    /// 主 voice を止めずに 1 度だけ重ねて鳴らす。同じフレームに何度呼んでも
    /// すべて鳴る (被弾が同時多発しても取りこぼさない)。
    /// @param volumeScale AudioSource の volume に掛ける倍率。強弱を付ける用。
    void  PlayOneShot(std::string_view clipPath, float volumeScale = 1.0f) const;
    void  PlayOneShot(const AudioClipRef& clip, float volumeScale = 1.0f) const;
    ///@}

    /// @name 位置を指定した使い捨て再生
    /// AudioSource を持たないオブジェクトの音や、発生源が既に消えている音
    /// (弾着・撃破) 向け。距離減衰とパンは呼び出し時点の Listener を基準に
    /// 一度だけ計算して焼き込む。鳴っている間に音像は動かない。
    ///@{
    void PlayAtPoint(std::string_view clipPath, const math::Vector3& position,
                     float volume = 1.0f) const;
    void PlayAtPoint(const AudioClipRef& clip, const math::Vector3& position,
                     float volume = 1.0f) const;
    void PlayClipAtPoint(SynthClip clip, const math::Vector3& position,
                         float volume = 1.0f) const;
    ///@}

    /// @name BGM
    /// 常に 1 本だけ鳴る専用スロット。AudioSource を経由しないので
    /// シーンにオブジェクトを置かなくても切り替えられる。
    ///@{
    void PlayBGM(std::string_view clipPath, bool loop = true) const;
    void PlayBGM(const AudioClipRef& clip, bool loop = true) const;
    void StopBGM() const;
    [[nodiscard]] bool IsBGMPlaying() const;
    ///@}

    /// @name 手続き効果音
    ///@{
    /// プリセットの基準パラメーター。seed が 0 以外なら派生を返す。
    [[nodiscard]] audio::SynthSpec MakeSpec(audio::SynthPreset preset, uint32_t seed = 0) const;
    /// base を ±amount の割合で揺らした派生。amount は 0-1。
    [[nodiscard]] audio::SynthSpec MutateSpec(const audio::SynthSpec& base,
                                              float amount, uint32_t seed) const;
    /// .synth アセットのパラメーターを可変コピーとして取り出す。
    /// @ret 読めなければ false。out は変更しない。
    [[nodiscard]] bool LoadSpec(const AudioClipRef& synthAsset, audio::SynthSpec& out) const;
    [[nodiscard]] bool LoadSpec(std::string_view synthAssetPath, audio::SynthSpec& out) const;

    /// spec を合成してハンドルを得る。同じ spec は同じハンドルを返すので、
    /// 繰り返し鳴らす音は OnStart で 1 度作って使い回す。
    [[nodiscard]] SynthClip Synthesize(const audio::SynthSpec& spec) const;
    [[nodiscard]] SynthClip Synthesize(audio::SynthPreset preset, uint32_t seed = 0) const;
    /// 不要になったハンドルを手放す。呼ばなくても Play 終了時にまとめて解放される。
    void ReleaseClip(SynthClip clip) const;

    /// 生成済みクリップを自 GameObject の AudioSource で鳴らす (3D 減衰・遮蔽が効く)。
    void PlayClip(SynthClip clip) const;
    /// 主 voice として鳴らす。AudioSource の loop 設定に従う。
    void PlayClipAsSource(SynthClip clip) const;

    /// 合成して即座に 1 度だけ鳴らす。使い捨ての音向け。
    void PlaySynth(const audio::SynthSpec& spec) const;
    void PlaySynth(audio::SynthPreset preset, uint32_t seed = 0) const;

    /// Listener に依存しない 2D 再生。AudioSource を持たない UI やシステム音向け。
    void PlaySynth2D(const audio::SynthSpec& spec, std::string_view bus = "UI") const;
    void PlayClip2D(SynthClip clip, std::string_view bus = "UI") const;
    ///@}

    /// @name ミキサーバス
    /// バス名は Project Settings > Audio で定義したもの。未知の名前は Master へ落ちる。
    ///@{
    /// 自 GameObject の AudioSource の出力先を変える。
    void  SetBus(std::string_view bus) const;
    [[nodiscard]] std::string_view GetBus() const;
    void  SetBusVolume(std::string_view bus, float volume) const;
    [[nodiscard]] float GetBusVolume(std::string_view bus) const;
    /// バス全体に低域通過を掛ける (0-1、1 で無加工)。水中や気絶の表現に使う。
    void  SetBusLowPass(std::string_view bus, float normalizedCutoff) const;
    ///@}
};

} // namespace fbzz::scene
