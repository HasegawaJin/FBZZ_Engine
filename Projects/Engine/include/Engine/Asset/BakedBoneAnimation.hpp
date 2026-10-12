/// @file    BakedBoneAnimation.hpp
/// @brief   Validated skeletal clip bake, independent clocks and CPU fallback palette.
/// @author  Hasegawa Jin
/// @date    2026-10-12
#pragma once

#include <Math/Matrix4.hpp>
#include <Math/Vector4.hpp>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace fbzz::asset {
struct AnimationClip;
struct Skeleton;

enum class BoneBakeStatus : uint8_t {
    OK, INVALID_SKELETON, INVALID_CLIP, UNSUPPORTED_TRACK,
    UNSUPPORTED_SIDE_EFFECT, INVALID_SETTINGS, CAPACITY_EXCEEDED,
    INVALID_DATA, INCOMPATIBLE_SKELETON, INCOMPATIBLE_LOD, STALE_SOURCE
};

/// @note Layout is shared with BakedBoneReplay.cs.hlsl; 32-bit floats avoid quantization in the first comparison.
struct BakedBoneTransform {
    math::Vector4 translation{0.0f, 0.0f, 0.0f, 0.0f};
    math::Vector4 rotation{0.0f, 0.0f, 0.0f, 1.0f};
    math::Vector4 scale{1.0f, 1.0f, 1.0f, 0.0f};
};
static_assert(sizeof(BakedBoneTransform) == 48);

struct BakedBoneNode {
    int32_t parent = -1;
    int32_t bone = -1;
    uint32_t padding[2]{};
};
static_assert(sizeof(BakedBoneNode) == 16);

struct BakedBoneBinding {
    math::Matrix4 offset = math::Matrix4::Identity();
    uint32_t node = 0;
    uint32_t padding[3]{};
};
static_assert(sizeof(BakedBoneBinding) == 80);

/// @note Both endpoints are retained; the last interval never interpolates directly from the last interior sample to frame zero.
/// @see https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Plugins/AnimToTexture/UAnimToTextureDataAsset Bone animation data and sample rate.
struct BakedBoneAnimation {
    static constexpr uint32_t FORMAT_VERSION = 1;
    uint64_t skeletonFingerprint = 0;
    uint64_t clipFingerprint = 0;
    double durationSeconds = 0.0;
    float sampleRate = 60.0f;
    float sourceMeshRadius = 0.0f;
    float conservativeRadius = 0.0f;
    uint32_t frameCount = 0;
    uint32_t root = 0;
    bool loop = false;
    math::Matrix4 rootInverse = math::Matrix4::Identity();
    std::vector<BakedBoneNode> nodes;
    std::vector<BakedBoneBinding> bones;
    std::vector<BakedBoneTransform> samples;
};

/// @note Row clocks are per individual; immutable samples and skeleton bindings are shared per species/clip.
struct BakedBoneFrame {
    uint32_t first = 0;
    uint32_t second = 0;
    float alpha = 0.0f;
    uint32_t padding = 0;
};
static_assert(sizeof(BakedBoneFrame) == 16);

struct BoneBakeSettings {
    float sampleRate = 60.0f;
    /// @note Maximum model-space length of every input vertex in every participating mesh LOD.
    float meshRadius = 0.0f;
    uint32_t maxFrames = 65536;
    uint64_t maxSampleBytes = 256ull * 1024ull * 1024ull;
};

/// @note One workspace per Animator reuses temporary storage; failed evaluation leaves published outputs unchanged.
struct BakedBoneWorkspace {
    std::vector<uint32_t> order;
    std::vector<uint8_t> visited;
    std::vector<math::Matrix4> globals, matrices;
};

/// @return Failure leaves output untouched; unsupported tracks must continue through the ordinary Animator.
BoneBakeStatus BakeBoneAnimation(const Skeleton& skeleton, const AnimationClip& clip,
                                const BoneBakeSettings& settings, BakedBoneAnimation& output);

/// @note Content keys include hierarchy, bind TRS, inverse bind matrices, clip keys, times and loop policy; never use a raw asset pointer as a cache key.
uint64_t BoneBakeSkeletonFingerprint(const Skeleton& skeleton);
uint64_t BoneBakeClipFingerprint(const AnimationClip& clip);

BoneBakeStatus ValidateBakedBoneAnimation(const BakedBoneAnimation& bake);
BoneBakeStatus ValidateBoneBakeSource(const BakedBoneAnimation& bake, const Skeleton& skeleton,
                                    const AnimationClip& clip, float lodMeshRadius);

/// @note Two-clip bounds combine per-node maxima before hierarchy propagation; max(radiusA, radiusB) is insufficient for blended hierarchies.
BoneBakeStatus BoneBakeBlendRadius(const BakedBoneAnimation& bake, const BakedBoneAnimation& target,
                                  float& output);

/// @param elapsedSeconds Unscaled per-individual time; phase is measured in clip seconds and is added after speed.
/// @return Non-finite clocks are rejected; finite loop clocks wrap in both directions and non-looping clocks clamp.
BoneBakeStatus ResolveBakedBoneFrame(const BakedBoneAnimation& bake, double elapsedSeconds,
                                   double speed, double phaseSeconds, bool loop,
                                   BakedBoneFrame& output);

/// @note Evaluates local TRS with shortest-arc Slerp before hierarchy composition; matrices are never linearly blended.
/// @note Optional target uses the same skeleton and local two-clip blend as the GPU kernel. Failure leaves outputs untouched.
BoneBakeStatus EvaluateBakedBoneAnimation(const BakedBoneAnimation& bake, const BakedBoneFrame& frame,
                                        std::vector<math::Matrix4>& palette,
                                        std::vector<math::Matrix4>& nodeGlobals,
                                        const BakedBoneAnimation* target = nullptr,
                                        const BakedBoneFrame* targetFrame = nullptr,
                                        float blendWeight = 0.0f,
                                        BakedBoneWorkspace* workspace = nullptr);

/// @note .bba is little endian, versioned, length checked and checksummed; it is a derived cache, not an FBX replacement.
/// @see https://github.com/microsoft/DirectXTex/wiki/DirectXTex DDS supports float image payloads; a structured asset also retains skeleton/cache metadata.
BoneBakeStatus EncodeBoneBake(const BakedBoneAnimation& bake, std::vector<std::byte>& output);
BoneBakeStatus DecodeBoneBake(std::span<const std::byte> bytes, BakedBoneAnimation& output);
}
