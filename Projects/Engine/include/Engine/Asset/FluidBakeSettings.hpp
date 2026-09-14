/// @file    FluidBakeSettings.hpp
/// @brief   .fluid の [bake] — 2D で焼くか 3D (Volume Flipbook Baker) で焼くかと、3D 固有の設定
/// @author  Hasegawa Jin
/// @date    2026-09-12
///
/// WHY レシピに焼き方まで持たせるか:
///   3D の焼き設定がパネルのメモリにしか無いと、同じ .fluid を渡しても同じ絵が出ない。
///   AI はファイルを書いて焼かせるしかないので、«このファイルを焼けばこの絵» を .fluid だけで決める。
///
/// ここに置くのは流体の 3D 焼きにしか無いノブだけ。コマ数・タイル・supersampling は [output]、
/// 細部ノイズは [render] / [gas] が正本 (2D と 3D で同じ値を別々に持たない)。
/// 写し方は MakeVolumeBakeSettings (FluidVolumeBake.hpp)。
#pragma once

#include <Math/Vector3.hpp>
#include <cstdint>

namespace fbzz::asset {

enum class FluidBakeMode : std::uint8_t { Flat2D = 0, Volume3D };
/// 3D をどちらで解くか。気体は FluidGpuSolver / FluidGasSolver、液体は FluidGpuLiquidSolver / FluidLiquidSolver。
///   Auto … GPU を試し、初期化できなければ CPU で解く (理由は結果に残る)
///   Gpu  … GPU で解けなければ焼かない。同じ .fluid から必ず同じ絵が欲しいときはこちら
///   Cpu  … 常に CPU (96³ まで)
enum class FluidBakeSolver : std::uint8_t { Auto = 0, Gpu, Cpu };

struct FluidBakeSettings {
    FluidBakeMode mode = FluidBakeMode::Flat2D;

    // ── 3D ──
    int volumeResolution = 64;
    /// 視線 1 本あたりのレイマーチの標本数。volume_resolution を上げても、ここが据え置きだと
    /// 1 ボクセルに標本が 1 個しか入らず絵が良くならない (目安: volume_resolution の 2 倍)。
    int raySteps = 128;
    /// 影の行進の標本数。上げると影の縞が減るが、標本数 × 視線標本数だけ焼き時間が増える。
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
    float extinction = 10.0f;
    float anisotropy = 0.3f;
    float emissionIntensity = 6.0f;
    float exposure = 0.8f;
    float cameraYawDegrees = 0.0f;
    float halfExtent = 1.0f;
};

} // namespace fbzz::asset
