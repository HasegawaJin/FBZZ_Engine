// FBZZ Engine
// RenderPasses/ForwardPasses.cpp | fbzz::scene
// Forward パイプライン: 不透明 + 半透明の静的・スキンドメッシュ描画
#include "GeometryPasses.hpp"
#include "Engine/Scene/Scene.hpp"
#include "Engine/Scene/Transform.hpp"
#include "Engine/Renderer/Mesh.hpp"
#include "Engine/Scene/Components/MeshRenderer.hpp"
#include "Engine/Scene/Components/MaterialComponent.hpp"
#include "Engine/Scene/Components/AnimatorComponent.hpp"
#include "Engine/Scene/Components/SkinnedMeshRenderer.hpp"
#include "Engine/Core/Logger.hpp"
#include "Engine/Renderer/DrawCall.hpp"
#include "Engine/Renderer/SamplerMode.hpp"
#include <Math/Matrix4.hpp>
#include <algorithm>
#include <vector>

namespace fbzz::scene {

namespace {

struct TransparentEntry {
    renderer::DrawCall dc;
    PerObjectCB        objData;
    float              distSqFromCamera;
    int32_t            renderQueue;
};

void SortAndSubmitTransparent(
    std::vector<TransparentEntry>& queue,
    renderer::IRenderer& renderer,
    renderer::ResourceManager& resources,
    renderer::ResourceHandle<renderer::ConstantBufferTag> objectCB)
{
    std::sort(queue.begin(), queue.end(),
        [](const TransparentEntry& a, const TransparentEntry& b) {
            if (a.renderQueue != b.renderQueue) return a.renderQueue < b.renderQueue;
            return a.distSqFromCamera > b.distSqFromCamera;
        });
    for (auto& entry : queue) {
        resources.Update(objectCB, &entry.objData, sizeof(PerObjectCB));
        renderer.Submit(entry.dc, resources);
    }
}

} // anonymous namespace

void ExecuteForwardPasses(RenderPassContext& ctx)
{
    auto& renderer  = ctx.renderer;
    auto& resources = ctx.resources;
    auto& h         = ctx.handles;
    const auto& rs  = ctx.settings;
    const auto& cam = ctx.camera;

    renderer.SetRenderTarget(h.hdrRT, resources);
    renderer.Clear({ 0.005f, 0.005f, 0.02f, 1.0f });

    PerFrameCB frameData{};
    frameData.view              = cam.GetViewMatrix();
    frameData.projection        = cam.GetProjectionMatrix();
    frameData.viewProjection    = cam.GetViewProjection();
    frameData.invViewProjection = math::Matrix4::Inverse(frameData.viewProjection);
    frameData.cameraPos         = cam.m_position;
    frameData.nearZ             = cam.m_near;
    frameData.farZ              = cam.m_far;
    resources.Update(h.frameCB, &frameData, sizeof(PerFrameCB));
    resources.Update(h.lightCB, &ctx.lightData, sizeof(renderer::LightConstantsCB));

    ShadowConstantsCB shadowData{};
    shadowData.lightViewProjection   = ctx.lightVP;
    shadowData.shadowMapTexelSize[0] = 1.0f / static_cast<float>(kShadowMapSize);
    shadowData.shadowMapTexelSize[1] = 1.0f / static_cast<float>(kShadowMapSize);
    shadowData.shadowBias            = 0.005f;
    resources.Update(h.shadowCB, &shadowData, sizeof(ShadowConstantsCB));

    renderer.SetSampler(0, renderer::SamplerMode::WRAP_ANISOTROPIC);
    renderer.SetSampler(1, renderer::SamplerMode::BORDER_ZERO);

    const auto shadowDepthTex = resources.GetDepthTexture(h.shadowMapRT);

    std::vector<TransparentEntry> transparentQueue;

    // ── 不透明 static meshes ───────────────────────────────────────────────────
    for (auto& go : ctx.scene.GameObjects()) {
        if (!ShouldRenderGameObject(go, ctx.cullingMask)) continue;
        auto* mr  = go.GetComponent<MeshRenderer>();
        auto* mat = go.GetComponent<MaterialComponent>();
        if (!mr || !mr->enabled || !mr->mesh || !mat) continue;
        if (!mr->mesh->vertexBuffer.IsValid() || !mr->mesh->indexBuffer.IsValid()) continue;
        if (mr->mesh->isSkinned) continue;
        if (mat->blendMode != renderer::BlendMode::OPAQUE) continue;
        auto* material = SyncMaterial(*mat, resources);
        if (!material || !material->shader.IsValid()) continue;

        PerObjectCB objData{};
        objData.world             = go.transform.GetWorldMatrix();
        objData.worldInvTranspose = math::Matrix4::Transpose(math::Matrix4::Inverse(objData.world));
        resources.Update(h.objectCB, &objData, sizeof(PerObjectCB));

        renderer::DrawCall dc;
        dc.vertexBuffer       = mr->mesh->vertexBuffer;
        dc.indexBuffer        = mr->mesh->indexBuffer;
        dc.indexCount         = mr->mesh->indexCount;
        dc.vertexCount        = mr->mesh->vertexCount;
        dc.shader             = material->shader;
        dc.pipelineState      = rs.wireframeMode ? h.wireframePSO
                                                 : GetOrCreateMaterialPSO(resources, mat->blendMode, mat->doubleSided);
        dc.layer              = renderer::RenderLayer::OPAQUE;
        dc.constantBuffers[0] = h.frameCB;
        dc.constantBuffers[1] = h.objectCB;
        dc.constantBuffers[2] = material->paramsBuffer;
        dc.constantBuffers[3] = h.lightCB;
        dc.constantBuffers[4] = h.shadowCB;
        for (size_t ti = 0; ti < material->textures.size() && ti < 5; ++ti)
            if (material->textures[ti].IsValid()) dc.textures[ti] = material->textures[ti];
        dc.textures[8] = shadowDepthTex;
        renderer.Submit(dc, resources);
    }

    // ── 不透明 skinned meshes ──────────────────────────────────────────────────
    for (auto& go : ctx.scene.GameObjects()) {
        if (!ShouldRenderGameObject(go, ctx.cullingMask)) continue;
        auto* smr  = go.GetComponent<SkinnedMeshRenderer>();
        auto* mat  = go.GetComponent<MaterialComponent>();
        auto* anim = go.GetComponent<AnimatorComponent>();
        if (!smr || !smr->enabled || !smr->model) continue;
        if (!mat) continue;
        if (mat->blendMode != renderer::BlendMode::OPAQUE) continue;
        auto* material = SyncMaterial(*mat, resources);
        if (!material) continue;

        const bool surfaceMisassigned =
            material->shader.IsValid() && IsSurfaceMaterialShader(material->shaderPath);
        if (surfaceMisassigned)
        {
            FBZZ_LOG_WARN("SkinnedMeshRenderer に Surface シェーダーが設定されています: %s"
                          " → SkinnedPBR にフォールバック。Skinned/ 以下のシェーダーを使用してください。",
                          material->shaderPath.c_str());
        }
        const auto skinnedShader = (!surfaceMisassigned && material->shader.IsValid())
            ? material->shader : h.skinnedPbrShader;
        if (!skinnedShader.IsValid()) continue;

        PerObjectCB objData{};
        objData.world             = go.transform.GetWorldMatrix();
        objData.worldInvTranspose = math::Matrix4::Transpose(math::Matrix4::Inverse(objData.world));
        resources.Update(h.objectCB, &objData, sizeof(PerObjectCB));

        const auto skinCB = (anim && anim->skinningBuffer.IsValid())
            ? anim->skinningBuffer : h.bindPoseSkinningCB;

        for (const auto& meshPtr : smr->model->meshes) {
            if (!meshPtr) continue;
            if (!meshPtr->vertexBuffer.IsValid() || !meshPtr->indexBuffer.IsValid()) continue;

            renderer::DrawCall dc;
            dc.vertexBuffer       = meshPtr->vertexBuffer;
            dc.indexBuffer        = meshPtr->indexBuffer;
            dc.indexCount         = meshPtr->indexCount;
            dc.vertexCount        = meshPtr->vertexCount;
            dc.shader             = skinnedShader;
            dc.pipelineState      = rs.wireframeMode ? h.wireframePSO
                                                     : GetOrCreateMaterialPSO(resources, mat->blendMode, mat->doubleSided);
            dc.layer              = renderer::RenderLayer::OPAQUE;
            dc.constantBuffers[0] = h.frameCB;
            dc.constantBuffers[1] = h.objectCB;
            dc.constantBuffers[2] = material->paramsBuffer;
            dc.constantBuffers[3] = h.lightCB;
            dc.constantBuffers[4] = h.shadowCB;
            dc.constantBuffers[7] = skinCB;
            for (size_t ti = 0; ti < material->textures.size() && ti < 5; ++ti)
                if (material->textures[ti].IsValid()) dc.textures[ti] = material->textures[ti];
            dc.textures[8] = shadowDepthTex;
            renderer.Submit(dc, resources);
        }
    }

    // ── 半透明 static meshes (キュー蓄積) ────────────────────────────────────
    for (auto& go : ctx.scene.GameObjects()) {
        if (!ShouldRenderGameObject(go, ctx.cullingMask)) continue;
        auto* mr  = go.GetComponent<MeshRenderer>();
        auto* mat = go.GetComponent<MaterialComponent>();
        if (!mr || !mr->enabled || !mr->mesh || !mat) continue;
        if (!mr->mesh->vertexBuffer.IsValid() || !mr->mesh->indexBuffer.IsValid()) continue;
        if (mr->mesh->isSkinned) continue;
        if (mat->blendMode == renderer::BlendMode::OPAQUE) continue;
        auto* material = SyncMaterial(*mat, resources);
        if (!material || !material->shader.IsValid()) continue;

        PerObjectCB objData{};
        objData.world             = go.transform.GetWorldMatrix();
        objData.worldInvTranspose = math::Matrix4::Transpose(math::Matrix4::Inverse(objData.world));

        renderer::DrawCall dc;
        dc.vertexBuffer       = mr->mesh->vertexBuffer;
        dc.indexBuffer        = mr->mesh->indexBuffer;
        dc.indexCount         = mr->mesh->indexCount;
        dc.vertexCount        = mr->mesh->vertexCount;
        dc.shader             = material->shader;
        dc.pipelineState      = rs.wireframeMode ? h.wireframePSO
                                                 : GetOrCreateMaterialPSO(resources, mat->blendMode, mat->doubleSided);
        dc.layer              = renderer::RenderLayer::TRANSPARENT;
        dc.constantBuffers[0] = h.frameCB;
        dc.constantBuffers[1] = h.objectCB;
        dc.constantBuffers[2] = material->paramsBuffer;
        dc.constantBuffers[3] = h.lightCB;
        dc.constantBuffers[4] = h.shadowCB;
        for (size_t ti = 0; ti < material->textures.size() && ti < 5; ++ti)
            if (material->textures[ti].IsValid()) dc.textures[ti] = material->textures[ti];
        dc.textures[8] = shadowDepthTex;

        const float dx = objData.world.m[0][3] - cam.m_position.x;
        const float dy = objData.world.m[1][3] - cam.m_position.y;
        const float dz = objData.world.m[2][3] - cam.m_position.z;
        transparentQueue.push_back({ dc, objData, dx*dx + dy*dy + dz*dz, mat->renderQueue });
    }

    // ── 半透明 skinned meshes (キュー蓄積) ───────────────────────────────────
    for (auto& go : ctx.scene.GameObjects()) {
        if (!ShouldRenderGameObject(go, ctx.cullingMask)) continue;
        auto* smr  = go.GetComponent<SkinnedMeshRenderer>();
        auto* mat  = go.GetComponent<MaterialComponent>();
        auto* anim = go.GetComponent<AnimatorComponent>();
        if (!smr || !smr->enabled || !smr->model) continue;
        if (!mat) continue;
        if (mat->blendMode == renderer::BlendMode::OPAQUE) continue;
        auto* material = SyncMaterial(*mat, resources);
        if (!material) continue;

        const bool surfaceMisassigned =
            material->shader.IsValid() && IsSurfaceMaterialShader(material->shaderPath);
        if (surfaceMisassigned)
        {
            FBZZ_LOG_WARN("SkinnedMeshRenderer に Surface シェーダーが設定されています: %s"
                          " → SkinnedPBR にフォールバック。Skinned/ 以下のシェーダーを使用してください。",
                          material->shaderPath.c_str());
        }
        const auto skinnedShader = (!surfaceMisassigned && material->shader.IsValid())
            ? material->shader : h.skinnedPbrShader;
        if (!skinnedShader.IsValid()) continue;

        PerObjectCB objData{};
        objData.world             = go.transform.GetWorldMatrix();
        objData.worldInvTranspose = math::Matrix4::Transpose(math::Matrix4::Inverse(objData.world));

        const auto skinCB = (anim && anim->skinningBuffer.IsValid())
            ? anim->skinningBuffer : h.bindPoseSkinningCB;

        for (const auto& meshPtr : smr->model->meshes) {
            if (!meshPtr) continue;
            if (!meshPtr->vertexBuffer.IsValid() || !meshPtr->indexBuffer.IsValid()) continue;

            renderer::DrawCall dc;
            dc.vertexBuffer       = meshPtr->vertexBuffer;
            dc.indexBuffer        = meshPtr->indexBuffer;
            dc.indexCount         = meshPtr->indexCount;
            dc.vertexCount        = meshPtr->vertexCount;
            dc.shader             = skinnedShader;
            dc.pipelineState      = rs.wireframeMode ? h.wireframePSO
                                                     : GetOrCreateMaterialPSO(resources, mat->blendMode, mat->doubleSided);
            dc.layer              = renderer::RenderLayer::TRANSPARENT;
            dc.constantBuffers[0] = h.frameCB;
            dc.constantBuffers[1] = h.objectCB;
            dc.constantBuffers[2] = material->paramsBuffer;
            dc.constantBuffers[3] = h.lightCB;
            dc.constantBuffers[4] = h.shadowCB;
            dc.constantBuffers[7] = skinCB;
            for (size_t ti = 0; ti < material->textures.size() && ti < 5; ++ti)
                if (material->textures[ti].IsValid()) dc.textures[ti] = material->textures[ti];
            dc.textures[8] = shadowDepthTex;

            const float dx = objData.world.m[0][3] - cam.m_position.x;
            const float dy = objData.world.m[1][3] - cam.m_position.y;
            const float dz = objData.world.m[2][3] - cam.m_position.z;
            transparentQueue.push_back({ dc, objData, dx*dx + dy*dy + dz*dz, mat->renderQueue });
        }
    }

    // ── 半透明をソートして Submit ─────────────────────────────────────────────
    SortAndSubmitTransparent(transparentQueue, renderer, resources, h.objectCB);
}

} // namespace fbzz::scene
