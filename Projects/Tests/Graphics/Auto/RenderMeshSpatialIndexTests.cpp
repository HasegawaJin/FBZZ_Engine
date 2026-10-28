/// @file    RenderMeshSpatialIndexTests.cpp
/// @brief   静的・動的BVHの候補と更新を現在値の線形走査と比較する。
/// @author  Hasegawa Jin
/// @date    2026-10-23
#include <TestKit/TestKit.hpp>
#include "../../../Graphics/src/Renderer/RenderMeshSpatialIndex.hpp"
#include <Math/MathUtils.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace fbzz::tests {
namespace {
using namespace renderer::spatial_detail;

math::Frustum View(const math::Vector3& position, const math::Vector3& forward = {0, 0, 1})
{
    return math::Frustum::FromViewProjection(math::Matrix4::Perspective(math::ToRad(70.0f), 1.2f, 0.1f, 200.0f)
        * math::Matrix4::LookAt(position, position + forward, math::Vector3::UP));
}

std::vector<MeshSource> Sources(size_t count)
{
    std::vector<MeshSource> sources;
    sources.reserve(count);
    for (size_t index = 0; index < count; ++index)
        sources.push_back({{static_cast<uint32_t>(index), 3, index + 1, false},
            {static_cast<float>(static_cast<int>(index % 32) - 16) * 8.0f,
                static_cast<float>(static_cast<int>(index / 32 % 8) - 4) * 7.0f,
                10.0f + static_cast<float>(index / 256) * 30.0f}, 1.0f, index % 7 == 0});
    return sources;
}

class RenderMeshSpatialIndexTest : public testkit::Fixture {
protected:
    void SetUp() override
    {
        testkit::Fixture::SetUp();
        options.mode = QueryMode::BVH;
        options.minimumTreeItems = 1;
    }
    void ExpectParity(RenderMeshSpatialIndex& index, std::span<const math::Frustum* const> frusta)
    {
        QueryDiagnostics linearStats, treeStats;
        std::vector<uint32_t> linear, tree;
        index.QueryUnion(frusta, linear, linearStats, QueryMode::LINEAR);
        index.QueryUnion(frusta, tree, treeStats, QueryMode::BVH);
        EXPECT_EQ(tree, linear);
        EXPECT_TRUE(std::is_sorted(tree.begin(), tree.end()));
        EXPECT_EQ(std::adjacent_find(tree.begin(), tree.end()), tree.end());
    }
    IndexOptions options;
};
} /// @note namespace

TEST_F(RenderMeshSpatialIndexTest, CameraShadowAndReflectionVolumesHaveIndependentOrderedCandidates)
{
    auto sources = Sources(1024);
    RenderMeshSpatialIndex index;
    const auto update = index.Refresh(10, sources, options);
    EXPECT_GT(update.staticSources, 0u);
    EXPECT_GT(update.dynamicSources, 0u);
    EXPECT_EQ(update.rebuiltTrees, 2u);
    const auto camera = View({0, 0, 0});
    const auto shadow = View({-120, 20, 0}, {0.2f, -0.1f, 1});
    const auto reflection = View({140, 0, 0}, {-0.2f, 0, 1});
    const std::array<const math::Frustum*, 1> cameraViews{&camera}, reflectionViews{&reflection};
    const std::array<const math::Frustum*, 2> shadowViews{&shadow, &reflection};
    ExpectParity(index, cameraViews);
    ExpectParity(index, shadowViews);
    ExpectParity(index, reflectionViews);
    QueryDiagnostics stats;
    std::vector<uint32_t> cameraCandidates, shadowCandidates;
    index.QueryUnion(cameraViews, cameraCandidates, stats);
    index.QueryUnion(shadowViews, shadowCandidates, stats);
    EXPECT_NE(cameraCandidates, shadowCandidates);
}

TEST_F(RenderMeshSpatialIndexTest, SurvivingVolumesRestoreSiblingMasksForBothPartitions)
{
    auto sources = Sources(256);
    for (size_t ordinal = 0; ordinal < sources.size(); ++ordinal) {
        sources[ordinal].center = {ordinal % 2 == 0 ? -600.0f : 600.0f,
            static_cast<float>(static_cast<int>(ordinal / 2 % 7) - 3) * 2.0f,
            20.0f + static_cast<float>(ordinal % 16) * 2.0f};
        sources[ordinal].dynamic = ordinal % 3 == 0;
    }
    RenderMeshSpatialIndex index;
    const auto update = index.Refresh(40, sources, options);
    ASSERT_EQ(update.rebuiltTrees, 2u);
    const auto left = View({-600, 0, 0}), right = View({600, 0, 0}), missed = View({0, 600, 0});
    const std::array<const math::Frustum*, 3> views{&left, &right, &missed};
    const std::array<const math::Frustum*, 4> reordered{&missed, &right, &left, &right};
    ExpectParity(index, views);
    ExpectParity(index, reordered);
    QueryDiagnostics stats;
    std::vector<uint32_t> first, second, oneVolume;
    index.QueryUnion(views, first, stats);
    index.QueryUnion(reordered, second, stats);
    EXPECT_EQ(first.size(), sources.size());
    EXPECT_EQ(first, second);
    for (const auto* volume : {&left, &right}) {
        const std::array<const math::Frustum*, 1> single{volume};
        index.QueryUnion(single, oneVolume, stats);
        EXPECT_EQ(oneVolume.size(), sources.size() / 2);
    }
}

