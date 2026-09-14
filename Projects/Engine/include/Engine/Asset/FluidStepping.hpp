/// @file    FluidStepping.hpp
/// @brief   .fluid を解く刻みの正本 — コマ間隔・substeps・warmup のコマ数
/// @author  Hasegawa Jin
/// @date    2026-09-14
///
/// WHY 刻み方を 1 か所に集めるか:
///   同じ .fluid でも «2D の焼き» と «プレビュー» と «GPU» で warmup の進め方が違っていた
///   (前者は substeps を掛けない 1 刻み、後者は通常コマと同じ分割)。substeps は既定 2・
///   プリセットは 3〜4 なので、AI がプレビューで見た絵と焼いた絵は別のシミュレーションだった。
///   刻みはここだけが決め、4 つのソルバー経路 (2D CPU・3D CPU・GPU 気体・GPU 液体) が必ず通る。
///
/// 決め事: **warmup も «普通のコマ» として解く**。端数は切り上げてコマ数に丸める。
///   こうすると「焼きの第 n コマ」= 「warmupFrames + n コマ進めた状態」で、どの経路でも一致する。
#pragma once

#include <Engine/Asset/FluidRecipe.hpp>

namespace fbzz::asset {

/// 1 本のレシピを解く刻み。
struct FluidStepPlan {
    /// コマ間隔 [秒] (= duration ÷ コマ数)。
    float frameDt = 1.0f / 24.0f;
    /// 1 コマを何回に分けて解くか。
    int substeps = 2;
    /// 焼き始めの前に進めるコマ数 (= ceil(warmup ÷ frameDt))。
    int warmupFrames = 0;
    /// 焼くコマ数 (columns × rows)。
    int frameCount = 1;
    /// ループのために末尾へ余分に解くコマ数 (ループしなければ 0)。
    int loopOverlap = 0;

    [[nodiscard]] float StepDt() const { return frameDt / static_cast<float>(substeps); }
    /// 焼き始めから frame コマ目の時刻 [秒]。
    [[nodiscard]] float TimeOfFrame(int frame) const { return static_cast<float>(frame) * frameDt; }
    /// 秒をコマ番号へ吸着させる ([0, frameCount + loopOverlap - 1] に収める)。
    /// プレビューの time 指定はここを通し、コマ境界に乗らない «どのコマでもない状態» を作らない。
    [[nodiscard]] int FrameOfTime(float seconds) const;
};

/// output の値域を揃える。焼き・プレビュー・3D が別々にクランプすると、境目の値で刻みがずれる。
void NormalizeFluidOutput(FluidOutputSettings& output);

/// warmup [秒] を «コマ数» へ切り上げる。コマ間隔をレシピから導かない経路 (3D の焼き設定) も
/// ここを通し、warmup の長さが経路ごとに 1 コマずれるのを防ぐ。
[[nodiscard]] int FluidWarmupFrames(float warmupSeconds, float frameDt);

/// レシピから刻みを組む (recipe は書き換えない)。
[[nodiscard]] FluidStepPlan MakeFluidStepPlan(const FluidRecipe& recipe);

/// 1 コマ進める。
template <class Solver>
void AdvanceFluidFrame(Solver& solver, const FluidStepPlan& plan)
{
    const float stepDt = plan.StepDt();
    for (int step = 0; step < plan.substeps; ++step) solver.Advance(stepDt);
}

/// frames コマ進める。
template <class Solver>
void AdvanceFluidFrames(Solver& solver, const FluidStepPlan& plan, int frames)
{
    for (int frame = 0; frame < frames; ++frame) AdvanceFluidFrame(solver, plan);
}

/// Reset 直後の場を «焼き始め» の状態まで進める。
template <class Solver>
void AdvanceFluidWarmup(Solver& solver, const FluidStepPlan& plan)
{
    AdvanceFluidFrames(solver, plan, plan.warmupFrames);
}

} // namespace fbzz::asset
