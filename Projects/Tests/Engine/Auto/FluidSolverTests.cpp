/// @file    FluidSolverTests.cpp
/// @brief   流体レシピのソルバー・プリセット・.fluid 入出力・Motion Vector 符号化を固定する。
/// @author  Hasegawa Jin
/// @date    2026-09-11
///
/// 焼きはエディターでしか走らず、結果は «それらしい絵» なので壊れても気付きにくい。
/// ここでは絵の良し悪しではなく «物理として外してはいけない性質» と «規約» だけを見る。

#include <TestKit/TestKit.hpp>
#include <TestKit/TempDir.hpp>

#include <Engine/Asset/FluidBaker.hpp>
#include <Engine/Asset/FluidOperatorEval.hpp>
#include <Engine/Asset/FluidRecipe.hpp>
#include <Engine/Asset/FluidSolver.hpp>
#include <Engine/Util/FileSystem.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace fbzz::tests {
namespace {

bool AllFinite(const std::vector<float>& values)
{
    return std::all_of(values.begin(), values.end(), [](float v) { return std::isfinite(v); });
}

// 密度で重み付けした高さ (セル単位)。
float DensityCentroidY(const asset::FluidGasSolver& solver)
{
    double weighted = 0.0;
    double total = 0.0;
    for (int y = 0; y < solver.SizeY(); ++y)
        for (int x = 0; x < solver.SizeX(); ++x) {
            const float density = solver.Density()[solver.Index(x, y, 0)];
            weighted += static_cast<double>(density) * y;
            total += density;
        }
    return total > 0.0 ? static_cast<float>(weighted / total) : 0.0f;
}

void AdvanceInFrames(asset::FluidLiquidSolver& solver, float seconds)
{
    constexpr float kFrame = 1.0f / 30.0f;
    for (float t = 0.0f; t < seconds; t += kFrame) solver.Advance(kFrame);
}

// 動かず、上へ撃ち出さない液体の塊。
asset::FluidSource DropletSource(const math::Vector3& center, int count)
{
    asset::FluidSource source;
    source.center = center;
    source.size = { 0.08f, 0.08f, 0.08f };
    source.velocity = { 0.0f, 0.0f, 0.0f };
    source.spread = 0.0f;
    source.count = count;
    source.startTime = 0.0f;
    source.duration = 0.2f;
    return source;
}

// 粒子の中心が障害物の表面から最も深く潜った距離 (外なら正)。
float MinColliderDistance(const asset::FluidLiquidSolver& solver, const asset::FluidCollider& collider)
{
    float minimum = 1.0e9f;
    for (const auto& particle : solver.Particles())
        minimum = (std::min)(minimum, asset::FluidColliderDistance(collider, collider.center,
                                                                   { particle.x, particle.y, particle.z }, 0.0f,
                                                                   solver.IsVolumetric()));
    return minimum;
}

} // namespace

TEST(FluidGasSolverTest, ProjectionRemovesMostOfTheDivergence)
{
    asset::FluidRecipe recipe;
    recipe.gas.pressureIterations = 120;
    asset::FluidGasSolver solver;
    solver.Reset(recipe, 32, 32, 1);
    // 中心から湧き出す滑らかな流れ (発散しか持たない場)。投影でほぼ消えなければならない。
    for (int y = 0; y < 32; ++y) {
        for (int x = 0; x < 32; ++x) {
            const float px = (static_cast<float>(x) + 0.5f) / 32.0f * 2.0f - 1.0f;
            const float py = (static_cast<float>(y) + 0.5f) / 32.0f * 2.0f - 1.0f;
            const float falloff = std::exp(-(px * px + py * py) / 0.08f);
            solver.VelocityX()[solver.Index(x, y, 0)] = px * falloff;
            solver.VelocityY()[solver.Index(x, y, 0)] = py * falloff;
        }
    }
    const float before = solver.MaxDivergence();
    solver.Project();
    const float after = solver.MaxDivergence();
    EXPECT_GT(before, 0.0f);
    EXPECT_LT(after, before * 0.5f);
}

TEST(FluidGasSolverTest, HotSmokeRises)
{
    asset::FluidRecipe recipe = asset::MakeFluidPreset(asset::FluidPreset::Smoke);
    asset::FluidGasSolver solver;
    solver.Reset(recipe, 32, 32, 1);
    solver.Advance(0.3f);
    const float early = DensityCentroidY(solver);
    solver.Advance(0.9f);
    EXPECT_GT(DensityCentroidY(solver), early);
    EXPECT_TRUE(AllFinite(solver.Density()));
    EXPECT_TRUE(AllFinite(solver.VelocityX()));
    EXPECT_TRUE(AllFinite(solver.VelocityY()));
}

