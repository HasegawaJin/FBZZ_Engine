// FBZZ Engine
// RenderPasses/DeferredPasses.cpp | fbzz::scene
// Deferred パイプライン: GBuffer / DepthCopy / Lighting / SkinnedForward / ForwardTransparent
#include "GeometryPasses.hpp"
#include "Engine/Scene/Scene.hpp"
#include "Engine/Scene/Transform.hpp"
#include "Engine/Renderer/Mesh.hpp"
#include "Engine/Scene/Components/MeshRenderer.hpp"
#include "Engine/Scene/Components/MaterialComponent.hpp"
#include "Engine/Scene/Components/AnimatorComponent.hpp"
#include "Engine/Scene/Components/SkinnedMeshRenderer.hpp"
#include "Engine/Scene/Components/WaterComponent.hpp"
#include "Engine/Core/Logger.hpp"
#include "Engine/Core/Time.hpp"
#include "Engine/Renderer/DrawCall.hpp"
#include "Engine/Renderer/IShader.hpp"
#include "Engine/Renderer/SamplerMode.hpp"
#include <Math/Matrix4.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
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

bool IsCameraUnderwater(const RenderPassContext& ctx)
{
    const float time = core::Time::TotalTime();
    for (auto [water, transform] : ctx.scene.View<WaterComponent, Transform>()) {
        if (!water.enabled) continue;

        const float localX = ctx.camera.m_position.x - transform.position.x;
        const float localZ = ctx.camera.m_position.z - transform.position.z;
        if (std::abs(localX) > water.extentX * 0.5f || std::abs(localZ) > water.extentZ * 0.5f) {
            continue;
        }

        // WHY: 水中では画面全体の濁りと散乱が支配的になり、SSAO の接触影がノイズに見えやすい。
        //      Deferred 合成時点で強度だけ 0 にして、既存の SSAO パス構成を変えずに見た目を無効化する。
        const float surfaceY = transform.position.y + water.GetSurfaceHeightAt(localX, localZ, time);
        if (ctx.camera.m_position.y < surfaceY) {
            return true;
        }
    }
    return false;
}

} // anonymous namespace

