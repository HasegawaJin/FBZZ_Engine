/// @file    BakedBoneAnimation.cpp
/// @brief   Safe skeletal bake generation, sampling, bounds and derived-cache codec.
/// @author  Hasegawa Jin
/// @date    2026-10-12
#include <Engine/Asset/BakedBoneAnimation.hpp>
#include <Engine/Asset/AnimationSampling.hpp>
#include <Engine/Asset/Skeleton.hpp>
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace fbzz::asset {
namespace {
constexpr uint32_t BONE_BAKE_MAX_NODES = 1024;
constexpr uint32_t BONE_BAKE_MAX_FRAMES = 65536;
constexpr uint64_t BONE_BAKE_MAX_BYTES = 256ull * 1024ull * 1024ull;
constexpr uint32_t BONE_BAKE_MAGIC = 0x31414242u;
constexpr uint64_t BONE_BAKE_FNV_OFFSET = 14695981039346656037ull;
constexpr uint64_t BONE_BAKE_FNV_PRIME = 1099511628211ull;

struct BoneBakeHash {
    uint64_t value = BONE_BAKE_FNV_OFFSET;
    void Byte(uint8_t b) { value = (value ^ b) * BONE_BAKE_FNV_PRIME; }
    void Integer(uint64_t v) { for (int i = 0; i < 8; ++i) { Byte(static_cast<uint8_t>(v)); v >>= 8; } }
    void Float(float v) { Integer(std::bit_cast<uint32_t>(v)); }
    void Double(double v) { Integer(std::bit_cast<uint64_t>(v)); }
    void String(std::string_view v) { Integer(v.size()); for (unsigned char c : v) Byte(c); }
    void Vector(const math::Vector3& v) { Float(v.x); Float(v.y); Float(v.z); }
    void Quaternion(const math::Quaternion& v) { Float(v.x); Float(v.y); Float(v.z); Float(v.w); }
    void Matrix(const math::Matrix4& v) { for (const auto& row : v.m) for (float x : row) Float(x); }
};

bool BoneBakeFinite(const math::Vector3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
bool BoneBakeFinite(const math::Vector4& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) && std::isfinite(v.w); }
bool BoneBakeUnit(const math::Quaternion& q)
{
    return std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z) && std::isfinite(q.w)
        && std::abs(math::Quaternion::Dot(q, q) - 1.0f) <= 0.001f;
}
math::Vector3 BoneBakeVec(const math::Vector4& v) { return {v.x, v.y, v.z}; }
math::Quaternion BoneBakeQuat(const math::Vector4& v) { return {v.x, v.y, v.z, v.w}; }
math::Vector4 BoneBakePacked(const math::Vector3& v) { return {v.x, v.y, v.z, 0.0f}; }
math::Vector4 BoneBakePacked(const math::Quaternion& v) { return {v.x, v.y, v.z, v.w}; }
bool BoneBakeValidTransform(const BakedBoneTransform& v)
{
    return BoneBakeFinite(v.translation) && BoneBakeFinite(v.scale) && BoneBakeUnit(BoneBakeQuat(v.rotation))
        && v.translation.w == 0.0f && v.scale.w == 0.0f;
}
bool BoneBakeAffine(const math::Matrix4& m)
{
    for (const auto& row : m.m) for (float v : row) if (!std::isfinite(v)) return false;
    return m.m[3][0] == 0.0f && m.m[3][1] == 0.0f && m.m[3][2] == 0.0f && m.m[3][3] == 1.0f;
}
bool BoneBakeNearMatrix(const math::Matrix4& a, const math::Matrix4& b)
{
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c)
        if (std::abs(a.m[r][c] - b.m[r][c]) > 0.0001f * (1.0f + std::abs(b.m[r][c]))) return false;
    return true;
}

