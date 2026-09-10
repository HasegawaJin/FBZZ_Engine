/// @file    RenderPasses/ShadowPass.cpp
/// @brief   カスケードシャドウマップ描画 (静的メッシュ + スキンドメッシュ + Terrain)。
/// @author  Hasegawa Jin
/// @date    2026-06-18
///
/// 1 枚の深度テクスチャを 2x2 のタイルへ分け、カスケードごとに別タイルへ描き込む
/// (アトラス)。カスケードの分割位置・行列・タイル矩形は RenderSystem が
/// RenderPassContext::shadowCascades へ組み立て済みで、このパスは
/// 「タイルを選ぶ → frameCB をそのカスケードの行列へ差し替える → caster を提出」
/// を分割数ぶん繰り返すだけになっている。
#include "GeometryPasses.hpp"
#include "Engine/Scene/Systems/RenderPasses/Geometry/TerrainRenderPass.hpp"
#include "Engine/Scene/Scene.hpp"
#include "Engine/Scene/Transform.hpp"
#include "Engine/Renderer/Mesh.hpp"
#include "Engine/Scene/Components/MeshRenderer.hpp"
#include "Engine/Scene/Components/MaterialComponent.hpp"
#include "Engine/Scene/Components/AnimatorComponent.hpp"
#include "Engine/Scene/Components/SkinnedMeshRenderer.hpp"
#include "Engine/Renderer/DrawCall.hpp"
#include <algorithm>
#include <cstdint>
#include <vector>

