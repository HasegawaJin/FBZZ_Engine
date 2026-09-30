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

#include <Math/Vector3.hpp>
#include <cstdint>

namespace fbzz::fluid {

enum class FluidBakeMode : std::uint8_t { Flat2D = 0, Volume3D };
/// @brief 3D をどちらで解くか。気体は FluidGpuSolver / FluidGasSolver、液体は FluidGpuLiquidSolver / FluidLiquidSolver。
/// @note Auto は GPU を試し、初期化できなければ CPU で解く (理由は結果に残る)。
/// @note Gpu は GPU で解けなければ焼かない。同じ .fluid から必ず同じ絵が欲しいときに使う。
/// @note Cpu は常に CPU で解く (96³ まで)。
enum class FluidBakeSolver : std::uint8_t { Auto = 0, Gpu, Cpu };

struct FluidBakeSettings {
    FluidBakeMode mode = FluidBakeMode::Flat2D;

    /// @name 3D
    /// @{
    int volumeResolution = 64;
    /// @brief 視線 1 本あたりのレイマーチの標本数。volume_resolution を上げても、ここが据え置きだと絵が良くならない。
    /// @note 1 ボクセルに標本が 1 個しか入らない場合の目安は volume_resolution の 2 倍。
    int raySteps = 128;
    /// @brief 影の行進の標本数。上げると影の縞が減るが、標本数 × 視線標本数だけ焼き時間が増える。
    int shadowSteps = 16;
    FluidBakeSolver solver = FluidBakeSolver::Auto;
    float densityScale = 1.0f;
    int scatteringOctaves = 3;
    float skyOcclusion = 0.6f;
    bool blackbodyEmission = false;
    float blackbodyMinKelvin = 1000.0f;
    float blackbodyMaxKelvin = 2800.0f;
    bool sixWayLightmaps = false;

    float lightYawDegrees = 35.0f;
    float lightPitchDegrees = 50.0f;
    math::Vector3 lightColor{ 3.0f, 2.85f, 2.7f };
    math::Vector3 ambient{ 0.25f, 0.28f, 0.33f };
    /// @brief 3D の密度から光学厚さを作る基準倍率。render.opacity の相対倍率を掛けてベイクへ渡す。
    float extinction = 10.0f;
    float anisotropy = 0.3f;
    float emissionIntensity = 6.0f;
    float exposure = 0.8f;
    float cameraYawDegrees = 0.0f;
    float halfExtent = 1.0f;
    /// @}
};

} // namespace fbzz::fluid
