/// @file    FluidBakeBudget.cpp
/// @brief   Fluid Bake の RAM・ディスク容量の事前検査。
/// @author  Hasegawa Jin
/// @date    2026-09-25
#include <Engine/Asset/FluidBakeBudget.hpp>

#include <Engine/Asset/VolumeFlipbookBaker.hpp>
#include <Fluid/FluidRecipe.hpp>
#include <Fluid/FluidStepping.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <algorithm>
#include <cstdint>
#include <system_error>

namespace fbzz::asset {
namespace {

constexpr std::uint64_t kMaxAtlasPixels = 16ull * 1024ull * 1024ull;
constexpr std::uint64_t kMiB = 1024ull * 1024ull;

std::string MiB(std::uint64_t bytes)
{
    return std::to_string((bytes + kMiB - 1) / kMiB) + " MiB";
}

} /// @note namespace

FluidBakeBudget EstimateFluidBakeBudget(const fluid::FluidRecipe& source)
{
    fluid::FluidRecipe recipe = source;
    fluid::NormalizeFluidOutput(recipe.output);
    const std::uint64_t tile = static_cast<std::uint64_t>(recipe.output.frameSize);
    const std::uint64_t pixels = tile * tile * recipe.output.columns * recipe.output.rows;
    const std::uint64_t supersample = static_cast<std::uint64_t>(recipe.output.supersampling);
    FluidBakeBudget budget;
    budget.atlasPixels = pixels;
    /// @note コマの float RGBA/MV、Atlas の色/MV/被覆と超解像の作業画像を同時に保持する。
    budget.cpuBytes = pixels * (recipe.output.motionVectors ? 48ull : 32ull)
        + tile * tile * supersample * supersample * 32ull;
    budget.diskBytes = pixels * (recipe.output.motionVectors ? 20ull : 10ull);
    if (recipe.output.vectorField) budget.cpuBytes += 64ull * 64ull * 64ull * 32ull;
    return budget;
}

FluidBakeBudget EstimateVolumeBakeBudget(const VolumeFlipbookBakeSettings& settings)
{
    const std::uint64_t tile = static_cast<std::uint64_t>((std::max)(settings.tileSize, 1));
    const FlipbookGrid grid = ComputeFlipbookGrid(settings.source.frameCount, settings.columns);
    const std::uint64_t frames = static_cast<std::uint64_t>((std::max)(grid.frameCount, 1));
    const std::uint64_t pixels = tile * tile * grid.columns * grid.rows;
    const std::uint64_t volume = static_cast<std::uint64_t>((std::max)(settings.volumeResolution, 1));
    const std::uint64_t maps = settings.sixWayLightmaps ? 4ull : 0ull;
    FluidBakeBudget budget;
    budget.atlasPixels = pixels;
    /// @note Atlas の 8bit 出力、float の動き/被覆、ループ先頭の HDR タイルを含む概算。
    budget.cpuBytes = pixels * (28ull + maps * 8ull)
        + tile * tile * (std::min)(frames / 2, 32ull) * (32ull + maps * 16ull);
    const std::uint64_t supersampling = static_cast<std::uint64_t>((std::max)(settings.supersampling, 1));
    budget.gpuBytes = volume * volume * volume * 32ull
        + tile * tile * supersampling * supersampling * 96ull;
    budget.diskBytes = pixels * (settings.distortion ? 10ull : 20ull + maps * 10ull);
    return budget;
}

bool ValidateFluidBakeBudget(const FluidBakeBudget& budget,
                             const std::filesystem::path& outputDirectory,
                             std::string& outError)
{
    outError.clear();
    if (budget.atlasPixels > kMaxAtlasPixels) {
        outError = "Atlas の総画素数が 16M を超えます。Frame Size またはコマ数を減らしてください";
        return false;
    }
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory)
        && budget.cpuBytes > static_cast<std::uint64_t>(memory.ullAvailPhys) * 3ull / 4ull) {
        outError = "Bake の推定 RAM " + MiB(budget.cpuBytes)
            + " が空き容量 " + MiB(memory.ullAvailPhys) + " に対して大きすぎます";
        return false;
    }
    std::error_code error;
    const auto space = std::filesystem::space(outputDirectory, error);
    /// @note 旧出力と仮出力を同時に保持するため、新しい出力の 2 倍を予約する。
    if (!error && budget.diskBytes * 2ull > space.available) {
        outError = "Bake の推定ディスク使用量 " + MiB(budget.diskBytes * 2ull)
            + " に対して空き容量 " + MiB(space.available) + " が不足しています";
        return false;
    }
    return true;
}

} /// @note namespace fbzz::asset
