/// @file    RenderPasses/Geometry/ClusterLightPass.cpp
/// @brief   クラスタライトカリング (Forward+ / Deferred+) の実行パス。
/// @author  Hasegawa Jin
/// @date    2026-08-16
///
/// 視錐台を 32x18x24 のクラスタへ分割し、各クラスタへ影響するライトの番号を
/// 1 回のディスパッチで集める。以降のライティングは、そのピクセルが属するクラスタの
/// ライトだけを評価すればよくなる。
///
/// WHY 1 フレーム 1 ディスパッチで済ませるか: DX12 の Dispatch はテクスチャ/UAV テーブルの
/// ディスクリプタコピーをキャッシュしないため、呼ぶたびに 40 回のコピーが走る。
/// クラスタ分割は画面全体で 1 度決まれば十分なので、Shadow より前に 1 回だけ実行する。
#include "GeometryPasses.hpp"
#include <Engine/Renderer/ComputeCall.hpp>

namespace fbzz::scene {

void ExecuteClusterLightCullPass(RenderPassContext& ctx)
{
    auto& resources = ctx.resources;
    auto& h         = ctx.handles;

    // Clustered モード以外 (Legacy / Linear) ではリストを作る必要がない。
    // WHY Linear も除外するか: 全ライトを線形評価する検証モードでは、そもそも
    //     クラスタリストを引かないため、作っても誰も読まない。
    if (ctx.clusterLightMode != ClusterLightMode::Clustered)
        return;
    if (!h.clusterCullCS.IsValid()
        || !h.punctualLightBuffer.IsValid()
        || !h.clusterIndexBuffer.IsValid()
        || !h.clusterCB.IsValid())
        return;

    // b0 をこのビューのカメラで書いてから使う。
    // WHY ここで書くか: このパスは Shadow より前に走るので、b0 には直前に書いた誰か
    //     (前フレームの ShadowPass の光源行列、もう片方のビューのカメラ、プローブ捕捉) が
    //     残っている。その行列でクラスタ AABB を切ると、ライトは別の視点の画面位置に
    //     割り当てられ、このビューでは端のタイルにしか届かない (SceneView と GameView を
    //     同時に出すと必ず起きる)。b0 の中身は ForwardPasses が後で同じ値に書き直す。
    const PerFrameCB frameData = MakeCameraFrameCB(ctx.camera, ctx.taaJitterNdcX, ctx.taaJitterNdcY);
    resources.Update(h.frameCB, &frameData, sizeof(PerFrameCB));

    renderer::ComputeCall cc;
    cc.shader             = h.clusterCullCS;
    cc.constantBuffers[0] = h.frameCB;    // b0: view / projection (クラスタ AABB の逆投影に使う)
    cc.constantBuffers[9] = h.clusterCB;  // b9: グリッド係数 / ライト本数
    cc.srvBuffers[14]     = h.punctualLightBuffer;  // t14: StructuredBuffer<PunctualLight>
    cc.uavBuffers[0]      = h.clusterIndexBuffer;   // u2 : RWStructuredBuffer<uint>

    // 1 スレッド = 1 クラスタ。ClusterLightCull.cs.hlsl の numthreads と一致させること。
    constexpr uint32_t kGroupSize = 64;
    cc.dispatchX = (kClusterCount + kGroupSize - 1) / kGroupSize;
    cc.dispatchY = 1;
    cc.dispatchZ = 1;

    ctx.renderer.Dispatch(cc, resources);
}

} // namespace fbzz::scene