TEST(FluidGasSolverTest, SameSeedGivesTheSameResult)
{
    // 焼き直すたびに絵が変わると、パラメーターを 1 つ変えた効果を見比べられない。
    const asset::FluidRecipe recipe = asset::MakeFluidPreset(asset::FluidPreset::Explosion);
    asset::FluidGasSolver a;
    asset::FluidGasSolver b;
    a.Reset(recipe, 24, 24, 1);
    b.Reset(recipe, 24, 24, 1);
    a.Advance(0.4f);
    b.Advance(0.4f);
    EXPECT_EQ(a.Density(), b.Density());
}

TEST(FluidGasSolverTest, CombustionTurnsFuelIntoSmoke)
{
    // 炎のプリセットは煤を直接注入しない。煤があるなら燃焼が働いている。
    const asset::FluidRecipe recipe = asset::MakeFluidPreset(asset::FluidPreset::Fire);
    ASSERT_FALSE(recipe.sources.empty());
    EXPECT_EQ(recipe.sources.front().density, 0.0f);
    asset::FluidGasSolver solver;
    solver.Reset(recipe, 32, 32, 1);
    solver.Advance(0.4f);
    EXPECT_GT(solver.TotalDensity(), 0.0f);
}

TEST(FluidGasSolverTest, VolumeSolveStaysFinite)
{
    const asset::FluidRecipe recipe = asset::MakeFluidPreset(asset::FluidPreset::Explosion);
    asset::FluidGasSolver solver;
    solver.Reset(recipe, 12, 12, 12);
    solver.Advance(0.3f);
    EXPECT_GT(solver.TotalDensity(), 0.0f);
    EXPECT_TRUE(AllFinite(solver.VelocityX()));
    EXPECT_TRUE(AllFinite(solver.VelocityY()));
    EXPECT_TRUE(AllFinite(solver.VelocityZ()));
}

TEST(FluidLiquidSolverTest, ParticlesSettleOnTheFloorWithoutCollapsing)
{
    asset::FluidRecipe recipe;
    recipe.kind = asset::FluidKind::Liquid;
    asset::FluidSource source;
    source.center = { 0.0f, -0.5f, 0.0f };
    source.size = { 0.1f, 0.1f, 0.1f };
    source.velocity = { 0.0f, 0.0f, 0.0f };
    source.spread = 0.0f;
    source.count = 200;
    source.startTime = 0.0f;
    source.duration = 0.2f;
    recipe.sources.push_back(source);
    recipe.liquid.floor = true;
    recipe.liquid.floorHeight = -0.9f;

    asset::FluidLiquidSolver solver;
    solver.Reset(recipe);
    AdvanceInFrames(solver, 2.0f);

    const auto& particles = solver.Particles();
    ASSERT_GE(particles.size(), 190u);
    float minX = 1.0e9f;
    float maxX = -1.0e9f;
    for (const auto& particle : particles) {
        EXPECT_TRUE(std::isfinite(particle.x) && std::isfinite(particle.y));
        EXPECT_GE(particle.y, recipe.liquid.floorHeight - 1.0e-3f);
        minX = (std::min)(minX, particle.x);
        maxX = (std::max)(maxX, particle.x);
    }
    // 密度拘束が効いていれば、粒子は 1 点に潰れず床の上へ広がる。
    EXPECT_GT(maxX - minX, solver.ParticleRadius() * 10.0f);
}

TEST(FluidLiquidSolverTest, DensityStaysNearRest)
{
    asset::FluidRecipe recipe = asset::MakeFluidPreset(asset::FluidPreset::LavaBlob);
    asset::FluidLiquidSolver solver;
    solver.Reset(recipe);
    AdvanceInFrames(solver, 1.5f);
    const auto& particles = solver.Particles();
    ASSERT_FALSE(particles.empty());
    double total = 0.0;
    for (std::size_t i = 0; i < particles.size(); ++i) total += solver.MeasuredDensity(i);
    const float average = static_cast<float>(total / static_cast<double>(particles.size())) / solver.RestDensity();
    // 表面の粒子は近傍が少ないので平均は 1 を下回る。潰れ (> 1.6) と散乱 (< 0.3) だけを弾く。
    EXPECT_GT(average, 0.3f);
    EXPECT_LT(average, 1.6f);
}