bool BoneBakeOrder(const BakedBoneAnimation& b, std::vector<uint32_t>& order, std::vector<uint8_t>* visited = nullptr)
{
    if (b.nodes.empty() || b.nodes.size() > BONE_BAKE_MAX_NODES || b.root >= b.nodes.size()) return false;
    order.clear(); order.reserve(b.nodes.size());
    std::vector<uint8_t> temporary;
    auto& done = visited ? *visited : temporary;
    done.assign(b.nodes.size(), 0);
    for (size_t pass = 0; pass < b.nodes.size(); ++pass) {
        const size_t before = order.size();
        for (uint32_t i = 0; i < b.nodes.size(); ++i) {
            if (done[i]) continue;
            const int32_t parent = b.nodes[i].parent;
            if (parent < -1 || parent >= static_cast<int32_t>(b.nodes.size()) || parent == static_cast<int32_t>(i)) return false;
            if ((parent == -1) != (i == b.root)) return false;
            if (parent >= 0 && !done[static_cast<size_t>(parent)]) continue;
            done[i] = 1; order.push_back(i);
        }
        if (order.size() == b.nodes.size()) return true;
        if (order.size() == before) return false;
    }
    return false;
}

bool BoneBakeShape(const BakedBoneAnimation& b)
{
    return b.nodes.size() <= BONE_BAKE_MAX_NODES && !b.nodes.empty() && b.root < b.nodes.size()
        && !b.bones.empty() && b.bones.size() <= static_cast<size_t>(MAX_SKINNING_BONES)
        && b.frameCount >= 2 && b.frameCount <= BONE_BAKE_MAX_FRAMES
        && b.samples.size() == static_cast<uint64_t>(b.frameCount) * b.nodes.size()
        && b.samples.size() <= BONE_BAKE_MAX_BYTES / sizeof(BakedBoneTransform)
        && std::isfinite(b.durationSeconds) && b.durationSeconds > 0.0
        && std::isfinite(b.sampleRate) && b.sampleRate >= 1.0f && b.sampleRate <= 1000.0f
        && std::isfinite(b.sourceMeshRadius) && b.sourceMeshRadius >= 0.0f
        && std::isfinite(b.conservativeRadius) && b.conservativeRadius >= 0.0f
        && BoneBakeAffine(b.rootInverse);
}

/// @note sqrt(||A||1 * ||A||inf) bounds the spectral norm even for shear and nonuniform scale.
double BoneBakeLinearNorm(const math::Matrix4& m)
{
    double one = 0.0, infinity = 0.0;
    for (int i = 0; i < 3; ++i) {
        double row = 0.0, column = 0.0;
        for (int j = 0; j < 3; ++j) { row += std::abs(m.m[i][j]); column += std::abs(m.m[j][i]); }
        one = (std::max)(one, column); infinity = (std::max)(infinity, row);
    }
    return std::sqrt(one * infinity);
}
double BoneBakeTranslationNorm(const math::Matrix4& m)
{
    return std::hypot(static_cast<double>(m.m[0][3]), static_cast<double>(m.m[1][3]), static_cast<double>(m.m[2][3]));
}

float BoneBakeRadius(const BakedBoneAnimation& b, const std::vector<uint32_t>& order,
             const BakedBoneAnimation* target = nullptr)
{
    std::vector<double> localT(b.nodes.size()), localS(b.nodes.size()), globalT(b.nodes.size()), globalS(b.nodes.size());
    const auto add = [&](const BakedBoneAnimation& clip) {
        for (size_t k = 0; k < clip.samples.size(); ++k) {
            const auto& v = clip.samples[k]; const size_t n = k % b.nodes.size();
            localT[n] = (std::max)(localT[n], std::hypot(static_cast<double>(v.translation.x), static_cast<double>(v.translation.y), static_cast<double>(v.translation.z)));
            localS[n] = (std::max)(localS[n], static_cast<double>((std::max)({std::abs(v.scale.x), std::abs(v.scale.y), std::abs(v.scale.z)})));
        }
    };
    add(b); if (target) add(*target);
    for (uint32_t i : order) {
        const int32_t p = b.nodes[i].parent;
        const double pt = p < 0 ? 0.0 : globalT[static_cast<size_t>(p)];
        const double ps = p < 0 ? 1.0 : globalS[static_cast<size_t>(p)];
        globalT[i] = pt + ps * localT[i]; globalS[i] = ps * localS[i];
    }
    double radius = 0.0;
    const double meshRadius = target ? (std::max)(b.sourceMeshRadius, target->sourceMeshRadius) : b.sourceMeshRadius;
    for (const auto& bone : b.bones) {
        const double r = globalT[bone.node] + globalS[bone.node]
            * (BoneBakeTranslationNorm(bone.offset) + BoneBakeLinearNorm(bone.offset) * meshRadius);
        radius = (std::max)(radius, BoneBakeTranslationNorm(b.rootInverse) + BoneBakeLinearNorm(b.rootInverse) * r);
    }
    /// @note Float conversion and matrix accumulation must not round an otherwise conservative radius downward.
    radius = radius * 1.00001 + 0.00001;
    if (!std::isfinite(radius) || radius > (std::numeric_limits<float>::max)()) return -1.0f;
    return std::nextafter(static_cast<float>(radius), (std::numeric_limits<float>::infinity)());
}

