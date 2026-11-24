/// @file    RenderSceneInstances.cpp
/// @brief   配置セットと編集中のプレビューをインスタンス描画入力へ変換する。
/// @author  Hasegawa Jin
/// @date    2026-11-21
#include "RenderSceneExtraction.hpp"
#include "RenderExtractionPhases.hpp"
#include "RenderSceneMaterialBindings.hpp"
#include "RenderSceneMaterialResolver.hpp"
#include "RenderPasses/Debug/SelectionPasses.hpp"

#include <Core/Profiler/ProfileScope.hpp>
#include <Engine/Asset/InstanceSetAsset.hpp>
#include <Engine/Asset/MeshInstancesAsset.hpp>
#include <Engine/Scene/Components/InstancedMeshComponent.hpp>
#include <Engine/Scene/MeshResolver.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Systems/RenderSystem.hpp>
#include <Graphics/Renderer/ResourceManager.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace fbzz::scene::render_extraction_detail {

void ExtractRenderInstances(RenderPassContext& context, renderer::RenderScene& snapshot,
    const RenderSelectionLookup& selection, RenderMaterialResolver& materialResolver)
{
    FBZZ_PROFILE_SCOPE("RenderSceneExtractor::Instances");
    bool collectInstancePhases = false;
    if (profiler::Profiler::IsEnabled()) {
        const auto requestedFilter = profiler::Profiler::GetRequestedCaptureFilter();
        /// @note 明示的な診断フィルターのときだけ工程の時刻を取得し、サンプルは現フレームの適用済みフィルターで選別する。
        collectInstancePhases = requestedFilter.scope == "RenderSceneExtractor::Instances"
            && (requestedFilter.category.empty() || requestedFilter.category == "General");
    }
    int64_t setSnapshotNs = 0, meshAndBoundsNs = 0, materialNs = 0;
    int64_t definitionNs = 0, snapshotNs = 0, ownerNs = 0;
    static uint64_t instanceResourceOwner=0, instanceResourceResetVersion=UINT64_MAX;
    static std::unordered_map<std::string,MaterialComponent> instanceMaterials;
    if (instanceResourceOwner!=context.resources.GetInstanceId() || instanceResourceResetVersion!=context.resources.GetResetVersion()) {
        instanceMaterials.clear(); instanceResourceOwner=context.resources.GetInstanceId(); instanceResourceResetVersion=context.resources.GetResetVersion();
    }
    struct InstanceSourceView {
        const asset::InstanceSetAsset* definition;
        renderer::RenderInstanceInput input;
        GameObject* owner = nullptr;
    };
    std::vector<InstanceSourceView> sources;
    for (EntityID entity : context.scene.GetEntities<InstancedMeshComponent>()) {
        detail::RenderPhaseTimer<> setSnapshotTimer(setSnapshotNs, collectInstancePhases);
        detail::RenderPhaseTimer<> ownerTimer(ownerNs, collectInstancePhases);
        auto* owner = context.scene.GetGameObject(entity);
        const auto* component = context.scene.GetComponent<InstancedMeshComponent>(entity);
        if (!owner || !owner->activeInHierarchy() || !component || !component->m_enabled) continue;
        ownerTimer.Finish();
        detail::RenderPhaseTimer<> definitionTimer(definitionNs, collectInstancePhases);
        const auto* definition = asset::ResolveMeshInstanceDefinition(component->m_instanceSetRef.path);
        definitionTimer.Finish();
        if (!definition) continue;
        renderer::RenderInstanceInput input;
        detail::RenderPhaseTimer<> snapshotTimer(snapshotNs, collectInstancePhases);
        input.data = component->Snapshot(*definition);
        snapshotTimer.Finish();
        detail::RenderPhaseTimer<> ownerStateTimer(ownerNs, collectInstancePhases);
        input.identity = {context.scene.GetRenderSceneGeneration(), (static_cast<uint64_t>(entity.generation) << 32) | entity.index};
        input.world = owner->transform.GetWorldMatrix(); input.layer = static_cast<uint32_t>(owner->layer);
        input.selected = selection.Contains(owner->GetID()); input.castShadows = component->m_castShadows;
        input.maxDistance = component->m_maxDistance;
        const auto& budget = component->m_vegetationBudget;
        input.vegetationBudget = {budget.enabled,
            static_cast<uint32_t>(std::max(0, budget.drawDetailPerCell)),
            static_cast<uint32_t>(std::max(0, budget.nearDetailPerCell)),
            static_cast<uint32_t>(std::max(0, budget.shadowDetailPerCell))};
        sources.push_back({definition, std::move(input), owner});
    }
    if (context.uiOptions) for (const auto& preview : context.uiOptions->instancePreviews) {
        const auto* definition = asset::ResolveMeshInstanceDefinition(preview.definition);
        if (!definition || !preview.data || preview.data->placements.size() > 384) continue;
        renderer::RenderInstanceInput input;
        input.data = preview.data; input.world = preview.world;
        input.identity = {context.scene.GetRenderSceneGeneration() ^ (1ull << 63), preview.identity};
        input.selected = preview.erase || preview.highlight; input.selectionOnly = preview.erase; input.castShadows = false;
        sources.push_back({definition, std::move(input)});
    }
    for (auto& sourceView : sources) {
        detail::RenderPhaseTimer<> setSnapshotTimer(setSnapshotNs, collectInstancePhases);
        const auto& definition = *sourceView.definition;
        auto input = std::move(sourceView.input);
        if (!input.data || input.data->placements.empty()) continue;
        input.lodCount = static_cast<uint32_t>(definition.LodHeights().size());
        std::copy(definition.LodHeights().begin(), definition.LodHeights().end(), input.lodHeights.begin());
        math::Vector3 boundsCenter{}; float boundsRadius=-1, windBounds=0;
        setSnapshotTimer.Finish();
        for (const auto& sourcePart : definition.Parts()) {
            detail::RenderPhaseTimer<> meshTimer(meshAndBoundsNs, collectInstancePhases);
            auto* mesh=ResolveMeshPath(sourcePart.meshPath,context.resources);
            if (!mesh || mesh->isSkinned || mesh->indexCount==0) { input.parts.clear(); break; }
            renderer::RenderInstancePart part;
            part.mesh = sourceView.owner ? ExtractMesh(*sourceView.owner, *mesh)
                : ExtractMesh(input.world, ComputeWorldBoundsScale(input.world), *mesh);
            part.lod=sourcePart.lod;
            meshTimer.Finish();
            detail::RenderPhaseTimer<> materialTimer(materialNs, collectInstancePhases);
            auto& partMaterialComponent=instanceMaterials[sourcePart.materialPath];
            partMaterialComponent.SetMaterialPath(sourcePart.materialPath); partMaterialComponent.EnsureMaterialAsset();
            auto& material=part.mesh.material;
            materialResolver.ApplySlotState(partMaterialComponent, material);
            if (!materialResolver.CopySynchronized(partMaterialComponent,context.resources,false,0,material,true)) {
                const auto* preparedParams=materialResolver.Prepare(partMaterialComponent,0,context.resources,false);
                const auto* synchronizedMaterial=SyncMaterialSlotPrepared(partMaterialComponent,0,context.resources,false,0,preparedParams);
                CopyResolvedMaterial(material,synchronizedMaterial,context.resources,partMaterialComponent,materialResolver);
                materialResolver.RememberSynchronized(partMaterialComponent,context.resources,false,0,material,true);
            }
            part.mesh.forwardMaterial=material;
            float windStrength = 0; std::memcpy(&windStrength, material.gbufferParams.data() + 68, sizeof(windStrength));
            windBounds = std::max(windBounds, std::abs(windStrength) * 2.0f);
            materialTimer.Finish();
            if (!material.valid || material.capabilities.blend!=renderer::BlendMode::OPAQUE_BLEND) { input.parts.clear(); break; }
            detail::RenderPhaseTimer<> boundsTimer(meshAndBoundsNs, collectInstancePhases);
            const auto partBoundsCenter=mesh->boundsCenter; const float partBoundsRadius=mesh->boundsRadius;
            const auto centerOffset=partBoundsCenter-boundsCenter; const float centerDistance=centerOffset.Length();
            /// @note 包囲球を箱へ変換せずに合成し、過大な境界で LOD の簡略化が遅れるのを防ぐ。
            if (boundsRadius<0 || partBoundsRadius>=centerDistance+boundsRadius) { boundsCenter=partBoundsCenter; boundsRadius=partBoundsRadius; }
            else if (centerDistance+partBoundsRadius>boundsRadius) {
                const float mergedRadius=(boundsRadius+centerDistance+partBoundsRadius)*0.5f;
                if (centerDistance>0.00001f) boundsCenter+=centerOffset*((mergedRadius-boundsRadius)/centerDistance);
                boundsRadius=mergedRadius;
            }
            input.parts.push_back(std::move(part));
        }
        if (input.parts.empty()) continue;
        if (sourceView.owner) input.world = sourceView.owner->transform.GetWorldMatrix();
        input.localBoundsCenter=boundsCenter; input.localBoundsRadius=boundsRadius + windBounds;
        snapshot.instances.push_back(std::move(input));
    }
    if (collectInstancePhases) {
        /// @note Definition・Snapshot・Owner は SetSnapshot の内訳で重複し、上位スコープの自己時間は求められない。
        profiler::Profiler::PushSample({"RenderSceneExtractor::Instances::SetSnapshot", "General"}, static_cast<double>(setSnapshotNs) / 1000000.0);
        profiler::Profiler::PushSample({"RenderSceneExtractor::Instances::MeshAndBounds", "General"}, static_cast<double>(meshAndBoundsNs) / 1000000.0);
        profiler::Profiler::PushSample({"RenderSceneExtractor::Instances::Material", "General"}, static_cast<double>(materialNs) / 1000000.0);
        profiler::Profiler::PushSample({"RenderSceneExtractor::Instances::Definition", "General"}, static_cast<double>(definitionNs) / 1000000.0);
        profiler::Profiler::PushSample({"RenderSceneExtractor::Instances::Snapshot", "General"}, static_cast<double>(snapshotNs) / 1000000.0);
        profiler::Profiler::PushSample({"RenderSceneExtractor::Instances::Owner", "General"}, static_cast<double>(ownerNs) / 1000000.0);
    }
}

}
