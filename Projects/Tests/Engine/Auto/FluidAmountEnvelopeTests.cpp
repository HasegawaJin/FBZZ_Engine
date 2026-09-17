/// @file    FluidAmountEnvelopeTests.cpp
/// @brief   .fluid の «量のエンベロープ» (FluidAmount) — 倍率の式・既定で何も変わらないこと・TOML 往復・CPU と GPU の一致を固定する。
/// @author  Hasegawa Jin
/// @date    2026-09-13
///
/// このエンベロープは既存の .fluid すべてに «キーの無い» 形で入る。倍率が 1 にならない実装ミスは
/// 焼いた絵が少し濃い / 薄いという形でしか出ず、どのテストも落ちないまま全プリセットの絵が変わる。
/// 掛ける先 (density / temperature / fuel / strength) と掛けない先 (位置・velocity) もここで縛る。

#include <TestKit/TestKit.hpp>
#include <TestKit/TempDir.hpp>

#include <Fluid/FluidGpuStep.hpp>
#include <Fluid/FluidOperatorEval.hpp>
#include <Engine/Asset/FluidRecipeCodec.hpp>
#include <Fluid/FluidSolver.hpp>
#include <Engine/Util/FileSystem.hpp>

#include <cmath>
#include <cstddef>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace fbzz::tests {
namespace {

using fluid::FluidAmountKey;
using fluid::FluidForce;
using fluid::FluidForceType;
using fluid::FluidSource;

float Length(const math::Vector3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }

fluid::FluidAmount Envelope(std::vector<FluidAmountKey> keys)
{
    fluid::FluidAmount amount;
    amount.keys = std::move(keys);
    return amount;
}

/// 密度が «注いだ量» だけで決まる気体のレシピ (浮力・重み・乱流・渦度を切り、流速も与えない)。
/// 速度場が動かないので、全体の密度は注いだ量に正比例する = 倍率をそのまま量として測れる。
fluid::FluidRecipe LinearGasRecipe(float density)
{
    fluid::FluidRecipe recipe;
    recipe.gas.buoyancy = 0.0f;
    recipe.gas.weight = 0.0f;
    recipe.gas.turbulence = 0.0f;
    recipe.gas.vorticity = 0.0f;
    recipe.gas.densityDissipation = 0.0f;
    FluidSource source;
    source.shape = fluid::FluidSourceShape::Sphere;
    source.center = { 0.0f, 0.0f, 0.0f };
    source.size = { 0.2f, 0.2f, 0.2f };
    source.density = density;
    source.temperature = 0.0f;
    source.fuel = 0.0f;
    source.noise = 0.0f;
    source.velocity = { 0.0f, 0.0f, 0.0f };
    recipe.sources.push_back(source);
    return recipe;
}

float SolveTotalDensity(const fluid::FluidRecipe& recipe)
{
    fluid::FluidGasSolver solver;
    solver.Reset(recipe, 32, 32, 1);
    for (int i = 0; i < 6; ++i) solver.Step(1.0f / 30.0f);
    return solver.TotalDensity();
}

fluid::FluidRecipe LiquidRecipe()
{
    fluid::FluidRecipe recipe;
    recipe.kind = fluid::FluidKind::Liquid;
    recipe.liquid.floor = false;
    recipe.liquid.gravity = 0.0f;
    FluidSource source;
    source.shape = fluid::FluidSourceShape::Sphere;
    source.center = { 0.0f, 0.0f, 0.0f };
    source.size = { 0.15f, 0.15f, 0.15f };
    source.velocity = { 0.0f, 0.0f, 0.0f };
    source.spread = 0.0f;
    source.count = 120;
    recipe.sources.push_back(source);
    return recipe;
}

float MeanX(const fluid::FluidLiquidSolver& solver)
{
    double total = 0.0;
    for (const auto& particle : solver.Particles()) total += particle.x;
    return solver.Particles().empty() ? 0.0f : static_cast<float>(total / static_cast<double>(solver.Particles().size()));
}

} // namespace

