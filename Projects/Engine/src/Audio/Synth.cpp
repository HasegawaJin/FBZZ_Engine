/// @file    Synth.cpp
/// @brief   SynthSpec から PCM を合成する実装。
/// @author  Hasegawa Jin
/// @date    2026-08-23
#include <Engine/Audio/Synth.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <iterator>

namespace fbzz::audio {
namespace {

constexpr uint32_t kDefaultSampleRate = 44100;
constexpr float    kTwoPi             = 6.28318530717958647692f;
constexpr float    kMaxDuration       = 30.0f;
constexpr int      kNoiseTableSize    = 32;

/// @brief 決定的乱数生成器。同じ seed から同じ音が出ることが仕様の一部。
/// @note std::mt19937 は標準ライブラリの実装・バージョン差を挟むため使わない。xorshift32 なら
///       ここに全部書いてあり、どの環境でも同じ列になる。
struct Rng {
    uint32_t state;

    explicit Rng(uint32_t seed) : state(seed ? seed : 0x9E3779B9u) {}

    uint32_t Next()
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    }

    float Unit()   { return static_cast<float>(Next() >> 8) / 16777216.0f; }
    float Signed() { return Unit() * 2.0f - 1.0f; }
};

float Clamp(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

SynthSpec Sanitize(const SynthSpec& in)
{
    SynthSpec s = in;
    s.attack  = Clamp(s.attack,  0.0f, kMaxDuration);
    s.sustain = Clamp(s.sustain, 0.0f, kMaxDuration);
    s.decay   = Clamp(s.decay,   0.0f, kMaxDuration);
    if (s.attack + s.sustain + s.decay > kMaxDuration) {
        const float scale = kMaxDuration / (s.attack + s.sustain + s.decay);
        s.attack  *= scale;
        s.sustain *= scale;
        s.decay   *= scale;
    }
    s.punch           = Clamp(s.punch, 0.0f, 4.0f);
    s.startFrequency  = Clamp(s.startFrequency, 1.0f, 20000.0f);
    s.minFrequency    = Clamp(s.minFrequency, 0.0f, 20000.0f);
    s.slide           = Clamp(s.slide, -32.0f, 32.0f);
    s.vibratoDepth    = Clamp(s.vibratoDepth, 0.0f, 1.0f);
    s.vibratoRate     = Clamp(s.vibratoRate, 0.0f, 200.0f);
    s.dutyCycle       = Clamp(s.dutyCycle, 0.01f, 0.99f);
    s.dutySweep       = Clamp(s.dutySweep, -8.0f, 8.0f);
    s.lowPassCutoff   = Clamp(s.lowPassCutoff, 0.0f, 1.0f);
    s.lowPassSweep    = Clamp(s.lowPassSweep, -8.0f, 8.0f);
    s.highPassCutoff  = Clamp(s.highPassCutoff, 0.0f, 1.0f);
    s.bitCrush        = Clamp(s.bitCrush, 0.0f, 1.0f);
    s.repeatRate      = Clamp(s.repeatRate, 0.0f, 200.0f);
    s.arpeggioMod     = Clamp(s.arpeggioMod, 0.0f, 8.0f);
    s.amplitude       = Clamp(s.amplitude, 0.0f, 1.0f);
    if (s.sampleRate < 8000 || s.sampleRate > 192000) s.sampleRate = kDefaultSampleRate;
    return s;
}

float EnvelopeAt(const SynthSpec& spec, float t)
{
    if (t < spec.attack)
        return spec.attack > 0.0f ? t / spec.attack : 1.0f;

    const float sustainEnd = spec.attack + spec.sustain;
    if (t < sustainEnd) {
        const float k = spec.sustain > 0.0f ? (t - spec.attack) / spec.sustain : 1.0f;
        return 1.0f + spec.punch * (1.0f - k);
    }

    const float k = spec.decay > 0.0f ? (t - sustainEnd) / spec.decay : 1.0f;
    return Clamp(1.0f - k, 0.0f, 1.0f);
}

float Oscillate(SynthWave wave, float phase, float duty, const float* noise)
{
    switch (wave) {
    case SynthWave::Sine:     return std::sin(kTwoPi * phase);
    case SynthWave::Square:   return phase < duty ? 1.0f : -1.0f;
    case SynthWave::Saw:      return 1.0f - 2.0f * phase;
    case SynthWave::Triangle: return 4.0f * std::fabs(phase - 0.5f) - 1.0f;
    case SynthWave::Noise:
    default: {
        const int index = static_cast<int>(phase * kNoiseTableSize);
        return noise[index & (kNoiseTableSize - 1)];
    }
    }
}

/// @brief 正規化カットオフ (0-1) を一次フィルターの係数へ落とす。
/// @note 聴感上の変化は低域に密集しているため二乗する。線形のままだと 0.0-0.2 の
///       狭い範囲でしか音が変わらず、Editor のスライダーがほぼ使えなくなる。
float FilterCoefficient(float normalizedCutoff, uint32_t sampleRate)
{
    const float nyquist = static_cast<float>(sampleRate) * 0.5f;
    const float cutoffHz = normalizedCutoff * normalizedCutoff * nyquist;
    return 1.0f - std::exp(-kTwoPi * cutoffHz / static_cast<float>(sampleRate));
}

uint64_t HashBytes(uint64_t hash, const void* data, size_t bytes)
{
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < bytes; ++i) {
        hash ^= p[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

uint64_t HashFloat(uint64_t hash, float value)
{
    /// @note -0.0f と +0.0f はビット列が違うが合成結果は同じため、生ビットでなく正規化してから
    ///       畳み込み、別クリップとして二重生成されるのを防ぐ。
    if (value == 0.0f) value = 0.0f;
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return HashBytes(hash, &bits, sizeof(bits));
}

constexpr const char* kWaveNames[] = { "sine", "square", "saw", "triangle", "noise" };
constexpr const char* kPresetNames[] = {
    "pickup", "laser", "explosion", "powerup", "hit", "jump", "blip"
};

} // namespace

std::vector<uint8_t> Render(const SynthSpec& in, WaveFormat& outFmt)
{
    const SynthSpec spec = Sanitize(in);
    const float duration = spec.attack + spec.sustain + spec.decay;
    if (duration <= 0.0f || spec.amplitude <= 0.0f) return {};

    const uint32_t sampleRate  = spec.sampleRate;
    const float    invRate     = 1.0f / static_cast<float>(sampleRate);
    const size_t   sampleCount = static_cast<size_t>(duration * static_cast<float>(sampleRate));
    if (sampleCount == 0) return {};

    outFmt.sampleRate    = sampleRate;
    outFmt.channels      = 1;
    outFmt.bitsPerSample = 16;

    Rng   rng(spec.seed);
    float noise[kNoiseTableSize];
    for (float& n : noise) n = rng.Signed();

    const float nyquist      = static_cast<float>(sampleRate) * 0.5f - 1.0f;
    const float minFrequency = spec.minFrequency > 0.0f ? spec.minFrequency : 1.0f;
    const float repeatPeriod = spec.repeatRate > 0.0f ? 1.0f / spec.repeatRate : 0.0f;
    const float arpeggio     = spec.arpeggioMod > 0.0f ? spec.arpeggioMod : 1.0f;
    const float crushLevels  = spec.bitCrush > 0.0f
        ? std::exp2(16.0f - spec.bitCrush * 12.0f) : 0.0f;

    /// @note 係数計算は exp() を含み、掃引していない間は毎サンプル同じ値になるため、
    ///       掃引の有無で分けループの外で 1 回だけ求める。
    const bool  sweepsLowPass = spec.lowPassSweep != 0.0f;
    const float staticLowPass = FilterCoefficient(spec.lowPassCutoff, sampleRate);
    const float highPassCoeff = spec.highPassCutoff > 0.0f
        ? FilterCoefficient(spec.highPassCutoff, sampleRate) : 0.0f;

    const size_t guardSamples = (std::min)(static_cast<size_t>(sampleRate / 1000),
                                           sampleCount / 4);

    float  phase        = 0.0f;
    float  baseFrequency = spec.startFrequency;
    float  segmentStart = 0.0f;
    float  lowPassState = 0.0f;
    float  highPassState = 0.0f;

    std::vector<uint8_t> pcm(sampleCount * 2);

    for (size_t i = 0; i < sampleCount; ++i) {
        const float t = static_cast<float>(i) * invRate;

        if (repeatPeriod > 0.0f && t - segmentStart >= repeatPeriod) {
            segmentStart += repeatPeriod;
            baseFrequency = Clamp(baseFrequency * arpeggio, 1.0f, 20000.0f);
            phase = 0.0f;
        }
        const float tSegment = t - segmentStart;

        float frequency = baseFrequency * std::exp2(spec.slide * tSegment);
        if (spec.vibratoDepth > 0.0f && spec.vibratoRate > 0.0f)
            frequency *= 1.0f + spec.vibratoDepth * std::sin(kTwoPi * spec.vibratoRate * t);
        frequency = Clamp(frequency, minFrequency, nyquist);

        phase += frequency * invRate;
        if (phase >= 1.0f) {
            phase -= std::floor(phase);
            if (spec.wave == SynthWave::Noise)
                for (float& n : noise) n = rng.Signed();
        }

        const float duty = Clamp(spec.dutyCycle + spec.dutySweep * tSegment, 0.01f, 0.99f);
        float value = Oscillate(spec.wave, phase, duty, noise);

        /// @note 係数は cutoff=1 でも 1 に達しないため、係数側で判定すると既定の「無加工」
        ///       設定でも高域が削れる。正規化値側で判定する。
        const float lowPassNorm = sweepsLowPass
            ? Clamp(spec.lowPassCutoff + spec.lowPassSweep * t, 0.0f, 1.0f)
            : spec.lowPassCutoff;
        if (lowPassNorm < 1.0f) {
            const float coeff = sweepsLowPass
                ? FilterCoefficient(lowPassNorm, sampleRate) : staticLowPass;
            lowPassState += coeff * (value - lowPassState);
            value = lowPassState;
        }
        if (highPassCoeff > 0.0f) {
            highPassState += highPassCoeff * (value - highPassState);
            value -= highPassState;
        }

        if (crushLevels > 0.0f)
            value = std::round(value * crushLevels) / crushLevels;

        value *= EnvelopeAt(spec, t) * spec.amplitude;

        /// @note attack / decay が 0 のときの発音端クリックを潰す。
        if (guardSamples > 0) {
            if (i < guardSamples)
                value *= static_cast<float>(i) / static_cast<float>(guardSamples);
            else if (i + guardSamples >= sampleCount)
                value *= static_cast<float>(sampleCount - i) / static_cast<float>(guardSamples);
        }

        /// @note WAV の 16bit PCM はリトルエンディアン。バイトで書くことで、
        ///       アラインメントもエンディアンも環境任せにしない。
        const auto sample = static_cast<int16_t>(Clamp(value, -1.0f, 1.0f) * 32767.0f);
        pcm[i * 2]     = static_cast<uint8_t>(static_cast<uint16_t>(sample) & 0xFFu);
        pcm[i * 2 + 1] = static_cast<uint8_t>((static_cast<uint16_t>(sample) >> 8) & 0xFFu);
    }

    return pcm;
}

SynthSpec MakePreset(SynthPreset preset, uint32_t seed)
{
    SynthSpec s;
    switch (preset) {
    case SynthPreset::Pickup:
        s.wave = SynthWave::Square;
        s.sustain = 0.05f; s.decay = 0.12f; s.punch = 0.25f;
        s.startFrequency = 900.0f;
        s.repeatRate = 12.0f; s.arpeggioMod = 1.5f;
        s.amplitude = 0.45f;
        break;
    case SynthPreset::Laser:
        s.wave = SynthWave::Saw;
        s.sustain = 0.04f; s.decay = 0.16f;
        s.startFrequency = 1100.0f; s.minFrequency = 120.0f; s.slide = -6.0f;
        s.lowPassCutoff = 0.9f;
        s.amplitude = 0.42f;
        break;
    case SynthPreset::Explosion:
        s.wave = SynthWave::Noise;
        s.sustain = 0.10f; s.decay = 0.40f; s.punch = 0.5f;
        s.startFrequency = 260.0f; s.slide = -1.6f;
        s.lowPassCutoff = 0.55f; s.lowPassSweep = -0.4f;
        s.amplitude = 0.55f;
        break;
    case SynthPreset::PowerUp:
        s.wave = SynthWave::Triangle;
        s.attack = 0.02f; s.sustain = 0.12f; s.decay = 0.25f;
        s.startFrequency = 420.0f; s.slide = 2.2f;
        s.vibratoDepth = 0.12f; s.vibratoRate = 14.0f;
        s.amplitude = 0.40f;
        break;
    case SynthPreset::Hit:
        s.wave = SynthWave::Noise;
        s.sustain = 0.03f; s.decay = 0.14f;
        s.startFrequency = 480.0f; s.slide = -2.4f;
        s.lowPassCutoff = 0.7f; s.highPassCutoff = 0.08f;
        s.amplitude = 0.50f;
        break;
    case SynthPreset::Jump:
        s.wave = SynthWave::Square;
        s.attack = 0.005f; s.sustain = 0.06f; s.decay = 0.10f;
        s.startFrequency = 340.0f; s.slide = 2.0f; s.dutyCycle = 0.4f;
        s.amplitude = 0.40f;
        break;
    case SynthPreset::Blip:
    default:
        s.wave = SynthWave::Square;
        s.sustain = 0.02f; s.decay = 0.05f;
        s.startFrequency = 900.0f;
        s.amplitude = 0.35f;
        break;
    }
    s.seed = seed;
    return seed != 0 ? Mutate(s, 0.10f, seed) : s;
}

SynthSpec Mutate(const SynthSpec& base, float amount, uint32_t seed)
{
    const float scale = Clamp(amount, 0.0f, 1.0f);
    if (scale <= 0.0f) return base;

    Rng rng(seed);
    const auto jitter = [&](float value, float lo, float hi) {
        return Clamp(value * (1.0f + rng.Signed() * scale), lo, hi);
    };
    /// @note 0 のままにしておきたいパラメーター (無効化されている機能) は乗算では動かない。
    ///       掛け算だけだと Mutate が「一度でも 0 にした項目を永久に殺す」挙動になるため、
    ///       中心が 0 の項目は加算で揺らす。
    const auto offset = [&](float value, float span, float lo, float hi) {
        return Clamp(value + rng.Signed() * scale * span, lo, hi);
    };

    SynthSpec s = base;
    s.attack          = jitter(s.attack, 0.0f, 5.0f);
    s.sustain         = jitter(s.sustain, 0.001f, 5.0f);
    s.decay           = jitter(s.decay, 0.001f, 5.0f);
    s.punch           = offset(s.punch, 0.5f, 0.0f, 2.0f);
    s.startFrequency  = jitter(s.startFrequency, 20.0f, 18000.0f);
    s.slide           = offset(s.slide, 2.0f, -32.0f, 32.0f);
    s.vibratoDepth    = offset(s.vibratoDepth, 0.2f, 0.0f, 1.0f);
    s.dutyCycle       = offset(s.dutyCycle, 0.3f, 0.01f, 0.99f);
    s.dutySweep       = offset(s.dutySweep, 1.0f, -8.0f, 8.0f);
    s.lowPassCutoff   = offset(s.lowPassCutoff, 0.2f, 0.05f, 1.0f);
    s.highPassCutoff  = offset(s.highPassCutoff, 0.1f, 0.0f, 1.0f);
    s.seed            = rng.Next();
    return s;
}

SynthSpec Randomize(uint32_t seed)
{
    Rng rng(seed);
    SynthSpec s;
    s.wave            = static_cast<SynthWave>(rng.Next() % 5);
    s.attack          = rng.Unit() * rng.Unit() * 0.2f;
    s.sustain         = 0.01f + rng.Unit() * 0.2f;
    s.decay           = 0.02f + rng.Unit() * 0.4f;
    s.punch           = rng.Unit() * rng.Unit();
    s.startFrequency  = 120.0f + rng.Unit() * rng.Unit() * 1800.0f;
    s.minFrequency    = rng.Unit() < 0.3f ? 60.0f + rng.Unit() * 200.0f : 0.0f;
    s.slide           = rng.Signed() * rng.Unit() * 8.0f;
    s.vibratoDepth    = rng.Unit() < 0.3f ? rng.Unit() * 0.3f : 0.0f;
    s.vibratoRate     = s.vibratoDepth > 0.0f ? 4.0f + rng.Unit() * 30.0f : 0.0f;
    s.dutyCycle       = 0.1f + rng.Unit() * 0.8f;
    s.dutySweep       = rng.Unit() < 0.3f ? rng.Signed() * 2.0f : 0.0f;
    s.lowPassCutoff   = rng.Unit() < 0.5f ? 0.2f + rng.Unit() * 0.8f : 1.0f;
    s.lowPassSweep    = rng.Unit() < 0.3f ? rng.Signed() * 1.0f : 0.0f;
    s.highPassCutoff  = rng.Unit() < 0.3f ? rng.Unit() * 0.2f : 0.0f;
    s.bitCrush        = rng.Unit() < 0.2f ? rng.Unit() * 0.6f : 0.0f;
    s.repeatRate      = rng.Unit() < 0.25f ? 4.0f + rng.Unit() * 20.0f : 0.0f;
    s.arpeggioMod     = s.repeatRate > 0.0f ? 0.5f + rng.Unit() * 1.5f : 0.0f;
    s.amplitude       = 0.35f + rng.Unit() * 0.25f;
    s.seed            = rng.Next();
    return s;
}

std::vector<uint8_t> EncodeWav(const void* pcmData, size_t bytes, const WaveFormat& fmt)
{
    if (!pcmData || bytes == 0 || fmt.channels == 0 || fmt.sampleRate == 0) return {};

    const uint16_t blockAlign  = static_cast<uint16_t>(fmt.channels * fmt.bitsPerSample / 8);
    const uint32_t byteRate    = fmt.sampleRate * blockAlign;
    const uint32_t dataSize    = static_cast<uint32_t>(bytes);
    const uint32_t riffSize    = 36 + dataSize;

    std::vector<uint8_t> wav;
    wav.reserve(44 + bytes);

    const auto push = [&wav](const void* data, size_t size) {
        const auto* p = static_cast<const uint8_t*>(data);
        wav.insert(wav.end(), p, p + size);
    };
    const auto pushU32 = [&push](uint32_t v) { push(&v, sizeof(v)); };
    const auto pushU16 = [&push](uint16_t v) { push(&v, sizeof(v)); };

    push("RIFF", 4);
    pushU32(riffSize);
    push("WAVE", 4);
    push("fmt ", 4);
    pushU32(16);
    /// @note WAVE_FORMAT_PCM
    pushU16(1);
    pushU16(fmt.channels);
    pushU32(fmt.sampleRate);
    pushU32(byteRate);
    pushU16(blockAlign);
    pushU16(fmt.bitsPerSample);
    push("data", 4);
    pushU32(dataSize);
    push(pcmData, bytes);

    return wav;
}

uint64_t HashSpec(const SynthSpec& spec)
{
    uint64_t h = 14695981039346656037ull;
    const auto wave = static_cast<uint8_t>(spec.wave);
    h = HashBytes(h, &wave, sizeof(wave));
    for (const float v : { spec.attack, spec.sustain, spec.decay, spec.punch,
                           spec.startFrequency, spec.minFrequency, spec.slide,
                           spec.vibratoDepth, spec.vibratoRate,
                           spec.dutyCycle, spec.dutySweep,
                           spec.lowPassCutoff, spec.lowPassSweep,
                           spec.highPassCutoff, spec.bitCrush,
                           spec.repeatRate, spec.arpeggioMod, spec.amplitude })
        h = HashFloat(h, v);
    h = HashBytes(h, &spec.seed, sizeof(spec.seed));
    h = HashBytes(h, &spec.sampleRate, sizeof(spec.sampleRate));
    return h;
}

SynthAnalysis Analyze(const SynthSpec& spec)
{
    SynthAnalysis analysis{};

    WaveFormat fmt{};
    const std::vector<uint8_t> pcm = Render(spec, fmt);
    const size_t count = pcm.size() / 2;
    if (count == 0 || fmt.sampleRate == 0) return analysis;

    analysis.sampleRate      = fmt.sampleRate;
    analysis.sampleCount     = static_cast<uint32_t>(count);
    analysis.durationSeconds = static_cast<float>(count) / static_cast<float>(fmt.sampleRate);

    const auto sampleAt = [&pcm](size_t index) {
        const auto lo = static_cast<uint16_t>(pcm[index * 2]);
        const auto hi = static_cast<uint16_t>(pcm[index * 2 + 1]);
        const auto raw = static_cast<int16_t>(static_cast<uint16_t>(lo | (hi << 8)));
        return static_cast<float>(raw) / 32767.0f;
    };

    double energy     = 0.0;
    double diffEnergy = 0.0;
    size_t crossings  = 0;
    size_t clipped    = 0;
    size_t peakIndex  = 0;
    float  previous   = 0.0f;

    for (size_t i = 0; i < count; ++i) {
        const float value = sampleAt(i);
        const float magnitude = std::fabs(value);
        if (magnitude > analysis.peakAmplitude) {
            analysis.peakAmplitude = magnitude;
            peakIndex = i;
        }
        if (magnitude >= 0.999f) ++clipped;
        energy += static_cast<double>(value) * value;
        if (i > 0) {
            const float delta = value - previous;
            diffEnergy += static_cast<double>(delta) * delta;
            if ((value >= 0.0f) != (previous >= 0.0f)) ++crossings;
        }
        previous = value;
    }

    analysis.rmsAmplitude = static_cast<float>(std::sqrt(energy / static_cast<double>(count)));
    analysis.clippedRatio = static_cast<float>(clipped) / static_cast<float>(count);
    analysis.timeToPeakSeconds =
        static_cast<float>(peakIndex) / static_cast<float>(fmt.sampleRate);
    analysis.zeroCrossingHz = static_cast<float>(crossings) / analysis.durationSeconds;

    /// @note パワー加重の実効周波数: f = fs/(2π) * sqrt(Σ(Δx)² / Σx²)。
    ///       必要なのは「明るいか暗いか」の 1 スカラーだけで、一次差分のエネルギー比がそのまま
    ///       二乗平均周波数になるため FFT は使わず、窓関数もビン幅選択も不要で O(N) で済む。
    if (energy > 0.0) {
        analysis.brightnessHz = static_cast<float>(
            static_cast<double>(fmt.sampleRate) / static_cast<double>(kTwoPi)
            * std::sqrt(diffEnergy / energy));
    }
    return analysis;
}

const char* WaveToString(SynthWave wave)
{
    const auto index = static_cast<size_t>(wave);
    return index < std::size(kWaveNames) ? kWaveNames[index] : kWaveNames[1];
}

SynthWave WaveFromString(const char* name, SynthWave fallback)
{
    if (!name) return fallback;
    for (size_t i = 0; i < std::size(kWaveNames); ++i)
        if (std::strcmp(name, kWaveNames[i]) == 0) return static_cast<SynthWave>(i);
    return fallback;
}

const char* PresetToString(SynthPreset preset)
{
    const auto index = static_cast<size_t>(preset);
    return index < std::size(kPresetNames) ? kPresetNames[index] : kPresetNames[0];
}

SynthPreset PresetFromString(const char* name, SynthPreset fallback)
{
    if (!name) return fallback;
    for (size_t i = 0; i < std::size(kPresetNames); ++i)
        if (std::strcmp(name, kPresetNames[i]) == 0) return static_cast<SynthPreset>(i);
    return fallback;
}

} // namespace fbzz::audio
