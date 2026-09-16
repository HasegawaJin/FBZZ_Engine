/// @file    Synth.hpp
/// @brief   SynthSpec から PCM を合成する決定論的な効果音ジェネレーター。
/// @author  Hasegawa Jin
/// @date    2026-08-23
#pragma once
#include "IAudioDevice.hpp"
#include "SynthSpec.hpp"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace fbzz::audio {

/// spec から 16bit モノラル PCM を生成する。
/// @param outFmt 生成した PCM の形式。spec.sampleRate が 0 なら 44100 を入れて返す。
/// @ret  総再生長が 0 以下、または amplitude が 0 以下なら空。outFmt は未定義。
[[nodiscard]] std::vector<uint8_t> Render(const SynthSpec& spec, WaveFormat& outFmt);

/// プリセットの基準 spec。seed が 0 以外なら Mutate を軽く掛けた派生を返す。
[[nodiscard]] SynthSpec MakePreset(SynthPreset preset, uint32_t seed = 0);

/// base の各パラメーターを seed 由来の乱数で ±amount の割合だけ揺らす。
/// 足音や被弾音の「毎回わずかに違う」を作る用途。amount は 0-1 を想定。
[[nodiscard]] SynthSpec Mutate(const SynthSpec& base, float amount, uint32_t seed);

/// 全パラメーターを seed からランダムに決める。Editor の Randomize 用。
[[nodiscard]] SynthSpec Randomize(uint32_t seed);

/// PCM を RIFF/WAVE のバイト列にする。Editor の Export と実素材への焼き出しで使う。
[[nodiscard]] std::vector<uint8_t> EncodeWav(
    const void* pcmData, size_t bytes, const WaveFormat& fmt);

/// 同じ spec が同じ値になるハッシュ。生成クリップの重複排除に使う。
[[nodiscard]] uint64_t HashSpec(const SynthSpec& spec);

/// 合成結果の客観的な特徴量。
///
/// WHY 必要か: AI は音を聴けない。パラメーターを動かした結果が「重くなったのか
///     軽くなったのか」を判断する手がかりが無いと、反復オーサリングが
///     当てずっぽうになる。数値で返せば「brightnessHz を下げる」という
///     目標に対して spec を詰められる (Docs/design/audio-system.md §9)。
struct SynthAnalysis {
    float    durationSeconds   = 0.0f;
    float    peakAmplitude     = 0.0f;  ///< 0-1
    float    rmsAmplitude      = 0.0f;  ///< 0-1。体感音量に近い
    /// パワー加重の実効周波数 (Hz)。いわゆる「明るさ」。
    /// 窓付き FFT のスペクトル重心ではなく、一次差分から求める RMS 周波数。
    /// 低いほど「重い / こもった」音になる。
    float    brightnessHz      = 0.0f;
    /// 1 秒あたりのゼロ交差数。ノイズ性の指標で、同じ brightness でも
    /// ノイズ波形の方が高く出る。
    float    zeroCrossingHz    = 0.0f;
    /// ピークに達するまでの秒数。小さいほど打撃感が強い。
    float    timeToPeakSeconds = 0.0f;
    /// ±1 に張り付いたサンプルの割合 0-1。amplitude 過大の検出に使う。
    float    clippedRatio      = 0.0f;
    uint32_t sampleRate        = 0;
    uint32_t sampleCount       = 0;
};

/// spec を合成して特徴量を測る。合成を伴うので毎フレーム呼ぶ用途には向かない。
[[nodiscard]] SynthAnalysis Analyze(const SynthSpec& spec);

/// 列挙と TOML / UI 用の文字列を相互変換する。未知の名前は fallback を返す。
[[nodiscard]] const char* WaveToString(SynthWave wave);
[[nodiscard]] SynthWave   WaveFromString(const char* name, SynthWave fallback);
[[nodiscard]] const char* PresetToString(SynthPreset preset);
[[nodiscard]] SynthPreset PresetFromString(const char* name, SynthPreset fallback);

} // namespace fbzz::audio