/// @name 倍率の式

TEST(FluidAmountEnvelopeTest, EmptyKeysAreAlwaysOne)
{
    const fluid::FluidAmount empty;
    for (const float time : { -5.0f, 0.0f, 0.5f, 100.0f })
        EXPECT_EQ(fluid::SampleFluidAmount(empty, time), 1.0f) << time;

    /// @note 既定の部品 (既存の .fluid すべて) は倍率を持たない。
    EXPECT_EQ(fluid::FluidSourceAmount(FluidSource{}, 0.7f), 1.0f);
    EXPECT_EQ(fluid::FluidForceAmount(FluidForce{}, 0.7f), 1.0f);
}

TEST(FluidAmountEnvelopeTest, HoldsItsEndKeysOutsideTheRange)
{
    const fluid::FluidAmount amount = Envelope({ { 0.5f, 2.0f }, { 1.5f, 0.25f } });
    /// @note 端の外は端の値で止まる (外挿しない)。
    EXPECT_FLOAT_EQ(fluid::SampleFluidAmount(amount, -10.0f), 2.0f);
    EXPECT_FLOAT_EQ(fluid::SampleFluidAmount(amount, 0.5f), 2.0f);
    EXPECT_FLOAT_EQ(fluid::SampleFluidAmount(amount, 1.5f), 0.25f);
    EXPECT_FLOAT_EQ(fluid::SampleFluidAmount(amount, 90.0f), 0.25f);

    const fluid::FluidAmount single = Envelope({ { 1.0f, 0.5f } });
    EXPECT_FLOAT_EQ(fluid::SampleFluidAmount(single, 0.0f), 0.5f);
    EXPECT_FLOAT_EQ(fluid::SampleFluidAmount(single, 5.0f), 0.5f);
}

TEST(FluidAmountEnvelopeTest, InterpolatesLinearlyBetweenKeys)
{
    const fluid::FluidAmount amount = Envelope({ { 0.0f, 1.0f }, { 2.0f, 0.0f } });
    EXPECT_NEAR(fluid::SampleFluidAmount(amount, 0.5f), 0.75f, 1.0e-6f);
    EXPECT_NEAR(fluid::SampleFluidAmount(amount, 1.0f), 0.5f, 1.0e-6f);
    EXPECT_NEAR(fluid::SampleFluidAmount(amount, 1.5f), 0.25f, 1.0e-6f);

    /// @note 同じ時刻に 2 つ置くと «その時刻で切り替わる» 階段になる (0 除算にはならない)。
    ///       1.0 ちょうどは切り替わった後の値。SampleFluidMotion も同じ取り方をする。
    const fluid::FluidAmount step = Envelope({ { 0.0f, 1.0f }, { 1.0f, 1.0f }, { 1.0f, 0.0f }, { 2.0f, 0.0f } });
    EXPECT_FLOAT_EQ(fluid::SampleFluidAmount(step, 0.5f), 1.0f);
    EXPECT_NEAR(fluid::SampleFluidAmount(step, 0.999f), 1.0f, 1.0e-3f);
    EXPECT_FLOAT_EQ(fluid::SampleFluidAmount(step, 1.0f), 0.0f);
    EXPECT_NEAR(fluid::SampleFluidAmount(step, 1.5f), 0.0f, 1.0e-6f);

    /// @note 倍率は 1 を超えてもよい (溜めて吹き上げる)。
    EXPECT_NEAR(fluid::SampleFluidAmount(Envelope({ { 0.0f, 1.0f }, { 1.0f, 4.0f } }), 0.5f), 2.5f, 1.0e-6f);
}

/// @name 掛ける先