namespace fbzz::scene {

namespace {

// ShadowCaster — 深度専用パスへ流す 1 描画分の最小情報。
// WHY: 提出をいったんこの配列へ溜めるのは、光源に近い順へ並べ替えてから描くため。
//      シャドウマップは深度しか書かないので、手前を先に描くと GPU の階層 Z (Hi-Z) が
//      後続の遮蔽されたフラグメントをラスタライズ前に捨てられる。逆順だと同じ画素を
//      何度も書き直すことになり、影ボリュームいっぱいに広がる床・壁が重なるシーンでは
//      ここがそのまま ShadowPass の実行時間になる。
struct ShadowCaster {
    float    depthKey  = 0.0f;   // ライト方向に沿った距離 (小さいほど光源に近い)
    uint32_t objectId  = 0;      // 同一オブジェクト由来の submesh を判別する
    // このメッシュを描くカスケードのビットマスク (bit i = カスケード i)。
    // WHY: 収集・bounds 計算・ソートはカスケード間で完全に共有できる。
    //      「どのカスケードに属するか」だけをここへ畳み込んでおけば、
    //      描画時はマスクを見て投げるだけで済む。
    uint32_t cascadeMask = 0;
    // このメッシュを描く Spot / Point シャドウスロットのビットマスク。
    // WHY cascadeMask と分けるか: 判定基準が違う。カスケードは正射影なので
    //     ワールド半径からテクセル数が一意に決まるが、Spot / Point は透視投影で
    //     深度に比例して変わる。1 本のマスクへ詰めると、どちらの規則で立った
    //     ビットなのかがコードから読めなくなる。
    uint32_t punctualMask = 0;
    renderer::ResourceHandle<renderer::BufferTag>         vertexBuffer;
    renderer::ResourceHandle<renderer::BufferTag>         indexBuffer;
    uint32_t                                              indexCount = 0;
    renderer::ResourceHandle<renderer::ShaderTag>         shader;
    renderer::ResourceHandle<renderer::ConstantBufferTag> skinningCB; // 無効 = 静的メッシュ
    math::Matrix4                                         world;
    // LOD クロスフェード中のディザしきい値。影も本体と同じ市松で抜かないと、
    // 遷移中だけ 2 レベルぶんの影が重なって濃くなる。
    float                                                 lodDither = 0.0f;
};

// ライト空間の深度キー。ライトビューの第 3 行が「ライト前方への射影」なので、
// 中心をそのまま射影して視線方向の距離を取り出す。
//
// WHY カスケード 0 の view を全カスケードで使い回せるか:
//   全カスケードは同じ lightDir・同じ up で LookAt しているので、view の第 3 行の
//   xyz 成分 (= ライト前方ベクトル) は完全に同一。違うのは eye 位置に由来する
//   平行移動項 m[2][3] だけで、これは全 caster に同じ定数が乗るだけなので
//   「光源に近い順」の並びはカスケードによらず変わらない。よってソートは 1 回でよい。
float ComputeLightDepthKey(const math::Matrix4& lightView, const math::Vector3& center)
{
    return lightView.m[2][0] * center.x
         + lightView.m[2][1] * center.y
         + lightView.m[2][2] * center.z
         + lightView.m[2][3];
}

// IsShadowRelevant — この caster がこのカスケード上で意味のある大きさを持つか。
// WHY: 影ボリュームは正射影なので、ワールド半径がそのままテクセル数へ換算できる。
//      直径が数テクセルにも満たない caster は、描いても PCF (既定 5x5) で完全に均されて
//      絵に出ない。それでも DrawCall・頂点処理・ラスタライズのコストは満額かかるため、
//      提出前に落とす。しきい値を PCF カーネルより小さく取ってあるので、
//      「見えていた影が消える」方向の破綻は起きない。
// NOTE: 判定はカスケードごとに行う。遠いカスケードでは切り捨てられる小物も、
//       手前のカスケードでは十分な大きさを持つ。
bool IsShadowRelevant(const ShadowCascade& cascade, const WorldBounds& bounds)
{
    if (cascade.texelWorldSize <= 0.0f) return true;  // テクセル情報が無ければカリングしない
    if (bounds.radius <= 0.0f)          return true;  // bounds 未計算メッシュも同様

    // 直径が 2 テクセル未満なら影として解像できない。
    constexpr float kMinTexelDiameter = 2.0f;
    return (bounds.radius * 2.0f) >= cascade.texelWorldSize * kMinTexelDiameter;
}

// ComputeCascadeMask — この bounds がどのカスケードへ提出されるかを一度に判定する。
uint32_t ComputeCascadeMask(const RenderPassContext& ctx, int cascadeCount,
                            const WorldBounds& bounds, bool hasBounds)
{
    if (!hasBounds) {
        // bounds が取れないメッシュはカリングせず全カスケードへ出す (従来の安全側)。
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

// ComputePunctualMask — この bounds がどの Spot / Point スロットへ提出されるか。
//
// WHY 極小カリングを角度で行うか: 透視投影では 1 テクセルの覆うワールド距離が
//     ライトからの距離に比例するため、カスケードのような固定の texelWorldSize を
//     持てない。caster の見かけの角半径 (radius / distance) と、テクセルの張る角度を
//     直接比べれば、深度に依存しない 1 つの規則で判定できる。
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
                // 直径が 2 テクセルに満たない caster は PCF で完全に均されて絵に出ない。
                // ライトの内側にいる (dist <= radius) 場合は必ず描く。
                if (dist > bounds.radius) {
                    const float angularDiameter = 2.0f * bounds.radius / dist;
                    if (angularDiameter < view.texelAngularSize * 2.0f) continue;
                }
            }
        }
        // bounds が取れないメッシュはカリングせず全スロットへ出す (従来の安全側)。
        mask |= (1u << i);
    }
    return mask;
}

