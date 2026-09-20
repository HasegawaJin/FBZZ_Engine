/// @file    ShadowPass.cpp
/// @brief   カスケードシャドウマップ描画 (静的メッシュ + スキンドメッシュ + Terrain)。
/// @author  Hasegawa Jin
/// @date    2026-06-18
///
/// @note 1 枚の深度テクスチャを 2x2 のタイルへ分け、カスケードごとに別タイルへ描き込む
/// @note (アトラス)。カスケードの分割位置・行列・タイル矩形は RenderSystem が
/// @note RenderPassContext::shadowCascades へ組み立て済みで、このパスは
/// @note 「タイルを選ぶ → frameCB をそのカスケードの行列へ差し替える → caster を提出」
/// @note を分割数ぶん繰り返すだけになっている。
#include "RenderScenePassHelpers.hpp"
#include <Graphics/Passes/Geometry/FiberRenderPass.hpp>
#include <Graphics/Pipeline/InstanceBatch.hpp>
#include "Graphics/Passes/Geometry/TerrainRenderPass.hpp"
#include "Graphics/Renderer/DrawCall.hpp"
#include <algorithm>
#include <cstdint>
#include <vector>

namespace fbzz::renderer {

namespace {

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
    renderer::ResourceHandle<renderer::ShaderTag>         shader;
    renderer::ResourceHandle<renderer::ConstantBufferTag> skinningCB; ///< @note 無効 = 静的メッシュ
    math::Matrix4                                         world;
    /// @note LOD クロスフェード中のディザしきい値。影も本体と同じ市松で抜かないと、
    /// @note 遷移中だけ 2 レベルぶんの影が重なって濃くなる。
    float                                                 lodDither = 0.0f;
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

/// @brief IsShadowRelevant — この caster がこのカスケード上で意味のある大きさを持つか。
/// @note 影ボリュームは正射影なのでワールド半径がテクセル数に一意換算できる。直径が数テクセル
/// @note 未満の caster は PCF (既定 5x5) で均されて絵に出ないため、DrawCall・頂点・ラスタライズの
/// @note コストが無駄になる前に落とす。しきい値は PCF カーネルより小さく、影が消える方向の破綻は無い。
/// @note 判定はカスケードごとに行う。遠いカスケードで切り捨てられる小物も、手前では十分な大きさを持つ。
bool IsShadowRelevant(const ShadowCascade& cascade, const WorldBounds& bounds)
{
    /// @note テクセル情報が無ければカリングしない
    if (cascade.texelWorldSize <= 0.0f) return true;
    /// @note bounds 未計算メッシュも同様
    if (bounds.radius <= 0.0f)          return true;

    /// @note 直径が 2 テクセル未満なら影として解像できない。
    constexpr float kMinTexelDiameter = 2.0f;
    return (bounds.radius * 2.0f) >= cascade.texelWorldSize * kMinTexelDiameter;
}

/// @note ComputeCascadeMask — この bounds がどのカスケードへ提出されるかを一度に判定する。
uint32_t ComputeCascadeMask(const RenderPassContext& ctx, int cascadeCount,
                            const WorldBounds& bounds, bool hasBounds)
{
    if (!hasBounds) {
        /// @note bounds が取れないメッシュはカリングせず全カスケードへ出す (従来の安全側)。
        return (1u << cascadeCount) - 1u;
    }

    uint32_t mask = 0;
    for (int i = 0; i < cascadeCount; ++i) {
        const ShadowCascade& cascade = ctx.shadowCascades[i];
        if (!cascade.frustum.IntersectsSphere(bounds.center, bounds.radius)) continue;
        if (!IsShadowRelevant(cascade, bounds)) continue;
        mask |= (1u << i);
    }
    return mask;
}

/// @brief ComputePunctualMask — この bounds がどの Spot / Point スロットへ提出されるか。
/// @note 透視投影では 1 テクセルの覆うワールド距離がライトからの距離に比例し、カスケードの
/// @note ような固定 texelWorldSize を持てないため、caster の見かけの角半径 (radius/distance)
/// @note とテクセルの張る角度を直接比べる、深度に依存しない規則でカリングする。
uint32_t ComputePunctualMask(const RenderPassContext& ctx,
                             const WorldBounds& bounds, bool hasBounds)
{
    const int count = std::clamp(ctx.punctualShadowViewCount, 0, kMaxPunctualShadows);
    if (count == 0) return 0;

    uint32_t mask = 0;
    for (int i = 0; i < count; ++i) {
        const PunctualShadowView& view = ctx.punctualShadowViews[i];
        if (hasBounds) {
            if (!view.frustum.IntersectsSphere(bounds.center, bounds.radius)) continue;
            if (bounds.radius > 0.0f && view.texelAngularSize > 0.0f) {
                const math::Vector3 toLight = bounds.center - view.eyePos;
                const float dist = toLight.Length();
                /// @note 直径が 2 テクセルに満たない caster は PCF で完全に均されて絵に出ない。
                /// @note ライトの内側にいる (dist <= radius) 場合は必ず描く。
                if (dist > bounds.radius) {
                    const float angularDiameter = 2.0f * bounds.radius / dist;
                    if (angularDiameter < view.texelAngularSize * 2.0f) continue;
                }
            }
        }
        /// @note bounds が取れないメッシュはカリングせず全スロットへ出す (従来の安全側)。
        mask |= (1u << i);
    }
    return mask;
}

void CollectMeshShadowCasters(RenderPassContext& ctx, int cascadeCount,
    const math::Matrix4& sortView, std::vector<ShadowCaster>& outCasters, bool skinned)
{
    const auto& input = SceneInput(ctx);
    const auto& h = ctx.handles;
    if (skinned && !h.shadowSkinnedShader.IsValid()) return;
    for (const auto& object : input.objects) {
        if (object.skinned != skinned || !MatchesView(ctx, object) || !object.castShadows || !object.colorEligible) continue;
        const auto bounds = RenderBounds(ctx, object);
        const bool hasBounds = bounds.radius > 0.0f;
        if (hasBounds && !renderer::IsWithinDrawDistance(CullingView(ctx),
            { bounds.center, bounds.radius, DrawDistance(ctx, object) })) continue;
        const uint32_t cascadeMask = ComputeCascadeMask(ctx, cascadeCount, bounds, hasBounds);
        const uint32_t punctualMask = ComputePunctualMask(ctx, bounds, hasBounds);
        if (cascadeMask == 0 && punctualMask == 0) continue;
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
                caster.cascadeMask &= ComputeCascadeMask(ctx, cascadeCount, submeshBounds, true);
                caster.punctualMask &= ComputePunctualMask(ctx, submeshBounds, true);
                if (caster.cascadeMask == 0 && caster.punctualMask == 0) continue;
            }
            caster.depthKey = depthKey;
            caster.objectId = item.objectIndex + 1;
            caster.indexBuffer = item.indexBuffer;
            caster.indexCount = item.indexCount;
            caster.world = object.world;
            caster.lodDither = object.lodDither;
            const bool vertexSkinning = skinned && !item.deformedVertexBuffer.IsValid();
            caster.vertexBuffer = skinned ? (vertexSkinning ? item.skinningVertexBuffer
                : item.deformedVertexBuffer) : item.vertexBuffer;
            caster.shader = vertexSkinning ? h.shadowSkinnedShader : h.shadowShader;
            if (vertexSkinning) caster.skinningCB = object.skinningPalette;
            outCasters.push_back(caster);
        }
    }
}