template<class Key, class Valid>
bool BoneBakeKeys(const std::vector<Key>& keys, double duration, Valid valid)
{
    double previous = -(std::numeric_limits<double>::infinity)();
    for (const auto& k : keys) {
        if (!std::isfinite(k.time) || k.time < 0.0 || k.time > duration || k.time <= previous || !valid(k.value)) return false;
        previous = k.time;
    }
    return true;
}

BakedBoneTransform BoneBakeInterpolate(const BakedBoneTransform& a, const BakedBoneTransform& b, float t)
{
    return {BoneBakePacked(math::Vector3::Lerp(BoneBakeVec(a.translation), BoneBakeVec(b.translation), t)),
        BoneBakePacked(math::Quaternion::Slerp(BoneBakeQuat(a.rotation), BoneBakeQuat(b.rotation), t).Normalized()),
        BoneBakePacked(math::Vector3::Lerp(BoneBakeVec(a.scale), BoneBakeVec(b.scale), t))};
}
bool BoneBakeFrameValid(const BakedBoneAnimation& b, const BakedBoneFrame& f)
{
    return BoneBakeShape(b) && f.first < b.frameCount && f.second < b.frameCount
        && std::isfinite(f.alpha) && f.alpha >= 0.0f && f.alpha <= 1.0f && f.padding == 0;
}
bool BoneBakeCompatible(const BakedBoneAnimation& a, const BakedBoneAnimation& b)
{
    if (a.skeletonFingerprint != b.skeletonFingerprint || a.root != b.root
        || a.nodes.size() != b.nodes.size() || a.bones.size() != b.bones.size()) return false;
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c)
        if (a.rootInverse.m[r][c] != b.rootInverse.m[r][c]) return false;
    for (size_t i = 0; i < a.nodes.size(); ++i)
        if (a.nodes[i].parent != b.nodes[i].parent || a.nodes[i].bone != b.nodes[i].bone) return false;
    for (size_t i = 0; i < a.bones.size(); ++i) {
        if (a.bones[i].node != b.bones[i].node) return false;
        for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c)
            if (a.bones[i].offset.m[r][c] != b.bones[i].offset.m[r][c]) return false;
    }
    return true;
}

struct BoneBakeWriter {
    std::vector<std::byte> bytes;
    void U32(uint32_t v) { for (int i = 0; i < 4; ++i) { bytes.push_back(static_cast<std::byte>(v & 255)); v >>= 8; } }
    void U64(uint64_t v) { U32(static_cast<uint32_t>(v)); U32(static_cast<uint32_t>(v >> 32)); }
    void Float(float v) { U32(std::bit_cast<uint32_t>(v)); }
    void Double(double v) { U64(std::bit_cast<uint64_t>(v)); }
    void Matrix(const math::Matrix4& m) { for (const auto& row : m.m) for (float v : row) Float(v); }
    void Vector(const math::Vector4& v) { Float(v.x); Float(v.y); Float(v.z); Float(v.w); }
};
struct BoneBakeReader {
    std::span<const std::byte> bytes;
    size_t cursor = 0;
    bool valid = true;
    uint32_t U32() {
        if (cursor > bytes.size() || bytes.size() - cursor < 4) { valid = false; return 0; }
        uint32_t v = 0; for (int i = 0; i < 4; ++i) v |= std::to_integer<uint32_t>(bytes[cursor++]) << (i * 8);
        return v;
    }
    uint64_t U64() { const uint64_t low = U32(); return low | (static_cast<uint64_t>(U32()) << 32); }
    float Float() { return std::bit_cast<float>(U32()); }
    double Double() { return std::bit_cast<double>(U64()); }
    math::Matrix4 Matrix() { math::Matrix4 m; for (auto& row : m.m) for (float& v : row) v = Float(); return m; }
    math::Vector4 Vector() { const float x = Float(), y = Float(), z = Float(), w = Float(); return {x, y, z, w}; }
};
uint64_t BoneBakeChecksum(std::span<const std::byte> bytes)
{
    BoneBakeHash h; for (const auto b : bytes) h.Byte(std::to_integer<uint8_t>(b)); return h.value;
}
}

