/// @file    FroxelFogPass.cpp
/// @brief   視錐台フロクセルへ霧を焼き、Z 方向へ積分する
/// @author  Hasegawa Jin
/// @date    2026-08-25
///
/// @note Inject → Integrate の 2 ディスパッチ。結果は `handles.froxelIntegrated` に残り、
///       Composite が深度からスライスを引いて 1 回サンプルする。
#include "PostProcessPasses.hpp"
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <Engine/Renderer/ComputeCall.hpp>
#include <Engine/Renderer/Camera.hpp>
#include <Math/Matrix4.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace fbzz::scene {

namespace {
/// @brief スライス内サンプル位置のディザ列。
/// @note 乱数でなく Halton 列にするのは、連続する数フレームが偏らずスライスを均等に埋める
///       ため。乱数だと同じ位置を続けて引いた瞬間に「板」が戻ってしまう。
float HaltonBase2(uint32_t index)
{
    float result = 0.0f;
    float f = 0.5f;
    for (uint32_t i = index + 1u; i > 0u; i >>= 1u) {
        if (i & 1u) result += f;
        f *= 0.5f;
    }
    return result;
}
} // namespace

void ExecuteFroxelFogPass(RenderPassContext& ctx)
{
    auto& renderer  = ctx.renderer;
    auto& resources = ctx.resources;
    auto& h         = ctx.handles;
    const auto& fog = ctx.settings.froxelFog;

    /// @note 走れない条件では霧を切った状態を b13 へ明示的に書いてから帰る。定数バッファの
    ///       束縛はドローをまたいで残るため、何も書かず帰ると Composite が前フレームの b13
    ///       (gridZ != 0) を読んだまま未束縛の t23 をサンプルし、透過率 0 で画面全体が真っ黒
    ///       になる (3D コンピュートテクスチャ未対応バックエンドも同じ経路)。froxelFogState
    ///       が無いフレームもここを通す必要があるため、null 判定は早期 return でなく canRun に畳む。
    const bool canRun =
        fog.enabled
        && ctx.froxelFogState != nullptr
        && h.froxelInjectCS.IsValid() && h.froxelIntegrateCS.IsValid()
        && h.froxelScatter.IsValid()  && h.froxelIntegrated.IsValid();

    if (!canRun) {
        if (h.froxelFogCB.IsValid()) {
            /// @note gridZ = 0 が「無効」の印
            const FroxelFogCB disabled{};
            resources.Update(h.froxelFogCB, &disabled, sizeof(FroxelFogCB));
        }
        /// @note 走らなかったフレームのぶん履歴が途切れる。次に有効化されたとき
        ///       古いボリュームを混ぜないよう、ここで無効化しておく。
        if (ctx.froxelFogState)
            ctx.froxelFogState->grid[0] = ctx.froxelFogState->grid[1]
                                        = ctx.froxelFogState->grid[2] = 0u;
        return;
    }
    if (!h.froxelFogCB.IsValid()) return;

    /// @note 履歴の引き直しに使う前フレームの行列とグリッド寸法。グリッドが変わった/霧を
    ///       切って入れ直したフレームは前の中身が今のグリッドと対応しないため履歴を捨てる
    ///       (寸法を 0 にするのが無効の印)。static で持てないのはフロクセルのグリッドが
    ///       カメラの視錐台に貼り付いているため。SceneView と GameView が 1 組を共有すると
    ///       相手のカメラ行列で履歴を引き直し、相手が今フレーム書いたボリュームを前フレーム
    ///       として読み、霧が視界の中でとぎれとぎれに明滅する。状態はビューが持つ。
    auto& state = *ctx.froxelFogState;

    const uint32_t gridX = (std::max)(fog.gridX, 1u);
    const uint32_t gridY = (std::max)(fog.gridY, 1u);
    const uint32_t gridZ = (std::max)(fog.gridZ, 1u);

    state.jitterIndex = (state.jitterIndex + 1u) & 31u;

    const math::Matrix4 viewProj =
        ctx.camera.GetProjectionMatrix() * ctx.camera.GetViewMatrix();

    const bool historyValid =
        state.grid[0] == gridX && state.grid[1] == gridY && state.grid[2] == gridZ;

    FroxelFogCB data{};
    data.invViewProj   = math::Matrix4::Inverse(viewProj);
    data.prevViewProj  = state.prevViewProjection;
    data.historyValid  = historyValid ? 1u : 0u;
    /// @note 今フレームの寄与率。指数移動平均なので実効窓は約 1/α フレーム。ジッター周期と
    ///       一致させるのは、スライス内ジッターが Halton 列を 32 個で一巡するため。窓が短い
    ///       と列を一巡ぶん平均できず平均値が列に沿って揺れ、光源近くは 1/d^2 の勾配が急
    ///       なのでそのまま明滅になる (以前の 1/8 は窓 8 フレームで周期の 1/4 だった)。窓を
    ///       伸ばすぶん動くものは尾を引くが、霧は低周波でカメラ移動は FBZZ_FroxelHistoryUVW
    ///       の再投影が吸収するため周期側に合わせる。履歴が無いフレームは全部を今フレームで埋める。
    data.historyBlend  = historyValid ? (1.0f / 32.0f) : 1.0f;

    state.prevViewProjection = viewProj;
    state.grid[0] = gridX; state.grid[1] = gridY; state.grid[2] = gridZ;
    data.cameraPos     = ctx.camera.m_position;
    data.nearDistance  = (std::max)(fog.nearDistance, 0.01f);
    data.farDistance   = (std::max)(fog.farDistance, data.nearDistance * 2.0f);
    data.albedo        = { fog.albedo[0], fog.albedo[1], fog.albedo[2] };
    data.emissive      = { fog.emissive[0], fog.emissive[1], fog.emissive[2] };
    data.density       = (std::max)(fog.density, 0.0f);
    /// @note g = ±1 は位相関数の分母が 0 に落ちる特異点。手前で止める。
    data.anisotropy    = std::clamp(fog.anisotropy, -0.95f, 0.95f);
    data.heightFalloff = (std::max)(fog.heightFalloff, 0.0f);
    data.heightStart   = fog.heightStart;
    data.jitter        = HaltonBase2(state.jitterIndex);
    data.gridX         = gridX;
    data.gridY         = gridY;
    data.gridZ         = gridZ;
    data.ambient       = (std::max)(fog.ambient, 0.0f);
    resources.Update(h.froxelFogCB, &data, sizeof(FroxelFogCB));

    /// @name 1. Inject
    renderer::ComputeCall inject;
    inject.shader              = h.froxelInjectCS;
    /// @note b0: view / cameraPos
    inject.constantBuffers[0]  = h.frameCB;
    /// @note b3: Directional (Legacy 時は点光源 / スポットも)
    inject.constantBuffers[3]  = h.lightCB;
    /// @note b4: カスケードシャドウ
    inject.constantBuffers[4]  = h.shadowCB;
    /// @note b12: Spot / Point の影と Cookie
    inject.constantBuffers[12] = h.punctualShadowCB;
    /// @note b13
    inject.constantBuffers[13] = h.froxelFogCB;

    /// @note ライトの供給元。束縛の規則は `BindForwardShadingResources` と同じにすること
    ///       (片方だけ変えると霧とサーフェスに映るライトの顔ぶれがずれる)。クラスタの Z
    ///       スライスはカメラ near 〜 clustered.maxDistance (既定 200m) を覆い、froxelFar
    ///       (既定 64m) がその外側まで伸びると奥のフロクセルは最終スライスへ丸められそこの
    ///       ライトを引くが、霧の最奥だけの話なので破綻はしない。
    if (ctx.clusterLightMode != ClusterLightMode::Legacy) {
        /// @note b9: 供給モード / グリッド係数
        inject.constantBuffers[9] = h.clusterCB;
        /// @note t29: 統合ライト配列
        inject.srvBuffers[29]     = h.punctualLightBuffer;
        if (ctx.clusterLightMode == ClusterLightMode::Clustered)
            /// @note t30: クラスタごとのライト番号
            inject.srvBuffers[30] = h.clusterIndexBuffer;
    }
    /// @note t8
    inject.srvInputs[8]        = resources.GetDepthTexture(ctx.Res().Target("ShadowMap"));
    /// @note t28
    inject.srvInputs[28]       = resources.GetDepthTexture(ctx.Res().Target("PunctualShadowMap"));
    /// @note t31
    inject.srvInputs[31]       = resources.GetColorTexture(ctx.Res().Target("LightCookieAtlas"), 0);
    if (data.historyValid != 0u)
        /// @note t20: 前フレームの散乱
        inject.srvInputs[20]   = h.froxelScatterHistory;
    /// @note u0
    inject.uavOutputs[0]       = h.froxelScatter;
    inject.dispatchX           = (gridX + 7) / 8;
    inject.dispatchY           = (gridY + 7) / 8;
    inject.dispatchZ           = gridZ;
    renderer.Dispatch(inject, resources);

    /// @name 2. Integrate
    /// @note Z 列ごとに直列積分するので dispatchZ は 1。
    renderer::ComputeCall integrate;
    integrate.shader              = h.froxelIntegrateCS;
    integrate.constantBuffers[13] = h.froxelFogCB;
    /// @note t20
    integrate.srvInputs[20]       = h.froxelScatter;
    /// @note u1
    integrate.uavOutputs[1]       = h.froxelIntegrated;
    integrate.dispatchX           = (gridX + 7) / 8;
    integrate.dispatchY           = (gridY + 7) / 8;
    integrate.dispatchZ           = 1;
    renderer.Dispatch(integrate, resources);
}


void FroxelFogPass::Setup(PassBuilder& builder, const RenderPassContext&) const
{
    builder.Read("ShadowMap").Read("PunctualShadowMap").Read("LightCookieAtlas");
}

void FroxelFogPass::Execute(PassResources&, RenderPassContext& ctx)
{
    ExecuteFroxelFogPass(ctx);
}
} // namespace fbzz::scene
