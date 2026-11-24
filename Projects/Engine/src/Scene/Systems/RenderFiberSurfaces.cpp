/// @file    RenderFiberSurfaces.cpp
/// @brief   静的・スキン付きメッシュと地形から繊維の下地を収集する。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#include "RenderFiberExtraction.hpp"
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Components/FiberComponent.hpp>
#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Core/Time.hpp>
#include <utility>

namespace fbzz::scene::fiber_extraction_detail {

bool HasFiberSurface(GameObject& owner, fbzz::LayerMask mask)
{
    if (!ShouldRenderGameObject(owner, mask)) return false;
    const auto* fiber = owner.GetComponent<FiberComponent>();
    if (!fiber || !fiber->m_enabled || fiber->m_materialPath.empty()) return false;
    if (const auto* mesh=owner.GetComponent<MeshRenderer>(); mesh && mesh->enabled && mesh->lodVisible && mesh->mesh) return true;
    if (const auto* skin=owner.GetComponent<SkinnedMeshRenderer>(); skin && skin->enabled && skin->lodVisible && skin->model) return true;
    const auto* terrain=owner.GetComponent<TerrainComponent>();
    return terrain && terrain->enabled;
}

void CollectFiberSurfaces(GameObject& owner, const FiberComponent& fiber, float fiberPadding,
    FiberResources& cache, RenderPassContext& context, std::vector<FiberSurface>& surfaces)
{
    auto& resources = context.resources;
    surfaces.clear();
    if (auto* meshRenderer=owner.GetComponent<MeshRenderer>(); meshRenderer && meshRenderer->enabled && meshRenderer->lodVisible && meshRenderer->mesh && !meshRenderer->mesh->isSkinned)
        surfaces.push_back({meshRenderer->mesh,meshRenderer->mesh->vertexBuffer,{}, {},meshRenderer->castShadows,true,meshRenderer->lodDither});
    if (auto* skinnedRenderer=owner.GetComponent<SkinnedMeshRenderer>(); skinnedRenderer && skinnedRenderer->enabled && skinnedRenderer->lodVisible && skinnedRenderer->model) {
        auto* animator=FindAnimator(owner);
        const auto skin=ResolveSkinningCB(animator ? animator->skinningBuffer.Get() : renderer::ResourceHandle<renderer::ConstantBufferTag>{},
            skinnedRenderer->model,context.handles.bindPoseSkinningCB);
        const bool hasPreviousSkin=animator && animator->prevBoneMatricesValid && animator->prevSkinningBuffer.IsValid();
        const auto previousSkin=hasPreviousSkin ? animator->prevSkinningBuffer.Get() : skin;
        const auto* baseMaterial=owner.GetComponent<MaterialComponent>();
        /// @note 骨の今の広がりから作る球 (Animator が焼く)。以前はスキンをカリングせず、画面外でも全カスケードへ全層を描いていた。
        WorldBounds skinBounds{};
        const bool hasSkinBounds=ComputeSkinnedWorldBounds(owner,*skinnedRenderer,skinBounds,fiberPadding);
        for (size_t submeshSlot=0;submeshSlot<skinnedRenderer->SubmeshCount();++submeshSlot) {
            auto* mesh=skinnedRenderer->SubmeshMesh(submeshSlot);
            if (!mesh || (baseMaterial && !baseMaterial->SlotAt(submeshSlot).visible)) continue;
            FiberSurface surface{mesh,skinnedRenderer->ResolveSlotVertexBuffer(submeshSlot,mesh->vertexBuffer),skin,previousSkin,skinnedRenderer->castShadows,hasPreviousSkin,skinnedRenderer->lodDither};
            surface.skinnedVertices=skinnedRenderer->ResolveSlotSkinnedVertexBuffer(submeshSlot);
            const uint32_t submesh = skinnedRenderer->SubmeshAt(submeshSlot);
            if (skinnedRenderer->preparedSkinning && skinnedRenderer->preparedSkinningFrame == Time::frameCount
                && submesh < skinnedRenderer->preparedSkinningRows.size()) {
                surface.preparedSkinning = skinnedRenderer->preparedSkinning;
                surface.preparedSkinningWrite = skinnedRenderer->preparedSkinningRows[submesh];
            }
            surface.hasSkinBounds=hasSkinBounds;
            surface.skinBounds=skinBounds;
            surfaces.push_back(surface);
        }
    }
    if (auto* terrain=owner.GetComponent<TerrainComponent>(); terrain && terrain->enabled) {
        auto& patches=cache.m_terrains[{context.scene.GetRenderSceneGeneration(),owner.instanceId}];
        UpdateFiberTerrain(patches,*terrain,fiber.m_terrainPatchCells,fiber.m_terrainLayer,fiber.m_terrainLayerThreshold,
            owner.transform,context.camera.m_position,fiber.m_distanceLod,fiber.m_lodNear,fiber.m_lodFar,resources);
        const bool useVertexAlphaDensity=fiber.m_terrainLayer>=0;
        for (auto* mesh:patches.m_visible) {
            FiberSurface surface{mesh,mesh->vertexBuffer,{},{},true,true,0,useVertexAlphaDensity};
            surface.terrain = true;
            surfaces.push_back(std::move(surface));
        }
    }
}

} /// @note namespace fbzz::scene::fiber_extraction_detail