uint64_t BoneBakeSkeletonFingerprint(const Skeleton& s)
{
    BoneBakeHash h; h.Integer(BakedBoneAnimation::FORMAT_VERSION); h.Integer(static_cast<uint64_t>(s.rootNodeIndex));
    h.Matrix(s.rootInverseTransform); h.Integer(s.nodes.size()); h.Integer(s.bones.size());
    for (const auto& n : s.nodes) {
        h.String(n.name); h.Integer(static_cast<uint64_t>(n.parentIndex)); h.Integer(static_cast<uint64_t>(n.boneIndex));
        h.Vector(n.bindTranslation); h.Quaternion(n.bindRotation); h.Vector(n.bindScale); h.Matrix(n.localBindTransform);
        h.Integer(n.children.size()); for (int c : n.children) h.Integer(static_cast<uint64_t>(c));
    }
    for (const auto& b : s.bones) { h.String(b.name); h.Integer(static_cast<uint64_t>(b.nodeIndex)); h.Matrix(b.offsetMatrix); }
    return h.value;
}
uint64_t BoneBakeClipFingerprint(const AnimationClip& c)
{
    BoneBakeHash h; h.Integer(BakedBoneAnimation::FORMAT_VERSION); h.String(c.name);
    h.Double(c.durationSeconds); h.Double(c.durationTicks); h.Double(c.ticksPerSecond); h.Float(c.frameRate);
    h.Integer(c.loop); h.Integer(c.hasRootMotion); h.Integer(c.propertyTracks.size()); h.Integer(c.tracks.size());
    for (const auto& t : c.tracks) {
        h.String(t.nodeName); h.String(t.targetPath); h.Integer(static_cast<uint8_t>(t.interp));
        h.Integer(t.positions.size()); for (const auto& k : t.positions) { h.Double(k.time); h.Vector(k.value); }
        h.Integer(t.rotations.size()); for (const auto& k : t.rotations) { h.Double(k.time); h.Quaternion(k.value); }
        h.Integer(t.scales.size()); for (const auto& k : t.scales) { h.Double(k.time); h.Vector(k.value); }
    }
    return h.value;
}

BoneBakeStatus ValidateBakedBoneAnimation(const BakedBoneAnimation& b)
{
    if (!BoneBakeShape(b)) return BoneBakeStatus::INVALID_DATA;
    std::vector<uint32_t> order;
    if (!BoneBakeOrder(b, order)) return BoneBakeStatus::INVALID_SKELETON;
    for (size_t i = 0; i < b.nodes.size(); ++i) {
        const int32_t bone = b.nodes[i].bone;
        if (bone < -1 || bone >= static_cast<int32_t>(b.bones.size())) return BoneBakeStatus::INVALID_SKELETON;
        if (bone >= 0 && b.bones[static_cast<size_t>(bone)].node != i) return BoneBakeStatus::INVALID_SKELETON;
        if (b.nodes[i].padding[0] || b.nodes[i].padding[1]) return BoneBakeStatus::INVALID_DATA;
    }
    for (size_t i = 0; i < b.bones.size(); ++i) {
        const auto& bone = b.bones[i];
        if (bone.node >= b.nodes.size() || b.nodes[bone.node].bone != static_cast<int32_t>(i) || !BoneBakeAffine(bone.offset))
            return BoneBakeStatus::INVALID_SKELETON;
        for (uint32_t p : bone.padding) if (p) return BoneBakeStatus::INVALID_DATA;
    }
    for (const auto& v : b.samples) if (!BoneBakeValidTransform(v)) return BoneBakeStatus::INVALID_DATA;
    const float minimumRadius = BoneBakeRadius(b, order);
    if (minimumRadius < 0.0f || b.conservativeRadius < minimumRadius) return BoneBakeStatus::INVALID_DATA;
    return BoneBakeStatus::OK;
}

