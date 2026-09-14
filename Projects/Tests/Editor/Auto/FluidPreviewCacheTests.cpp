/// @file    FluidPreviewCacheTests.cpp
/// @brief   Fluid Editor のプレビューキャッシュ — 解き直しの起点 (FluidInvalidationTime) と裏の解きの契約
/// @author  Hasegawa Jin
/// @date    2026-09-12
///
/// 起点を遅く見積もると «古いコマが残って焼いた絵と違う» が黙って起き、早く見積もると毎回頭から解き直しになる。
/// どちらも画面では気付きにくいので、変更の種類ごとに起点を縛る。
/// 続きから解く経路 (ReplaceOperators) も «速くなった» としか画面に出ないので、残した帯と締め切りで縛る。
#include <TestKit/TestKit.hpp>

#include <Editor/Util/FluidPreviewCache.hpp>
#include <Engine/Asset/FluidBaker.hpp>
#include <Engine/Asset/FluidRecipe.hpp>

#include <chrono>
#include <thread>

namespace fbzz::tests {
namespace {

using editor::FluidInvalidationTime;
using editor::FluidPreviewCache;
using editor::FluidPreviewQuality;

asset::FluidRecipe RecipeWithSourceAt(float startTime)
{
    asset::FluidRecipe recipe;
    recipe.kind = asset::FluidKind::Gas;
    recipe.output.warmup = 0.0f;
    asset::FluidSource source;
    source.name = "Puff";
    source.startTime = startTime;
    recipe.sources.push_back(source);
    return recipe;
}

// 4 コマ・粗い格子で、裏の解きが数百ミリ秒で終わる大きさ。
asset::FluidRecipe TinyRecipe()
{
    asset::FluidRecipe recipe = RecipeWithSourceAt(0.0f);
    recipe.output.columns = 2;
    recipe.output.rows = 2;
    recipe.output.duration = 0.2f;
    recipe.output.substeps = 1;
    recipe.gas.pressureIterations = 10;
    return recipe;
}

// 8 コマ (dt = 0.1)。後ろの発生源は «プレビューの 0.5 秒» から効く (部品の時刻はソルバーの時計なので
// 0.5 + warmup)。warmup を長く取ってあるのは «頭から解き直したか» を時間で見分けるため — 頭からだと
// warmup 20 刻み + 8 コマ、続きからだと 3 コマで済む。
asset::FluidRecipe ResumeRecipe()
{
    asset::FluidRecipe recipe = RecipeWithSourceAt(0.0f);
    recipe.output.columns = 4;
    recipe.output.rows = 2;
    recipe.output.duration = 0.8f;
    recipe.output.warmup = 2.0f;
    recipe.output.substeps = 1;
    recipe.gas.pressureIterations = 20;
    asset::FluidSource late;
    late.name = "Late";
    late.startTime = 2.5f;
    recipe.sources.push_back(late);
    return recipe;
}

template <class Predicate>
bool TickUntil(FluidPreviewCache& cache, Predicate done,
               std::chrono::milliseconds timeout = std::chrono::milliseconds(10000))
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        cache.Tick(nullptr, nullptr);
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

std::chrono::milliseconds MillisecondsSince(std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
}

} // namespace

// ── FluidInvalidationTime ──

TEST(FluidInvalidationTime, NothingChangedNeedsNoResolve)
{
    const asset::FluidRecipe recipe = RecipeWithSourceAt(0.5f);
    EXPECT_LT(FluidInvalidationTime(recipe, recipe), 0.0f);
}

TEST(FluidInvalidationTime, LookOnlyChangeOnlyRedraws)
{
    const asset::FluidRecipe before = RecipeWithSourceAt(0.5f);
    asset::FluidRecipe after = before;
    after.render.opacity += 1.0f;
    after.render.smokeColor.x = 0.9f;
    EXPECT_LT(FluidInvalidationTime(before, after), 0.0f);
}

TEST(FluidInvalidationTime, BakeOnlyChangeDoesNotTouchThePreview)
{
    const asset::FluidRecipe before = RecipeWithSourceAt(0.5f);
    asset::FluidRecipe after = before;
    after.bake.volumeResolution += 32;
    after.output.motionVectors = !after.output.motionVectors;
    after.output.vectorField = !after.output.vectorField;
    after.output.supersampling = 2;
    EXPECT_LT(FluidInvalidationTime(before, after), 0.0f);
}

TEST(FluidInvalidationTime, RenamingAPartOnlyRedraws)
{
    const asset::FluidRecipe before = RecipeWithSourceAt(0.5f);
    asset::FluidRecipe after = before;
    after.sources[0].name = "Renamed";
    EXPECT_LT(FluidInvalidationTime(before, after), 0.0f);
}

TEST(FluidInvalidationTime, ChangedSourceResolvesFromItsStartTime)
{
    const asset::FluidRecipe before = RecipeWithSourceAt(0.5f);
    asset::FluidRecipe after = before;
    after.sources[0].density = 5.0f;
    EXPECT_FLOAT_EQ(FluidInvalidationTime(before, after), 0.5f);
}

TEST(FluidInvalidationTime, MovingTheStartTimeResolvesFromTheEarlierOne)
{
    const asset::FluidRecipe before = RecipeWithSourceAt(0.5f);
    asset::FluidRecipe after = before;
    after.sources[0].startTime = 0.8f;
    EXPECT_FLOAT_EQ(FluidInvalidationTime(before, after), 0.5f);
}

TEST(FluidInvalidationTime, WarmupIsSubtracted)
{
    asset::FluidRecipe before = RecipeWithSourceAt(0.5f);
    before.output.warmup = 0.2f;
    asset::FluidRecipe after = before;
    after.sources[0].density = 5.0f;
    EXPECT_NEAR(FluidInvalidationTime(before, after), 0.3f, 1.0e-5f);
}

TEST(FluidInvalidationTime, PartActiveDuringWarmupResolvesFromTheStart)
{
    asset::FluidRecipe before = RecipeWithSourceAt(0.1f);
    before.output.warmup = 0.5f;
    asset::FluidRecipe after = before;
    after.sources[0].density = 5.0f;
    EXPECT_FLOAT_EQ(FluidInvalidationTime(before, after), 0.0f);
}

TEST(FluidInvalidationTime, MotionKeyResolvesNoLaterThanTheKey)
{
    asset::FluidRecipe before = RecipeWithSourceAt(0.0f);
    before.sources[0].motion.keys = { { 0.1f, { 0.0f, 0.0f, 0.0f } }, { 0.3f, { 0.2f, 0.0f, 0.0f } } };
    asset::FluidRecipe after = before;
    after.sources[0].motion.keys[1].offset.x = 0.4f;
    const float from = FluidInvalidationTime(before, after);
    EXPECT_GE(from, 0.0f);
    EXPECT_LE(from, 0.3f);
}

// キーの間は直線で結ぶので、変えたキーの 1 つ前から道筋が変わる。
TEST(FluidInvalidationTime, LateMotionKeyResolvesFromThePreviousKey)
{
    asset::FluidRecipe before = RecipeWithSourceAt(0.0f);
    before.sources[0].motion.keys = { { 0.2f, { 0.0f, 0.0f, 0.0f } },
                                      { 0.3f, { 0.1f, 0.0f, 0.0f } },
                                      { 0.6f, { 0.2f, 0.0f, 0.0f } } };
    asset::FluidRecipe after = before;
    after.sources[0].motion.keys[2].offset.y = 0.5f;
    EXPECT_FLOAT_EQ(FluidInvalidationTime(before, after), 0.3f);
}

TEST(FluidInvalidationTime, SimulationChangeResolvesEverything)
{
    const asset::FluidRecipe before = RecipeWithSourceAt(0.5f);
    asset::FluidRecipe after = before;
    after.gas.buoyancy += 1.0f;
    EXPECT_FLOAT_EQ(FluidInvalidationTime(before, after), 0.0f);
}

TEST(FluidInvalidationTime, OutputTimingChangeResolvesEverything)
{
    const asset::FluidRecipe before = RecipeWithSourceAt(0.5f);
    asset::FluidRecipe after = before;
    after.output.duration = 3.0f;
    EXPECT_FLOAT_EQ(FluidInvalidationTime(before, after), 0.0f);
}

TEST(FluidInvalidationTime, AddingAPartResolvesEverything)
{
    const asset::FluidRecipe before = RecipeWithSourceAt(0.5f);
    asset::FluidRecipe after = before;
    after.forces.push_back(asset::FluidForce{});
    EXPECT_FLOAT_EQ(FluidInvalidationTime(before, after), 0.0f);
}

// 有効な部品の中での添字でノイズが決まるため、1 つ消すと他の部品の出方が最初から変わる。
TEST(FluidInvalidationTime, TogglingAPartResolvesEverything)
{
    const asset::FluidRecipe before = RecipeWithSourceAt(0.5f);
    asset::FluidRecipe after = before;
    after.sources[0].enabled = false;
    EXPECT_FLOAT_EQ(FluidInvalidationTime(before, after), 0.0f);
}

TEST(FluidInvalidationTime, EditingADisabledPartNeedsNoResolve)
{
    asset::FluidRecipe before = RecipeWithSourceAt(0.5f);
    before.sources[0].enabled = false;
    asset::FluidRecipe after = before;
    after.sources[0].density = 5.0f;
    EXPECT_LT(FluidInvalidationTime(before, after), 0.0f);
}

// ── FluidPreviewCache ──

TEST(FluidPreviewCache, SolvesInTheBackgroundWithoutARenderer)
{
    FluidPreviewCache cache;
    cache.SetQuality(FluidPreviewQuality::Draft);
    cache.SetRecipe(TinyRecipe(), 1, 0.0f);
    EXPECT_EQ(cache.FrameCount(), 4);
    EXPECT_NEAR(cache.FrameDt(), 0.05f, 1.0e-6f);
    EXPECT_FLOAT_EQ(cache.Duration(), 0.2f);

    ASSERT_TRUE(TickUntil(cache, [&cache] { return cache.SolvedUntil() > 0.0f; }));
    const asset::FluidFrameImage* frame = cache.FrameAt(0.0f);
    ASSERT_NE(frame, nullptr);
    EXPECT_GT(frame->size, 0);
    EXPECT_EQ(cache.TextureAt(0.0f), ImTextureID{});
    cache.Shutdown(nullptr);
}

TEST(FluidPreviewCache, LookChangeKeepsShowingTheSolvedFrames)
{
    FluidPreviewCache cache;
    cache.SetQuality(FluidPreviewQuality::Draft);
    asset::FluidRecipe recipe = TinyRecipe();
    cache.SetRecipe(recipe, 1, 0.0f);
    ASSERT_TRUE(TickUntil(cache, [&cache] { return !cache.IsSolving(); }));
    EXPECT_FLOAT_EQ(cache.SolvedUntil(), cache.Duration());

    recipe.render.opacity += 2.0f;
    cache.SetRecipe(recipe, 2, -1.0f);
    // 描き直しが済むまでは古い絵を出し続ける (空白にしない)。
    EXPECT_NE(cache.FrameAt(0.1f), nullptr);
    ASSERT_TRUE(TickUntil(cache, [&cache] { return !cache.IsSolving(); }));
    EXPECT_FLOAT_EQ(cache.SolvedUntil(), cache.Duration());
    cache.Shutdown(nullptr);
}

TEST(FluidPreviewCache, PartChangeDropsFramesFromItsStartTime)
{
    FluidPreviewCache cache;
    cache.SetQuality(FluidPreviewQuality::Draft);
    asset::FluidRecipe recipe = TinyRecipe();
    asset::FluidSource late;
    late.startTime = 0.1f;
    recipe.sources.push_back(late);
    cache.SetRecipe(recipe, 1, 0.0f);
    ASSERT_TRUE(TickUntil(cache, [&cache] { return !cache.IsSolving(); }));

    recipe.sources[1].density = 6.0f;
    // 呼び手が «描き直しだけ» と申告しても、キャッシュ自身の比較が部品の変化を拾う。
    cache.SetRecipe(recipe, 2, -1.0f);
    // 0.1 秒より前のコマ (0 と 0.05) は残り、そこで帯が止まる。
    EXPECT_NEAR(cache.SolvedUntil(), 0.1f, 1.0e-5f);
    ASSERT_TRUE(TickUntil(cache, [&cache] { return !cache.IsSolving(); }));
    EXPECT_FLOAT_EQ(cache.SolvedUntil(), cache.Duration());
    cache.Shutdown(nullptr);
}

// 部品を触ったら、変わり目の手前の途中経過から続きを解く (頭へは戻らない)。
TEST(FluidPreviewCache, PartEditResumesInsteadOfSolvingFromTheStart)
{
    FluidPreviewCache cache;
    cache.SetQuality(FluidPreviewQuality::Draft);
    asset::FluidRecipe recipe = ResumeRecipe();
    cache.SetRecipe(recipe, 1, 0.0f);
    ASSERT_EQ(cache.FrameCount(), 8);
    ASSERT_NEAR(cache.FrameDt(), 0.1f, 1.0e-6f);

    const auto scratchStart = std::chrono::steady_clock::now();
    ASSERT_TRUE(TickUntil(cache, [&cache] { return !cache.IsSolving(); }));
    const std::chrono::milliseconds fromScratch = MillisecondsSince(scratchStart);
    EXPECT_FLOAT_EQ(cache.SolvedUntil(), cache.Duration());

    recipe.sources[1].density = 6.0f;
    cache.SetRecipe(recipe, 2, -1.0f);
    // 0.5 秒より前の 5 枚 (0.0〜0.4) は残り、そこで «解けた帯» が止まる。
    EXPECT_NEAR(cache.SolvedUntil(), 0.5f, 1.0e-5f);
    EXPECT_NE(cache.FrameAt(0.4f), nullptr);

    // 全部解くのに掛かった時間の半分を締め切りにする。続きから解けば 3 コマぶん (1 割ほど) で終わるが、
    // 頭から解き直すと warmup ぶんだけで既に超える。下限は «速い機械で締め切りが潰れない» ための余裕。
    const std::chrono::milliseconds half = fromScratch / 2;
    const std::chrono::milliseconds budget = half > std::chrono::milliseconds(100)
                                                 ? half
                                                 : std::chrono::milliseconds(100);
    bool bandHeld = true;
    const bool finished = TickUntil(cache, [&] {
        // 続きから解く間も、残した帯は縮まない。
        if (cache.SolvedUntil() + 1.0e-5f < 0.5f) bandHeld = false;
        return !cache.IsSolving();
    }, budget);
    EXPECT_TRUE(bandHeld);
    ASSERT_TRUE(finished);
    EXPECT_FLOAT_EQ(cache.SolvedUntil(), cache.Duration());
    EXPECT_NE(cache.FrameAt(0.7f), nullptr);
    cache.Shutdown(nullptr);
}

// 種類が変わると «場» ごと通じないので、続きからは解けない。全コマ捨てて頭から解き直す。
// (ソルバーの ReplaceOperators が断るのと同じ条件だが、FluidInvalidationTime が先に 0 を返すため、
//  ここでは断られる前に起点そのものが無くなる。)
TEST(FluidPreviewCache, SwitchingKindSolvesEverythingAgain)
{
    FluidPreviewCache cache;
    cache.SetQuality(FluidPreviewQuality::Draft);
    asset::FluidRecipe recipe = TinyRecipe();
    cache.SetRecipe(recipe, 1, 0.0f);
    ASSERT_TRUE(TickUntil(cache, [&cache] { return !cache.IsSolving(); }));
    EXPECT_FLOAT_EQ(cache.SolvedUntil(), cache.Duration());

    recipe.kind = asset::FluidKind::Liquid;
    EXPECT_FLOAT_EQ(FluidInvalidationTime(TinyRecipe(), recipe), 0.0f);
    cache.SetRecipe(recipe, 2, -1.0f);
    EXPECT_FLOAT_EQ(cache.SolvedUntil(), 0.0f);
    EXPECT_EQ(cache.FrameAt(0.0f), nullptr);

    ASSERT_TRUE(TickUntil(cache, [&cache] { return !cache.IsSolving(); }));
    EXPECT_FLOAT_EQ(cache.SolvedUntil(), cache.Duration());
    const asset::FluidFrameImage* frame = cache.FrameAt(0.1f);
    ASSERT_NE(frame, nullptr);
    EXPECT_GT(frame->size, 0);
    cache.Shutdown(nullptr);
}

TEST(FluidPreviewCache, SameRevisionIsIgnored)
{
    FluidPreviewCache cache;
    cache.SetQuality(FluidPreviewQuality::Draft);
    asset::FluidRecipe recipe = TinyRecipe();
    cache.SetRecipe(recipe, 7, 0.0f);
    recipe.output.columns = 4;
    cache.SetRecipe(recipe, 7, 0.0f);
    EXPECT_EQ(cache.FrameCount(), 4);
}

} // namespace fbzz::tests