TEST_F(RenderMeshSpatialIndexTest, SurvivingVolumesUseCurrentBoundsAfterJournalAndDenseRefits)
{
    options.updateMode = UpdateMode::STABLE;
    options.denseUpdate = DenseUpdate::REFIT;
    auto sources = Sources(256);
    for (size_t ordinal = 0; ordinal < sources.size(); ++ordinal)
        sources[ordinal].center = {ordinal % 2 == 0 ? -600.0f : 600.0f, 0, 30};
    RenderMeshSpatialIndex index;
    (void)index.Refresh(41, sources, options);
    const auto left = View({-600, 0, 0}), right = View({600, 0, 0});
    const std::array<const math::Frustum*, 2> views{&left, &right};
    QueryDiagnostics stats;
    std::vector<uint32_t> retained;
    index.QueryUnion(views, retained, stats);
    const auto saved = retained;
    sources[7].center = {0, 0, 30};
    sources[14].center = {600, 0, 30};
    const std::array<uint32_t, 2> changed{7, 14};
    UpdateDiagnostics sparse;
    ASSERT_TRUE(index.ApplyChanges(41, sources, changed, options, sparse));
    EXPECT_EQ(sparse.refittedLeaves, 2u);
    EXPECT_EQ(sparse.rebuiltTrees, 0u);
    ExpectParity(index, views);
    for (auto& source : sources) source.center.x += 1200;
    const auto dense = index.Refresh(41, sources, options);
    EXPECT_EQ(dense.refittedLeaves, sources.size());
    EXPECT_EQ(dense.rebuiltTrees, 0u);
    ExpectParity(index, views);
    EXPECT_EQ(retained, saved);
}

TEST_F(RenderMeshSpatialIndexTest, SurvivingVolumesRetainBoundaryCancellationAndUnboundedSpheres)
{
    const auto large = View({100000, 100000, 100000}), origin = View({0, 0, 0});
    const auto remote = View({-600, 0, 0});
    const std::array<const math::Frustum*, 3> views{&remote, &large, &origin};
    auto sources = Sources(129);
    for (size_t ordinal = 0; ordinal < sources.size(); ++ordinal) {
        auto& source = sources[ordinal];
        const auto plane = large.GetPlane(static_cast<int>(ordinal % math::Frustum::PLANE_COUNT));
        const float radius = ordinal % 2 == 0 ? 0.0001f : 10.0f;
        const math::Vector3 seed{100000, 100000, 100040};
        const float distance = math::Vector3::Dot(plane.normal, seed) + plane.distance;
        source.center = seed - plane.normal * (distance + radius);
        source.center.x = std::nextafter(source.center.x, ordinal % 3 == 0
            ? std::numeric_limits<float>::infinity() : -std::numeric_limits<float>::infinity());
        source.radius = radius;
    }
    sources[0].center = {0, 0, -1.0e8f};
    sources[0].radius = 1.0e8f;
    sources[1].center = {0, 0, -1.0e10f};
    sources[1].radius = 1.0e10f;
    sources[2].radius = 0;
    sources[3].radius = std::numeric_limits<float>::quiet_NaN();
    sources[4].center.x = std::numeric_limits<float>::infinity();
    sources[5].center = {0, 0, 20};
    sources[5].radius = std::numeric_limits<float>::denorm_min();
    for (const auto mode : {UpdateMode::LEGACY, UpdateMode::STABLE}) {
        options.updateMode = mode;
        RenderMeshSpatialIndex index;
        (void)index.Refresh(42, sources, options);
        ExpectParity(index, views);
        QueryDiagnostics stats;
        std::vector<uint32_t> candidates;
        index.QueryUnion(views, candidates, stats);
        for (uint32_t ordinal : {0u, 2u, 3u, 4u})
            EXPECT_NE(std::find(candidates.begin(), candidates.end(), ordinal), candidates.end());
    }
}

TEST_F(RenderMeshSpatialIndexTest, VolumeMaskBoundariesAndOversizedUnionsKeepTheLastVolume)
{
    auto sources = Sources(128);
    for (auto& source : sources) source.center = {600, 0, 30};
    RenderMeshSpatialIndex index;
    (void)index.Refresh(43, sources, options);
    const auto missed = View({-600, 0, 0}), hit = View({600, 0, 0});
    for (size_t count : {1u, 2u, 31u, 32u, 63u, 64u, 65u, 96u}) {
        std::vector<const math::Frustum*> views(count, &missed);
        views.back() = &hit;
        ExpectParity(index, views);
        QueryDiagnostics stats;
        std::vector<uint32_t> candidates;
        index.QueryUnion(views, candidates, stats);
        EXPECT_EQ(candidates.size(), sources.size());
        EXPECT_FALSE(stats.linear);
        EXPECT_FALSE(stats.numericFallback);
        EXPECT_GT(stats.visitedNodes, 0u);
    }
}

