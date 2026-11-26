/// @file    FluidGpuStep.hpp
/// @brief   GPU 流体ソルバーの 1 刻みぶんの定数 (FluidGpuCommon.hlsli の cbuffer と 1:1)
/// @author  Hasegawa Jin
/// @date    2026-09-11

/// @note 詰め方を純関数にする理由: 発生源の「今は出ているか」・散逸の刻み幅換算・seed のずらし方は
/// @note CPU ソルバー (FluidGasSolver) と同じ規則でなければならない。GPU の結果は読み戻さないため、
/// @note ずれてもテストでしか気付けない。
/// @note 部品の動き (FluidMotion) はここで CPU が評価し、中心と速度を「今の値」として詰める (GPU は動きを知らない)。
#pragma once
#include <Fluid/Api.hpp>

#include <Fluid/Recipe/FluidRecipe.hpp>
#include <Math/LinearAlgebra/Vector3.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace fbzz::fluid {

/// @note cbuffer に載せる部品の数 (レシピの上限と同じ)。
inline constexpr int kMaxFluidGpuSources = kMaxFluidSources;
inline constexpr int kMaxFluidGpuForces = kMaxFluidForces;
inline constexpr int kMaxFluidGpuColliders = kMaxFluidColliders;

struct FluidGpuSource {
    float centerShape[4]; ///< xyz 中心 (動き込み) / w 形 (FluidSourceShape: 0 球, 1 箱, 2 円錐, 3 輪, 4 テクスチャ, 5 カプセル, 6 円柱)
    float sizeNoise[4];    ///< @note xyz 大きさ (セル幅まで広げ済み。球は半径を 3 軸に複製) / w 注入のゆらぎ
    float amounts[4];      ///< @note x 密度 / y 温度 / z 燃料 [1/秒] / w 今の刻みで出ているか (0/1)
    float velocity[4];     ///< @note xyz 流速 (動きの速度込み) / w 流速を与えるか (0/1)
    float axis[4]; ///< xyz 円錐の向き・輪とテクスチャの法線・カプセルと円柱の軸 (正規化済み) / w テクスチャのアトラスのタイル番号 (他は −1)
    float extra[4];        ///< @note x 色の鍵 (FluidSource::colorKey) / yzw 未使用 (0)
};
static_assert(sizeof(FluidGpuSource) == 96);

struct FluidGpuForce {
    float centerType[4];         ///< @note xyz 中心 (動き込み) / w 種類 (FluidForceType)
    float directionStrength[4];  ///< @note xyz 向き (正規化済み) / w 強さ (効いていない刻みは 0)
    float params[4];             ///< @note x 半径 (0 以下は全域) / y 減衰の指数 / z ノイズの細かさ / w ノイズの速さ
};
static_assert(sizeof(FluidGpuForce) == 48);

struct FluidGpuCollider {
    float centerShape[4]; ///< xyz 中心 (動き込み) / w 形 (FluidColliderShape: 0 球, 1 箱, 2 平面, 3 カプセル, 4 円柱)
    float sizeActive[4];   ///< @note xyz 大きさ (セル幅まで広げ済み。球は半径を 3 軸に複製) / w 居るか (0/1)
    float normal[4];       ///< @note xyz 平面の法線・カプセルと円柱の軸 (正規化済み) / w 未使用 (0)
    float velocity[4];     ///< @note xyz 動きの速度 (inheritVelocity が false なら 0) / w 未使用 (0)
};
static_assert(sizeof(FluidGpuCollider) == 64);

