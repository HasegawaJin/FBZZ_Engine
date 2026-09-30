/// @file    ScriptAudioProxy.hpp
/// @brief   Script から音声再生と手続き効果音の生成を行うショートハンド。
/// @author  Hasegawa Jin
/// @date    2025-01-01
///
/// @note 手続き生成はパラメーターから PCM を合成するので、素材を用意せずに
/// @note        Mutate だけで「毎回わずかに違う」音を作れる。.synth を基準にスクリプトで
/// @note        揺らすのが基本の使い方。詳細は Docs/design/audio-system.md。
#pragma once

#include <Engine/Audio/SynthSpec.hpp>
#include <Engine/Scene/ScriptAssetRef.hpp>
#include <Math/Vector3.hpp>
#include <Engine/Scene/Entity.hpp>
#include <memory>
#include <cstdint>
#include <string>
#include <string_view>

namespace fbzz::audio { struct VoiceLifetime; }

namespace fbzz::scene {

class Script;

/// @note  実行時に合成したクリップのハンドル。
///
/// @note  Play を抜けると無効になる。Play→Stop では合成済み PCM をまとめて捨てるため、
/// @note  メンバーに持ち越さず OnStart で作り直すこと。
/// @note Play の終わりでスクリプト DLL ごと作り直されるため、持ち越すとハンドルの
/// @note        持ち主が消えて誰も解放できなくなる。
struct SynthClip {
    uint32_t id = 0;
    [[nodiscard]] bool IsValid() const { return id != 0; }
};

/// @brief 所有 Script と音源の寿命が有効な間だけ操作できるループ音のハンドル。
class AudioLoop {
public:
    [[nodiscard]] bool IsValid() const;
private:
    EntityID m_source = EntityID::INVALID;
    std::weak_ptr<audio::VoiceLifetime> m_lifetime;
    const Script* m_owner = nullptr;
    friend struct ScriptAudioProxy;
};

struct AudioLoopSettings {
    std::string label = "Loop";
    std::string bus = "SE";
    float spatialBlend = 1.0f;
    float minDistance = 2.0f;
    float maxDistance = 22.0f;
};

struct ScriptAudioProxy {
    Script* script = nullptr;

    /// @brief 独立したループ音を開始・更新する。同一クリップの更新では再生位置を戻さない。
    /// @note 主 AudioSource は不要。音源は所有 GameObject に追従し、名前で共有しない。
    /// @note 空パス・volume <= 0.001 は停止。非有限値・別所有者のハンドルは false、変更なし。
    /// @note Script の無効化・破棄・再読み込みで失効する。再有効化後は新たに要求する。
    [[nodiscard]] bool UpdateLoop(AudioLoop& loop, std::string_view clipPath, float volume,
                                  float pitch = 1.0f, const AudioLoopSettings& settings = {}) const;
    [[nodiscard]] bool UpdateLoop(AudioLoop& loop, const AudioClipRef& clip, float volume,
                                  float pitch = 1.0f, const AudioLoopSettings& settings = {}) const;
    /// @note 停止したハンドルは同じ所有者から再利用できる。停止要求は次の AudioSystem 更新で反映する。
    bool StopLoop(const AudioLoop& loop) const;
    /// @note 再利用しない音源を手放す。古いハンドルのコピーも失効する。
    void ReleaseLoop(AudioLoop& loop) const;

    /// @note  再生せず同期読み込みする。AudioSource 不要、voice 枠を消費しない。
    /// @note  パス由来の PCM は AudioManager 終了まで保持。失敗は false とログで通知する。
    /// @note  メインスレッド専用。読み込み対象とタイミングは呼び出し側が決める。
    [[nodiscard]] bool Preload(std::string_view clipPath) const;
    [[nodiscard]] bool Preload(const AudioClipRef& clip) const;

    /// @name クリップ再生 (自 GameObject の AudioSource 経由)
    ///
    /// @note  AudioSource が付いていない GameObject では、これらは何もせず
    /// @note  Console へ一度だけ警告を出す (無言で鳴らないのを防ぐため)。
    ///@{
    /// @note  Inspector で設定済みの clipPath を先頭から鳴らす。
    /// @note  「シーンでオーサリングし、スクリプトはきっかけだけ与える」用。
    void Play() const;
    void Play(std::string_view clipPath) const;
    void Play(const AudioClipRef& clip) const;
    void Stop() const;
    /// @note  再生位置を保ったまま止める。Resume() で続きから鳴る。
    void Pause() const;
    /// @note  Pause() した音を続きから再開する。停止中・未再生なら何もしない。
    void Resume() const;
    void SetVolume(float v) const;
    void SetLoop(bool loop) const;

    bool  IsPlaying() const;
    float GetVolume() const;
    void  SetPitch(float pitch) const;
    float GetPitch() const;
    bool  GetLoop() const;
    /// @note  Pause() 中は IsPlaying() が false になるため、「止めた」と「一時停止した」の
    /// @note  区別が要る側 (ポーズ解除で鳴らし直すか、Resume するか) はこちらも見る。
    bool  IsPaused() const;
    void  SetSpatialBlend(float blend) const;
    float GetSpatialBlend() const;
    void  Set3DDistances(float minDistance, float maxDistance, float rolloff = 1.0f) const;
    /// @note  主 voice を止めずに 1 度だけ重ねて鳴らす。同じフレームに何度呼んでも
    /// @note  すべて鳴る (被弾が同時多発しても取りこぼさない)。
    /// @param volumeScale AudioSource の volume に掛ける倍率。強弱を付ける用。
    void  PlayOneShot(std::string_view clipPath, float volumeScale = 1.0f) const;
    void  PlayOneShot(const AudioClipRef& clip, float volumeScale = 1.0f) const;
    ///@}

