/// @file    VFXLineGeometry.cpp
/// @brief   VFX Line の形と明るさ
/// @author  Hasegawa Jin
/// @date    2026-09-12
#include <Engine/Scene/VFXLineGeometry.hpp>

#include <Math/CurlNoise.hpp>

#include <algorithm>
#include <cmath>

namespace fbzz::scene {
namespace {

constexpr float kTwoPi = 6.28318530718f;
constexpr float kDegreesToRadians = 3.14159265359f / 180.0f;

/// 決定論的な乱数 [0,1)。
class LineRandom {
public:
    explicit LineRandom(std::uint32_t seed) : m_state(math::PcgHash(seed * 2654435761u + 0x9e3779b9u) | 1u) {}
    float Next()
    {
        m_state = math::PcgHash(m_state + 0x6d2b79f5u);
        return static_cast<float>(m_state >> 8) * (1.0f / 16777216.0f);
    }
    float Signed() { return Next() * 2.0f - 1.0f; }

private:
    std::uint32_t m_state;
};

/// 線に直交する 2 軸。真上・真下へ伸びる線 (落雷) で外積が縮退しないよう、基準を切り替える。
void Basis(const math::Vector3& direction, math::Vector3& outRight, math::Vector3& outUp)
{
    const math::Vector3 reference = std::fabs(math::Vector3::Dot(direction, math::Vector3::UP)) > 0.99f
        ? math::Vector3::FORWARD : math::Vector3::UP;
    outRight = math::Vector3::Cross(direction, reference).NormalizedOr(math::Vector3::RIGHT);
    outUp = math::Vector3::Cross(outRight, direction).NormalizedOr(math::Vector3::UP);
}

/// 中点変位 (midpoint displacement)。段ごとに区間を 2 分し、中点を線に直交する向きへずらす。
/// ずらす量は段ごとに半分にする — 大きな折れと細かい震えが入れ子になり、稲妻の «自己相似» になる。
std::vector<math::Vector3> Displace(const math::Vector3& from, const math::Vector3& to, int levels, float offset,
                                    LineRandom& random)
{
    std::vector<math::Vector3> points = { from, to };
    const math::Vector3 delta = to - from;
    const float length = delta.Length();
    math::Vector3 right{}, up{};
    Basis(length > 1.0e-6f ? delta * (1.0f / length) : math::Vector3::FORWARD, right, up);
    for (int level = 0; level < levels; ++level) {
        std::vector<math::Vector3> next;
        next.reserve(points.size() * 2);
        for (std::size_t i = 0; i + 1 < points.size(); ++i) {
            next.push_back(points[i]);
            const float angle = random.Next() * kTwoPi;
            const float amount = random.Signed() * offset;
            const math::Vector3 mid = (points[i] + points[i + 1]) * 0.5f
                + (right * std::cos(angle) + up * std::sin(angle)) * amount;
            next.push_back(mid);
        }
        next.push_back(points.back());
        points = std::move(next);
        offset *= 0.5f;
    }
    return points;
}

} // namespace

void GenerateLightning(const VFXLineComponent& line, const math::Vector3& from, const math::Vector3& to,
                       std::uint32_t strikeSeed, float jitterTime, std::vector<VFXLineStrand>& out)
{
    out.clear();
    const math::Vector3 delta = to - from;
    const float length = delta.Length();
    if (length < 1.0e-4f) return;
    const math::Vector3 direction = delta * (1.0f / length);
    math::Vector3 right{}, up{};
    Basis(direction, right, up);

    LineRandom random(strikeSeed);
    const int levels = std::clamp(line.detail, 1, 9);
    VFXLineStrand main;
    main.points = Displace(from, to, levels, (std::max)(line.chaos, 0.0f) * length, random);
    main.endTaper = std::clamp(line.endTaper, 0.0f, 1.0f);

    /// @note 打ち直しの間の震え。両端は動かさない (端が離れると «繋がっていない» に見える)。
    const float jitter = (std::max)(line.jitter, 0.0f) * length;
    if (jitter > 0.0f) {
        const std::size_t last = main.points.size() - 1;
        for (std::size_t i = 1; i < last; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(last);
            const float taper = 4.0f * t * (1.0f - t);
            const float phase = jitterTime * 37.0f + static_cast<float>(i) * 1.7f;
            main.points[i] += (right * std::sin(phase) + up * std::cos(phase * 1.31f)) * (jitter * taper);
        }
    }
    /// @note 枝は本流の点を参照しながら out へ積む。途中で out が伸び直すと参照が宙に浮くので、先に枠を取る。
    const int branches = std::clamp(line.branchCount, 0, 16);
    out.reserve(static_cast<std::size_t>(branches) + 1);
    out.push_back(main);

    /// @note 枝。本流の途中から、本流の向きを少し開いた方向へ伸びる短い雷。
    const std::vector<math::Vector3>& trunk = out.front().points;
    for (int b = 0; b < branches && trunk.size() >= 3; ++b) {
        const float along = 0.15f + random.Next() * 0.6f;
        const std::size_t index = std::clamp(static_cast<std::size_t>(along * static_cast<float>(trunk.size() - 1)),
                                             std::size_t{ 1 }, trunk.size() - 2);
        const math::Vector3 start = trunk[index];
        const math::Vector3 localDirection = (trunk[index + 1] - trunk[index - 1]).NormalizedOr(direction);
        math::Vector3 sideA{}, sideB{};
        Basis(localDirection, sideA, sideB);
        const float spin = random.Next() * kTwoPi;
        const math::Vector3 side = sideA * std::cos(spin) + sideB * std::sin(spin);
        const float angle = (std::max)(line.branchAngle, 0.0f) * kDegreesToRadians * (0.6f + 0.8f * random.Next());
        const math::Vector3 branchDirection =
            (localDirection * std::cos(angle) + side * std::sin(angle)).NormalizedOr(localDirection);
        const float branchLength = (std::max)(line.branchLength, 0.0f) * length * (0.5f + 0.5f * random.Next());
        if (branchLength < 1.0e-4f) continue;

        VFXLineStrand branch;
        branch.points = Displace(start, start + branchDirection * branchLength, (std::max)(levels - 2, 1),
                                 (std::max)(line.chaos, 0.0f) * branchLength, random);
        branch.width = (std::max)(line.branchWidth, 0.0f);
        branch.brightness = (std::max)(line.branchBrightness, 0.0f);
        branch.endTaper = 0.0f;
        out.push_back(std::move(branch));
    }
}

void GenerateBeam(const VFXLineComponent& line, const math::Vector3& from, const math::Vector3& to, float time,
                  std::vector<VFXLineStrand>& out)
{
    out.clear();
    const math::Vector3 delta = to - from;
    const float length = delta.Length();
    if (length < 1.0e-4f) return;
    math::Vector3 right{}, up{};
    Basis(delta * (1.0f / length), right, up);

    VFXLineStrand beam;
    beam.endTaper = std::clamp(line.endTaper, 0.0f, 1.0f);
    const int segments = std::clamp(line.beamSegments, 1, 128);
    const float phase = time * line.wobbleFrequency * kTwoPi;
    beam.points.reserve(static_cast<std::size_t>(segments) + 1);
    for (int i = 0; i <= segments; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(segments);
        math::Vector3 point = from + delta * t;
        /// @note たるみは放物線 (4t(1-t))。sin では «張った紐» に見えない。横ゆれも両端を固定する。
        const float taper = 4.0f * t * (1.0f - t);
        point.y -= line.sag * taper;
        if (line.wobble != 0.0f) {
            const float wave = t * line.wobbleWaves * kTwoPi;
            point += right * (std::sin(wave + phase) * line.wobble * taper);
            point += up * (std::cos(wave * 1.37f + phase * 0.8f) * line.wobble * taper);
        }
        beam.points.push_back(point);
    }
    out.push_back(std::move(beam));
}

float VFXLineEnvelope(const VFXLineComponent& line, float time)
{
    const float fadeIn = (std::max)(line.fadeIn, 0.0f);
    const float fadeOut = (std::max)(line.fadeOut, 0.0f);
    if (line.duration <= 0.0f) return fadeIn > 0.0f ? std::clamp(time / fadeIn, 0.0f, 1.0f) : 1.0f;
    float local = time;
    if (line.loop) local = std::fmod((std::max)(time, 0.0f), line.duration);
    else if (time >= line.duration) return 0.0f;
    const float in = fadeIn > 0.0f ? std::clamp(local / fadeIn, 0.0f, 1.0f) : 1.0f;
    const float outValue = fadeOut > 0.0f ? std::clamp((line.duration - local) / fadeOut, 0.0f, 1.0f) : 1.0f;
    return (std::min)(in, outValue);
}

float VFXLineBrightness(const VFXLineComponent& line, float time, std::uint32_t strikeIndex, float strikeClock)
{
    const float flicker = std::clamp(line.flicker, 0.0f, 1.0f);
    float pulse = 1.0f;
    if (line.mode == VFXLineMode::Lightning) {
        /// @note 打つたびに強さが揺れ、打った瞬間から落ちていく。雷の «バチッ» はこの落ち方で出る。
        const float strength = 1.0f - flicker * (static_cast<float>(math::PcgHash(strikeIndex * 747796405u + 1u) >> 8)
                                                 * (1.0f / 16777216.0f));
        pulse = strength * (1.0f - flicker * 0.5f * (1.0f - std::exp(-(std::max)(strikeClock, 0.0f) * 12.0f)));
    } else {
        pulse = 1.0f - flicker * 0.3f * (0.5f + 0.5f * std::sin(time * 37.0f) * std::sin(time * 11.3f));
    }
    return VFXLineEnvelope(line, time) * std::clamp(pulse, 0.0f, 1.0f);
}

const char* VFXLinePresetName(VFXLinePreset preset)
{
    switch (preset) {
    case VFXLinePreset::Lightning:   return "Lightning";
    case VFXLinePreset::ElectricArc: return "Electric Arc";
    case VFXLinePreset::Laser:       return "Laser";
    case VFXLinePreset::EnergyBeam:  return "Energy Beam";
    case VFXLinePreset::Tether:      return "Tether";
    case VFXLinePreset::Count:       break;
    }
    return "";
}

void ApplyVFXLinePreset(VFXLineComponent& line, VFXLinePreset preset)
{
    const VFXLineComponent defaults{};
    /// @note 端点・マテリアル・seed は «どこに置いたか» なので残し、形と見た目だけを入れ替える。
    const VFXLineComponent keep = line;
    line = defaults;
    line.enabled = keep.enabled;
    line.fromEntity = keep.fromEntity;
    line.toEntity = keep.toEntity;
    line.fromOffset = keep.fromOffset;
    line.toPoint = keep.toPoint;
    line.disableWhenEndpointMissing = keep.disableWhenEndpointMissing;
    line.materialPath = keep.materialPath;
    line.seed = keep.seed;

    switch (preset) {
    case VFXLinePreset::Lightning:
        line.mode = VFXLineMode::Lightning;
        line.color = { 0.35f, 0.55f, 1.0f, 1.0f };
        line.intensity = 6.0f;
        line.width = 0.18f;
        line.detail = 7;
        line.chaos = 0.2f;
        line.branchCount = 4;
        line.strikeRate = 10.0f;
        line.flicker = 0.7f;
        break;
    case VFXLinePreset::ElectricArc:
        line.mode = VFXLineMode::Lightning;
        line.color = { 0.3f, 0.75f, 1.0f, 1.0f };
        line.intensity = 3.0f;
        line.width = 0.06f;
        line.detail = 5;
        line.chaos = 0.12f;
        line.branchCount = 1;
        line.branchLength = 0.2f;
        line.strikeRate = 24.0f;
        line.flicker = 0.35f;
        line.jitter = 0.02f;
        break;
    case VFXLinePreset::Laser:
        line.mode = VFXLineMode::Beam;
        line.color = { 1.0f, 0.12f, 0.08f, 1.0f };
        line.intensity = 5.0f;
        line.width = 0.08f;
        line.coreWidth = 0.35f;
        line.endTaper = 1.0f;
        line.breakup = 0.1f;
        line.flicker = 0.15f;
        break;
    case VFXLinePreset::EnergyBeam:
        line.mode = VFXLineMode::Beam;
        line.color = { 0.4f, 0.9f, 1.0f, 1.0f };
        line.intensity = 4.0f;
        line.width = 0.45f;
        line.coreWidth = 0.3f;
        line.endTaper = 0.8f;
        line.wobble = 0.03f;
        line.wobbleFrequency = 6.0f;
        line.breakup = 0.5f;
        line.pulseSpeed = 6.0f;
        line.pulseDensity = 4.0f;
        line.flicker = 0.25f;
        break;
    case VFXLinePreset::Tether:
        line.mode = VFXLineMode::Beam;
        line.color = { 0.7f, 0.4f, 1.0f, 0.9f };
        line.intensity = 2.0f;
        line.width = 0.07f;
        line.endTaper = 1.0f;
        line.sag = 0.4f;
        line.wobble = 0.05f;
        line.wobbleFrequency = 2.0f;
        line.pulseSpeed = -2.0f;
        line.pulseDensity = 6.0f;
        line.flicker = 0.1f;
        break;
    case VFXLinePreset::Count:
        break;
    }
}

} // namespace fbzz::scene