TEST_F(RenderMeshSpatialIndexTest, LateUnboundedAndUnsafeVolumesKeepFallbackVisitsTruthful)
{
    auto sources = Sources(64);
    RenderMeshSpatialIndex index;
    (void)index.Refresh(44, sources, options);
    const auto missed = View({10000, 0, 0});
    std::vector<const math::Frustum*> views(65, &missed);
    views.back() = nullptr;
    QueryDiagnostics stats;
    std::vector<uint32_t> candidates;
    index.QueryUnion(views, candidates, stats);
    EXPECT_EQ(candidates.size(), sources.size());
    EXPECT_EQ(stats.visitedNodes, 0u);
    EXPECT_EQ(stats.testedLeaves, 0u);
    EXPECT_FALSE(stats.numericFallback);
    const auto degenerate = math::Frustum::FromViewProjection(math::Matrix4{});
    auto invalidProjection = math::Matrix4::Identity();
    invalidProjection.m[0][0] = std::numeric_limits<float>::infinity();
    const auto nonfinite = math::Frustum::FromViewProjection(invalidProjection);
    for (const auto* unsafe : {&degenerate, &nonfinite}) {
        views.resize(64);
        views.back() = unsafe;
        ExpectParity(index, views);
        index.QueryUnion(views, candidates, stats);
        EXPECT_EQ(candidates.size(), sources.size());
        EXPECT_TRUE(stats.numericFallback);
        EXPECT_EQ(stats.visitedNodes, 0u);
        EXPECT_EQ(stats.testedLeaves, 0u);
    }
}

TEST_F(RenderMeshSpatialIndexTest, DirectBoundsWritesRefitOnlyChangedDynamicLeaves)
{
    auto sources = Sources(512);
    RenderMeshSpatialIndex index;
    (void)index.Refresh(11, sources, options);
    sources[7].center = {2, 1, 15};
    sources[14].radius = 12;
    const auto update = index.Refresh(11, sources, options);
    EXPECT_EQ(update.changedBounds, 2u);
    EXPECT_EQ(update.changedMembership, 0u);
    EXPECT_EQ(update.rebuiltTrees, 0u);
    EXPECT_EQ(update.refittedLeaves, 2u);
    EXPECT_EQ(update.refittedTrees, 1u);
    const auto view = View({0, 0, 0});
    const std::array<const math::Frustum*, 1> views{&view};
    ExpectParity(index, views);
    const auto unchanged = index.Refresh(11, sources, options);
    EXPECT_EQ(unchanged.changedBounds, 0u);
    EXPECT_EQ(unchanged.rebuiltTrees + unchanged.refittedTrees, 0u);
}

TEST_F(RenderMeshSpatialIndexTest, CreationDeletionReorderAndLifetimeReplacementRebuildCurrentMembership)
{
    auto sources = Sources(128);
    RenderMeshSpatialIndex index;
    (void)index.Refresh(12, sources, options);
    sources.erase(sources.begin() + 13);
    sources.push_back({{13, 4, 1000, false}, {0, 0, 8}, 2, false});
    std::reverse(sources.begin(), sources.end());
    auto update = index.Refresh(12, sources, options);
    EXPECT_GT(update.changedMembership, 0u);
    EXPECT_GT(update.rebuiltTrees, 0u);
    sources[4].identity.lifetime += 1000;
    update = index.Refresh(12, sources, options);
    EXPECT_EQ(update.changedMembership, 1u);
    const auto view = View({0, 0, 0});
    const std::array<const math::Frustum*, 1> views{&view};
    ExpectParity(index, views);
}

TEST_F(RenderMeshSpatialIndexTest, DenseMovementSupportsRefitRebuildAndLinearPolicies)
{
    auto sources = Sources(96);
    for (auto& source : sources) source.dynamic = true;
    RenderMeshSpatialIndex index;
    (void)index.Refresh(13, sources, options);
    options.denseUpdate = DenseUpdate::REFIT;
    for (auto& source : sources) source.center.x += 20;
    auto update = index.Refresh(13, sources, options);
    EXPECT_EQ(update.refittedLeaves, sources.size());
    EXPECT_EQ(update.rebuiltTrees, 0u);
    const auto view = View({0, 0, 0});
    const std::array<const math::Frustum*, 1> views{&view};
    ExpectParity(index, views);
    options.denseUpdate = DenseUpdate::REBUILD;
    for (auto& source : sources) source.center.x -= 40;
    update = index.Refresh(13, sources, options);
    EXPECT_EQ(update.rebuiltTrees, 1u);
    ExpectParity(index, views);
    options.denseUpdate = DenseUpdate::LINEAR;
    for (auto& source : sources) source.center.z += 30;
    update = index.Refresh(13, sources, options);
    EXPECT_TRUE(update.denseLinearFallback);
    ExpectParity(index, views);
    QueryDiagnostics query;
    std::vector<uint32_t> output;
    index.QueryUnion(views, output, query);
    EXPECT_TRUE(query.linear);
    update = index.Refresh(13, sources, options);
    EXPECT_EQ(update.rebuiltTrees, 1u);
    ExpectParity(index, views);
}

TEST_F(RenderMeshSpatialIndexTest, UnknownBoundsRemainCandidatesAndZeroConsumersReturnNothing)
{
    auto sources = Sources(64);
    sources[0].radius = 0;
    sources[1].radius = -1;
    sources[2].radius = std::numeric_limits<float>::quiet_NaN();
    sources[3].center.x = std::numeric_limits<float>::infinity();
    sources[4].center.x = 1.0e11f;
    RenderMeshSpatialIndex index;
    const auto update = index.Refresh(14, sources, options);
    EXPECT_EQ(update.unboundedSources, 5u);
    const auto view = View({0, 0, 0});
    const std::array<const math::Frustum*, 1> views{&view};
    ExpectParity(index, views);
    QueryDiagnostics stats;
    std::vector<uint32_t> output;
    index.QueryUnion(views, output, stats);
    for (uint32_t ordinal = 0; ordinal < 5; ++ordinal)
        EXPECT_NE(std::find(output.begin(), output.end(), ordinal), output.end());
    index.QueryUnion({}, output, stats);
    EXPECT_TRUE(output.empty());
    const std::array<const math::Frustum*, 1> unbounded{nullptr};
    index.QueryUnion(unbounded, output, stats);
    EXPECT_EQ(output.size(), sources.size());
}

