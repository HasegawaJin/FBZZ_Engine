/// @file    FluidGpuStep.hpp
/// @brief   GPU 流体ソルバーの 1 刻みぶんの定数 (FluidGpuCommon.hlsli の cbuffer と 1:1)
/// @author  Hasegawa Jin
/// @date    2026-09-11
///
/// @note 詰め方を純関数にする理由: 発生源の「今は出ているか」・散逸の刻み幅換算・seed のずらし方は
///       CPU ソルバー (FluidGasSolver) と同じ規則でなければならない。GPU の結果は読み戻さないため、
///       ずれてもテストでしか気付けない。
/// @note 部品の動き (FluidMotion) はここで CPU が評価し、中心と速度を「今の値」として詰める (GPU は動きを知らない)。
#pragma once

#include <Fluid/FluidRecipe.hpp>
#include <Math/Vector3.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace fbzz::fluid {

/// cbuffer に載せる部品の数 (レシピの上限と同じ)。
inline constexpr int kMaxFluidGpuSources = kMaxFluidSources;
inline constexpr int kMaxFluidGpuForces = kMaxFluidForces;
inline constexpr int kMaxFluidGpuColliders = kMaxFluidColliders;

struct FluidGpuSource {
    /// xyz 中心 (動き込み) / w 形 (FluidSourceShape: 0 球, 1 箱, 2 円錐, 3 輪, 4 テクスチャ, 5 カプセル, 6 円柱)
    float centerShape[4];
    float sizeNoise[4];    ///< xyz 大きさ (セル幅まで広げ済み。球は半径を 3 軸に複製) / w 注入のゆらぎ
    float amounts[4];      ///< x 密度 / y 温度 / z 燃料 [1/秒] / w 今の刻みで出ているか (0/1)
    float velocity[4];     ///< xyz 流速 (動きの速度込み) / w 流速を与えるか (0/1)
    /// xyz 円錐の向き・輪とテクスチャの法線・カプセルと円柱の軸 (正規化済み)
    /// / w テクスチャのアトラスのタイル番号 (他は −1)
    float axis[4];
    float extra[4];        ///< x 色の鍵 (FluidSource::colorKey) / yzw 未使用 (0)
};
static_assert(sizeof(FluidGpuSource) == 96);

struct FluidGpuForce {
    float centerType[4];         ///< xyz 中心 (動き込み) / w 種類 (FluidForceType)
    float directionStrength[4];  ///< xyz 向き (正規化済み) / w 強さ (効いていない刻みは 0)
    float params[4];             ///< x 半径 (0 以下は全域) / y 減衰の指数 / z ノイズの細かさ / w ノイズの速さ
};
static_assert(sizeof(FluidGpuForce) == 48);

struct FluidGpuCollider {
    /// xyz 中心 (動き込み) / w 形 (FluidColliderShape: 0 球, 1 箱, 2 平面, 3 カプセル, 4 円柱)
    float centerShape[4];
    float sizeActive[4];   ///< xyz 大きさ (セル幅まで広げ済み。球は半径を 3 軸に複製) / w 居るか (0/1)
    float normal[4];       ///< xyz 平面の法線・カプセルと円柱の軸 (正規化済み) / w 未使用 (0)
    float velocity[4];     ///< xyz 動きの速度 (inheritVelocity が false なら 0) / w 未使用 (0)
};
static_assert(sizeof(FluidGpuCollider) == 64);

struct alignas(16) FluidGpuStepConstants {
    std::uint32_t resolution;  float cellSize;          float dt;              float time;
    float buoyancy;            float weight;            float vorticity;       float turbulence;
    float turbulenceScale;     float densityKeep;       float temperatureKeep; float velocityKeep;
    float ignitionTemperature; float burnFraction;      float burnHeat;        float burnSmoke;
    float burnExpansion;       std::uint32_t floor;     std::uint32_t sharp;   std::uint32_t sourceCount;
    float wind[3];                                                            float densityScale;
    float noiseOffset[3];                                                     float temperatureScale;
    std::uint32_t forceCount;  std::uint32_t colliderCount; float countPad[2];
    FluidGpuSource sources[kMaxFluidGpuSources];
    FluidGpuForce forces[kMaxFluidGpuForces];
    FluidGpuCollider colliders[kMaxFluidGpuColliders];
};
static_assert(sizeof(FluidGpuStepConstants)
                  == 128 + 96 * kMaxFluidGpuSources + 48 * kMaxFluidGpuForces + 64 * kMaxFluidGpuColliders,
              "FluidGpuCommon.hlsli の FluidStepConstants と一致させること");

/// seed → ノイズ格子の切り出し位置 (FluidGasSolver::Reset と同じ式)。
[[nodiscard]] math::Vector3 FluidNoiseOffset(std::uint32_t seed);

/// 立方体の格子 (1 辺 resolution) で、time から dt 秒進める刻みの定数を作る。
/// densityScale / temperatureScale は結果をボリュームへ書き出すときの倍率 (FluidVolumeScale と同じ意味)。
/// 無効 (enabled = false) の部品は詰めない。上限を超えた部品は捨てる (レシピ側で上限を守らせる)。
/// テクスチャ発生源のタイル番号は FluidGpuMaskPaths が返す順と同じ。
[[nodiscard]] FluidGpuStepConstants PackFluidGpuStep(const FluidRecipe& recipe, int resolution, float time,
                                                     float dt, float densityScale, float temperatureScale);

/// GPU に詰める発生源のうち Texture のものの画像パスを、アトラスのタイル番号の順 (0, 1, ...) に返す。
/// FluidGpuSolver はこの順にマスクを読み、タイル (t % 4, t / 4) × kFluidSourceMaskSize に置く。
[[nodiscard]] std::vector<std::string> FluidGpuMaskPaths(const FluidRecipe& recipe);

} // namespace fbzz::fluid
