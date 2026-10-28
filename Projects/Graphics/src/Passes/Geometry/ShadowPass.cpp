/// @file    ShadowPass.cpp
/// @brief   カスケードシャドウマップ描画 (静的メッシュ + スキンドメッシュ + Terrain)。
/// @author  Hasegawa Jin
/// @date    2026-06-18
/// @note 1 枚の深度テクスチャを 2x2 のタイルへ分け、カスケードごとに別タイルへ描き込む
/// @note (アトラス)。カスケードの分割位置・行列・タイル矩形は RenderSystem が
/// @note RenderPassContext::shadowCascades へ組み立て済みで、このパスは
/// @note 「タイルを選ぶ → frameCB をそのカスケードの行列へ差し替える → caster を提出」
/// @note を分割数ぶん繰り返すだけになっている。
#include "RenderScenePassHelpers.hpp"
#include <Graphics/Effects/ComputedSkinningOutput.hpp>
#include "ShadowCasterVisibility.hpp"
#include "../../Renderer/RenderSceneSpatialQueries.hpp"
#include <Graphics/Passes/Geometry/FiberRenderPass.hpp>
#include <Graphics/Passes/Geometry/InstanceRenderPasses.hpp>
#include <Graphics/Pipeline/MeshletDrawHelpers.hpp>
#include <Graphics/Pipeline/InstanceBatch.hpp>
#include "Graphics/Passes/Geometry/TerrainRenderPass.hpp"
#include "Graphics/Renderer/DrawCall.hpp"
#include <algorithm>
#include <cstdint>
#include <vector>

