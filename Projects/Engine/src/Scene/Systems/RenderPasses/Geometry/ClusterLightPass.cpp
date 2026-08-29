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