TEST(FluidRecipeTest, EveryPresetProducesAVisibleFrame)
{
    // プリセットの数値を触ったときに «焼いたら真っ白 / 真っ透明» を見逃さないための網。
    for (int index = 0; index < static_cast<int>(asset::FluidPreset::Count); ++index) {
        const auto preset = static_cast<asset::FluidPreset>(index);
        asset::FluidRecipe recipe = asset::MakeFluidPreset(preset);
        recipe.gas.resolution = 32;
        const float time = (std::min)(recipe.output.warmup + recipe.output.duration * 0.3f, 1.2f);
        asset::FluidFrameImage frame;
        if (recipe.kind == asset::FluidKind::Gas) {
            asset::FluidGasSolver solver;
            solver.Reset(recipe, 32, 32, 1);
            solver.Advance(time);
            asset::RenderFluidGasFrame(solver, recipe, 32, 1.0f / 30.0f, frame);
        } else {
            asset::FluidLiquidSolver solver;
            solver.Reset(recipe);
            AdvanceInFrames(solver, time);
            asset::RenderFluidLiquidFrame(solver, recipe, 32, 1.0f / 30.0f, frame);
        }
        float alpha = 0.0f;
        for (std::size_t pixel = 0; pixel < static_cast<std::size_t>(frame.size) * frame.size; ++pixel)
            alpha += frame.rgba[pixel * 4 + 3];
        EXPECT_GT(alpha, 0.5f) << asset::FluidPresetName(preset);
        EXPECT_TRUE(AllFinite(frame.rgba)) << asset::FluidPresetName(preset);
        EXPECT_TRUE(AllFinite(frame.motion)) << asset::FluidPresetName(preset);
    }
}

TEST(FluidRecipeTest, RoundTripsThroughToml)
{
    testkit::TempDir temp{ "fluidrecipe" };
    ASSERT_TRUE(temp.IsValid());

    asset::FluidRecipe gas = asset::MakeFluidPreset(asset::FluidPreset::Explosion);
    gas.seed = 42;
    gas.gas.floor = true;
    gas.output.columns = 6;
    gas.output.vectorField = true;
    gas.output.vectorFieldExtents = { 3.0f, 5.0f, 3.0f };
    const std::string gasPath = util::FileSystem::PathToUtf8(temp.File("explosion.fluid"));
    ASSERT_TRUE(asset::SaveFluidRecipe(gasPath, gas));
    asset::FluidRecipe loaded;
    ASSERT_TRUE(asset::LoadFluidRecipe(gasPath, loaded));
    EXPECT_EQ(loaded.kind, asset::FluidKind::Gas);
    EXPECT_EQ(loaded.seed, 42u);
    EXPECT_TRUE(loaded.gas.floor);
    EXPECT_FLOAT_EQ(loaded.gas.burnExpansion, gas.gas.burnExpansion);
    ASSERT_EQ(loaded.sources.size(), gas.sources.size());
    EXPECT_FLOAT_EQ(loaded.sources[0].center.y, gas.sources[0].center.y);
    EXPECT_FLOAT_EQ(loaded.sources[0].duration, gas.sources[0].duration);
    EXPECT_EQ(loaded.render.shading, asset::FluidShading::Fire);
    EXPECT_FLOAT_EQ(loaded.render.fireKelvin, gas.render.fireKelvin);
    EXPECT_EQ(loaded.output.columns, 6);
    EXPECT_TRUE(loaded.output.vectorField);
    EXPECT_FLOAT_EQ(loaded.output.vectorFieldExtents.y, 5.0f);

    const asset::FluidRecipe liquid = asset::MakeFluidPreset(asset::FluidPreset::BloodBurst);
    const std::string liquidPath = util::FileSystem::PathToUtf8(temp.File("blood.fluid"));
    ASSERT_TRUE(asset::SaveFluidRecipe(liquidPath, liquid));
    asset::FluidRecipe loadedLiquid;
    ASSERT_TRUE(asset::LoadFluidRecipe(liquidPath, loadedLiquid));
    EXPECT_EQ(loadedLiquid.kind, asset::FluidKind::Liquid);
    ASSERT_EQ(loadedLiquid.sources.size(), liquid.sources.size());
    EXPECT_FLOAT_EQ(loadedLiquid.sources[0].spread, liquid.sources[0].spread);
    EXPECT_FALSE(loadedLiquid.liquid.floor);
    EXPECT_FLOAT_EQ(loadedLiquid.liquid.particleLifetime, liquid.liquid.particleLifetime);
}

