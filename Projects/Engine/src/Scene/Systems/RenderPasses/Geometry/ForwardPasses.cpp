// FBZZ Engine
// RenderPasses/ForwardPasses.cpp | fbzz::scene
// Forward パイプライン: 不透明 + 半透明の静的・スキンドメッシュ描画
//
// カリング戦略:
//   1. Frustum Culling (フラスタムカリング)
//      カメラ視錐台に交差しないバウンディング球を持つオブジェクトを除外する。
//      Frustum::IntersectsSphere() で 6 平面テストを行う。
//
//   2. Software Occlusion Culling (ソフトウェアオクルージョンカリング)
//      不透明静的オブジェクトを前から後ろ順にソートし、CPU 上の小型深度バッファで
//      完全に隠蔽されているかどうかを判定する。
//      スキンドメッシュはバインドポーズ球がアニメーション後の姿勢と乖離するため除外。
//      半透明は深度書き込みを行わないためオクルージョンカリング対象外。
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

// 不透明静的メッシュのギャザーエントリ (ソート + オクルージョンカリング用)
struct OpaqueStaticEntry {
    GameObject*        go;
    MeshRenderer*      mr;
    MaterialComponent* mat;
    float              distSq; // カメラからの距離^2 (前から後ろ順ソート用)
};