TEST_F(RenderMeshSpatialIndexTest, BoundaryAndLargeCoordinateSpheresMatchCurrentFloatPredicate)
{
    const auto view = View({100000, 100000, 100000});
    auto sources = Sources(128);
    for (size_t ordinal = 0; ordinal < sources.size(); ++ordinal) {
        auto& source = sources[ordinal];
        const auto plane = view.GetPlane(static_cast<int>(ordinal % math::Frustum::PLANE_COUNT));
        const float radius = ordinal % 2 == 0 ? 0.0001f : 10.0f;
        const math::Vector3 seed{100000, 100000, 100040};
        const float signedDistance = math::Vector3::Dot(plane.normal, seed) + plane.distance;
        source.center = seed - plane.normal * (signedDistance + radius);
        source.radius = radius;
        source.center.x = std::nextafter(source.center.x,
            ordinal % 3 == 0 ? std::numeric_limits<float>::infinity() : -std::numeric_limits<float>::infinity());
    }
    RenderMeshSpatialIndex index;
    (void)index.Refresh(15, sources, options);
    const std::array<const math::Frustum*, 1> views{&view};
    ExpectParity(index, views);
}

TEST_F(RenderMeshSpatialIndexTest, SceneLifetimeResetCannotChangePreviouslyReturnedCandidateValues)
{
    auto sources = Sources(64);
    RenderMeshSpatialIndex index;
    (void)index.Refresh(16, sources, options);
    const auto view = View({0, 0, 0});
    const std::array<const math::Frustum*, 1> views{&view};
    QueryDiagnostics stats;
    std::vector<uint32_t> previous;
    index.QueryUnion(views, previous, stats);
    const auto saved = previous;
    sources.resize(3);
    for (auto& source : sources) { ++source.identity.generation; source.center = {10000, 0, 0}; }
    (void)index.Refresh(17, sources, options);
    EXPECT_EQ(previous, saved);
    ExpectParity(index, views);
}

TEST_F(RenderMeshSpatialIndexTest, GiantSphereCancellationRetainsCurrentFloatLeafVisibility)
{
    const auto view = View({0, 0, 0});
    const std::array<const math::Frustum*, 1> views{&view};
    auto sources = Sources(2);
    sources[0].center = {0, 0, -1.0e8f};
    sources[0].radius = 1.0e8f;
    sources[1].center = {0, 0, -1.0e10f};
    sources[1].radius = 1.0e10f;
    RenderMeshSpatialIndex index;
    (void)index.Refresh(19, sources, options);
    EXPECT_TRUE(view.IntersectsSphere(sources[0].center, sources[0].radius));
    ExpectParity(index, views);
    QueryDiagnostics stats;
    std::vector<uint32_t> output;
    index.QueryUnion(views, output, stats);
    EXPECT_NE(std::find(output.begin(), output.end(), 0u), output.end());
}

TEST_F(RenderMeshSpatialIndexTest, DegenerateFiniteProjectionFallsBackToAllCurrentSources)
{
    auto sources = Sources(64);
    math::Matrix4 invalidProjection{};
    for (auto& row : invalidProjection.m) for (float& value : row) value = 0.0f;
    const auto degenerate = math::Frustum::FromViewProjection(invalidProjection);
    const std::array<const math::Frustum*, 1> views{&degenerate};
    RenderMeshSpatialIndex index;
    (void)index.Refresh(20, sources, options);
    QueryDiagnostics stats;
    std::vector<uint32_t> output;
    index.QueryUnion(views, output, stats);
    EXPECT_TRUE(stats.numericFallback);
    EXPECT_EQ(output.size(), sources.size());
    ExpectParity(index, views);
}

TEST_F(RenderMeshSpatialIndexTest, OffscreenSubtreesSkipLeafPredicates)
{
    auto sources = Sources(1024);
    for (auto& source : sources) { source.center.x += 10000; source.dynamic = false; }
    RenderMeshSpatialIndex index;
    (void)index.Refresh(18, sources, options);
    const auto view = View({0, 0, 0});
    const std::array<const math::Frustum*, 1> views{&view};
    QueryDiagnostics linearStats, treeStats;
    std::vector<uint32_t> linear, tree;
    index.QueryUnion(views, linear, linearStats, QueryMode::LINEAR);
    index.QueryUnion(views, tree, treeStats, QueryMode::BVH);
    EXPECT_EQ(tree, linear);
    EXPECT_TRUE(tree.empty());
    EXPECT_EQ(linearStats.testedLeaves, sources.size());
    EXPECT_EQ(treeStats.testedLeaves, 0u);
    EXPECT_EQ(treeStats.visitedNodes, 1u);
}