TEST(FluidGasSolverTest, DetailLayersCrossFadeWithoutPopping)
{
    // 層を初期位置へ戻す瞬間にその層の重みが 0 でないと、細部のノイズがコマ間で «跳ぶ»。
    asset::FluidRecipe recipe = asset::MakeFluidPreset(asset::FluidPreset::Smoke);
    recipe.gas.detailPeriod = 0.5f;
    asset::FluidGasSolver solver;
    solver.Reset(recipe, 16, 16, 1);
    ASSERT_TRUE(solver.HasDetail());
    for (int step = 0; step < 40; ++step) {
        solver.Step(1.0f / 60.0f);
        float weight0 = 0.0f;
        float weight1 = 0.0f;
        solver.DetailWeights(weight0, weight1);
        EXPECT_NEAR(weight0 + weight1, 1.0f, 1.0e-5f);
        EXPECT_GE(weight0, 0.0f);
        EXPECT_GE(weight1, 0.0f);
    }
}

TEST(FluidGasSolverTest, DetailCoordinatesMoveWithTheFlow)
{
    // 右へ一様に流れていれば、原点にある細部の座標は «少し左から運ばれてきたもの» になる。
    asset::FluidRecipe recipe;
    recipe.gas.detailPeriod = 10.0f;
    recipe.gas.turbulence = 0.0f;
    recipe.gas.vorticity = 0.0f;
    recipe.gas.buoyancy = 0.0f;
    asset::FluidGasSolver solver;
    solver.Reset(recipe, 32, 32, 1);
    std::fill(solver.VelocityX().begin(), solver.VelocityX().end(), 0.5f);
    solver.Step(0.1f);
    float u = 0.0f;
    float v = 0.0f;
    solver.SampleDetailCoordinate(0, 0.0f, 0.0f, u, v);
    EXPECT_LT(u, 0.0f);
    EXPECT_NEAR(v, 0.0f, 1.0e-3f);
    EXPECT_FALSE(asset::FluidGasSolver{}.HasDetail());
}

TEST(FluidGasSolverTest, VolumeSolveHasNoDetailLayers)
{
    // 3D (.vfield) は速度しか焼かない。細部の座標を持つとメモリだけ 2 倍になる。
    const asset::FluidRecipe recipe = asset::MakeFluidPreset(asset::FluidPreset::Smoke);
    asset::FluidGasSolver solver;
    solver.Reset(recipe, 8, 8, 8);
    EXPECT_FALSE(solver.HasDetail());
}

// ── 障害物 ──

TEST(FluidGasSolverTest, SphereColliderKeepsSmokeOutAndTheFlowProjected)
{
    asset::FluidCollider collider;
    collider.shape = asset::FluidColliderShape::Sphere;
    collider.center = { 0.0f, -0.3f, 0.0f };
    collider.size = { 0.25f, 0.25f, 0.25f };
    const auto solve = [&collider](bool blocked) {
        asset::FluidRecipe recipe;
        recipe.gas.turbulence = 0.0f;
        recipe.gas.vorticity = 0.0f;
        recipe.gas.pressureIterations = 80;
        // 煙の重みを切って密度を受け身のスカラーにする。これで «届いたか» は注入量に比例して測れる。
        recipe.gas.weight = 0.0f;
        asset::FluidSource source;
        source.center = { 0.0f, -0.75f, 0.0f };
        source.size = { 0.15f, 0.15f, 0.15f };
        source.velocity = { 0.0f, 0.8f, 0.0f };
        source.density = 20.0f;
        source.noise = 0.0f;
        recipe.sources.push_back(source);
        if (blocked) recipe.colliders.push_back(collider);
        asset::FluidGasSolver solver;
        solver.Reset(recipe, 32, 32, 1);
        // 止まった場から 1 回の Advance で進めると CFL の分割が 1 回になり、2 秒を 1 刻みで解いてしまう。
        for (int frame = 0; frame < 60; ++frame) solver.Advance(1.0f / 30.0f);
        return solver;
    };
    asset::FluidGasSolver blocked = solve(true);
    asset::FluidGasSolver open = solve(false);

    // 障害物が無ければ煙は障害物の居る所まで届く (= 下の «中は 0» が空振りではない)。
    // 絶対値ではなく «開けたときと塞いだときの差» で見る: 柱の濃さは浮力と散逸で桁が動くが、
    // 塞げばそこは必ず 0 になる。
    const float openAtCentre = open.SampleDensity(0.0f, collider.center.y);
    const float blockedAtCentre = blocked.SampleDensity(0.0f, collider.center.y);
    EXPECT_GT(openAtCentre, 1.0e-3f);
    EXPECT_EQ(blockedAtCentre, 0.0f);
    const float h = blocked.CellSize();
    int insideCells = 0;
    float insideDensity = 0.0f;
    for (int y = 0; y < blocked.SizeY(); ++y)
        for (int x = 0; x < blocked.SizeX(); ++x) {
            const math::Vector3 p = { (static_cast<float>(x) + 0.5f - static_cast<float>(blocked.SizeX()) * 0.5f) * h,
                                      (static_cast<float>(y) + 0.5f - static_cast<float>(blocked.SizeY()) * 0.5f) * h,
                                      0.0f };
            if (asset::FluidColliderDistance(collider, collider.center, p, h, false) >= 0.0f) continue;
            ++insideCells;
            insideDensity = (std::max)(insideDensity, blocked.Density()[blocked.Index(x, y, 0)]);
        }
    ASSERT_GT(insideCells, 0);
    EXPECT_EQ(insideDensity, 0.0f);
    EXPECT_GT(blocked.TotalDensity(), 0.0f);

    // 固体の隣を壁として解いても、投影は障害物が無いときと同程度に発散を消せる。
    blocked.Project();
    open.Project();
    EXPECT_TRUE(std::isfinite(blocked.MaxDivergence()));
    EXPECT_LT(blocked.MaxDivergence(), open.MaxDivergence() * 4.0f + 1.0f);
    EXPECT_TRUE(AllFinite(blocked.VelocityX()));
    EXPECT_TRUE(AllFinite(blocked.VelocityY()));
}

