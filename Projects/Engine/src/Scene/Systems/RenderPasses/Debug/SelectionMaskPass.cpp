// FBZZ Engine
// SelectionMaskPass.cpp | fbzz::scene
// Selection mask render pass implementation
#include "SelectionPasses.hpp"
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include "../Geometry/GeometryPasses.hpp"  // FindAnimator (親方向探索) を共用する
#include <Engine/Scene/Systems/RenderPasses/Geometry/TerrainRenderPass.hpp>
#include <Engine/Scene/Systems/RenderPasses/Geometry/WaterRenderPass.hpp>
#include <Engine/Renderer/DrawCall.hpp>
#include <Engine/Renderer/Mesh.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Components/ParticleEmitter.hpp>
#include <Engine/Scene/Components/ParticleGpuSimulation.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Math/Matrix4.hpp>
#include <Physics/Layer.hpp>
#include <algorithm>
#include <vector>

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

// ParticlePassと同じ規則でLocal座標をWorld座標へ移す。
math::Vector3 ParticleWorldPoint(const Transform& transform, const math::Vector3& localPoint)
{
    const math::Vector3 scaled = {
        localPoint.x * transform.worldScale.x,
        localPoint.y * transform.worldScale.y,
        localPoint.z * transform.worldScale.z
    };
    return transform.worldPosition + transform.worldRotation * scaled;
}

math::Vector3 ParticleWorldVector(const Transform& transform, const math::Vector3& localVector)
{
    return transform.worldRotation * localVector;
}

// 選択されたParticleEmitterの現在形状を、テクスチャAlpha込みでSelection Maskへ描く。
// WHY: GameObjectのBounds矩形では炎・煙の透明部分まで囲まれ、Unity型のシルエット輪郭にならない。
void DrawParticleSelectionMask(GameObject& go, ParticleEmitter& emitter, RenderPassContext& ctx)
{
    auto& h = ctx.handles;
    if (!emitter.enabled || emitter.particles.empty() || !emitter.texture.IsValid()
        || !emitter.renderCB.IsValid() || !h.selectionMaskParticleShader.IsValid()
        || !h.particleVB.IsValid() || !h.particleIB.IsValid()
        || !h.selectionMaskPSO.IsValid() || !emitter.meshParticlePath.empty()) {
        return;
    }

    constexpr float uv[4][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 1 } };
    std::vector<ParticleVertex> vertices;
    const int particleCount = (std::min)(
        static_cast<int>(emitter.particles.size()), kMaxParticleDraw);
    vertices.reserve(static_cast<std::size_t>(particleCount) * 4);
    int quadCount = 0;

    const auto emitQuad = [&](const math::Vector3& center, const math::Vector3& velocity,
                              float size, float rotation, const math::Vector4& color,
                              const Particle& source) {
        if (quadCount >= kMaxParticleDraw) return;
        for (int corner = 0; corner < 4; ++corner) {
            ParticleVertex vertex{};
            vertex.center[0] = center.x;
            vertex.center[1] = center.y;
            vertex.center[2] = center.z;
            vertex.uv[0] = uv[corner][0];
            vertex.uv[1] = uv[corner][1];
            vertex.color[0] = color.x;
            vertex.color[1] = color.y;
            vertex.color[2] = color.z;
            vertex.color[3] = color.w;
            vertex.size = size;
            vertex.rotation = rotation;
            vertex.uvRect[0] = source.uvRect.x;
            vertex.uvRect[1] = source.uvRect.y;
            vertex.uvRect[2] = source.uvRect.z;
            vertex.uvRect[3] = source.uvRect.w;
            vertex.velocity[0] = velocity.x;
            vertex.velocity[1] = velocity.y;
            vertex.velocity[2] = velocity.z;
            vertex.nextUvRect[0] = source.nextUvRect.x;
            vertex.nextUvRect[1] = source.nextUvRect.y;
            vertex.nextUvRect[2] = source.nextUvRect.z;
            vertex.nextUvRect[3] = source.nextUvRect.w;
            vertex.spriteBlend = source.spriteBlend;
            vertices.push_back(vertex);
        }
        ++quadCount;
    };

    const bool localSpace = emitter.simulationSpace == ParticleSimulationSpace::Local;
    const bool billboardTrails = emitter.trailEnabled && !emitter.trailRibbon;
    const int trailPoints = billboardTrails
        ? std::clamp(emitter.trailPointCount, 1, kMaxParticleTrailPoints) : 0;
    for (int index = 0; index < particleCount; ++index) {
        const Particle& particle = emitter.particles[static_cast<std::size_t>(index)];
        const math::Vector3 position = localSpace
            ? ParticleWorldPoint(go.transform, particle.position) : particle.position;
        const math::Vector3 velocity = localSpace
            ? ParticleWorldVector(go.transform, particle.velocity) : particle.velocity;
        emitQuad(position, velocity, particle.size, particle.rotation, particle.color, particle);

        const int usedTrail = (std::min)(static_cast<int>(particle.trailCount), trailPoints);
        for (int trail = 0; trail < usedTrail; ++trail) {
            const float fade = 1.0f - static_cast<float>(trail + 1)
                / static_cast<float>(trailPoints + 1);
            const math::Vector3 trailPosition = localSpace
                ? ParticleWorldPoint(go.transform,
                    particle.trailPoints[static_cast<std::size_t>(trail)])
                : particle.trailPoints[static_cast<std::size_t>(trail)];
            const float width = emitter.trailWidthScale
                + (1.0f - emitter.trailWidthScale) * fade;
            const float alpha = emitter.trailAlphaScale
                + (1.0f - emitter.trailAlphaScale) * fade;
            const math::Vector4 trailColor = {
                particle.color.x * emitter.trailColorTint.x,
                particle.color.y * emitter.trailColorTint.y,
                particle.color.z * emitter.trailColorTint.z,
                particle.color.w * emitter.trailColorTint.w * alpha
            };
            emitQuad(trailPosition, velocity, particle.size * width, particle.rotation,
                     trailColor, particle);
        }
    }
    if (quadCount == 0) return;

    ctx.resources.Update(h.particleVB, vertices.data(),
                         static_cast<std::uint32_t>(vertices.size() * sizeof(ParticleVertex)));
    renderer::DrawCall draw;
    draw.vertexBuffer = h.particleVB;
    draw.indexBuffer = h.particleIB;
    draw.indexCount = static_cast<std::uint32_t>(quadCount * 6);
    draw.shader = h.selectionMaskParticleShader;
    draw.pipelineState = h.selectionMaskPSO;
    draw.constantBuffers[0] = h.frameCB;
    draw.constantBuffers[2] = emitter.renderCB;
    draw.textures[0] = emitter.texture;
    ctx.renderer.SetSampler(0, renderer::SamplerMode::WRAP_BILINEAR);
    ctx.renderer.Submit(draw, ctx.resources);
}

