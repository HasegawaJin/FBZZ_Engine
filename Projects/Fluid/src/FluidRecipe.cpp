/// @file    FluidRecipe.cpp
/// @brief   レシピの設定型のうち、解くために要る派生値。
/// @author  Hasegawa Jin
/// @date    2026-09-16
#include <Fluid/FluidRecipe.hpp>

#include <algorithm>

namespace fbzz::fluid {

int ResolveGasResolution(const FluidRecipe& recipe)
{
    if (recipe.gas.resolution > 0) return std::clamp(recipe.gas.resolution, 16, 512);
    /// @note Auto: コマの解像度に合わせる。256 を超えると焼き時間が分単位になるので、
    ///       そこから先は細部ノイズに任せる。
    return std::clamp(recipe.output.frameSize, 64, 256);
}

} // namespace fbzz::fluid
