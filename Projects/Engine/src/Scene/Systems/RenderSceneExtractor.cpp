/// @file    RenderSceneExtractor.cpp
/// @brief   機能ごとの抽出を順に実行し、ビューの描画スナップショットを公開する。
/// @author  Hasegawa Jin
/// @date    2026-09-20
#include "RenderSceneExtraction.hpp"
#include "RenderSceneMaterialDemand.hpp"
#include "RenderSceneMaterialResolver.hpp"
#include "RenderPasses/Debug/SelectionPasses.hpp"
#include "RenderPasses/Geometry/GeometryPasses.hpp"

#include <Core/Profiler/ProfileScope.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Scene/Components/WaterfallComponent.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Systems/RenderCustomPostExtractor.hpp>
#include <Engine/Scene/Systems/RenderDecalExtractor.hpp>
#include <Engine/Scene/Systems/RenderEnvironmentExtractor.hpp>
#include <Engine/Scene/Systems/RenderFiberExtractor.hpp>
#include <Engine/Scene/Systems/RenderMeshTrailExtractor.hpp>
#include <Engine/Scene/Systems/RenderParticleExtractor.hpp>
#include <Engine/Scene/Systems/RenderTerrainExtractor.hpp>
#include <Engine/Scene/Systems/RenderTrailExtractor.hpp>
#include <Engine/Scene/Systems/RenderWaterExtractor.hpp>
#include <Graphics/Renderer/MaterialTransferScope.hpp>
#include <algorithm>
#include <utility>