TEST(FluidGasSolverTest, MovingColliderPushesTheAirAhead)
{
    const auto solve = [](bool inheritVelocity) {
        asset::FluidRecipe recipe;
        recipe.gas.turbulence = 0.0f;
        recipe.gas.vorticity = 0.0f;
        recipe.gas.buoyancy = 0.0f;
        recipe.gas.weight = 0.0f;
        asset::FluidCollider collider;
        collider.shape = asset::FluidColliderShape::Sphere;
        collider.center = { 0.0f, 0.0f, 0.0f };
        collider.size = { 0.2f, 0.2f, 0.2f };
        collider.motion.keys.push_back({ 0.0f, { -0.6f, 0.0f, 0.0f } });
        collider.motion.keys.push_back({ 1.0f, { 0.6f, 0.0f, 0.0f } });
        collider.motion.inheritVelocity = inheritVelocity;
        recipe.colliders.push_back(collider);
        asset::FluidGasSolver solver;
        solver.Reset(recipe, 32, 32, 1);
        for (int step = 0; step < 20; ++step) solver.Step(1.0f / 60.0f);
        // 1/3 秒後、球の中心は x ≈ −0.2 で前の縁は x ≈ 0。その少し先を見る。
        float vx = 0.0f;
        float vy = 0.0f;
        float vz = 0.0f;
        solver.SampleVelocity(0.15f, 0.0f, 0.0f, vx, vy, vz);
        return vx;
    };
    const float pushed = solve(true);
    const float still = solve(false);
    EXPECT_GT(pushed, 0.05f);
    // 速さを渡さない障害物はただ場所を塞ぐだけで、止まった空気は止まったまま。
    EXPECT_NEAR(still, 0.0f, 1.0e-6f);
}

TEST(FluidLiquidSolverTest, ParticlesNeverEndInsideASphereCollider)
{
    asset::FluidRecipe recipe;
    recipe.kind = asset::FluidKind::Liquid;
    recipe.liquid.floor = true;
    recipe.liquid.floorHeight = -0.9f;
    recipe.sources.push_back(DropletSource({ 0.02f, 0.35f, 0.0f }, 200));
    asset::FluidCollider collider;
    collider.shape = asset::FluidColliderShape::Sphere;
    collider.center = { 0.0f, -0.1f, 0.0f };
    collider.size = { 0.25f, 0.25f, 0.25f };
    recipe.colliders.push_back(collider);

    asset::FluidLiquidSolver solver;
    solver.Reset(recipe);
    constexpr float kFrame = 1.0f / 30.0f;
    for (int frame = 0; frame < 45; ++frame) {
        solver.Advance(kFrame);
        EXPECT_GE(MinColliderDistance(solver, collider), -1.0e-3f) << "frame " << frame;
    }
    ASSERT_FALSE(solver.Particles().empty());
    for (const auto& particle : solver.Particles())
        EXPECT_TRUE(std::isfinite(particle.x) && std::isfinite(particle.y));
}