TEST(FluidAmountEnvelopeTest, ScalesTheInjectedAmountButNotTheFlowVelocity)
{
    /// @note 倍率 0.5 の発生源 = 基準の量を半分にした発生源。掛ける順まで同じなら 1 ビットも違わない。
    fluid::FluidRecipe scaled = LinearGasRecipe(2.0f);
    scaled.sources[0].amount = Envelope({ { 0.0f, 0.5f }, { 9.0f, 0.5f } });
    EXPECT_FLOAT_EQ(SolveTotalDensity(scaled), SolveTotalDensity(LinearGasRecipe(1.0f)));

    /// @note 倍率 1 のキーを並べても «キーが無い» のと変わらない (既存の絵が動かないことの本体)。
    fluid::FluidRecipe unity = LinearGasRecipe(2.0f);
    unity.sources[0].amount = Envelope({ { 0.0f, 1.0f }, { 0.4f, 1.0f }, { 1.0f, 1.0f } });
    EXPECT_FLOAT_EQ(SolveTotalDensity(unity), SolveTotalDensity(LinearGasRecipe(2.0f)));

    /// @note 位置と流速には掛からない (動きは motion の担当)。
    fluid::FluidRecipe recipe = LinearGasRecipe(2.0f);
    recipe.sources[0].center = { 0.1f, -0.2f, 0.0f };
    recipe.sources[0].velocity = { 0.0f, 1.0f, 0.0f };
    recipe.sources[0].amount = Envelope({ { 0.0f, 0.25f } });
    const fluid::FluidGpuStepConstants step = fluid::PackFluidGpuStep(recipe, 32, 0.5f, 0.01f, 1.0f, 1.0f);
    const fluid::FluidGpuSource& packed = step.sources[0];
    EXPECT_EQ(packed.centerShape[0], 0.1f);
    EXPECT_EQ(packed.centerShape[1], -0.2f);
    EXPECT_EQ(packed.velocity[1], 1.0f);
    EXPECT_EQ(packed.velocity[3], 1.0f);
}

TEST(FluidAmountEnvelopeTest, ForceDeltaScalesTheStrengthBeforeTheFalloff)
{
    const math::Vector3 noOffset = { 0.0f, 0.0f, 0.0f };
    const math::Vector3 p = { 0.3f, 0.1f, 0.0f };
    const math::Vector3 v = { 0.4f, -0.2f, 0.0f };
    FluidForce wind;
    wind.type = FluidForceType::Wind;
    wind.direction = { 1.0f, 0.0f, 0.0f };
    wind.strength = 4.0f;
    wind.radius = 1.0f;
    wind.falloffPower = 2.0f;

    FluidForce weaker = wind;
    weaker.strength = 1.0f;
    const math::Vector3 scaled =
        fluid::FluidForceDelta(wind, wind.center, p, v, 0.0f, 0.1f, noOffset, 0, true, 0.25f);
    const math::Vector3 rebased =
        fluid::FluidForceDelta(weaker, weaker.center, p, v, 0.0f, 0.1f, noOffset, 0, true, 1.0f);
    EXPECT_FLOAT_EQ(scaled.x, rebased.x);
    EXPECT_FLOAT_EQ(scaled.y, rebased.y);

    /// @note 倍率 1 は «掛けない» と完全に同じ。
    FluidForce drag;
    drag.type = FluidForceType::Drag;
    drag.strength = 5.0f;
    const math::Vector3 plain =
        fluid::FluidForceDelta(drag, drag.center, p, v, 0.0f, 0.1f, noOffset, 0, true, 1.0f);
    EXPECT_FLOAT_EQ(plain.x, -v.x * (1.0f - std::exp(-5.0f * 0.1f)));

    /// @note Drag は強さに対して線形でない。«Δv を後から倍率で縮める» 実装とは別物であることを縛る
    ///       (GPU は強さを詰めるしかないので、後から縮める実装にすると CPU とずれる)。
    const math::Vector3 half =
        fluid::FluidForceDelta(drag, drag.center, p, v, 0.0f, 0.1f, noOffset, 0, true, 0.5f);
    EXPECT_FLOAT_EQ(half.x, -v.x * (1.0f - std::exp(-2.5f * 0.1f)));
    EXPECT_GT(std::fabs(half.x), std::fabs(plain.x) * 0.5f);

    /// @note 倍率 0 は «効かない» (窓の外と同じ)。
    EXPECT_EQ(Length(fluid::FluidForceDelta(wind, wind.center, p, v, 0.0f, 0.1f, noOffset, 0, true, 0.0f)), 0.0f);
}

