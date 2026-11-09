/// @file    SurfacePatchLodTests.cpp
/// @brief   連続面の LOD 予算・有限被覆・境界縫合・共有頂点の高さ帯域を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-07
#include <TestKit/TestKit.hpp>
#include <Graphics/Renderer/SurfacePatchLod.hpp>
#include <Math/Frustum.hpp>
#include <Math/Matrix4.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cfenv>
#include <limits>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace fbzz::tests {
namespace {

class SurfacePatchLodTest : public testkit::Fixture {};

std::array<uint32_t, 3> CanonicalOrientedTriangle(std::array<uint32_t, 3> triangle)
{
    std::rotate(triangle.begin(), std::min_element(triangle.begin(), triangle.end()), triangle.end());
    return triangle;
}

void ExpectSameOrientedFaceOrder(const std::vector<uint32_t>& list, const std::vector<uint32_t>& strip)
{
    std::vector<std::array<uint32_t, 3>> expected, actual;
    for (size_t index = 0; index < list.size(); index += 3u) {
        expected.push_back(CanonicalOrientedTriangle({list[index], list[index + 1u], list[index + 2u]}));
    }
    uint32_t runLength = 0;
    for (size_t index = 0; index < strip.size(); ++index) {
        if (strip[index] == 0xFFFFFFFFu) {
            EXPECT_GE(runLength, 3u);
            runLength = 0;
            continue;
        }
        if (++runLength < 3u) continue;
        std::array<uint32_t, 3> triangle{strip[index - 2u], strip[index - 1u], strip[index]};
        if (runLength % 2u == 0) std::swap(triangle[0], triangle[1]);
        actual.push_back(CanonicalOrientedTriangle(triangle));
    }
    EXPECT_GE(runLength, 3u);
    EXPECT_EQ(actual, expected);
}

TEST_F(SurfacePatchLodTest, AdaptiveStripPreservesOrientedFaceOrderForEveryStitchMask)
{
    for (const uint32_t cells : {2u, 4u, 16u, 64u}) {
        for (uint8_t mask = 0; mask < 16u; ++mask) {
            SCOPED_TRACE(::testing::Message() << cells << " cells, mask " << uint32_t{mask});
            std::vector<uint32_t> list, strip;
            ASSERT_TRUE(renderer::BuildSurfacePatchIndices(cells, mask, list));
            ASSERT_TRUE(renderer::BuildSurfacePatchStripIndices(cells, mask, strip));
            ExpectSameOrientedFaceOrder(list, strip);
            EXPECT_LT(strip.size(), list.size());
        }
    }
}

TEST_F(SurfacePatchLodTest, RegularStripPreservesNonSquareRowsAndDiagonals)
{
    constexpr uint32_t CELLS_X = 16, CELLS_Z = 5, COLUMNS = CELLS_X + 1u;
    std::vector<uint32_t> list, strip;
    for (uint32_t z = 0; z < CELLS_Z; ++z) {
        for (uint32_t x = 0; x < CELLS_X; ++x) {
            const uint32_t base = z * COLUMNS + x;
            list.insert(list.end(), {base, base + COLUMNS, base + 1u,
                base + 1u, base + COLUMNS, base + COLUMNS + 1u});
        }
    }
    ASSERT_TRUE(renderer::BuildRegularSurfaceStripIndices(CELLS_X, CELLS_Z, strip));
    EXPECT_EQ(strip.size(), 2u * COLUMNS * CELLS_Z + CELLS_Z - 1u);
    ExpectSameOrientedFaceOrder(list, strip);
    const auto previous = strip;
    EXPECT_FALSE(renderer::BuildRegularSurfaceStripIndices(0, CELLS_Z, strip));
    EXPECT_FALSE(renderer::BuildSurfacePatchStripIndices(3, 0, strip));
    EXPECT_FALSE(renderer::BuildSurfacePatchStripIndices(16, 16, strip));
    EXPECT_EQ(strip, previous);
}

bool Overlap(float aMin, float aMax, float bMin, float bMax)
{
    return (std::min)(aMax, bMax) > (std::max)(aMin, bMin);
}

bool ShareEdge(const renderer::SurfacePatch& a, const renderer::SurfacePatch& b)
{
    return ((a.uvMax.x == b.uvMin.x || a.uvMin.x == b.uvMax.x)
            && Overlap(a.uvMin.y, a.uvMax.y, b.uvMin.y, b.uvMax.y))
        || ((a.uvMax.y == b.uvMin.y || a.uvMin.y == b.uvMax.y)
            && Overlap(a.uvMin.x, a.uvMax.x, b.uvMin.x, b.uvMax.x));
}

void ExpectFullBalancedCoverage(const renderer::SurfacePatchSelection& selection)
{
    double area = 0.0;
    for (const auto& patch : selection.patches) {
        EXPECT_GE(patch.uvMin.x, 0.0f);
        EXPECT_GE(patch.uvMin.y, 0.0f);
        EXPECT_LE(patch.uvMax.x, 1.0f);
        EXPECT_LE(patch.uvMax.y, 1.0f);
        area += static_cast<double>(patch.uvMax.x - patch.uvMin.x)
              * static_cast<double>(patch.uvMax.y - patch.uvMin.y);
    }
    EXPECT_DOUBLE_EQ(area, 1.0);
    for (size_t i = 0; i < selection.patches.size(); ++i) {
        for (size_t j = i + 1; j < selection.patches.size(); ++j) {
            const auto& a = selection.patches[i];
            const auto& b = selection.patches[j];
            EXPECT_FALSE(Overlap(a.uvMin.x, a.uvMax.x, b.uvMin.x, b.uvMax.x)
                      && Overlap(a.uvMin.y, a.uvMax.y, b.uvMin.y, b.uvMax.y));
            if (ShareEdge(a, b))
                EXPECT_LE(std::abs(static_cast<int>(a.depth) - static_cast<int>(b.depth)), 1);
        }
    }
}

renderer::SurfacePatchSurface WideSurface()
{
    renderer::SurfacePatchSurface surface;
    surface.localMin = { -1024.0f, -512.0f };
    surface.localMax = { 1024.0f, 512.0f };
    surface.minHeight = -3.0f;
    surface.maxHeight = 3.0f;
    surface.horizontalDisplacement = 2.0f;
    return surface;
}

renderer::SurfacePatchView LocalView()
{
    renderer::SurfacePatchView view;
    view.cameraPosition = { -300.0f, 20.0f, -100.0f };
    view.cameraForward = { 0.0f, 0.0f, 1.0f };
    view.orthographic = true;
    view.projectionScalePixels = 0.001f;
    return view;
}

TEST_F(SurfacePatchLodTest, InvalidInputsPreserveThePreviousSelection)
{
    auto surface = WideSurface();
    renderer::SurfacePatchLodSettings settings;
    auto view = LocalView();
    renderer::SurfacePatchSelection previous;
    previous.qualityLimited = true;
    previous.patches.emplace_back().depth = 99;
    auto checkUnchanged = [&] {
        ASSERT_EQ(previous.patches.size(), 1u);
        EXPECT_EQ(previous.patches.front().depth, 99u);
        EXPECT_TRUE(previous.qualityLimited);
    };
    settings.cellsPerPatch = 3;
    EXPECT_FALSE(renderer::SelectSurfacePatches(surface, settings, view, previous));
    checkUnchanged();
    settings.cellsPerPatch = 16;
    settings.maxDepth = 11;
    EXPECT_FALSE(renderer::SelectSurfacePatches(surface, settings, view, previous));
    checkUnchanged();
    settings.maxDepth = 8;
    settings.maxPatches = 16385;
    EXPECT_FALSE(renderer::SelectSurfacePatches(surface, settings, view, previous));
    checkUnchanged();
    settings.maxPatches = 4096;
    surface.localMax.x = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(renderer::SelectSurfacePatches(surface, settings, view, previous));
    checkUnchanged();
    surface = WideSurface();
    view.cameraForward = math::Vector3::ZERO;
    EXPECT_FALSE(renderer::SelectSurfacePatches(surface, settings, view, previous));
    checkUnchanged();
    view = LocalView();
    surface.worldMatrix.m[3][0] = 1.0f;
    EXPECT_FALSE(renderer::SelectSurfacePatches(surface, settings, view, previous));
    checkUnchanged();
}

TEST_F(SurfacePatchLodTest, FiniteInputsThatOverflowDerivedGeometryPreserveThePreviousSelection)
{
    renderer::SurfacePatchLodSettings settings;
    const auto view = LocalView();
    renderer::SurfacePatchSelection previous;
    previous.qualityLimited = true;
    previous.patches.emplace_back().depth = 99;
    auto checkUnchanged = [&] {
        ASSERT_EQ(previous.patches.size(), 1u);
        EXPECT_EQ(previous.patches.front().depth, 99u);
        EXPECT_TRUE(previous.qualityLimited);
    };
    auto surface = WideSurface();
    surface.localMin.x = -std::numeric_limits<float>::max();
    surface.localMax.x = std::numeric_limits<float>::max();
    EXPECT_FALSE(renderer::SelectSurfacePatches(surface, settings, view, previous));
    checkUnchanged();
    surface = WideSurface();
    surface.localMin.x = -1.0e30f;
    surface.localMax.x = 1.0e30f;
    surface.worldMatrix.m[0][0] = 1.0e19f;
    EXPECT_FALSE(renderer::SelectSurfacePatches(surface, settings, view, previous));
    checkUnchanged();
    surface = WideSurface();
    surface.localMin.x = 1.0e38f;
    surface.localMax.x = 2.0e38f;
    surface.horizontalDisplacement = 2.0e38f;
    EXPECT_FALSE(renderer::SelectSurfacePatches(surface, settings, view, previous));
    checkUnchanged();
    surface = WideSurface();
    surface.localMin.x = 3.0e19f;
    surface.localMax.x = 3.5e19f;
    surface.worldMatrix.m[0][0] = 1.0e19f;
    EXPECT_FALSE(renderer::SelectSurfacePatches(surface, settings, view, previous));
    checkUnchanged();
    surface = WideSurface();
    surface.localMin.x = 2.5e19f;
    surface.localMax.x = 3.5e19f;
    surface.minHeight = 3.0e19f;
    surface.maxHeight = 3.0e19f;
    surface.horizontalDisplacement = 0.0f;
    surface.worldMatrix.m[0][0] = 1.0e19f;
    surface.worldMatrix.m[0][1] = -1.0e19f;
    EXPECT_FALSE(renderer::SelectSurfacePatches(surface, settings, view, previous));
    checkUnchanged();
}

TEST_F(SurfacePatchLodTest, ExtremelyLargeProjectionRequestsKeepFiniteGeometryAndReportTheQualityLimit)
{
    const auto surface = WideSurface();
    renderer::SurfacePatchLodSettings settings;
    settings.maxDepth = 1;
    settings.maxPatches = 4;
    settings.nearDistance = 0.0f;
    settings.targetCellPixels = std::numeric_limits<float>::min();
    auto view = LocalView();
    view.orthographic = false;
    view.projectionScalePixels = std::numeric_limits<float>::max();
    view.minProjectionDistance = std::numeric_limits<float>::min();
    renderer::SurfacePatchSelection selection;
    ASSERT_TRUE(renderer::SelectSurfacePatches(surface, settings, view, selection));
    ASSERT_EQ(selection.patches.size(), 4u);
    EXPECT_TRUE(selection.qualityLimited);
    for (const auto& patch : selection.patches) {
        EXPECT_TRUE(std::isfinite(patch.cellSize.x));
        EXPECT_TRUE(std::isfinite(patch.cellSize.y));
        EXPECT_TRUE(std::isfinite(patch.aabbMin.x));
        EXPECT_TRUE(std::isfinite(patch.aabbMax.x));
    }
    ExpectFullBalancedCoverage(selection);
}

TEST_F(SurfacePatchLodTest, AllStitchMasksKeepPositiveAreaAndAManifoldRectangularBoundary)
{
    for (uint32_t cells : { 2u, 4u, 16u }) {
        for (uint8_t mask = 0; mask < 16; ++mask) {
            std::vector<uint32_t> indices;
            ASSERT_TRUE(renderer::BuildSurfacePatchIndices(cells, mask, indices));
            ASSERT_EQ(indices.size() % 3u, 0u);
            const uint32_t columns = cells + 1u;
            int area = 0;
            std::map<std::pair<uint32_t, uint32_t>, uint32_t> edgeUses;
            for (size_t i = 0; i < indices.size(); i += 3u) {
                const uint32_t a = indices[i], b = indices[i + 1u], c = indices[i + 2u];
                ASSERT_LT(a, columns * columns);
                ASSERT_LT(b, columns * columns);
                ASSERT_LT(c, columns * columns);
                const int ax = static_cast<int>(a % columns), az = static_cast<int>(a / columns);
                const int bx = static_cast<int>(b % columns), bz = static_cast<int>(b / columns);
                const int cx = static_cast<int>(c % columns), cz = static_cast<int>(c / columns);
                const int signedArea = (bx - ax) * (cz - az) - (bz - az) * (cx - ax);
                EXPECT_LT(signedArea, 0);
                area -= signedArea;
                const std::array<uint32_t, 3> corners = { a, b, c };
                for (uint32_t edge = 0; edge < 3; ++edge) {
                    const auto pair = std::minmax(corners[edge], corners[(edge + 1u) % 3u]);
                    ++edgeUses[{ pair.first, pair.second }];
                }
            }
            EXPECT_EQ(area, static_cast<int>(2u * cells * cells));
            for (const auto& [edge, count] : edgeUses) {
                const uint32_t ax = edge.first % columns, az = edge.first / columns;
                const uint32_t bx = edge.second % columns, bz = edge.second / columns;
                const bool boundary = (ax == 0 && bx == 0) || (ax == cells && bx == cells)
                    || (az == 0 && bz == 0) || (az == cells && bz == cells);
                EXPECT_EQ(count, boundary ? 1u : 2u);
            }
            for (uint32_t index : indices) {
                const uint32_t x = index % columns, z = index / columns;
                if ((x == 0 && (mask & 1u)) || (x == cells && (mask & 2u))) EXPECT_EQ(z % 2u, 0u);
                if ((z == 0 && (mask & 4u)) || (z == cells && (mask & 8u))) EXPECT_EQ(x % 2u, 0u);
            }
        }
    }
}

TEST_F(SurfacePatchLodTest, InvalidIndexRequestsPreserveThePreviousBuffer)
{
    std::vector<uint32_t> indices = { 17, 23, 42 };
    EXPECT_FALSE(renderer::BuildSurfacePatchIndices(1, 0, indices));
    EXPECT_FALSE(renderer::BuildSurfacePatchIndices(3, 0, indices));
    EXPECT_FALSE(renderer::BuildSurfacePatchIndices(512, 0, indices));
    EXPECT_FALSE(renderer::BuildSurfacePatchIndices(16, 16, indices));
    EXPECT_EQ(indices, (std::vector<uint32_t>{ 17, 23, 42 }));
}

TEST_F(SurfacePatchLodTest, ThePatchBudgetKeepsCompleteCoverageAndBalancedEdges)
{
    const auto surface = WideSurface();
    renderer::SurfacePatchLodSettings settings;
    settings.cellsPerPatch = 4;
    settings.maxDepth = 10;
    settings.nearDistance = 224.0f;
    settings.nearCellSize = 0.5f;
    const auto view = LocalView();
    for (uint32_t budget : { 1u, 4u, 7u, 31u, 127u }) {
        settings.maxPatches = budget;
        renderer::SurfacePatchSelection selection;
        ASSERT_TRUE(renderer::SelectSurfacePatches(surface, settings, view, selection));
        EXPECT_LE(selection.patches.size(), budget);
        EXPECT_TRUE(selection.qualityLimited);
        ExpectFullBalancedCoverage(selection);
    }
}

TEST_F(SurfacePatchLodTest, NearViewsUseMorePatchesAndDoNotChangeAnotherViewsSelection)
{
    const auto surface = WideSurface();
    renderer::SurfacePatchLodSettings settings;
    settings.cellsPerPatch = 4;
    settings.maxDepth = 7;
    settings.maxPatches = 1024;
    settings.nearDistance = 100.0f;
    settings.nearCellSize = 8.0f;
    auto nearView = LocalView();
    auto farView = nearView;
    farView.cameraPosition = { 0.0f, 5000.0f, 0.0f };
    renderer::SurfacePatchSelection near, far, repeated;
    ASSERT_TRUE(renderer::SelectSurfacePatches(surface, settings, nearView, near));
    ASSERT_TRUE(renderer::SelectSurfacePatches(surface, settings, farView, far));
    ASSERT_TRUE(renderer::SelectSurfacePatches(surface, settings, nearView, repeated));
    EXPECT_GT(near.patches.size(), far.patches.size());
    ASSERT_EQ(near.patches.size(), repeated.patches.size());
    for (size_t i = 0; i < near.patches.size(); ++i) {
        EXPECT_EQ(near.patches[i].depth, repeated.patches[i].depth);
        EXPECT_EQ(near.patches[i].x, repeated.patches[i].x);
        EXPECT_EQ(near.patches[i].z, repeated.patches[i].z);
        EXPECT_EQ(near.patches[i].coarseEdgeMask, repeated.patches[i].coarseEdgeMask);
    }
}

TEST_F(SurfacePatchLodTest, SelectionCacheReusesOnlyMatchingSurfaceSettingsAndViewValues)
{
    renderer::SurfacePatchSurface surface;
    surface.localMin = {-0.5f, -0.5f};
    surface.localMax = {0.5f, 0.5f};
    renderer::SurfacePatchLodSettings settings;
    settings.maxDepth = 0;
    settings.maxPatches = 1;
    settings.nearDistance = 0.0f;
    auto view = LocalView();
    view.viewportHeight = 720u;
    auto frustum = math::Frustum::FromViewProjection(math::Matrix4::Identity());
    view.frustum = &frustum;

    renderer::SurfacePatchSelection selected;
    ASSERT_TRUE(renderer::SelectSurfacePatches(surface, settings, view, selected));
    renderer::SurfacePatchSelectionCache cache;
    cache.Store(surface, settings, view, selected);
    const auto* hit = cache.Find(surface, settings, view);
    ASSERT_NE(hit, nullptr);
    ASSERT_EQ(hit->patches.size(), selected.patches.size());
    ASSERT_FALSE(hit->patches.empty());
    EXPECT_EQ(hit->patches.front().coarseEdgeMask, selected.patches.front().coarseEdgeMask);

    auto alternateView = view;
    alternateView.cameraPosition.x += 1.0f;
    renderer::SurfacePatchSelection alternateSelection;
    ASSERT_TRUE(renderer::SelectSurfacePatches(surface, settings, alternateView, alternateSelection));
    cache.Store(surface, settings, alternateView, alternateSelection);
    EXPECT_NE(cache.Find(surface, settings, view), nullptr);
    EXPECT_NE(cache.Find(surface, settings, alternateView), nullptr);

    auto changedSurface = surface;
    changedSurface.horizontalDisplacement = 0.25f;
    EXPECT_EQ(cache.Find(changedSurface, settings, view), nullptr);
    changedSurface = surface;
    changedSurface.minHeight = -0.25f;
    EXPECT_EQ(cache.Find(changedSurface, settings, view), nullptr);

    auto changedSettings = settings;
    changedSettings.morphEnabled = true;
    EXPECT_EQ(cache.Find(surface, changedSettings, view), nullptr);
    changedSettings = settings;
    changedSettings.morphTransitionWidth += 0.1f;
    EXPECT_EQ(cache.Find(surface, changedSettings, view), nullptr);

    auto changedView = view;
    changedView.cameraPosition.x += 2.0f;
    EXPECT_EQ(cache.Find(surface, settings, changedView), nullptr);
    changedView = view;
    changedView.projectionScalePixels += 1.0f;
    EXPECT_EQ(cache.Find(surface, settings, changedView), nullptr);
    changedView = view;
    changedView.viewportHeight += 1u;
    EXPECT_EQ(cache.Find(surface, settings, changedView), nullptr);
    changedView = view;
    changedView.includeCulledPatches = true;
    EXPECT_EQ(cache.Find(surface, settings, changedView), nullptr);

    auto changedClip = math::Matrix4::Identity();
    changedClip.m[0][3] = 0.25f;
    const auto changedFrustum = math::Frustum::FromViewProjection(changedClip);
    changedView = view;
    changedView.frustum = &changedFrustum;
    EXPECT_EQ(cache.Find(surface, settings, changedView), nullptr);
    frustum = changedFrustum;
    EXPECT_EQ(cache.Find(surface, settings, view), nullptr);
    cache.Clear();
    EXPECT_EQ(cache.Find(surface, settings, view), nullptr);
}

std::shared_ptr<const renderer::SurfacePatchSelection> StoreSelected(
    renderer::SurfacePatchSelectionCache& cache, const renderer::SurfacePatchSurface& surface,
    const renderer::SurfacePatchLodSettings& settings, const renderer::SurfacePatchView& view)
{
    renderer::SurfacePatchSelection selected;
    if (!renderer::SelectSurfacePatches(surface, settings, view, selected)) {
        ADD_FAILURE() << "Expected valid production selection";
        return {};
    }
    return cache.Store(surface, settings, view, std::move(selected));
}

TEST_F(SurfacePatchLodTest, OwnedSelectionsSurviveReplacementEvictionAndClearWithoutRetainingSources)
{
    const auto surface = WideSurface();
    renderer::SurfacePatchLodSettings settings;
    settings.maxDepth = 0; settings.maxPatches = 1;
    auto view = LocalView();
    renderer::SurfacePatchSelectionCache cache;
    auto owned = StoreSelected(cache, surface, settings, view);
    ASSERT_NE(owned, nullptr);
    ASSERT_EQ(owned->patches.size(), 1u);
    const auto firstPatch = owned->patches.front();
    const std::weak_ptr<const renderer::SurfacePatchSelection> retired = owned;
    EXPECT_EQ(cache.FindOwned(surface, settings, view), owned);

    for (uint32_t index = 1; index <= 8; ++index) {
        auto alternate = view; alternate.cameraPosition.x += static_cast<float>(index);
        ASSERT_NE(StoreSelected(cache, surface, settings, alternate), nullptr);
    }

    EXPECT_EQ(cache.FindOwned(surface, settings, view), nullptr);
    EXPECT_FALSE(retired.expired());
    EXPECT_EQ(owned->patches.front().depth, firstPatch.depth);
    EXPECT_FLOAT_EQ(owned->patches.front().uvMax.x, firstPatch.uvMax.x);
    cache.Clear();
    EXPECT_FALSE(retired.expired());
    owned.reset();
    EXPECT_TRUE(retired.expired());
}

TEST_F(SurfacePatchLodTest, ReplacementAndOversizedCapacityRespectTheRetainedPatchBudget)
{
    const auto surface = WideSurface();
    renderer::SurfacePatchLodSettings settings;
    settings.maxDepth = 0; settings.maxPatches = 1;
    const auto view = LocalView();
    auto alternate = view; alternate.cameraPosition.x += 1.0f;
    renderer::SurfacePatchSelectionCache cache;
    ASSERT_NE(StoreSelected(cache, surface, settings, view), nullptr);
    ASSERT_NE(StoreSelected(cache, surface, settings, alternate), nullptr);
    renderer::SurfacePatchSelection selected;
    ASSERT_TRUE(renderer::SelectSurfacePatches(surface, settings, view, selected));
    selected.patches.reserve(16384);

    const auto retained = cache.Store(surface, settings, view, std::move(selected));

    ASSERT_NE(retained, nullptr);
    EXPECT_EQ(cache.FindOwned(surface, settings, view), retained);
    EXPECT_EQ(cache.FindOwned(surface, settings, alternate), nullptr);
    ASSERT_TRUE(renderer::SelectSurfacePatches(surface, settings, alternate, selected));
    selected.patches.reserve(16385);
    const auto local = cache.Store(surface, settings, alternate, std::move(selected));
    ASSERT_NE(local, nullptr);
    EXPECT_EQ(local->patches.size(), 1u);
    EXPECT_EQ(cache.FindOwned(surface, settings, alternate), nullptr);
    EXPECT_EQ(cache.FindOwned(surface, settings, view), retained);
}

TEST_F(SurfacePatchLodTest, InvalidPersistentInputsClearTheCacheAndPreservePinnedOutput)
{
    auto surface = WideSurface();
    renderer::SurfacePatchLodSettings settings;
    settings.maxDepth = 0; settings.maxPatches = 1;
    const auto view = LocalView();
    renderer::SurfacePatchSelectionCache cache;
    const auto pinned = StoreSelected(cache, surface, settings, view);
    ASSERT_NE(pinned, nullptr);
    auto invalid = surface;
    invalid.localMin.x = std::numeric_limits<float>::quiet_NaN();

    EXPECT_EQ(cache.FindOwned(invalid, settings, view), nullptr);
    EXPECT_EQ(cache.FindOwned(surface, settings, view), nullptr);
    EXPECT_EQ(pinned->patches.size(), 1u);
    renderer::SurfacePatchSelection unchanged;
    unchanged.qualityLimited = true;
    EXPECT_FALSE(renderer::SelectSurfacePatches(invalid, settings, view, unchanged));
    EXPECT_TRUE(unchanged.qualityLimited);
}

TEST_F(SurfacePatchLodTest, PersistentKeysDistinguishRoundingAndDenormalControlsButIgnoreStickyFlags)
{
    struct RestoreFp {
        int rounding = std::fegetround();
        unsigned int csr = _mm_getcsr();
        ~RestoreFp() { (void)std::fesetround(rounding); _mm_setcsr(csr); }
    } restore;
    ASSERT_EQ(std::fesetround(FE_TONEAREST), 0);
    const auto surface = WideSurface();
    renderer::SurfacePatchLodSettings settings;
    settings.maxDepth = 0; settings.maxPatches = 1;
    const auto view = LocalView();
    renderer::SurfacePatchSelectionCache cache;
    const auto pinned = StoreSelected(cache, surface, settings, view);
    ASSERT_NE(pinned, nullptr);
    const unsigned int original = _mm_getcsr();

    _mm_setcsr(original ^ 0x20u);
    EXPECT_EQ(cache.FindOwned(surface, settings, view), pinned);
    _mm_setcsr(original ^ 0x8000u);
    EXPECT_EQ(cache.FindOwned(surface, settings, view), nullptr);
    _mm_setcsr(original ^ 0x40u);
    EXPECT_EQ(cache.FindOwned(surface, settings, view), nullptr);
    _mm_setcsr(original);
    ASSERT_EQ(std::fesetround(FE_DOWNWARD), 0);
    EXPECT_EQ(cache.FindOwned(surface, settings, view), nullptr);
    ASSERT_EQ(std::fesetround(FE_TONEAREST), 0);
    EXPECT_EQ(cache.FindOwned(surface, settings, view), pinned);
}

TEST_F(SurfacePatchLodTest, ViewOwnerRetainsSceneAndGameKeysAcrossInvocationsAndRevalidatesEpochs)
{
    const auto surface = WideSurface();
    renderer::SurfacePatchLodSettings settings;
    settings.maxDepth = 0; settings.maxPatches = 1;
    const auto view = LocalView();
    auto gameView = view; gameView.cameraPosition.x += 100.0f;
    renderer::SurfacePatchSelectionCacheOwner owner;
    renderer::SurfacePatchSelectionCache execution;
    renderer::SurfacePatchSelectionCacheOwner::ViewKey sceneKey{1, 1, 10, 1, 20, 1};
    auto gameKey = sceneKey; gameKey.view = 2; gameKey.outputId = 21; gameKey.sceneGeneration = 11;
    std::shared_ptr<const renderer::SurfacePatchSelection> scenePlan, gamePlan;
    {
        const auto lease = owner.Borrow(sceneKey, execution);
        scenePlan = StoreSelected(execution, surface, settings, view);
    }
    EXPECT_EQ(execution.FindOwned(surface, settings, view), nullptr);
    {
        const auto lease = owner.Borrow(gameKey, execution);
        gamePlan = StoreSelected(execution, surface, settings, gameView);
    }
    for (uint32_t repeat = 0; repeat < 2; ++repeat) {
        {
            const auto lease = owner.Borrow(sceneKey, execution);
            EXPECT_EQ(execution.FindOwned(surface, settings, view), scenePlan);
        }
        {
            const auto lease = owner.Borrow(gameKey, execution);
            EXPECT_EQ(execution.FindOwned(surface, settings, gameView), gamePlan);
        }
    }
    ++sceneKey.sceneGeneration;
    {
        const auto lease = owner.Borrow(sceneKey, execution);
        EXPECT_EQ(execution.FindOwned(surface, settings, view), nullptr);
        EXPECT_NE(StoreSelected(execution, surface, settings, view), nullptr);
    }
    {
        const auto lease = owner.Borrow(gameKey, execution);
        EXPECT_EQ(execution.FindOwned(surface, settings, gameView), gamePlan);
    }
    ++sceneKey.resourceEpoch;
    {
        const auto lease = owner.Borrow(sceneKey, execution);
        EXPECT_EQ(execution.FindOwned(surface, settings, view), nullptr);
    }
    ++gameKey.resourceEpoch;
    {
        const auto lease = owner.Borrow(gameKey, execution);
        EXPECT_EQ(execution.FindOwned(surface, settings, gameView), nullptr);
    }
}

TEST_F(SurfacePatchLodTest, ActiveViewLeasesSurviveNestedCallsClearAndOwnerEviction)
{
    const auto surface = WideSurface();
    renderer::SurfacePatchLodSettings settings;
    settings.maxDepth = 0; settings.maxPatches = 1;
    const auto view = LocalView();
    renderer::SurfacePatchSelectionCacheOwner owner;
    renderer::SurfacePatchSelectionCache execution, nested;
    renderer::SurfacePatchSelectionCacheOwner::ViewKey key{1, 1, 10, 1, 20, 1};
    std::shared_ptr<const renderer::SurfacePatchSelection> pinned;
    {
        const auto lease = owner.Borrow(key, execution);
        pinned = StoreSelected(execution, surface, settings, view);
        ASSERT_NE(pinned, nullptr);
        {
            const auto nestedLease = owner.Borrow(key, nested);
            EXPECT_EQ(nested.FindOwned(surface, settings, view), nullptr);
            auto other = view; other.cameraPosition.x += 1.0f;
            ASSERT_NE(StoreSelected(nested, surface, settings, other), nullptr);
        }
        EXPECT_EQ(execution.FindOwned(surface, settings, view), pinned);
        owner.Clear();
        EXPECT_EQ(execution.FindOwned(surface, settings, view), pinned);
        {
            const auto sameContext = owner.Borrow(key, execution);
            execution.Clear();
            EXPECT_EQ(pinned->patches.size(), 1u);
        }
    }
    {
        const auto lease = owner.Borrow(key, execution);
        EXPECT_EQ(execution.FindOwned(surface, settings, view), nullptr);
    }
    const std::weak_ptr<const renderer::SurfacePatchSelection> retired = pinned;
    pinned.reset();
    EXPECT_TRUE(retired.expired());
    auto alternate = key; ++alternate.view; ++alternate.outputId;
    {
        const auto lease = owner.Borrow(key, execution);
        ASSERT_NE(StoreSelected(execution, surface, settings, view), nullptr);
    }
    {
        const auto lease = owner.Borrow(alternate, execution);
        ASSERT_NE(StoreSelected(execution, surface, settings, view), nullptr);
    }
    auto third = key; third.view += 2; third.outputId += 2;
    {
        const auto lease = owner.Borrow(third, execution);
        EXPECT_EQ(execution.FindOwned(surface, settings, view), nullptr);
    }
    {
        const auto lease = owner.Borrow(key, execution);
        EXPECT_EQ(execution.FindOwned(surface, settings, view), nullptr);
    }
}

TEST_F(SurfacePatchLodTest, ProjectionQualityAndWorldScaleControlTheCellFootprint)
{
    auto surface = WideSurface();
    renderer::SurfacePatchLodSettings settings;
    settings.cellsPerPatch = 16;
    settings.nearDistance = 0.0f;
    settings.targetCellPixels = 8.0f;
    auto view = LocalView();
    view.projectionScalePixels = 0.0625f;
    renderer::SurfacePatchSelection low, high, scaled;
    ASSERT_TRUE(renderer::SelectSurfacePatches(surface, settings, view, low));
    ASSERT_EQ(low.patches.size(), 1u);
    view.projectionScalePixels = 0.5f;
    ASSERT_TRUE(renderer::SelectSurfacePatches(surface, settings, view, high));
    EXPECT_GT(high.patches.size(), low.patches.size());
    view.projectionScalePixels = 0.0625f;
    surface.worldMatrix.m[0][0] = 4.0f;
    ASSERT_TRUE(renderer::SelectSurfacePatches(surface, settings, view, scaled));
    EXPECT_GT(scaled.patches.size(), low.patches.size());
    for (const auto& patch : scaled.patches)
        EXPECT_LE((std::max)(patch.cellSize.x, patch.cellSize.y) * view.projectionScalePixels, settings.targetCellPixels);
}

TEST_F(SurfacePatchLodTest, MorphPaddingDoesNotIncreaseRefinementBeyondTheBaseTile)
{
    renderer::SurfacePatchSurface surface;
    surface.localMin = { 0.0f, 0.0f };
    surface.localMax = { 8.0f, 8.0f };
    renderer::SurfacePatchLodSettings settings;
    settings.cellsPerPatch = 8;
    settings.maxDepth = 4;
    settings.maxPatches = 256;
    settings.nearDistance = 1.5f;
    settings.nearCellSize = 0.25f;
    settings.targetCellPixels = 1000000.0f;
    auto view = LocalView();
    view.cameraPosition = { 0.5f, 0.0f, 0.5f };
    renderer::SurfacePatchSelection baseTile, morphEnabled;
    ASSERT_TRUE(renderer::SelectSurfacePatches(surface, settings, view, baseTile));
    settings.morphEnabled = true;
    ASSERT_TRUE(renderer::SelectSurfacePatches(surface, settings, view, morphEnabled));
    EXPECT_FALSE(baseTile.qualityLimited);
    EXPECT_FALSE(morphEnabled.qualityLimited);
    ASSERT_EQ(baseTile.patches.size(), 7u);
    ASSERT_EQ(morphEnabled.patches.size(), baseTile.patches.size());
    std::array<uint32_t, 5> depthCounts{};
    bool cameraLeafReachedTargetDensity = false;
    for (size_t index = 0; index < baseTile.patches.size(); ++index) {
        const auto& basePatch = baseTile.patches[index];
        const auto& morphPatch = morphEnabled.patches[index];
        EXPECT_EQ(basePatch.depth, morphPatch.depth);
        EXPECT_EQ(basePatch.x, morphPatch.x);
        EXPECT_EQ(basePatch.z, morphPatch.z);
        ++depthCounts[basePatch.depth];
        if (morphPatch.localMin.x <= view.cameraPosition.x && morphPatch.localMax.x >= view.cameraPosition.x
            && morphPatch.localMin.y <= view.cameraPosition.z && morphPatch.localMax.y >= view.cameraPosition.z) {
            cameraLeafReachedTargetDensity = true;
            EXPECT_LE((std::max)(morphPatch.cellSize.x, morphPatch.cellSize.y), settings.nearCellSize);
        }
    }
    EXPECT_TRUE(cameraLeafReachedTargetDensity);
    EXPECT_EQ(depthCounts[0], 0u);
    EXPECT_EQ(depthCounts[1], 3u);
    EXPECT_EQ(depthCounts[2], 4u);
    EXPECT_EQ(depthCounts[3], 0u);
    EXPECT_EQ(depthCounts[4], 0u);
}

TEST_F(SurfacePatchLodTest, MorphExpandedBoundsRetainLeavesAtTheFrustumFringe)
{
    renderer::SurfacePatchSurface surface;
    surface.localMin = { 1.1f, 0.2f };
    surface.localMax = { 1.3f, 0.6f };
    renderer::SurfacePatchLodSettings settings;
    settings.cellsPerPatch = 2;
    settings.maxDepth = 0;
    settings.maxPatches = 1;
    settings.nearDistance = 0.0f;
    auto view = LocalView();
    const math::Frustum frustum = math::Frustum::FromViewProjection(math::Matrix4::Identity());
    view.frustum = &frustum;
    renderer::SurfacePatchSelection selection;
    ASSERT_TRUE(renderer::SelectSurfacePatches(surface, settings, view, selection));
    EXPECT_TRUE(selection.patches.empty());
    settings.morphEnabled = true;
    ASSERT_TRUE(renderer::SelectSurfacePatches(surface, settings, view, selection));
    ASSERT_EQ(selection.patches.size(), 1u);
    EXPECT_NEAR(selection.patches.front().aabbMin.x, 0.9f, 1.0e-6f);
    EXPECT_NEAR(selection.patches.front().aabbMax.x, 1.5f, 1.0e-6f);
    EXPECT_NEAR(selection.patches.front().aabbMin.z, -0.2f, 1.0e-6f);
    EXPECT_NEAR(selection.patches.front().aabbMax.z, 1.0f, 1.0e-6f);
}

TEST_F(SurfacePatchLodTest, NearCellDensityUsesSignedRotatedNonuniformWorldScale)
{
    renderer::SurfacePatchSurface surface;
    surface.localMin = { 0.0f, 0.0f };
    surface.localMax = { 8.0f, 8.0f };
    const float diagonal = std::sqrt(0.5f);
    surface.worldMatrix.m[0][0] = -2.0f * diagonal;
    surface.worldMatrix.m[2][0] = -2.0f * diagonal;
    surface.worldMatrix.m[0][2] = 0.5f * diagonal;
    surface.worldMatrix.m[2][2] = -0.5f * diagonal;
    surface.worldMatrix.m[1][1] = 1.5f;
    surface.worldMatrix.m[0][3] = 20.0f;
    surface.worldMatrix.m[2][3] = -30.0f;
    renderer::SurfacePatchLodSettings settings;
    settings.cellsPerPatch = 8;
    settings.maxDepth = 4;
    settings.maxPatches = 256;
    settings.nearDistance = 1000.0f;
    settings.nearCellSize = 0.25f;
    settings.targetCellPixels = 1000000.0f;
    settings.morphEnabled = true;
    auto view = LocalView();
    view.cameraPosition = { 20.0f - 6.0f * diagonal, 0.0f, -30.0f - 10.0f * diagonal };
    renderer::SurfacePatchSelection selection;
    ASSERT_TRUE(renderer::SelectSurfacePatches(surface, settings, view, selection));
    ASSERT_EQ(selection.patches.size(), 64u);
    EXPECT_FALSE(selection.qualityLimited);
    for (const auto& patch : selection.patches) {
        EXPECT_EQ(patch.depth, 3u);
        EXPECT_LE((std::max)(patch.cellSize.x, patch.cellSize.y), settings.nearCellSize);
    }
}

TEST_F(SurfacePatchLodTest, AGenericSurfaceAcceptsSmallFiniteAxisScales)
{
    renderer::SurfacePatchSurface surface;
    surface.worldMatrix.m[0][0] = 1.0e-5f;
    surface.worldMatrix.m[1][1] = 1.0e-5f;
    surface.worldMatrix.m[2][2] = 1.0e-5f;
    renderer::SurfacePatchLodSettings settings;
    settings.maxDepth = 0;
    settings.maxPatches = 1;
    settings.nearDistance = 0.0f;
    auto view = LocalView();
    renderer::SurfacePatchSelection selection;
    ASSERT_TRUE(renderer::SelectSurfacePatches(surface, settings, view, selection));
    ASSERT_EQ(selection.patches.size(), 1u);
    const float expectedCell = 100.0f * 1.0e-5f / settings.cellsPerPatch;
    EXPECT_NEAR(selection.patches.front().cellSize.x, expectedCell, expectedCell * 0.0001f);
    EXPECT_NEAR(selection.patches.front().cellSize.y, expectedCell, expectedCell * 0.0001f);
    EXPECT_FALSE(selection.qualityLimited);
}

TEST_F(SurfacePatchLodTest, InvisibleCoarseNeighborsStillControlVisibleFinePatchBoundaries)
{
    renderer::SurfacePatchSurface surface;
    surface.localMin = {-2.0f, -2.0f};
    surface.localMax = {2.0f, 2.0f};
    surface.minHeight = -0.1f;
    surface.maxHeight = 0.1f;
    renderer::SurfacePatchLodSettings settings;
    settings.cellsPerPatch = 4;
    settings.maxDepth = 6;
    settings.maxPatches = 1024;
    settings.nearDistance = 0.2f;
    settings.nearCellSize = 0.015625f;
    auto view = LocalView();
    view.cameraPosition = {-1.3f, 0.1f, 0.4f};
    auto clip = math::Matrix4::Identity();
    clip.m[0][3] = 0.35f;
    const auto frustum = math::Frustum::FromViewProjection(clip);
    view.frustum = &frustum;
    renderer::SurfacePatchSelection selection;
    ASSERT_TRUE(renderer::SelectSurfacePatches(surface, settings, view, selection));
    uint32_t boundaries = 0;
    for (const auto& patch : selection.patches) {
        const auto mask = static_cast<uint8_t>(renderer::SurfacePatchEdge::MIN_X);
        if ((patch.coarseEdgeMask & mask) == 0 || patch.localMin.x >= -1.35f) continue;
        ++boundaries;
        const float width = patch.localMax.x - patch.localMin.x;
        const float height = patch.localMax.y - patch.localMin.y;
        const float coarseZ = surface.localMin.y
            + static_cast<float>(patch.z / 2u) * height * 2.0f;
        const math::Vector3 neighborCenter = {patch.localMin.x - width, 0.0f, coarseZ + height};
        const math::Vector3 neighborExtents = {width, 0.1f, height};
        EXPECT_FALSE(frustum.IntersectsAABB(neighborCenter, neighborExtents));
        const auto expectedCell = patch.cellSize * 2.0f;
        EXPECT_FLOAT_EQ(patch.edgeCellSize[0].x, expectedCell.x);
        EXPECT_FLOAT_EQ(patch.edgeCellSize[0].y, expectedCell.y);
        const auto boundaryCell = renderer::ResolveSurfacePatchCellSize(patch, {0.0f, 0.5f});
        EXPECT_FLOAT_EQ(boundaryCell.x, expectedCell.x);
        EXPECT_FLOAT_EQ(boundaryCell.y, expectedCell.y);
        for (uint32_t corner : {0u, 2u}) {
            EXPECT_GE(patch.cornerCellSize[corner].x, expectedCell.x);
            EXPECT_GE(patch.cornerCellSize[corner].y, expectedCell.y);
        }
    }
    EXPECT_GT(boundaries, 0u);
}

TEST_F(SurfacePatchLodTest, IncludingCulledLeavesKeepsTheSameVisibleTopologyAndBoundaryBandwidth)
{
    renderer::SurfacePatchSurface surface;
    surface.localMin = {-2.0f, -2.0f};
    surface.localMax = {2.0f, 2.0f};
    renderer::SurfacePatchLodSettings settings;
    settings.cellsPerPatch = 16;
    settings.maxDepth = 6;
    settings.nearDistance = 0.2f;
    settings.nearCellSize = 0.015625f;
    settings.morphEnabled = true;
    auto view = LocalView();
    view.cameraPosition = {-1.3f, 0.1f, 0.4f};
    auto clip = math::Matrix4::Identity();
    clip.m[0][3] = 0.35f;
    const auto frustum = math::Frustum::FromViewProjection(clip);
    view.frustum = &frustum;
    renderer::SurfacePatchSelection visible, all;
    ASSERT_TRUE(renderer::SelectSurfacePatches(surface, settings, view, visible));
    auto allView = view;
    allView.includeCulledPatches = true;
    ASSERT_TRUE(renderer::SelectSurfacePatches(surface, settings, allView, all));
    EXPECT_GT(all.patches.size(), visible.patches.size());
    EXPECT_EQ(all.qualityLimited, visible.qualityLimited);
    std::vector<const renderer::SurfacePatch*> visibleFromAll;
    for (const auto& patch : all.patches)
        if (renderer::IsSurfacePatchVisible(surface, patch, view)) visibleFromAll.push_back(&patch);
    ASSERT_EQ(visibleFromAll.size(), visible.patches.size());
    const auto expectVec2Equal = [](const math::Vector2& expected, const math::Vector2& actual) {
        EXPECT_FLOAT_EQ(expected.x, actual.x);
        EXPECT_FLOAT_EQ(expected.y, actual.y);
    };
    const auto expectVec3Equal = [](const math::Vector3& expected, const math::Vector3& actual) {
        EXPECT_FLOAT_EQ(expected.x, actual.x);
        EXPECT_FLOAT_EQ(expected.y, actual.y);
        EXPECT_FLOAT_EQ(expected.z, actual.z);
    };
    for (size_t i = 0; i < visible.patches.size(); ++i) {
        const auto& expected = visible.patches[i];
        const auto& actual = *visibleFromAll[i];
        EXPECT_EQ(actual.depth, expected.depth);
        EXPECT_EQ(actual.x, expected.x);
        EXPECT_EQ(actual.z, expected.z);
        expectVec2Equal(expected.localMin, actual.localMin);
        expectVec2Equal(expected.localMax, actual.localMax);
        expectVec2Equal(expected.uvMin, actual.uvMin);
        expectVec2Equal(expected.uvMax, actual.uvMax);
        expectVec3Equal(expected.aabbMin, actual.aabbMin);
        expectVec3Equal(expected.aabbMax, actual.aabbMax);
        expectVec2Equal(expected.cellSize, actual.cellSize);
        expectVec2Equal(expected.morphCellSize, actual.morphCellSize);
        EXPECT_EQ(actual.coarseEdgeMask, expected.coarseEdgeMask);
        for (uint32_t side = 0; side < 4; ++side) {
            expectVec2Equal(expected.edgeCellSize[side], actual.edgeCellSize[side]);
            expectVec2Equal(expected.cornerCellSize[side], actual.cornerCellSize[side]);
        }
    }
    ExpectFullBalancedCoverage(all);
}

TEST_F(SurfacePatchLodTest, AllSharedStitchedVerticesUseTheSameHeightSamplingBandwidth)
{
    const auto surface = WideSurface();
    renderer::SurfacePatchLodSettings settings;
    settings.cellsPerPatch = 4;
    settings.maxDepth = 7;
    settings.maxPatches = 1024;
    settings.nearDistance = 224.0f;
    settings.nearCellSize = 8.0f;
    const auto view = LocalView();
    renderer::SurfacePatchSelection selection;
    ASSERT_TRUE(renderer::SelectSurfacePatches(surface, settings, view, selection));
    std::map<std::pair<uint32_t, uint32_t>, std::pair<math::Vector2, uint32_t>> shared;
    uint32_t sharedCount = 0, stitchCount = 0, maximumSharedDepthDifference = 0;
    for (const auto& patch : selection.patches) {
        if (patch.coarseEdgeMask != 0) ++stitchCount;
        std::vector<uint32_t> indices;
        ASSERT_TRUE(renderer::BuildSurfacePatchIndices(settings.cellsPerPatch, patch.coarseEdgeMask, indices));
        const std::set<uint32_t> vertices(indices.begin(), indices.end());
        for (uint32_t index : vertices) {
            const uint32_t x = index % (settings.cellsPerPatch + 1u);
            const uint32_t z = index / (settings.cellsPerPatch + 1u);
            const uint32_t shift = settings.maxDepth - patch.depth;
            const std::pair<uint32_t, uint32_t> key = {
                (patch.x * settings.cellsPerPatch + x) << shift,
                (patch.z * settings.cellsPerPatch + z) << shift
            };
            const math::Vector2 cell = renderer::ResolveSurfacePatchCellSize(patch,
                { static_cast<float>(x) / settings.cellsPerPatch, static_cast<float>(z) / settings.cellsPerPatch });
            const auto [found, inserted] = shared.emplace(key, std::pair{ cell, patch.depth });
            if (!inserted) {
                ++sharedCount;
                maximumSharedDepthDifference = (std::max)(maximumSharedDepthDifference,
                    patch.depth - found->second.second);
                EXPECT_FLOAT_EQ(cell.x, found->second.first.x);
                EXPECT_FLOAT_EQ(cell.y, found->second.first.y);
            }
        }
    }
    EXPECT_GT(stitchCount, 0u);
    EXPECT_GT(sharedCount, 0u);
    EXPECT_GE(maximumSharedDepthDifference, 2u);
    ExpectFullBalancedCoverage(selection);
}

TEST_F(SurfacePatchLodTest, CullingIncludesHorizontalAndVerticalDisplacementBounds)
{
    renderer::SurfacePatchSurface surface;
    surface.localMin = { 1.1f, 0.2f };
    surface.localMax = { 1.3f, 0.6f };
    renderer::SurfacePatchLodSettings settings;
    settings.maxPatches = 1;
    auto view = LocalView();
    const math::Frustum frustum = math::Frustum::FromViewProjection(math::Matrix4::Identity());
    view.frustum = &frustum;
    renderer::SurfacePatchSelection selection;
    ASSERT_TRUE(renderer::SelectSurfacePatches(surface, settings, view, selection));
    EXPECT_TRUE(selection.patches.empty());
    surface.horizontalDisplacement = 0.2f;
    ASSERT_TRUE(renderer::SelectSurfacePatches(surface, settings, view, selection));
    ASSERT_EQ(selection.patches.size(), 1u);
    EXPECT_FLOAT_EQ(selection.patches.front().aabbMin.x, 0.9f);
    surface.minHeight = 1.1f;
    surface.maxHeight = 1.4f;
    ASSERT_TRUE(renderer::SelectSurfacePatches(surface, settings, view, selection));
    EXPECT_TRUE(selection.patches.empty());
    surface.minHeight = 0.9f;
    ASSERT_TRUE(renderer::SelectSurfacePatches(surface, settings, view, selection));
    ASSERT_EQ(selection.patches.size(), 1u);
    EXPECT_FLOAT_EQ(selection.patches.front().aabbMin.y, 0.9f);
    EXPECT_FLOAT_EQ(selection.patches.front().aabbMax.y, 1.4f);
}

TEST_F(SurfacePatchLodTest, MaximumDepthStopsRefinementWithoutDroppingTheSurface)
{
    const auto surface = WideSurface();
    renderer::SurfacePatchLodSettings settings;
    settings.cellsPerPatch = 4;
    settings.maxDepth = 2;
    settings.nearDistance = 0.0f;
    settings.targetCellPixels = 0.25f;
    auto view = LocalView();
    view.projectionScalePixels = 1.0f;
    renderer::SurfacePatchSelection selection;
    ASSERT_TRUE(renderer::SelectSurfacePatches(surface, settings, view, selection));
    ASSERT_EQ(selection.patches.size(), 16u);
    EXPECT_TRUE(selection.qualityLimited);
    for (const auto& patch : selection.patches) EXPECT_EQ(patch.depth, 2u);
    ExpectFullBalancedCoverage(selection);
}

}
}