TEST(FluidLiquidSolverTest, TiltedPlaneColliderCatchesTheParticles)
{
    asset::FluidRecipe recipe;
    recipe.kind = asset::FluidKind::Liquid;
    recipe.liquid.floor = false;
    recipe.sources.push_back(DropletSource({ 0.3f, 0.1f, 0.0f }, 150));
    recipe.sources.back().duration = 0.0f;
    asset::FluidCollider plane;
    plane.shape = asset::FluidColliderShape::Plane;
    plane.center = { 0.0f, -0.3f, 0.0f };
    plane.direction = { -0.5f, 0.8660254f, 0.0f };   // 30° 傾いた斜面 (右が高い)
    recipe.colliders.push_back(plane);

    asset::FluidLiquidSolver solver;
    solver.Reset(recipe);
    AdvanceInFrames(solver, 1.0f);

    // 斜面が無ければ 1 秒で y < −1.6 まで落ちて全部消えている。
    EXPECT_GE(solver.Particles().size(), 120u);
    EXPECT_GE(MinColliderDistance(solver, plane), -1.0e-3f);
    // 受け止めた後は斜面を下る (左へ流れる)。
    double meanX = 0.0;
    for (const auto& particle : solver.Particles()) meanX += particle.x;
    if (!solver.Particles().empty()) meanX /= static_cast<double>(solver.Particles().size());
    EXPECT_LT(meanX, 0.3);
}

TEST(FluidRecipeTest, TextureSourceWithoutAnImageStillFillsThePlate)
{
    // 画像が読めないマスクは 1 として引く。«何も起きない» より原因に気付きやすい。
    asset::FluidSource plate;
    plate.shape = asset::FluidSourceShape::Texture;
    plate.texture = "Textures/__missing_fluid_mask__.png";
    plate.center = { 0.0f, -0.4f, 0.0f };
    plate.size = { 0.2f, 0.1f, 0.05f };
    plate.direction = { 0.0f, 0.0f, 1.0f };
    plate.noise = 0.0f;
    plate.count = 100;
    plate.spread = 0.0f;
    plate.velocity = { 0.0f, 0.0f, 0.0f };

    asset::FluidRecipe gas;
    gas.sources.push_back(plate);
    asset::FluidGasSolver gasSolver;
    gasSolver.Reset(gas, 32, 32, 1);
    gasSolver.Advance(0.2f);
    EXPECT_GT(gasSolver.TotalDensity(), 0.0f);

    asset::FluidRecipe liquid;
    liquid.kind = asset::FluidKind::Liquid;
    liquid.sources.push_back(plate);
    asset::FluidLiquidSolver liquidSolver;
    liquidSolver.Reset(liquid);
    liquidSolver.Advance(1.0f / 30.0f);
    EXPECT_EQ(liquidSolver.Particles().size(), 100u);
}

TEST(FluidRecipeTest, AutoGridFollowsTheFrameSize)
{
    asset::FluidRecipe recipe;
    recipe.gas.resolution = 0;
    recipe.output.frameSize = 128;
    EXPECT_EQ(asset::ResolveGasResolution(recipe), 128);
    recipe.output.frameSize = 512;
    EXPECT_EQ(asset::ResolveGasResolution(recipe), 256);   // 焼き時間の都合で頭打ち。先は細部ノイズ
    recipe.gas.resolution = 96;
    EXPECT_EQ(asset::ResolveGasResolution(recipe), 96);
}

// ── 色の鍵 ──

TEST(FluidGasSolverTest, ColorKeyStaysWithItsSource)
{
    // 鍵 0 と鍵 1 の発生源を離して置く。煙の量で重み付けした鍵なので、それぞれの芯は自分の鍵になる。
    asset::FluidRecipe recipe;
    recipe.gas.turbulence = 0.0f;
    recipe.gas.vorticity = 0.0f;
    asset::FluidSource left;
    left.center = { -0.5f, -0.6f, 0.0f };
    left.size = { 0.15f, 0.15f, 0.15f };
    left.noise = 0.0f;
    left.colorKey = 0.0f;
    asset::FluidSource right = left;
    right.center.x = 0.5f;
    right.colorKey = 1.0f;
    recipe.sources = { left, right };

    asset::FluidGasSolver solver;
    solver.Reset(recipe, 32, 32, 1);
    for (int frame = 0; frame < 24; ++frame) solver.Advance(1.0f / 30.0f);

    ASSERT_GT(solver.SampleDensity(-0.5f, -0.6f), 0.01f);
    ASSERT_GT(solver.SampleDensity(0.5f, -0.6f), 0.01f);
    EXPECT_LT(solver.SampleColorKey(-0.5f, -0.6f), 0.05f);
    EXPECT_GT(solver.SampleColorKey(0.5f, -0.6f), 0.95f);

    const std::vector<float>& key = solver.ColorKey();
    ASSERT_EQ(key.size(), solver.Density().size());
    EXPECT_TRUE(std::all_of(key.begin(), key.end(),
                            [](float k) { return std::isfinite(k) && k >= 0.0f && k <= 1.0f; }));
}