void CollectStaticMeshShadowCasters(RenderPassContext& ctx, int cascadeCount,
    const math::Matrix4& sortView, std::vector<ShadowCaster>& outCasters)
{
    CollectMeshShadowCasters(ctx, cascadeCount, sortView, outCasters, false);
}

void CollectSkinnedMeshShadowCasters(RenderPassContext& ctx, int cascadeCount,
    const math::Matrix4& sortView, std::vector<ShadowCaster>& outCasters)
{
    CollectMeshShadowCasters(ctx, cascadeCount, sortView, outCasters, true);
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
                       int viewIndex, bool punctual)
{
    auto& h = ctx.handles;

    const uint32_t viewBit = 1u << viewIndex;

    /// @note 変種を渡すのは静的メッシュ用のシェーダーで描く caster だけ。VS スキニングへ落ちた
    /// @note caster は shader が違うので prototype が一致せず、束ねられずに 1 件ずつ出る。
    InstanceBatcher batcher(ctx, h.shadowShader,
                            ctx.settings.gpuInstancing
                                ? h.shadowInstancedShader
                                : renderer::ResourceHandle<renderer::ShaderTag>{},
                            true);

    for (const ShadowCaster& caster : casters) {
        const uint32_t mask = punctual ? caster.punctualMask : caster.cascadeMask;
        if ((mask & viewBit) == 0) continue;

        PerObjectCB objData{};
        objData.world          = caster.world;
        objData.objectParams.x = caster.lodDither;

        renderer::DrawCall dc;
        dc.vertexBuffer       = caster.vertexBuffer;
        dc.indexBuffer        = caster.indexBuffer;
        dc.indexCount         = caster.indexCount;
        dc.shader             = caster.shader;
        dc.pipelineState      = h.defaultPSO;
        dc.constantBuffers[0] = h.frameCB;
        if (caster.skinningCB.IsValid())
            dc.constantBuffers[7] = caster.skinningCB;
        /// @note VS スキニングの caster は b1 の world も使うが、束ねないので従来どおり載る。
        batcher.Add(dc, objData);
    }
    batcher.Flush();
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

    EmitShadowCasters(ctx, casters, cascadeIndex, /*punctual=*/false);
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

    EmitShadowCasters(ctx, casters, viewIndex, /*punctual=*/true);
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
    CollectStaticMeshShadowCasters(ctx, cascadeCount, sortView, casters);
    CollectSkinnedMeshShadowCasters(ctx, cascadeCount, sortView, casters);

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