TEST(FluidAmountEnvelopeTest, GasSolverSilencesAForceWithAZeroEnvelope)
{
    fluid::FluidRecipe recipe = LinearGasRecipe(2.0f);
    FluidForce wind;
    wind.type = FluidForceType::Wind;
    wind.direction = { 1.0f, 0.0f, 0.0f };
    wind.strength = 4.0f;
    recipe.forces.push_back(wind);

    const auto solve = [](const fluid::FluidRecipe& r) {
        fluid::FluidGasSolver solver;
        solver.Reset(r, 32, 32, 1);
        for (int i = 0; i < 4; ++i) solver.Step(1.0f / 30.0f);
        float total = 0.0f;
        for (const float value : solver.VelocityX()) total += value;
        return total;
    };

    const float blowing = solve(recipe);
    EXPECT_GT(blowing, 1.0f);
    recipe.forces[0].amount = Envelope({ { 0.0f, 0.0f }, { 9.0f, 0.0f } });
    EXPECT_EQ(solve(recipe), 0.0f);
    /// @note 倍率 1 のキーは «キーが無い» のと同じ。
    recipe.forces[0].amount = Envelope({ { 0.0f, 1.0f }, { 9.0f, 1.0f } });
    EXPECT_FLOAT_EQ(solve(recipe), blowing);
}

TEST(FluidAmountEnvelopeTest, LiquidForceFollowsTheEnvelopeButEmissionDoesNot)
{
    fluid::FluidRecipe recipe = LiquidRecipe();
    FluidForce wind;
    wind.type = FluidForceType::Wind;
    wind.direction = { 1.0f, 0.0f, 0.0f };
    wind.strength = 4.0f;
    recipe.forces.push_back(wind);

    const auto solve = [](const fluid::FluidRecipe& r) {
        fluid::FluidLiquidSolver solver;
        solver.Reset(r);
        for (int i = 0; i < 9; ++i) solver.Advance(1.0f / 30.0f);
        return solver;
    };

    const fluid::FluidLiquidSolver blown = solve(recipe);
    recipe.forces[0].amount = Envelope({ { 0.0f, 0.0f }, { 9.0f, 0.0f } });
    const fluid::FluidLiquidSolver calm = solve(recipe);
    ASSERT_FALSE(blown.Particles().empty());
    EXPECT_GT(MeanX(blown), MeanX(calm) + 0.05f);

    /// @note 発生源の倍率は液体には効かない (掛ける先の density / temperature / fuel を液体は使わない)。
    ///       撒く量を変えるのは count と duration。ここは «黙って効かない» のではなく
    ///       «そう決めた» ことの記録 — 変えるなら CPU (FluidLiquidSolver::Emit) と
    ///       GPU (BuildGpuLiquidEmission) の両方の湧かせ方を同じ規則で直すこと。
    fluid::FluidRecipe muted = LiquidRecipe();
    muted.sources[0].amount = Envelope({ { 0.0f, 0.0f }, { 9.0f, 0.0f } });
    EXPECT_EQ(solve(muted).Particles().size(), solve(LiquidRecipe()).Particles().size());
}

/// @name CPU と GPU

