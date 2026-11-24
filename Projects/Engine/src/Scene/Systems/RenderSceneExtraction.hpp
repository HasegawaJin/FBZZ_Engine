/// @file    RenderSceneExtraction.hpp
/// @brief   ジオメトリの生成・共有とビュー別の描画入力をつなぐ内部境界。
/// @author  Hasegawa Jin
/// @date    2026-11-21
#pragma once

#include <Engine/Scene/Systems/RenderSceneExtractor.hpp>
#include <optional>
#include <span>

namespace fbzz::renderer { struct Mesh; }

namespace fbzz::scene {
class GameObject;
class RenderSelectionLookup;

namespace render_extraction_detail {
class MaterialDemandTraceScope;
class RenderMaterialResolver;

/// @note 静的メッシュが途中に混ざり得る、スキンド物体を包含する半開区間。
struct SkinnedObjectRange {
    std::size_t begin = 0;
    std::size_t end = 0;
};

struct GeometryBuildOptions {
    const renderer::ResourceManager* resources = nullptr;
    bool rayTracingInputs = false;
    bool extractItemMaterialState = true;
    bool fuseColorEligibility = false;
};

struct FrameGeometryView {
    std::shared_ptr<renderer::RenderScene> snapshot;
    std::optional<SkinnedObjectRange> skinnedObjects;
};

/// @note 既知の空集合は known が true で indices が null。未知のスナップショットは走査で補う。
struct PreparedSnapshotLookup {
    bool known = false;
    std::shared_ptr<const std::vector<uint32_t>> indices;
};

[[nodiscard]] uint64_t NextRenderSnapshotSerial();
void RetainSpatialUpdateForCompletion(renderer::RenderScene& snapshot);
[[nodiscard]] renderer::RenderScene BuildRenderSceneGeometry(Scene& scene, uint64_t frameStamp,
    renderer::ResourceHandle<renderer::ConstantBufferTag> identityPalette, const GeometryBuildOptions& options,
    uint64_t* staticGeometryRevision = nullptr, renderer::RenderScene* reusableStorage = nullptr,
    SkinnedObjectRange* skinnedObjects = nullptr);
[[nodiscard]] FrameGeometryView AcquireFrameGeometry(RenderPassContext& context, RenderFrameGeometryCache* frameCache,
    bool rayTracingInputs, uint64_t resourceResetVersion);
[[nodiscard]] renderer::RenderMeshItem ExtractMesh(const math::Matrix4& world, float maximumScale,
    const renderer::Mesh& mesh);
[[nodiscard]] renderer::RenderMeshItem ExtractMesh(GameObject& owner, const renderer::Mesh& mesh);
[[nodiscard]] std::vector<uint32_t> RefreshViewMeshInputs(RenderPassContext& context, renderer::RenderScene& snapshot,
    bool rayTracingInputs, const std::optional<SkinnedObjectRange>& skinnedObjects);
void RefreshPreparedMeshOutputs(renderer::RenderScene& snapshot, std::span<const uint32_t> preparedItemIndices);
void RecordPreparedSnapshotItems(const std::shared_ptr<const renderer::RenderScene>& snapshot,
    std::vector<uint32_t> preparedItemIndices);
[[nodiscard]] PreparedSnapshotLookup FindPreparedSnapshotItems(const std::shared_ptr<const renderer::RenderScene>& snapshot);
void RememberPreparedSnapshotItems(const std::shared_ptr<const renderer::RenderScene>& snapshot,
    std::shared_ptr<const std::vector<uint32_t>> indices);

void ExtractGeometryMaterialState(Scene& scene, renderer::RenderScene& snapshot, bool extractItemMaterialState);
/// @note 走査で得た添字は、材質コールバックを含む抽出の終了後にスナップショットとともに公開する。
struct ViewMaterialResult {
    std::vector<uint32_t> omittedItemIndices;
    std::vector<uint32_t> selectedOrdinals;
};
[[nodiscard]] ViewMaterialResult ResolveViewMaterials(RenderPassContext& context, renderer::RenderScene& snapshot,
    const RenderSelectionLookup& selection, bool rayTracingInputs, MaterialDemandTraceScope& materialDemandTrace,
    RenderMaterialResolver& materialResolver);
void ExtractRenderInstances(RenderPassContext& context, renderer::RenderScene& snapshot,
    const RenderSelectionLookup& selection, RenderMaterialResolver& materialResolver);
void RestoreMaterialBindings(RenderPassContext& context, renderer::RenderScene& snapshot,
    std::span<const uint32_t> omittedItemIndices, MaterialDemandTraceScope* trace = nullptr);
[[nodiscard]] bool LeafWindBoundsMayHaveExpanded(Scene& scene, const renderer::RenderObject& object);
void FailOpenChangedLeafWindBounds(Scene& scene, renderer::RenderScene& snapshot);

} /// @note namespace render_extraction_detail
} /// @note namespace fbzz::scene
