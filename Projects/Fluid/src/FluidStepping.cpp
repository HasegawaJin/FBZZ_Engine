/// @file    FluidStepping.cpp
/// @brief   .fluid を解く刻みの正本の実装
/// @author  Hasegawa Jin
/// @date    2026-09-14
#include <Fluid/FluidStepping.hpp>

#include <algorithm>
#include <cmath>

namespace fbzz::fluid {

int FluidStepPlan::FrameOfTime(float seconds) const
{
    if (frameDt <= 0.0f) return 0;
    const int last = (std::max)(frameCount + loopOverlap - 1, 0);
    const float frame = std::round(seconds / frameDt);
    if (!(frame > 0.0f)) return 0;
    return frame >= static_cast<float>(last) ? last : static_cast<int>(frame);
}

void NormalizeFluidOutput(FluidOutputSettings& output)
{
    output.frameSize = std::clamp(output.frameSize, 16, 1024);
    output.columns   = std::clamp(output.columns, 1, 32);
    output.rows      = std::clamp(output.rows, 1, 32);
    output.supersampling = std::clamp(output.supersampling, 1, 4);
    output.duration  = (std::max)(output.duration, 0.05f);
    output.warmup    = std::clamp(output.warmup, 0.0f, 30.0f);
    output.substeps  = std::clamp(output.substeps, 1, 16);
}

int FluidWarmupFrames(float warmupSeconds, float frameDt)
{
    if (frameDt <= 0.0f) return 0;
    /// @note 1e-4 を引くのは «ちょうど割り切れる warmup» で 1 コマ余分に数えないため (2.0 / 0.03125 = 64.0)。
    const float frames = std::ceil((std::max)(warmupSeconds, 0.0f) / frameDt - 1.0e-4f);
    return frames > 0.0f ? static_cast<int>(frames) : 0;
}

FluidStepPlan MakeFluidStepPlan(const FluidRecipe& recipe)
{
    FluidOutputSettings output = recipe.output;
    NormalizeFluidOutput(output);

    FluidStepPlan plan;
    plan.frameCount = output.columns * output.rows;
    plan.frameDt    = output.duration / static_cast<float>(plan.frameCount);
    plan.substeps   = output.substeps;
    plan.warmupFrames = FluidWarmupFrames(output.warmup, plan.frameDt);
    plan.loopOverlap  = output.loop ? (std::max)(plan.frameCount / 4, 1) : 0;
    return plan;
}

} // namespace fbzz::fluid