BoneBakeStatus BakeBoneAnimation(const Skeleton& s, const AnimationClip& c,
                                const BoneBakeSettings& settings, BakedBoneAnimation& output)
{
    if (!std::isfinite(settings.sampleRate) || settings.sampleRate < 1.0f || settings.sampleRate > 1000.0f
        || !std::isfinite(settings.meshRadius) || settings.meshRadius < 0.0f || settings.maxFrames < 2
        || settings.maxFrames > BONE_BAKE_MAX_FRAMES || settings.maxSampleBytes > BONE_BAKE_MAX_BYTES || settings.maxSampleBytes < 48)
        return BoneBakeStatus::INVALID_SETTINGS;
    if (!std::isfinite(c.durationSeconds) || c.durationSeconds <= 0.0 || !std::isfinite(c.durationTicks)
        || c.durationTicks <= 0.0 || !std::isfinite(c.ticksPerSecond) || c.ticksPerSecond <= 0.0
        || std::abs(c.durationSeconds * c.ticksPerSecond - c.durationTicks) > 0.0001 * (1.0 + c.durationTicks))
        return BoneBakeStatus::INVALID_CLIP;
    if (c.hasRootMotion || !c.propertyTracks.empty()) return BoneBakeStatus::UNSUPPORTED_SIDE_EFFECT;
    if (s.nodes.empty() || s.nodes.size() > BONE_BAKE_MAX_NODES || s.bones.empty() || s.bones.size() > MAX_SKINNING_BONES
        || s.rootNodeIndex < 0 || static_cast<size_t>(s.rootNodeIndex) >= s.nodes.size() || !BoneBakeAffine(s.rootInverseTransform))
        return BoneBakeStatus::INVALID_SKELETON;
    const double intervals = std::ceil(c.durationSeconds * settings.sampleRate);
    if (!std::isfinite(intervals) || intervals >= settings.maxFrames || intervals < 1.0
        || (intervals + 1.0) * s.nodes.size() > settings.maxSampleBytes / sizeof(BakedBoneTransform))
        return BoneBakeStatus::CAPACITY_EXCEEDED;

    BakedBoneAnimation b;
    b.skeletonFingerprint = BoneBakeSkeletonFingerprint(s); b.clipFingerprint = BoneBakeClipFingerprint(c);
    b.durationSeconds = c.durationSeconds; b.sampleRate = settings.sampleRate; b.sourceMeshRadius = settings.meshRadius;
    b.frameCount = static_cast<uint32_t>(intervals) + 1u; b.root = static_cast<uint32_t>(s.rootNodeIndex); b.loop = c.loop;
    b.rootInverse = s.rootInverseTransform;
    std::unordered_map<std::string, uint32_t> names;
    for (uint32_t i = 0; i < s.nodes.size(); ++i) {
        const auto& n = s.nodes[i];
        if (n.name.empty() || !names.emplace(n.name, i).second || !BoneBakeFinite(n.bindTranslation) || !BoneBakeFinite(n.bindScale)
            || !BoneBakeUnit(n.bindRotation) || !BoneBakeAffine(n.localBindTransform)
            || !BoneBakeNearMatrix(math::Matrix4::TRS(n.bindTranslation, n.bindRotation, n.bindScale), n.localBindTransform))
            return BoneBakeStatus::INVALID_SKELETON;
        b.nodes.push_back({n.parentIndex, n.boneIndex, {}});
    }
    for (const auto& bone : s.bones) {
        if (bone.nodeIndex < 0 || static_cast<size_t>(bone.nodeIndex) >= s.nodes.size() || !BoneBakeAffine(bone.offsetMatrix))
            return BoneBakeStatus::INVALID_SKELETON;
        b.bones.push_back({bone.offsetMatrix, static_cast<uint32_t>(bone.nodeIndex), {}});
    }
    std::vector<uint32_t> order;
    if (!BoneBakeOrder(b, order)) return BoneBakeStatus::INVALID_SKELETON;
    std::vector<uint32_t> childCount(s.nodes.size());
    for (size_t i = 0; i < s.nodes.size(); ++i) {
        std::unordered_set<int> children;
        for (int child : s.nodes[i].children) {
            if (child < 0 || static_cast<size_t>(child) >= s.nodes.size() || !children.insert(child).second
                || s.nodes[static_cast<size_t>(child)].parentIndex != static_cast<int>(i)) return BoneBakeStatus::INVALID_SKELETON;
            ++childCount[static_cast<size_t>(child)];
        }
    }
    for (size_t i = 0; i < s.nodes.size(); ++i)
        if (childCount[i] != (i == b.root ? 0u : 1u)) return BoneBakeStatus::INVALID_SKELETON;
    std::vector<const NodeAnimationTrack*> tracks(s.nodes.size());
    for (const auto& t : c.tracks) {
        const auto it = names.find(t.nodeName);
        if (it == names.end() || tracks[it->second] || t.interp != AnimInterp::Linear)
            return BoneBakeStatus::UNSUPPORTED_TRACK;
        if (!BoneBakeKeys(t.positions, c.durationTicks, [](const auto& v) { return BoneBakeFinite(v); })
            || !BoneBakeKeys(t.scales, c.durationTicks, [](const auto& v) { return BoneBakeFinite(v); })
            || !BoneBakeKeys(t.rotations, c.durationTicks, [](const auto& v) { return BoneBakeUnit(v); })) return BoneBakeStatus::INVALID_CLIP;
        tracks[it->second] = &t;
    }
    b.samples.reserve(static_cast<size_t>(b.frameCount) * b.nodes.size());
    for (uint32_t frame = 0; frame < b.frameCount; ++frame) {
        const double ticks = c.durationTicks * (static_cast<double>(frame) / (b.frameCount - 1u));
        for (size_t i = 0; i < s.nodes.size(); ++i) {
            const auto& n = s.nodes[i]; const auto* t = tracks[i];
            b.samples.push_back({
                BoneBakePacked(t ? SampleVectorKeys(t->positions, ticks, n.bindTranslation) : n.bindTranslation),
                BoneBakePacked(t ? SampleQuaternionKeys(t->rotations, ticks, n.bindRotation) : n.bindRotation),
                BoneBakePacked(t ? SampleVectorKeys(t->scales, ticks, n.bindScale) : n.bindScale)});
        }
    }
    b.conservativeRadius = BoneBakeRadius(b, order);
    if (const auto status = ValidateBakedBoneAnimation(b); status != BoneBakeStatus::OK) return status;
    output = std::move(b); return BoneBakeStatus::OK;
}

