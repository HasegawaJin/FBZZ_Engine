/// @file    DeferredPasses.cpp
/// @brief   抽出済み RenderScene の GBuffer・照明・Forward 合成。
/// @author  Hasegawa Jin
/// @date    2026-06-18
#include "RenderScenePassHelpers.hpp"
#include <Graphics/Pipeline/GpuHiZ.hpp>
#include <Graphics/Effects/ComputedSkinningOutput.hpp>
#include <Graphics/Passes/Geometry/FiberRenderPass.hpp>
#include <Graphics/Pipeline/MeshletDrawHelpers.hpp>
#include <Graphics/Pipeline/InstanceBatch.hpp>
#include <Graphics/Passes/Geometry/InstanceRenderPasses.hpp>
#include <Core/Logger.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>
#include <optional>
#include <tuple>

namespace fbzz::renderer {
namespace {
struct MeshEntry {
    const renderer::RenderObject* object;
    const renderer::RenderMeshItem* item;
    float distanceSquared;
    std::array<uint8_t, 96> params{};
};

}

/// @name GBuffer パス
void ExecuteGBufferPass(RenderPassContext& ctx)
{
    auto& resources = ctx.resources;
    auto& h = ctx.handles;
    const auto sceneOwner = ctx.renderScene;
    const auto& input = sceneOwner ? *sceneOwner : SceneInput(ctx);
    const auto arguments = ctx.isDeferred && ctx.gpuOcclusionEnabled
        ? ctx.Res().StructuredBuffer(OCCLUSION_DRAW_ARGUMENTS) : ResourceHandle<StructuredBufferTag>{};
    ctx.renderer.SetRenderTarget(ctx.Res().Target("GBuffer"), resources);
    ctx.renderer.Clear({ 0.0f, 0.0f, 0.0f, 0.0f });
    const auto frame = MakeCameraFrameCB(ctx.camera, ctx.taaJitterNdcX, ctx.taaJitterNdcY);
    MeshletClusterView clusterView{};
    GpuHiZ clusterHierarchy;
    if (ctx.settings.geometryDrawMode == GeometryDrawMode::MESH_SHADER) {
        clusterView = {ctx.camera.GetGpuProjectionMatrix() * frame.view, ctx.width, ctx.height};
        if (ctx.isDeferred) clusterHierarchy = ReadViewGpuHiZ(ctx);
    }
    resources.Update(h.frameCB, &frame, sizeof(frame));
    resources.Update(h.lightCB, &ctx.lightData, sizeof(renderer::LightConstantsCB));
    UpdateShadowConstants(ctx);
    UpdatePunctualShadowConstants(ctx);
    if (!h.gbufferShader.IsValid()) return;
    auto& materialCB = h.gbufferMaterialCB;
    if (!resources.Get(materialCB)) materialCB = resources.CreateConstantBuffer(96);
    if (!resources.Get(materialCB)) return;
    /// @note Sorted value groups share b2; flush the previous draws before updating the view-owned fallback, whose queued update owns its bytes.
    /// @note Native Submit snapshots CB data into its upload slice before later ordered updates; material-slot buffers remain untouched.
    /// @see Docs/design/runtime-workset-optimization.md GBuffer材質の同値共有。
    std::array<uint8_t, 96> boundParameters{};
    renderer::ResourceHandle<renderer::ConstantBufferTag> parameterBuffer;
    for (bool skinned : { false, true }) {
        std::vector<MeshEntry> queue;
        for (const auto& object : input.Objects()) {
            if (object.skinned != skinned || !object.colorEligible || !MatchesView(ctx, object)) continue;
            if (!skinned) {
                const auto& item = input.items[object.firstItem];
                if (!HasColorGeometry(item) || renderer::ResolveGeometryRoute(item.material.capabilities, true)
                    != renderer::GeometryRoute::GBuffer) continue;
                ++ctx.statsTotalObjects;
            }
            const bool gpuFrustum = !arguments.IsValid() && !skinned && object.boundsRadius > 0 && object.lodDither == 0
                && (ctx.settings.geometryDrawMode == GeometryDrawMode::GPU_INDIRECT
                    || ctx.settings.geometryDrawMode == GeometryDrawMode::GPU_INSTANCE_INDIRECT) && ctx.settings.gpuInstancing
                && ctx.renderer.SupportsIndirectDraw() && resources.Get(h.gbufferInstancedShader);
            if (!IsRenderObjectVisible(ctx, object, gpuFrustum)) continue;
            for (uint32_t i = object.firstItem; i < object.firstItem + object.itemCount; ++i) {
                const auto& item = input.items[i];
                if (!item.visible || !item.indexBuffer.IsValid() ||
                    renderer::ResolveGeometryRoute(item.material.capabilities, true) != renderer::GeometryRoute::GBuffer) continue;
                if (skinned && (!item.material.valid || (!item.preparedSkinning && !item.deformedVertexBuffer.IsValid() &&
                    (!item.skinningVertexBuffer.IsValid() || !h.gbufferSkinnedShader.IsValid())))) continue;
                queue.push_back({ &object, &item, (object.sortPosition - ctx.camera.m_position).LengthSq(),
                    MakeHybridGBufferParams(item.material.gbufferParams, item.material.surface) });
            }
        }
        if (!skinned) {
            std::sort(queue.begin(), queue.end(), [](const MeshEntry& a, const MeshEntry& b) {
                return a.distanceSquared < b.distanceSquared;
            });
        }
        /// @note Opaque material sorting uses final canonical values, so identical per-object buffers do not split one material group; DrawCall still validates every binding.
        std::stable_sort(queue.begin(), queue.end(), [](const MeshEntry& a, const MeshEntry& b) {
            if (a.params != b.params) return a.params < b.params;
            return a.item->vertexBuffer.id < b.item->vertexBuffer.id;
        });
        const auto gpuFrustum = math::Frustum::FromViewProjection(frame.viewProjection);
        InstanceBatcher batcher(ctx, h.gbufferShader,
            ctx.settings.gpuInstancing ? h.gbufferInstancedShader : renderer::ResourceHandle<renderer::ShaderTag>{}, false, &gpuFrustum);
        for (const auto& entry : queue) {
            const auto& item = *entry.item;
            const auto& material = item.material;
            renderer::DrawCall dc;
            dc.vertexBuffer = skinned ? (item.deformedVertexBuffer.IsValid()
                ? item.deformedVertexBuffer : item.skinningVertexBuffer) : item.vertexBuffer;
            if (skinned && !BindPreparedSkinningOutput(item, dc)) continue;
            dc.indexBuffer = item.indexBuffer;
            dc.indexCount = item.indexCount;
            dc.vertexCount = item.vertexCount;
            const bool vertexSkinning = skinned && !item.preparedSkinning && !item.deformedVertexBuffer.IsValid();
            dc.shader = vertexSkinning ? h.gbufferSkinnedShader : h.gbufferShader;
            if (skinned && item.deformedVertexBuffer.IsValid()) dc.deformationSource = item.skinningVertexBuffer;
            dc.pipelineState = MaterialPipeline(ctx, material);
            dc.constantBuffers[0] = h.frameCB;
            if (material.valid) {
                const auto& params = entry.params;
                if (!parameterBuffer.IsValid() || params != boundParameters) {
                    batcher.Flush();
                    parameterBuffer = material.directGBufferParams && params == material.gbufferParams
                        && resources.Get(material.paramsBuffer) ? material.paramsBuffer
                        : renderer::ResourceHandle<renderer::ConstantBufferTag>{};
                    if (!parameterBuffer.IsValid()) {
                        resources.Update(materialCB, params.data(), params.size());
                        parameterBuffer = materialCB;
                    }
                    boundParameters = params;
                }
                dc.constantBuffers[2] = parameterBuffer;
            }
            if (vertexSkinning) { dc.constantBuffers[7] = entry.object->skinningPalette; BindGpuBonePalette(dc, *entry.object); }
            if (entry.object->leafWindBoundsPadding > 0.0f) dc.constantBuffers[5] = MeshletWindFrame(ctx);
            dc.constantBuffers[8] = h.advancedGraphicsCB;
            for (size_t i = 0; i < material.textures.size(); ++i) dc.textures[i] = material.textures[i];

            const size_t itemIndex = &item - input.items.data();
            if (arguments.IsValid() && itemIndex < ctx.gpuOcclusionArguments.size()
                && ctx.gpuOcclusionArguments[itemIndex] != UINT32_MAX) {
                batcher.Flush();
                const auto constants = ObjectConstants(*entry.object);
                resources.Update(h.objectCB, &constants, sizeof(constants));
                dc.constantBuffers[1] = h.objectCB;
                (void)BindMeshOcclusionArguments(ctx, input, itemIndex, arguments, dc);
                SubmitCounted(ctx, dc);
                batcher.InvalidateObjectConstants();
                continue;
            }
            /// @note A seed item must keep its indexed surface even when mesh selection failed and instances still use Hi-Z.
            const bool seeded = ctx.gpuOccluderDepthReady && ctx.renderScene.get() == &input
                && itemIndex < ctx.gpuOccluderItems.size() && ctx.gpuOccluderItems[itemIndex];
            if (!seeded && ctx.settings.geometryDrawMode == GeometryDrawMode::MESH_SHADER) {
                renderer::DrawCall meshCall = dc;
                const bool clusterEligible = CanUseGBufferClusterLod(*entry.object, item);
                auto objectClusterView = clusterView;
                if (skinned) objectClusterView.skinDisplacement = entry.object->skinClusterDisplacement;
                if (TryPrepareMeshletDraw(resources, ctx.renderer, frame.viewProjection,
                    entry.object->world, MeshletDrawPass::GBUFFER, item, skinned, meshCall,
                    ctx.frustumCullingEnabled, false, clusterEligible ? &objectClusterView : nullptr, &clusterHierarchy)) {
                    batcher.Flush();
                    const renderer::PerObjectCB objectCB = ObjectConstants(*entry.object);
                    resources.Update(h.objectCB, &objectCB, sizeof(objectCB));
                    meshCall.constantBuffers[1] = h.objectCB;
                    SubmitMeshletDraw(ctx, meshCall, MeshletDrawPass::GBUFFER);
                    batcher.InvalidateObjectConstants();
                    continue;
                }
            }
            const auto bounds = RenderBounds(ctx, *entry.object);
            batcher.Add(dc, ObjectConstants(*entry.object), {bounds.center.x, bounds.center.y, bounds.center.z,
                bounds.radius > 0 ? bounds.radius : -1.0f});
        }
        batcher.Flush();
    }
    const auto hierarchy = ctx.isDeferred ? ReadViewGpuHiZ(ctx) : GpuHiZ{};
    if (hierarchy.depths.IsValid()) ExecuteInstanceGBuffer(ctx, hierarchy);
    else ExecuteInstanceGBuffer(ctx);
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
    const auto mediumAtlas = ctx.waterMediumPassActive && h.waterMediumAtlasRT
        && h.waterMediumAtlasRT->IsValid()
        ? resources.GetColorTexture(*h.waterMediumAtlasRT, 0)
        : renderer::ResourceHandle<renderer::TextureTag>{};
    if (ctx.waterMediumPassActive && h.waterMediumTable.IsValid() && mediumAtlas.IsValid()) {
        dc.auxiliaryPsBuffers[0] = h.waterMediumTable;
        dc.textures[15] = mediumAtlas;
    } else {
        dc.auxiliaryPsBuffers[0] = {};
        dc.textures[15] = {};
    }
    /// @note b9 + t29/t30: クラスタライティング。Legacy モードのときは束縛しない。b9 が未束縛だと clusterLightMode が 0 (= Legacy) として読まれ、シェーダーは b3 の固定長配列へフォールバックするため従来と完全に同じ経路になる。
    if (ctx.clusterLightMode != ClusterLightMode::Legacy) {
        dc.constantBuffers[9] = h.clusterCB;
        /// @note t29
        dc.psBuffers[0]       = h.punctualLightBuffer;
        /// @note クラスタリストは Clustered モードでしか読まれない (Linear は全数走査)。
        if (UsesClusterLightIndices(ctx.clusterLightMode))
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
    dc.indirectReadTextures = { h.lightProbeVisibility[0], h.lightProbeVisibility[1] };
    renderer.Submit(dc, resources);
}

/// @name Deferred 内スキンドメッシュフォワードパス
namespace {
void SubmitForwardRoute(RenderPassContext& ctx, renderer::GeometryRoute route, bool waterfalls = false)
{
    const auto sceneOwner = ctx.renderScene;
    const auto& input = sceneOwner ? *sceneOwner : SceneInput(ctx);
    const auto arguments = route == GeometryRoute::ForwardOpaque && ctx.gpuOcclusionEnabled
        ? ctx.Res().StructuredBuffer(OCCLUSION_DRAW_ARGUMENTS) : ResourceHandle<StructuredBufferTag>{};
    std::vector<MeshEntry> queue;
    const auto append = [&](const RenderObject& object) {
        if ((object.waterfallFarIndexStart != 0) != waterfalls || !object.colorEligible || !MatchesView(ctx, object)) return;
        if (route == renderer::GeometryRoute::ForwardOpaque && object.skinned) ++ctx.statsTotalObjects;
        const auto first = input.items.begin() + object.firstItem;
        const auto last = first + object.itemCount;
        if (std::none_of(first, last, [&](const auto& item) {
            return HasColorGeometry(item) && renderer::ResolveGeometryRoute(item.material.capabilities, ctx.isDeferred) == route;
        })) return;
        if (ctx.isDeferred && !object.skinned) ++ctx.statsTotalObjects;
        const bool visible = [&] {
            if (route == renderer::GeometryRoute::ForwardTransparent && (!ctx.isDeferred || object.skinned)) {
                CullStatsRollback rollback(ctx);
                return IsRenderObjectVisible(ctx, object);
            }
            return IsRenderObjectVisible(ctx, object);
        }();
        if (!visible) return;
        for (auto it = first; it != last; ++it) {
            const auto& item = *it;
            if (!HasColorGeometry(item) || !item.forwardMaterial.valid || !item.forwardMaterial.shader.IsValid()
                || renderer::ResolveGeometryRoute(item.material.capabilities, ctx.isDeferred) != route) continue;
            queue.push_back({&object, &item, (object.sortPosition - ctx.camera.m_position).LengthSq()});
        }
    };
    const auto objects = input.Objects();
    if (waterfalls) {
        for (const auto index : input.waterfallObjects) if (index < objects.size()) append(objects[index]);
    } else {
        for (const auto& object : objects) append(object);
    }
    std::sort(queue.begin(), queue.end(), [route](const MeshEntry& a, const MeshEntry& b) {
        if (a.item->material.renderQueue != b.item->material.renderQueue)
            return a.item->material.renderQueue < b.item->material.renderQueue;
        if (a.distanceSquared != b.distanceSquared)
            return route == renderer::GeometryRoute::ForwardTransparent
                ? a.distanceSquared > b.distanceSquared : a.distanceSquared < b.distanceSquared;
        return std::tie(a.object->sourceIndex, a.object->sourceGeneration, a.item->materialSlot, a.item->sourceSubmesh)
            < std::tie(b.object->sourceIndex, b.object->sourceGeneration, b.item->materialSlot, b.item->sourceSubmesh);
    });
    std::optional<DrawCall> bindings;
    const float projectionY = waterfalls ? std::abs(ctx.camera.GetProjectionMatrix().m[1][1]) : 0.0f;
    const auto viewForward = waterfalls ? ctx.camera.GetForward() : math::Vector3{};
    for (const auto& entry : queue) {
        if (!bindings) bindings = ForwardPassBindings(ctx);
        const auto constants = ObjectConstants(*entry.object);
        ctx.resources.Update(ctx.handles.objectCB, &constants, sizeof(constants));
        auto draw = ForwardDrawWithBindings(ctx, *entry.object, *entry.item, *bindings);
        (void)BindMeshOcclusionArguments(ctx, input, entry.item - input.items.data(), arguments, draw);
        const auto farStart = entry.object->waterfallFarIndexStart;
        if (waterfalls && farStart < draw.indexCount) {
            const float frontDepth = math::Vector3::Dot(entry.object->boundsCenter - ctx.camera.m_position, viewForward)
                - entry.object->boundsRadius;
            const float divisor = ctx.camera.m_projection == ProjectionMode::Orthographic ? 1.0f
                : (std::max)(frontDepth, ctx.camera.m_near);
            const float diameterPx = entry.object->boundsRadius * projectionY * static_cast<float>(ctx.height) / divisor;
            if (diameterPx < 170.0f) {
                draw.startIndex = farStart;
                draw.indexCount -= farStart;
            } else {
                draw.indexCount = farStart;
            }
        }
        SubmitCounted(ctx, draw);
    }
}
}

void ExecuteDeferredSkinnedForwardPass(RenderPassContext& ctx)
{
    ctx.renderer.SetRenderTarget(ctx.Res().Target("HDR"), ctx.resources);
    SubmitForwardRoute(ctx, renderer::GeometryRoute::ForwardOpaque);
    ExecuteInstanceForward(ctx);
}

namespace {
void ExecuteForwardTransparency(RenderPassContext& ctx, bool waterfalls)
{
    if (waterfalls && SceneInput(ctx).waterfallObjects.empty()) return;
    ctx.renderer.SetRenderTarget(ctx.Res().Target("HDR"), ctx.resources);
    const auto frame = MakeCameraFrameCB(ctx.camera, ctx.taaJitterNdcX, ctx.taaJitterNdcY);
    ctx.resources.Update(ctx.handles.frameCB, &frame, sizeof(frame));
    ctx.resources.Update(ctx.handles.lightCB, &ctx.lightData, sizeof(renderer::LightConstantsCB));
    UpdateShadowConstants(ctx);
    UpdatePunctualShadowConstants(ctx);
    SubmitForwardRoute(ctx, renderer::GeometryRoute::ForwardTransparent, waterfalls);
}
} /// @note namespace

void ExecuteForwardTransparentPass(RenderPassContext& ctx)
{
    ExecuteForwardTransparency(ctx, false);
}

void ExecuteDeferredForwardTransparentPass(RenderPassContext& ctx)
{
    ExecuteForwardTransparentPass(ctx);
}

std::string_view GBufferPass::Name() const
{
    return m_mode == GBufferPassMode::DepthNormalPrepass ? "DepthNormalPrepass" : "DeferredGBuffer";
}

void GBufferPass::Setup(PassBuilder& builder, const RenderPassContext& ctx) const
{
    /// @note Forward のプリパスだけが影と Cookie を読む。Deferred 本経路は
    /// @note ライティングをしないので、法線・深度・roughness を書くだけ。
    if (m_mode == GBufferPassMode::DepthNormalPrepass)
        builder.Read("ShadowMap").Read("PunctualShadowMap").Read("LightCookieAtlas");
    else if (ctx.gpuOcclusionEnabled)
        builder.Read(OCCLUSION_HIZ, PassBuilder::Purpose::SHADER_READ).Read(OCCLUSION_DRAW_ARGUMENTS);
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

void DeferredSkinnedForwardPass::Setup(PassBuilder& builder, const RenderPassContext& ctx) const
{
    builder.Read("ShadowMap").Read("PunctualShadowMap").Read("LightCookieAtlas")
           .ReadWrite("HDR");
    if (ctx.gpuOcclusionEnabled) builder.Read(OCCLUSION_DRAW_ARGUMENTS);
    if (ctx.waterMediumPassActive)
        builder.Read("WaterMediumAtlas");
}

void DeferredSkinnedForwardPass::Execute(PassResources&, RenderPassContext& ctx)
{
    ExecuteDeferredSkinnedForwardPass(ctx);
}

void ForwardTransparentPass::Setup(PassBuilder& builder, const RenderPassContext& ctx) const
{
    builder.Read("ShadowMap").Read("PunctualShadowMap").Read("LightCookieAtlas")
           .ReadWrite("HDR");
    if (ctx.gbufferDepthReady) DeclareScreenSpaceOcclusionReads(builder, ctx);
    if (ctx.waterMediumPassActive)
        builder.Read("WaterMediumAtlas");
}

void ForwardTransparentPass::Execute(PassResources&, RenderPassContext& ctx)
{
    ExecuteForwardTransparency(ctx, m_waterfalls);
}

void DeferredLightingPass::Setup(PassBuilder& builder, const RenderPassContext& ctx) const
{
    builder.Read("GBuffer").ReadWrite("HDR")
           .Read("ShadowMap").Read("PunctualShadowMap").Read("LightCookieAtlas");
    if (ctx.waterMediumPassActive)
        builder.Read("WaterMediumAtlas");
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
