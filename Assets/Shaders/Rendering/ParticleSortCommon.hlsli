// FBZZ Engine
// Rendering/ParticleSortCommon.hlsli | Common
// GPU パーティクルのソートで 3 本の CS が共有する定数と規約
//
// WHY: キー生成 / bitonic のグローバル段 / bitonic の LDS 段 は別ファイル (.cs.hlsl は
//      エントリ 1 本のため) だが、キーの意味と定数バッファのレイアウトがずれると
//      「ソートしたのに順序が違う」という、絵からは原因を特定できない壊れ方をする。
//      共有できるものは全てここへ置き、3 本が同じ定義を見るようにする。
#ifndef PARTICLE_SORT_COMMON_HLSLI
#define PARTICLE_SORT_COMMON_HLSLI

#include "Common/Binding.hlsli"
#include "Common/BindlessIndices.hlsli"

// LDS 段の 1 グループが担当する要素数。スレッド数と一致させること。
// この値の半分までの j (比較距離) はグループ内で完結するため、グローバル往復を省ける。
#define PARTICLE_SORT_BLOCK 256

cbuffer GpuParticleSortCB : register(b0)
{
    float3 gSortCameraPos;
    uint   gSortAliveCount;   // = maxParticles。これ以上の index は詰め物
    uint   gSortPaddedCount;  // 2 のべき乗へ切り上げた総要素数
    uint   gSortBackToFront;  // 1 = 遠い順に描く (半透明の正しい合成)
    uint   gSortStageK;       // bitonic の外側ステージ幅
    uint   gSortStageJ;       // bitonic の比較距離。LDS 段ではここから 1 まで一気に処理する
};

// ソート対象 1 件。x = キー (昇順に並べる)、y = 粒子インデックス。
// キーの作り方は ParticleGpuSortKeys.cs.hlsl を参照。死亡・詰め物は 0xFFFFFFFF で必ず末尾へ落ちる。
FBZZ_RWSBUFFER_T(uint2, gSortEntries, UAV_GPU_SORT_SLOT);

// bitonic ネットワークの向き。i の k ビットが 0 の区間は昇順、1 の区間は降順に組む。
bool ParticleSortAscending(uint index)
{
    return (index & gSortStageK) == 0u;
}

// 昇順キーで見て入れ替えるべきか。
bool ParticleSortShouldSwap(uint keyA, uint keyB, bool ascending)
{
    return (keyA > keyB) == ascending;
}

#endif // PARTICLE_SORT_COMMON_HLSLI