BoneBakeStatus ValidateBoneBakeSource(const BakedBoneAnimation& b, const Skeleton& s,
                                    const AnimationClip& c, float lodMeshRadius)
{
    if (const auto status = ValidateBakedBoneAnimation(b); status != BoneBakeStatus::OK) return status;
    if (BoneBakeSkeletonFingerprint(s) != b.skeletonFingerprint) return BoneBakeStatus::INCOMPATIBLE_SKELETON;
    if (BoneBakeClipFingerprint(c) != b.clipFingerprint) return BoneBakeStatus::STALE_SOURCE;
    if (!std::isfinite(lodMeshRadius) || lodMeshRadius < 0.0f || lodMeshRadius > b.sourceMeshRadius)
        return BoneBakeStatus::INCOMPATIBLE_LOD;
    return BoneBakeStatus::OK;
}

BoneBakeStatus ResolveBakedBoneFrame(const BakedBoneAnimation& b, double elapsed, double speed,
                                   double phase, bool loop, BakedBoneFrame& output)
{
    if (!BoneBakeShape(b) || !std::isfinite(elapsed) || !std::isfinite(speed) || !std::isfinite(phase)) return BoneBakeStatus::INVALID_DATA;
    double time = elapsed * speed + phase;
    if (!std::isfinite(time)) return BoneBakeStatus::INVALID_DATA;
    if (loop) { time = std::fmod(time, b.durationSeconds); if (time < 0.0) time += b.durationSeconds; }
    else time = std::clamp(time, 0.0, b.durationSeconds);
    const double position = std::clamp(time / b.durationSeconds * (b.frameCount - 1u), 0.0, static_cast<double>(b.frameCount - 1u));
    const uint32_t first = static_cast<uint32_t>(std::floor(position));
    output = {first, (std::min)(first + 1u, b.frameCount - 1u), static_cast<float>(position - first), 0};
    return BoneBakeStatus::OK;
}