// GPU粒子はCPU側にparticles配列が無いため、StructuredBufferを直接読むMask VSで合成する。
void DrawGpuParticleSelectionMask(ParticleEmitter& emitter, RenderPassContext& ctx)
{
    auto& h = ctx.handles;
    if (!CanUseGpuSimulation(emitter) || !emitter.gpuParticleBuffer.IsValid()
        || !emitter.texture.IsValid() || !emitter.renderCB.IsValid()
        || !h.selectionMaskParticleGpuShader.IsValid() || !h.selectionMaskPSO.IsValid()
        || !emitter.meshParticlePath.empty()) {
        return;
    }

    const int maximumParticles = (std::max)(emitter.maxParticles, 0);
    if (maximumParticles == 0) return;
    renderer::DrawCall draw;
    draw.shader = h.selectionMaskParticleGpuShader;
    draw.pipelineState = h.selectionMaskPSO;
    draw.vertexCount = static_cast<std::uint32_t>(maximumParticles) * 6u;
    draw.constantBuffers[0] = h.frameCB;
    draw.constantBuffers[2] = emitter.renderCB;
    draw.textures[0] = emitter.texture;
    draw.vsBuffers[0] = emitter.gpuParticleBuffer;
    draw.vsBuffers[1] = emitter.gpuSortBuffer;
    ctx.renderer.SetSampler(0, renderer::SamplerMode::WRAP_BILINEAR);
    ctx.renderer.Submit(draw, ctx.resources);
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
        objData.worldInvTranspose = math::Matrix4::InverseTransposeAffine(objData.world);
        resources.Update(h.objectCB, &objData, sizeof(PerObjectCB));

        if (h.selectionMaskShader.IsValid()) {
            auto* mr = go.GetComponent<MeshRenderer>();
    if (mr && mr->enabled && mr->lodVisible && mr->mesh && !mr->mesh->isSkinned &&
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
            // WHY: Animator はモデルルート側、SkinnedMeshRenderer はサブメッシュ子 GO に
            //      分かれる構成が一般的。同一 GO だけを見ると Animator を見失い
            //      bind pose CB へフォールバックしてアウトラインが T ポーズのまま止まる。
            //      通常描画パスと同じ FindAnimator (親方向探索) で解決する。
            auto* anim = FindAnimator(go);
    if (smr && smr->enabled && smr->lodVisible && smr->model) {
                const auto skinCB = ResolveSkinningCB(
                    anim ? anim->skinningBuffer : decltype(anim->skinningBuffer){},
                    smr->model, h.bindPoseSkinningCB);

                // 通常描画パスと同じく、モデル全体のうち可視スロットの submesh だけを描く。
                const auto* mat = go.GetComponent<MaterialComponent>();
                // mi はローカルスロット番号 (submeshIndices 対応)。
                for (size_t mi = 0; mi < smr->SubmeshCount(); ++mi) {
                    renderer::Mesh* meshPtr = smr->SubmeshMesh(mi);
                    if (!meshPtr) continue;
                    if (!meshPtr->vertexBuffer.IsValid() || !meshPtr->indexBuffer.IsValid()) continue;
                    if (mat && !mat->SlotAt(mi).visible) continue;

                    renderer::DrawCall dc;
                    dc.vertexBuffer = smr->ResolveSlotVertexBuffer(mi, meshPtr->vertexBuffer);
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

        if (auto* particle = go.GetComponent<ParticleEmitter>()) {
            DrawParticleSelectionMask(go, *particle, ctx);
            DrawGpuParticleSelectionMask(*particle, ctx);
        }
    }

    TerrainSelectionMaskSystem(ctx);
    WaterSelectionMaskSystem(ctx);

    r.SetRenderTarget(h.hdrRT, resources);
}

} // namespace fbzz::scene
