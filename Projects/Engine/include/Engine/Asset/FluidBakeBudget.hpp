/// @file    FluidBakeBudget.hpp
/// @brief   Fluid Bake の作業メモリと出力容量を事前に見積もる。
/// @author  Hasegawa Jin
/// @date    2026-09-25
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace fbzz::fluid { struct FluidRecipe; }
namespace fbzz::asset { struct VolumeFlipbookBakeSettings; }

namespace fbzz::asset {

struct FluidBakeBudget {
    std::uint64_t atlasPixels = 0;
    std::uint64_t cpuBytes = 0;
    std::uint64_t gpuBytes = 0;
    std::uint64_t diskBytes = 0;
};

/// @brief 全コマ画像・Atlas・書き出しの同時保持量を安全側に見積もる。
[[nodiscard]] FluidBakeBudget EstimateFluidBakeBudget(const fluid::FluidRecipe& recipe);
[[nodiscard]] FluidBakeBudget EstimateVolumeBakeBudget(const VolumeFlipbookBakeSettings& settings);

/// @brief RAM と出力先ディスクの空き容量を確認する。
/// @return 不足・Atlas 過大なら false。利用可能量を取得できなければ容量判定だけ省く。
[[nodiscard]] bool ValidateFluidBakeBudget(const FluidBakeBudget& budget,
                                           const std::filesystem::path& outputDirectory,
                                           std::string& outError);

} /// @note namespace fbzz::asset