BoneBakeStatus BoneBakeBlendRadius(const BakedBoneAnimation& b, const BakedBoneAnimation& target, float& output)
{
    if (ValidateBakedBoneAnimation(b) != BoneBakeStatus::OK || ValidateBakedBoneAnimation(target) != BoneBakeStatus::OK)
        return BoneBakeStatus::INVALID_DATA;
    if (!BoneBakeCompatible(b, target)) return BoneBakeStatus::INCOMPATIBLE_SKELETON;
    std::vector<uint32_t> order;
    if (!BoneBakeOrder(b, order)) return BoneBakeStatus::INVALID_SKELETON;
    const float radius = BoneBakeRadius(b, order, &target);
    if (radius < 0.0f) return BoneBakeStatus::INVALID_DATA;
    output = radius; return BoneBakeStatus::OK;
}

BoneBakeStatus EvaluateBakedBoneAnimation(const BakedBoneAnimation& b, const BakedBoneFrame& f,
                                        std::vector<math::Matrix4>& palette, std::vector<math::Matrix4>& nodeGlobals,
                                        const BakedBoneAnimation* target, const BakedBoneFrame* tf, float weight,
                                        BakedBoneWorkspace* workspace)
{
    if (!BoneBakeFrameValid(b, f) || !std::isfinite(weight) || weight < 0.0f || weight > 1.0f
        || ((target == nullptr) != (tf == nullptr))) return BoneBakeStatus::INVALID_DATA;
    if (target && !BoneBakeCompatible(b, *target)) return BoneBakeStatus::INCOMPATIBLE_SKELETON;
    if (target && !BoneBakeFrameValid(*target, *tf)) return BoneBakeStatus::INVALID_DATA;
    BakedBoneWorkspace temporary;
    auto& scratch = workspace ? *workspace : temporary;
    auto& order = scratch.order;
    if (!BoneBakeOrder(b, order, &scratch.visited)) return BoneBakeStatus::INVALID_SKELETON;
    auto& globals = scratch.globals; auto& matrices = scratch.matrices;
    globals.resize(b.nodes.size()); matrices.resize(b.bones.size());
    for (uint32_t i : order) {
        const auto& a = b.samples[static_cast<size_t>(f.first) * b.nodes.size() + i];
        const auto& z = b.samples[static_cast<size_t>(f.second) * b.nodes.size() + i];
        if (!BoneBakeValidTransform(a) || !BoneBakeValidTransform(z)) return BoneBakeStatus::INVALID_DATA;
        auto local = BoneBakeInterpolate(a, z, f.alpha);
        if (target) {
            const auto& ta = target->samples[static_cast<size_t>(tf->first) * b.nodes.size() + i];
            const auto& tz = target->samples[static_cast<size_t>(tf->second) * b.nodes.size() + i];
            if (!BoneBakeValidTransform(ta) || !BoneBakeValidTransform(tz)) return BoneBakeStatus::INVALID_DATA;
            local = BoneBakeInterpolate(local, BoneBakeInterpolate(ta, tz, tf->alpha), weight);
        }
        const auto m = math::Matrix4::TRS(BoneBakeVec(local.translation), BoneBakeQuat(local.rotation), BoneBakeVec(local.scale));
        globals[i] = b.nodes[i].parent < 0 ? m : globals[static_cast<size_t>(b.nodes[i].parent)] * m;
        if (!BoneBakeAffine(globals[i])) return BoneBakeStatus::INVALID_DATA;
    }
    for (size_t i = 0; i < b.bones.size(); ++i) {
        const auto& bone = b.bones[i];
        if (bone.node >= globals.size() || b.nodes[bone.node].bone != static_cast<int32_t>(i) || !BoneBakeAffine(bone.offset))
            return BoneBakeStatus::INVALID_SKELETON;
        matrices[i] = b.rootInverse * globals[bone.node] * bone.offset;
        if (!BoneBakeAffine(matrices[i])) return BoneBakeStatus::INVALID_DATA;
    }
    palette.swap(matrices); nodeGlobals.swap(globals); return BoneBakeStatus::OK;
}