namespace fbzz::renderer {

namespace {

struct MaskedShadowResources {
    uint64_t owner=0, epoch=UINT64_MAX;
    std::array<ResourceHandle<ShaderTag>,3> shaders{};
    ResourceHandle<ConstantBufferTag> material;
    std::array<uint8_t,96> lastMaterial{};
    bool hasMaterial=false;
};
MaskedShadowResources& MaskedShadows(ResourceManager& resources)
{
    static MaskedShadowResources state;
    if (state.owner!=resources.GetInstanceId() || state.epoch!=resources.GetResetVersion()) {
        state={}; state.owner=resources.GetInstanceId(); state.epoch=resources.GetResetVersion();
    }
    constexpr std::array<const char*,3> paths{"Assets/Shaders/Pipeline/Shadow/AlphaShadow.hlsl",
        "Assets/Shaders/Pipeline/Shadow/AlphaShadowInstanced.hlsl","Assets/Shaders/Pipeline/Shadow/AlphaShadowSkinned.hlsl"};
    for (size_t i=0; i<3; ++i) if (!resources.Get(state.shaders[i])) state.shaders[i]=resources.LoadShader(paths[i]);
    if (!resources.Get(state.material)) state.material=resources.CreateConstantBuffer(96);
    return state;
}

/// @brief ShadowCaster — 深度専用パスへ流す 1 描画分の最小情報。
/// @note 光源に近い順に並べてから描く。シャドウマップは深度専用なので、手前を先に描けば
/// @note GPU の Hi-Z が後続の遮蔽フラグメントを捨てられる。逆順だと同じ画素を描き直す量が
/// @note 増え、床・壁が重なるシーンではそのまま実行時間に効く。
struct ShadowCaster {
    float    depthKey  = 0.0f;   ///< @note ライト方向に沿った距離 (小さいほど光源に近い)
    uint32_t objectId  = 0;      ///< @note 同一オブジェクト由来の submesh を判別する
    /// @note このメッシュを描くカスケードのビットマスク (bit i = カスケード i)。収集・bounds 計算・
    /// @note ソートはカスケード間で共有し、描画時はこのマスクを見て投げるだけにする。
    uint32_t cascadeMask = 0;
    /// @note このメッシュを描く Spot / Point シャドウスロットのビットマスク。cascadeMask とは判定
    /// @note 基準が違う (カスケードは正射影でワールド半径から一意、Spot/Point は透視投影で深度依存)。
    uint32_t punctualMask = 0;
    renderer::ResourceHandle<renderer::BufferTag>         vertexBuffer;
    renderer::ResourceHandle<renderer::BufferTag>         indexBuffer;
    uint32_t                                              indexCount = 0;
    const renderer::RenderMeshItem*                       item = nullptr;
    bool                                                  skinned = false;
    renderer::ResourceHandle<renderer::ShaderTag>         shader;
    renderer::ResourceHandle<renderer::ConstantBufferTag> skinningCB; ///< @note 無効 = 静的メッシュ
    math::Matrix4                                         world;
    math::Vector4                                         worldSphere{0, 0, 0, -1};
    /// @note LOD クロスフェード中のディザしきい値。影も本体と同じ市松で抜かないと、
    /// @note 遷移中だけ 2 レベルぶんの影が重なって濃くなる。
    float                                                 lodDither = 0.0f;
    RenderGpuBonePalette gpuBones;
};

/// @brief ライト空間の深度キー。ライトビューの第 3 行 (ライト前方への射影) で中心を射影し、
/// @note 視線方向の距離を取り出す。
/// @note 全カスケードは同じ lightDir/up で LookAt するため view 第 3 行の xyz は同一で、
/// @note 違いは eye 由来の平行移動項 m[2][3] だけ。この項は全 caster に同じ定数として乗る
/// @note ため、光源に近い順の並びはカスケードに依存せず、ソートは 1 回で済む。
float ComputeLightDepthKey(const math::Matrix4& lightView, const math::Vector3& center)
{
    return lightView.m[2][0] * center.x
         + lightView.m[2][1] * center.y
         + lightView.m[2][2] * center.z
         + lightView.m[2][3];
}

void CollectMeshShadowCasters(RenderPassContext& ctx, int cascadeCount,
    const math::Matrix4& sortView, std::vector<ShadowCaster>& outCasters, bool skinned,
    const spatial_detail::CandidateBatch* candidates)
{
    const auto& input = candidates ? *candidates->snapshot : SceneInput(ctx);
    const auto& h = ctx.handles;
    if (skinned && !h.shadowSkinnedShader.IsValid()) return;
    const auto collectObject = [&](const RenderObject& object) {
        if (object.skinned != skinned || !MatchesView(ctx, object) || !object.castShadows || !object.colorEligible) return;
        const auto bounds = RenderBounds(ctx, object);
        const bool hasBounds = bounds.radius > 0.0f;
        if (hasBounds && !renderer::IsWithinDrawDistance(CullingView(ctx),
            { bounds.center, bounds.radius, DrawDistance(ctx, object) })) return;
        const uint32_t cascadeMask = shadow_visibility_detail::ComputeCascadeMask(ctx, cascadeCount, bounds, hasBounds);
        const uint32_t punctualMask = shadow_visibility_detail::ComputePunctualMask(ctx, bounds, hasBounds);
        if (cascadeMask == 0 && punctualMask == 0) return;
        const math::Vector3 worldPosition{ object.world.m[0][3], object.world.m[1][3], object.world.m[2][3] };
        const float depthKey = ComputeLightDepthKey(sortView, hasBounds ? bounds.center : worldPosition);
        for (uint32_t i = object.firstItem; i < object.firstItem + object.itemCount; ++i) {
            const auto& item = input.items[i];
            if (!HasColorGeometry(item) || item.material.capabilities.blend != renderer::BlendMode::OPAQUE_BLEND) continue;
            ShadowCaster caster;
            caster.cascadeMask = cascadeMask;
            caster.punctualMask = punctualMask;
            if (skinned && item.boundsRadius > 0.0f) {
                const WorldBounds submeshBounds{ item.boundsCenter,
                    item.boundsRadius + (std::max)(ctx.cullingBoundsPadding, 0.0f) };
                caster.cascadeMask &= shadow_visibility_detail::ComputeCascadeMask(ctx, cascadeCount, submeshBounds, true);
                caster.punctualMask &= shadow_visibility_detail::ComputePunctualMask(ctx, submeshBounds, true);
                if (caster.cascadeMask == 0 && caster.punctualMask == 0) continue;
            }
            caster.depthKey = depthKey;
            caster.objectId = item.objectIndex + 1;
            caster.indexBuffer = item.indexBuffer;
            caster.indexCount = item.indexCount;
            caster.item = &item;
            caster.skinned = skinned;
            caster.world = object.world;
            if (hasBounds) caster.worldSphere = {bounds.center.x, bounds.center.y, bounds.center.z, bounds.radius};
            caster.lodDither = object.lodDither;
            const bool vertexSkinning = skinned && !item.preparedSkinning && !item.deformedVertexBuffer.IsValid();
            caster.vertexBuffer = skinned ? (vertexSkinning ? item.skinningVertexBuffer
                : item.deformedVertexBuffer) : item.vertexBuffer;
            caster.shader = vertexSkinning ? h.shadowSkinnedShader : h.shadowShader;
            if (vertexSkinning) { caster.skinningCB = object.skinningPalette; caster.gpuBones = object.gpuBones; }
            outCasters.push_back(caster);
        }
    };
    if (candidates) {
        for (size_t position = 0; position < candidates->Size(); ++position)
            collectObject(input.Objects()[candidates->ObjectIndex(position)]);
    } else {
        for (const auto& object : input.Objects()) collectObject(object);
    }
}

void CollectStaticMeshShadowCasters(RenderPassContext& ctx, int cascadeCount,
    const math::Matrix4& sortView, std::vector<ShadowCaster>& outCasters,
    const spatial_detail::CandidateBatch* candidates = nullptr)
{
    CollectMeshShadowCasters(ctx, cascadeCount, sortView, outCasters, false, candidates);
}

void CollectSkinnedMeshShadowCasters(RenderPassContext& ctx, int cascadeCount,
    const math::Matrix4& sortView, std::vector<ShadowCaster>& outCasters,
    const spatial_detail::CandidateBatch* candidates = nullptr)
{
    CollectMeshShadowCasters(ctx, cascadeCount, sortView, outCasters, true, candidates);
}

/// @brief EmitShadowCasters — 収集済み caster のうち、指定ビューに属するものを描画する。
/// @param punctual false なら cascadeMask の bit viewIndex を、true なら punctualMask を見る。
/// @pre 配列は呼び出し前にソート済みであること (`ComputeLightDepthKey` 参照。ソートは 1 回でよい)。
/// @note b1 は 1 本を使い回すため更新と Submit は交互に行う。同一オブジェクトの連続 submesh は
/// @note world が同じなので InstanceBatcher が載せ直しを省く。
/// @note 束ねるのは深度順の並びの中で連続している同一メッシュだけ。並べ替えないので early-Z の
/// @note 効き方は変わらない (Docs/design/gpu-instancing.md «やらないこと»)。
void EmitShadowCasters(RenderPassContext& ctx,
                       const std::vector<ShadowCaster>& casters,
                       int viewIndex, bool punctual,
                       const math::Matrix4& viewProjection)
{
    auto& h = ctx.handles;

    const uint32_t viewBit = 1u << viewIndex;

    /// @note 変種を渡すのは静的メッシュ用のシェーダーで描く caster だけ。VS スキニングへ落ちた
    /// @note caster は shader が違うので prototype が一致せず、束ねられずに 1 件ずつ出る。
    const auto gpuFrustum = math::Frustum::FromViewProjection(viewProjection);
    auto& masked=MaskedShadows(ctx.resources);
    InstanceBatcher maskedBatch(ctx,masked.shaders[0],ctx.settings.gpuInstancing ? masked.shaders[1] : ResourceHandle<ShaderTag>{},true,&gpuFrustum);
    InstanceBatcher batcher(ctx, h.shadowShader,
                            ctx.settings.gpuInstancing
                                ? h.shadowInstancedShader
                                : renderer::ResourceHandle<renderer::ShaderTag>{},
                            true, &gpuFrustum);

    for (const ShadowCaster& caster : casters) {
        const uint32_t mask = punctual ? caster.punctualMask : caster.cascadeMask;
        if ((mask & viewBit) == 0) continue;

        PerObjectCB objData{};
        objData.world          = caster.world;
        objData.objectParams.x = caster.lodDither;
        objData.objectParams.z = caster.gpuBones.current.IsValid() ? 1.0f : 0.0f;
        objData.objectParams.w = static_cast<float>(caster.gpuBones.currentOffset);

        renderer::DrawCall dc;
        dc.vertexBuffer       = caster.vertexBuffer;
        dc.vsBuffers[0] = caster.gpuBones.current;
        if (caster.skinned && caster.item && !BindPreparedSkinningOutput(*caster.item, dc)) continue;
        if (caster.skinned && caster.item && caster.item->deformedVertexBuffer.IsValid()
            && !caster.skinningCB.IsValid()) dc.deformationSource = caster.item->skinningVertexBuffer;
        dc.indexBuffer        = caster.indexBuffer;
        dc.indexCount         = caster.indexCount;
        dc.shader             = caster.shader;
        dc.pipelineState      = h.defaultPSO;
        dc.constantBuffers[0] = h.frameCB;
        const bool alpha=caster.item && caster.item->material.surface.alphaCutoff>0;
        if (caster.item) {
            const auto& material=caster.item->material;
            dc.pipelineState=GetOrCreateMaterialPSO(ctx.resources,BlendMode::OPAQUE_BLEND,material.doubleSided,
                material.depthBias,material.depthBiasSlope,false);
        }
        if (alpha) {
            batcher.Flush();
            const auto& material=caster.item->material;
            dc.shader=caster.skinningCB.IsValid() ? masked.shaders[2] : masked.shaders[0];
            if (!ctx.resources.Get(dc.shader) || !ctx.resources.Get(masked.material)) continue;
            if (!masked.hasMaterial || masked.lastMaterial!=material.gbufferParams) {
                maskedBatch.Flush();
                ctx.resources.Update(masked.material,material.gbufferParams.data(),material.gbufferParams.size());
                masked.lastMaterial=material.gbufferParams; masked.hasMaterial=true;
            }
            dc.constantBuffers[2]=masked.material;
            for (size_t i=0; i<material.textures.size(); ++i) dc.textures[i]=material.textures[i];
        } else maskedBatch.Flush();
        if (caster.skinningCB.IsValid() && !caster.gpuBones.current.IsValid())
            dc.constantBuffers[7] = caster.skinningCB;
        /// @note VS スキニングの caster は b1 の world も使うが、束ねないので従来どおり載る。
        if (ctx.settings.geometryDrawMode == GeometryDrawMode::MESH_SHADER && caster.item) {
            renderer::DrawCall meshCall = dc;
            if (TryPrepareMeshletDraw(ctx.resources, ctx.renderer, viewProjection, caster.world,
                MeshletDrawPass::SHADOW, *caster.item, caster.skinned, meshCall)) {
                batcher.Flush();
                ctx.resources.Update(h.objectCB, &objData, sizeof(objData));
                meshCall.constantBuffers[1] = h.objectCB;
                SubmitCountedShadow(ctx, meshCall);
                batcher.InvalidateObjectConstants();
                continue;
            }
        }
        if (alpha) maskedBatch.Add(dc,objData,caster.worldSphere);
        else batcher.Add(dc, objData, caster.worldSphere);
    }
    batcher.Flush();
    maskedBatch.Flush();
}

/// @note RenderShadowCascade — カスケード 1 枚をアトラスの担当タイルへ描く。
/// @note casters は全カスケード共通のソート済み配列。ここでは cascadeMask で絞って投げるだけ。
void RenderShadowCascade(RenderPassContext& ctx, const ShadowCascade& cascade,
                         int cascadeIndex, const std::vector<ShadowCaster>& casters)
{
    /// @note このカスケードのタイルへビューポートを絞る。SetRenderTarget はビューポートを
    /// @note RT 全体へ戻すため、必ずその後に呼ぶ。
    ctx.renderer.SetViewport(cascade.viewportX, cascade.viewportY,
                             cascade.viewportSize, cascade.viewportSize);

    /// @note シャドウ用 VS は b0 の viewProjection しか読まない。カスケードの行列へ差し替える。
    PerFrameCB lightFrameData{};
    lightFrameData.viewProjection = cascade.viewProjection;
    lightFrameData.view = cascade.view;
    lightFrameData.cameraPos = cascade.eyePos;
    lightFrameData.isOrthographic = 1.0f;
    ctx.resources.Update(ctx.handles.frameCB, &lightFrameData, sizeof(PerFrameCB));

    EmitShadowCasters(ctx, casters, cascadeIndex, false, cascade.viewProjection);
    SubmitInstanceShadows(ctx,cascade.frustum,ctx.handles.frameCB);
    SubmitFiberShadowCasters(ctx, lightFrameData, cascade.frustum);

    /// @note Terrain は自前でチャンクを走査し、チャンク単位で objectCB を更新するため
    /// @note caster 配列には混ぜない。地形は 1 枚の連続面で自己重なりが無く、
    /// @note 並べ替えても Hi-Z の効きが変わらない。
    SubmitTerrainShadowCasters(ctx, cascade.frustum,
                               ctx.handles.shadowShader, ctx.handles.defaultPSO,
                               ctx.handles.frameCB, ctx.handles.objectCB);
}

/// @note RenderPunctualShadowView — Spot / Point のタイル 1 枚を描く。
/// @note カスケードとの違いはビューポートと行列の出どころだけで、提出の流れは同一。
void RenderPunctualShadowView(RenderPassContext& ctx, const PunctualShadowView& view,
                              int viewIndex, const std::vector<ShadowCaster>& casters)
{
    ctx.renderer.SetViewport(view.viewportX, view.viewportY,
                             view.viewportSize, view.viewportSize);

    PerFrameCB lightFrameData{};
    lightFrameData.viewProjection = view.viewProjection;
    lightFrameData.view = view.view;
    lightFrameData.cameraPos = view.eyePos;
    ctx.resources.Update(ctx.handles.frameCB, &lightFrameData, sizeof(PerFrameCB));

    EmitShadowCasters(ctx, casters, viewIndex, true, view.viewProjection);
    SubmitInstanceShadows(ctx,view.frustum,ctx.handles.frameCB);
    SubmitFiberShadowCasters(ctx, lightFrameData, view.frustum);

    /// @note 地形も Spot / Point の影を落とす。屋内の床が地形でできているシーンでは、
    /// @note ここを飛ばすと「壁の影は出るのに床には何も落ちない」ことになる。
    SubmitTerrainShadowCasters(ctx, view.frustum,
                               ctx.handles.shadowShader, ctx.handles.defaultPSO,
                               ctx.handles.frameCB, ctx.handles.objectCB);
}

} /// @note namespace

void ExecuteShadowPass(RenderPassContext& ctx)
{
    auto& renderer  = ctx.renderer;
    auto& resources = ctx.resources;
    auto& h         = ctx.handles;
    const auto& rs  = ctx.settings;

    const int punctualViewCount =
        std::clamp(ctx.punctualShadowViewCount, 0, kMaxPunctualShadows);
    const bool hasPunctualAtlas = ctx.Res().Target("PunctualShadowMap").IsValid();

    /// @note Spot / Point のアトラスも毎フレームまっさらにしてから始める。スロットの割り当ては
    /// @note フレームごとに変わる (カメラが動けば近い順が入れ替わる) ため、前フレームの深度が
    /// @note 残っていると割り当て直後の 1 フレームだけ別ライトの遮蔽を引いてしまう。
    if (hasPunctualAtlas) {
        renderer.SetRenderTarget(ctx.Res().Target("PunctualShadowMap"), resources);
        renderer.ClearDepth();
    }

    /// @note クリアはアトラス全面へ 1 回。カスケードごとのビューポートを張る前に行う。
    renderer.SetRenderTarget(ctx.Res().Target("ShadowMap"), resources);
    renderer.ClearDepth();

    if (!rs.shadowEnabled) return;
    /// @note 影の濃さが 0 のときは面の陰影に深度が一切効かない (`Shadow.hlsli`:
    /// @note `lerp(1 - shadowStrength, 1, factor)` が常に 1 を返す) ため、caster の提出を丸ごと省く。
    /// @note 光芒 (VolumetricLight) は shadowStrength を通さず生の遮蔽を読むため、有効なときは
    /// @note このスキップを行わない。
    /// @note ctx.shadowStrength は Directional の設定。Spot / Point は自前のスロットごとの強度を
    /// @note 持つので、こちらが 0 でも描く必要がある。
    const bool needDirectional = (ctx.shadowStrength > 0.0f) || rs.volumetricLight.enabled;
    const bool needPunctual    = hasPunctualAtlas && punctualViewCount > 0;
    if (!needDirectional && !needPunctual) return;

    /// @note このスコープの `renderer` は ctx.renderer への参照なので、
    /// @note 定数は名前空間から完全修飾で引く。
    const int cascadeCount =
        std::clamp(ctx.shadowCascadeCount, 1, fbzz::renderer::kMaxShadowCascades);

    /// @name caster の収集はフレームに 1 回だけ
    /// @note シーン走査・GetComponent・bounds 計算・ソートはカスケード間で共有できる。カスケード
    /// @note ごとに違うのは「どの錐台に入るか」だけなので、それを cascadeMask へ畳み込み、描画側は
    /// @note マスクを見て投げるだけにする。収集バッファはフレームをまたいで使い回し、ヒープ確保を
    /// @note 避ける。ShadowPass はレンダースレッド 1 本からしか呼ばれない (RenderGraph 直列実行)。
    static std::vector<ShadowCaster> casters;
    casters.clear();

    /// @note 並び順はカスケードによらず同じなので、ソート基準はカスケード 0 の view でよい。
    const math::Matrix4& sortView = ctx.shadowCascades[0].view;
    /// @note Collected caster item pointers retain the same immutable owner through emission, even if a resource hook publishes another snapshot.
    std::shared_ptr<const RenderScene> spatialSnapshot;
    if (spatial_detail::MeshSpatialQueriesEnabled()) {
        const auto candidates = spatial_detail::CollectShadowCandidates(ctx, cascadeCount);
        spatialSnapshot = candidates.snapshot;
        const auto* validCandidates = candidates.snapshot ? &candidates : nullptr;
        CollectStaticMeshShadowCasters(ctx, cascadeCount, sortView, casters, validCandidates);
        CollectSkinnedMeshShadowCasters(ctx, cascadeCount, sortView, casters, validCandidates);
    } else {
        CollectStaticMeshShadowCasters(ctx, cascadeCount, sortView, casters);
        CollectSkinnedMeshShadowCasters(ctx, cascadeCount, sortView, casters);
    }

    /// @note 光源に近い順。Hi-Z が後続の遮蔽フラグメントを捨てられるようにする。
    std::stable_sort(casters.begin(), casters.end(),
                     [](const ShadowCaster& a, const ShadowCaster& b) {
                         return a.depthKey < b.depthKey;
                     });

    if (needDirectional) {
        /// @note クリアの後に punctual アトラスを触っている可能性があるので、束縛し直す。
        renderer.SetRenderTarget(ctx.Res().Target("ShadowMap"), resources);
        for (int i = 0; i < cascadeCount; ++i)
            RenderShadowCascade(ctx, ctx.shadowCascades[i], i, casters);
    }

    if (needPunctual) {
        renderer.SetRenderTarget(ctx.Res().Target("PunctualShadowMap"), resources);
        for (int i = 0; i < punctualViewCount; ++i)
            RenderPunctualShadowView(ctx, ctx.punctualShadowViews[i], i, casters);
    }

    /// @note 後続パスがビューポートを RT 全体だと仮定してよいよう、タイル絞りを解除しておく。
    /// @note SetRenderTarget を経由しない描画がこの直後に来ても壊れないようにする保険。
    const uint32_t atlasResolution = (std::max)(rs.shadow.mapResolution, 1u);
    renderer.SetViewport(0, 0, atlasResolution, atlasResolution);
}


void ShadowPass::Setup(PassBuilder& builder, const RenderPassContext&) const
{
    /// @note Directional の CSM と Spot / Point のアトラスを 1 パスで描く。caster の収集と
    /// @note ソートを両者で共有するので、分けるとシーン走査が丸ごと 2 回になる。
    builder.Write("ShadowMap").Write("PunctualShadowMap");
}

void ShadowPass::Execute(PassResources&, RenderPassContext& ctx)
{
    ExecuteShadowPass(ctx);
}
} /// @note namespace fbzz::renderer