namespace fbzz::scene {

namespace {

void ExtractObjectMasks(RenderPassContext& context, renderer::RenderScene& snapshot)
{
    constexpr uint32_t NO_OBJECT = UINT32_MAX;
    std::vector<uint32_t> firstObjectByEntity;
    std::vector<uint32_t> nextObjectWithSameEntity;
    const profiler::ProfileScope objectMasksProfile(
        profiler::ProfilerMarker("RenderSceneExtractor::ObjectMasks", "Rendering"));
    if (std::any_of(context.settings.objectMaskRequests.begin(), context.settings.objectMaskRequests.end(),
            [&snapshot](const auto& request) { return renderer::IsObjectMaskRequestLive(request, snapshot.frameStamp); })) {
        firstObjectByEntity.assign(context.scene.EntitySlotCount(), NO_OBJECT);
        nextObjectWithSameEntity.assign(snapshot.Objects().size(), NO_OBJECT);
        /// @note 同一 Entity に静的・スキンド両方がある場合も、元の描画物体順で全候補を保持する。
        for (size_t remainingObjects = snapshot.Objects().size(); remainingObjects > 0; --remainingObjects) {
            const uint32_t objectIndex = static_cast<uint32_t>(remainingObjects - 1);
            const auto entityIndex = snapshot.Objects()[objectIndex].sourceIndex;
            if (entityIndex >= firstObjectByEntity.size()) continue;
            nextObjectWithSameEntity[objectIndex] = firstObjectByEntity[entityIndex];
            firstObjectByEntity[entityIndex] = objectIndex;
        }
    }
    for (const auto& request : context.settings.objectMaskRequests) {
        if (!renderer::IsObjectMaskRequestLive(request, snapshot.frameStamp)) continue;
        ++snapshot.liveMaskRequests;
        auto* root = context.scene.GetGameObject({ request.id.index, request.id.generation });
        if (!root) continue;
        renderer::RenderMaskGroup group;
        group.payload = { request.color[0], request.color[1], request.color[2], std::clamp(request.value, 0.02f, 1.0f) };
        group.visibleOnly = request.visibleOnly;
        const auto collect = [&](const auto& self, GameObject& owner) -> void {
            if (!ShouldRenderGameObject(owner, context.cullingMask)) return;
            const EntityID entity = owner.GetID();
            if (entity.index >= firstObjectByEntity.size()) return;
            for (uint32_t objectIndex = firstObjectByEntity[entity.index]; objectIndex != NO_OBJECT;
                 objectIndex = nextObjectWithSameEntity[objectIndex]) {
                const auto& object = snapshot.Objects()[objectIndex];
                if (object.sourceIndex == entity.index && object.sourceGeneration == entity.generation)
                    group.objectIndices.push_back(objectIndex);
            }
            if (request.includeChildren) {
                for (int childIndex = 0; childIndex < owner.GetChildCount(); ++childIndex)
                    if (auto* child = owner.GetChild(childIndex)) self(self, *child);
            }
        };
        collect(collect, *root);
        snapshot.maskGroups.push_back(std::move(group));
    }
}

void ExtractSceneEffects(RenderPassContext& context, renderer::RenderScene& snapshot)
{
    {
        FBZZ_PROFILE_SCOPE("RenderSceneExtractor::CustomPost");
        ExtractRenderCustomPost(context, snapshot);
    }
    {
        FBZZ_PROFILE_SCOPE("RenderSceneExtractor::Trails");
        ExtractRenderTrails(context, snapshot);
    }
    {
        FBZZ_PROFILE_SCOPE("RenderSceneExtractor::Decals");
        ExtractRenderDecals(context, snapshot);
    }
    {
        FBZZ_PROFILE_SCOPE("RenderSceneExtractor::Terrains");
        ExtractRenderTerrains(context, snapshot);
    }
    {
        FBZZ_PROFILE_SCOPE("RenderSceneExtractor::Water");
        ExtractRenderWater(context, snapshot);
    }
    {
        FBZZ_PROFILE_SCOPE("RenderSceneExtractor::Fibers");
        ExtractRenderFibers(context, snapshot);
    }
    {
        FBZZ_PROFILE_SCOPE("RenderSceneExtractor::Particles");
        ExtractRenderParticles(context, snapshot);
    }
    {
        FBZZ_PROFILE_SCOPE("RenderSceneExtractor::MeshTrails");
        ExtractRenderMeshTrails(context, snapshot);
    }
    {
        FBZZ_PROFILE_SCOPE("RenderSceneExtractor::Environment");
        context.environment = ExtractRenderEnvironment(context);
        snapshot.environment = context.environment;
        snapshot.lighting = static_cast<const renderer::RenderLightingInput&>(context);
    }
}

}

using namespace render_extraction_detail;

void ExtractRenderScene(RenderPassContext& context, RenderFrameGeometryCache* frameGeometry)
{
    FBZZ_PROFILE_SCOPE("RenderSystem::ExtractRenderScene");
    material_demand_detail::ExtractionDepthScope extractionDepth;
    renderer::material_transfer_detail::MaterialTransferPayloadScope materialTransferPayloads(context.resources);
    /// @note 抽出開始時の資源フレームとリセット版を基準に、材質 callback 後の資源状態を照合する。
    const uint64_t resourceFrame = context.resources.FrameStamp();
    context.frameStamp = resourceFrame;
    context.time = Time::time;
    context.deltaTime = Time::deltaTime;
    context.unscaledDeltaTime = Time::unscaledDeltaTime;
    MaterialDemandTraceScope materialDemandTrace(context);
    const RenderSelectionLookup selection(context.settings.selectedObjects);
    RenderMaterialResolver materialResolver(&context.resources);

    const uint64_t resourceResetVersion = context.resources.GetResetVersion();
    const bool rayTracingInputs = CurrentRayMaterialInputs(context);
    /// @note 同一フレームの姿勢はビュー間で共有できる。ビュー固有の材質・選択・変形入力は別に持つ。
    auto [snapshot, skinnedObjects] =
        AcquireFrameGeometry(context, frameGeometry, rayTracingInputs, resourceResetVersion);
    /// @note このビューの変形結果を先に解決し、材質の需要判定が実際に描く形状を参照できるようにする。
    auto preparedItemIndices = RefreshViewMeshInputs(context, *snapshot, rayTracingInputs, skinnedObjects);
    auto [omittedItemIndices, selectedOrdinals] = ResolveViewMaterials(context, *snapshot, selection, rayTracingInputs,
        materialDemandTrace, materialResolver);
    ExtractObjectMasks(context, *snapshot);
    ExtractSceneEffects(context, *snapshot);
    /// @note 材質・環境の callback 後に、資源の状態が変わって省略した材質が必要になっていないか再確認する。
    RepairViewMaterialBindings(context, *snapshot, omittedItemIndices,
        resourceFrame, resourceResetVersion, materialDemandTrace);
    RefreshPreparedMeshOutputs(*snapshot, preparedItemIndices);

    if (!context.scene.GetEntities<WaterfallPart>().empty()) {
        const auto objects = snapshot->Objects();
        for (std::uint32_t objectIndex = 0; objectIndex < objects.size(); ++objectIndex)
            if (objects[objectIndex].waterfallFarIndexStart != 0) snapshot->waterfallObjects.push_back(objectIndex);
    }
    if (!omittedItemIndices.empty() || !preparedItemIndices.empty()) RetainSpatialUpdateForCompletion(*snapshot);
    RecordPreparedSnapshotItems(snapshot, std::move(preparedItemIndices));
    snapshot->selection.Publish(std::move(selectedOrdinals));
    /// @note 公開先は const の共有参照。以降の描画パスは同じ抽出結果を読む。
    context.renderScene = std::move(snapshot);
    material_demand_detail::RetainPendingBindings(context, context.renderScene, std::move(omittedItemIndices));
    materialDemandTrace.Complete(context);
}

}