// CollectStaticMeshShadowCasters — MeshRenderer の静的メッシュを収集する。
// WHY: ShadowPass は「どのカスケードのどの面へ描くか」を管理し、MeshRenderer 固有の
//      DrawCall 組み立ては caster 収集関数へ分離する。Spot / Point シャドウを追加するときも
//      ライトループから同じ収集関数を再利用できる。
// NOTE: 収集はフレームに 1 回だけ行い、カスケードごとの所属は cascadeMask へ畳み込む。
//       以前はカスケードごとにシーン全体を走査し直しており、分割数ぶん
//       GetComponent と bounds 計算を丸ごと重複させていた。
void CollectStaticMeshShadowCasters(RenderPassContext& ctx,
                                    int cascadeCount,
                                    const math::Matrix4& sortView,
                                    std::vector<ShadowCaster>& outCasters)
{
    auto& h = ctx.handles;

    uint32_t objectId = 0;
    for (auto& go : ctx.scene.GameObjects()) {
        ++objectId;
        if (!ShouldRenderGameObject(go, ctx.cullingMask)) continue;
        auto* mr = go.GetComponent<MeshRenderer>();
        if (!mr || !mr->enabled || !mr->castShadows || !mr->lodVisible || !mr->mesh) continue;
        if (!mr->mesh->vertexBuffer.IsValid() || !mr->mesh->indexBuffer.IsValid()) continue;
        if (mr->mesh->isSkinned) continue;

        // 深度専用パスなので、実際に描くと決まってから MaterialComponent を触る。
        // WHY: カリングで落ちる caster にまでマテリアル解決 (アセットロードを伴う) を
        //      走らせる必要はない。ここで使うのは「描画対象として有効か」の判定だけ。
        auto* mat = go.GetComponent<MaterialComponent>();
        if (!mat || !mat->enabled) continue;

        // 全カスケードのカリング判定と深度キーを 1 回の bounds 計算で賄う。
        const bool hasBounds = mr->mesh->boundsRadius > 0.0f;
        WorldBounds bounds{};
        // 余白はカメラ側の Culling Bounds Padding と共通。
        // WHY 影にも効かせるか: bounds が実際のシルエットより小さいという問題は同じで、
        //     カメラ側だけ広げると「本体は出ているのに影だけ消える」というさらに分かりにくい
        //     壊れ方になる。
        if (hasBounds) bounds = ComputeWorldBounds(go.transform, *mr->mesh, ctx.cullingBoundsPadding);

        // 距離カリングで本体が消えた caster は影も落とさない。
        if (hasBounds && !IsWithinCullDistance(ctx, go, bounds)) continue;

        const uint32_t cascadeMask  = ComputeCascadeMask(ctx, cascadeCount, bounds, hasBounds);
        const uint32_t punctualMask = ComputePunctualMask(ctx, bounds, hasBounds);
        if (cascadeMask == 0 && punctualMask == 0) continue;  // どの影にも映らない

        const float depthKey = ComputeLightDepthKey(
            sortView, hasBounds ? bounds.center : go.transform.worldPosition);

        if (!mat->EnsureMaterialAsset()) continue;

        // 半透明マテリアルは影を落とさない (スキンド側と同じ規則)。
        // ガラス・水・エフェクト板が真っ黒な影を落とすのを防ぎ、DrawCall も減らす。
        // アルファテスト (切り抜き) は OPAQUE 扱いのままなので葉や柵は影を保つ。
        if (mat->GetBlendMode() != renderer::BlendMode::OPAQUE_BLEND) continue;

        ShadowCaster caster;
        caster.cascadeMask  = cascadeMask;
        caster.punctualMask = punctualMask;
        caster.depthKey     = depthKey;
        caster.objectId     = objectId;
        caster.vertexBuffer = mr->mesh->vertexBuffer;
        caster.indexBuffer  = mr->mesh->indexBuffer;
        caster.indexCount   = mr->mesh->indexCount;
        caster.shader       = h.shadowShader;
        caster.world        = go.transform.GetWorldMatrix();
        caster.lodDither    = mr->lodDither;
        outCasters.push_back(caster);
    }
}

