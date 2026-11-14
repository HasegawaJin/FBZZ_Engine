/// @file    TerrainScatterTests.cpp
/// @brief   地形条件・変換・配置アセットの分離と確定 Undo を検証する。
/// @author  Hasegawa Jin
/// @date    2026-11-04
#include <TestKit/TestKit.hpp>
#include <TestKit/Fixture.hpp>
#include <TestKit/Editor/EditorFixture.hpp>
#include <TestKit/Engine/ResourceTrackingRenderer.hpp>
#include <Tools/TerrainScatter.hpp>
#include <Tools/TerrainPaintClearance.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <Editor/Util/AssetDirtyRegistry.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Asset/DataAssetRegistry.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/InstanceSetAsset.hpp>
#include <Engine/Scene/Components/InstancedMeshComponent.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Engine/Scene/Components/WaterComponent.hpp>
#include <Engine/Scene/TerrainSurfaceQuery.hpp>
#include <Engine/Scene/Transform.hpp>
#include <Engine/Scene/WorldCell.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <array>
#include <limits>
#include <map>

namespace fbzz::tests {
class TerrainScatterTest : public testkit::Fixture {
protected:
    void SetUp() override
    {
        Fixture::SetUp();
        m_terrain.Resize(5, 5); m_terrain.cellSize = 2.0f; m_terrain.maxHeight = 10.0f;
        m_terrain.InitFlat(); m_settings.spacing = 2.0f;
    }
    bool Build() { return editor::BuildTerrainScatter(m_terrain, m_terrainTransform, m_targetTransform, m_settings, m_placements, m_reason); }
    scene::TerrainComponent m_terrain;
    scene::Transform m_terrainTransform, m_targetTransform;
    editor::TerrainScatterSettings m_settings;
    std::vector<renderer::InstancePlacement> m_placements;
    std::string m_reason;
};

TEST_F(TerrainScatterTest, WaterClearanceDistanceMatchesExistingThresholdSearch)
{
    scene::WaterComponent water;
    water.extentX = 4.0f;
    water.extentZ = 4.0f;
    scene::Transform waterTransform;
    waterTransform.worldPosition = {4.0f, 1.0f, 4.0f};
    const std::array surfaces{std::pair<const scene::WaterComponent*, const scene::Transform*>{&water, &waterTransform}};
    const std::array centers{math::Vector3{4.0f, 0.0f, 4.0f}};
    const editor::TerrainScatterBrush brush{centers, 4.0f, false};
    constexpr float RADIUS = 10.3f;
    const auto clearance = editor::BuildTerrainPaintClearance(m_terrain, m_terrainTransform, brush, {}, surfaces, RADIUS, 0.0f);
    ASSERT_EQ(clearance.water.size(), 4u);
    const auto originalDistance = [&](const math::Vector3& point, float radius) {
        if (!clearance.NearWater(point, radius)) return 1000000.0f;
        float low = 0.0f, high = radius;
        for (int step = 0; step < 16; ++step) {
            const float middle = (low + high) * 0.5f;
            if (clearance.NearWater(point, middle)) high = middle;
            else low = middle;
        }
        return high;
    };
    const std::array points{
        math::Vector3{6.0f, 1.0f, 4.0f},
        math::Vector3{7.0f, 3.0f, 4.0f},
        math::Vector3{7.0f, 6.0f, 4.0f},
        math::Vector3{9.0f, 4.0f, 9.0f},
        math::Vector3{7.0f, 25.0f, 4.0f},
        math::Vector3{20.0f, 1.0f, 4.0f},
    };
    EXPECT_GT(clearance.WaterDistance(points[0], RADIUS), 0.0f);
    EXPECT_NEAR(clearance.WaterDistance(points[2], RADIUS), 5.0f, RADIUS / 65536.0f);
    EXPECT_EQ(clearance.WaterDistance(points[4], RADIUS), 1000000.0f);
    EXPECT_EQ(clearance.WaterDistance(points[1], 1.0f), 1000000.0f);
    for (const auto& point : points) {
        for (const float radius : {1.0f, 4.0f, RADIUS}) {
            EXPECT_EQ(clearance.WaterDistance(point, radius), originalDistance(point, radius));
        }
    }
}

TEST_F(TerrainScatterTest, CombinedPathClearancePreservesSharedSamplesAndHoles)
{
    m_terrain.layerMaterials = {"ground", "trail-a", "trail-b"};
    for (int z = 0; z < m_terrain.rows; ++z) {
        for (int x = 2; x < m_terrain.columns; ++x) {
            ASSERT_TRUE(m_terrain.PaintLayerAtGrid(x, z, 1, 0.30f));
            ASSERT_TRUE(m_terrain.PaintLayerAtGrid(x, z, 2, 0.25f));
        }
    }
    const float mask = m_terrain.GetLayerWeightAtGrid(2, 1, 1) + m_terrain.GetLayerWeightAtGrid(2, 1, 2);
    ASSERT_GT(mask, 0.35f);
    const float edgeX = (1.0f + 0.35f / mask) * m_terrain.cellSize;
    const std::array layers{1, 2};
    const std::array centers{math::Vector3{4.0f, 0.0f, 4.0f}};
    const editor::TerrainScatterBrush brush{centers, 4.0f, false};
    const auto makeClearance = [&](bool combine) {
        return editor::BuildTerrainPaintClearance(m_terrain, m_terrainTransform, brush, layers, {}, 0.0f, 1.0f, combine);
    };
    EXPECT_FALSE(makeClearance(false).NearPath({edgeX, 0.0f, 3.0f}, 0.1f));
    EXPECT_NEAR(makeClearance(true).PathDistance({edgeX, 0.0f, 3.0f}, 1.0f), 0.0f, 0.00001f);

    ASSERT_TRUE(m_terrain.SetHoleCell(1, 1, true));
    const auto clearance = makeClearance(true);
    EXPECT_FALSE(clearance.NearPath({edgeX, 0.0f, 3.0f}, 0.1f));
    EXPECT_TRUE(clearance.NearPath({edgeX, 0.0f, 5.0f}, 0.1f));
    EXPECT_FALSE(clearance.NearPath({100.0f, 0.0f, 100.0f}, 1.0f));
}

TEST_F(TerrainScatterTest, SameSeedReproducesTransformsAndDensityKeepsCandidateIdentity)
{
    ASSERT_TRUE(Build()) << m_reason; ASSERT_EQ(m_placements.size(), 16u);
    const auto before = m_placements;
    ASSERT_TRUE(Build()); ASSERT_EQ(m_placements.size(), before.size());
    for (size_t i = 0; i < before.size(); ++i) {
        EXPECT_EQ(m_placements[i].id, before[i].id);
        EXPECT_VEC3_NEAR(m_placements[i].position, before[i].position, 0.0f);
        EXPECT_VEC3_NEAR(m_placements[i].eulerRadians, before[i].eulerRadians, 0.0f);
        EXPECT_VEC3_NEAR(m_placements[i].scale, before[i].scale, 0.0f);
    }
    m_settings.density = 0.5f; ASSERT_TRUE(Build()); ASSERT_FALSE(m_placements.empty()); EXPECT_LT(m_placements.size(), before.size());
    for (const auto& placement : m_placements) EXPECT_VEC3_NEAR(placement.position, before[placement.id - 1].position, 0.0f);
    m_settings.density = 0.0f; ASSERT_TRUE(Build()); EXPECT_TRUE(m_placements.empty());
}

TEST_F(TerrainScatterTest, LimitsPlacementsInWorldCellsBeforeApplyingTheTotalLimit)
{
    m_terrainTransform.worldPosition = {-4, 0, -4};
    ASSERT_TRUE(Build());
    const auto unrestricted = m_placements;
    m_settings.maxPlacementsPerCell = 2;
    m_settings.maxInstances = 8;
    editor::TerrainPaintDiagnostics diagnostics;
    ASSERT_TRUE(editor::BuildTerrainScatter(m_terrain, m_terrainTransform, m_targetTransform, m_settings,
        m_placements, m_reason, nullptr, {}, &diagnostics)) << m_reason;
    ASSERT_EQ(m_placements.size(), 8u);
    std::map<scene::WorldCellId, size_t> cells;
    const scene::WorldCellGrid grid{{}, scene::VEGETATION_BUDGET_CELL_SIZE};
    for (const auto& placement : m_placements) {
        ASSERT_GT(placement.id, 0u);
        ASSERT_LE(placement.id, unrestricted.size());
        EXPECT_VEC3_NEAR(placement.position, unrestricted[placement.id - 1].position, 0.0f);
        EXPECT_VEC3_NEAR(placement.scale, unrestricted[placement.id - 1].scale, 0.0f);
        scene::WorldCellId cell;
        ASSERT_TRUE(scene::TryLocateWorldCell(grid, placement.position, cell));
        ++cells[cell];
    }
    ASSERT_EQ(cells.size(), 4u);
    for (const auto& [cell, count] : cells) EXPECT_EQ(count, 2u);
    EXPECT_EQ(diagnostics.counts[static_cast<size_t>(editor::TerrainPaintReason::CELL_BUDGET)], 8u);
    const auto previous = m_placements;
    m_settings.maxInstances = 7;
    EXPECT_FALSE(Build());
    ASSERT_EQ(m_placements.size(), previous.size());
    for (size_t index = 0; index < previous.size(); ++index) EXPECT_EQ(m_placements[index].id, previous[index].id);
}

TEST_F(TerrainScatterTest, MinimumSeparationAndWorldFilterApplyToExistingAndNewPoints)
{
    m_settings.spacing = 0.5f; m_settings.minimumDistance = 1.5f;
    const std::array<math::Vector3, 1> centers{{{4, 0, 4}}};
    const editor::TerrainScatterBrush brush{centers, 10, false};
    m_placements = {{701, {6, 0, 4}, {}, {1, 1, 1}, true}};
    ASSERT_TRUE(editor::BuildTerrainScatter(m_terrain, m_terrainTransform, m_targetTransform, m_settings,
        m_placements, m_reason, &brush, [](const auto& point) { return point.x > 4; })) << m_reason;
    ASSERT_GT(m_placements.size(), 1u);
    for (size_t i = 0; i < m_placements.size(); ++i) {
        EXPECT_GT(m_placements[i].position.x, 4);
        for (size_t j = 0; j < i; ++j)
            EXPECT_GE((m_placements[i].position - m_placements[j].position).Length(), 1.5f - 0.00001f);
    }
    const auto before = m_placements;
    m_settings.minimumDistance = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(editor::BuildTerrainScatter(m_terrain, m_terrainTransform, m_targetTransform, m_settings, m_placements, m_reason, &brush));
    ASSERT_EQ(m_placements.size(), before.size()); EXPECT_EQ(m_placements[0].id, 701u);
}

TEST_F(TerrainScatterTest, PolygonFillHandlesConcavityAndRejectsCrossingsWithoutChangingPlacements)
{
    const std::array<math::Vector3,6> polygon{{{0,0,0},{8,0,0},{8,0,3},{3,0,3},{3,0,8},{0,0,8}}};
    editor::TerrainScatterBrush brush{polygon,2,false,true,0};
    ASSERT_TRUE(editor::ValidateTerrainRegion(m_terrainTransform,brush));
    ASSERT_TRUE(editor::BuildTerrainScatter(m_terrain,m_terrainTransform,m_targetTransform,m_settings,m_placements,m_reason,&brush));
    ASSERT_FALSE(m_placements.empty());
    for(const auto& p:m_placements)EXPECT_TRUE(p.position.x<3||p.position.z<3);
    const auto before=m_placements;
    const std::array<math::Vector3,4> crossed{{{0,0,0},{8,0,8},{0,0,8},{8,0,0}}};brush.centers=crossed;
    EXPECT_FALSE(editor::BuildTerrainScatter(m_terrain,m_terrainTransform,m_targetTransform,m_settings,m_placements,m_reason,&brush));
    EXPECT_EQ(m_placements.size(),before.size());
}

TEST_F(TerrainScatterTest, SoftBoundaryIsDeterministicAndDiagnosticsAccountForEveryCandidate)
{
    m_settings.spacing=0.5f;
    const std::array<math::Vector3,1> center{{{4,0,4}}};editor::TerrainScatterBrush brush{center,4,false,false,0};
    editor::TerrainPaintDiagnostics hard;
    ASSERT_TRUE(editor::BuildTerrainScatter(m_terrain,m_terrainTransform,m_targetTransform,m_settings,m_placements,m_reason,&brush,{},&hard));
    const auto before=m_placements; m_placements.clear();brush.feather=0.8f;editor::TerrainPaintDiagnostics soft;
    ASSERT_TRUE(editor::BuildTerrainScatter(m_terrain,m_terrainTransform,m_targetTransform,m_settings,m_placements,m_reason,&brush,{},&soft));
    EXPECT_LT(m_placements.size(),before.size());EXPECT_GT(m_placements.size(),0u);
    EXPECT_EQ(soft.counts[static_cast<size_t>(editor::TerrainPaintReason::ACCEPTED)],m_placements.size());
    size_t visits=0;for(size_t count:soft.counts)visits+=count;
    EXPECT_EQ(visits,before.size());EXPECT_GT(soft.counts[static_cast<size_t>(editor::TerrainPaintReason::DENSITY)],0u);
    const auto repeated=m_placements;m_placements.clear();
    ASSERT_TRUE(editor::BuildTerrainScatter(m_terrain,m_terrainTransform,m_targetTransform,m_settings,m_placements,m_reason,&brush));
    ASSERT_EQ(repeated.size(),m_placements.size());for(size_t n=0;n<repeated.size();++n)EXPECT_EQ(repeated[n].id,m_placements[n].id);
}

TEST_F(TerrainScatterTest, LongEraseStrokeMatchesDirectCoverageAcrossCells)
{
    m_terrainTransform.worldPosition = {12.0f, 3.0f, -18.0f};
    m_terrainTransform.worldRotation = math::Quaternion::FromAxisAngle(math::Vector3::UP, 0.8f);
    m_targetTransform.worldPosition = {-4.0f, 0.0f, 9.0f};
    const auto terrainWorld = m_terrainTransform.GetWorldMatrix();
    const auto targetWorld = m_targetTransform.GetWorldMatrix();
    std::array<math::Vector3, 40> centers;
    for (size_t i = 0; i < centers.size(); ++i) {
        const auto world = terrainWorld * math::Vector4{static_cast<float>(i) * 3.0f - 40.0f, 0,
            static_cast<float>(i % 3) * 3.0f - 5.0f, 1};
        centers[i] = {world.x, world.y, world.z};
    }
    for (int z = -30; z < 30; ++z) for (int x = -60; x < 120; ++x)
        m_placements.push_back({static_cast<uint32_t>(m_placements.size() + 1),
            {static_cast<float>(x), 0, static_cast<float>(z)}, {}, {1, 1, 1}, true});
    const editor::TerrainScatterBrush brush{centers, 5.0f, true, false, 0.65f};
    std::vector<uint32_t> expected;
    for (const auto& placement : m_placements) {
        const auto world = targetWorld * math::Vector4{placement.position, 1.0f};
        if (editor::TerrainRegionCoverage(m_terrainTransform, brush, {world.x, world.y, world.z}) <= 0.0f)
            expected.push_back(placement.id);
    }
    ASSERT_LT(expected.size(), m_placements.size());
    ASSERT_TRUE(editor::BuildTerrainScatter(m_terrain, m_terrainTransform, m_targetTransform, m_settings,
        m_placements, m_reason, &brush)) << m_reason;
    std::vector<uint32_t> actual;
    for (const auto& placement : m_placements) actual.push_back(placement.id);
    EXPECT_EQ(actual, expected);
}

TEST_F(TerrainScatterTest, HolesHeightSlopeAndSavedLayerRestrictTheSameSurface)
{
    ASSERT_TRUE(m_terrain.SetHoleCell(0, 0, true));
    ASSERT_TRUE(Build()); EXPECT_EQ(m_placements.size(), 15u);
    for (const auto& placement : m_placements) EXPECT_FALSE(m_terrain.IsHoleAtLocal(placement.position.x, placement.position.z));
    m_settings.layer = 1; ASSERT_TRUE(Build()); EXPECT_TRUE(m_placements.empty());
    m_settings.layer = 0; ASSERT_TRUE(Build()); EXPECT_EQ(m_placements.size(), 15u);
    m_settings.minHeight = 1; ASSERT_TRUE(Build()); EXPECT_TRUE(m_placements.empty());
    m_settings.minHeight = -100; m_settings.minSlope = 5; ASSERT_TRUE(Build()); EXPECT_TRUE(m_placements.empty());
    m_settings.minSlope = 0; m_settings.maxSlope = 10;
    for (int z = 0; z < m_terrain.rows; ++z) for (int x = 0; x < m_terrain.columns; ++x)
        m_terrain.heightData[static_cast<size_t>(z) * m_terrain.columns + x] = static_cast<float>(x) * 0.2f;
    ASSERT_TRUE(Build()); EXPECT_TRUE(m_placements.empty());
    m_settings.maxSlope = 50; ASSERT_TRUE(Build()); EXPECT_EQ(m_placements.size(), 15u);
}

TEST_F(TerrainScatterTest, WorldSamplingAndNormalsAgreeAfterRotatedNegativeNonuniformTerrainTransform)
{
    for (int z = 0; z < m_terrain.rows; ++z) for (int x = 0; x < m_terrain.columns; ++x)
        m_terrain.heightData[static_cast<size_t>(z) * m_terrain.columns + x] = static_cast<float>(x) * 0.02f + static_cast<float>(z) * 0.01f;
    m_terrainTransform.worldScale = {-2.0f, 1.5f, 3.0f};
    m_terrainTransform.worldRotation = math::Quaternion::FromEuler({0.12f, 0.4f, -0.1f});
    m_terrainTransform.worldPosition = {20, 10, -4};
    m_targetTransform.worldPosition = {-5, 3, 2}; m_targetTransform.worldScale = {2, 2, 2};
    m_targetTransform.worldRotation = math::Quaternion::FromEuler({0.1f, -0.3f, 0.2f});
    m_settings.alignToNormal = true; m_settings.minScale = 1.4f; m_settings.maxScale = 1.4f;
    ASSERT_TRUE(Build()) << m_reason; ASSERT_FALSE(m_placements.empty());
    for (const auto& placement : m_placements) {
        const auto point = m_targetTransform.GetWorldMatrix() * math::Vector4{placement.position.x, placement.position.y, placement.position.z, 1};
        scene::TerrainSurfaceSample sample;
        ASSERT_TRUE(scene::TrySampleTerrainWorld(m_terrain, m_terrainTransform, point.x, point.z, sample));
        EXPECT_VEC3_NEAR(sample.position, math::Vector3(point.x, point.y, point.z), 0.0001f);
        const auto up = m_targetTransform.worldRotation * (math::Quaternion::FromEuler(placement.eulerRadians) * math::Vector3::UP);
        EXPECT_VEC3_NEAR(up, sample.normal, 0.0001f);
        EXPECT_NEAR(placement.scale.x * 2.0f, 1.4f, 0.00001f);
    }
}

TEST_F(TerrainScatterTest, DownslopeRotationAndEmbeddingUseTheWorldSurfaceWithoutChangingCandidateIds)
{
    for (int z = 0; z < m_terrain.rows; ++z) for (int x = 0; x < m_terrain.columns; ++x)
        m_terrain.heightData[static_cast<size_t>(z) * m_terrain.columns + x] = static_cast<float>(x) * 0.05f;
    m_terrainTransform.worldScale = {-1.5f, 2.0f, 1.0f};
    m_terrainTransform.worldPosition = {12, 6, -8};
    m_terrainTransform.worldRotation = math::Quaternion::FromEuler({0, 0.5f, 0});
    m_targetTransform.worldScale = {2, 2, 2};
    m_targetTransform.worldPosition = {3, 2, 1};
    m_targetTransform.worldRotation = math::Quaternion::FromEuler({0.12f, -0.3f, 0.2f});
    ASSERT_TRUE(Build()) << m_reason;
    ASSERT_FALSE(m_placements.empty());
    const auto original = m_placements;

    m_settings.alignDownslope = true;
    m_settings.embedDepth = 0.20f;
    ASSERT_TRUE(Build()) << m_reason;
    ASSERT_EQ(m_placements.size(), original.size());
    const auto world = m_targetTransform.GetWorldMatrix();
    for (size_t i = 0; i < original.size(); ++i) {
        const auto& before = original[i];
        const auto& after = m_placements[i];
        const auto surface = world * math::Vector4{before.position, 1};
        const auto center = world * math::Vector4{after.position, 1};
        scene::TerrainSurfaceSample sample;
        ASSERT_TRUE(scene::TrySampleTerrainWorld(m_terrain, m_terrainTransform, surface.x, surface.z, sample));
        const auto rotation = m_targetTransform.worldRotation * math::Quaternion::FromEuler(after.eulerRadians);
        const auto downhill = (sample.normal * sample.normal.y - math::Vector3::UP).Normalized();
        EXPECT_EQ(after.id, before.id);
        EXPECT_VEC3_NEAR(rotation * math::Vector3::UP, sample.normal, 0.0001f);
        EXPECT_VEC3_NEAR(rotation * math::Vector3::FORWARD, downhill, 0.0001f);
        EXPECT_VEC3_NEAR(math::Vector3(center.x, center.y, center.z), sample.position - sample.normal * 0.20f, 0.0001f);
    }
}

TEST_F(TerrainScatterTest, DownslopeOnFlatGroundUsesTheExistingYawRange)
{
    m_settings.minYaw = 42.0f;
    m_settings.maxYaw = 42.0f;
    ASSERT_TRUE(Build());
    const auto original = m_placements;
    m_settings.alignDownslope = true;
    m_settings.downslopeJitter = 15.0f;
    ASSERT_TRUE(Build());
    ASSERT_EQ(m_placements.size(), original.size());
    for (size_t i = 0; i < original.size(); ++i)
        EXPECT_VEC3_NEAR(m_placements[i].eulerRadians, original[i].eulerRadians, 0.0f);
}

TEST_F(TerrainScatterTest, InvalidInputsAndBothBudgetsPreservePreviousOutput)
{
    ASSERT_TRUE(Build()); const auto before = m_placements;
    m_settings.maxInstances = 2; EXPECT_FALSE(Build()); EXPECT_EQ(m_placements.size(), before.size());
    m_settings.maxInstances = 100000; m_settings.spacing = std::numeric_limits<float>::quiet_NaN(); EXPECT_FALSE(Build());
    m_settings.spacing = 2.0f; m_settings.embedDepth = -0.1f; EXPECT_FALSE(Build());
    m_settings.embedDepth = 0.0f; m_settings.downslopeJitter = 181.0f; EXPECT_FALSE(Build());
    m_settings.downslopeJitter = 0.0f;
    m_settings.spacing = 0.25f; m_terrainTransform.worldScale = {1000, 1, 1000}; EXPECT_FALSE(Build());
    m_terrainTransform.worldScale = {1, 1, 1}; m_targetTransform.worldScale = {1, 2, 1}; EXPECT_FALSE(Build());
    m_targetTransform.worldScale = {1, 1, 1}; m_terrain.heightData[0] = std::numeric_limits<float>::infinity(); EXPECT_FALSE(Build());
    ASSERT_EQ(m_placements.size(), before.size());
    for (size_t i = 0; i < before.size(); ++i) EXPECT_VEC3_NEAR(m_placements[i].position, before[i].position, 0.0f);
}

class TerrainScatterOperatorTest : public testkit::EditorFixture {
protected:
    void SetUp() override
    {
        EditorFixture::SetUp();
        editor::AssetDirtyRegistry::DiscardAll(); asset::DataAssetRegistry::ClearCache();
        Context().projectRoot = util::FileSystem::PathToUtf8(ProjectRoot());
        asset::AssetManager::Init(m_resources, util::FileSystem::PathToUtf8(ProjectRoot() / "Assets"));
        auto& scene = AttachScene();
        auto& terrainNode = scene.CreateGameObject("Mountain"); m_terrainId = terrainNode.instanceId;
        auto& terrain = terrainNode.AddComponent<scene::TerrainComponent>();
        terrain.Resize(5, 5); terrain.cellSize = 2; terrain.InitFlat();
        auto& target = scene.CreateGameObject("Trees"); m_targetId = target.instanceId;
        m_source = util::FileSystem::PathToUtf8(ProjectRoot() / "Assets" / "Template.fzdata");
        ASSERT_TRUE(asset::DataAssetRegistry::Create(m_source, asset::InstanceSetAsset::TYPE_NAME));
        auto* source = static_cast<asset::InstanceSetAsset*>(asset::DataAssetRegistry::Resolve(m_source));
        ASSERT_NE(source, nullptr);
        const std::array<asset::InstanceMeshPart, 1> parts{{{"primitive:cone", "guid:test-material", 0}}};
        const std::array<float, 1> lod{0};
        ASSERT_TRUE(source->Replace(parts, lod, {})); ASSERT_TRUE(asset::DataAssetRegistry::Save(m_source));
        m_sourceSnapshot = asset::DataAssetRegistry::Snapshot(m_source);
        Context().operators = &m_registry; Context().undoStack = &m_undo;
        editor::RegisterTerrainScatterOperators(m_registry);
    }
    void TearDown() override
    {
        m_undo.Clear(); editor::AssetDirtyRegistry::DiscardAll();
        asset::DataAssetRegistry::ClearCache(); asset::AssetManager::UnloadAll(); EditorFixture::TearDown();
    }
    scene::InstancedMeshComponent* Target()
    {
        return AttachedScene()->FindByGuid(m_targetId)->GetComponent<scene::InstancedMeshComponent>();
    }
    editor::OpResult Scatter(int cap = 100000)
    {
        editor::OpArgs args; args.Set("terrain", m_terrainId); args.Set("target", m_targetId); args.Set("source", m_source);
        args.Set("spacing", 2.0f); args.Set("maxInstances", cap);
        return editor::InvokeOperator(Context(), "terrain.scatter", args);
    }
    editor::OpArgs PaintArguments(std::span<const std::string> paths, const math::Vector3& center, float radius, bool erase = false)
    {
        auto sources = editor::OpData::MakeArray();
        for (const auto& path : paths) sources.Push(editor::OpData("guid:" + asset::AssetDatabase::GuidFromPath(path)));
        auto stroke = editor::OpData::MakeArray();
        stroke.Push(editor::OpData(center));
        editor::OpArgs args;
        args.Set("terrain", m_terrainId);
        args.Set("sources", std::move(sources));
        args.Set("stroke", std::move(stroke));
        args.Set("spacing", 2.0f);
        args.Set("brushRadius", radius);
        args.Set("erase", erase);
        return args;
    }
    editor::OpResult Paint(std::span<const std::string> paths, const math::Vector3& center, float radius, bool erase = false)
    {
        return editor::InvokeOperator(Context(), "terrain.paint_instances", PaintArguments(paths, center, radius, erase));
    }
    editor::OperatorRegistry m_registry;
    editor::UndoStack m_undo;
    std::string m_terrainId, m_targetId, m_source, m_sourceSnapshot;
    testkit::ResourceTrackingRenderer m_device;
    renderer::ResourceManager m_resources{m_device};
};

TEST_F(TerrainScatterOperatorTest, MixedBrushSharesCandidateDensityAndUndoAcrossSources)
{
    const auto rockPath = util::FileSystem::PathToUtf8(ProjectRoot() / "Assets" / "Rock.fzdata");
    ASSERT_TRUE(asset::DataAssetRegistry::Create(rockPath, asset::InstanceSetAsset::TYPE_NAME));
    auto* rock = static_cast<asset::InstanceSetAsset*>(asset::DataAssetRegistry::Resolve(rockPath));
    ASSERT_NE(rock, nullptr);
    const std::array<asset::InstanceMeshPart, 1> rockParts{{{"primitive:sphere", "guid:test-material", 0}}};
    const std::array<float, 1> lod{0};
    ASSERT_TRUE(rock->Replace(rockParts, lod, {}));
    ASSERT_TRUE(asset::DataAssetRegistry::Save(rockPath));

    const std::array sources{m_source, rockPath};
    auto previewArgs = PaintArguments(sources, {4, 0, 4}, 6.0f);
    auto weights = editor::OpData::MakeArray();
    weights.Push(editor::OpData(0));
    weights.Push(editor::OpData(100));
    previewArgs.Set("weights", std::move(weights));
    std::vector<editor::TerrainPaintPreviewPoint> preview;
    size_t count = 0;
    std::string reason;
    ASSERT_TRUE(editor::BuildTerrainPaintPreview(Context(), previewArgs, preview, count, reason)) << reason;
    ASSERT_EQ(count, 16u);
    ASSERT_EQ(preview.size(), count);
    EXPECT_TRUE(AttachedScene()->GetEntities<scene::InstancedMeshComponent>().empty());
    EXPECT_EQ(m_undo.GetHistorySize(), 0u);
    const auto weighted = editor::InvokeOperator(Context(), "terrain.paint_instances", previewArgs);
    ASSERT_TRUE(weighted.ok) << weighted.message;
    const auto weightedOwners = AttachedScene()->GetEntities<scene::InstancedMeshComponent>();
    ASSERT_EQ(weightedOwners.size(), 1u);
    EXPECT_EQ(AttachedScene()->GetGameObject(weightedOwners.front())->name, "Rock (Terrain Paint)");
    const auto weightedSnapshot = AttachedScene()->GetComponent<scene::InstancedMeshComponent>(weightedOwners.front())->Snapshot();
    ASSERT_TRUE(weightedSnapshot);
    ASSERT_EQ(weightedSnapshot->placements.size(), preview.size());
    for (size_t index = 0; index < preview.size(); ++index) {
        EXPECT_EQ(preview[index].sourceIndex, 1u);
        EXPECT_VEC3_NEAR(preview[index].position, weightedSnapshot->placements[index].position, 0.0001f);
    }
    ASSERT_TRUE(m_undo.Undo()) << m_undo.LastDiagnostic();

    const auto paint = Paint(sources, {4, 0, 4}, 6.0f);
    ASSERT_TRUE(paint.ok) << paint.message;
    const auto placements = [&]() {
        size_t count = 0;
        for (const auto id : AttachedScene()->GetEntities<scene::InstancedMeshComponent>()) {
            const auto snapshot = AttachedScene()->GetComponent<scene::InstancedMeshComponent>(id)->Snapshot();
            if (snapshot) count += snapshot->placements.size();
        }
        return count;
    };
    EXPECT_EQ(AttachedScene()->GetEntities<scene::InstancedMeshComponent>().size(), 2u);
    EXPECT_EQ(placements(), 16u);
    EXPECT_EQ(m_undo.GetHistorySize(), 1u);
    const std::array oneSource{rockPath};
    EXPECT_TRUE(Paint(oneSource, {4, 0, 4}, 6.0f).noChange);
    EXPECT_EQ(m_undo.GetHistorySize(), 1u);

    const auto erase = Paint(sources, {2, 0, 2}, 2.1f, true);
    ASSERT_TRUE(erase.ok) << erase.message;
    const auto remaining = placements();
    EXPECT_LT(remaining, 16u);
    EXPECT_GT(remaining, 0u);
    ASSERT_TRUE(m_undo.Undo()) << m_undo.LastDiagnostic();
    EXPECT_EQ(placements(), 16u);
    ASSERT_TRUE(m_undo.Undo()) << m_undo.LastDiagnostic();
    EXPECT_EQ(AttachedScene()->GetEntities<scene::InstancedMeshComponent>().size(), 0u);
    ASSERT_TRUE(m_undo.Redo()) << m_undo.LastDiagnostic();
    EXPECT_EQ(placements(), 16u);
    ASSERT_TRUE(m_undo.Redo()) << m_undo.LastDiagnostic();
    EXPECT_EQ(placements(), remaining);
    EXPECT_EQ(asset::DataAssetRegistry::Snapshot(m_source), m_sourceSnapshot);
}

TEST_F(TerrainScatterOperatorTest, EmptyTargetGetsIndependentSavedAssetAndUndoRedo)
{
    const auto result = Scatter(); ASSERT_TRUE(result.ok) << result.message;
    ASSERT_TRUE(Target()); ASSERT_TRUE(Target()->Snapshot()); EXPECT_EQ(Target()->Snapshot()->placements.size(), 16u);
    EXPECT_EQ(asset::DataAssetRegistry::Snapshot(m_source), m_sourceSnapshot);
    EXPECT_TRUE(asset::AssetDatabase::IsGuidRef(Target()->m_instanceSetRef.path));
    const auto reference = Target()->m_instanceSetRef.path;
    const auto disk = asset::AssetDatabase::PathFromGuid(asset::AssetDatabase::GuidFromRef(reference));
    ASSERT_FALSE(disk.empty()); EXPECT_TRUE(util::FileSystem::Exists(util::FileSystem::PathFromUtf8(disk)));
    ASSERT_TRUE(m_undo.Undo()); EXPECT_FALSE(Target()); ASSERT_TRUE(m_undo.Redo()); ASSERT_TRUE(Target());
    EXPECT_EQ(Target()->m_instanceSetRef.path, reference);
    EXPECT_EQ(util::FileSystem::GetExtension(disk), ".meshinstances");
    ASSERT_GT(asset::AssetManager::ReloadPath(disk), 0); ASSERT_TRUE(Target()->Snapshot());
    EXPECT_EQ(Target()->Snapshot()->placements.size(), 16u);
}

TEST_F(TerrainScatterOperatorTest, ScatterUsesTerrainAndTargetTransformsEditedInTheSameFrame)
{
    ASSERT_TRUE(Scatter().ok);
    const auto before = Target()->Snapshot();
    ASSERT_TRUE(before);
    AttachedScene()->FindByGuid(m_terrainId)->transform.position = {10, 3, -6};
    AttachedScene()->FindByGuid(m_targetId)->transform.position = {-5, 2, 1};
    ASSERT_TRUE(Scatter().ok);
    const auto after = Target()->Snapshot();
    ASSERT_TRUE(after);
    ASSERT_EQ(after->placements.size(), before->placements.size());
    const math::Vector3 displacement{15, 1, -7};
    for (size_t i = 0; i < after->placements.size(); ++i)
        EXPECT_VEC3_NEAR(after->placements[i].position, before->placements[i].position + displacement, 0.0001f);
}

TEST_F(TerrainScatterOperatorTest, SharedOriginalAndSiblingAreUnchangedAndRepeatedBakesRemainUndoable)
{
    auto& first = AttachedScene()->FindByGuid(m_targetId)->AddComponent<scene::InstancedMeshComponent>();
    first.m_instanceSetRef.path = m_source;
    auto& sibling = AttachedScene()->CreateGameObject("Other Trees"); const auto siblingId = sibling.instanceId;
    sibling.AddComponent<scene::InstancedMeshComponent>().m_instanceSetRef.path = m_source;
    const auto shared = first.Snapshot(); ASSERT_TRUE(shared);
    ASSERT_TRUE(Scatter().ok); const auto firstBake = Target()->m_instanceSetRef.path;
    EXPECT_EQ(AttachedScene()->FindByGuid(siblingId)->GetComponent<scene::InstancedMeshComponent>()->Snapshot(), shared);
    ASSERT_TRUE(Scatter().ok); EXPECT_NE(Target()->m_instanceSetRef.path, firstBake);
    ASSERT_TRUE(m_undo.Undo()); EXPECT_EQ(Target()->m_instanceSetRef.path, firstBake);
    ASSERT_TRUE(m_undo.Undo()); EXPECT_EQ(Target()->m_instanceSetRef.path, m_source);
    EXPECT_EQ(asset::DataAssetRegistry::Snapshot(m_source), m_sourceSnapshot);
}

TEST_F(TerrainScatterOperatorTest, FailedBakeCreatesNoComponentOrHistoryAndStaleUndoIsRejected)
{
    EXPECT_FALSE(Scatter(1).ok); EXPECT_FALSE(Target()); EXPECT_EQ(m_undo.GetHistorySize(), 0u);
    ASSERT_TRUE(Scatter().ok); Target()->m_enabled = false;
    EXPECT_FALSE(m_undo.Undo()); ASSERT_TRUE(Target()); EXPECT_FALSE(Target()->m_enabled);
    EXPECT_EQ(m_undo.GetCursor(), 1u);
}

TEST_F(TerrainScatterOperatorTest, SceneClearWithReusedGuidCannotReceiveOldRedo)
{
    ASSERT_TRUE(Scatter().ok); ASSERT_TRUE(m_undo.Undo());
    AttachedScene()->Clear();
    auto& replacement = AttachedScene()->CreateGameObject("Replacement"); replacement.instanceId = m_targetId;
    EXPECT_FALSE(m_undo.Redo()); EXPECT_FALSE(replacement.GetComponent<scene::InstancedMeshComponent>());
    EXPECT_EQ(m_undo.GetCursor(), 0u);
}

TEST_F(TerrainScatterOperatorTest, ExternalInstanceOverridePreventsUndoFromDiscardingTheEdit)
{
    ASSERT_TRUE(Scatter().ok); ASSERT_TRUE(Target());
    const auto id = Target()->Snapshot()->placements.front().id;
    ASSERT_TRUE(Target()->SetTransform(id, {100, 2, 3}, {}, {1, 1, 1}));
    const auto edited = Target()->Snapshot();
    EXPECT_FALSE(m_undo.Undo()); EXPECT_EQ(Target()->Snapshot(), edited);
    EXPECT_EQ(m_undo.GetCursor(), 1u);
}
} /// @note namespace fbzz::tests