    /// @name 位置を指定した使い捨て再生
    /// @note  AudioSource を持たないオブジェクトの音や、発生源が既に消えている音
    /// @note  (弾着・撃破) 向け。距離減衰とパンは呼び出し時点の Listener を基準に
    /// @note  一度だけ計算して焼き込む。鳴っている間に音像は動かない。
    ///@{
    void PlayAtPoint(std::string_view clipPath, const math::Vector3& position,
                     float volume = 1.0f) const;
    void PlayAtPoint(const AudioClipRef& clip, const math::Vector3& position,
                     float volume = 1.0f) const;
    void PlayClipAtPoint(SynthClip clip, const math::Vector3& position,
                         float volume = 1.0f) const;
    ///@}

    /// @name BGM
    /// @note  常に 1 本だけ鳴る専用スロット。AudioSource を経由しないので
    /// @note  シーンにオブジェクトを置かなくても切り替えられる。
    ///@{
    /// @param fadeSeconds 0 より大きいと、鳴っていた曲を消しながら新しい曲を
    /// @note         立ち上げる (クロスフェード)。場面転換で音が途切れない。
    void PlayBGM(std::string_view clipPath, bool loop = true, float fadeSeconds = 0.0f) const;
    void PlayBGM(const AudioClipRef& clip, bool loop = true, float fadeSeconds = 0.0f) const;
    void StopBGM(float fadeSeconds = 0.0f) const;
    [[nodiscard]] bool IsBGMPlaying() const;
    ///@}

    /// @name フェード (自 GameObject の主 voice)
    /// @note  環境音のループを場面に合わせて出し入れする用。AudioSource の volume とは
    /// @note  別枠で掛かるので、距離減衰や遮蔽の計算と喧嘩しない。
    ///@{
    void FadeTo(float gain, float seconds) const;
    /// @note  フェードし切ったところで停止する。呼んだ時点で IsPlaying() は false になる。
    void FadeOutAndStop(float seconds) const;
    ///@}

    /// @name 手続き効果音
    ///@{
    /// @note  プリセットの基準パラメーター。seed が 0 以外なら派生を返す。
    [[nodiscard]] audio::SynthSpec MakeSpec(audio::SynthPreset preset, uint32_t seed = 0) const;
    /// @note  base を ±amount の割合で揺らした派生。amount は 0-1。
    [[nodiscard]] audio::SynthSpec MutateSpec(const audio::SynthSpec& base,
                                              float amount, uint32_t seed) const;
    /// @note  .synth アセットのパラメーターを可変コピーとして取り出す。
    /// @return 読めなければ false。out は変更しない。
    [[nodiscard]] bool LoadSpec(const AudioClipRef& synthAsset, audio::SynthSpec& out) const;
    [[nodiscard]] bool LoadSpec(std::string_view synthAssetPath, audio::SynthSpec& out) const;

    /// @note  spec を合成してハンドルを得る。同じ spec は同じハンドルを返すので、
    /// @note  繰り返し鳴らす音は OnStart で 1 度作って使い回す。
    [[nodiscard]] SynthClip Synthesize(const audio::SynthSpec& spec) const;
    [[nodiscard]] SynthClip Synthesize(audio::SynthPreset preset, uint32_t seed = 0) const;
    /// @note  不要になったハンドルを手放す。呼ばなくても Play 終了時にまとめて解放される。
    void ReleaseClip(SynthClip clip) const;

    /// @note  生成済みクリップを自 GameObject の AudioSource で鳴らす (3D 減衰・遮蔽が効く)。
    void PlayClip(SynthClip clip) const;
    /// @note  主 voice として鳴らす。AudioSource の loop 設定に従う。
    void PlayClipAsSource(SynthClip clip) const;

    /// @note  合成して即座に 1 度だけ鳴らす。使い捨ての音向け。
    void PlaySynth(const audio::SynthSpec& spec) const;
    void PlaySynth(audio::SynthPreset preset, uint32_t seed = 0) const;

    /// @note  Listener に依存しない 2D 再生。AudioSource を持たない UI やシステム音向け。
    void PlaySynth2D(const audio::SynthSpec& spec, std::string_view bus = "UI") const;
    void PlayClip2D(SynthClip clip, std::string_view bus = "UI") const;
    ///@}

    /// @name ミキサーバス
    /// @note  バス名は Project Settings > Audio で定義したもの。未知の名前は Master へ落ちる。
    ///@{
    /// @note  自 GameObject の AudioSource の出力先を変える。
    void  SetBus(std::string_view bus) const;
    [[nodiscard]] std::string_view GetBus() const;
    void  SetBusVolume(std::string_view bus, float volume) const;
    [[nodiscard]] float GetBusVolume(std::string_view bus) const;
    /// @note  バス全体に低域通過を掛ける (0-1、1 で無加工)。水中や気絶の表現に使う。
    void  SetBusLowPass(std::string_view bus, float normalizedCutoff) const;
    ///@}
};

} /// @note namespace fbzz::scene
