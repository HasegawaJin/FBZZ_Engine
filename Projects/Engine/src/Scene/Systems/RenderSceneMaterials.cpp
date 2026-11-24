/// @file    RenderSceneMaterials.cpp
/// @brief   ビューの需要に応じて物体の材質を同期し、省略したバインディングを復元する。
/// @author  Hasegawa Jin
/// @date    2026-11-21
#include "RenderSceneExtraction.hpp"
#include "RenderExtractionPhases.hpp"
#include "RenderSceneMaterialBindings.hpp"
#include "RenderSceneMaterialDemand.hpp"
#include "RenderSceneMaterialResolver.hpp"
#include "RenderPasses/Debug/SelectionPasses.hpp"

#include <Core/Profiler/ProfileScope.hpp>
#include <Engine/Renderer/IBuffer.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Systems/RenderPasses/GeometryRoute.hpp>
#include <Engine/Scene/Systems/RenderSystem.hpp>
#include <Graphics/Renderer/BuiltinShader.hpp>
#include <Graphics/Renderer/ResourceManager.hpp>

namespace fbzz::scene {

namespace render_extraction_detail {

namespace {

struct MaterialPhaseTimes {
    int64_t stateSlotsNs = 0;
    int64_t projectionReuseNs = 0;
    int64_t parameterPreparationNs = 0;
    int64_t syncCopyTexturesNs = 0;
    int64_t rayInputsNs = 0;
};

MaterialSlot& LoadMaterialSlot(MaterialComponent& component, renderer::RenderMeshItem& item,
    RenderMaterialResolver& resolver)
{
    component.EnsureMaterialAsset(item.materialSlot);
    auto& slot = component.SlotAt(item.materialSlot);
    item.slotVisible = slot.visible;
    item.visible = component.enabled && slot.visible;
    resolver.ApplySlotState(slot, item.material);
    return slot;
}

/// @note 復元時は形状を抽出した後の更新も取り込む。
bool RefreshRayGeometryVersions(renderer::RenderMeshItem& item, renderer::ResourceManager& resources, bool skinned)
{
    const auto* vertices = resources.Get(item.vertexBuffer);
    const auto* indices = resources.Get(item.indexBuffer);
    const auto* deformed = resources.Get(item.deformedVertexBuffer);
    item.vertexContentVersion = vertices ? vertices->GetContentVersion() : 0;
    item.indexContentVersion = indices ? indices->GetContentVersion() : 0;
    item.deformedContentVersion = deformed ? deformed->GetContentVersion() : 0;
    if (vertices) item.vertexStride = vertices->GetStride();
    return skinned && deformed && item.deformedContentVersion != 0
        && deformed->GetStride() == sizeof(renderer::Vertex);
}

bool VerifyRayDeformation(const renderer::RenderMeshItem& item, renderer::ResourceManager& resources, bool skinned)
{
    const auto* deformed = skinned && item.deformedContentVersion != 0
        ? resources.Get(item.deformedVertexBuffer) : nullptr;
    return deformed && deformed->GetStride() == sizeof(renderer::Vertex)
        && deformed->GetContentVersion() == item.deformedContentVersion;
}

template<bool DIAGNOSTICS_ENABLED, bool DEMAND_TRACE_ENABLED, bool RESTORING_BINDINGS = false>
void SynchronizeItemMaterial(RenderPassContext& context, const renderer::RenderObject& object,
    renderer::RenderMeshItem& item, MaterialComponent& component, const MaterialSlot& slot,
    RenderMaterialResolver& materialResolver, bool rayTracingInputs, MaterialPhaseTimes& phases,
    MaterialDemandTraceScope* trace)
{
    auto& material = item.material;
    detail::RenderPhaseTimer<DIAGNOSTICS_ENABLED> projectionReuseTimer(phases.projectionReuseNs);
    /// @note レイは画面外にも到達するため、カメラへの投影サイズを要求しない。
    const bool projectedTextureRequest = !rayTracingInputs
        && renderer::ResolveGeometryRoute(material.capabilities, true) == renderer::GeometryRoute::GBuffer;
    /// @note スキンドメッシュはアニメーション姿勢を含む物体全体の境界を使う。
    const float screenPixels = projectedTextureRequest
        ? EstimateRenderTexturePixels(object, item, context.camera.m_position,
              context.cullProjScaleY, context.height, context.cullOrthographic)
        : 0.0f;
    if constexpr (DEMAND_TRACE_ENABLED) {
        if (projectedTextureRequest) ++trace->Counters().projectedTextureRequestItems;
        if (screenPixels > 0.0f) ++trace->Counters().positiveScreenPixelRequests;
    }
    /// @note レイ用の表面は物体ごとの変形とテクスチャ世代を使うため、スロットごとに同期する。
    if (!rayTracingInputs && materialResolver.CopySynchronized(slot, context.resources, object.skinned, screenPixels, material)) {
        item.forwardMaterial = material;
        if constexpr (DEMAND_TRACE_ENABLED) {
            ++trace->Counters().synchronizedReuseItems;
            if (material.valid) trace->Counters().canonicalCopiedLogicalBytes += material.gbufferParams.size();
        }
        return;
    }
    projectionReuseTimer.Finish();
    detail::RenderPhaseTimer<DIAGNOSTICS_ENABLED> preparationTimer(phases.parameterPreparationNs);
    const auto* preparedParams = materialResolver.Prepare(component, item.materialSlot, context.resources, object.skinned);
    preparationTimer.Finish();
    detail::RenderPhaseTimer<DIAGNOSTICS_ENABLED> syncCopyTimer(phases.syncCopyTexturesNs);
    if constexpr (DEMAND_TRACE_ENABLED && !RESTORING_BINDINGS) ++trace->Counters().syncAttempts;
    const auto* synchronizedMaterial = SyncMaterialSlotPrepared(component, item.materialSlot, context.resources,
        object.skinned, screenPixels, preparedParams);
    if constexpr (DEMAND_TRACE_ENABLED) {
        if constexpr (RESTORING_BINDINGS) ++trace->Counters().syncAttempts;
        if (synchronizedMaterial) {
            auto& counters = trace->Counters();
            ++counters.syncNonNullItems;
            counters.syncReturnedParameterBytes += synchronizedMaterial->paramData.size();
            if (renderer::IsErrorShader(synchronizedMaterial->shaderPath)) ++counters.errorFallbackItems;
        }
    }
    /// @note 同期中にコールバックが走るため、以後はスロットの参照を取り直す。
    CopyResolvedMaterial(material, synchronizedMaterial, context.resources, component.SlotAt(item.materialSlot), materialResolver);
    if constexpr (DIAGNOSTICS_ENABLED) {
        if (rayTracingInputs) syncCopyTimer.Finish();
    }
    detail::RenderPhaseTimer<DIAGNOSTICS_ENABLED> rayInputsTimer(phases.rayInputsNs, rayTracingInputs);
    if (rayTracingInputs) {
        bool verified;
        if constexpr (RESTORING_BINDINGS)
            verified = RefreshRayGeometryVersions(item, context.resources, object.skinned);
        else
            verified = VerifyRayDeformation(item, context.resources, object.skinned);
        ExtractRayMaterialInputs(material, synchronizedMaterial, component.SlotAt(item.materialSlot), context.resources, verified);
    } else {
        materialResolver.RememberSynchronized(component.SlotAt(item.materialSlot), context.resources, object.skinned, screenPixels, material);
    }
    item.forwardMaterial = material;
    if constexpr (DEMAND_TRACE_ENABLED) {
        if (material.valid) trace->Counters().canonicalCopiedLogicalBytes += material.gbufferParams.size();
    }
}

}

void ExtractGeometryMaterialState(Scene& scene, renderer::RenderScene& snapshot, bool extractItemMaterialState)
{
    std::optional<RenderMaterialResolver> materialResolver;
    if (extractItemMaterialState) materialResolver.emplace();
    for (auto& object : snapshot.geometry.EditObjects()) {
        auto* owner = scene.GetGameObject({ object.sourceIndex, object.sourceGeneration });
        const auto* materialComponent = owner ? owner->GetComponent<MaterialComponent>() : nullptr;
        if (!materialComponent) {
            object.colorEligible = true;
            continue;
        }
        object.colorEligible = materialComponent->enabled;
        /// @note 全抽出では EnsureMaterialAsset 後の値を使うため、ここでスロットを読むのは形状単独抽出に限る。
        if (!materialResolver) continue;
        for (uint32_t itemIndex = object.firstItem; itemIndex < object.firstItem + object.itemCount; ++itemIndex) {
            auto& item = snapshot.items[itemIndex];
            const auto& slot = materialComponent->SlotAt(item.materialSlot);
            item.slotVisible = slot.visible;
            item.visible = materialComponent->enabled && slot.visible;
            item.material.capabilities = materialResolver->ReadSlotCapabilities(slot);
        }
    }
}

ViewMaterialResult ResolveViewMaterials(RenderPassContext& context, renderer::RenderScene& snapshot,
    const RenderSelectionLookup& selection, bool rayTracingInputs, MaterialDemandTraceScope& materialDemandTrace,
    RenderMaterialResolver& materialResolver)
{
    const auto materialsProfile = profiler::Profiler::BeginScope(
        profiler::ProfilerMarker("RenderSceneExtractor::Materials", "Rendering"));
    bool collectMaterialPhases = false;
    if (profiler::Profiler::IsEnabled()) {
        const auto requestedFilter = profiler::Profiler::GetRequestedCaptureFilter();
        /// @note 要求スコープが完全一致したときだけ計測し、外部サンプルは適用済みのフレームフィルターに従う。
        collectMaterialPhases = requestedFilter.scope == "RenderSceneExtractor::Materials"
            && (requestedFilter.category.empty() || requestedFilter.category == "Rendering");
    }
    MaterialPhaseTimes phases;
    if (materialDemandTrace.Active()) {
        auto& counters = materialDemandTrace.Counters();
        counters.snapshotSerial = snapshot.snapshotSerial;
        counters.renderObjects = snapshot.Objects().size();
        counters.renderItems = snapshot.items.size();
    }
    auto initialDemandView = CurrentMaterialDemandView(context, rayTracingInputs);
    std::optional<math::Frustum> initialDemandFrustum;
    if (initialDemandView.culling.frustum) {
        initialDemandFrustum = *initialDemandView.culling.frustum;
        initialDemandView.culling.frustum = &*initialDemandFrustum;
    }
    /// @note 抽出開始時のビューだけを共有し、終了時と消費直前の修復判定ではそれぞれ最新ビューを評価する。
    const material_demand_detail::PreparedView preparedDemandView(initialDemandView);
    ViewMaterialResult result;
    const auto processMaterials = [&]<bool DIAGNOSTICS_ENABLED, bool DEMAND_TRACE_ENABLED>() {
        detail::RenderPhaseTimer<DIAGNOSTICS_ENABLED> setupTimer(phases.stateSlotsNs);
        const bool hasSelection = !context.settings.selectedObjects.empty();
        const auto objects = snapshot.Objects();
        auto& flags = snapshot.selection.EditFlags();
        if (hasSelection) flags.reserve(objects.size());
        setupTimer.Finish();
        for (uint32_t ordinal = 0; ordinal < objects.size(); ++ordinal) {
            const auto& object = objects[ordinal];
            material_demand_detail::ObjectEvaluator demandEvaluator(preparedDemandView, object);
            detail::RenderPhaseTimer<DIAGNOSTICS_ENABLED> objectStateTimer(phases.stateSlotsNs);
            auto* owner = context.scene.GetGameObject({ object.sourceIndex, object.sourceGeneration });
            const bool selected = hasSelection ? owner && selection.Contains(owner->GetID()) : object.selected;
            if (hasSelection) flags.push_back(selected);
            if (selected) result.selectedOrdinals.push_back(ordinal);
            auto* materialComponent = owner ? owner->GetComponent<MaterialComponent>() : nullptr;
            /// @note Component 自体が無い場合も未割当として可視化し、シーンへ自動追加しない。
            std::optional<MaterialComponent> unassigned;
            if (!materialComponent) materialComponent = &unassigned.emplace();
            /// @note Material の解決失敗はスロット単位で代替し、他の submesh を巻き添えにしない。
            objectStateTimer.Finish();
            for (uint32_t itemIndex = object.firstItem; itemIndex < object.firstItem + object.itemCount; ++itemIndex) {
                detail::RenderPhaseTimer<DIAGNOSTICS_ENABLED> slotStateTimer(phases.stateSlotsNs);
                auto& item = snapshot.items[itemIndex];
                const auto& slot = LoadMaterialSlot(*materialComponent, item, materialResolver);
                slotStateTimer.Finish();
                if constexpr (DEMAND_TRACE_ENABLED) ++materialDemandTrace.Counters().consideredItems;
                if (!object.colorEligible || !item.visible) continue;
                const auto demand = demandEvaluator.Evaluate(item);
                if constexpr (DEMAND_TRACE_ENABLED) {
                    auto& counters = materialDemandTrace.Counters();
                    ++counters.syncEligibleItems;
                    counters.eligibleDeclaredVertices += item.vertexCount;
                    counters.eligibleDeclaredIndices += item.indexCount;
                    materialDemandTrace.CountPotential(initialDemandView, item, demand);
                }
                /// @note 形状と設定を保持し、カリング判定を通していない未解決の形状では従来どおり材質を同期する。
                if (demand.reasons == 0 && (demand.cameraEvaluated || !demand.matchesView)) {
                    result.omittedItemIndices.push_back(itemIndex);
                    if constexpr (DEMAND_TRACE_ENABLED) ++materialDemandTrace.Counters().omittedBindingItems;
                    continue;
                }
                SynchronizeItemMaterial<DIAGNOSTICS_ENABLED, DEMAND_TRACE_ENABLED>(context, object, item,
                    *materialComponent, slot, materialResolver, rayTracingInputs, phases, &materialDemandTrace);
            }
        }
    };
    if (collectMaterialPhases) {
        if (materialDemandTrace.Active()) processMaterials.template operator()<true, true>();
        else processMaterials.template operator()<true, false>();
    } else {
        if (materialDemandTrace.Active()) processMaterials.template operator()<false, true>();
        else processMaterials.template operator()<false, false>();
    }
    if (collectMaterialPhases) {
        /// @note 5 工程の計測値は計時・集計コストを含み、Materials とその上位の自己時間は求められない。
        profiler::Profiler::PushSample({"RenderSceneExtractor::Materials::StateSlots","Rendering"},static_cast<double>(phases.stateSlotsNs)/1000000.0);
        profiler::Profiler::PushSample({"RenderSceneExtractor::Materials::ProjectionReuse","Rendering"},static_cast<double>(phases.projectionReuseNs)/1000000.0);
        profiler::Profiler::PushSample({"RenderSceneExtractor::Materials::ParameterPreparation","Rendering"},static_cast<double>(phases.parameterPreparationNs)/1000000.0);
        profiler::Profiler::PushSample({"RenderSceneExtractor::Materials::SyncCopyTextures","Rendering"},static_cast<double>(phases.syncCopyTexturesNs)/1000000.0);
        profiler::Profiler::PushSample({"RenderSceneExtractor::Materials::RayInputs","Rendering"},static_cast<double>(phases.rayInputsNs)/1000000.0);
    }
    profiler::Profiler::EndScope(materialsProfile);

    ExtractRenderInstances(context, snapshot, selection, materialResolver);
    return result;
}

/// @note 省略した項目を需要判定なしで一度ずつ復元し、同期中のコールバックで再試行しない。
void RestoreMaterialBindings(RenderPassContext& context, renderer::RenderScene& snapshot, std::span<const uint32_t> omittedItemIndices,
                             MaterialDemandTraceScope* trace)
{
    RenderMaterialResolver materialResolver(&context.resources);
    const bool rayInputs = CurrentRayMaterialInputs(context);
    if (trace) {
        auto& counters = trace->Counters();
        ++counters.bindingFallbacks;
        counters.restoredBindingItems += omittedItemIndices.size();
        counters.omittedBindingItems -= omittedItemIndices.size();
        counters.provenNoDemandItems -= omittedItemIndices.size();
        counters.conservativeUnionItems += omittedItemIndices.size();
        counters.unknownConsumerItems += omittedItemIndices.size();
        counters.consumerScopeCertified = 0;
    }
    MaterialPhaseTimes phases;
    const auto restoreBindings = [&]<bool DEMAND_TRACE_ENABLED>() {
        for (uint32_t itemIndex : omittedItemIndices) {
            if (itemIndex >= snapshot.items.size()) continue;
            auto& item = snapshot.items[itemIndex];
            if (item.objectIndex >= snapshot.Objects().size()) continue;
            const auto& object = snapshot.Objects()[item.objectIndex];
            auto* owner = context.scene.GetGameObject({object.sourceIndex, object.sourceGeneration});
            auto* materialComponent = owner ? owner->GetComponent<MaterialComponent>() : nullptr;
            std::optional<MaterialComponent> unassigned;
            if (!materialComponent) materialComponent = &unassigned.emplace();
            const auto& slot = LoadMaterialSlot(*materialComponent, item, materialResolver);
            if (!object.colorEligible || !item.visible) continue;
            SynchronizeItemMaterial<false, DEMAND_TRACE_ENABLED, true>(context, object, item,
                *materialComponent, slot, materialResolver, rayInputs, phases, trace);
        }
    };
    if (trace) restoreBindings.template operator()<true>();
    else restoreBindings.template operator()<false>();
}

}

float EstimateRenderTexturePixels(const renderer::RenderObject& object,
    const renderer::RenderMeshItem& item, const math::Vector3& cameraPosition,
    float projectionScaleY, uint32_t viewportHeight, bool orthographic)
{
    const auto boundsCenter = object.skinned ? object.boundsCenter : item.boundsCenter;
    const float boundsRadius = object.skinned ? object.boundsRadius : item.boundsRadius;
    if (!(projectionScaleY > 0.0f) || !(boundsRadius > 0.0f) || viewportHeight == 0) return 0.0f;
    const float cameraDistance = (boundsCenter - cameraPosition).Length();
    if (!orthographic && cameraDistance <= boundsRadius) return static_cast<float>(viewportHeight);
    return boundsRadius * projectionScaleY * static_cast<float>(viewportHeight) / (orthographic ? 1.0f : cameraDistance);
}

}
