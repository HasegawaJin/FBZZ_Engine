/// @file    SkinningComputeCache.hpp
/// @brief   コンピュートスキニングが借用する GPU 入力の内部境界。
/// @author  Hasegawa Jin
/// @date    2026-11-21
#pragma once

#include <Graphics/Renderer/Resources/ResourceHandle.hpp>
#include <Engine/Scene/References/Entity.hpp>
#include <functional>
#include <vector>

namespace fbzz::math { struct Matrix4; }
namespace fbzz::renderer {
class ResourceManager;
struct Mesh;
namespace buffer_state_detail { class BufferStateReadScope; }
}

namespace fbzz::scene {
struct AnimationSkinningCpuRow;
struct RenderPassContext;

namespace skinning_cache_detail {

struct EntityIDHash {
    size_t operator()(const EntityID& id) const noexcept
    {
        size_t result = std::hash<uint32_t>{}(id.index);
        result ^= std::hash<uint32_t>{}(id.generation) + 0x9e3779b9u +
                  (result << 6u) + (result >> 2u);
        return result;
    }
};

/// @note 前フレームの全ビューで未使用だった入力だけを回収する。
void BeginSkinningCacheFrame(renderer::ResourceManager& resources, uint64_t frameStamp, AnimationSkinningCpuRow* trace);
/// @note EntityID は同一 Scene 内だけで一意なので、ビューの準備前にパレット索引を消す。
void ResetSkinningPalettes();
[[nodiscard]] bool HasSkinningPalette(EntityID entity);

/// @note 再利用した入力もフレーム全体の使用集合へ含める。
void RetainSkinningSource(const renderer::Mesh& mesh, renderer::ResourceHandle<renderer::BufferTag> source,
    uint64_t contentVersion);
[[nodiscard]] renderer::ResourceHandle<renderer::StructuredBufferTag> EnsureSourceVertexBuffer(
    renderer::ResourceManager& resources, const renderer::Mesh& mesh,
    renderer::ResourceHandle<renderer::BufferTag> source, size_t boneCount, AnimationSkinningCpuRow* trace,
    renderer::buffer_state_detail::BufferStateReadScope* bufferReads = nullptr, uint64_t* capturedVersion = nullptr);
[[nodiscard]] renderer::ResourceHandle<renderer::StructuredBufferTag> AcquireBonePalette(
    renderer::ResourceManager& resources, EntityID animatorEntity,
    const std::vector<math::Matrix4>& matrices, AnimationSkinningCpuRow* trace);

void RecordDeferredSkinningDispatches(uint32_t dispatches, uint64_t vertices);
void AccumulateSkinningFrameStats(RenderPassContext& context);

}
}
