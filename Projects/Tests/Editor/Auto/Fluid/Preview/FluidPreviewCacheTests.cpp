/// @file    FluidPreviewCacheTests.cpp
/// @brief   Fluid Editor のプレビューキャッシュ — 解き直しの起点 (FluidInvalidationTime) と裏の解きの契約
/// @author  Hasegawa Jin
/// @date    2026-09-12
///
/// 起点を遅く見積もると «古いコマが残って焼いた絵と違う» が黙って起き、早く見積もると毎回頭から解き直しになる。
/// どちらも画面では気付きにくいので、変更の種類ごとに起点を縛る。
/// 続きから解く経路 (ReplaceOperators) も «速くなった» としか画面に出ないので、残した帯と締め切りで縛る。
#include <TestKit/TestKit.hpp>

#include <Editor/Fluid/Preview/FluidPreviewCache.hpp>
#include <Engine/Asset/Fluid/Baking/FluidBaker.hpp>
#include <Fluid/Recipe/FluidRecipe.hpp>

#include <chrono>
#include <thread>

namespace fbzz::tests {
namespace {

using editor::FluidInvalidationTime;
using editor::FluidPreviewCache;
using editor::FluidPreviewQuality;

fluid::FluidRecipe RecipeWithSourceAt(float startTime)
{
    fluid::FluidRecipe recipe;
    recipe.kind = fluid::FluidKind::Gas;
    recipe.output.warmup = 0.0f;
    fluid::FluidSource source;
    source.name = "Puff";
    source.startTime = startTime;
    recipe.sources.push_back(source);
    return recipe;
}

/// 4 コマ・粗い格子で、裏の解きが数百ミリ秒で終わる大きさ。
fluid::FluidRecipe TinyRecipe()
{
    fluid::FluidRecipe recipe = RecipeWithSourceAt(0.0f);
    recipe.output.columns = 2;
    recipe.output.rows = 2;
    recipe.output.duration = 0.2f;
    recipe.output.substeps = 1;
    recipe.gas.pressureIterations = 10;
    return recipe;
}

/// 8 コマ (dt = 0.1)。後ろの発生源は «プレビューの 0.5 秒» から効く (部品の時刻はソルバーの時計なので
/// 0.5 + warmup)。warmup を長く取ってあるのは «頭から解き直したか» を時間で見分けるため — 頭からだと
/// warmup 20 刻み + 8 コマ、続きからだと 3 コマで済む。
fluid::FluidRecipe ResumeRecipe()
{
    fluid::FluidRecipe recipe = RecipeWithSourceAt(0.0f);
    recipe.output.columns = 4;
    recipe.output.rows = 2;
    recipe.output.duration = 0.8f;
    recipe.output.warmup = 2.0f;
    recipe.output.substeps = 1;
    recipe.gas.pressureIterations = 20;
    fluid::FluidSource late;
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
        if (done()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

std::chrono::milliseconds MillisecondsSince(std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
}

} // namespace

/// @name FluidInvalidationTime

/// @brief Checks nothing changed needs no resolve.
TEST(FluidInvalidationTime, NothingChangedNeedsNoResolve)
{
    const fluid::FluidRecipe recipe = RecipeWithSourceAt(0.5f);
    EXPECT_LT(FluidInvalidationTime(recipe, recipe), 0.0f);
}

/// @brief Checks look only change only redraws.
TEST(FluidInvalidationTime, LookOnlyChangeOnlyRedraws)
{
    const fluid::FluidRecipe before = RecipeWithSourceAt(0.5f);
    fluid::FluidRecipe after = before;
    after.render.opacity += 1.0f;
    after.render.smokeColor.x = 0.9f;
    EXPECT_LT(FluidInvalidationTime(before, after), 0.0f);
}

/// @brief Bake only change does not touch the preview.
TEST(FluidInvalidationTime, BakeOnlyChangeDoesNotTouchThePreview)
{
    const fluid::FluidRecipe before = RecipeWithSourceAt(0.5f);
    fluid::FluidRecipe after = before;
    after.bake.volumeResolution += 32;
    after.output.motionVectors = !after.output.motionVectors;
    after.output.vectorField = !after.output.vectorField;
    after.output.supersampling = 2;
    EXPECT_LT(FluidInvalidationTime(before, after), 0.0f);
}

/// @brief Checks renaming a part only redraws.
TEST(FluidInvalidationTime, RenamingAPartOnlyRedraws)
{
    const fluid::FluidRecipe before = RecipeWithSourceAt(0.5f);
    fluid::FluidRecipe after = before;
    after.sources[0].name = "Renamed";
    EXPECT_LT(FluidInvalidationTime(before, after), 0.0f);
}

/// @brief Changed source resolves from its start time.
TEST(FluidInvalidationTime, ChangedSourceResolvesFromItsStartTime)
{
    const fluid::FluidRecipe before = RecipeWithSourceAt(0.5f);
    fluid::FluidRecipe after = before;
    after.sources[0].density = 5.0f;
    EXPECT_FLOAT_EQ(FluidInvalidationTime(before, after), 0.5f);
}

/// @brief Moving the start time resolves from the earlier one.
TEST(FluidInvalidationTime, MovingTheStartTimeResolvesFromTheEarlierOne)
{
    const fluid::FluidRecipe before = RecipeWithSourceAt(0.5f);
    fluid::FluidRecipe after = before;
    after.sources[0].startTime = 0.8f;
    EXPECT_FLOAT_EQ(FluidInvalidationTime(before, after), 0.5f);
}

/// @brief Warmup is subtracted.
TEST(FluidInvalidationTime, WarmupIsSubtracted)
{
    fluid::FluidRecipe before = RecipeWithSourceAt(0.5f);
    before.output.warmup = 0.2f;
    fluid::FluidRecipe after = before;
    after.sources[0].density = 5.0f;
    EXPECT_NEAR(FluidInvalidationTime(before, after), 0.3f, 1.0e-5f);
}

/// @brief Part active during warmup resolves from the start.
TEST(FluidInvalidationTime, PartActiveDuringWarmupResolvesFromTheStart)
{
    fluid::FluidRecipe before = RecipeWithSourceAt(0.1f);
    before.output.warmup = 0.5f;
    fluid::FluidRecipe after = before;
    after.sources[0].density = 5.0f;
    EXPECT_FLOAT_EQ(FluidInvalidationTime(before, after), 0.0f);
}

/// @brief Motion key resolves no later than the key.
TEST(FluidInvalidationTime, MotionKeyResolvesNoLaterThanTheKey)
{
    fluid::FluidRecipe before = RecipeWithSourceAt(0.0f);
    before.sources[0].motion.keys = { { 0.1f, { 0.0f, 0.0f, 0.0f } }, { 0.3f, { 0.2f, 0.0f, 0.0f } } };
    fluid::FluidRecipe after = before;
    after.sources[0].motion.keys[1].offset.x = 0.4f;
    const float from = FluidInvalidationTime(before, after);
    EXPECT_GE(from, 0.0f);
    EXPECT_LE(from, 0.3f);
}

/// @brief Late motion key resolves from the previous key.
/// キーの間は直線で結ぶので、変えたキーの 1 つ前から道筋が変わる。
TEST(FluidInvalidationTime, LateMotionKeyResolvesFromThePreviousKey)
{
    fluid::FluidRecipe before = RecipeWithSourceAt(0.0f);
    before.sources[0].motion.keys = { { 0.2f, { 0.0f, 0.0f, 0.0f } },
                                      { 0.3f, { 0.1f, 0.0f, 0.0f } },
                                      { 0.6f, { 0.2f, 0.0f, 0.0f } } };
    fluid::FluidRecipe after = before;
    after.sources[0].motion.keys[2].offset.y = 0.5f;
    EXPECT_FLOAT_EQ(FluidInvalidationTime(before, after), 0.3f);
}

/// @brief Simulation change resolves everything.
TEST(FluidInvalidationTime, SimulationChangeResolvesEverything)
{
    const fluid::FluidRecipe before = RecipeWithSourceAt(0.5f);
    fluid::FluidRecipe after = before;
    after.gas.buoyancy += 1.0f;
    EXPECT_FLOAT_EQ(FluidInvalidationTime(before, after), 0.0f);
}

/// @brief Output timing change resolves everything.
TEST(FluidInvalidationTime, OutputTimingChangeResolvesEverything)
{
    const fluid::FluidRecipe before = RecipeWithSourceAt(0.5f);
    fluid::FluidRecipe after = before;
    after.output.duration = 3.0f;
    EXPECT_FLOAT_EQ(FluidInvalidationTime(before, after), 0.0f);
}

/// @brief Adding a part resolves everything.
TEST(FluidInvalidationTime, AddingAPartResolvesEverything)
{
    const fluid::FluidRecipe before = RecipeWithSourceAt(0.5f);
    fluid::FluidRecipe after = before;
    after.forces.push_back(fluid::FluidForce{});
    EXPECT_FLOAT_EQ(FluidInvalidationTime(before, after), 0.0f);
}

/// @brief Toggling a part resolves everything.
/// 有効な部品の中での添字でノイズが決まるため、1 つ消すと他の部品の出方が最初から変わる。
TEST(FluidInvalidationTime, TogglingAPartResolvesEverything)
{
    const fluid::FluidRecipe before = RecipeWithSourceAt(0.5f);
    fluid::FluidRecipe after = before;
    after.sources[0].enabled = false;
    EXPECT_FLOAT_EQ(FluidInvalidationTime(before, after), 0.0f);
}

/// @brief Checks editing a disabled part needs no resolve.
TEST(FluidInvalidationTime, EditingADisabledPartNeedsNoResolve)
{
    fluid::FluidRecipe before = RecipeWithSourceAt(0.5f);
    before.sources[0].enabled = false;
    fluid::FluidRecipe after = before;
    after.sources[0].density = 5.0f;
    EXPECT_LT(FluidInvalidationTime(before, after), 0.0f);
}

/// @name FluidPreviewCache

/// @brief Solves in the background without a renderer.
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

/// @brief Look change keeps showing the solved frames.
TEST(FluidPreviewCache, LookChangeKeepsShowingTheSolvedFrames)
{
    FluidPreviewCache cache;
    cache.SetQuality(FluidPreviewQuality::Draft);
    fluid::FluidRecipe recipe = TinyRecipe();
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

/// @brief Part change drops frames from its start time.
TEST(FluidPreviewCache, PartChangeDropsFramesFromItsStartTime)
{
    FluidPreviewCache cache;
    cache.SetQuality(FluidPreviewQuality::Draft);
    fluid::FluidRecipe recipe = TinyRecipe();
    fluid::FluidSource late;
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

/// @brief Part edit resumes instead of solving from the start.
/// 部品を触ったら、変わり目の手前の途中経過から続きを解く (頭へは戻らない)。
TEST(FluidPreviewCache, PartEditResumesInsteadOfSolvingFromTheStart)
{
    FluidPreviewCache cache;
    cache.SetQuality(FluidPreviewQuality::Draft);
    fluid::FluidRecipe recipe = ResumeRecipe();
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
    //       頭から解き直すと warmup ぶんだけで既に超える。下限は «速い機械で締め切りが潰れない» ための余裕。
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

/// @brief Checks switching kind solves everything again.
/// 種類が変わると «場» ごと通じないので、続きからは解けない。全コマ捨てて頭から解き直す。
/// (ソルバーの ReplaceOperators が断るのと同じ条件だが、FluidInvalidationTime が先に 0 を返すため、
///  ここでは断られる前に起点そのものが無くなる。)
TEST(FluidPreviewCache, SwitchingKindSolvesEverythingAgain)
{
    FluidPreviewCache cache;
    cache.SetQuality(FluidPreviewQuality::Draft);
    fluid::FluidRecipe recipe = TinyRecipe();
    cache.SetRecipe(recipe, 1, 0.0f);
    ASSERT_TRUE(TickUntil(cache, [&cache] { return !cache.IsSolving(); }));
    EXPECT_FLOAT_EQ(cache.SolvedUntil(), cache.Duration());

    recipe.kind = fluid::FluidKind::Liquid;
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

/// @brief Same revision is ignored.
TEST(FluidPreviewCache, SameRevisionIsIgnored)
{
    FluidPreviewCache cache;
    cache.SetQuality(FluidPreviewQuality::Draft);
    fluid::FluidRecipe recipe = TinyRecipe();
    cache.SetRecipe(recipe, 7, 0.0f);
    recipe.output.columns = 4;
    cache.SetRecipe(recipe, 7, 0.0f);
    EXPECT_EQ(cache.FrameCount(), 4);
}

} // namespace fbzz::tests
