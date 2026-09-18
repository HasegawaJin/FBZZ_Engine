/// @file    FluidGpuLiquidPack.hpp
/// @brief   GPU 液体ソルバー (FluidGpuLiquidSolver) へ上げる値を CPU で決める純関数 (湧かせ方・核の定数・格子・部品)
/// @author  Hasegawa Jin
/// @date    2026-09-12
///
/// @note GPU の結果は読み戻さないので、湧く時刻・位置・静止密度が CPU ソルバー (FluidLiquidSolver)
///       の規則からずれても絵でしか気付けない。ここだけ純関数に切り出しテストで縛る。
#pragma once

#include <Fluid/FluidGpuStep.hpp>
#include <Fluid/FluidRecipe.hpp>

#include <cstdint>
#include <vector>

namespace fbzz::asset {

/// 粒子 1 つぶんの湧かせ方。LiquidCommon.hlsli の gSpawn (float4 × 2) と 1:1。
struct GpuLiquidSpawn {
    float positionTime[4];  ///< xyz 湧く位置 / w 湧く時刻 [秒] (湧かない枠は 3e38)
    float velocityKey[4];   ///< xyz 初速 (ばらつき・動きの速度込み) / w 色の鍵 (fluid::FluidSource::colorKey)
};
static_assert(sizeof(GpuLiquidSpawn) == 32);

/// PBF の核の定数。FluidLiquidSolver::Reset (volumetric = true) と同じ式・同じ演算順で求める。
struct GpuLiquidKernel {
    float radius = 0.01f;           ///< 粒子半径 (0.002〜0.1 に丸めた値)
    float h = 0.04f;                ///< 核の半径 = radius × 4
    float poly6 = 0.0f;
    float spikyGradient = 0.0f;
    float restDensity = 1.0f;
    float relaxation = 0.0f;
    float tensileReference = 1.0f;
    float tensileScale = 0.0f;
};

/// 近傍探索の一様格子。範囲は CPU と同じ x/z ∈ [−1.6, 1.6]、y ∈ [−1.6, 3.0]。セルの幅は h。
struct GpuLiquidGrid {
    float cellSize = 0.04f;
    float boundsMin[3] = { -1.6f, -1.6f, -1.6f };
    float boundsMax[3] = { 1.6f, 3.0f, 1.6f };
    int cellsX = 1;
    int cellsY = 1;
    int cellsZ = 1;
};

/// 並べ替え (bitonic sort) の 1 段。local = true の段は j から 1 までをグループ共有メモリで一気に回す。
struct GpuLiquidSortStage {
    std::uint32_t k = 2;
    std::uint32_t j = 1;
    bool local = true;
};

/// bitonic sort の 1 グループの要素数 (LiquidSort.hlsli の LIQUID_SORT_BLOCK と一致させること)。
inline constexpr std::uint32_t kGpuLiquidSortBlock = 256;

[[nodiscard]] float GpuLiquidParticleRadius(const fluid::FluidLiquidSettings& settings);
[[nodiscard]] GpuLiquidKernel MakeGpuLiquidKernel(float particleRadius);
[[nodiscard]] GpuLiquidGrid MakeGpuLiquidGrid(float particleRadius);

/// 粒子の枠の数 = min(liquid.maxParticles (1 以上), limit, 有効な発生源 (先頭 kMaxFluidSources 個) の count の和)。
[[nodiscard]] int GpuLiquidParticleCapacity(const fluid::FluidRecipe& recipe, int limit);

/// 全粒子ぶんの湧かせ方を決める。要素数は GpuLiquidParticleCapacity と同じ。湧く時刻の昇順に並ぶ。
/// 発生源の k 番目 (0 始まり) の粒子は duration <= 0 なら startTime、正なら startTime + duration × (k + 1) / count に湧く
/// (FluidLiquidSolver::Emit の «count × 経過割合» を切り捨てた数だけ出す規則を時刻へ直したもの)。
/// 枠が足りないときは早く湧く粒子から採る (同時刻は発生源の順)。位置・初速・詰めすぎの広げ方は CPU と同じ規則で、
/// 乱数は recipe.seed から決まる (同じレシピなら同じ結果)。Texture 発生源はマスクを読み込んで選り分ける。
[[nodiscard]] std::vector<GpuLiquidSpawn> BuildGpuLiquidEmission(const fluid::FluidRecipe& recipe, int limit);

/// time で効いている力を詰める (有効な部品を先頭から kMaxFluidGpuForces まで。効いていない刻みは強さ 0)。@return 詰めた数。
int PackGpuLiquidForces(const fluid::FluidRecipe& recipe, float time, fluid::FluidGpuForce (&out)[fluid::kMaxFluidGpuForces]);

/// time の障害物を詰める。大きさは広げない (液体は minSize = 0 で測る。球は半径を 3 軸に複製)。
/// velocity.w に «表面に沿った速度を残す割合» exp(−max(friction, 0) × 10 × dt) を入れる。@return 詰めた数。
int PackGpuLiquidColliders(const fluid::FluidRecipe& recipe, float time, float dt,
                           fluid::FluidGpuCollider (&out)[fluid::kMaxFluidGpuColliders]);

/// 並べ替える要素数 (particleCount 以上の 2 のべき乗。下限 kGpuLiquidSortBlock)。
[[nodiscard]] std::uint32_t GpuLiquidSortCount(int particleCount);
/// sortCount (2 のべき乗) を昇順に並べる bitonic sort の段の並び。
[[nodiscard]] std::vector<GpuLiquidSortStage> BuildGpuLiquidSortStages(std::uint32_t sortCount);

} // namespace fbzz::asset
