/// @file    SelectionMaskPass.cpp
/// @brief   選択対象を画面空間のマスクに描く。
/// @author  Hasegawa Jin
/// @date    2026-06-18
#include "SelectionPasses.hpp"
#include <Engine/Scene/Systems/RenderPasses/Geometry/FiberRenderPass.hpp>
#include <Graphics/Passes/Geometry/InstanceRenderPasses.hpp>
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include "../Geometry/GeometryPasses.hpp"
#include <Graphics/Passes/Geometry/RenderSceneDraw.hpp>
#include <Engine/Scene/Systems/RenderPasses/Geometry/TerrainRenderPass.hpp>
#include <Engine/Scene/Systems/RenderPasses/Geometry/WaterRenderPass.hpp>
#include <Engine/Renderer/DrawCall.hpp>
#include <Graphics/Effects/ParticleBillboards.hpp>
#include <Graphics/Pipeline/RenderResources.hpp>
#include <Engine/Renderer/RenderScene.hpp>
#include <Engine/Scene/Components/ParticleEmitter.hpp>
#include <Engine/Scene/Components/ParticleGpuSimulation.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Math/Matrix4.hpp>
#include <Physics/Layer.hpp>
#include <algorithm>
#include <bit>
#include <cassert>
#include <vector>

namespace fbzz::scene {

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

namespace {

/// @brief 選択された ParticleEmitter の現在形状を、テクスチャ Alpha 込みで Selection Mask へ描く。
/// @note GameObject の Bounds 矩形では炎・煙の透明部分まで囲まれ、Unity 型のシルエット輪郭にならない。
void DrawParticleSelectionMask(GameObject& go, ParticleEmitter& emitter, RenderPassContext& ctx)
{
    auto& h = ctx.handles;
    if (!emitter.settings.enabled || emitter.runtime.particles.empty() || !emitter.runtime.texture.IsValid()
        || !emitter.runtime.renderCB.IsValid() || !h.selectionMaskParticleShader.IsValid()
        || !h.particleIB.IsValid()
        || !h.selectionMaskPSO.IsValid() || !emitter.settings.meshParticlePath.empty()) {
        return;
    }

    /// @note 選択マスクは抽出前の粒子を読むため、ローカル空間の粒子だけワールドへ変換する。
    const bool localSpace = emitter.settings.simulationSpace == ParticleSimulationSpace::Local;
    const auto& transform = go.transform;
    const renderer::ParticleBillboardTransform localToWorld{
        transform.worldPosition, transform.worldRotation, transform.worldScale,
    };
    std::vector<ParticleVertex> vertices;
    const int quadCount = renderer::BuildParticleBillboards(emitter.runtime.particles, emitter.settings.trail,
        emitter.settings.trail.trailColorTint, vertices, localSpace ? &localToWorld : nullptr);
    if (quadCount == 0) return;

    /// @note 同じフレームの別パスやエミッターには、同じプールから別々のバッファを貸し出す。
    const auto vertexBuffer = ctx.resources.Rendering().Shared().particleVertexPool.Acquire(
        ctx.resources, vertices.size(), static_cast<std::uint32_t>(sizeof(ParticleVertex)));
    if (!vertexBuffer.IsValid()) return;
    ctx.resources.Update(vertexBuffer, vertices.data(),
                         static_cast<std::uint32_t>(vertices.size() * sizeof(ParticleVertex)));
    renderer::DrawCall draw;
    draw.vertexBuffer = vertexBuffer;
    draw.indexBuffer = h.particleIB;
    draw.indexCount = static_cast<std::uint32_t>(quadCount * 6);
    draw.shader = h.selectionMaskParticleShader;
    draw.pipelineState = h.selectionMaskPSO;
    draw.constantBuffers[0] = h.frameCB;
    draw.constantBuffers[kParticleConstantSlot] = emitter.runtime.renderCB;
    draw.textures[0] = emitter.runtime.texture;
    ctx.renderer.Submit(draw, ctx.resources);
}

/// @note GPU 粒子は CPU 配列を持たないため StructuredBuffer を読む。
void DrawGpuParticleSelectionMask(ParticleEmitter& emitter, RenderPassContext& ctx)
{
    auto& h = ctx.handles;
    if (!CanUseGpuSimulation(emitter.settings, &emitter.runtime.material)
        || !emitter.runtime.gpuParticleBuffer.IsValid()
        || !emitter.runtime.texture.IsValid() || !emitter.runtime.renderCB.IsValid()
        || !h.selectionMaskParticleGpuShader.IsValid() || !h.selectionMaskPSO.IsValid()
        || !emitter.settings.meshParticlePath.empty()) {
        return;
    }

    const int maximumParticles = (std::max)(emitter.settings.maxParticles, 0);
    if (maximumParticles == 0) return;
    renderer::DrawCall draw;
    draw.shader = h.selectionMaskParticleGpuShader;
    draw.pipelineState = h.selectionMaskPSO;
    draw.vertexCount = static_cast<std::uint32_t>(maximumParticles) * 6u;
    draw.constantBuffers[0] = h.frameCB;
    draw.constantBuffers[kParticleConstantSlot] = emitter.runtime.renderCB;
    draw.textures[0] = emitter.runtime.texture;
    draw.vsBuffers[0] = emitter.runtime.gpuParticleBuffer;
    draw.vsBuffers[1] = emitter.runtime.gpuSortBuffer;
    ctx.renderer.Submit(draw, ctx.resources);
}

}

void ExecuteSelectionMaskPass(PassResources& res, RenderPassContext& ctx)
{
    if (!ctx.selectionOutlineEnabled) return;

    auto& r = ctx.renderer;
    auto& resources = ctx.resources;
    auto& h = ctx.handles;

    r.SetRenderTarget(res.Target("SelectionMask"), resources);
    r.Clear({ 0.0f, 0.0f, 0.0f, 0.0f });

    assert(ctx.renderScene && "RenderScene must be extracted after skinning and before drawing");
    const auto& input = *ctx.renderScene;
    const auto objects = input.Objects();
    const auto drawMesh = [&](const renderer::RenderObject& object) {
        if (!object.lodVisible || !fbzz::Layer::Contains(ctx.cullingMask, object.layer)) return;
        const auto shader = object.skinned ? h.selectionMaskSkinnedShader : h.selectionMaskShader;
        if (!shader.IsValid()) return;
        auto constants = renderer::ObjectConstants(object);
        constants.objectParams.x = 0.0f;
        resources.Update(h.objectCB, &constants, sizeof(constants));
        for (uint32_t i = object.firstItem; i < object.firstItem + object.itemCount; ++i) {
            const auto& item = input.items[i];
            if (!item.vertexBuffer.IsValid() || !item.indexBuffer.IsValid() || (object.skinned && !item.slotVisible)) continue;
            renderer::DrawCall dc;
            dc.vertexBuffer = object.skinned ? item.skinningVertexBuffer : item.vertexBuffer;
            dc.indexBuffer = item.indexBuffer;
            dc.indexCount = item.indexCount;
            dc.vertexCount = item.vertexCount;
            dc.shader = shader;
            dc.pipelineState = h.selectionMaskPSO;
            dc.constantBuffers[0] = h.frameCB;
            dc.constantBuffers[1] = h.objectCB;
            if (object.skinned) {
                if (object.gpuBones.current.IsValid()) dc.vsBuffers[0] = object.gpuBones.current;
                else dc.constantBuffers[7] = object.skinningPalette;
            }
            r.Submit(dc, resources);
        }
    };
    if (const auto selection = input.PublishedMeshSelection()) {
        for (uint32_t ordinal : *selection)
            if (ordinal < objects.size()) drawMesh(objects[ordinal]);
    } else {
        for (size_t ordinal = 0; ordinal < objects.size(); ++ordinal)
            if (input.IsObjectSelected(ordinal)) drawMesh(objects[ordinal]);
    }

    const auto drawSelectedObject = [&](GameObject& go) {
        if (!go.activeInHierarchy() || !fbzz::Layer::Contains(ctx.cullingMask, go.layer)) return;

        PerObjectCB objData{};
        objData.world = go.transform.GetWorldMatrix();
        objData.worldInvTranspose = math::Matrix4::InverseTransposeAffine(objData.world);
        resources.Update(h.objectCB, &objData, sizeof(PerObjectCB));

        if (auto* particle = go.GetComponent<ParticleEmitter>()) {
            DrawParticleSelectionMask(go, *particle, ctx);
            DrawGpuParticleSelectionMask(*particle, ctx);
        }
    };
    const auto& selected = ctx.settings.selectedObjects;
    if (!selected.empty()) {
        /// @note 選択 ID の整列に要する比較数の目安がシーン走査を上回る場合は、走査と選択集合の照合を使う。
        const auto sortDepth = std::bit_width(selected.size());
        if (selected.size() > ctx.scene.GameObjectCount() / sortDepth) {
            const RenderSelectionLookup selection(selected);
            for (auto& go : ctx.scene.GameObjects())
                if (selection.Contains(go.GetID())) drawSelectedObject(go);
        } else {
            std::vector<EntityID> selectedIds;
            selectedIds.reserve(selected.size());
            for (const auto id : selected) selectedIds.push_back({id.index, id.generation});
            /// @note 透過シルエットの提出順と同一 ID の一回描画を従来の Scene 順に揃える。
            ctx.scene.SortEntitiesBySceneOrder(selectedIds);
            const auto end = std::unique(selectedIds.begin(), selectedIds.end());
            for (auto id = selectedIds.begin(); id != end; ++id)
                if (auto* go = ctx.scene.GetGameObject(*id)) drawSelectedObject(*go);
        }
    }

    renderer::ExecuteInstanceSelection(ctx);
    ExecuteFiberSelectionMask(ctx);
    TerrainSelectionMaskSystem(ctx);
    WaterSelectionMaskSystem(ctx);
    if (ctx.appendUISelectionMask) ctx.appendUISelectionMask();

    r.SetRenderTarget(ctx.Res().Target("HDR"), resources);
}


void SelectionMaskPass::Setup(PassBuilder& builder, const RenderPassContext& ctx) const
{
    builder.Read("HDR").Write("SelectionMask");
    if (ctx.waterMediumPassActive && ctx.resourceRegistry.Target("WaterMediumAtlas").IsValid())
        builder.Read("WaterMediumAtlas");
}

bool SelectionMaskPass::IsEnabled(const RenderPassContext& ctx) const
{
    return ctx.selectionOutlineEnabled;
}

void SelectionMaskPass::Execute(PassResources& resources, RenderPassContext& ctx)
{
    ExecuteSelectionMaskPass(resources, ctx);
}
}