TEST_F(RenderMeshSpatialIndexTest, StableMovementRefitsTheOriginalPartitionWithoutPromotion)
{
    options.updateMode = UpdateMode::STABLE;
    auto sources = Sources(512);
    for (auto& source : sources) source.dynamic = false;
    RenderMeshSpatialIndex index;
    (void)index.Refresh(21, sources, options);

    sources[7].center = {2, 1, 15};
    const auto update = index.Refresh(21, sources, options);

    EXPECT_EQ(update.staticSources, sources.size());
    EXPECT_EQ(update.dynamicSources, 0u);
    EXPECT_EQ(update.rebuiltTrees, 0u);
    EXPECT_EQ(update.refittedLeaves, 1u);
    const auto camera = View({0, 0, 0}), reflection = View({120, 0, 0}, {-0.2f, 0, 1});
    const std::array<const math::Frustum*, 1> cameraViews{&camera}, reflectionViews{&reflection};
    ExpectParity(index, cameraViews);
    ExpectParity(index, reflectionViews);
}

TEST_F(RenderMeshSpatialIndexTest, StableOrdinalReuseTracksReplacementAndReorderWithCurrentBounds)
{
    options.updateMode = UpdateMode::STABLE;
    options.denseUpdate = DenseUpdate::REFIT;
    auto sources = Sources(128);
    for (auto& source : sources) source.dynamic = false;
    RenderMeshSpatialIndex index;
    (void)index.Refresh(22, sources, options);

    std::reverse(sources.begin(), sources.end());
    ++sources[4].identity.generation;
    ++sources[7].identity.lifetime;
    const auto update = index.Refresh(22, sources, options);

    EXPECT_EQ(update.changedMembership, sources.size());
    EXPECT_EQ(update.rebuiltTrees, 0u);
    EXPECT_EQ(update.refittedLeaves, sources.size());
    const auto view = View({0, 0, 0});
    const std::array<const math::Frustum*, 1> views{&view};
    ExpectParity(index, views);

    ++sources[2].identity.generation;
    const auto identityOnly = index.Refresh(22, sources, options);
    EXPECT_EQ(identityOnly.changedMembership, 1u);
    EXPECT_EQ(identityOnly.changedBounds, 0u);
    EXPECT_EQ(identityOnly.rebuiltTrees + identityOnly.refittedTrees, 0u);
    ExpectParity(index, views);
}

TEST_F(RenderMeshSpatialIndexTest, StableTopologyChangesAndModeSwitchesRebuildCurrentClassification)
{
    options.updateMode = UpdateMode::STABLE;
    auto sources = Sources(64);
    for (auto& source : sources) source.dynamic = false;
    RenderMeshSpatialIndex index;
    (void)index.Refresh(23, sources, options);
    const auto view = View({0, 0, 0});
    const std::array<const math::Frustum*, 1> views{&view};

    sources[1].dynamic = true;
    EXPECT_GT(index.Refresh(23, sources, options).rebuiltTrees, 0u);
    sources[2].radius = 0;
    EXPECT_GT(index.Refresh(23, sources, options).rebuiltTrees, 0u);
    ExpectParity(index, views);
    sources[2].radius = 2;
    EXPECT_GT(index.Refresh(23, sources, options).rebuiltTrees, 0u);
    sources.pop_back();
    EXPECT_GT(index.Refresh(23, sources, options).rebuiltTrees, 0u);
    EXPECT_GT(index.Refresh(24, sources, options).rebuiltTrees, 0u);
    options.updateMode = UpdateMode::LEGACY;
    EXPECT_GT(index.Refresh(24, sources, options).rebuiltTrees, 0u);
    sources[3].center.x += 1;
    EXPECT_GT(index.Refresh(24, sources, options).rebuiltTrees, 0u);
    options.updateMode = UpdateMode::STABLE;
    const auto switched = index.Refresh(24, sources, options);
    EXPECT_GT(switched.rebuiltTrees, 0u);
    EXPECT_EQ(switched.dynamicSources, 1u);
    ExpectParity(index, views);
}

TEST_F(RenderMeshSpatialIndexTest, StableDenseRefitScansPostorderAndSparseRefitSortsOnlyAncestors)
{
    options.updateMode = UpdateMode::STABLE;
    options.denseUpdate = DenseUpdate::REFIT;
    auto sources = Sources(256);
    for (auto& source : sources) source.dynamic = false;
    RenderMeshSpatialIndex index;
    (void)index.Refresh(25, sources, options);

    for (auto& source : sources) source.center.x += 20;
    const auto dense = index.Refresh(25, sources, options);

    EXPECT_EQ(dense.refittedLeaves, sources.size());
    EXPECT_GT(dense.refitScannedNodes, 0u);
    EXPECT_EQ(dense.refitSortedAncestors, 0u);
    const auto view = View({0, 0, 0});
    const std::array<const math::Frustum*, 1> views{&view};
    ExpectParity(index, views);

    sources[7].center.z += 100;
    const auto sparse = index.Refresh(25, sources, options);
    EXPECT_EQ(sparse.refittedLeaves, 1u);
    EXPECT_EQ(sparse.refitScannedNodes, 0u);
    EXPECT_GT(sparse.refitSortedAncestors, 0u);
    ExpectParity(index, views);
}

