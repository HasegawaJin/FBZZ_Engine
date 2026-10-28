/// @file    FluidSourceMask.cpp
/// @brief   テクスチャ発生源の濃さマスクの引き方。
/// @author  Hasegawa Jin
/// @date    2026-09-16
#include <Fluid/FluidSourceMask.hpp>
#include <Fluid/FluidRecipe.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace fbzz::fluid {
namespace {

/// @note 読み手が無い経路ではテクスチャ発生源を板の形として解く。
FluidSourceMaskResolver g_resolver = nullptr;

} /// @note namespace

void SetFluidSourceMaskResolver(FluidSourceMaskResolver resolver)
{
    g_resolver = resolver;
}

bool ResolveFluidSourceMask(const std::string& texture, FluidSourceMask& out)
{
    out.values.clear();
    if (g_resolver == nullptr || texture.empty()) return false;
    if (g_resolver(texture, out)) return true;
    out.values.clear();
    return false;
}

void ResolveFluidSourceMasks(const std::vector<FluidSource>& sources, std::vector<FluidSourceMask>& outMasks)
{
    outMasks.assign(sources.size(), FluidSourceMask{});
    for (std::size_t i = 0; i < sources.size(); ++i) {
        if (sources[i].shape != FluidSourceShape::Texture || sources[i].texture.empty()) continue;
        if (!ResolveFluidSourceMask(sources[i].texture, outMasks[i])) outMasks[i].values.clear();
    }
}

float SampleFluidSourceMask(const FluidSourceMask& mask, float u, float v)
{
    if (!mask.IsValid()) return 1.0f;
    /// @note NaN を int へ落とすと未定義動作になる。
    if (!std::isfinite(u)) u = 0.0f;
    if (!std::isfinite(v)) v = 0.0f;
    const int size = kFluidSourceMaskSize;
    /// @note テクセル中心の規則 (GPU の双線形フィルター + clamp と同じ)。
    const float x = std::clamp(u, 0.0f, 1.0f) * static_cast<float>(size) - 0.5f;
    const float y = std::clamp(v, 0.0f, 1.0f) * static_cast<float>(size) - 0.5f;
    const float baseX = std::floor(x);
    const float baseY = std::floor(y);
    const float tx = x - baseX;
    const float ty = y - baseY;
    const int x0 = std::clamp(static_cast<int>(baseX), 0, size - 1);
    const int x1 = std::clamp(static_cast<int>(baseX) + 1, 0, size - 1);
    const int y0 = std::clamp(static_cast<int>(baseY), 0, size - 1);
    const int y1 = std::clamp(static_cast<int>(baseY) + 1, 0, size - 1);
    const auto at = [&mask, size](int px, int py) {
        return mask.values[static_cast<std::size_t>(py) * static_cast<std::size_t>(size) + static_cast<std::size_t>(px)];
    };
    const float top = at(x0, y0) + (at(x1, y0) - at(x0, y0)) * tx;
    const float bottom = at(x0, y1) + (at(x1, y1) - at(x0, y1)) * tx;
    return top + (bottom - top) * ty;
}

} /// @note namespace fbzz::fluid
