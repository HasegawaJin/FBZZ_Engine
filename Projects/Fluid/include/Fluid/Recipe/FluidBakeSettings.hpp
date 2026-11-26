/// @file    FluidBakeSettings.hpp
/// @brief   .fluid の [bake] — 2D で焼くか 3D (Volume Flipbook Baker) で焼くかと、3D 固有の設定
/// @author  Hasegawa Jin
/// @date    2026-09-12
///
/// @note レシピに焼き方まで持たせる理由: 3D の焼き設定が UI 側のメモリだけにあると、同じ .fluid でも同じ絵にならない。
/// @note AI がファイルを書いて焼く運用なので、結果は .fluid だけで決まるようにする。
/// @note ここに置くのは 3D 焼き専用のノブだけ。コマ数・タイル・supersampling は [output]、細部ノイズは
/// @note [render]/[gas] が正本 (2D/3D で値を別々に持たない)。写し方は MakeVolumeBakeSettings (FluidVolumeBake.hpp)。
#pragma once

#include <Math/LinearAlgebra/Vector3.hpp>
#include <cstdint>

namespace fbzz::fluid {

enum class FluidBakeMode : std::uint8_t { Flat2D = 0, Volume3D };
/// @brief 3D をどちらで解くか。気体は FluidGpuSolver / FluidGasSolver、液体は FluidGpuLiquidSolver / FluidLiquidSolver。
/// @note Auto は GPU を試し、初期化できなければ CPU で解く (理由は結果に残る)。
/// @note Gpu は GPU で解けなければ焼かない。同じ .fluid から必ず同じ絵が欲しいときに使う。
/// @note Cpu は常に CPU で解く (96³ まで)。
enum class FluidBakeSolver : std::uint8_t { Auto = 0, Gpu, Cpu };

struct FluidBakeSettings {
    FluidBakeMode mode = FluidBakeMode::Flat2D; ///< フラット画像またはボリュームから作るベイク出力の形式。

    /// @name 3D
    /// @{
    int volumeResolution = 64; ///< ベイクで使うボリューム格子の一辺のセル数。
    /// @brief 視線 1 本あたりのレイマーチの標本数。volume_resolution を上げても、ここが据え置きだと絵が良くならない。
    /// @note 1 ボクセルに標本が 1 個しか入らない場合の目安は volume_resolution の 2 倍。
    int raySteps = 128; ///< ベイク視線に沿って密度を積分する刻み数。
    int shadowSteps = 16; ///< 影の行進の標本数。上げると影の縞が減るが、標本数 × 視線標本数だけ焼き時間が増える。
    FluidBakeSolver solver = FluidBakeSolver::Auto; ///< ベイク時に使う流体ソルバーの選択。
    float densityScale = 1.0f; ///< ベイクで参照する密度に掛ける倍率。
    int scatteringOctaves = 3; ///< 散乱光の近似で重ねる次数。
    float skyOcclusion = 0.6f; ///< ベイク時の環境光遮蔽の強さ。
    bool blackbodyEmission = false; ///< 温度から黒体放射を評価するかどうか。
    float blackbodyMinKelvin = 1000.0f; ///< 黒体放射へ写すときの下限温度 [K]。
    float blackbodyMaxKelvin = 2800.0f; ///< 黒体放射へ写すときの上限温度 [K]。
    bool sixWayLightmaps = false; ///< 6 方向からのライティング応答を出力するかどうか。

    float lightYawDegrees = 35.0f; ///< 入射光方向を決める方位角 [度]。
    float lightPitchDegrees = 50.0f; ///< 入射光方向を決める仰角 [度]。
    math::Vector3 lightColor{ 3.0f, 2.85f, 2.7f }; ///< ベイク時に当てる主光源のリニア RGB。
    math::Vector3 ambient{ 0.25f, 0.28f, 0.33f }; ///< ベイク時に加える環境光のリニア RGB。
    float extinction = 10.0f; ///< 3D の密度から光学厚さを作る基準倍率。render.opacity の相対倍率を掛けてベイクへ渡す。
    float anisotropy = 0.3f; ///< 散乱位相の方向性を指定する係数。
    float emissionIntensity = 6.0f; ///< ベイクで加える放射光の強度倍率。
    float exposure = 0.8f; ///< ベイク結果の露出倍率。
    float cameraYawDegrees = 0.0f; ///< 流体を撮る視線の方位角 [度]。
    float halfExtent = 1.0f; ///< ベイク対象の局所座標での領域の半幅。
    /// @}
};

} // namespace fbzz::fluid