TEST_F(RenderMeshSpatialIndexTest, CompleteJournalAppliesOnlyUniqueChangedOrdinals)
{
    options.updateMode = UpdateMode::STABLE;
    auto sources = Sources(512);
    RenderMeshSpatialIndex index;
    (void)index.Refresh(26, sources, options);
    sources[7].center = {2, 1, 15};
    ++sources[14].identity.lifetime;
    const std::array<uint32_t, 3> changed{7, 14, 7};
    UpdateDiagnostics update;

    ASSERT_TRUE(index.ApplyChanges(26, sources, changed, options, update));

    EXPECT_EQ(update.witnessedSources, 2u);
    EXPECT_EQ(update.changedBounds, 1u);
    EXPECT_EQ(update.changedMembership, 1u);
    EXPECT_EQ(update.refittedLeaves, 1u);
    EXPECT_EQ(update.rebuiltTrees, 0u);
    const auto view = View({0, 0, 0});
    const std::array<const math::Frustum*, 1> views{&view};
    ExpectParity(index, views);
}

TEST_F(RenderMeshSpatialIndexTest, JournalRejectsInvalidLineageAndTopologyBeforeChangingCurrentValues)
{
    options.updateMode = UpdateMode::STABLE;
    auto sources = Sources(64);
    RenderMeshSpatialIndex index;
    (void)index.Refresh(27, sources, options);
    const auto view = View({0, 0, 0});
    const std::array<const math::Frustum*, 1> views{&view};
    QueryDiagnostics query;
    std::vector<uint32_t> previous, current;
    index.QueryUnion(views, previous, query);
    const std::array<uint32_t, 1> changed{7}, invalid{64};
    UpdateDiagnostics update;
    sources[7].center = {10000, 0, 0};

    EXPECT_FALSE(index.ApplyChanges(28, sources, changed, options, update));
    EXPECT_FALSE(index.ApplyChanges(27, sources, invalid, options, update));
    sources[7].radius = 0;
    EXPECT_FALSE(index.ApplyChanges(27, sources, changed, options, update));
    sources[7].radius = 1;
    sources[7].dynamic = false;
    EXPECT_FALSE(index.ApplyChanges(27, sources, changed, options, update));

    index.QueryUnion(views, current, query);
    EXPECT_EQ(current, previous);
    (void)index.Refresh(27, sources, options);
    ExpectParity(index, views);
}

TEST_F(RenderMeshSpatialIndexTest, StableBoundsBuiltOncePreserveVariedRadiusAndCancellationVisibility)
{
    options.updateMode = UpdateMode::STABLE;
    auto sources = Sources(129);
    for (size_t ordinal = 0; ordinal < sources.size(); ++ordinal)
        sources[ordinal].radius = ordinal % 2 == 0 ? 0.0001f : 100.0f;
    sources[0].center = {0, 0, -1.0e8f};
    sources[0].radius = 1.0e8f;
    sources[1].center = {0, 0, -1.0e10f};
    sources[1].radius = 1.0e10f;
    sources[2].radius = std::numeric_limits<float>::quiet_NaN();
    RenderMeshSpatialIndex index;

    (void)index.Refresh(29, sources, options);

    const auto view = View({0, 0, 0});
    const std::array<const math::Frustum*, 1> views{&view};
    ExpectParity(index, views);
}

TEST_F(RenderMeshSpatialIndexTest, StableDenseLinearFallbackRequiresAFullWitnessBeforeTreeRecovery)
{
    options.updateMode = UpdateMode::STABLE;
    options.denseUpdate = DenseUpdate::LINEAR;
    auto sources = Sources(64);
    RenderMeshSpatialIndex index;
    (void)index.Refresh(30, sources, options);
    for (auto& source : sources) source.center.x += 50;
    std::vector<uint32_t> changed;
    for (uint32_t ordinal = 0; ordinal < sources.size(); ++ordinal) changed.push_back(ordinal);
    UpdateDiagnostics update;

    ASSERT_TRUE(index.ApplyChanges(30, sources, changed, options, update));

    EXPECT_TRUE(update.denseLinearFallback);
    EXPECT_FALSE(index.ApplyChanges(30, sources, {}, options, update));
    const auto view = View({0, 0, 0});
    const std::array<const math::Frustum*, 1> views{&view};
    ExpectParity(index, views);
    const auto recovered = index.Refresh(30, sources, options);
    EXPECT_GT(recovered.rebuiltTrees, 0u);
    EXPECT_EQ(recovered.witnessedSources, sources.size());
    ExpectParity(index, views);
}

TEST_F(RenderMeshSpatialIndexTest, DisabledDebugDoesNotCaptureOrChangeProductionCandidates)
{
    auto sources = Sources(128);
    RenderMeshSpatialIndex index;
    (void)index.Refresh(31, sources, options);
    const auto view = View({0, 0, 0});
    const std::array<const math::Frustum*, 1> views{&view};
    renderer::RenderBvhDebugRequest request;
    request.sceneGeneration = 31;
    renderer::RenderBvhDebugSnapshot capture;
    capture.totalNodes = 999;
    capture.nodes.push_back({});
    const auto* retained = capture.nodes.data();
    QueryDiagnostics referenceStats, capturedStats;
    std::vector<uint32_t> reference, captured;

    index.QueryUnion(views, reference, referenceStats);
    index.QueryUnion(views, captured, capturedStats, QueryMode::BVH, &request, &capture);

    EXPECT_EQ(captured, reference);
    EXPECT_EQ(capturedStats.visitedNodes, referenceStats.visitedNodes);
    EXPECT_EQ(capture.totalNodes, 999u);
    EXPECT_EQ(capture.nodes.size(), 1u);
    EXPECT_EQ(capture.nodes.data(), retained);
}

