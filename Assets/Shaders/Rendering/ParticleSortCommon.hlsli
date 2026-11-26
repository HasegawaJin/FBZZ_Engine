/// @file    ParticleSortCommon.hlsli
/// @brief   GPU パーティクルのソートで 3 本の CS が共有する定数と規約
/// @author  Hasegawa Jin
/// @date    2026-08-12
///
/// @note キー生成 / bitonic のグローバル段 / bitonic の LDS 段 は別ファイル (.cs.hlsl は
/// @note エントリ 1 本のため) だが、キーの意味と定数バッファのレイアウトがずれると
/// @note 「ソートしたのに順序が違う」という、絵からは原因を特定できない壊れ方をする。
/// @note 共有できるものは全てここへ置き、3 本が同じ定義を見るようにする。
#ifndef PARTICLE_SORT_COMMON_HLSLI
#define PARTICLE_SORT_COMMON_HLSLI

#include "Common/Binding.hlsli"
#include "Common/BindlessIndices.hlsli"

/// @note LDS 段の 1 グループが担当する要素数。スレッド数と一致させること。
/// @note この値の半分までの j (比較距離) はグループ内で完結するため、グローバル往復を省ける。
#define PARTICLE_SORT_BLOCK 256

cbuffer GpuParticleSortCB : register(FBZZ_REGISTER(b, CB_PARTICLE_SORT_SLOT))
{
    float3 gSortCameraPos;
    uint   gSortAliveCount;   /// @note = maxParticles。これ以上の index は詰め物
    uint   gSortPaddedCount;  /// @note 2 のべき乗へ切り上げた総要素数
    uint   gSortBackToFront;  /// @note 1 = 遠い順に描く (半透明の正しい合成)
    uint   gSortStageK;       /// @note bitonic の外側ステージ幅
    uint   gSortStageJ;       /// @note bitonic の比較距離。LDS 段ではここから 1 まで一気に処理する
};

/// @note ソート対象 1 件。x = キー (昇順に並べる)、y = 粒子インデックス。
/// @note キーの作り方は ParticleGpuSortKeys.cs.hlsl を参照。死亡・詰め物は 0xFFFFFFFF で必ず末尾へ落ちる。
FBZZ_RWSBUFFER_T(uint2, gSortEntries, UAV_GPU_SORT_SLOT);

/// @note bitonic ネットワークの向き。i の k ビットが 0 の区間は昇順、1 の区間は降順に組む。
bool ParticleSortAscending(uint index)
{
    return (index & gSortStageK) == 0u;
}

/// @note 昇順キーで見て入れ替えるべきか。
bool ParticleSortShouldSwap(uint keyA, uint keyB, bool ascending)
{
    return (keyA > keyB) == ascending;
}

#endif /// @note PARTICLE_SORT_COMMON_HLSLI
