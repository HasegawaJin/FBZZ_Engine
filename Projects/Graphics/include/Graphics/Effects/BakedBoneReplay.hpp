/// @file    BakedBoneReplay.hpp
/// @brief   Scene-independent resident bone bake upload and GPU palette replay.
/// @author  Hasegawa Jin
/// @date    2026-10-12
#pragma once
#include <Graphics/Renderer/ResourceHandle.hpp>
#include <Math/Matrix4.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace fbzz::renderer {
class ResourceManager;
struct RenderPassContext;

enum class BoneReplayStatus : uint8_t { OK, UNSUPPORTED, INVALID_SOURCE, INVALID_ROW, RESOURCE_FAILED, DISPATCH_FAILED };

/// @pre Payload was validated by the owning asset layer: transform stride 48, node stride 16, binding stride 80.
/// @note Graphics receives immutable bytes and typed resource handles; asset paths and scene pointers stay in Engine.
struct BoneReplaySource {
    std::span<const std::byte> samples;
    std::span<const std::byte> nodes;
    std::span<const std::byte> bindings;
    uint32_t nodeCount = 0;
    uint32_t boneCount = 0;
    math::Matrix4 rootInverse = math::Matrix4::Identity();
};

/// @note Frame offsets are absolute indices of the first node in a sample; target may refer to another clip in the same atlas.
struct BoneReplayRow {
    uint32_t first = 0, second = 0;
    float alpha = 0.0f;
    uint32_t padding = 0;
    uint32_t targetFirst = 0, targetSecond = 0;
    float targetAlpha = 0.0f, blendWeight = 0.0f;
};
static_assert(sizeof(BoneReplayRow) == 32);

/// @note Single owner of GPU resources; release uses ResourceManager's normal GPU-fence retirement.
struct BoneReplayGpuAsset {
    BoneReplayGpuAsset() = default;
    BoneReplayGpuAsset(const BoneReplayGpuAsset&) = delete;
    BoneReplayGpuAsset& operator=(const BoneReplayGpuAsset&) = delete;
    ~BoneReplayGpuAsset();
    ResourceHandle<StructuredBufferTag> samples, nodes, bindings;
    uint32_t sampleCount = 0, nodeCount = 0, boneCount = 0;
    uint64_t owner = 0, epoch = 0, residentBytes = 0;
    math::Matrix4 rootInverse = math::Matrix4::Identity();
};

std::unique_ptr<BoneReplayGpuAsset> UploadBakedBoneReplay(ResourceManager& resources,
    const BoneReplaySource& source, BoneReplayStatus& status);

struct BoneReplayResult {
    BoneReplayStatus status = BoneReplayStatus::UNSUPPORTED;
    ResourceHandle<StructuredBufferTag> palette;
    uint64_t rowUploadBytes = 0;
    uint32_t dispatchCount = 0;
};

/// @note Render-thread only. Palette belongs to the existing three-lane dynamic buffer pool and must not be released by callers.
/// @param shaderOverride Optional caller-owned program handle, allowing isolated tests to load the Engine shader by absolute path.
/// @return Failure publishes no palette; caller keeps its current CPU/VS skinning fallback and history.
BoneReplayResult ReplayBakedBones(RenderPassContext& context, const BoneReplayGpuAsset& asset,
                                 std::span<const BoneReplayRow> rows, ResourceHandle<ShaderTag> shaderOverride = {});
void ReleaseBakedBoneReplayCaches(ResourceManager& resources);
}