TEST_F(RenderMeshSpatialIndexTest, DebugCaptureHonorsDepthAndNodeBudgetFromActualTree)
{
    auto sources = Sources(128);
    RenderMeshSpatialIndex index;
    const auto update = index.Refresh(32, sources, options);
    const auto view = View({0, 0, 0});
    const std::array<const math::Frustum*, 1> views{&view};
    renderer::RenderBvhDebugRequest request;
    request.enabled = true;
    request.sceneGeneration = 32;
    request.minDepth = 1;
    request.maxDepth = 3;
    request.maxNodes = 5;
    renderer::RenderBvhDebugSnapshot capture;
    QueryDiagnostics stats;
    std::vector<uint32_t> candidates;

    index.QueryUnion(views, candidates, stats, QueryMode::BVH, &request, &capture);

    EXPECT_EQ(capture.availability, renderer::RenderBvhDebugAvailability::READY);
    EXPECT_EQ(capture.totalNodes, update.nodes);
    ASSERT_FALSE(capture.nodes.empty());
    EXPECT_LE(capture.nodes.size(), 5u);
    EXPECT_EQ(capture.examinedNodes, 5u);
    EXPECT_TRUE(capture.truncated);
    EXPECT_EQ(capture.visitedNodes, stats.visitedNodes);
    EXPECT_EQ(capture.candidates, candidates.size());
    for (const auto& node : capture.nodes) {
        EXPECT_GE(node.depth, 1u);
        EXPECT_LE(node.depth, 3u);
        EXPECT_LT(node.parentIndex, node.nodeIndex);
        EXPECT_LE(node.low.x, node.high.x);
        EXPECT_LE(node.low.y, node.high.y);
        EXPECT_LE(node.low.z, node.high.z);
    }
    request.maxNodes = 0;
    index.QueryUnion(views, candidates, stats, QueryMode::BVH, &request, &capture);
    EXPECT_TRUE(capture.nodes.empty());
    EXPECT_TRUE(capture.truncated);
    EXPECT_EQ(capture.examinedNodes, 0u);
    EXPECT_EQ(capture.selectionSourcesExamined, 0u);
    request.minDepth = 9;
    request.maxDepth = 2;
    index.QueryUnion(views, candidates, stats, QueryMode::BVH, &request, &capture);
    EXPECT_TRUE(capture.nodes.empty());
    EXPECT_FALSE(capture.truncated);
}

TEST_F(RenderMeshSpatialIndexTest, RejectedSubtreeDescendantsRemainUnobserved)
{
    auto sources = Sources(64);
    for (auto& source : sources) source.center.x += 10000;
    RenderMeshSpatialIndex index;
    (void)index.Refresh(33, sources, options);
    const auto view = View({0, 0, 0});
    const std::array<const math::Frustum*, 1> views{&view};
    renderer::RenderBvhDebugRequest request;
    request.enabled = true;
    request.sceneGeneration = 33;
    request.maxNodes = renderer::RENDER_BVH_DEBUG_MAX_NODES;
    renderer::RenderBvhDebugSnapshot capture;
    QueryDiagnostics stats;
    std::vector<uint32_t> candidates;

    index.QueryUnion(views, candidates, stats, QueryMode::BVH, &request, &capture);

    EXPECT_TRUE(candidates.empty());
    EXPECT_EQ(stats.visitedNodes, 2u);
    ASSERT_GT(capture.nodes.size(), 2u);
    for (const auto& node : capture.nodes)
        EXPECT_EQ(node.state, node.depth == 0 ? renderer::RenderBvhDebugState::REJECTED
            : renderer::RenderBvhDebugState::UNOBSERVED);
}

TEST_F(RenderMeshSpatialIndexTest, SelectedEntityPathKeepsGenerationAndPartitionIdentity)
{
    auto sources = Sources(64);
    sources[7].identity.skinned = true;
    sources[7].identity.generation = 0;
    RenderMeshSpatialIndex index;
    (void)index.Refresh(34, sources, options);
    const auto view = View({0, 0, 0});
    const std::array<const math::Frustum*, 1> views{&view};
    renderer::RenderBvhDebugRequest request;
    request.enabled = request.selectedOnly = true;
    request.sceneGeneration = 34;
    request.sourceIndex = sources[7].identity.index;
    request.sourceGeneration = sources[7].identity.generation;
    renderer::RenderBvhDebugSnapshot capture;
    QueryDiagnostics stats;
    std::vector<uint32_t> candidates;

    index.QueryUnion(views, candidates, stats, QueryMode::BVH, &request, &capture);

    EXPECT_TRUE(capture.selectedFound);
    ASSERT_FALSE(capture.nodes.empty());
    EXPECT_EQ(capture.nodes.front().depth, 0u);
    EXPECT_TRUE(capture.nodes.back().leaf);
    EXPECT_TRUE(capture.nodes.back().skinned);
    EXPECT_EQ(capture.nodes.back().sourceIndex, 7u);
    EXPECT_EQ(capture.nodes.back().sourceGeneration, 0u);
    for (size_t position = 0; position < capture.nodes.size(); ++position) {
        const auto& node = capture.nodes[position];
        EXPECT_TRUE(node.selectedPath);
        EXPECT_EQ(node.partition, renderer::RenderBvhDebugPartition::DYNAMIC);
        EXPECT_EQ(node.depth, position);
        if (position != 0) EXPECT_EQ(node.parentIndex, capture.nodes[position - 1].nodeIndex);
    }
    ++request.sourceGeneration;
    index.QueryUnion(views, candidates, stats, QueryMode::BVH, &request, &capture);
    EXPECT_FALSE(capture.selectedFound);
    EXPECT_TRUE(capture.nodes.empty());
}