TEST(FluidGasSolverTest, ColorKeyDoesNotChangeTheFlow)
{
    // 鍵は «描く色を選ぶ» だけ。付けても付けなくても流れ (密度・温度) は 1 ビットも変わらない。
    // 燃焼で煤が生まれる爆発で見る (煤が鍵を受け継ぐ経路も通る)。
    asset::FluidRecipe plain = asset::MakeFluidPreset(asset::FluidPreset::Explosion);
    // プリセットは鍵付きの発生源 (Dust Ring) を持つ。«鍵なし» の側は明示的に全部 0 にする。
    for (auto& source : plain.sources) source.colorKey = 0.0f;
    asset::FluidRecipe keyed = plain;
    for (auto& source : keyed.sources) source.colorKey = 0.7f;
    asset::FluidGasSolver a;
    asset::FluidGasSolver b;
    a.Reset(plain, 24, 24, 1);
    b.Reset(keyed, 24, 24, 1);
    a.Advance(0.4f);
    b.Advance(0.4f);
    EXPECT_EQ(a.Density(), b.Density());
    EXPECT_EQ(a.Temperature(), b.Temperature());
    // 鍵の付いた発生源が無いレシピは、鍵が全部 0 (色の量を運ぶのも飛ばしている)。
    EXPECT_TRUE(std::all_of(a.ColorKey().begin(), a.ColorKey().end(), [](float k) { return k == 0.0f; }));
    EXPECT_EQ(a.SampleColorKey(0.0f, 0.0f), 0.0f);
}

TEST(FluidGasSolverTest, FuelOnlyFireCarriesItsKeyIntoTheSoot)
{
    // 炎のプリセットは煙を注がない (density 0・fuel > 0)。煤は «燃えた燃料の鍵» で生まれるので、
    // 発生源に鍵を付ければ、一度も煙を注いでいなくてもその色が乗る。
    asset::FluidRecipe plain = asset::MakeFluidPreset(asset::FluidPreset::Fire);
    ASSERT_FALSE(plain.sources.empty());
    ASSERT_EQ(plain.sources.front().density, 0.0f);
    ASSERT_GT(plain.sources.front().fuel, 0.0f);
    for (auto& source : plain.sources) source.colorKey = 0.0f;
    asset::FluidRecipe keyed = plain;
    for (auto& source : keyed.sources) source.colorKey = 0.8f;

    asset::FluidGasSolver a;
    asset::FluidGasSolver b;
    a.Reset(plain, 32, 32, 1);
    b.Reset(keyed, 32, 32, 1);
    for (int frame = 0; frame < 18; ++frame) {
        a.Advance(1.0f / 30.0f);
        b.Advance(1.0f / 30.0f);
    }
    ASSERT_GT(b.TotalDensity(), 0.0f);
    // 燃料に色を積んでも燃え方は変わらない。
    EXPECT_EQ(a.Density(), b.Density());
    EXPECT_EQ(a.Temperature(), b.Temperature());

    double weighted = 0.0;
    double total = 0.0;
    for (std::size_t i = 0; i < b.Density().size(); ++i) {
        weighted += static_cast<double>(b.ColorKey()[i]) * static_cast<double>(b.Density()[i]);
        total += static_cast<double>(b.Density()[i]);
    }
    ASSERT_GT(total, 0.0);
    EXPECT_NEAR(weighted / total, 0.8, 0.05);
    EXPECT_TRUE(std::all_of(a.ColorKey().begin(), a.ColorKey().end(), [](float k) { return k == 0.0f; }));
}

// ── 部品の差し替え (Fluid Editor のプレビュー) ──

