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

// 不透明スキンドメッシュのギャザーエントリ。
// WHY: 1 GameObject がモデル全体 (複数 submesh) を描き、submesh ごとに別マテリアル
//      スロットを持つため、ブレンドモード振り分けもキュー要素も submesh 粒度になる。
struct OpaqueSkinnedEntry {
    GameObject*          go;
    SkinnedMeshRenderer* smr;
    MaterialComponent*   mat;
    AnimatorComponent*   anim;
    float                distSq;
    size_t               meshIndex;
};

struct TransparentEntry {
    renderer::DrawCall dc;
    PerObjectCB        objData;
    float              distSqFromCamera;
    int32_t            renderQueue;
};

void SortAndSubmitTransparent(
    RenderPassContext& ctx,
    std::vector<TransparentEntry>& queue,
    renderer::ResourceHandle<renderer::ConstantBufferTag> objectCB)
{
    std::sort(queue.begin(), queue.end(),
        [](const TransparentEntry& a, const TransparentEntry& b) {
            if (a.renderQueue != b.renderQueue) return a.renderQueue < b.renderQueue;
            return a.distSqFromCamera > b.distSqFromCamera;
        });
    for (auto& entry : queue) {
        ctx.resources.Update(objectCB, &entry.objData, sizeof(PerObjectCB));
        SubmitCounted(ctx, entry.dc);
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

    UpdateShadowConstants(ctx);

    renderer.SetSampler(0, renderer::SamplerMode::WRAP_ANISOTROPIC);
    renderer.SetSampler(1, renderer::SamplerMode::BORDER_ZERO);
    renderer.SetSampler(2, renderer::SamplerMode::CLAMP_LINEAR);

    const auto shadowDepthTex = resources.GetDepthTexture(h.shadowMapRT);

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
        if (!mr || !mr->enabled || !mr->lodVisible || !mr->mesh || !mat || !mat->EnsureMaterialAsset()) continue;
        if (!mr->mesh->vertexBuffer.IsValid() || !mr->mesh->indexBuffer.IsValid()) continue;
        if (mr->mesh->isSkinned) continue;

        ++ctx.statsTotalObjects;

        // 距離 / 極小 / 錐台カリング。落ちた理由の統計は IsMeshVisible が加算する。
        if (!IsMeshVisible(ctx, go, *mr->mesh)) continue;

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
            objData.worldInvTranspose = math::Matrix4::InverseTransposeAffine(objData.world);

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
            dc.constantBuffers[8] = h.advancedGraphicsCB;
            BindClusterLighting(dc, ctx);
            for (size_t ti = 0; ti < material->textures.size() && ti < 8; ++ti)
                if (material->textures[ti].IsValid()) dc.textures[ti] = material->textures[ti];
            dc.textures[8] = shadowDepthTex;
            dc.textures[16] = h.iblIrradiance;
            dc.textures[17] = h.iblPrefilter;
            dc.textures[18] = h.iblBrdfLut;
            transparentQueue.push_back({ dc, objData, distSq, mat->GetRenderQueue() });
        }
    }

    // ── スキンドメッシュのギャザー ─────────────────────────────────────────────
    for (auto& go : ctx.scene.GameObjects()) {
        if (!ShouldRenderGameObject(go, ctx.cullingMask)) continue;
        auto* smr  = go.GetComponent<SkinnedMeshRenderer>();
        auto* mat  = go.GetComponent<MaterialComponent>();
        auto* anim = FindAnimator(go);
        if (!smr || !smr->enabled || !smr->lodVisible || !smr->model || !mat || !mat->EnsureMaterialAsset()) continue;

        ++ctx.statsTotalObjects;
        if (!IsSkinnedVisible(ctx, go, *smr)) continue;

        const float dx = go.transform.position.x - cam.m_position.x;
        const float dy = go.transform.position.y - cam.m_position.y;
        const float dz = go.transform.position.z - cam.m_position.z;
        const float distSq = dx*dx + dy*dy + dz*dz;

        // submesh ごとにマテリアルスロットを引き、そのスロットのブレンドモードで
        // 不透明キュー / 半透明キューへ振り分ける。
        // WHY: 1 モデル内に不透明ボディと半透明バイザーが混在するのが普通のため、
        //      オブジェクト単位で振り分けると片方が必ず誤ったキューへ入る。
        // mi は「この Renderer の中での」スロット番号。model->meshes の添字とは
        // 一致しないことがあるため (submeshIndices)、メッシュは必ずアクセサから引く。
        const size_t meshCount = smr->SubmeshCount();
        for (size_t mi = 0; mi < meshCount; ++mi) {
            renderer::Mesh* meshPtr = smr->SubmeshMesh(mi);
            if (!meshPtr) continue;
            if (!meshPtr->vertexBuffer.IsValid() || !meshPtr->indexBuffer.IsValid()) continue;

            auto& slot = mat->SlotAt(mi);
            if (!slot.visible) continue;
            slot.EnsureMaterialAsset();

            if (slot.GetBlendMode() == renderer::BlendMode::OPAQUE_BLEND) {
                opaqueSkinnedQueue.push_back({ &go, smr, mat, anim, distSq, mi });
                continue;
            }

            auto* material = SyncMaterialSlot(*mat, mi, resources, true);
            if (!material) continue;
            const bool surfaceMisassigned = IsSurfaceMaterial(slot);
            if (surfaceMisassigned)
                LogSkinnedSurfaceFallbackWarningOnce(material->shaderPath);
            renderer::Material* drawMaterial = surfaceMisassigned
                ? GetFallbackMaterial(resources, true)
                : material;
            if (!drawMaterial) continue;
            const auto skinnedShader = drawMaterial->shader;
            if (!skinnedShader.IsValid()) continue;

            PerObjectCB objData{};
            // スキンドメッシュは Socket / Bone Transform と同じ物理ワールドを描画する。
            objData.world             = go.transform.GetWorldMatrix();
            objData.worldInvTranspose = math::Matrix4::InverseTransposeAffine(objData.world);
            const auto skinCB = ResolveSkinningCB(
                anim ? anim->skinningBuffer : decltype(anim->skinningBuffer){},
                smr->model, h.bindPoseSkinningCB);

            renderer::DrawCall dc;
            dc.vertexBuffer       = smr->ResolveSlotVertexBuffer(mi, meshPtr->vertexBuffer);
            dc.indexBuffer        = meshPtr->indexBuffer;
            dc.indexCount         = meshPtr->indexCount;
            dc.vertexCount        = meshPtr->vertexCount;
            dc.shader             = skinnedShader;
            dc.pipelineState      = rs.IsWireframe() ? h.wireframePSO
                                                     : GetOrCreateMaterialPSO(resources, slot.GetBlendMode(), slot.IsDoubleSided());
            dc.layer              = renderer::RenderLayer::TRANSPARENT_LAYER;
            dc.constantBuffers[0] = h.frameCB;
            dc.constantBuffers[1] = h.objectCB;
            dc.constantBuffers[2] = drawMaterial->paramsBuffer;
            dc.constantBuffers[3] = h.lightCB;
            dc.constantBuffers[4] = h.shadowCB;
            dc.constantBuffers[8] = h.advancedGraphicsCB;
            dc.constantBuffers[7] = skinCB;
            BindClusterLighting(dc, ctx);
            for (size_t ti = 0; ti < drawMaterial->textures.size() && ti < 8; ++ti)
                if (drawMaterial->textures[ti].IsValid()) dc.textures[ti] = drawMaterial->textures[ti];
            dc.textures[8] = shadowDepthTex;
            dc.textures[16] = h.iblIrradiance;
            dc.textures[17] = h.iblPrefilter;
            dc.textures[18] = h.iblBrdfLut;
            transparentQueue.push_back({ dc, objData, distSq, slot.GetRenderQueue() });
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
            // RenderQueue は submesh ごとのスロットから引く。
            const int32_t qa = a.mat->SlotAt(a.meshIndex).GetRenderQueue();
            const int32_t qb = b.mat->SlotAt(b.meshIndex).GetRenderQueue();
            if (qa != qb) return qa < qb;
            return a.distSq < b.distSq;
        });

    // =========================================================================
    // Phase 3: オクルージョンカリング + 不透明描画
    // =========================================================================
    // カメラ側で Occlusion Culling を切っている場合はテストも遮蔽者登録も行わない。
    const bool useOcclusion = ctx.occlusionCuller != nullptr && ctx.occlusionCullingEnabled;
    if (useOcclusion)
        ctx.occlusionCuller->Reset(cam);

    // ── 不透明静的メッシュ ─────────────────────────────────────────────────────
    for (auto& entry : opaqueStaticQueue) {
        auto& go  = *entry.go;
        auto* mr  = entry.mr;
        auto* mat = entry.mat;

        auto* material = SyncMaterial(*mat, resources);
        if (!material || !material->shader.IsValid()) continue;

        // オクルージョンカリング: 完全に隠蔽されていれば描画スキップ
        if (useOcclusion) {
            const auto bounds =
                ComputeWorldBounds(go.transform, *mr->mesh, ctx.cullingBoundsPadding);
            if (!ctx.occlusionCuller->TestAndRaster(bounds.center, bounds.radius)) {
                ++ctx.statsOcclusionCulled;
                continue;
            }
        }

        PerObjectCB objData{};
        // スキンドメッシュは Socket / Bone Transform と同じ物理ワールドを描画する。
        objData.world             = go.transform.GetWorldMatrix();
        objData.worldInvTranspose = math::Matrix4::InverseTransposeAffine(objData.world);
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
        dc.constantBuffers[8] = h.advancedGraphicsCB;
        BindClusterLighting(dc, ctx);
        for (size_t ti = 0; ti < material->textures.size() && ti < 8; ++ti)
            if (material->textures[ti].IsValid()) dc.textures[ti] = material->textures[ti];
        dc.textures[8] = shadowDepthTex;
        dc.textures[16] = h.iblIrradiance;
        dc.textures[17] = h.iblPrefilter;
        dc.textures[18] = h.iblBrdfLut;
        SubmitCounted(ctx, dc);
    }

    // ── 不透明スキンドメッシュ ─────────────────────────────────────────────────
    // バインドポーズ球はアニメーション後の実際の姿勢と乖離するため
    // SW オクルージョンカリングは適用しない。
    for (auto& entry : opaqueSkinnedQueue) {
        auto& go   = *entry.go;
        auto* smr  = entry.smr;
        auto* mat  = entry.mat;
        auto* anim = entry.anim;
        const size_t mi = entry.meshIndex;

        renderer::Mesh* meshPtr = smr->SubmeshMesh(mi);
        if (!meshPtr) continue;

        auto& slot = mat->SlotAt(mi);
        slot.EnsureMaterialAsset();
        auto* material = SyncMaterialSlot(*mat, mi, resources, true);
        if (!material) continue;

        const bool surfaceMisassigned = IsSurfaceMaterial(slot);
        if (surfaceMisassigned)
            LogSkinnedSurfaceFallbackWarningOnce(material->shaderPath);
        renderer::Material* drawMaterial = surfaceMisassigned
            ? GetFallbackMaterial(resources, true)
            : material;
        if (!drawMaterial) continue;
        const auto skinnedShader = drawMaterial->shader;
        if (!skinnedShader.IsValid()) continue;

        PerObjectCB objData{};
        // スキンドメッシュは Socket / Bone Transform と同じ物理ワールドを描画する。
        objData.world             = go.transform.GetWorldMatrix();
        objData.worldInvTranspose = math::Matrix4::InverseTransposeAffine(objData.world);
        resources.Update(h.objectCB, &objData, sizeof(PerObjectCB));

        const auto skinCB = ResolveSkinningCB(
            anim ? anim->skinningBuffer : decltype(anim->skinningBuffer){},
            smr->model, h.bindPoseSkinningCB);

        renderer::DrawCall dc;
        dc.vertexBuffer       = smr->ResolveSlotVertexBuffer(mi, meshPtr->vertexBuffer);
        dc.indexBuffer        = meshPtr->indexBuffer;
        dc.indexCount         = meshPtr->indexCount;
        dc.vertexCount        = meshPtr->vertexCount;
        dc.shader             = skinnedShader;
        dc.pipelineState      = rs.IsWireframe() ? h.wireframePSO
                                                 : GetOrCreateMaterialPSO(resources, slot.GetBlendMode(), slot.IsDoubleSided());
        dc.layer              = renderer::RenderLayer::OPAQUE_LAYER;
        dc.constantBuffers[0] = h.frameCB;
        dc.constantBuffers[1] = h.objectCB;
        dc.constantBuffers[2] = drawMaterial->paramsBuffer;
        dc.constantBuffers[3] = h.lightCB;
        dc.constantBuffers[4] = h.shadowCB;
        dc.constantBuffers[7] = skinCB;
        BindClusterLighting(dc, ctx);
        for (size_t ti = 0; ti < drawMaterial->textures.size() && ti < 8; ++ti)
            if (drawMaterial->textures[ti].IsValid()) dc.textures[ti] = drawMaterial->textures[ti];
        dc.textures[8] = shadowDepthTex;
        SubmitCounted(ctx, dc);
    }

    // ── 半透明をソートして Submit ─────────────────────────────────────────────
    SortAndSubmitTransparent(ctx, transparentQueue, h.objectCB);
}

} // namespace fbzz::scene