TEST(FluidAmountEnvelopeTest, CpuAndGpuFoldTheSameAmounts)
{
    fluid::FluidRecipe recipe;
    FluidSource source;
    source.density = 3.0f;
    source.temperature = 2.0f;
    source.fuel = 1.5f;
    source.amount = Envelope({ { 0.0f, 1.0f }, { 1.0f, 0.2f } });
    recipe.sources.push_back(source);
    FluidForce vortex;
    vortex.type = FluidForceType::Vortex;
    vortex.strength = 6.0f;
    vortex.amount = Envelope({ { 0.0f, 0.0f }, { 1.0f, 1.0f } });
    recipe.forces.push_back(vortex);

    for (const float time : { 0.0f, 0.25f, 0.5f, 1.0f, 3.0f }) {
        SCOPED_TRACE(time);
        const fluid::FluidGpuStepConstants step = fluid::PackFluidGpuStep(recipe, 32, time, 0.01f, 1.0f, 1.0f);
        /// @note シェーダー (FluidInject.cs.hlsl) は amounts に weight・dt を掛けるだけ。CPU の注入
        ///       (FluidGasSolver::Inject) も «基準の量 × 倍率» を先に作るので、掛ける順まで一致する。
        const float amount = fluid::FluidSourceAmount(recipe.sources[0], time);
        EXPECT_FLOAT_EQ(step.sources[0].amounts[0], recipe.sources[0].density * amount);
        EXPECT_FLOAT_EQ(step.sources[0].amounts[1], recipe.sources[0].temperature * amount);
        EXPECT_FLOAT_EQ(step.sources[0].amounts[2], recipe.sources[0].fuel * amount);
        /// @note 力は «強さ × 倍率» を詰め、influence はシェーダーが掛ける (CPU の strengthScale と同じ順)。
        EXPECT_FLOAT_EQ(step.forces[0].directionStrength[3],
                        recipe.forces[0].strength * fluid::FluidForceAmount(recipe.forces[0], time));
    }

    /// @note 窓の外は倍率に関係なく 0 (枠は残る)。
    recipe.forces[0].startTime = 5.0f;
    recipe.forces[0].duration = 1.0f;
    const fluid::FluidGpuStepConstants outside = fluid::PackFluidGpuStep(recipe, 32, 0.5f, 0.01f, 1.0f, 1.0f);
    EXPECT_EQ(outside.forces[0].directionStrength[3], 0.0f);
    EXPECT_EQ(outside.forceCount, 1u);
}

TEST(FluidAmountEnvelopeTest, PackingWithoutKeysIsBitIdenticalToTheRawValues)
{
    fluid::FluidRecipe recipe;
    FluidSource source;
    source.density = 2.3f;
    source.temperature = 1.7f;
    source.fuel = 0.9f;
    recipe.sources.push_back(source);
    FluidForce force;
    force.strength = 3.1f;
    recipe.forces.push_back(force);

    const fluid::FluidGpuStepConstants step = fluid::PackFluidGpuStep(recipe, 64, 0.37f, 0.01f, 1.0f, 1.0f);
    EXPECT_EQ(step.sources[0].amounts[0], 2.3f);
    EXPECT_EQ(step.sources[0].amounts[1], 1.7f);
    EXPECT_EQ(step.sources[0].amounts[2], 0.9f);
    EXPECT_EQ(step.forces[0].directionStrength[3], 3.1f);
}

/// @name ファイル