TEST_F(RenderMeshSpatialIndexTest, CandidateLeafStatesMatchTheActualUnionAndFallbackDoesNotInventVisits)
{
    auto sources = Sources(128);
    RenderMeshSpatialIndex index;
    (void)index.Refresh(35, sources, options);
    const auto view = View({0, 0, 0});
    const std::array<const math::Frustum*, 1> views{&view};
    renderer::RenderBvhDebugRequest request;
    request.enabled = true;
    request.sceneGeneration = 35;
    request.maxNodes = renderer::RENDER_BVH_DEBUG_MAX_NODES;
    renderer::RenderBvhDebugSnapshot capture;
    QueryDiagnostics stats;
    std::vector<uint32_t> candidates, observedCandidates;

    index.QueryUnion(views, candidates, stats, QueryMode::BVH, &request, &capture);

    for (const auto& node : capture.nodes)
        if (node.state == renderer::RenderBvhDebugState::CANDIDATE) {
            EXPECT_TRUE(node.leaf);
            observedCandidates.push_back(node.sourceIndex);
        }
    std::sort(observedCandidates.begin(), observedCandidates.end());
    EXPECT_EQ(observedCandidates, candidates);
    const std::array<const math::Frustum*, 1> unbounded{nullptr};
    index.QueryUnion(unbounded, candidates, stats, QueryMode::BVH, &request, &capture);
    EXPECT_EQ(capture.availability, renderer::RenderBvhDebugAvailability::NUMERIC_FALLBACK);
    for (const auto& node : capture.nodes) EXPECT_EQ(node.state, renderer::RenderBvhDebugState::UNOBSERVED);
    EXPECT_EQ(capture.visitedNodes, 0u);
    options.mode = QueryMode::LINEAR;
    (void)index.Refresh(35, sources, options);
    index.QueryUnion(views, candidates, stats, QueryMode::BVH, &request, &capture);
    EXPECT_EQ(capture.availability, renderer::RenderBvhDebugAvailability::NO_CURRENT_TREE);
    EXPECT_TRUE(capture.nodes.empty());
    EXPECT_EQ(capture.totalNodes, 0u);
}

TEST_F(RenderMeshSpatialIndexTest, DebugWorkBudgetBoundsFilteredHierarchyAndAbsentSelectedLookup)
{
    auto sources = Sources(renderer::RENDER_BVH_DEBUG_MAX_SELECTION_SOURCES + 512);
    RenderMeshSpatialIndex index;
    (void)index.Refresh(36, sources, options);
    const auto view = View({0, 0, 0});
    const std::array<const math::Frustum*, 1> views{&view};
    renderer::RenderBvhDebugRequest request;
    request.enabled = true;
    request.sceneGeneration = 36;
    request.maxNodes = 0;
    request.sourceIndex = std::numeric_limits<uint32_t>::max() - 1;
    request.sourceGeneration = 3;
    renderer::RenderBvhDebugSnapshot capture;
    QueryDiagnostics stats;
    std::vector<uint32_t> candidates;

    index.QueryUnion(views, candidates, stats, QueryMode::BVH, &request, &capture);

    EXPECT_TRUE(capture.nodes.empty());
    EXPECT_EQ(capture.examinedNodes, 0u);
    EXPECT_EQ(capture.selectionSourcesExamined, 0u);
    EXPECT_TRUE(capture.selectedLookupTruncated);
    request.maxNodes = 17;
    request.minDepth = request.maxDepth = 64;
    index.QueryUnion(views, candidates, stats, QueryMode::BVH, &request, &capture);
    EXPECT_TRUE(capture.nodes.empty());
    EXPECT_EQ(capture.examinedNodes, 17u);
    EXPECT_EQ(capture.selectionSourcesExamined, renderer::RENDER_BVH_DEBUG_MAX_SELECTION_SOURCES);
    EXPECT_TRUE(capture.truncated);
    EXPECT_FALSE(capture.selectedFound);
    EXPECT_TRUE(capture.selectedLookupTruncated);
    request.sourceIndex = std::numeric_limits<uint32_t>::max();
    index.QueryUnion(views, candidates, stats, QueryMode::BVH, &request, &capture);
    EXPECT_EQ(capture.selectionSourcesExamined, 0u);
    request.selectedOnly = true;
    request.sourceIndex = sources.back().identity.index;
    request.sourceGeneration = sources.back().identity.generation;
    request.minDepth = 0;
    request.maxDepth = 32;
    index.QueryUnion(views, candidates, stats, QueryMode::BVH, &request, &capture);
    EXPECT_TRUE(capture.nodes.empty());
    EXPECT_EQ(capture.examinedNodes, 0u);
    EXPECT_EQ(capture.selectionSourcesExamined, renderer::RENDER_BVH_DEBUG_MAX_SELECTION_SOURCES);
    EXPECT_FALSE(capture.selectedFound);
    EXPECT_TRUE(capture.selectedLookupTruncated);
}

} /// @note namespace fbzz::tests