// CollectSkinnedMeshShadowCasters — SkinnedMeshRenderer をスキニング CB 付きで収集する。
// WHAT: Animator の skinningBuffer が未生成のフレームでは bind pose CB にフォールバックする。
void CollectSkinnedMeshShadowCasters(RenderPassContext& ctx,
                                     int cascadeCount,
                                     const math::Matrix4& sortView,
                                     std::vector<ShadowCaster>& outCasters)
{
    auto& h = ctx.handles;

    if (!h.shadowSkinnedShader.IsValid())
        return;

    uint32_t objectId = 0x8000'0000u; // 静的メッシュ側の objectId と衝突させない
    for (auto& go : ctx.scene.GameObjects()) {
        ++objectId;
        if (!ShouldRenderGameObject(go, ctx.cullingMask)) continue;
        auto* smr = go.GetComponent<SkinnedMeshRenderer>();
        if (!smr || !smr->enabled || !smr->castShadows || !smr->lodVisible || !smr->model) continue;

        // bounds 計算は全 submesh を 2 周するので、安いフラグ判定を全て通してから呼ぶ。
        WorldBounds bounds{};
        const bool hasBounds =
            ComputeSkinnedWorldBounds(go, *smr, bounds, ctx.cullingBoundsPadding);

        // 距離カリングで本体が消えた caster は影も落とさない。
        if (hasBounds && !IsWithinCullDistance(ctx, go, bounds)) continue;

        const uint32_t cascadeMask  = ComputeCascadeMask(ctx, cascadeCount, bounds, hasBounds);
        const uint32_t punctualMask = ComputePunctualMask(ctx, bounds, hasBounds);
        if (cascadeMask == 0 && punctualMask == 0) continue;

        const float depthKey = ComputeLightDepthKey(
            sortView, hasBounds ? bounds.center : go.transform.worldPosition);

        auto* mat = go.GetComponent<MaterialComponent>();
        if (!mat || !mat->enabled || !mat->EnsureMaterialAsset()) continue;
        auto* anim = FindAnimator(go);

        const auto skinCB = ResolveSkinningCB(
            anim ? anim->skinningBuffer : decltype(anim->skinningBuffer){},
            smr->model, h.bindPoseSkinningCB);

        const math::Matrix4 world = go.transform.GetWorldMatrix();

        // この Renderer が担当する submesh を全て影として収集する。
        // 深度キーは全 submesh 共通なので、安定ソート後も 1 オブジェクトの submesh は
        // 隣り合ったままになり、PerObjectCB の更新は 1 回で済む。
        // mi はローカルスロット番号で、model->meshes の添字とは一致しないことがある。
        const size_t meshCount = smr->SubmeshCount();
        for (size_t mi = 0; mi < meshCount; ++mi) {
            renderer::Mesh* meshPtr = smr->SubmeshMesh(mi);
            if (!meshPtr) continue;
            if (!meshPtr->vertexBuffer.IsValid() || !meshPtr->indexBuffer.IsValid()) continue;
            MaterialSlot& slot = mat->SlotAt(mi);
            // 非表示指定の submesh は影も落とさない。
            if (!slot.visible) continue;
            // ブレンドモードを見るには .mat が解決済みである必要がある。
            // 未解決なら GetBlendMode() は OPAQUE を返すため、影を落とす側 (安全側) に倒れる。
            slot.EnsureMaterialAsset();

            // 半透明 submesh は影を落とさない。
            // WHY: バイザー・ガラス・髪の毛先のような ALPHA_BLEND のパーツが、
            //      シャドウマップでは完全な不透明として真っ黒な影を落としていた。
            //      見た目の誤りであると同時に、キャラ 1 体あたりの影 DrawCall を
            //      無駄に増やしている。Unity / Unreal も既定で半透明は影を落とさない。
            //      アルファテスト (切り抜き) は不透明扱いのままなので葉や柵は影を保つ。
            if (slot.GetBlendMode() != renderer::BlendMode::OPAQUE_BLEND) continue;

            // submesh 単位のテクセル未満カリング。
            // WHY: これまでカリング判定はモデル全体の bounds でしか行っておらず、
            //      目・歯・小さな装飾のようにシャドウマップ上で 1 テクセルにも満たない
            //      submesh まで、キャラ 1 体につき全て DrawCall を発行していた。
            //      キャラの submesh 数がそのまま影の描画数になっていた原因がこれ。
            uint32_t submeshMask         = cascadeMask;
            uint32_t submeshPunctualMask = punctualMask;
            if (meshPtr->boundsRadius > 0.0f) {
                const WorldBounds submeshBounds =
                    ComputeWorldBounds(go.transform, *meshPtr, ctx.cullingBoundsPadding);
                submeshMask         &= ComputeCascadeMask(ctx, cascadeCount, submeshBounds, true);
                submeshPunctualMask &= ComputePunctualMask(ctx, submeshBounds, true);
                if (submeshMask == 0 && submeshPunctualMask == 0) continue;
            }

            ShadowCaster caster;
            caster.cascadeMask  = submeshMask;
            caster.punctualMask = submeshPunctualMask;
            caster.depthKey     = depthKey;
            caster.objectId     = objectId;
            caster.indexBuffer  = meshPtr->indexBuffer;
            caster.indexCount   = meshPtr->indexCount;
            caster.world        = world;
            caster.lodDither    = smr->lodDither;

            // コンピュートスキニング済みなら「ただの静的メッシュ」として描く。
            // WHY: 変形は SkinningCompute パスで済んでいるので、VS でボーンを混ぜ直す必要がない。
            //      静的シェーダーは POSITION だけを読むうえ、頂点あたり 16 回の
            //      動的ボーン行列アクセスが丸ごと消える。
            const auto skinnedVB = smr->ResolveSlotSkinnedVertexBuffer(mi);
            if (skinnedVB.IsValid()) {
                caster.vertexBuffer = skinnedVB;
                caster.shader       = h.shadowShader;   // 静的メッシュ用 (スキニングなし)
            } else {
                // フォールバック: 従来どおり VS でスキニングする。
                caster.vertexBuffer = smr->ResolveSlotVertexBuffer(mi, meshPtr->vertexBuffer);
                caster.shader       = h.shadowSkinnedShader;
                caster.skinningCB   = skinCB;
            }
            outCasters.push_back(caster);
        }
    }
}