TEST(FluidAmountEnvelopeTest, AmountKeysRoundTripThroughToml)
{
    testkit::TempDir temp{ "fluidamount" };
    ASSERT_TRUE(temp.IsValid());

    fluid::FluidRecipe recipe;
    FluidSource source;
    source.name = "Puff";
    source.amount = Envelope({ { 0.0f, 1.25f }, { 0.35f, 0.5f }, { 1.0f, 0.0f } });
    recipe.sources.push_back(source);
    FluidForce force;
    force.name = "Swell";
    force.amount = Envelope({ { 0.1f, 0.0f }, { 0.9f, 2.5f } });
    recipe.forces.push_back(force);

    const std::string path = util::FileSystem::PathToUtf8(temp.File("amount.fluid"));
    ASSERT_TRUE(asset::SaveFluidRecipe(path, recipe));
    fluid::FluidRecipe loaded;
    ASSERT_TRUE(asset::LoadFluidRecipe(path, loaded));

    ASSERT_EQ(loaded.sources.size(), 1u);
    ASSERT_EQ(loaded.sources[0].amount.keys.size(), 3u);
    EXPECT_FLOAT_EQ(loaded.sources[0].amount.keys[1].time, 0.35f);
    EXPECT_FLOAT_EQ(loaded.sources[0].amount.keys[1].scale, 0.5f);
    EXPECT_FLOAT_EQ(loaded.sources[0].amount.keys[2].scale, 0.0f);
    ASSERT_EQ(loaded.forces.size(), 1u);
    ASSERT_EQ(loaded.forces[0].amount.keys.size(), 2u);
    EXPECT_FLOAT_EQ(loaded.forces[0].amount.keys[1].time, 0.9f);
    EXPECT_FLOAT_EQ(loaded.forces[0].amount.keys[1].scale, 2.5f);
}

TEST(FluidAmountEnvelopeTest, AmountlessPartsWriteNoAmountSection)
{
    testkit::TempDir temp{ "fluidamount" };
    ASSERT_TRUE(temp.IsValid());

    /// @note キーが無い部品は節ごと書かない (既存の .fluid に «倍率 1» の節を増やさない)。
    fluid::FluidRecipe recipe;
    recipe.sources.emplace_back();
    recipe.forces.emplace_back();
    const std::filesystem::path file = temp.File("plain.fluid");
    ASSERT_TRUE(asset::SaveFluidRecipe(util::FileSystem::PathToUtf8(file), recipe));
    std::string text;
    ASSERT_TRUE(util::FileSystem::ReadText(file, text));
    EXPECT_EQ(text.find("amount"), std::string::npos) << text;

    fluid::FluidRecipe loaded;
    ASSERT_TRUE(asset::LoadFluidRecipe(util::FileSystem::PathToUtf8(file), loaded));
    ASSERT_EQ(loaded.sources.size(), 1u);
    EXPECT_TRUE(loaded.sources[0].amount.keys.empty());
    EXPECT_TRUE(loaded.forces[0].amount.keys.empty());
}

TEST(FluidAmountEnvelopeTest, HandWrittenKeysAreSortedAndCapped)
{
    testkit::TempDir temp{ "fluidamount" };
    ASSERT_TRUE(temp.IsValid());

    std::string text = "version = 2\nkind = \"gas\"\n\n[[source]]\nname = \"Hand\"\n";
    /// @note 上限を超える数を、しかも降順で書く。
    for (int i = fluid::kMaxFluidAmountKeys + 4; i > 0; --i) {
        text += "[[source.amount.key]]\ntime = " + std::to_string(static_cast<float>(i) * 0.1f)
              + "\nscale = " + std::to_string(static_cast<float>(i)) + "\n";
    }
    const std::filesystem::path file = temp.File("hand.fluid");
    ASSERT_TRUE(util::FileSystem::WriteText(file, text));
    fluid::FluidRecipe loaded;
    ASSERT_TRUE(asset::LoadFluidRecipe(util::FileSystem::PathToUtf8(file), loaded));

    ASSERT_EQ(loaded.sources.size(), 1u);
    const std::vector<fluid::FluidAmountKey>& keys = loaded.sources[0].amount.keys;
    ASSERT_EQ(keys.size(), static_cast<std::size_t>(fluid::kMaxFluidAmountKeys));
    for (std::size_t i = 1; i < keys.size(); ++i) EXPECT_LE(keys[i - 1].time, keys[i].time);
}

} // namespace fbzz::tests
