/// @file    FroxelFogPass.cpp
/// @brief   視錐台フロクセルへ霧を焼き、Z 方向へ積分する
/// @author  Hasegawa Jin
/// @date    2026-08-25
//
// Inject → Integrate の 2 ディスパッチ。結果は handles.froxelIntegrated に残り、
// Composite が深度からスライスを引いて 1 回サンプルする。
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
// スライス内サンプル位置のディザ列。
// WHY 乱数でなく Halton 列か: 連続する数フレームが偏らずスライスを均等に埋める。
//     乱数だと同じ位置を続けて引いた瞬間に「板」が戻ってしまう。
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

    // 走れない条件では「霧を切った状態」を b13 へ明示的に書いてから帰る。
    //
    // WHY 何も書かずに帰ってはいけないか: 定数バッファの束縛はドローをまたいで残る。
    //     霧を一度有効にした後で切ると、Composite は前フレームの b13 (gridZ != 0) を
    //     読んだまま未束縛の t23 をサンプルし、透過率 0 = 画面全体が真っ黒になる。
    //     3D コンピュートテクスチャ未対応のバックエンドでも同じ経路を通る。
    //     froxelFogState が無いフレームもここを通す必要があるため、null 判定は
    //     早期 return ではなく canRun に畳む。
    const bool canRun =
        fog.enabled
        && ctx.froxelFogState != nullptr
        && h.froxelInjectCS.IsValid() && h.froxelIntegrateCS.IsValid()
        && h.froxelScatter.IsValid()  && h.froxelIntegrated.IsValid();

    if (!canRun) {
        if (h.froxelFogCB.IsValid()) {
            const FroxelFogCB disabled{};  // gridZ = 0 が「無効」の印
            resources.Update(h.froxelFogCB, &disabled, sizeof(FroxelFogCB));
        }
        // 走らなかったフレームのぶん履歴が途切れる。次に有効化されたとき
        // 古いボリュームを混ぜないよう、ここで無効化しておく。
        if (ctx.froxelFogState)
            ctx.froxelFogState->grid[0] = ctx.froxelFogState->grid[1]
                                        = ctx.froxelFogState->grid[2] = 0u;
        return;
    }
    if (!h.froxelFogCB.IsValid()) return;

    // 履歴の引き直しに使う前フレームの行列とグリッド寸法。
    // グリッドが変わった / 霧を切って入れ直したフレームは、前の中身が今のグリッドと
    // 対応しないので履歴を捨てる (寸法を 0 にするのが「無効」の印)。
    //
    // WHY static で持てないか: フロクセルのグリッドはカメラの視錐台に貼り付いている。
    //      SceneView と GameView が 1 組を共有すると、相手のカメラ行列で履歴を引き直し、
    //      相手が今フレーム書いたボリュームを「前フレーム」として読むことになる。
    //      霧が視界の中でとぎれとぎれに明滅する。状態はビューが持つ。
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
    // 今フレームの寄与率。指数移動平均なので実効窓は約 1/α フレーム。
    //
    // WHY ジッター周期と一致させるか: スライス内のジッターは Halton 列を 32 個で
    //     一巡させている。窓がそれより短いと列を一巡ぶん平均できず、平均値そのものが
    //     列に沿って揺れ続ける。以前の 1/8 は窓が 8 フレームしかなく、周期の 1/4 だった。
    //     光源の近くは 1/d^2 の勾配が急なので、この取りこぼしがそのまま明滅になる。
    // NOTE: 窓を伸ばすぶん動くものは尾を引く。霧は低周波なうえ、カメラの移動は
    //       FBZZ_FroxelHistoryUVW の再投影が吸収するので、実用上は周期側に合わせる。
    // 履歴が無いフレームは全部を今フレームで埋める (残像から始めない)。
    data.historyBlend  = historyValid ? (1.0f / 32.0f) : 1.0f;

    state.prevViewProjection = viewProj;
    state.grid[0] = gridX; state.grid[1] = gridY; state.grid[2] = gridZ;
    data.cameraPos     = ctx.camera.m_position;
    data.nearDistance  = (std::max)(fog.nearDistance, 0.01f);
    data.farDistance   = (std::max)(fog.farDistance, data.nearDistance * 2.0f);
    data.albedo        = { fog.albedo[0], fog.albedo[1], fog.albedo[2] };
    data.emissive      = { fog.emissive[0], fog.emissive[1], fog.emissive[2] };
    data.density       = (std::max)(fog.density, 0.0f);
    // g = ±1 は位相関数の分母が 0 に落ちる特異点。手前で止める。
    data.anisotropy    = std::clamp(fog.anisotropy, -0.95f, 0.95f);
    data.heightFalloff = (std::max)(fog.heightFalloff, 0.0f);
    data.heightStart   = fog.heightStart;
    data.jitter        = HaltonBase2(state.jitterIndex);
    data.gridX         = gridX;
    data.gridY         = gridY;
    data.gridZ         = gridZ;
    data.ambient       = (std::max)(fog.ambient, 0.0f);
    resources.Update(h.froxelFogCB, &data, sizeof(FroxelFogCB));

    // ---- 1. Inject ----
    renderer::ComputeCall inject;
    inject.shader              = h.froxelInjectCS;
    inject.constantBuffers[0]  = h.frameCB;           // b0: view / cameraPos
    inject.constantBuffers[3]  = h.lightCB;           // b3: Directional (Legacy 時は点光源 / スポットも)
    inject.constantBuffers[4]  = h.shadowCB;          // b4: カスケードシャドウ
    inject.constantBuffers[12] = h.punctualShadowCB;  // b12: Spot / Point の影と Cookie
    inject.constantBuffers[13] = h.froxelFogCB;       // b13

    // ライトの供給元。束縛の規則は BindForwardShadingResources と同じにすること。
    // 片方だけ変えると、同じシーンで霧とサーフェスに映るライトの顔ぶれがずれる。
    //
    // NOTE: クラスタの Z スライスはカメラ near 〜 clustered.maxDistance (既定 200m) を覆う。
    //       froxelFar (既定 64m) がその外側まで伸びた場合、奥のフロクセルは最終スライスへ
    //       丸められ、そこのライトを引く。霧の最奥だけの話なので破綻はしない。
    if (ctx.clusterLightMode != ClusterLightMode::Legacy) {
        inject.constantBuffers[9] = h.clusterCB;              // b9: 供給モード / グリッド係数
        inject.srvBuffers[29]     = h.punctualLightBuffer;    // t29: 統合ライト配列
        if (ctx.clusterLightMode == ClusterLightMode::Clustered)
            inject.srvBuffers[30] = h.clusterIndexBuffer;     // t30: クラスタごとのライト番号
    }
    inject.srvInputs[8]        = resources.GetDepthTexture(ctx.Res().Target("ShadowMap"));       // t8
    inject.srvInputs[28]       = resources.GetDepthTexture(ctx.Res().Target("PunctualShadowMap"));  // t28
    inject.srvInputs[31]       = resources.GetColorTexture(ctx.Res().Target("LightCookieAtlas"), 0);  // t31
    if (data.historyValid != 0u)
        inject.srvInputs[20]   = h.froxelScatterHistory;  // t20: 前フレームの散乱
    inject.uavOutputs[0]       = h.froxelScatter;     // u0
    inject.dispatchX           = (gridX + 7) / 8;
    inject.dispatchY           = (gridY + 7) / 8;
    inject.dispatchZ           = gridZ;
    renderer.Dispatch(inject, resources);

    // ---- 2. Integrate ----
    // Z 列ごとに直列積分するので dispatchZ は 1。
    renderer::ComputeCall integrate;
    integrate.shader              = h.froxelIntegrateCS;
    integrate.constantBuffers[13] = h.froxelFogCB;
    integrate.srvInputs[20]       = h.froxelScatter;      // t20
    integrate.uavOutputs[1]       = h.froxelIntegrated;   // u1
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