TEST(FluidGasSolverTest, ReplaceOperatorsKeepsTheFieldAndAppliesTheNewParts)
{
    asset::FluidRecipe recipe;
    recipe.gas.turbulence = 0.0f;
    recipe.gas.vorticity = 0.0f;
    asset::FluidSource source;
    source.center = { -0.5f, -0.6f, 0.0f };
    source.size = { 0.15f, 0.15f, 0.15f };
    source.noise = 0.0f;
    recipe.sources.push_back(source);

    asset::FluidGasSolver solver;
    solver.Reset(recipe, 32, 32, 1);
    for (int frame = 0; frame < 12; ++frame) solver.Advance(1.0f / 30.0f);
    const std::vector<float> field = solver.Density();
    const float time = solver.Time();
    const float leftBefore  = solver.SampleDensity(-0.5f, -0.6f);
    const float rightBefore = solver.SampleDensity(0.5f, -0.6f);
    ASSERT_GT(leftBefore, 0.01f);

    asset::FluidRecipe moved = recipe;
    moved.sources[0].center.x = 0.5f;
    ASSERT_TRUE(solver.ReplaceOperators(moved));
    // 差し替えただけでは場も時刻も 1 ビットも動かない (解き直しの途中から «続ける» ための入口)。
    EXPECT_EQ(solver.Density(), field);
    EXPECT_EQ(solver.Time(), time);

    for (int frame = 0; frame < 12; ++frame) solver.Advance(1.0f / 30.0f);
    // 新しい場所に湧き、古い場所はもう積み増されない (昇って薄くなるだけ)。
    EXPECT_GT(solver.SampleDensity(0.5f, -0.6f), rightBefore + 0.01f);
    EXPECT_LT(solver.SampleDensity(-0.5f, -0.6f), leftBefore);
}

TEST(FluidGasSolverTest, ReplaceOperatorsRefusesWhatTheFieldDependsOn)
{
    const asset::FluidRecipe recipe;
    asset::FluidGasSolver solver;
    solver.Reset(recipe, 32, 32, 1);

    // 刻みごとに効くだけの設定は途中から変えてよい。
    asset::FluidRecipe tuned = recipe;
    tuned.gas.buoyancy = 5.0f;
    tuned.gas.floor = true;
    EXPECT_TRUE(solver.ReplaceOperators(tuned));

    // 格子の意味が変わるもの (ノイズの切り出し・要求解像度・種類) は断る。
    asset::FluidRecipe reseeded = recipe;
    reseeded.seed = recipe.seed + 1u;
    EXPECT_FALSE(solver.ReplaceOperators(reseeded));
    asset::FluidRecipe resized = recipe;
    resized.gas.resolution = 64;
    EXPECT_FALSE(solver.ReplaceOperators(resized));
    asset::FluidRecipe liquid = recipe;
    liquid.kind = asset::FluidKind::Liquid;
    EXPECT_FALSE(solver.ReplaceOperators(liquid));
}

TEST(FluidLiquidSolverTest, ReplaceOperatorsKeepsTheParticlesAndTheEmittedCounts)
{
    asset::FluidRecipe recipe;
    recipe.kind = asset::FluidKind::Liquid;
    recipe.sources.push_back(DropletSource({ -0.4f, 0.2f, 0.0f }, 120));

    asset::FluidLiquidSolver solver;
    solver.Reset(recipe);
    AdvanceInFrames(solver, 0.5f);
    const std::size_t count = solver.Particles().size();
    ASSERT_EQ(count, 120u);
    const float time = solver.Time();

    asset::FluidRecipe moved = recipe;
    moved.sources[0].center.x = 0.4f;
    ASSERT_TRUE(solver.ReplaceOperators(moved));
    EXPECT_EQ(solver.Particles().size(), count);
    EXPECT_EQ(solver.Time(), time);

    // 撃ち出し済みの数を発生源ごとに引き継ぐので、同じ発生源が最初から湧き直すことはない。
    AdvanceInFrames(solver, 0.3f);
    EXPECT_EQ(solver.Particles().size(), count);

    // 足した発生源はその場で効く。
    asset::FluidRecipe added = moved;
    added.sources.push_back(DropletSource({ 0.4f, 0.5f, 0.0f }, 40));
    ASSERT_TRUE(solver.ReplaceOperators(added));
    AdvanceInFrames(solver, 0.3f);
    EXPECT_EQ(solver.Particles().size(), count + 40);

    // 粒子の並びの意味が変わるもの (粒子半径・種類) は断る。
    asset::FluidRecipe thicker = added;
    thicker.liquid.particleRadius = added.liquid.particleRadius * 2.0f;
    EXPECT_FALSE(solver.ReplaceOperators(thicker));
    asset::FluidRecipe gas = added;
    gas.kind = asset::FluidKind::Gas;
    EXPECT_FALSE(solver.ReplaceOperators(gas));
}

} // namespace fbzz::tests