BoneBakeStatus EncodeBoneBake(const BakedBoneAnimation& b, std::vector<std::byte>& output)
{
    if (const auto status = ValidateBakedBoneAnimation(b); status != BoneBakeStatus::OK) return status;
    BoneBakeWriter w; w.U32(BONE_BAKE_MAGIC); w.U32(BakedBoneAnimation::FORMAT_VERSION);
    w.U64(b.skeletonFingerprint); w.U64(b.clipFingerprint); w.Double(b.durationSeconds);
    w.Float(b.sampleRate); w.Float(b.sourceMeshRadius); w.Float(b.conservativeRadius);
    w.U32(b.frameCount); w.U32(b.root); w.U32(b.loop ? 1u : 0u);
    w.U32(static_cast<uint32_t>(b.nodes.size())); w.U32(static_cast<uint32_t>(b.bones.size()));
    w.Matrix(b.rootInverse);
    for (const auto& n : b.nodes) { w.U32(std::bit_cast<uint32_t>(n.parent)); w.U32(std::bit_cast<uint32_t>(n.bone)); }
    for (const auto& bone : b.bones) { w.Matrix(bone.offset); w.U32(bone.node); }
    for (const auto& s : b.samples) { w.Vector(s.translation); w.Vector(s.rotation); w.Vector(s.scale); }
    w.U64(BoneBakeChecksum(w.bytes)); output = std::move(w.bytes); return BoneBakeStatus::OK;
}

BoneBakeStatus DecodeBoneBake(std::span<const std::byte> bytes, BakedBoneAnimation& output)
{
    constexpr size_t HEADER_BYTES = 128;
    if (bytes.size() < HEADER_BYTES + 8u || bytes.size() > BONE_BAKE_MAX_BYTES + 1024u * 8u + 128u * 68u + HEADER_BYTES + 8u)
        return BoneBakeStatus::INVALID_DATA;
    BoneBakeReader checksumReader{bytes.last(8)};
    if (BoneBakeChecksum(bytes.first(bytes.size() - 8u)) != checksumReader.U64()) return BoneBakeStatus::INVALID_DATA;
    BoneBakeReader r{bytes.first(bytes.size() - 8u)};
    if (r.U32() != BONE_BAKE_MAGIC || r.U32() != BakedBoneAnimation::FORMAT_VERSION) return BoneBakeStatus::INVALID_DATA;
    BakedBoneAnimation b;
    b.skeletonFingerprint = r.U64(); b.clipFingerprint = r.U64(); b.durationSeconds = r.Double();
    b.sampleRate = r.Float(); b.sourceMeshRadius = r.Float(); b.conservativeRadius = r.Float();
    b.frameCount = r.U32(); b.root = r.U32(); const uint32_t loop = r.U32(); b.loop = loop != 0;
    const uint32_t nodes = r.U32(), bones = r.U32(); b.rootInverse = r.Matrix();
    if (!r.valid || r.cursor != HEADER_BYTES || loop > 1u || nodes == 0 || nodes > BONE_BAKE_MAX_NODES
        || bones == 0 || bones > MAX_SKINNING_BONES || b.frameCount < 2 || b.frameCount > BONE_BAKE_MAX_FRAMES) return BoneBakeStatus::INVALID_DATA;
    const uint64_t count = static_cast<uint64_t>(nodes) * b.frameCount;
    if (count > BONE_BAKE_MAX_BYTES / sizeof(BakedBoneTransform)
        || HEADER_BYTES + nodes * 8ull + bones * 68ull + count * 48ull != r.bytes.size()) return BoneBakeStatus::INVALID_DATA;
    b.nodes.reserve(nodes); b.bones.reserve(bones); b.samples.reserve(static_cast<size_t>(count));
    for (uint32_t i = 0; i < nodes; ++i) { const auto parent = std::bit_cast<int32_t>(r.U32()), bone = std::bit_cast<int32_t>(r.U32()); b.nodes.push_back({parent, bone, {}}); }
    for (uint32_t i = 0; i < bones; ++i) { const auto matrix = r.Matrix(); const auto node = r.U32(); b.bones.push_back({matrix, node, {}}); }
    for (uint64_t i = 0; i < count; ++i) { const auto t = r.Vector(), q = r.Vector(), s = r.Vector(); b.samples.push_back({t, q, s}); }
    if (!r.valid || r.cursor != r.bytes.size()) return BoneBakeStatus::INVALID_DATA;
    if (const auto status = ValidateBakedBoneAnimation(b); status != BoneBakeStatus::OK) return status;
    output = std::move(b); return BoneBakeStatus::OK;
}
}
