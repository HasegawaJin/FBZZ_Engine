/// @file    RayGeometryCache.hpp
/// @brief   静的 BLAS と対象集合別 TLAS・ヒット表を同じ版で公開する。
/// @author  Hasegawa Jin
/// @date    2026-09-30
#pragma once
#include <Graphics/RayTracing/RayScene.hpp>
#include <Graphics/Pipeline/RenderPipeline.hpp>

namespace fbzz::renderer {

/// @note LAYOUT: RayDebug.cs.hlsl の RayHitRecord と一致させる。
struct RayHitRecord {
    uint32_t vertexSrv = UINT32_MAX;
    uint32_t indexSrv = UINT32_MAX;
    uint32_t vertexStride = 0;
    uint32_t positionOffset = 0;
    uint32_t firstVertex = 0;
    uint32_t firstIndex = 0;
    uint32_t indexCount = 0;
    uint32_t vertexCount = 0;
    uint32_t objectIndex = 0;
    uint32_t objectGeneration = 0;
    uint32_t sceneGenerationLow = 0;
    uint32_t sceneGenerationHigh = 0;
    bool operator==(const RayHitRecord&) const = default;
};
static_assert(sizeof(RayHitRecord) == 48);

/// @note 非所有の公開結果。次の Prepare / Release まで CPU で参照し、GPU 退役は ResourceManager が保護する。
struct RaySceneGpu {
    ResourceHandle<AccelerationStructureTag> topLevel;
    ResourceHandle<StructuredBufferTag> hitRecords;
    ResourceHandle<StructuredBufferTag> surfaceMaterials;
    std::vector<ResourceHandle<BufferTag>> readBuffers;
    std::vector<ResourceHandle<TextureTag>> readTextures;
    uint32_t instanceCount = 0;
    uint32_t builtBottomLevels = 0;
    bool builtTopLevel = false;
    bool ready = false;
    /// @note True only for the submitted Hybrid candidate/culling policy; a manually assembled scene defaults to conservative queries.
    bool hybridCandidatePolicy = false;
};

/// @note ResourceManager::Rendering の共有状態に置く。異なる Manager / device へ持ち越さない。
/// @note 版の不一致は旧 TLAS へ復帰せず失敗する。静的 geometry は full key の等値で共有する。
class RayGeometryCache {
public:
    /// @pre フレーム内の DIRECT queue で、compute batch / 非同期区間の外から呼ぶ。
    /// @note 需要のあるビューだけが呼ぶ。新規 AS は共有準備 graph を実行してから公開する。
    /// @note TLAS inputs and each immutable table are compared independently; failed preparation never publishes prior handles as current.
    [[nodiscard]] RaySceneGpu Prepare(const RayScene& scene, RenderPassContext& context);
    void Release(ResourceManager& resources);
    void Trim(ResourceManager& resources, uint64_t frameStamp) { Prune(resources, frameStamp); }
    [[nodiscard]] uint32_t BottomLevelCount() const { return static_cast<uint32_t>(m_geometry.size()); }
private:
    struct GeometryEntry {
        RayGeometryKey key;
        ResourceHandle<AccelerationStructureTag> handle;
        uint64_t lastUse = 0;
    };
    struct SceneEntry {
        uint64_t sceneGeneration = 0;
        uint32_t layerMask = 0;
        std::vector<RayInstance> instances;
        std::vector<RayHitRecord> records;
        std::vector<RaySurfaceRecord> surfaces;
        RaySceneGpu gpu;
        uint64_t lastUse = 0;
    };
    void Prune(ResourceManager& resources, uint64_t frameStamp);
    std::vector<GeometryEntry> m_geometry;
    std::vector<SceneEntry> m_scenes;
    RenderPipeline m_preparation;
};

} /// @note namespace fbzz::renderer
