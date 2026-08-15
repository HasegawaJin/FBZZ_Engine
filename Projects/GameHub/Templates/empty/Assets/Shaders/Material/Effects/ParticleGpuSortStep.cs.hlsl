// FBZZ Engine
// Material/Effects/ParticleGpuSortStep.cs.hlsl | Compute Shader
// bitonic sort のグローバル 1 段 (比較距離 j がグループ幅を超える場合)
//
// dispatch: ceil(paddedCount / 256) × 1 × 1 を (k, j) の組ごとに 1 回
// スロット: b0 = GpuParticleSortCB / u3 = RWStructuredBuffer<uint2>
//
// WHY: j がグループ幅の半分を超えると比較相手が別グループへ出るため、
//      グループ共有メモリでは同期が取れずグローバルメモリを往復するしかない。
//      j がグループ内へ収まったら ParticleGpuSortLocal.cs.hlsl へ切り替える
//      (残り全段を LDS で回すので、ディスパッチ数が log2(n)^2/2 から大幅に減る)。

#include "Common/Binding.hlsli"
#include "Rendering/ParticleSortCommon.hlsli"

[numthreads(PARTICLE_SORT_BLOCK, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    uint i = id.x;
    if (i >= gSortPaddedCount) return;

    // 各ペアは片側のスレッドだけが処理する。両方が処理すると入れ替えが二重に起きて元へ戻る。
    uint partner = i ^ gSortStageJ;
    if (partner <= i || partner >= gSortPaddedCount) return;

    uint2 a = gSortEntries[i];
    uint2 b = gSortEntries[partner];
    if (ParticleSortShouldSwap(a.x, b.x, ParticleSortAscending(i)))
    {
        gSortEntries[i]       = b;
        gSortEntries[partner] = a;
    }
}
