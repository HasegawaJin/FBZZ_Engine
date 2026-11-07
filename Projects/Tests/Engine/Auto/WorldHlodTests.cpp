/// @file    WorldHlodTests.cpp
/// @brief   Cell HLOD aggregation preserves authored parts, provenance, transforms and failure atomicity.
/// @author  Hasegawa Jin
/// @date    2026-11-07
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Engine/Scene/WorldHlod.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector4.hpp>
#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <set>
#include <utility>

namespace fbzz::tests {
namespace {
math::Matrix4 PlacementMatrix(const renderer::InstancePlacement& placement)
{
    return math::Matrix4::TRS(placement.position, math::Quaternion::FromEuler(placement.eulerRadians), placement.scale);
}

void ExpectMatrixNear(const math::Matrix4& actual, const math::Matrix4& expected)
{
    for (size_t row = 0; row < 4; ++row) for (size_t column = 0; column < 4; ++column)
        EXPECT_NEAR(actual.m[row][column], expected.m[row][column], 0.0002f);
}

void ExpectVectorBits(const math::Vector3& actual, const math::Vector3& expected)
{
    EXPECT_EQ(std::bit_cast<uint32_t>(actual.x), std::bit_cast<uint32_t>(expected.x));
    EXPECT_EQ(std::bit_cast<uint32_t>(actual.y), std::bit_cast<uint32_t>(expected.y));
    EXPECT_EQ(std::bit_cast<uint32_t>(actual.z), std::bit_cast<uint32_t>(expected.z));
}

void ExpectGroupsEqual(const std::vector<scene::WorldHlodGroup>& actual, const std::vector<scene::WorldHlodGroup>& expected)
{
    ASSERT_EQ(actual.size(), expected.size());
    for (size_t i = 0; i < actual.size(); ++i) {
        EXPECT_EQ(actual[i].cell, expected[i].cell); EXPECT_EQ(actual[i].layer, expected[i].layer);
        EXPECT_EQ(actual[i].castShadows, expected[i].castShadows); EXPECT_EQ(actual[i].hasWorldBounds, expected[i].hasWorldBounds);
        ExpectVectorBits(actual[i].origin, expected[i].origin);
        ExpectVectorBits(actual[i].worldBounds.min, expected[i].worldBounds.min);
        ExpectVectorBits(actual[i].worldBounds.max, expected[i].worldBounds.max);
        EXPECT_TRUE(asset::MeshInstancesAssetsEqual(actual[i].proxy, expected[i].proxy));
        ASSERT_EQ(actual[i].sources.size(), expected[i].sources.size());
        for (size_t j = 0; j < actual[i].sources.size(); ++j) {
            EXPECT_EQ(actual[i].sources[j].sourceKey, expected[i].sources[j].sourceKey);
            EXPECT_EQ(actual[i].sources[j].sourceId, expected[i].sources[j].sourceId);
        }
    }
}
} /// @note namespace

class WorldHlodTest : public testkit::EngineFixture {
protected:
    scene::WorldHlodSource Source(std::string key) const
    {
        scene::WorldHlodSource source;
        source.sourceKey = std::move(key); source.parts = m_parts; source.lodHeights = m_lodHeights; source.placements = m_placements;
        return source;
    }
    std::vector<asset::InstanceMeshPart> m_parts{{"HighTrunk.obj", "Bark.mat", 0}, {"HighLeaves.obj", "Leaves.mat", 0},
        {"LowTrunk.obj", "Bark.mat", 1}, {"LowLeaves.obj", "Leaves.mat", 1}};
    std::vector<float> m_lodHeights{0.3f, 0.1f};
    std::vector<renderer::InstancePlacement> m_placements{{7, {10, 0, 10}, {}, {1, 1, 1}, true}};
    scene::WorldHlodBuildSettings m_settings{{{}, 64.0f}};
    std::vector<scene::WorldHlodGroup> m_groups;
    std::string m_diagnostic;
};

TEST_F(WorldHlodTest, AggregatesOwnersAndKeepsEveryPartOfOnlyTheLowestLod)
{
    std::array sources{Source("Forest/A"), Source("Forest/B")};
    sources[1].worldTransform = math::Matrix4::Translate({20, 0, 0});
    ASSERT_TRUE(scene::BuildWorldHlod(sources, m_settings, m_groups, m_diagnostic)) << m_diagnostic;
    ASSERT_EQ(m_groups.size(), 1u); const auto& group = m_groups.front();
    EXPECT_EQ(group.cell, (scene::WorldCellId{0, 0}));
    const auto parts = group.proxy.instances.Parts(); ASSERT_EQ(parts.size(), 2u);
    std::set<std::string> meshes;
    for (const auto& part : parts) {
        EXPECT_EQ(part.lod, 0u); meshes.insert(part.meshPath);
        EXPECT_EQ(part.materialPath, part.meshPath == "LowTrunk.obj" ? "Bark.mat" : "Leaves.mat");
    }
    EXPECT_EQ(meshes, (std::set<std::string>{"LowLeaves.obj", "LowTrunk.obj"}));
    const auto snapshot = group.proxy.instances.Snapshot(); ASSERT_NE(snapshot, nullptr);
    ASSERT_EQ(snapshot->placements.size(), 2u); ASSERT_EQ(group.sources.size(), 2u);
    std::set<uint32_t> ids; std::set<std::string> owners;
    for (size_t i = 0; i < snapshot->placements.size(); ++i) {
        EXPECT_GT(snapshot->placements[i].id, 0u); EXPECT_TRUE(ids.insert(snapshot->placements[i].id).second);
        EXPECT_EQ(group.sources[i].sourceId, 7u); owners.insert(group.sources[i].sourceKey);
        EXPECT_VEC3_NEAR(snapshot->placements[i].position + group.origin,
            math::Vector3(group.sources[i].sourceKey == "Forest/A" ? 10.0f : 30.0f, 0, 10), 0.0002f);
    }
    EXPECT_EQ(owners, (std::set<std::string>{"Forest/A", "Forest/B"}));
}

TEST_F(WorldHlodTest, LayerShadowAndMaterialDifferencesCreateIndependentGroups)
{
    auto alternateParts = m_parts; alternateParts[3].materialPath = "AutumnLeaves.mat";
    std::array sources{Source("A"), Source("B"), Source("C"), Source("D")};
    sources[1].layer = 3; sources[2].castShadows = false; sources[3].parts = alternateParts;
    ASSERT_TRUE(scene::BuildWorldHlod(sources, m_settings, m_groups, m_diagnostic)) << m_diagnostic;
    ASSERT_EQ(m_groups.size(), 4u);
    for (const auto& group : m_groups) {
        const auto snapshot = group.proxy.instances.Snapshot(); ASSERT_NE(snapshot, nullptr);
        EXPECT_EQ(snapshot->placements.size(), 1u); EXPECT_EQ(group.sources.size(), 1u);
    }
}

TEST_F(WorldHlodTest, NegativeCellsPreserveComposedWorldTransformsWithUniformOwnerScale)
{
    m_placements[0] = {7, {1, 2, 3}, {0.2f, 0.4f, -0.1f}, {0.7f, 1.4f, 0.9f}, true};
    auto source = Source("Rotated");
    source.worldTransform = math::Matrix4::TRS({-128, 7, -128}, math::Quaternion::FromEuler({0.3f, 0.6f, -0.2f}), {2, 2, 2});
    ASSERT_TRUE(scene::BuildWorldHlod(std::span(&source, 1), m_settings, m_groups, m_diagnostic)) << m_diagnostic;
    ASSERT_EQ(m_groups.size(), 1u); const auto& group = m_groups[0];
    EXPECT_EQ(group.cell, (scene::WorldCellId{-2, -2})); EXPECT_VEC3_NEAR(group.origin, math::Vector3(-128, 0, -128), 0.0f);
    const auto snapshot = group.proxy.instances.Snapshot(); ASSERT_NE(snapshot, nullptr); ASSERT_EQ(snapshot->placements.size(), 1u);
    ExpectMatrixNear(math::Matrix4::Translate(group.origin) * PlacementMatrix(snapshot->placements[0]),
        source.worldTransform * PlacementMatrix(m_placements[0]));
    m_placements[0].eulerRadians = {1.57079632679f, 0.4f, -0.2f};
    source.worldTransform = math::Matrix4::TRS({-128, 7, -128}, math::Quaternion::Identity(), {2, 2, 2});
    ASSERT_TRUE(scene::BuildWorldHlod(std::span(&source, 1), m_settings, m_groups, m_diagnostic)) << m_diagnostic;
    ASSERT_EQ(m_groups.size(), 1u); const auto gimbalSnapshot = m_groups[0].proxy.instances.Snapshot();
    ASSERT_NE(gimbalSnapshot, nullptr); ASSERT_EQ(gimbalSnapshot->placements.size(), 1u);
    ExpectMatrixNear(math::Matrix4::Translate(m_groups[0].origin) * PlacementMatrix(gimbalSnapshot->placements[0]),
        source.worldTransform * PlacementMatrix(m_placements[0]));
}

TEST_F(WorldHlodTest, SourceTraversalOrderDoesNotChangeGroupsIdsOrProvenance)
{
    std::array sources{Source("C"), Source("A"), Source("B")};
    sources[0].worldTransform = math::Matrix4::Translate({80, 0, 0});
    sources[2].worldTransform = math::Matrix4::Translate({20, 0, 0});
    ASSERT_TRUE(scene::BuildWorldHlod(sources, m_settings, m_groups, m_diagnostic)) << m_diagnostic;
    const auto before = m_groups; std::reverse(sources.begin(), sources.end());
    ASSERT_TRUE(scene::BuildWorldHlod(sources, m_settings, m_groups, m_diagnostic)) << m_diagnostic;
    ExpectGroupsEqual(m_groups, before);
}

TEST_F(WorldHlodTest, ExcludedSourceIsIgnoredBeforeItsInvalidDataIsExamined)
{
    std::array sources{Source("Valid"), scene::WorldHlodSource{}};
    sources[1].includeHlod = false; sources[1].worldTransform.m[0][0] = std::numeric_limits<float>::quiet_NaN();
    ASSERT_TRUE(scene::BuildWorldHlod(sources, m_settings, m_groups, m_diagnostic)) << m_diagnostic;
    ASSERT_EQ(m_groups.size(), 1u); ASSERT_EQ(m_groups[0].sources.size(), 1u);
    EXPECT_EQ(m_groups[0].sources[0].sourceKey, "Valid");
}

TEST_F(WorldHlodTest, KnownBoundsEncloseEveryTransformedCornerAndUnknownBoundsDisableRejection)
{
    auto source = Source("Known"); source.hasLocalBounds = true; source.localBounds = {{-1, -2, -3}, {1, 2, 3}};
    source.worldTransform = math::Matrix4::TRS({20, 4, 20}, math::Quaternion::FromEuler({0.1f, 0.5f, 0.2f}), {2, 2, 2});
    ASSERT_TRUE(scene::BuildWorldHlod(std::span(&source, 1), m_settings, m_groups, m_diagnostic)) << m_diagnostic;
    ASSERT_EQ(m_groups.size(), 1u); ASSERT_TRUE(m_groups[0].hasWorldBounds);
    const auto world = source.worldTransform * PlacementMatrix(m_placements[0]);
    math::Vector3 minimum{1e9f, 1e9f, 1e9f}, maximum{-1e9f, -1e9f, -1e9f};
    for (float x : {-1.0f, 1.0f}) for (float y : {-2.0f, 2.0f}) for (float z : {-3.0f, 3.0f}) {
        const auto corner = world * math::Vector4{x, y, z, 1};
        EXPECT_LE(m_groups[0].worldBounds.min.x, corner.x); EXPECT_GE(m_groups[0].worldBounds.max.x, corner.x);
        EXPECT_LE(m_groups[0].worldBounds.min.y, corner.y); EXPECT_GE(m_groups[0].worldBounds.max.y, corner.y);
        EXPECT_LE(m_groups[0].worldBounds.min.z, corner.z); EXPECT_GE(m_groups[0].worldBounds.max.z, corner.z);
        minimum = {(std::min)(minimum.x, corner.x), (std::min)(minimum.y, corner.y), (std::min)(minimum.z, corner.z)};
        maximum = {(std::max)(maximum.x, corner.x), (std::max)(maximum.y, corner.y), (std::max)(maximum.z, corner.z)};
    }
    EXPECT_VEC3_NEAR(m_groups[0].worldBounds.min, minimum, 0.0002f);
    EXPECT_VEC3_NEAR(m_groups[0].worldBounds.max, maximum, 0.0002f);
    std::array sources{source, Source("Unknown")};
    ASSERT_TRUE(scene::BuildWorldHlod(sources, m_settings, m_groups, m_diagnostic)) << m_diagnostic;
    ASSERT_EQ(m_groups.size(), 1u); EXPECT_FALSE(m_groups[0].hasWorldBounds);
}

TEST_F(WorldHlodTest, InvalidTransformsAndBudgetsPreservePreviousOutputAndAllInputValues)
{
    auto source = Source("Source");
    ASSERT_TRUE(scene::BuildWorldHlod(std::span(&source, 1), m_settings, m_groups, m_diagnostic)) << m_diagnostic;
    const auto previousOutput = m_groups; const auto validPlacement = m_placements[0];
    const auto reject = [&](const scene::WorldHlodBuildSettings& settings) {
        const auto placementsBefore = m_placements; const auto partsBefore = m_parts; const auto matrixBefore = source.worldTransform;
        const auto lodHeightsBefore = m_lodHeights; const auto keyBefore = source.sourceKey;
        EXPECT_FALSE(scene::BuildWorldHlod(std::span(&source, 1), settings, m_groups, m_diagnostic)); EXPECT_FALSE(m_diagnostic.empty());
        ExpectGroupsEqual(m_groups, previousOutput); ASSERT_EQ(m_placements.size(), placementsBefore.size());
        EXPECT_EQ(m_lodHeights, lodHeightsBefore); EXPECT_EQ(source.sourceKey, keyBefore);
        for (size_t i = 0; i < m_placements.size(); ++i) {
            EXPECT_EQ(m_placements[i].id, placementsBefore[i].id); EXPECT_EQ(m_placements[i].active, placementsBefore[i].active);
            ExpectVectorBits(m_placements[i].position, placementsBefore[i].position);
            ExpectVectorBits(m_placements[i].eulerRadians, placementsBefore[i].eulerRadians); ExpectVectorBits(m_placements[i].scale, placementsBefore[i].scale);
        }
        for (size_t i = 0; i < m_parts.size(); ++i) {
            EXPECT_EQ(m_parts[i].meshPath, partsBefore[i].meshPath); EXPECT_EQ(m_parts[i].materialPath, partsBefore[i].materialPath); EXPECT_EQ(m_parts[i].lod, partsBefore[i].lod);
        }
        for (size_t row = 0; row < 4; ++row) for (size_t column = 0; column < 4; ++column)
            EXPECT_EQ(std::bit_cast<uint32_t>(source.worldTransform.m[row][column]), std::bit_cast<uint32_t>(matrixBefore.m[row][column]));
    };
    m_placements[0].position.x = std::numeric_limits<float>::quiet_NaN(); reject(m_settings);
    m_placements[0] = validPlacement; m_placements[0].scale.x = -1; reject(m_settings);
    m_placements[0] = validPlacement; source.worldTransform.m[0][0] = std::numeric_limits<float>::infinity(); reject(m_settings);
    source.worldTransform = math::Matrix4::Scale({2, 1, 1}); m_placements[0].eulerRadians.y = 0.5f; reject(m_settings);
    source.worldTransform = math::Matrix4::Identity(); m_placements[0] = validPlacement;
    m_placements.push_back({9, {90, 0, 10}, {}, {1, 1, 1}, true}); source.placements = m_placements;
    auto limited = m_settings; limited.maxGroups = 1; reject(limited);
    limited = m_settings; limited.maxInstances = 1; reject(limited);
}
} /// @note namespace fbzz::tests