struct alignas(16) FluidGpuStepConstants {
    std::uint32_t resolution; ///< 立方格子の一辺のセル数。
    float cellSize; ///< 正規化領域での隣接セル中心間の距離。
    float dt; ///< この刻みで進める時間 [秒]。
    float time; ///< 部品やノイズを評価する刻み開始時のシミュレーション時刻 [秒]。
    float buoyancy; ///< 温度から上向き加速度を求める倍率。
    float weight; ///< 煙の密度から下向き加速度を求める倍率。
    float vorticity; ///< 数値拡散で弱まった渦の回転を補う強さ。
    float turbulence; ///< カールノイズから加える乱流の強さ。
    float turbulenceScale; ///< 乱流ノイズの空間周波数。
    float densityKeep; ///< この刻みの散逸後に残す密度の割合。
    float temperatureKeep; ///< この刻みの散逸後に残す温度の割合。
    float velocityKeep; ///< この刻みの減衰後に残す速度の割合。
    float ignitionTemperature; ///< 燃料の燃焼を開始する温度の下限。
    float burnFraction; ///< 着火温度に達した燃料のうち、この刻みで燃やす割合。
    float burnHeat; ///< 燃料の燃焼によって加える熱量の倍率。
    float burnSmoke; ///< 燃料の燃焼によって加える煙の密度の倍率。
    float burnExpansion; ///< 燃料の燃焼によって加える目標発散の倍率。
    std::uint32_t floor; ///< 格子の下端を床として閉じるなら 1、開放するなら 0。
    std::uint32_t sharp; ///< MacCormack の移流補正を使うなら 1、使わなければ 0。
    std::uint32_t sourceCount; ///< sources の先頭から詰めた発生源の数。
    float wind[3]; ///< 正規化領域の局所 XYZ 方向に加える風の加速度。
    float densityScale; ///< 密度を出力ボリュームへ書くときの倍率。
    float noiseOffset[3]; ///< 種に応じてノイズの XYZ 評価座標へ加えるオフセット。
    float temperatureScale; ///< 温度を出力ボリュームへ書くときの倍率。
    std::uint32_t forceCount; ///< forces の先頭から詰めた外力の数。
    std::uint32_t colliderCount; ///< colliders の先頭から詰めた障害物の数。
    float countPad[2]; ///< 次の配列を HLSL の 16 バイト境界へ配置するための余白。
    FluidGpuSource sources[kMaxFluidGpuSources]; ///< GPU が刻み中に評価する発生源と注入内容。
    FluidGpuForce forces[kMaxFluidGpuForces]; ///< GPU が刻み中に速度へ反映する外力。
    FluidGpuCollider colliders[kMaxFluidGpuColliders]; ///< GPU が刻み中に固体セルを構成するための障害物。
};
static_assert(sizeof(FluidGpuStepConstants)
                  == 128 + 96 * kMaxFluidGpuSources + 48 * kMaxFluidGpuForces + 64 * kMaxFluidGpuColliders,
              "FluidGpuCommon.hlsli の FluidStepConstants と一致させること");

/// @note seed → ノイズ格子の切り出し位置 (FluidGasSolver::Reset と同じ式)。
[[nodiscard]] FBZZ_FLUID_API math::Vector3 FluidNoiseOffset(std::uint32_t seed);

/// @note 立方体の格子 (1 辺 resolution) で、time から dt 秒進める刻みの定数を作る。
/// @note densityScale / temperatureScale は結果をボリュームへ書き出すときの倍率 (FluidVolumeScale と同じ意味)。
/// @note 無効 (enabled = false) の部品は詰めない。上限を超えた部品は捨てる (レシピ側で上限を守らせる)。
/// @note テクスチャ発生源のタイル番号は FluidGpuMaskPaths が返す順と同じ。
[[nodiscard]] FBZZ_FLUID_API FluidGpuStepConstants PackFluidGpuStep(const FluidRecipe& recipe, int resolution, float time,
                                                     float dt, float densityScale, float temperatureScale);

/// @note GPU に詰める発生源のうち Texture のものの画像パスを、アトラスのタイル番号の順 (0, 1, ...) に返す。
/// @note FluidGpuSolver はこの順にマスクを読み、タイル (t % 4, t / 4) × kFluidSourceMaskSize に置く。
[[nodiscard]] FBZZ_FLUID_API std::vector<std::string> FluidGpuMaskPaths(const FluidRecipe& recipe);

} /// @note namespace fbzz::fluid
