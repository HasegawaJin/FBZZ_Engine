// FBZZ Engine
// Material/Effects/ParticleGpuSortLocal.cs.hlsl | Compute Shader
// bitonic sort の内側全段 (比較距離 j がグループ幅の半分以下) をグループ共有メモリで一気に回す
//
// dispatch: ceil(paddedCount / 256) × 1 × 1 を、外側ステージ k ごとに 1 回だけ
// スロット: b0 = GpuParticleSortCB / u3 = RWStructuredBuffer<uint2>
//
// WHY: j <= BLOCK/2 のとき比較相手 (i ^ j) は必ず同じ 256 要素ブロック内に収まる。
//      この範囲は 1 グループで閉じるので、j を 1 まで下げる全段をグローバル往復なしに処理できる。
//      素朴な実装では毎段ディスパッチが要り、10 万粒子で 150 回超になってソート自体が重くなる。

#include "Common/Binding.hlsli"
#include "Rendering/ParticleSortCommon.hlsli"

groupshared uint2 gBlock[PARTICLE_SORT_BLOCK];

[numthreads(PARTICLE_SORT_BLOCK, 1, 1)]
void CSMain(uint3 dispatchId : SV_DispatchThreadID, uint3 groupThreadId : SV_GroupThreadID)
{
    const uint global = dispatchId.x;
    const uint local  = groupThreadId.x;

    // 範囲外は最大キーで埋める。実バッファは 2 のべき乗まで確保してあるので通常は起きないが、
    // ここが未初期化だとブロック内の比較が不定値を掴んで順序が壊れる。
    gBlock[local] = global < gSortPaddedCount ? gSortEntries[global] : uint2(0xFFFFFFFFu, global);
    GroupMemoryBarrierWithGroupSync();

    [loop]
    for (uint j = gSortStageJ; j > 0u; j >>= 1u)
    {
        const uint partner = local ^ j;
        // 読みと書きを分ける。両方のスレッドが読み終わる前に片方が書くと、
        // 相手が書き換え後の値を掴んで入れ替えが壊れる。
        const uint2 mine  = gBlock[local];
        const uint2 other = gBlock[partner];
        GroupMemoryBarrierWithGroupSync();

        if (partner > local
            && ParticleSortShouldSwap(mine.x, other.x, ParticleSortAscending(global)))
        {
            gBlock[local]   = other;
            gBlock[partner] = mine;
        }
        GroupMemoryBarrierWithGroupSync();
    }

    if (global < gSortPaddedCount) gSortEntries[global] = gBlock[local];
}
