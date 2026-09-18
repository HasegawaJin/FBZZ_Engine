/// @file    ClusterLightPass.cpp
/// @brief   クラスタライトカリング (Forward+ / Deferred+) の実行パス。
/// @author  Hasegawa Jin
/// @date    2026-08-16
///
/// @note 視錐台を 32x18x24 のクラスタへ分割し、各クラスタへ影響するライトの番号を 1 回のディスパッチで集める。以降のライティングはそのピクセルが属するクラスタのライトだけを評価すればよい。
/// @note DX12 の Dispatch はディスクリプタコピーをキャッシュせず呼ぶたびに 40 回のコピーが走るため、クラスタ分割は画面全体で 1 度決まれば十分なこの性質を利用し Shadow より前に 1 回だけ実行する。
#include "GeometryPasses.hpp"
#include <Engine/Renderer/ComputeCall.hpp>

namespace fbzz::scene {

void ExecuteClusterLightCullPass(RenderPassContext& ctx)
{
    auto& resources = ctx.resources;
    auto& h         = ctx.handles;

    /// @note Clustered モード以外 (Legacy / Linear) ではリストを作る必要がない。Linear (全ライト線形評価の検証モード) もクラスタリストを引かないため対象外。
    if (ctx.clusterLightMode != ClusterLightMode::Clustered)
        return;
    if (!h.clusterCullCS.IsValid()
        || !h.punctualLightBuffer.IsValid()
        || !h.clusterIndexBuffer.IsValid()
        || !h.clusterCB.IsValid())
        return;

    /// @note b0 はこのビューのカメラで書いてから使う。このパスは Shadow より前に走るため、b0 には前フレームの ShadowPass の光源行列や別ビューのカメラなど直前に書かれた値が残っている。
    /// @note その行列でクラスタ AABB を切るとライトが別視点の画面位置に割り当てられ端のタイルにしか届かない (SceneView と GameView を同時に出すと必ず起きる)。b0 の中身は ForwardPasses が後で同じ値に書き直す。
    const PerFrameCB frameData = MakeCameraFrameCB(ctx.camera, ctx.taaJitterNdcX, ctx.taaJitterNdcY);
    resources.Update(h.frameCB, &frameData, sizeof(PerFrameCB));

    renderer::ComputeCall cc;
    cc.shader             = h.clusterCullCS;
    /// @note b0: view / projection (クラスタ AABB の逆投影に使う)
    cc.constantBuffers[0] = h.frameCB;
    /// @note b9: グリッド係数 / ライト本数
    cc.constantBuffers[9] = h.clusterCB;
    /// @note t14: `StructuredBuffer<PunctualLight>`
    cc.srvBuffers[14]     = h.punctualLightBuffer;
    /// @note u2 : `RWStructuredBuffer<uint>`
    cc.uavBuffers[0]      = h.clusterIndexBuffer;

    /// @note 1 スレッド = 1 クラスタ。ClusterLightCull.cs.hlsl の numthreads と一致させること。
    constexpr uint32_t kGroupSize = 64;
    cc.dispatchX = (kClusterCount + kGroupSize - 1) / kGroupSize;
    cc.dispatchY = 1;
    cc.dispatchZ = 1;

    ctx.renderer.Dispatch(cc, resources);
}


void ClusterLightCullPass::Execute(PassResources&, RenderPassContext& ctx)
{
    ExecuteClusterLightCullPass(ctx);
}
} // namespace fbzz::scene