// 不透明スキンドメッシュのギャザーエントリ
struct OpaqueSkinnedEntry {
    GameObject*          go;
    SkinnedMeshRenderer* smr;
    MaterialComponent*   mat;
    AnimatorComponent*   anim;
    float                distSq;
};

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
    renderer.Clear(kHdrClearColor);

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
    const float shadowTexelSize      = 1.0f / static_cast<float>(ctx.settings.shadow.mapResolution);
    shadowData.lightViewProjection   = ctx.lightVP;
    shadowData.shadowMapTexelSize[0] = shadowTexelSize;
    shadowData.shadowMapTexelSize[1] = shadowTexelSize;
    shadowData.shadowBias            = ctx.shadowBiasNDC;
    shadowData.shadowStrength        = ctx.shadowStrength;
    shadowData.shadowPcfRadius       = ctx.settings.shadow.pcfRadius;
    shadowData.cloudShadowStrength   = ctx.cloudShadowStrength;
    shadowData.cloudShadowCoverage   = ctx.cloudShadowCoverage;
    shadowData.cloudShadowScale      = ctx.cloudShadowScale;
    shadowData.cloudShadowSpeed      = ctx.cloudShadowSpeed;
    shadowData.cloudShadowTime       = ctx.cloudShadowTime;
    shadowData.cloudShadowWindX      = ctx.cloudShadowWindX;
    shadowData.cloudShadowWindZ      = ctx.cloudShadowWindZ;
    resources.Update(h.shadowCB, &shadowData, sizeof(ShadowConstantsCB));

    renderer.SetSampler(0, renderer::SamplerMode::WRAP_ANISOTROPIC);
    renderer.SetSampler(1, renderer::SamplerMode::BORDER_ZERO);

    const auto shadowDepthTex = resources.GetDepthTexture(h.shadowMapRT);
    const auto& frustum       = *ctx.cameraFrustum;

    // =========================================================================
    // Phase 1: フラスタムカリング + ギャザー
    // =========================================================================
    // フラスタム外のオブジェクトを除外し、不透明 / 半透明ごとに収集する。
    // 不透明は Phase 2 でソートし、Phase 3 でオクルージョンカリングを行う。

    std::vector<OpaqueStaticEntry>  opaqueStaticQueue;
    std::vector<OpaqueSkinnedEntry> opaqueSkinnedQueue;
    std::vector<TransparentEntry>   transparentQueue;

    // ── 静的メッシュのギャザー ─────────────────────────────────────────────────
    for (auto& go : ctx.scene.GameObjects()) {
        if (!ShouldRenderGameObject(go, ctx.cullingMask)) continue;
        auto* mr  = go.GetComponent<MeshRenderer>();
        auto* mat = go.GetComponent<MaterialComponent>();
        if (!mr || !mr->enabled || !mr->mesh || !mat || !mat->EnsureMaterialAsset()) continue;
        if (!mr->mesh->vertexBuffer.IsValid() || !mr->mesh->indexBuffer.IsValid()) continue;
        if (mr->mesh->isSkinned) continue;

        ++ctx.statsTotalObjects;

        // フラスタムカリング: バウンディング球が視錐台外なら除外
        if (!IsVisibleInFrustum(frustum, go.transform, *mr->mesh)) {
            ++ctx.statsFrustumCulled;
            continue;
        }

        const float dx = go.transform.position.x - cam.m_position.x;
        const float dy = go.transform.position.y - cam.m_position.y;
        const float dz = go.transform.position.z - cam.m_position.z;
        const float distSq = dx*dx + dy*dy + dz*dz;

        if (mat->GetBlendMode() == renderer::BlendMode::OPAQUE_BLEND) {
            opaqueStaticQueue.push_back({ &go, mr, mat, distSq });
        } else {
            // 半透明は即収集 (オクルージョンカリング対象外)
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
            dc.pipelineState      = rs.IsWireframe() ? h.wireframePSO
                                                     : GetOrCreateMaterialPSO(resources, mat->GetBlendMode(), mat->IsDoubleSided());
            dc.layer              = renderer::RenderLayer::TRANSPARENT_LAYER;
            dc.constantBuffers[0] = h.frameCB;
            dc.constantBuffers[1] = h.objectCB;
            dc.constantBuffers[2] = material->paramsBuffer;
            dc.constantBuffers[3] = h.lightCB;
            dc.constantBuffers[4] = h.shadowCB;
            for (size_t ti = 0; ti < material->textures.size() && ti < 8; ++ti)
                if (material->textures[ti].IsValid()) dc.textures[ti] = material->textures[ti];
            dc.textures[8] = shadowDepthTex;
            transparentQueue.push_back({ dc, objData, distSq, mat->GetRenderQueue() });
        }
    }

    // ── スキンドメッシュのギャザー ─────────────────────────────────────────────
    for (auto& go : ctx.scene.GameObjects()) {
        if (!ShouldRenderGameObject(go, ctx.cullingMask)) continue;
        auto* smr  = go.GetComponent<SkinnedMeshRenderer>();
        auto* mat  = go.GetComponent<MaterialComponent>();
        auto* anim = FindAnimator(go);
        if (!smr || !smr->enabled || !smr->model || !mat || !mat->EnsureMaterialAsset()) continue;

        ++ctx.statsTotalObjects;
        if (!IsSkinnedVisibleInFrustum(frustum, go.transform, *smr)) {
            ++ctx.statsFrustumCulled;
            continue;
        }

        const float dx = go.transform.position.x - cam.m_position.x;
        const float dy = go.transform.position.y - cam.m_position.y;
        const float dz = go.transform.position.z - cam.m_position.z;
        const float distSq = dx*dx + dy*dy + dz*dz;

        if (mat->GetBlendMode() == renderer::BlendMode::OPAQUE_BLEND) {
            opaqueSkinnedQueue.push_back({ &go, smr, mat, anim, distSq });
        } else {
            auto* material = SyncMaterial(*mat, resources, true);
            if (!material) continue;
            const bool surfaceMisassigned =
                material->shader.IsValid() && IsSurfaceMaterialShader(material->shaderPath);
            if (surfaceMisassigned)
                LogSkinnedSurfaceFallbackWarningOnce(material->shaderPath);
            renderer::Material* drawMaterial = surfaceMisassigned
                ? GetFallbackMaterial(resources, true)
                : material;
            if (!drawMaterial) continue;
            const auto skinnedShader = drawMaterial->shader;
            if (!skinnedShader.IsValid()) continue;

            PerObjectCB objData{};
            objData.world             = go.transform.GetWorldMatrix();
            objData.worldInvTranspose = math::Matrix4::Transpose(math::Matrix4::Inverse(objData.world));
            const auto skinCB = (anim && anim->skinningBuffer.IsValid())
                ? anim->skinningBuffer : h.bindPoseSkinningCB;

            const int meshStart = (smr->meshIndex < 0) ? 0 : smr->meshIndex;
            const int meshEnd   = (smr->meshIndex < 0) ? static_cast<int>(smr->model->meshes.size()) : smr->meshIndex + 1;
            for (int mi = meshStart; mi < meshEnd; ++mi) {
                const auto& meshPtr = smr->model->meshes[mi];
                if (!meshPtr) continue;
                if (!meshPtr->vertexBuffer.IsValid() || !meshPtr->indexBuffer.IsValid()) continue;

                renderer::DrawCall dc;
                dc.vertexBuffer       = meshPtr->vertexBuffer;
                dc.indexBuffer        = meshPtr->indexBuffer;
                dc.indexCount         = meshPtr->indexCount;
                dc.vertexCount        = meshPtr->vertexCount;
                dc.shader             = skinnedShader;
                dc.pipelineState      = rs.IsWireframe() ? h.wireframePSO
                                                         : GetOrCreateMaterialPSO(resources, mat->GetBlendMode(), mat->IsDoubleSided());
                dc.layer              = renderer::RenderLayer::TRANSPARENT_LAYER;
                dc.constantBuffers[0] = h.frameCB;
                dc.constantBuffers[1] = h.objectCB;
                dc.constantBuffers[2] = drawMaterial->paramsBuffer;
                dc.constantBuffers[3] = h.lightCB;
                dc.constantBuffers[4] = h.shadowCB;
                dc.constantBuffers[7] = skinCB;
                for (size_t ti = 0; ti < drawMaterial->textures.size() && ti < 8; ++ti)
                    if (drawMaterial->textures[ti].IsValid()) dc.textures[ti] = drawMaterial->textures[ti];
                dc.textures[8] = shadowDepthTex;
                transparentQueue.push_back({ dc, objData, distSq, mat->GetRenderQueue() });
            }
        }
    }

    // =========================================================================
    // Phase 2: 不透明オブジェクトを RenderQueue → 前から後ろ順にソート
    // =========================================================================
    // WHY: RenderQueue で AlphaTest などの明示順を守り、その内側では前→後ろ順にする。
    //      前→後ろ順で描画すると GPU の Early-Z Rejection が機能しやすくなり、
    //      シェーダー実行コストを削減できる。
    //      また SW オクルージョンカリングは前に描かれたオブジェクトほど
    //      後続オブジェクトを効率よく遮蔽できるため、ソートが前提となる。

    std::sort(opaqueStaticQueue.begin(), opaqueStaticQueue.end(),
        [](const OpaqueStaticEntry& a, const OpaqueStaticEntry& b) {
            if (a.mat->GetRenderQueue() != b.mat->GetRenderQueue())
                return a.mat->GetRenderQueue() < b.mat->GetRenderQueue();
            return a.distSq < b.distSq;
        });
    std::sort(opaqueSkinnedQueue.begin(), opaqueSkinnedQueue.end(),
        [](const OpaqueSkinnedEntry& a, const OpaqueSkinnedEntry& b) {
            if (a.mat->GetRenderQueue() != b.mat->GetRenderQueue())
                return a.mat->GetRenderQueue() < b.mat->GetRenderQueue();
            return a.distSq < b.distSq;
        });

    // =========================================================================
    // Phase 3: オクルージョンカリング + 不透明描画
    // =========================================================================
    if (ctx.occlusionCuller)
        ctx.occlusionCuller->Reset(cam);

    // ── 不透明静的メッシュ ─────────────────────────────────────────────────────
    for (auto& entry : opaqueStaticQueue) {
        auto& go  = *entry.go;
        auto* mr  = entry.mr;
        auto* mat = entry.mat;

        auto* material = SyncMaterial(*mat, resources);
        if (!material || !material->shader.IsValid()) continue;

        // オクルージョンカリング: 完全に隠蔽されていれば描画スキップ
        const auto bounds = ComputeWorldBounds(go.transform, *mr->mesh);
        if (ctx.occlusionCuller && !ctx.occlusionCuller->TestAndRaster(bounds.center, bounds.radius)) {
            ++ctx.statsOcclusionCulled;
            continue;
        }

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
        dc.pipelineState      = rs.IsWireframe() ? h.wireframePSO
                                                 : GetOrCreateMaterialPSO(resources, mat->GetBlendMode(), mat->IsDoubleSided());
        dc.layer              = renderer::RenderLayer::OPAQUE_LAYER;
        dc.constantBuffers[0] = h.frameCB;
        dc.constantBuffers[1] = h.objectCB;
        dc.constantBuffers[2] = material->paramsBuffer;
        dc.constantBuffers[3] = h.lightCB;
        dc.constantBuffers[4] = h.shadowCB;
        for (size_t ti = 0; ti < material->textures.size() && ti < 8; ++ti)
            if (material->textures[ti].IsValid()) dc.textures[ti] = material->textures[ti];
        dc.textures[8] = shadowDepthTex;
        renderer.Submit(dc, resources);

        ++ctx.statsDrawCalls;
        ctx.statsVertexCount   += static_cast<int>(mr->mesh->vertexCount);
        ctx.statsTriangleCount += static_cast<int>(mr->mesh->indexCount / 3u);
    }

    // ── 不透明スキンドメッシュ ─────────────────────────────────────────────────
    // バインドポーズ球はアニメーション後の実際の姿勢と乖離するため
    // SW オクルージョンカリングは適用しない。
    for (auto& entry : opaqueSkinnedQueue) {
        auto& go   = *entry.go;
        auto* smr  = entry.smr;
        auto* mat  = entry.mat;
        auto* anim = entry.anim;

        auto* material = SyncMaterial(*mat, resources, true);
        if (!material) continue;

        const bool surfaceMisassigned =
            material->shader.IsValid() && IsSurfaceMaterialShader(material->shaderPath);
        if (surfaceMisassigned)
            LogSkinnedSurfaceFallbackWarningOnce(material->shaderPath);
        renderer::Material* drawMaterial = surfaceMisassigned
            ? GetFallbackMaterial(resources, true)
            : material;
        if (!drawMaterial) continue;
        const auto skinnedShader = drawMaterial->shader;
        if (!skinnedShader.IsValid()) continue;

        PerObjectCB objData{};
        objData.world             = go.transform.GetWorldMatrix();
        objData.worldInvTranspose = math::Matrix4::Transpose(math::Matrix4::Inverse(objData.world));
        resources.Update(h.objectCB, &objData, sizeof(PerObjectCB));

        const auto skinCB = (anim && anim->skinningBuffer.IsValid())
            ? anim->skinningBuffer : h.bindPoseSkinningCB;

        const int meshStart = (smr->meshIndex < 0) ? 0 : smr->meshIndex;
        const int meshEnd   = (smr->meshIndex < 0) ? static_cast<int>(smr->model->meshes.size()) : smr->meshIndex + 1;
        for (int mi = meshStart; mi < meshEnd; ++mi) {
            const auto& meshPtr = smr->model->meshes[mi];
            if (!meshPtr) continue;
            if (!meshPtr->vertexBuffer.IsValid() || !meshPtr->indexBuffer.IsValid()) continue;

            renderer::DrawCall dc;
            dc.vertexBuffer       = meshPtr->vertexBuffer;
            dc.indexBuffer        = meshPtr->indexBuffer;
            dc.indexCount         = meshPtr->indexCount;
            dc.vertexCount        = meshPtr->vertexCount;
            dc.shader             = skinnedShader;
            dc.pipelineState      = rs.IsWireframe() ? h.wireframePSO
                                                     : GetOrCreateMaterialPSO(resources, mat->GetBlendMode(), mat->IsDoubleSided());
            dc.layer              = renderer::RenderLayer::OPAQUE_LAYER;
            dc.constantBuffers[0] = h.frameCB;
            dc.constantBuffers[1] = h.objectCB;
            dc.constantBuffers[2] = drawMaterial->paramsBuffer;
            dc.constantBuffers[3] = h.lightCB;
            dc.constantBuffers[4] = h.shadowCB;
            dc.constantBuffers[7] = skinCB;
            for (size_t ti = 0; ti < material->textures.size() && ti < 8; ++ti)
                if (material->textures[ti].IsValid()) dc.textures[ti] = material->textures[ti];
            dc.textures[8] = shadowDepthTex;
            renderer.Submit(dc, resources);

            ++ctx.statsDrawCalls;
            ctx.statsVertexCount   += static_cast<int>(meshPtr->vertexCount);
            ctx.statsTriangleCount += static_cast<int>(meshPtr->indexCount / 3u);
        }
    }

    // ── 半透明をソートして Submit ─────────────────────────────────────────────
    SortAndSubmitTransparent(transparentQueue, renderer, resources, h.objectCB);
}

} // namespace fbzz::scene