// EmitShadowCasters — 収集済み caster のうち、指定ビューに属するものを描画する。
//   punctual = false のとき cascadeMask の bit viewIndex を、true のとき punctualMask を見る。
// WHY: PerObjectCB は 1 本を使い回すので、更新と Submit は必ず交互でなければならない。
//      同一オブジェクトの連続 submesh に限り更新を省ける (world が同じため)。
// NOTE: 配列は呼び出し前にソート済みであること。並び順はカスケードによらず同じなので
//       ソートはフレームに 1 回でよい (ComputeLightDepthKey のコメント参照)。
void EmitShadowCasters(RenderPassContext& ctx,
                       const std::vector<ShadowCaster>& casters,
                       int viewIndex, bool punctual)
{
    auto& resources = ctx.resources;
    auto& h         = ctx.handles;

    const uint32_t viewBit = 1u << viewIndex;

    uint32_t lastObjectId  = 0;
    bool     hasLastObject = false;
    for (const ShadowCaster& caster : casters) {
        const uint32_t mask = punctual ? caster.punctualMask : caster.cascadeMask;
        if ((mask & viewBit) == 0) continue;

        if (!hasLastObject || caster.objectId != lastObjectId) {
            PerObjectCB objData{};
            objData.world          = caster.world;
            objData.objectParams.x = caster.lodDither;
            resources.Update(h.objectCB, &objData, sizeof(PerObjectCB));
            lastObjectId  = caster.objectId;
            hasLastObject = true;
        }

        renderer::DrawCall dc;
        dc.vertexBuffer       = caster.vertexBuffer;
        dc.indexBuffer        = caster.indexBuffer;
        dc.indexCount         = caster.indexCount;
        dc.shader             = caster.shader;
        dc.pipelineState      = h.defaultPSO;
        dc.constantBuffers[0] = h.frameCB;
        dc.constantBuffers[1] = h.objectCB;
        if (caster.skinningCB.IsValid())
            dc.constantBuffers[7] = caster.skinningCB;
        SubmitCountedShadow(ctx, dc);
    }
}

