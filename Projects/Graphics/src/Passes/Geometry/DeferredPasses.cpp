/// @file    DeferredPasses.cpp
/// @brief   抽出済み RenderScene の GBuffer・照明・Forward 合成。
/// @author  Hasegawa Jin
/// @date    2026-06-18
#include "RenderScenePassHelpers.hpp"
#include <Graphics/Passes/Geometry/FiberRenderPass.hpp>
#include <Graphics/Pipeline/InstanceBatch.hpp>
#include <Core/Logger.hpp>
#include <algorithm>
#include <vector>

namespace fbzz::renderer {
namespace {
struct MeshEntry {
    const renderer::RenderObject* object;
    const renderer::RenderMeshItem* item;
    float distanceSquared;
};

}

/// @name GBuffer パス
void ExecuteGBufferPass(RenderPassContext& ctx)
{
    auto& resources = ctx.resources;
    auto& h = ctx.handles;
    const auto& input = SceneInput(ctx);
    ctx.renderer.SetRenderTarget(ctx.Res().Target("GBuffer"), resources);
    ctx.renderer.Clear({ 0.0f, 0.0f, 0.0f, 0.0f });
    const auto frame = MakeCameraFrameCB(ctx.camera, ctx.taaJitterNdcX, ctx.taaJitterNdcY);
    resources.Update(h.frameCB, &frame, sizeof(frame));
    resources.Update(h.lightCB, &ctx.lightData, sizeof(renderer::LightConstantsCB));
    UpdateShadowConstants(ctx);
    UpdatePunctualShadowConstants(ctx);
    if (!h.gbufferShader.IsValid()) return;
    auto& materialCB = h.gbufferMaterialCB;
    if (!resources.Get(materialCB)) materialCB = resources.CreateConstantBuffer(96);
    if (!resources.Get(materialCB)) return;
    for (bool skinned : { false, true }) {
        std::vector<MeshEntry> queue;
        for (const auto& object : input.objects) {
            if (object.skinned != skinned || !object.colorEligible || !MatchesView(ctx, object)) continue;
            if (!skinned) {
                const auto& item = input.items[object.firstItem];
                if (!HasColorGeometry(item) || renderer::ResolveGeometryRoute(item.material.capabilities, true)
                    != renderer::GeometryRoute::GBuffer) continue;
                ++ctx.statsTotalObjects;
            }
            if (!IsRenderObjectVisible(ctx, object)) continue;
            for (uint32_t i = object.firstItem; i < object.firstItem + object.itemCount; ++i) {
                const auto& item = input.items[i];
                if (!item.visible || !item.indexBuffer.IsValid() ||
                    renderer::ResolveGeometryRoute(item.material.capabilities, true) != renderer::GeometryRoute::GBuffer) continue;
                if (skinned && (!item.material.valid || (!item.deformedVertexBuffer.IsValid() &&
                    (!item.skinningVertexBuffer.IsValid() || !h.gbufferSkinnedShader.IsValid())))) continue;
                queue.push_back({ &object, &item, (object.sortPosition - ctx.camera.m_position).LengthSq() });
            }
        }
        if (!skinned) {
            std::sort(queue.begin(), queue.end(), [](const MeshEntry& a, const MeshEntry& b) {
                return a.distanceSquared < b.distanceSquared;
            });
            if (ctx.occlusionCuller && ctx.occlusionCullingEnabled) {
                ctx.occlusionCuller->Reset(ctx.camera);
                auto end = queue.begin();
                for (const auto& entry : queue) {
                    const auto bounds = RenderBounds(ctx, *entry.object);
                    if (!ctx.occlusionCuller->TestAndRaster(bounds.center, bounds.radius, entry.item->reliableOccluder)) {
                        ++ctx.statsOcclusionCulled;
                        continue;
                    }
                    *end++ = entry;
                }
                queue.erase(end, queue.end());
            }
        }
        /// @note GPU 資源の世代付きハンドルをキーに束ねる。Scene の実体アドレスへ依存しない。
        std::stable_sort(queue.begin(), queue.end(), [](const MeshEntry& a, const MeshEntry& b) {
            const auto ma = a.item->material.paramsBuffer;
            const auto mb = b.item->material.paramsBuffer;
            if (ma.id != mb.id) return ma.id < mb.id;
            if (ma.gen != mb.gen) return ma.gen < mb.gen;
            return a.item->vertexBuffer.id < b.item->vertexBuffer.id;
        });
        InstanceBatcher batcher(ctx, h.gbufferShader,
            ctx.settings.gpuInstancing ? h.gbufferInstancedShader : renderer::ResourceHandle<renderer::ShaderTag>{}, false);
        for (const auto& entry : queue) {
            const auto& item = *entry.item;
            const auto& material = item.material;
            renderer::DrawCall dc;
            dc.vertexBuffer = skinned ? (item.deformedVertexBuffer.IsValid()
                ? item.deformedVertexBuffer : item.skinningVertexBuffer) : item.vertexBuffer;
            dc.indexBuffer = item.indexBuffer;
            dc.indexCount = item.indexCount;
            dc.vertexCount = item.vertexCount;
            const bool vertexSkinning = skinned && !item.deformedVertexBuffer.IsValid();
            dc.shader = vertexSkinning ? h.gbufferSkinnedShader : h.gbufferShader;
            dc.pipelineState = MaterialPipeline(ctx, material);
            dc.constantBuffers[0] = h.frameCB;
            if (material.valid) {
                const auto params = MakeHybridGBufferParams(material.gbufferParams, material.surface);
                if (material.directGBufferParams && params == material.gbufferParams)
                    dc.constantBuffers[2] = material.paramsBuffer;
                else {
                    /// @note Typed marker changes use an immutable draw snapshot, never mutate the asset's material buffer or a pending instance batch.
                    batcher.Flush();
                    resources.Update(materialCB, params.data(), params.size());
                    dc.constantBuffers[2] = materialCB;
                }
            }
            if (vertexSkinning) dc.constantBuffers[7] = entry.object->skinningPalette;
            dc.constantBuffers[8] = h.advancedGraphicsCB;
            for (size_t i = 0; i < material.textures.size(); ++i) dc.textures[i] = material.textures[i];
            batcher.Add(dc, ObjectConstants(*entry.object));
        }
        batcher.Flush();
    }
}

void ExecuteDeferredDepthCopyPass(RenderPassContext& ctx)
{
    auto& renderer  = ctx.renderer;
    auto& resources = ctx.resources;
    auto& h         = ctx.handles;

    /// @note hdrRT をクリア (カラー・深度を最遠 = Reversed-Z の 0 にリセット) してから GBuffer 深度を転写する。
    /// @note この深度は Sky (DEPTH_SKY) と DeferredSkinnedForward (DEPTH_ON) が参照する。
    renderer.SetRenderTarget(ctx.Res().Target("HDR"), resources);
    ClearForCamera(renderer, ctx.camera);

    if (!h.depthCopyShader.IsValid() || !ctx.Res().Target("GBuffer").IsValid()) return;

    renderer::DrawCall dc;
    dc.shader        = h.depthCopyShader;
    /// @note DEPTH_ON: 深度テスト + 書き込みあり
    dc.pipelineState = h.defaultPSO;
    dc.vertexCount   = 3;
    /// @note TEX_DEPTH
    dc.textures[7]   = resources.GetDepthTexture(ctx.Res().Target("GBuffer"));
    renderer.Submit(dc, resources);
}

/// @name Deferred ライティング (フルスクリーン PBR)
void ExecuteDeferredLightingPass(RenderPassContext& ctx)
{
    auto& renderer  = ctx.renderer;
    auto& resources = ctx.resources;
    auto& h         = ctx.handles;
    const auto& rs  = ctx.settings;

    /// @note Sky 色・深度を保持したまま上書きするため SetRenderTarget のみ (Clear しない)。
    renderer.SetRenderTarget(ctx.Res().Target("HDR"), resources);
    /// @note AO 入力の選択: GTAO が有効ならそれを、無ければ SSAO を DeferredLighting の AO として供給する。DeferredLighting は AO テクスチャ (TEX_SSAO=t9) を ssaoIntensity>0 のとき乗算する 1 経路設計のため、GTAO もこの共通経路に流し込む。
    /// @note GTAO.cs は出力に gtaoIntensity を織り込み済みのため、二重適用を避けて ssaoIntensity=1.0 で「そのまま乗算」する。SSAO は raw 出力なので intensity をここで適用する。
    const bool underwater = ctx.environment.cameraUnderwater;
    const bool gtaoActive = rs.IsGtaoActive() && ctx.Res().Texture("GTAOResult").IsValid();
    renderer::ResourceHandle<renderer::TextureTag> aoTex{};
    float aoIntensity = 0.0f;
    if (gtaoActive && !underwater) {
        aoTex       = ctx.Res().Texture("GTAOResult");
        /// @note GTAO 出力は intensity 適用済み
        aoIntensity = 1.0f;
    } else if (ctx.ssaoEnabled && !underwater) {
        aoTex       = ctx.Res().Texture("SSAO");
        aoIntensity = rs.postProcess.ambientOcclusion.intensity;
    }

    PostProcCB lightingPostData{};
    lightingPostData.ssaoIntensity = aoIntensity;
    const bool rayReflectionActive = !ctx.hybridReflectionSourcePass && ctx.rayReflectionPassActive
        && resources.Get(ctx.handles.rayReflectionResult);
    lightingPostData.rayReflectionEnabled = rayReflectionActive ? 1.0f : 0.0f;
    const bool reflectionResolveActive = ctx.hybridReflectionResolveActive && !ctx.hybridReflectionSourcePass;
    const bool reflectionSsrActive = reflectionResolveActive && ctx.ssrPassActive
        && resources.Get(h.ssrResult);
    lightingPostData.reflectionResolveEnabled = static_cast<float>(ctx.hybridReflectionSourcePass
        ? ReflectionResolveStage::SOURCE : (reflectionResolveActive
            ? ReflectionResolveStage::FINAL : ReflectionResolveStage::LEGACY));
    lightingPostData.reflectionSsrEnabled = reflectionSsrActive ? 1.0f : 0.0f;
    resources.Update(h.postprocCB, &lightingPostData, sizeof(PostProcCB));

    if (!h.deferredLightingShader.IsValid() || !ctx.Res().Target("GBuffer").IsValid()) return;

    renderer::DrawCall dc;
    dc.shader             = h.deferredLightingShader;
    /// @note DEPTH_OFF: 深度テスト・書き込みなし
    dc.pipelineState      = h.postprocPSO;
    dc.vertexCount        = 3;
    dc.constantBuffers[0] = h.frameCB;
    dc.constantBuffers[3] = h.lightCB;
    dc.constantBuffers[4] = h.shadowCB;
    dc.constantBuffers[5] = h.postprocCB;
    /// @note b8: iblIntensity など IBL パラメータを含む AdvancedGraphicsCB
    /// @note iblIntensity == 0.0 のとき HLSL は IBL テクスチャをサンプルせず fallback ambient を使う
    dc.constantBuffers[8] = h.advancedGraphicsCB;
    /// @note b9 + t29/t30: クラスタライティング。Legacy モードのときは束縛しない。b9 が未束縛だと clusterLightMode が 0 (= Legacy) として読まれ、シェーダーは b3 の固定長配列へフォールバックするため従来と完全に同じ経路になる。
    if (ctx.clusterLightMode != ClusterLightMode::Legacy) {
        dc.constantBuffers[9] = h.clusterCB;
        /// @note t29
        dc.psBuffers[0]       = h.punctualLightBuffer;
        /// @note クラスタリストは Clustered モードでしか読まれない (Linear は全数走査)。
        if (ctx.clusterLightMode == ClusterLightMode::Clustered)
            /// @note t30
            dc.psBuffers[1]   = h.clusterIndexBuffer;
    }
    /// @note b12 + t28 + t31: Spot / Point のシャドウと Cookie。
    /// @note 供給経路 (Legacy / Clustered) によらず束縛する。
    if (h.punctualShadowCB.IsValid()) {
        dc.constantBuffers[12] = h.punctualShadowCB;
        dc.textures[28]        = resources.GetDepthTexture(ctx.Res().Target("PunctualShadowMap"));
        dc.textures[31]        = resources.GetColorTexture(ctx.Res().Target("LightCookieAtlas"), 0);
    }
    /// @note TEX_GBUFFER0
    dc.textures[5]        = resources.GetColorTexture(ctx.Res().Target("GBuffer"), 0);
    /// @note TEX_GBUFFER1
    dc.textures[6]        = resources.GetColorTexture(ctx.Res().Target("GBuffer"), 1);
    /// @note Deferred の t3 は材質の emissive texture ではなく、GBuffer に保持した線形 HDR emission。
    dc.textures[3]        = resources.GetColorTexture(ctx.Res().Target("GBuffer"), 2);
    /// @note TEX_DEPTH
    dc.textures[7]        = resources.GetDepthTexture(ctx.Res().Target("GBuffer"));
    /// @note TEX_SHADOW
    dc.textures[8]        = resources.GetDepthTexture(ctx.Res().Target("ShadowMap"));
    /// @note TEX_SSAO スロット: GTAO(優先) or SSAO の AO テクスチャ
    dc.textures[9]        = aoTex;
    /// @note TEX_CONTACT_SHADOW: 有効時のみ接触影マスクを供給。無効時は未バインド (シェーダーが
    /// @note contactShadowStrength>0 のガードでサンプルを回避する)。
    dc.textures[24]       = (rs.contactShadow.enabled && ctx.Res().Texture("ContactShadowResult").IsValid())
        ? ctx.Res().Texture("ContactShadowResult")
        /// @note TEX_CONTACT_SHADOW
        : renderer::ResourceHandle<renderer::TextureTag>{};
    /// @note TEX_IBL_IRRADIANCE: 拡散 IBL キューブマップ
    dc.textures[16]       = h.iblIrradiance;
    /// @note TEX_IBL_PREFILTER:  鏡面 IBL キューブマップ
    dc.textures[17]       = h.iblPrefilter;
    /// @note TEX_IBL_BRDF_LUT:   BRDF 積分テーブル
    dc.textures[18]       = h.iblBrdfLut;
    /// @note t20 はこのパスと Composite のレイ反射用。無効時は b5 の gate で読まない。
    if (rayReflectionActive) dc.textures[20] = ctx.handles.rayReflectionResult;
    if (reflectionSsrActive) dc.textures[23] = h.ssrResult;
    /// @note TEX_LIGHT_PROBE_SH / _OUTER: 無効ハンドルなら未束縛 (b8 の probeVolumes[i].intensity が 0 で引かない)
    dc.textures[22]       = h.lightProbeSH[0];
    dc.textures[21]       = h.lightProbeSH[1];
    renderer.Submit(dc, resources);
}

/// @name Deferred 内スキンドメッシュフォワードパス
namespace {
void SubmitDeferredForward(RenderPassContext& ctx, bool skinned, renderer::GeometryRoute route)
{
    const auto& input = SceneInput(ctx);
    std::vector<MeshEntry> queue;
    for (const auto& object : input.objects) {
        if (object.skinned != skinned || !object.colorEligible || !MatchesView(ctx, object)) continue;
        if (!skinned) {
            const auto& item = input.items[object.firstItem];
            if (!HasColorGeometry(item) || renderer::ResolveGeometryRoute(item.material.capabilities, true) != route) continue;
        }
        if (skinned || route == renderer::GeometryRoute::ForwardTransparent) ++ctx.statsTotalObjects;
        if (!IsRenderObjectVisible(ctx, object)) continue;
        const math::Vector3 position{ object.world.m[0][3], object.world.m[1][3], object.world.m[2][3] };
        for (uint32_t i = object.firstItem; i < object.firstItem + object.itemCount; ++i) {
            const auto& item = input.items[i];
            const auto itemRoute = renderer::ResolveGeometryRoute(item.material.capabilities, true);
            if (!HasColorGeometry(item) || !item.forwardMaterial.valid || !item.forwardMaterial.shader.IsValid()) continue;
            if (skinned ? itemRoute == renderer::GeometryRoute::GBuffer : itemRoute != route) continue;
            const auto constants = ObjectConstants(object);
            if (itemRoute == renderer::GeometryRoute::ForwardOpaque) {
                ctx.resources.Update(ctx.handles.objectCB, &constants, sizeof(constants));
                SubmitCounted(ctx, ForwardDraw(ctx, object, item));
            } else queue.push_back({ &object, &item, (position - ctx.camera.m_position).LengthSq() });
        }
    }
    std::sort(queue.begin(), queue.end(), [](const MeshEntry& a, const MeshEntry& b) {
        if (a.item->material.renderQueue != b.item->material.renderQueue)
            return a.item->material.renderQueue < b.item->material.renderQueue;
        return a.distanceSquared > b.distanceSquared;
    });
    for (const auto& entry : queue) {
        const auto constants = ObjectConstants(*entry.object);
        ctx.resources.Update(ctx.handles.objectCB, &constants, sizeof(constants));
        SubmitCounted(ctx, ForwardDraw(ctx, *entry.object, *entry.item));
    }
}
}

void ExecuteDeferredSkinnedForwardPass(RenderPassContext& ctx)
{
    ctx.renderer.SetRenderTarget(ctx.Res().Target("HDR"), ctx.resources);
    SubmitDeferredForward(ctx, true, renderer::GeometryRoute::ForwardOpaque);
}

void ExecuteDeferredForwardTransparentPass(RenderPassContext& ctx)
{
    ctx.renderer.SetRenderTarget(ctx.Res().Target("HDR"), ctx.resources);
    SubmitDeferredForward(ctx, false, renderer::GeometryRoute::ForwardTransparent);
    SubmitDeferredForward(ctx, false, renderer::GeometryRoute::ForwardOpaque);
}

std::string_view GBufferPass::Name() const
{
    return m_mode == GBufferPassMode::DepthNormalPrepass ? "DepthNormalPrepass" : "DeferredGBuffer";
}

void GBufferPass::Setup(PassBuilder& builder, const RenderPassContext&) const
{
    /// @note Forward のプリパスだけが影と Cookie を読む。Deferred 本経路は
    /// @note ライティングをしないので、法線・深度・roughness を書くだけ。
    if (m_mode == GBufferPassMode::DepthNormalPrepass)
        builder.Read("ShadowMap").Read("PunctualShadowMap").Read("LightCookieAtlas");
    builder.Write("GBuffer");
}

void GBufferPass::Execute(PassResources&, RenderPassContext& ctx)
{
    /// @note Forward の前段は ForwardOpaque と同じ物体を同じカメラで判定し直すので、カリング統計を数えない (数えると倍になる)。
    std::optional<CullStatsRollback> rollback;
    if (m_mode == GBufferPassMode::DepthNormalPrepass) rollback.emplace(ctx);
    ExecuteGBufferPass(ctx);
    ExecuteFiberGBufferPass(ctx);
}

void DeferredDepthCopyPass::Setup(PassBuilder& builder, const RenderPassContext&) const
{
    builder.Read("GBuffer").Write("HDR");
}

void DeferredDepthCopyPass::Execute(PassResources&, RenderPassContext& ctx)
{
    ExecuteDeferredDepthCopyPass(ctx);
}

void DeferredSkinnedForwardPass::Setup(PassBuilder& builder, const RenderPassContext&) const
{
    builder.Read("ShadowMap").Read("PunctualShadowMap").Read("LightCookieAtlas")
           .ReadWrite("HDR");
}

void DeferredSkinnedForwardPass::Execute(PassResources&, RenderPassContext& ctx)
{
    ExecuteDeferredSkinnedForwardPass(ctx);
}

void DeferredForwardTransparentPass::Setup(PassBuilder& builder, const RenderPassContext&) const
{
    builder.Read("ShadowMap").Read("PunctualShadowMap").Read("LightCookieAtlas")
           .ReadWrite("HDR");
}

void DeferredForwardTransparentPass::Execute(PassResources&, RenderPassContext& ctx)
{
    ExecuteDeferredForwardTransparentPass(ctx);
}

void DeferredLightingPass::Setup(PassBuilder& builder, const RenderPassContext& ctx) const
{
    builder.Read("GBuffer").ReadWrite("HDR")
           .Read("ShadowMap").Read("PunctualShadowMap").Read("LightCookieAtlas");
    DeclareScreenSpaceOcclusionReads(builder, ctx);
    if (!m_reflectionSource && ctx.rayReflectionPassActive)
        builder.Read("RayReflectionResult", RenderGraph::ResourceAccessPurpose::SHADER_READ);
    if (!m_reflectionSource && ctx.rayReflectionReconstructionPrepared)
        builder.Read("RayReflectionRaw", RenderGraph::ResourceAccessPurpose::SHADER_READ);
    if (!m_reflectionSource && ctx.hybridReflectionSsrPlanned)
        builder.Read("SSRResult", RenderGraph::ResourceAccessPurpose::SHADER_READ);
}

void DeferredLightingPass::Execute(PassResources&, RenderPassContext& ctx)
{
    const bool previousSourcePass = ctx.hybridReflectionSourcePass;
    ctx.hybridReflectionSourcePass = m_reflectionSource;
    ExecuteDeferredLightingPass(ctx);
    ctx.hybridReflectionSourcePass = previousSourcePass;
}
}
