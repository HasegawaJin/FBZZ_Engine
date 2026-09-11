/// @file    VolumeFlipbookAnalytic.cpp
/// @brief   解析ボリューム (アフィン移動する noise puff の和) の実装。
/// @author  Hasegawa Jin
/// @date    2026-09-11
#include <Engine/Asset/VolumeFlipbookAnalytic.hpp>

#include <Engine/Core/CurlNoise.hpp>

#include <algorithm>
#include <cmath>

namespace fbzz::asset {
namespace {

struct Matrix3x3 {
    float m[3][3]{};

    [[nodiscard]] math::Vector3 Apply(const math::Vector3& v) const
    {
        return { m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z,
                 m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z,
                 m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z };
    }
};

// ロドリゲスの回転公式。軸が固定なので R(a+Δ)·R(a)^T = R(Δ) が成り立ち、flow map が閉じる。
Matrix3x3 AxisAngleRotation(const math::Vector3& angularVelocity, float seconds)
{
    Matrix3x3 r;
    const float speed = angularVelocity.Length();
    if (speed < 1.0e-8f) {
        r.m[0][0] = r.m[1][1] = r.m[2][2] = 1.0f;
        return r;
    }
    const math::Vector3 k = angularVelocity / speed;
    const float angle = speed * seconds;
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    const float t = 1.0f - c;
    r.m[0][0] = c + t * k.x * k.x;       r.m[0][1] = t * k.x * k.y - s * k.z; r.m[0][2] = t * k.x * k.z + s * k.y;
    r.m[1][0] = t * k.y * k.x + s * k.z; r.m[1][1] = c + t * k.y * k.y;       r.m[1][2] = t * k.y * k.z - s * k.x;
    r.m[2][0] = t * k.z * k.x - s * k.y; r.m[2][1] = t * k.z * k.y + s * k.x; r.m[2][2] = c + t * k.z * k.z;
    return r;
}

math::Vector3 CenterAt(const VolumePuff& puff, float age)
{
    return puff.startCenter + puff.velocity * age;
}

float ScaleAt(const VolumePuff& puff, float age)
{
    return std::exp(puff.expansionRate * age);
}

float Envelope(const VolumePuff& puff, float age)
{
    if (age < 0.0f || age > puff.lifetime) return 0.0f;
    const float in = puff.fadeIn > 0.0f ? std::clamp(age / puff.fadeIn, 0.0f, 1.0f) : 1.0f;
    const float out = puff.fadeOut > 0.0f
        ? std::clamp((puff.lifetime - age) / puff.fadeOut, 0.0f, 1.0f) : 1.0f;
    return in * out;
}

// ParticleNoise.hlsli の FbmNoise3D と同じ (オクターブ間の周波数は 2.03 倍)。
float FbmNoise3D(const math::Vector3& p, int octaves)
{
    float sum = 0.0f;
    float amplitude = 0.5f;
    float frequency = 1.0f;
    for (int i = 0; i < octaves; ++i) {
        sum += core::ValueNoise3D(p * frequency) * amplitude;
        frequency *= 2.03f;
        amplitude *= 0.5f;
    }
    return sum;
}

float SmoothStep(float edge0, float edge1, float x)
{
    const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

float Saturate(float x) { return std::clamp(x, 0.0f, 1.0f); }

float Hash01(std::uint32_t seed, std::uint32_t index, std::uint32_t channel)
{
    const std::uint32_t h = core::PcgHash(seed * 0x9E3779B9u ^ index * 0x85EBCA6Bu ^ channel * 0xC2B2AE35u);
    return static_cast<float>(h) / 4294967296.0f;
}

float Signed(std::uint32_t seed, std::uint32_t index, std::uint32_t channel)
{
    return Hash01(seed, index, channel) * 2.0f - 1.0f;
}

math::Vector3 RandomAxis(std::uint32_t seed, std::uint32_t index)
{
    const math::Vector3 axis = { Signed(seed, index, 11), Signed(seed, index, 12), Signed(seed, index, 13) };
    return axis.NormalizedOr(math::Vector3::UP);
}

// 上端が bake 空間の箱 (y = 1) に届かない寿命と上昇速度。届くと箱の面で煙の頭が切れる。
// 見積もり: -0.78 + (0.42 + 0.1)·2.2 + 0.2·e^{0.45·2.2}·1.1 ≈ 0.9
constexpr float kPlumeLifetime = 2.2f;
constexpr float kPlumeSpawnPeriod = 0.12f;
constexpr float kPlumeBaseY = -0.78f;
constexpr float kPlumeRiseSpeed = 0.42f;

VolumePuff MakePlumePuff(std::uint32_t seed, std::uint32_t variant, float birthTime)
{
    VolumePuff puff;
    puff.birthTime = birthTime;
    puff.lifetime = kPlumeLifetime;
    puff.fadeIn = 0.25f;
    puff.fadeOut = 0.6f;
    puff.startCenter = { Signed(seed, variant, 1) * 0.06f, kPlumeBaseY, Signed(seed, variant, 2) * 0.06f };
    puff.velocity = { Signed(seed, variant, 3) * 0.08f, kPlumeRiseSpeed + Signed(seed, variant, 4) * 0.1f,
                      Signed(seed, variant, 5) * 0.08f };
    puff.angularVelocity = RandomAxis(seed, variant) * (1.2f + Hash01(seed, variant, 6) * 0.6f);
    puff.expansionRate = 0.45f;
    puff.radius = 0.16f + Hash01(seed, variant, 7) * 0.04f;
    puff.density = 1.2f;
    puff.temperature = 1.0f;
    // 0.5 秒だと見えている puff の大半が冷え切り、炎の芯が暗赤にしかならなかった (発光の最大 0.22)。
    puff.coolingTime = 0.8f;
    puff.noiseSeedOffset = Hash01(seed, variant, 8) * 97.0f;
    return puff;
}

} // namespace

float DefaultVolumeStartTime(VolumeFlipbookPreset preset)
{
    // Plume は湧き始めの 1 本目から焼くと «細い柱» から始まってしまう。寿命ぶん先へ進めて
    // 定常状態を 0 コマ目にする。
    return preset == VolumeFlipbookPreset::RisingPlume ? kPlumeLifetime : 0.0f;
}

float ResolveVolumeStartTime(const VolumeSourceSettings& settings)
{
    return settings.startTime >= 0.0f ? settings.startTime : DefaultVolumeStartTime(settings.preset);
}

std::vector<VolumePuff> BuildVolumePuffs(const VolumeSourceSettings& settings)
{
    std::vector<VolumePuff> puffs;
    const std::uint32_t seed = settings.seed;
    // 一発もの (Puff / Fireball) はベイク全体の長さで正規化する。移動量・膨張・回転・冷却を
    // «ベイクの始めから終わりまでに» どれだけ進むかで決めるので、コマ数や FPS を変えても
    // 構図が変わらず、箱からもはみ出さない (定数はシミュレーションで箱の内側に収まる値へ詰めた)。
    const float duration = static_cast<float>((std::max)(settings.frameCount, 1))
        * (std::max)(settings.frameDt, 1.0e-4f);
    const float start = ResolveVolumeStartTime(settings);
    switch (settings.preset) {
    case VolumeFlipbookPreset::Puff: {
        VolumePuff puff;
        puff.birthTime = start;
        puff.startCenter = { 0.0f, -0.4f, 0.0f };
        puff.velocity = math::Vector3{ 0.13f, 0.65f, 0.0f } / duration;
        puff.angularVelocity = math::Vector3{ 0.3f, 1.0f, 0.2f }.NormalizedOr(math::Vector3::UP) * (3.0f / duration);
        puff.expansionRate = std::log(1.8f) / duration;
        puff.radius = 0.35f;
        puff.density = 1.0f;
        puff.noiseSeedOffset = Hash01(seed, 0, 8) * 97.0f;
        puffs.push_back(puff);
        break;
    }
    case VolumeFlipbookPreset::Fireball: {
        VolumePuff center;
        center.birthTime = start;
        // 最終コマでちょうど消え切るよう、寿命をベイクの長さに合わせる。
        center.lifetime = duration * 1.001f;
        center.fadeOut = duration * 0.5f;
        center.startCenter = { 0.0f, -0.3f, 0.0f };
        center.velocity = { 0.0f, 0.25f / duration, 0.0f };
        center.angularVelocity = RandomAxis(seed, 0) * (4.0f / duration);
        center.expansionRate = std::log(2.2f) / duration;
        center.radius = 0.16f;
        center.density = 1.5f;
        center.temperature = 1.0f;
        center.coolingTime = duration * 0.2f;
        center.noiseSeedOffset = Hash01(seed, 0, 8) * 97.0f;
        puffs.push_back(center);
        const math::Vector3 directions[] = {
            { 1.0f, 0.2f, 0.0f }, { -1.0f, 0.2f, 0.0f }, { 0.0f, 0.2f, 1.0f },
            { 0.0f, 0.2f, -1.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, -0.6f, 0.0f } };
        std::uint32_t index = 1;
        for (const math::Vector3& raw : directions) {
            const math::Vector3 direction = raw.NormalizedOr(math::Vector3::UP);
            VolumePuff lobe = center;
            lobe.startCenter = center.startCenter + direction * 0.12f;
            lobe.velocity = (direction * 0.45f + math::Vector3{ 0.0f, 0.25f, 0.0f }) / duration;
            lobe.angularVelocity = RandomAxis(seed, index) * (6.0f / duration);
            lobe.expansionRate = std::log(2.6f) / duration;
            lobe.radius = 0.13f;
            lobe.noiseSeedOffset = Hash01(seed, index, 8) * 97.0f;
            puffs.push_back(lobe);
            ++index;
        }
        break;
    }
    case VolumeFlipbookPreset::RisingPlume:
    default: {
        // WHY 周期を割り切れる値へ寄せるか: duration が周期のちょうど M 倍なら、
        //     puff j と j+M が同じ見た目で duration だけずれて生まれる。場が厳密に元へ戻る。
        int variants = 0;
        float period = kPlumeSpawnPeriod;
        if (settings.loop) {
            variants = (std::max)(1, static_cast<int>(std::lround(duration / kPlumeSpawnPeriod)));
            period = duration / static_cast<float>(variants);
        }
        const int first = static_cast<int>(std::floor((start - kPlumeLifetime) / period)) - 1;
        const int last = static_cast<int>(std::ceil((start + duration + settings.frameDt) / period)) + 1;
        for (int j = first; j <= last; ++j) {
            const int variant = variants > 0 ? ((j % variants) + variants) % variants : j;
            puffs.push_back(MakePlumePuff(seed, static_cast<std::uint32_t>(variant),
                                          static_cast<float>(j) * period));
        }
        break;
    }
    }
    return puffs;
}

math::Vector3 PuffFlowMap(const VolumePuff& puff, const math::Vector3& x, float t0, float t1)
{
    const float a0 = t0 - puff.birthTime;
    const float a1 = t1 - puff.birthTime;
    const Matrix3x3 rotation = AxisAngleRotation(puff.angularVelocity, t1 - t0);
    const float scale = ScaleAt(puff, a1) / ScaleAt(puff, a0);
    const math::Vector3 rotated = rotation.Apply(x - CenterAt(puff, a0));
    return CenterAt(puff, a1) + rotated * scale;
}

float EvaluatePuffBodyDensity(const math::Vector3& bodyPosition, float noiseSeedOffset,
                              const VolumeNoiseSettings& noise)
{
    const float amplitude = std::clamp(noise.amplitude, 0.0f, 1.0f);
    const math::Vector3 seedShift = { noiseSeedOffset, noiseSeedOffset * 1.31f, noiseSeedOffset * 0.73f };
    const float n = FbmNoise3D(bodyPosition * noise.frequency + seedShift, 4);
    // FBM の振幅合計は 0.9375。縁は最大 0.375 外へずれるので、|y| < 1.375 に収まる。
    const float r = bodyPosition.Length() - amplitude * 0.4f * n;
    return Saturate(1.0f - SmoothStep(0.3f, 1.0f, r)) * Saturate(0.65f + 0.5f * n);
}

std::uint32_t PackVolumeFillConstants(std::span<const VolumePuff> puffs,
                                      const VolumeNoiseSettings& noise, std::uint32_t resolution,
                                      float time, float frameDt, VolumeFillConstants& out)
{
    out = {};
    out.resolution = resolution;
    out.noiseFrequency = noise.frequency;
    out.noiseAmplitude = std::clamp(noise.amplitude, 0.0f, 1.0f);
    const float dt = (std::max)(frameDt, 1.0e-4f);

    for (const VolumePuff& puff : puffs) {
        const float age = time - puff.birthTime;
        const float envelope = Envelope(puff, age);
        if (envelope <= 0.0f) continue;
        if (out.puffCount >= kVolumeFillMaxPuffs) break;

        // y = R(a)^T (x - c) / (s r0)
        const Matrix3x3 rotation = AxisAngleRotation(puff.angularVelocity, age);
        const float inverseSize = 1.0f / (ScaleAt(puff, age) * (std::max)(puff.radius, 1.0e-4f));
        const math::Vector3 center = CenterAt(puff, age);
        VolumeFillPuffGpu& gpu = out.puffs[out.puffCount];
        for (int row = 0; row < 3; ++row) {
            // R^T の行 row = R の列 row
            const math::Vector3 b = { rotation.m[0][row] * inverseSize, rotation.m[1][row] * inverseSize,
                                      rotation.m[2][row] * inverseSize };
            gpu.body[row][0] = b.x;
            gpu.body[row][1] = b.y;
            gpu.body[row][2] = b.z;
            gpu.body[row][3] = -math::Vector3::Dot(b, center);
        }

        // 割線速度: x' = c1 + A (x - c0), A = (s1/s0) R(Δ)  →  v = ((A - I) x + c1 - A c0) / Δ
        const Matrix3x3 step = AxisAngleRotation(puff.angularVelocity, dt);
        const float growth = ScaleAt(puff, age + dt) / ScaleAt(puff, age);
        const math::Vector3 nextCenter = CenterAt(puff, age + dt);
        Matrix3x3 a;
        for (int row = 0; row < 3; ++row)
            for (int column = 0; column < 3; ++column) a.m[row][column] = step.m[row][column] * growth;
        const math::Vector3 aCenter = a.Apply(center);
        for (int row = 0; row < 3; ++row) {
            for (int column = 0; column < 3; ++column)
                gpu.velocity[row][column] = (a.m[row][column] - (row == column ? 1.0f : 0.0f)) / dt;
        }
        gpu.velocity[0][3] = (nextCenter.x - aCenter.x) / dt;
        gpu.velocity[1][3] = (nextCenter.y - aCenter.y) / dt;
        gpu.velocity[2][3] = (nextCenter.z - aCenter.z) / dt;

        gpu.params[0] = puff.density * envelope;
        gpu.params[1] = puff.temperature * std::exp(-age / (std::max)(puff.coolingTime, 1.0e-4f));
        gpu.params[2] = puff.noiseSeedOffset;
        gpu.params[3] = kVolumePuffBodyCullRadius;
        ++out.puffCount;
    }
    return out.puffCount;
}

std::vector<VolumeBound> CollectVisibleVolumeBounds(std::span<const VolumePuff> puffs,
                                                    const VolumeNoiseSettings& noise, float time)
{
    // 縁の揺らぎの半分までを «見える» とする (smoothstep(0.3, 1, r) の裾はほぼ透明)。
    const float visibleBodyRadius = 1.0f + 0.2f * std::clamp(noise.amplitude, 0.0f, 1.0f);
    std::vector<VolumeBound> bounds;
    for (const VolumePuff& puff : puffs) {
        const float age = time - puff.birthTime;
        // フェードの裾で薄くなった puff は数えない。
        if (Envelope(puff, age) * puff.density < 0.1f) continue;
        bounds.push_back({ CenterAt(puff, age), visibleBodyRadius * ScaleAt(puff, age) * puff.radius });
    }
    return bounds;
}

VolumeSample SampleVolumeFill(const VolumeFillConstants& constants, const math::Vector3& x)
{
    const VolumeNoiseSettings noise{ constants.noiseFrequency, constants.noiseAmplitude };
    float densitySum = 0.0f;
    float temperatureSum = 0.0f;
    math::Vector3 velocitySum;
    const std::uint32_t count = (std::min)(constants.puffCount, kVolumeFillMaxPuffs);
    for (std::uint32_t i = 0; i < count; ++i) {
        const VolumeFillPuffGpu& puff = constants.puffs[i];
        const math::Vector3 y = {
            puff.body[0][0] * x.x + puff.body[0][1] * x.y + puff.body[0][2] * x.z + puff.body[0][3],
            puff.body[1][0] * x.x + puff.body[1][1] * x.y + puff.body[1][2] * x.z + puff.body[1][3],
            puff.body[2][0] * x.x + puff.body[2][1] * x.y + puff.body[2][2] * x.z + puff.body[2][3] };
        if (y.LengthSq() > puff.params[3] * puff.params[3]) continue;
        const float density = puff.params[0] * EvaluatePuffBodyDensity(y, puff.params[2], noise);
        if (density <= 0.0f) continue;
        const math::Vector3 v = {
            puff.velocity[0][0] * x.x + puff.velocity[0][1] * x.y + puff.velocity[0][2] * x.z + puff.velocity[0][3],
            puff.velocity[1][0] * x.x + puff.velocity[1][1] * x.y + puff.velocity[1][2] * x.z + puff.velocity[1][3],
            puff.velocity[2][0] * x.x + puff.velocity[2][1] * x.y + puff.velocity[2][2] * x.z + puff.velocity[2][3] };
        densitySum += density;
        temperatureSum += density * puff.params[1] * Saturate(1.0f - y.Length());
        velocitySum += v * density;
    }

    VolumeSample sample;
    sample.density = densitySum;
    if (densitySum > 1.0e-6f) {
        sample.temperature = temperatureSum / densitySum;
        sample.velocity = velocitySum / densitySum;
    }
    return sample;
}

} // namespace fbzz::asset