// RenderShadowCascade — カスケード 1 枚をアトラスの担当タイルへ描く。
// casters は全カスケード共通のソート済み配列。ここでは cascadeMask で絞って投げるだけ。
void RenderShadowCascade(RenderPassContext& ctx, const ShadowCascade& cascade,
                         int cascadeIndex, const std::vector<ShadowCaster>& casters)
{
    // このカスケードのタイルへビューポートを絞る。
    // NOTE: SetRenderTarget はビューポートを RT 全体へ戻すため、必ずその後に呼ぶ。
    ctx.renderer.SetViewport(cascade.viewportX, cascade.viewportY,
                             cascade.viewportSize, cascade.viewportSize);

    // シャドウ用 VS は b0 の viewProjection しか読まない。カスケードの行列へ差し替える。
    PerFrameCB lightFrameData{};
    lightFrameData.viewProjection = cascade.viewProjection;
    ctx.resources.Update(ctx.handles.frameCB, &lightFrameData, sizeof(PerFrameCB));

    EmitShadowCasters(ctx, casters, cascadeIndex, /*punctual=*/false);

    // Terrain は自前でチャンクを走査し、チャンク単位で objectCB を更新するため
    // caster 配列には混ぜない。地形は 1 枚の連続面で自己重なりが無く、
    // 並べ替えても Hi-Z の効きが変わらない。
    SubmitTerrainShadowCasters(ctx, cascade.frustum,
                               ctx.handles.shadowShader, ctx.handles.defaultPSO,
                               ctx.handles.frameCB, ctx.handles.objectCB);
}

// RenderPunctualShadowView — Spot / Point のタイル 1 枚を描く。
// カスケードとの違いはビューポートと行列の出どころだけで、提出の流れは同一。
void RenderPunctualShadowView(RenderPassContext& ctx, const PunctualShadowView& view,
                              int viewIndex, const std::vector<ShadowCaster>& casters)
{
    ctx.renderer.SetViewport(view.viewportX, view.viewportY,
                             view.viewportSize, view.viewportSize);

    PerFrameCB lightFrameData{};
    lightFrameData.viewProjection = view.viewProjection;
    ctx.resources.Update(ctx.handles.frameCB, &lightFrameData, sizeof(PerFrameCB));

    EmitShadowCasters(ctx, casters, viewIndex, /*punctual=*/true);

    // 地形も Spot / Point の影を落とす。屋内の床が地形でできているシーンでは、
    // ここを飛ばすと「壁の影は出るのに床には何も落ちない」ことになる。
    SubmitTerrainShadowCasters(ctx, view.frustum,
                               ctx.handles.shadowShader, ctx.handles.defaultPSO,
                               ctx.handles.frameCB, ctx.handles.objectCB);
}

} // namespace

