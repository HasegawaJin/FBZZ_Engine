// FBZZ Engine
// SelectionMaskPass.cpp | fbzz::scene
// Selection mask render pass implementation
#include "SelectionPasses.hpp"
#include <Engine/Scene/Systems/RenderPassContext.hpp>
#include <Engine/Renderer/DrawCall.hpp>
#include <Engine/Renderer/Mesh.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Math/Matrix4.hpp>
#include <Physics/Layer.hpp>

namespace fbzz::scene {

namespace {

bool IsSelectedForOutline(const GameObject& go, const renderer::RenderSettings& settings)
{
    const EntityID id = go.GetID();
    if (!id.IsValid()) return false;

    for (const renderer::RenderSelectionID& selected : settings.selectedObjects) {
        if (selected.index == id.index && selected.generation == id.generation) {
            return true;
        }
    }
    return false;
}

} // namespace

void ExecuteSelectionMaskPass(RenderPassContext& ctx)
{
    if (!ctx.selectionOutlineEnabled) return;

    auto& r = ctx.renderer;
    auto& resources = ctx.resources;
    auto& h = ctx.handles;

    r.SetRenderTarget(h.selectionMaskRT, resources);
    r.Clear({ 0.0f, 0.0f, 0.0f, 0.0f });

    for (auto& go : ctx.scene.GameObjects()) {
        if (!go.activeInHierarchy()) continue;
        if (!fbzz::Layer::Contains(ctx.cullingMask, go.layer)) continue;
        if (!IsSelectedForOutline(go, ctx.settings)) continue;

        PerObjectCB objData{};
        objData.world = go.transform.GetWorldMatrix();
        objData.worldInvTranspose = math::Matrix4::Transpose(math::Matrix4::Inverse(objData.world));
        resources.Update(h.objectCB, &objData, sizeof(PerObjectCB));

        if (h.selectionMaskShader.IsValid()) {
            auto* mr = go.GetComponent<MeshRenderer>();
            if (mr && mr->enabled && mr->mesh && !mr->mesh->isSkinned &&
                mr->mesh->vertexBuffer.IsValid() && mr->mesh->indexBuffer.IsValid())
            {
                renderer::DrawCall dc;
                dc.vertexBuffer = mr->mesh->vertexBuffer;
                dc.indexBuffer = mr->mesh->indexBuffer;
                dc.indexCount = mr->mesh->indexCount;
                dc.vertexCount = mr->mesh->vertexCount;
                dc.shader = h.selectionMaskShader;
                dc.pipelineState = h.selectionMaskPSO;
                dc.constantBuffers[0] = h.frameCB;
                dc.constantBuffers[1] = h.objectCB;
                r.Submit(dc, resources);
            }
        }

        if (h.selectionMaskSkinnedShader.IsValid()) {
            auto* smr = go.GetComponent<SkinnedMeshRenderer>();
            auto* anim = go.GetComponent<AnimatorComponent>();
            if (smr && smr->enabled && smr->model) {
                const auto skinCB = (anim && anim->skinningBuffer.IsValid())
                    ? anim->skinningBuffer : h.bindPoseSkinningCB;

                for (const auto& meshPtr : smr->model->meshes) {
                    if (!meshPtr) continue;
                    if (!meshPtr->vertexBuffer.IsValid() || !meshPtr->indexBuffer.IsValid()) continue;

                    renderer::DrawCall dc;
                    dc.vertexBuffer = meshPtr->vertexBuffer;
                    dc.indexBuffer = meshPtr->indexBuffer;
                    dc.indexCount = meshPtr->indexCount;
                    dc.vertexCount = meshPtr->vertexCount;
                    dc.shader = h.selectionMaskSkinnedShader;
                    dc.pipelineState = h.selectionMaskPSO;
                    dc.constantBuffers[0] = h.frameCB;
                    dc.constantBuffers[1] = h.objectCB;
                    dc.constantBuffers[7] = skinCB;
                    r.Submit(dc, resources);
                }
            }
        }
    }

    r.SetRenderTarget(h.hdrRT, resources);
}

} // namespace fbzz::scene
