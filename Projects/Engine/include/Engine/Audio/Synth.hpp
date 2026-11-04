/// @file    Synth.hpp
/// @brief   SynthSpec から PCM を合成する決定論的な効果音ジェネレーター。
/// @author  Hasegawa Jin
/// @date    2026-08-23
#pragma once
#include <Engine/Api.hpp>
#include "IAudioDevice.hpp"
#include "SynthSpec.hpp"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace fbzz::audio {

/// @note spec から 16bit モノラル PCM を生成する。
/// @param outFmt 生成した PCM の形式。spec.sampleRate が 0 なら 44100 を入れて返す。
/// @return  総再生長が 0 以下、または amplitude が 0 以下なら空。outFmt は未定義。
[[nodiscard]] FBZZ_ENGINE_API std::vector<uint8_t> Render(const SynthSpec& spec, WaveFormat& outFmt);

/// @note プリセットの基準 spec。seed が 0 以外なら Mutate を軽く掛けた派生を返す。
[[nodiscard]] FBZZ_ENGINE_API SynthSpec MakePreset(SynthPreset preset, uint32_t seed = 0);

/// @note base の各パラメーターを seed 由来の乱数で ±amount の割合だけ揺らす。
/// @note 足音や被弾音の「毎回わずかに違う」を作る用途。amount は 0-1 を想定。
[[nodiscard]] FBZZ_ENGINE_API SynthSpec Mutate(const SynthSpec& base, float amount, uint32_t seed);

/// @note 全パラメーターを seed からランダムに決める。Editor の Randomize 用。
[[nodiscard]] FBZZ_ENGINE_API SynthSpec Randomize(uint32_t seed);

/// @note PCM を RIFF/WAVE のバイト列にする。Editor の Export と実素材への焼き出しで使う。
[[nodiscard]] FBZZ_ENGINE_API std::vector<uint8_t> EncodeWav(
    const void* pcmData, size_t bytes, const WaveFormat& fmt);

/// @note 同じ spec が同じ値になるハッシュ。生成クリップの重複排除に使う。
[[nodiscard]] FBZZ_ENGINE_API uint64_t HashSpec(const SynthSpec& spec);

/// @note 合成結果の客観的な特徴量。

/// @note AI は音を聴けないため、パラメーターを動かした結果が重くなったか軽くなったかを判断する手がかりが要る。数値で返せば「brightnessHz を下げる」という目標に対して spec を詰められる。
/// @see Docs/design/audio-system.md §9
struct SynthAnalysis {
    float    durationSeconds   = 0.0f;
    float    peakAmplitude     = 0.0f;  ///< @note 0-1
    float    rmsAmplitude      = 0.0f;  ///< @note 0-1。体感音量に近い
    /// @note パワー加重の実効周波数 (Hz)。いわゆる「明るさ」。
    /// @note 窓付き FFT のスペクトル重心ではなく、一次差分から求める RMS 周波数。
    /// @note 低いほど「重い / こもった」音になる。
    float    brightnessHz      = 0.0f;
    /// @note 1 秒あたりのゼロ交差数。ノイズ性の指標で、同じ brightness でも
    /// @note ノイズ波形の方が高く出る。
    float    zeroCrossingHz    = 0.0f;
    /// @note ピークに達するまでの秒数。小さいほど打撃感が強い。
    float    timeToPeakSeconds = 0.0f;
    /// @note ±1 に張り付いたサンプルの割合 0-1。amplitude 過大の検出に使う。
    float    clippedRatio      = 0.0f;
    uint32_t sampleRate        = 0;
    uint32_t sampleCount       = 0;
};

/// @note spec を合成して特徴量を測る。合成を伴うので毎フレーム呼ぶ用途には向かない。
[[nodiscard]] FBZZ_ENGINE_API SynthAnalysis Analyze(const SynthSpec& spec);

/// @note 列挙と TOML / UI 用の文字列を相互変換する。未知の名前は fallback を返す。
[[nodiscard]] FBZZ_ENGINE_API const char* WaveToString(SynthWave wave);
[[nodiscard]] FBZZ_ENGINE_API SynthWave   WaveFromString(const char* name, SynthWave fallback);
[[nodiscard]] FBZZ_ENGINE_API const char* PresetToString(SynthPreset preset);
[[nodiscard]] FBZZ_ENGINE_API SynthPreset PresetFromString(const char* name, SynthPreset fallback);

} /// @note namespace fbzz::audio
