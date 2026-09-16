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

// これより弱い減速は «減速なし» の式で扱う。(1 - e^{-ka}) / k の桁落ちを避けるため。
constexpr float kMinDrag = 1.0e-4f;

struct Matrix3x3 {
    float m[3][3]{};

    [[nodiscard]] math::Vector3 Apply(const math::Vector3& v) const
    {
        return { m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z,
                 m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z,
                 m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z };
    }

    [[nodiscard]] Matrix3x3 operator*(const Matrix3x3& rhs) const
    {
        Matrix3x3 out;
        for (int row = 0; row < 3; ++row)
            for (int column = 0; column < 3; ++column)
                out.m[row][column] = m[row][0] * rhs.m[0][column] + m[row][1] * rhs.m[1][column]
                    + m[row][2] * rhs.m[2][column];
        return out;
    }

    [[nodiscard]] Matrix3x3 Scaled(float s) const
    {
        Matrix3x3 out = *this;
        for (auto& row : out.m)
            for (float& value : row) value *= s;
        return out;
    }

    [[nodiscard]] Matrix3x3 Transposed() const
    {
        Matrix3x3 out;
        for (int row = 0; row < 3; ++row)
            for (int column = 0; column < 3; ++column) out.m[row][column] = m[column][row];
        return out;
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

struct StretchFrame {
    math::Vector3 direction = math::Vector3::UP;
    float sigma = 1.0f;
};

StretchFrame StretchAt(const VolumePuff& puff, float age)
{
    const math::Vector3 velocity = PuffCenterVelocityAt(puff, age);
    const float speed = velocity.Length();
    StretchFrame frame;
    frame.sigma = (std::max)(1.0f, puff.stretch + puff.stretchPerSpeed * speed);
    // 向きの符号は d·dᵀ に効かない。速さが 0 を跨ぐ瞬間に向きが飛んでも、stretch = 1 なら σ = 1 で形は連続する。
    if (speed > 1.0e-6f) frame.direction = velocity / speed;
    return frame;
}

// d 方向へ σ 倍、横へ 1/√σ 倍 (体積を保つ)。inverse なら逆行列。
Matrix3x3 StretchMatrix(const StretchFrame& frame, bool inverse)
{
    const float rootSigma = std::sqrt(frame.sigma);
    const float along = inverse ? 1.0f / frame.sigma : frame.sigma;
    const float across = inverse ? rootSigma : 1.0f / rootSigma;
    const float d[3] = { frame.direction.x, frame.direction.y, frame.direction.z };
    Matrix3x3 e;
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 3; ++column)
            e.m[row][column] = (row == column ? across : 0.0f) + (along - across) * d[row] * d[column];
    return e;
}

float ScaleAt(const VolumePuff& puff, float age)
{
    return std::exp(puff.expansionRate * age);
}

// 物体座標 y = A(age)^-1 (x - c) / radius の行列部分。A = s·E·R。
Matrix3x3 BodyMatrix(const VolumePuff& puff, float age)
{
    const float inverseSize = 1.0f / (ScaleAt(puff, age) * (std::max)(puff.radius, 1.0e-4f));
    return (AxisAngleRotation(puff.angularVelocity, age).Transposed() * StretchMatrix(StretchAt(puff, age), true))
        .Scaled(inverseSize);
}

// A(a1)·A(a0)^-1。R(a1)·R(a0)^T を R(a1 - a0) で求め、回転角が大きくても誤差を溜めない。
Matrix3x3 FlowMatrix(const VolumePuff& puff, float a0, float a1)
{
    const float growth = ScaleAt(puff, a1) / ScaleAt(puff, a0);
    return (StretchMatrix(StretchAt(puff, a1), false) * AxisAngleRotation(puff.angularVelocity, a1 - a0)
            * StretchMatrix(StretchAt(puff, a0), true))
        .Scaled(growth);
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

float Affine(const float (&row)[4], const math::Vector3& x)
{
    return row[0] * x.x + row[1] * x.y + row[2] * x.z + row[3];
}

} // namespace

math::Vector3 PuffCenterAt(const VolumePuff& puff, float age)
{
    if (puff.drag < kMinDrag)
        return puff.startCenter + puff.velocity * age + puff.acceleration * (0.5f * age * age);
    // c'' = g - k·c' の解。
    const float k = puff.drag;
    const float decay = (1.0f - std::exp(-k * age)) / k;
    return puff.startCenter + puff.velocity * decay + puff.acceleration * ((age - decay) / k);
}

math::Vector3 PuffCenterVelocityAt(const VolumePuff& puff, float age)
{
    if (puff.drag < kMinDrag) return puff.velocity + puff.acceleration * age;
    const float k = puff.drag;
    const float decay = std::exp(-k * age);
    return puff.velocity * decay + puff.acceleration * ((1.0f - decay) / k);
}

math::Vector3 PuffFlowMap(const VolumePuff& puff, const math::Vector3& x, float t0, float t1)
{
    const float a0 = t0 - puff.birthTime;
    const float a1 = t1 - puff.birthTime;
    return PuffCenterAt(puff, a1) + FlowMatrix(puff, a0, a1).Apply(x - PuffCenterAt(puff, a0));
}

float EvaluatePuffBodyDensity(const math::Vector3& bodyPosition, float noiseSeedOffset,
                              const VolumeNoiseSettings& noise, float noiseScale)
{
    const float amplitude = std::clamp(noise.amplitude * noiseScale, 0.0f, 1.0f);
    const math::Vector3 seedShift = { noiseSeedOffset, noiseSeedOffset * 1.31f, noiseSeedOffset * 0.73f };
    const float n = FbmNoise3D(bodyPosition * noise.frequency + seedShift, 4);
    // FBM の振幅合計は 0.9375。縁は最大 0.375 外へずれるので、|y| < 1.375 に収まる。
    const float r = bodyPosition.Length() - amplitude * 0.4f * n;
    return Saturate(1.0f - SmoothStep(0.3f, 1.0f, r)) * Saturate(0.65f + 0.5f * n);
}

std::uint32_t CountLiveVolumePuffs(std::span<const VolumePuff> puffs, float time)
{
    std::uint32_t count = 0;
    for (const VolumePuff& puff : puffs)
        if (Envelope(puff, time - puff.birthTime) > 0.0f) ++count;
    return count;
}

std::uint32_t PackVolumeFill(std::span<const VolumePuff> puffs, const VolumeNoiseSettings& noise,
                             std::uint32_t resolution, float time, float frameDt, VolumeFillFrame& out)
{
    out.header = {};
    out.header.resolution = resolution;
    out.header.noiseFrequency = noise.frequency;
    out.header.noiseAmplitude = std::clamp(noise.amplitude, 0.0f, 1.0f);
    out.puffs.clear();
    const float dt = (std::max)(frameDt, 1.0e-4f);

    for (const VolumePuff& puff : puffs) {
        const float age = time - puff.birthTime;
        const float envelope = Envelope(puff, age);
        if (envelope <= 0.0f) continue;
        if (out.puffs.size() >= kVolumeFillMaxPuffs) break;

        VolumeFillPuffGpu& gpu = out.puffs.emplace_back();
        const math::Vector3 center = PuffCenterAt(puff, age);
        const Matrix3x3 body = BodyMatrix(puff, age);
        for (int row = 0; row < 3; ++row) {
            const math::Vector3 b = { body.m[row][0], body.m[row][1], body.m[row][2] };
            gpu.body[row][0] = b.x;
            gpu.body[row][1] = b.y;
            gpu.body[row][2] = b.z;
            gpu.body[row][3] = -math::Vector3::Dot(b, center);
        }

        // 割線速度: x' = c1 + F (x - c0)  →  v = ((F - I) x + c1 - F c0) / Δ
        const Matrix3x3 flow = FlowMatrix(puff, age, age + dt);
        const math::Vector3 flowCenter = flow.Apply(center);
        const math::Vector3 nextCenter = PuffCenterAt(puff, age + dt);
        for (int row = 0; row < 3; ++row) {
            for (int column = 0; column < 3; ++column)
                gpu.velocity[row][column] = (flow.m[row][column] - (row == column ? 1.0f : 0.0f)) / dt;
        }
        gpu.velocity[0][3] = (nextCenter.x - flowCenter.x) / dt;
        gpu.velocity[1][3] = (nextCenter.y - flowCenter.y) / dt;
        gpu.velocity[2][3] = (nextCenter.z - flowCenter.z) / dt;

        gpu.params[0] = puff.density * envelope;
        gpu.params[1] = puff.temperature * std::exp(-age / (std::max)(puff.coolingTime, 1.0e-4f));
        gpu.params[2] = puff.noiseSeedOffset;
        gpu.params[3] = kVolumePuffBodyCullRadius;
        gpu.look[0] = Saturate(puff.colorKey);
        gpu.look[1] = Saturate(puff.liquid);
        gpu.look[2] = (std::max)(puff.noiseScale, 0.0f);
    }
    out.header.puffCount = static_cast<std::uint32_t>(out.puffs.size());
    return out.header.puffCount;
}

std::vector<VolumeBound> CollectVisibleVolumeBounds(std::span<const VolumePuff> puffs,
                                                    const VolumeNoiseSettings& noise, float time)
{
    std::vector<VolumeBound> bounds;
    for (const VolumePuff& puff : puffs) {
        const float age = time - puff.birthTime;
        // フェードの裾で薄くなった puff は数えない。
        if (Envelope(puff, age) * puff.density < 0.1f) continue;
        // 縁の揺らぎの半分までを «見える» とする (smoothstep(0.3, 1, r) の裾はほぼ透明)。
        const float amplitude = std::clamp(noise.amplitude * puff.noiseScale, 0.0f, 1.0f);
        const float visibleBodyRadius = 1.0f + 0.2f * amplitude;
        // σ ≥ 1 なので最も伸びる向きは進行方向 (横は 1/√σ ≤ 1)。
        const float sigma = StretchAt(puff, age).sigma;
        bounds.push_back({ PuffCenterAt(puff, age), visibleBodyRadius * ScaleAt(puff, age) * puff.radius * sigma });
    }
    return bounds;
}

VolumeSample SampleVolumeFill(const VolumeFillFrame& frame, const math::Vector3& x)
{
    const VolumeNoiseSettings noise{ frame.header.noiseFrequency, frame.header.noiseAmplitude };
    float densitySum = 0.0f;
    float temperatureSum = 0.0f;
    float colorSum = 0.0f;
    float liquidSum = 0.0f;
    math::Vector3 velocitySum;
    const std::size_t count = (std::min)(static_cast<std::size_t>(frame.header.puffCount), frame.puffs.size());
    for (std::size_t i = 0; i < count; ++i) {
        const VolumeFillPuffGpu& puff = frame.puffs[i];
        const math::Vector3 y = { Affine(puff.body[0], x), Affine(puff.body[1], x), Affine(puff.body[2], x) };
        if (y.LengthSq() > puff.params[3] * puff.params[3]) continue;
        const float density = puff.params[0] * EvaluatePuffBodyDensity(y, puff.params[2], noise, puff.look[2]);
        if (density <= 0.0f) continue;
        const math::Vector3 v = { Affine(puff.velocity[0], x), Affine(puff.velocity[1], x),
                                  Affine(puff.velocity[2], x) };
        densitySum += density;
        temperatureSum += density * puff.params[1] * Saturate(1.0f - y.Length());
        colorSum += density * puff.look[0];
        liquidSum += density * puff.look[1];
        velocitySum += v * density;
    }

    VolumeSample sample;
    sample.density = densitySum;
    if (densitySum > 1.0e-6f) {
        sample.temperature = temperatureSum / densitySum;
        sample.colorKey = colorSum / densitySum;
        sample.liquid = liquidSum / densitySum;
        sample.velocity = velocitySum / densitySum;
    }
    return sample;
}

} // namespace fbzz::asset