// ── GBuffer パス ──────────────────────────────────────────────────────────────
void ExecuteGBufferPass(RenderPassContext& ctx)
{
    auto& renderer  = ctx.renderer;
    auto& resources = ctx.resources;
    auto& h         = ctx.handles;
    const auto& rs  = ctx.settings;

    renderer.SetRenderTarget(h.gbufferRT, resources);
    renderer.Clear({ 0.0f, 0.0f, 0.0f, 0.0f });

    PerFrameCB frameData{};
    frameData.view              = ctx.camera.GetViewMatrix();
    frameData.projection        = ctx.camera.GetProjectionMatrix();
    frameData.viewProjection    = ctx.camera.GetViewProjection();
    frameData.invViewProjection = math::Matrix4::Inverse(frameData.viewProjection);
    frameData.cameraPos         = ctx.camera.m_position;
    frameData.nearZ             = ctx.camera.m_near;
    frameData.farZ              = ctx.camera.m_far;
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

    if (!h.gbufferShader.IsValid()) { FBZZ_LOG_WARN("GBuffer shader is invalid!"); return; }

    // GBuffer.hlsl はデフォルト 96 バイト MaterialConstants を期待する。
    // マテリアル独自シェーダーは別レイアウトを持つため、フィールド名で値を抽出して
    // GBuffer 互換レイアウトに詰め直す共有 cbuffer を使う。
    // WHY: textureMask のオフセットはシェーダーごとに異なる (PBR=80, Toon=16, RimLight=28 等)。
    //      そのまま渡すと GBuffer が textureMask/roughness/metallic を誤読してライティングが壊れる。
    static renderer::ResourceHandle<renderer::ConstantBufferTag> s_gbufMatCB;
    static constexpr uint32_t kGBufCBSize = 96u;
    if (!s_gbufMatCB.IsValid())
        s_gbufMatCB = resources.CreateConstantBuffer(kGBufCBSize);

    const auto& frustum = *ctx.cameraFrustum;

    for (auto& go : ctx.scene.GameObjects()) {
        if (!ShouldRenderGameObject(go, ctx.cullingMask)) continue;
        auto* mr  = go.GetComponent<MeshRenderer>();
        auto* mat = go.GetComponent<MaterialComponent>();
        if (!mr || !mr->enabled || !mr->mesh || !mat || !mat->EnsureMaterialAsset()) continue;
        if (!mr->mesh->vertexBuffer.IsValid() || !mr->mesh->indexBuffer.IsValid()) continue;
        if (mr->mesh->isSkinned) continue;
        // 半透明・加算マテリアルは GBuffer に書き込まない。フォワードパスで描画する。
        // WHY: GBuffer はアルファブレンドをサポートしない (MRT への書き込みが 1 つの値のため)。
        if (mat->GetBlendMode() != renderer::BlendMode::OPAQUE_BLEND) continue;

        // フラスタムカリング: バウンディング球が視錐台外なら除外
        if (!IsVisibleInFrustum(frustum, go.transform, *mr->mesh)) continue;

        // GBuffer に収まらないエフェクト系シェーダー (RimLight / Toon 等) は
        // DeferredForwardEffects パスで Forward 描画するためここではスキップする。
        if (IsForwardOnly(*mat)) continue;

        auto* material = SyncMaterial(*mat, resources);

        // マテリアル CB を GBuffer 互換レイアウト (96 バイト) に変換してバインドする。
        renderer::ResourceHandle<renderer::ConstantBufferTag> gbufMatCB;
        if (material) {
            if (material->paramData.size() == kGBufCBSize) {
                // デフォルト 96 バイトレイアウト (PBR 等) はそのまま使える。
                gbufMatCB = material->paramsBuffer;
            } else {
                // 独自レイアウト: フィールド名で抽出して 96 バイトバッファに詰め直す。
                std::array<uint8_t, kGBufCBSize> gbufParams{};
                // uvTiling がないマテリアル (Lit/Phong 等) 向けデフォルト: (1,1) で UV そのまま
                // roughness がないマテリアル向けデフォルト: 0.5 (PBR で鏡面にならないよう)
                // alphaCutoff デフォルト: 0 (カットアウト無効)
                static constexpr float kDefTiling[2] = { 1.0f, 1.0f };
                static constexpr float kDefRoughness  = 0.5f;
                std::memcpy(gbufParams.data() + 20u, &kDefRoughness, 4u);
                std::memcpy(gbufParams.data() + 48u, kDefTiling,     8u);
                const renderer::ShaderDescriptor* mDesc = nullptr;
                if (auto* sh = resources.Get(material->shader))
                    mDesc = &sh->GetDescriptor();

                if (mDesc) {
                    auto copyField = [&](std::string_view name, uint32_t dstOff, uint32_t bytes) {
                        const auto* v = mDesc->FindVar(name);
                        if (!v || v->offset + bytes > static_cast<uint32_t>(material->paramData.size())) return;
                        std::memcpy(gbufParams.data() + dstOff,
                                    material->paramData.data() + v->offset, bytes);
                    };
                    copyField("albedo",         0u,  16u); // float4
                    copyField("metallic",       16u,  4u); // float
                    copyField("roughness",      20u,  4u); // float
                    copyField("normalStrength", 24u,  4u); // float
                    copyField("uvTiling",       48u,  8u); // float2
                    copyField("uvOffset",       56u,  8u); // float2
                    copyField("alphaCutoff",    64u,  4u); // float
                    // textureMask は Material::Upload が計算済みの値を使う
                    if (mDesc->textureMaskOffset != UINT32_MAX
                        && mDesc->textureMaskOffset + 4u <= static_cast<uint32_t>(material->paramData.size()))
                    {
                        std::memcpy(gbufParams.data() + 80u,
                                    material->paramData.data() + mDesc->textureMaskOffset, 4u);
                    }
                }
                resources.Update(s_gbufMatCB, gbufParams.data(), kGBufCBSize);
                gbufMatCB = s_gbufMatCB;
            }
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
        dc.shader             = h.gbufferShader;
        dc.pipelineState      = rs.IsWireframe() ? h.wireframePSO : h.defaultPSO;
        dc.constantBuffers[0] = h.frameCB;
        dc.constantBuffers[1] = h.objectCB;
        dc.constantBuffers[2] = gbufMatCB;
        if (material)
            for (size_t ti = 0; ti < material->textures.size() && ti < 8; ++ti)
                if (material->textures[ti].IsValid()) dc.textures[ti] = material->textures[ti];
        renderer.Submit(dc, resources);
    }
}

// ── Deferred 用深度コピー (GBuffer → hdrRT) ──────────────────────────────────
void ExecuteDeferredDepthCopyPass(RenderPassContext& ctx)
{
    auto& renderer  = ctx.renderer;
    auto& resources = ctx.resources;
    auto& h         = ctx.handles;

    // hdrRT をクリア (カラー・深度 1.0 にリセット) してから GBuffer 深度を転写する。
    // この深度は Sky (DEPTH_SKY) と DeferredSkinnedForward (DEPTH_ON) が参照する。
    renderer.SetRenderTarget(h.hdrRT, resources);
    renderer.Clear(kHdrClearColor);

    if (!h.depthCopyShader.IsValid() || !h.gbufferRT.IsValid()) return;

    renderer::DrawCall dc;
    dc.shader        = h.depthCopyShader;
    dc.pipelineState = h.defaultPSO;  // DEPTH_ON: 深度テスト + 書き込みあり
    dc.vertexCount   = 3;
    dc.textures[7]   = resources.GetDepthTexture(h.gbufferRT);  // TEX_DEPTH
    renderer.Submit(dc, resources);
}

// ── Deferred ライティング (フルスクリーン PBR) ────────────────────────────────
void ExecuteDeferredLightingPass(RenderPassContext& ctx)
{
    auto& renderer  = ctx.renderer;
    auto& resources = ctx.resources;
    auto& h         = ctx.handles;
    const auto& rs  = ctx.settings;

    // Sky 色・深度を保持したまま上書きするため SetRenderTarget のみ (Clear しない)。
    renderer.SetRenderTarget(h.hdrRT, resources);
    renderer.SetSampler(0, renderer::SamplerMode::WRAP_ANISOTROPIC);
    renderer.SetSampler(1, renderer::SamplerMode::BORDER_ZERO);

    PostProcCB lightingPostData{};
    lightingPostData.ssaoIntensity = ctx.ssaoEnabled && !IsCameraUnderwater(ctx)
        ? rs.postProcess.ambientOcclusion.intensity
        : 0.0f;
    resources.Update(h.postprocCB, &lightingPostData, sizeof(PostProcCB));

    if (!h.deferredLightingShader.IsValid() || !h.gbufferRT.IsValid()) return;

    renderer::DrawCall dc;
    dc.shader             = h.deferredLightingShader;
    dc.pipelineState      = h.postprocPSO;  // DEPTH_OFF: 深度テスト・書き込みなし
    dc.vertexCount        = 3;
    dc.constantBuffers[0] = h.frameCB;
    dc.constantBuffers[3] = h.lightCB;
    dc.constantBuffers[4] = h.shadowCB;
    dc.textures[5]        = resources.GetColorTexture(h.gbufferRT, 0);  // TEX_GBUFFER0
    dc.textures[6]        = resources.GetColorTexture(h.gbufferRT, 1);  // TEX_GBUFFER1
    dc.textures[7]        = resources.GetDepthTexture(h.gbufferRT);     // TEX_DEPTH
    dc.textures[8]        = resources.GetDepthTexture(h.shadowMapRT);   // TEX_SHADOW
    dc.textures[9]        = ctx.ssaoEnabled
        ? h.ssaoBlur
        : renderer::ResourceHandle<renderer::TextureTag>{};              // TEX_SSAO
    dc.constantBuffers[5] = h.postprocCB;
    renderer.Submit(dc, resources);
}

// ── Deferred 内スキンドメッシュフォワードパス ─────────────────────────────────
void ExecuteDeferredSkinnedForwardPass(RenderPassContext& ctx)
{
    auto& renderer  = ctx.renderer;
    auto& resources = ctx.resources;
    auto& h         = ctx.handles;
    const auto& rs  = ctx.settings;
    const auto& cam = ctx.camera;

    // hdrRT の深度には DeferredDepthCopy で転写した GBuffer 深度が入っているため
    // 静的ジオメトリとの正しいオクルージョンが保たれる。
    renderer.SetRenderTarget(h.hdrRT, resources);
    renderer.SetSampler(0, renderer::SamplerMode::WRAP_ANISOTROPIC);
    renderer.SetSampler(1, renderer::SamplerMode::BORDER_ZERO);

    const auto shadowDepthTex = resources.GetDepthTexture(h.shadowMapRT);

    std::vector<TransparentEntry> transparentQueue;
    const auto& frustum = *ctx.cameraFrustum;

    // 不透明 skinned meshes → 即座に Submit
    for (auto& go : ctx.scene.GameObjects()) {
        if (!ShouldRenderGameObject(go, ctx.cullingMask)) continue;
        auto* smr  = go.GetComponent<SkinnedMeshRenderer>();
        auto* mat  = go.GetComponent<MaterialComponent>();
        auto* anim = go.GetComponent<AnimatorComponent>();
        if (!smr || !smr->enabled || !smr->model) continue;
        if (!mat || !mat->EnsureMaterialAsset()) continue;
        if (mat->GetBlendMode() != renderer::BlendMode::OPAQUE_BLEND) continue;
        if (!IsSkinnedVisibleInFrustum(frustum, go.transform, *smr)) continue;
        auto* material = SyncMaterial(*mat, resources, true);
        if (!material) continue;

        // WHY: Surface 版シェーダー (Material/Surface/) はスキニング処理を持たない VS を使用する。
        //      SkinnedMeshRenderer に誤って設定するとメッシュが破綻するため FallbackSkinned へフォールバックし、
        //      開発者がすぐ気づけるよう警告を出す。
        const bool surfaceMisassigned = material->shader.IsValid() && IsSurfaceMaterial(*mat);
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

        for (const auto& meshPtr : smr->model->meshes) {
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
            for (size_t ti = 0; ti < drawMaterial->textures.size() && ti < 8; ++ti)
                if (drawMaterial->textures[ti].IsValid()) dc.textures[ti] = drawMaterial->textures[ti];
            dc.textures[8] = shadowDepthTex;
            renderer.Submit(dc, resources);
        }
    }

    // 半透明 skinned meshes → キュー蓄積
    for (auto& go : ctx.scene.GameObjects()) {
        if (!ShouldRenderGameObject(go, ctx.cullingMask)) continue;
        auto* smr  = go.GetComponent<SkinnedMeshRenderer>();
        auto* mat  = go.GetComponent<MaterialComponent>();
        auto* anim = go.GetComponent<AnimatorComponent>();
        if (!smr || !smr->enabled || !smr->model) continue;
        if (!mat || !mat->EnsureMaterialAsset()) continue;
        if (mat->GetBlendMode() == renderer::BlendMode::OPAQUE_BLEND) continue;
        if (!IsSkinnedVisibleInFrustum(frustum, go.transform, *smr)) continue;
        auto* material = SyncMaterial(*mat, resources, true);
        if (!material) continue;

        const bool surfaceMisassigned = material->shader.IsValid() && IsSurfaceMaterial(*mat);
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

        for (const auto& meshPtr : smr->model->meshes) {
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

            const float dx = objData.world.m[0][3] - cam.m_position.x;
            const float dy = objData.world.m[1][3] - cam.m_position.y;
            const float dz = objData.world.m[2][3] - cam.m_position.z;
            transparentQueue.push_back({ dc, objData, dx*dx + dy*dy + dz*dz, mat->GetRenderQueue() });
        }
    }

    SortAndSubmitTransparent(transparentQueue, renderer, resources, h.objectCB);
}

// ── Deferred 内透明 Static Mesh フォワードパス ───────────────────────────────
void ExecuteDeferredForwardTransparentPass(RenderPassContext& ctx)
{
    auto& renderer  = ctx.renderer;
    auto& resources = ctx.resources;
    auto& h         = ctx.handles;
    const auto& rs  = ctx.settings;
    const auto& cam = ctx.camera;

    // 透明 Static Mesh を Deferred パイプラインのフォワードパスで描画する。
    // DeferredDepthCopy で転写済みの GBuffer 深度を利用するため、静的不透明ジオメトリとの
    // 正しいオクルージョンが保たれる。
    // WHY: GBuffer はアルファブレンドを扱えないため、透明 Static Mesh は
    //      Deferred ライティング後に別途フォワードで描画する必要がある。
    renderer.SetRenderTarget(h.hdrRT, resources);
    renderer.SetSampler(0, renderer::SamplerMode::WRAP_ANISOTROPIC);
    renderer.SetSampler(1, renderer::SamplerMode::BORDER_ZERO);

    const auto shadowDepthTex = resources.GetDepthTexture(h.shadowMapRT);

    std::vector<TransparentEntry> transparentQueue;

    const auto& frustumTransp = *ctx.cameraFrustum;

    for (auto& go : ctx.scene.GameObjects()) {
        if (!ShouldRenderGameObject(go, ctx.cullingMask)) continue;
        auto* mr  = go.GetComponent<MeshRenderer>();
        auto* mat = go.GetComponent<MaterialComponent>();
        if (!mr || !mr->enabled || !mr->mesh || !mat || !mat->EnsureMaterialAsset()) continue;
        if (!mr->mesh->vertexBuffer.IsValid() || !mr->mesh->indexBuffer.IsValid()) continue;
        if (mr->mesh->isSkinned) continue;
        if (mat->GetBlendMode() == renderer::BlendMode::OPAQUE_BLEND) continue;  // 透明のみ

        // フラスタムカリング: バウンディング球が視錐台外なら除外
        if (!IsVisibleInFrustum(frustumTransp, go.transform, *mr->mesh)) continue;

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

        const float dx = objData.world.m[0][3] - cam.m_position.x;
        const float dy = objData.world.m[1][3] - cam.m_position.y;
        const float dz = objData.world.m[2][3] - cam.m_position.z;
        transparentQueue.push_back({ dc, objData, dx*dx + dy*dy + dz*dz, mat->GetRenderQueue() });
    }

    SortAndSubmitTransparent(transparentQueue, renderer, resources, h.objectCB);

    // ── 不透明エフェクト系 Static Mesh (RimLight / Toon / Subsurface 等) ──────────
    // GBuffer に収まらない独自ライティングを持つ不透明シェーダーを Forward で描画する。
    // GBufferPass でスキップされたオブジェクトをここで処理する。
    // DeferredDepthCopy で転写済みの深度を使って GBuffer ジオメトリとの
    // 正しいオクルージョンを保ちながら描画する。
    for (auto& go : ctx.scene.GameObjects()) {
        if (!ShouldRenderGameObject(go, ctx.cullingMask)) continue;
        auto* mr  = go.GetComponent<MeshRenderer>();
        auto* mat = go.GetComponent<MaterialComponent>();
        if (!mr || !mr->enabled || !mr->mesh || !mat || !mat->EnsureMaterialAsset()) continue;
        if (!mr->mesh->vertexBuffer.IsValid() || !mr->mesh->indexBuffer.IsValid()) continue;
        if (mr->mesh->isSkinned) continue;
        if (mat->GetBlendMode() != renderer::BlendMode::OPAQUE_BLEND) continue;  // 不透明のみ
        if (!IsForwardOnly(*mat)) continue;                                        // エフェクト系のみ

        if (!IsVisibleInFrustum(frustumTransp, go.transform, *mr->mesh)) continue;

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
    }
}

} // namespace fbzz::scene