void ExecuteShadowPass(RenderPassContext& ctx)
{
    auto& renderer  = ctx.renderer;
    auto& resources = ctx.resources;
    auto& h         = ctx.handles;
    const auto& rs  = ctx.settings;

    const int punctualViewCount =
        std::clamp(ctx.punctualShadowViewCount, 0, kMaxPunctualShadows);
    const bool hasPunctualAtlas = ctx.Res().Target("PunctualShadowMap").IsValid();

    // Spot / Point のアトラスも毎フレームまっさらにしてから始める。
    // WHY 本数 0 でもクリアするか: スロットの割り当てはフレームごとに変わる
    //     (カメラが動けば近い順が入れ替わる)。前フレームの深度が残っていると、
    //     割り当て直後の 1 フレームだけ別のライトの遮蔽を引いてしまう。
    if (hasPunctualAtlas) {
        renderer.SetRenderTarget(ctx.Res().Target("PunctualShadowMap"), resources);
        renderer.ClearDepth();
    }

    // クリアはアトラス全面へ 1 回。カスケードごとのビューポートを張る前に行う。
    renderer.SetRenderTarget(ctx.Res().Target("ShadowMap"), resources);
    renderer.ClearDepth();

    if (!rs.shadowEnabled) return;
    // 影の濃さが 0 のときは、面の陰影に深度が一切効かない
    // (Shadow.hlsli: lerp(1 - shadowStrength, 1, factor) が常に 1 を返す)。
    // ライトが影を落とさない設定でもシーン全体を光源視点で描き直していたので、
    // クリア済みの深度だけ残して caster の提出を丸ごと省く。
    // NOTE: 光芒 (VolumetricLight) だけは shadowStrength を通さず生の遮蔽を読むため、
    //       有効なときはこのスキップを行わない。切ると光芒から遮蔽が消えてしまう。
    // NOTE: ctx.shadowStrength は Directional の設定。Spot / Point は自前の
    //       スロットごとの強度を持つので、こちらが 0 でも描く必要がある。
    const bool needDirectional = (ctx.shadowStrength > 0.0f) || rs.volumetricLight.enabled;
    const bool needPunctual    = hasPunctualAtlas && punctualViewCount > 0;
    if (!needDirectional && !needPunctual) return;

    // NOTE: このスコープの `renderer` は ctx.renderer への参照なので、
    //       定数は名前空間から完全修飾で引く。
    const int cascadeCount =
        std::clamp(ctx.shadowCascadeCount, 1, fbzz::renderer::kMaxShadowCascades);

    // ── caster の収集はフレームに 1 回だけ ──────────────────────────────────
    // WHY: シーン走査・GetComponent・bounds 計算・ソートはカスケード間で結果が共有できる。
    //      カスケードごとに違うのは「どの錐台に入るか」だけなので、それを cascadeMask へ
    //      畳み込んでおけば、描画側はマスクを見て投げるだけで済む。
    //      分割数ぶん収集を繰り返す実装だと、ここが丸ごと 4 倍になっていた。
    // 収集バッファはフレームをまたいで使い回し、毎フレームのヒープ確保を避ける。
    // ShadowPass はレンダースレッド 1 本からしか呼ばれない (RenderGraph 直列実行)。
    static std::vector<ShadowCaster> casters;
    casters.clear();

    // 並び順はカスケードによらず同じなので、ソート基準はカスケード 0 の view でよい。
    const math::Matrix4& sortView = ctx.shadowCascades[0].view;
    CollectStaticMeshShadowCasters(ctx, cascadeCount, sortView, casters);
    CollectSkinnedMeshShadowCasters(ctx, cascadeCount, sortView, casters);

    // 光源に近い順。Hi-Z が後続の遮蔽フラグメントを捨てられるようにする。
    std::stable_sort(casters.begin(), casters.end(),
                     [](const ShadowCaster& a, const ShadowCaster& b) {
                         return a.depthKey < b.depthKey;
                     });

    if (needDirectional) {
        // クリアの後に punctual アトラスを触っている可能性があるので、束縛し直す。
        renderer.SetRenderTarget(ctx.Res().Target("ShadowMap"), resources);
        for (int i = 0; i < cascadeCount; ++i)
            RenderShadowCascade(ctx, ctx.shadowCascades[i], i, casters);
    }

    if (needPunctual) {
        renderer.SetRenderTarget(ctx.Res().Target("PunctualShadowMap"), resources);
        for (int i = 0; i < punctualViewCount; ++i)
            RenderPunctualShadowView(ctx, ctx.punctualShadowViews[i], i, casters);
    }

    // 後続パスがビューポートを RT 全体だと仮定してよいよう、タイル絞りを解除しておく。
    // WHY: SetRenderTarget を経由しない描画がこの直後に来ても壊れないようにするための保険。
    const uint32_t atlasResolution = (std::max)(rs.shadow.mapResolution, 1u);
    renderer.SetViewport(0, 0, atlasResolution, atlasResolution);
}

} // namespace fbzz::scene
