/// @file    TerrainAuthoringTests.cpp
/// @brief   標高・傾斜の塗り分けと高さ画像・マスクの編集履歴を検証する。
/// @author  Hasegawa Jin
/// @date    2026-11-04
#include <TestKit/TestKit.hpp>
#include <TestKit/Editor/EditorFixture.hpp>
#include <Editor/Op/OperatorGroups.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <Tools/TerrainAuthoring.hpp>
#include <Tools/TerrainBrush.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Components/WaterComponent.hpp>
#include <Engine/Scene/Components/WaterfallComponent.hpp>
#include <Engine/Scene/MeshBuilder.hpp>
#include <Engine/Scene/TerrainHeightMapLoader.hpp>
#include <Engine/Scene/TerrainAssetSerializer.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <fstream>
#include <limits>

namespace fbzz::tests {

class TerrainAuthoringTest : public testkit::EditorFixture {
protected:
    editor::OperatorRegistry m_registry;
    editor::UndoStack m_undo;
    void SetUp() override
    {
        EditorFixture::SetUp();
        (void)AttachScene();
        Context().operators = &m_registry;
        Context().undoStack = &m_undo;
        editor::RegisterTerrainOperators(m_registry);
    }
    void TearDown() override
    {
        m_undo.Clear();
        Context().operators = nullptr;
        Context().undoStack = nullptr;
        EditorFixture::TearDown();
    }
    scene::GameObject& MakeTerrain(const char* name = "Mountain")
    {
        auto& go = AttachedScene()->CreateGameObject(name);
        auto& terrain = go.AddComponent<scene::TerrainComponent>();
        terrain.columns = 3;
        terrain.rows = 3;
        terrain.cellSize = 10.0f;
        terrain.maxHeight = 100.0f;
        terrain.layerMaterials = {"grass", "rock", "snow"};
        terrain.InitFlat();
        Context().selectedEntities = {go.GetID()};
        return go;
    }
    scene::GameObject& MakeRiver(float upstreamHeight, float downstreamHeight, float depth, float flow = 1.0f)
    {
        auto& river = AttachedScene()->CreateGameObject("River");
        auto& water = river.AddComponent<scene::WaterComponent>();
        water.surfaceShape = scene::WaterSurfaceShape::SPLINE_RIVER;
        water.splineSamplesPerSegment = 1;
        water.splinePoints = {
            {1, {2.0f, upstreamHeight, 5.0f}, 2.0f, depth, flow},
            {2, {18.0f, downstreamHeight, 5.0f}, 2.0f, depth, flow}};
        return river;
    }
};

namespace {

const editor::OpData* FindNatureIssue(const editor::OpResult& result, std::string_view code)
{
    const auto* issues = result.data.Find("issues");
    if (issues) {
        for (const auto& row : issues->AsArray())
            if (row.Find("code")->AsString() == code) return &row;
    }
    return nullptr;
}

} /// @note namespace

TEST_F(TerrainAuthoringTest, RiverRulesAllowHighGroundAndFollowSignedFlow)
{
    auto& ground = MakeTerrain();
    ground.transform.position.y = 100.0f;
    auto& terrain = *ground.GetComponent<scene::TerrainComponent>();
    for (int z = 0; z < 3; ++z) {
        ASSERT_TRUE(terrain.SetHeightAtGrid(0, z, 9.25f));
        ASSERT_TRUE(terrain.SetHeightAtGrid(1, z, 8.0f));
        ASSERT_TRUE(terrain.SetHeightAtGrid(2, z, 6.75f));
    }
    auto& river = MakeRiver(11.0f, 9.0f, 2.0f);
    river.transform.position.y = 100.0f;
    auto& water = *river.GetComponent<scene::WaterComponent>();
    const auto points = water.splinePoints;
    const auto heights = terrain.heightData;
    bool dirty = false;
    Context().markSceneDirty = [&dirty] { dirty = true; };
    editor::OpArgs args;
    args.Set("node", river.instanceId);
    auto result = editor::InvokeOperator(Context(), "terrain.river.validate", args);
    ASSERT_TRUE(result.ok) << result.message;
    EXPECT_TRUE(result.data.Find("valid")->AsBool());
    EXPECT_GT(result.data.Find("sampleCount")->AsInt(), 2);
    EXPECT_EQ(water.splinePoints, points);
    EXPECT_EQ(terrain.heightData, heights);
    EXPECT_FALSE(dirty);
    EXPECT_EQ(m_undo.GetHistorySize(), 0u);
    Context().markSceneDirty = [] {};

    for (auto& point : water.splinePoints) point.flowSpeed = -1.0f;
    result = editor::InvokeOperator(Context(), "terrain.river.validate", args);
    ASSERT_TRUE(result.ok) << result.message;
    EXPECT_EQ(result.data.Find("flowDirection")->AsString(), "reverse");
    EXPECT_NE(FindNatureIssue(result, "UPHILL"), nullptr);
    water.splinePoints.front().flowSpeed = 1.0f;
    result = editor::InvokeOperator(Context(), "terrain.river.validate", args);
    ASSERT_TRUE(result.ok) << result.message;
    EXPECT_NE(FindNatureIssue(result, "MIXED_FLOW"), nullptr);
    EXPECT_EQ(FindNatureIssue(result, "UPHILL"), nullptr);
}

TEST_F(TerrainAuthoringTest, RiverRulesFindRidgeBetweenPointsAndReportMissingGround)
{
    auto& ground = MakeTerrain();
    auto& terrain = *ground.GetComponent<scene::TerrainComponent>();
    for (int z = 0; z < 3; ++z) ASSERT_TRUE(terrain.SetHeightAtGrid(1, z, 8.0f));
    auto& river = MakeRiver(4.0f, 4.0f, 1.0f);
    editor::OpArgs args;
    args.Set("node", river.instanceId);
    auto result = editor::InvokeOperator(Context(), "terrain.river.validate", args);
    ASSERT_TRUE(result.ok) << result.message;
    EXPECT_FALSE(result.data.Find("valid")->AsBool());
    const auto* buried = FindNatureIssue(result, "BURIED_SURFACE");
    ASSERT_NE(buried, nullptr);
    EXPECT_GT(buried->Find("position")->AsVec3().x, 2.0f);
    EXPECT_LT(buried->Find("position")->AsVec3().x, 18.0f);
    EXPECT_GT(buried->Find("differenceMeters")->AsDouble(), 0.1);
    EXPECT_NE(FindNatureIssue(result, "FLOATING_BED"), nullptr);

    ASSERT_TRUE(terrain.SetHoleCell(0, 0, true));
    result = editor::InvokeOperator(Context(), "terrain.river.validate", args);
    ASSERT_TRUE(result.ok) << result.message;
    EXPECT_NE(FindNatureIssue(result, "MISSING_TERRAIN"), nullptr);
}

TEST_F(TerrainAuthoringTest, RiverRulesCheckOutletFootprintAndHeightTolerance)
{
    MakeTerrain();
    const auto riverId = MakeRiver(1.0f, 1.0f, 1.0f).instanceId;
    auto& outlet = AttachedScene()->CreateGameObject("Lake");
    outlet.transform.position = {18.0f, 1.0f, 5.0f};
    auto& water = outlet.AddComponent<scene::WaterComponent>();
    water.extentX = water.extentZ = 4.0f;
    editor::OpArgs args;
    args.Set("node", riverId);
    args.Set("outlet", outlet.instanceId);
    auto result = editor::InvokeOperator(Context(), "terrain.river.validate", args);
    ASSERT_TRUE(result.ok) << result.message;
    EXPECT_TRUE(result.data.Find("valid")->AsBool());

    outlet.transform.position.y = 3.0f;
    result = editor::InvokeOperator(Context(), "terrain.river.validate", args);
    ASSERT_TRUE(result.ok) << result.message;
    const auto* mismatch = FindNatureIssue(result, "OUTLET_HEIGHT");
    ASSERT_NE(mismatch, nullptr);
    EXPECT_DOUBLE_EQ(mismatch->Find("differenceMeters")->AsDouble(), -2.0);
    args.Set("maxOutletHeightDifference", 2.0f);
    result = editor::InvokeOperator(Context(), "terrain.river.validate", args);
    ASSERT_TRUE(result.ok) << result.message;
    EXPECT_TRUE(result.data.Find("valid")->AsBool());

    outlet.transform.position.x = 40.0f;
    result = editor::InvokeOperator(Context(), "terrain.river.validate", args);
    ASSERT_TRUE(result.ok) << result.message;
    EXPECT_NE(FindNatureIssue(result, "OUTLET_OUTSIDE"), nullptr);
}

TEST_F(TerrainAuthoringTest, RiverRulesMeasureDropWidthAndDepthPerWorldMeter)
{
    MakeTerrain();
    auto& river = MakeRiver(8.0f, 1.0f, 1.0f);
    auto& downstream = river.GetComponent<scene::WaterComponent>()->splinePoints.back();
    downstream.width = 10.0f;
    downstream.depth = 7.0f;
    editor::OpArgs args;
    args.Set("node", river.instanceId);
    auto result = editor::InvokeOperator(Context(), "terrain.river.validate", args);
    ASSERT_TRUE(result.ok) << result.message;
    const auto* drop = FindNatureIssue(result, "STEEP_DROP");
    const auto* width = FindNatureIssue(result, "ABRUPT_WIDTH");
    const auto* depth = FindNatureIssue(result, "ABRUPT_DEPTH");
    ASSERT_NE(drop, nullptr);
    ASSERT_NE(width, nullptr);
    ASSERT_NE(depth, nullptr);
    EXPECT_NEAR(drop->Find("rate")->AsDouble(), 7.0 / 16.0, 1.0e-5);
    EXPECT_NEAR(width->Find("rate")->AsDouble(), 8.0 / 16.0, 1.0e-5);
    EXPECT_NEAR(depth->Find("rate")->AsDouble(), 6.0 / 16.0, 1.0e-5);

    args.Set("maxDownhillGrade", 0.0f);
    args.Set("maxWidthChangeRate", 0.0f);
    args.Set("maxDepthChangeRate", 0.0f);
    result = editor::InvokeOperator(Context(), "terrain.river.validate", args);
    ASSERT_TRUE(result.ok) << result.message;
    EXPECT_EQ(FindNatureIssue(result, "STEEP_DROP"), nullptr);
    EXPECT_EQ(FindNatureIssue(result, "ABRUPT_WIDTH"), nullptr);
    EXPECT_EQ(FindNatureIssue(result, "ABRUPT_DEPTH"), nullptr);
}

TEST_F(TerrainAuthoringTest, RiverBankRuleFindsOnlyTheLowSideOfTheValley)
{
    auto& terrain = *MakeTerrain().GetComponent<scene::TerrainComponent>();
    terrain.Resize(5, 3);
    terrain.cellSize = 5.0f;
    for (int x = 0; x < terrain.columns; ++x) {
        ASSERT_TRUE(terrain.SetHeightAtGrid(x, 0, 3.0f));
        ASSERT_TRUE(terrain.SetHeightAtGrid(x, 2, 3.0f));
    }
    editor::OpArgs args;
    args.Set("node", MakeRiver(1.0f, 1.0f, 1.0f).instanceId);
    args.Set("minBankRise", 1.0f);
    auto result = editor::InvokeOperator(Context(), "terrain.river.validate", args);
    ASSERT_TRUE(result.ok) << result.message;
    EXPECT_TRUE(result.data.Find("valid")->AsBool());

    for (int x = 0; x < terrain.columns; ++x) ASSERT_TRUE(terrain.SetHeightAtGrid(x, 0, 0.0f));
    result = editor::InvokeOperator(Context(), "terrain.river.validate", args);
    ASSERT_TRUE(result.ok) << result.message;
    const auto* bank = FindNatureIssue(result, "LOW_BANK");
    ASSERT_NE(bank, nullptr);
    EXPECT_FLOAT_EQ(bank->Find("position")->AsVec3().z, 2.0f);
    EXPECT_EQ(result.data.Find("issueCount")->AsInt(), result.data.Find("sampleCount")->AsInt());
}

TEST_F(TerrainAuthoringTest, ReliefRulesMeasureScaledSpikesAndPitsWithoutTreatingHolesAsTerrain)
{
    auto& ground = MakeTerrain();
    ground.transform.scale.y = 2.0f;
    auto& terrain = *ground.GetComponent<scene::TerrainComponent>();
    ASSERT_TRUE(terrain.SetHeightAtGrid(1, 1, 12.0f));
    auto result = editor::InvokeOperator(Context(), "terrain.relief.validate");
    ASSERT_TRUE(result.ok) << result.message;
    const auto* spike = FindNatureIssue(result, "ISOLATED_SPIKE");
    ASSERT_NE(spike, nullptr);
    EXPECT_NEAR(spike->Find("differenceMeters")->AsDouble(), 24.0, 1.0e-5);
    EXPECT_NE(FindNatureIssue(result, "STEEP_TERRAIN"), nullptr);

    ASSERT_TRUE(terrain.SetHeightAtGrid(1, 1, -12.0f));
    result = editor::InvokeOperator(Context(), "terrain.relief.validate");
    ASSERT_TRUE(result.ok) << result.message;
    EXPECT_NE(FindNatureIssue(result, "ISOLATED_PIT"), nullptr);
    ASSERT_TRUE(terrain.SetHoleCell(0, 0, true));
    result = editor::InvokeOperator(Context(), "terrain.relief.validate");
    ASSERT_TRUE(result.ok) << result.message;
    EXPECT_EQ(result.data.Find("checkedCells")->AsInt(), 3);
    EXPECT_EQ(result.data.Find("checkedVertices")->AsInt(), 0);
    EXPECT_EQ(FindNatureIssue(result, "ISOLATED_PIT"), nullptr);
    EXPECT_EQ(m_undo.GetHistorySize(), 0u);
}

TEST_F(TerrainAuthoringTest, SeamRuleChecksExtraFineVerticesAndReportsWorldHeightDifference)
{
    auto& first = MakeTerrain("West");
    auto& second = MakeTerrain("East");
    second.transform.position.x = 20.0f;
    auto& neighbor = *second.GetComponent<scene::TerrainComponent>();
    neighbor.Resize(5, 5);
    neighbor.cellSize = 5.0f;
    editor::OpArgs args;
    args.Set("node", first.instanceId);
    args.Set("neighbor", second.instanceId);
    auto result = editor::InvokeOperator(Context(), "terrain.seam.validate", args);
    ASSERT_TRUE(result.ok) << result.message;
    EXPECT_TRUE(result.data.Find("valid")->AsBool());
    EXPECT_EQ(result.data.Find("sampleCount")->AsInt(), 5);
    ASSERT_TRUE(neighbor.SetHeightAtGrid(0, 1, 3.0f));
    result = editor::InvokeOperator(Context(), "terrain.seam.validate", args);
    ASSERT_TRUE(result.ok) << result.message;
    EXPECT_FALSE(result.data.Find("valid")->AsBool());
    EXPECT_NEAR(result.data.Find("maxDifferenceMeters")->AsDouble(), 3.0, 1.0e-5);
    const auto* issue = FindNatureIssue(result, "TERRAIN_SEAM_STEP");
    ASSERT_NE(issue, nullptr);
    EXPECT_VEC3_NEAR(issue->Find("position")->AsVec3(), (math::Vector3{20.0f, 0.0f, 5.0f}), 0.00001f);
    second.transform.position.x = 20.1f;
    result = editor::InvokeOperator(Context(), "terrain.seam.validate", args);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.errorCode, "NO_SHARED_EDGE");
    EXPECT_EQ(m_undo.GetHistorySize(), 0u);
}

TEST_F(TerrainAuthoringTest, SeamRuleDetectsAReversedSlopeEvenWhenAllEdgeHeightsMatch)
{
    auto& first = MakeTerrain("West");
    auto& second = MakeTerrain("East");
    second.transform.position.x = 20.0f;
    auto& west = *first.GetComponent<scene::TerrainComponent>();
    auto& east = *second.GetComponent<scene::TerrainComponent>();
    for (int z = 0; z < 3; ++z) {
        ASSERT_TRUE(west.SetHeightAtGrid(1, z, -10.0f));
        ASSERT_TRUE(east.SetHeightAtGrid(1, z, 10.0f));
    }
    editor::OpArgs args;
    args.Set("node", first.instanceId);
    args.Set("neighbor", second.instanceId);
    args.Set("maxCrossSlopeDifference", 12.0f);
    auto result = editor::InvokeOperator(Context(), "terrain.seam.validate", args);
    ASSERT_TRUE(result.ok) << result.message;
    EXPECT_TRUE(result.data.Find("valid")->AsBool());
    EXPECT_EQ(result.data.Find("slopeSampleCount")->AsInt(), 2);
    EXPECT_DOUBLE_EQ(result.data.Find("maxCrossSlopeDifferenceDegrees")->AsDouble(), 0.0);

    ASSERT_TRUE(east.SetHeightAtGrid(1, 0, -10.0f));
    result = editor::InvokeOperator(Context(), "terrain.seam.validate", args);
    ASSERT_TRUE(result.ok) << result.message;
    EXPECT_FALSE(result.data.Find("valid")->AsBool());
    EXPECT_DOUBLE_EQ(result.data.Find("maxDifferenceMeters")->AsDouble(), 0.0);
    EXPECT_NEAR(result.data.Find("maxCrossSlopeDifferenceDegrees")->AsDouble(), 90.0, 0.00001);
    EXPECT_EQ(result.data.Find("issueCount")->AsInt(), 1);
    const auto* issue = FindNatureIssue(result, "TERRAIN_SEAM_SLOPE");
    ASSERT_NE(issue, nullptr);
    EXPECT_FLOAT_EQ(issue->Find("position")->AsVec3().z, 5.0f);
    args.Set("maxCrossSlopeDifference", 0.0f);
    result = editor::InvokeOperator(Context(), "terrain.seam.validate", args);
    ASSERT_TRUE(result.ok) << result.message;
    EXPECT_TRUE(result.data.Find("valid")->AsBool());
}

TEST_F(TerrainAuthoringTest, SeamBlendChangesOnlyTheSelectedInteriorAndUndoesInOneStep)
{
    auto& first = MakeTerrain("West");
    auto& second = MakeTerrain("East");
    auto& west = *first.GetComponent<scene::TerrainComponent>();
    auto& east = *second.GetComponent<scene::TerrainComponent>();
    west.Resize(5, 5);
    east.Resize(5, 5);
    west.cellSize = east.cellSize = 10.0f;
    second.transform.position.x = 40.0f;
    for (int z = 0; z < 5; ++z) {
        ASSERT_TRUE(west.SetHeightAtGrid(3, z, -4.0f));
        ASSERT_TRUE(east.SetHeightAtGrid(1, z, -4.0f));
    }
    const auto before = west.heightData, reference = east.heightData;
    editor::OpArgs args;
    args.Set("node", first.instanceId);
    args.Set("neighbor", second.instanceId);
    args.Set("maxCrossSlopeDifference", 12.0f);
    auto check = editor::InvokeOperator(Context(), "terrain.seam.validate", args);
    ASSERT_TRUE(check.ok) << check.message;
    EXPECT_EQ(check.data.Find("issueCount")->AsInt(), 4);
    editor::OpArgs blend;
    blend.Set("node", first.instanceId);
    blend.Set("neighbor", second.instanceId);
    blend.Set("blendRows", 2);
    auto result = editor::InvokeOperator(Context(), "terrain.seam.blend", blend);
    ASSERT_TRUE(result.ok) << result.message;
    EXPECT_EQ(result.data.Find("changedVertices")->AsInt(), 6);
    EXPECT_NEAR(west.GetHeightAt(30.0f, 20.0f), 4.0f, 0.00001f);
    EXPECT_NEAR(west.GetHeightAt(20.0f, 20.0f), 4.0f, 0.00001f);
    EXPECT_EQ(east.heightData, reference);
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(west.heightData[i], before[i]);
        EXPECT_EQ(west.heightData[20 + i], before[20 + i]);
        EXPECT_EQ(west.heightData[i * 5], before[i * 5]);
        EXPECT_EQ(west.heightData[i * 5 + 4], before[i * 5 + 4]);
    }
    check = editor::InvokeOperator(Context(), "terrain.seam.validate", args);
    ASSERT_TRUE(check.ok) << check.message;
    EXPECT_EQ(check.data.Find("issueCount")->AsInt(), 1);
    EXPECT_EQ(m_undo.GetHistorySize(), 1u);
    ASSERT_TRUE(m_undo.Undo());
    EXPECT_EQ(west.heightData, before);
    EXPECT_EQ(east.heightData, reference);
}

TEST_F(TerrainAuthoringTest, SculptRidgeUsesDifferentFlanksAndPreservesTileEdgesThroughUndo)
{
    auto& owner = MakeTerrain();
    auto& terrain = *owner.GetComponent<scene::TerrainComponent>();
    terrain.Resize(9, 9);
    terrain.cellSize = 5.0f;
    const auto before = terrain.heightData;
    editor::OpArgs args;
    args.Set("start", math::Vector3{0,0,20});
    args.Set("end", math::Vector3{40,0,20});
    args.Set("height", 20.0f);
    args.Set("leftWidth", 15.0f);
    args.Set("rightWidth", 5.0f);
    args.Set("peakAlong", 0.5f);
    const auto result = editor::InvokeOperator(Context(), "terrain.relief.sculpt", args);
    ASSERT_TRUE(result.ok) << result.message;
    EXPECT_FLOAT_EQ(terrain.GetHeightAt(20.0f, 20.0f), 20.0f);
    EXPECT_GT(terrain.GetHeightAt(20.0f, 25.0f), 0.0f);
    EXPECT_FLOAT_EQ(terrain.GetHeightAt(20.0f, 15.0f), 0.0f);
    EXPECT_FLOAT_EQ(terrain.GetHeightAt(0.0f, 25.0f), 0.0f);
    ASSERT_TRUE(m_undo.Undo());
    EXPECT_EQ(terrain.heightData, before);
    ASSERT_TRUE(m_undo.Redo());
    EXPECT_FLOAT_EQ(terrain.GetHeightAt(20.0f, 20.0f), 20.0f);
}

TEST_F(TerrainAuthoringTest, FitLakeShoreFollowsConcavityAndRejectsAnOpenSearch)
{
    editor::RegisterControlPointOperators(m_registry);
    auto& ground = MakeTerrain();
    auto& terrain = *ground.GetComponent<scene::TerrainComponent>();
    terrain.Resize(7, 7);
    terrain.cellSize = 2.0f;
    for (int z = 0; z < 7; ++z) {
        for (int x = 0; x < 7; ++x) {
            const bool wet = x > 0 && x < 6 && z > 0 && z < 6 && (x < 4 || z > 2);
            ASSERT_TRUE(terrain.SetHeightAtGrid(x, z, wet ? -4.0f : 4.0f));
        }
    }
    auto& lake = AttachedScene()->CreateGameObject("Lake");
    lake.transform.position = {6,0,6};
    auto& water = lake.AddComponent<scene::WaterComponent>();
    editor::OpArgs args;
    args.Set("node", lake.instanceId);
    args.Set("terrain", ground.instanceId);
    args.Set("width", 4.0f);
    args.Set("depth", 4.0f);
    args.Set("spacing", 2.0f);
    auto result = editor::InvokeOperator(Context(), "control_points.fit_lake_shore", args);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.errorCode, "SHORE_NOT_FOUND");
    EXPECT_EQ(water.surfaceShape, scene::WaterSurfaceShape::RECTANGLE);
    EXPECT_EQ(m_undo.GetHistorySize(), 0u);
    args.Set("width", 12.0f);
    args.Set("depth", 12.0f);
    args.Set("tolerance", 0.1f);
    result = editor::InvokeOperator(Context(), "control_points.fit_lake_shore", args);
    ASSERT_TRUE(result.ok) << result.message;
    ASSERT_EQ(water.surfaceShape, scene::WaterSurfaceShape::SPLINE_LAKE);
    ASSERT_EQ(water.splineSamplesPerSegment, 1u);
    scene::WaterSplineSnapshot mesh;
    ASSERT_TRUE(scene::BuildWaterSplineSnapshot(water.surfaceShape, water.splinePoints,
        water.splineSamplesPerSegment, lake.transform, mesh, water.buoyancyDepth));
    scene::WaterSplineSample sample;
    EXPECT_TRUE(scene::TrySampleWaterSpline(mesh, 6.0f, 6.0f, sample));
    EXPECT_FLOAT_EQ(sample.depth, water.buoyancyDepth);
    EXPECT_FALSE(scene::TrySampleWaterSpline(mesh, 10.0f, 2.0f, sample));
    ASSERT_TRUE(m_undo.Undo());
    EXPECT_EQ(water.surfaceShape, scene::WaterSurfaceShape::RECTANGLE);
    EXPECT_EQ(water.splineSamplesPerSegment, 8u);
}

TEST_F(TerrainAuthoringTest, MassifAddsSeededBranchReliefAndPreservesHoleVertices)
{
    auto& ground = MakeTerrain();
    auto& terrain = *ground.GetComponent<scene::TerrainComponent>();
    terrain.Resize(81, 81);
    terrain.cellSize = 1.0f;
    ASSERT_TRUE(terrain.SetHoleCell(38, 40, true));
    const auto before = terrain.heightData;
    editor::OpArgs args;
    args.Set("node", ground.instanceId);
    args.Set("start", math::Vector3{10, 0, 40});
    args.Set("end", math::Vector3{70, 0, 40});
    args.Set("height", 40.0f);
    args.Set("leftWidth", 14.0f);
    args.Set("rightWidth", 9.0f);
    args.Set("seed", 73);
    const auto result = editor::InvokeOperator(Context(), "terrain.relief.massif", args);
    ASSERT_TRUE(result.ok) << result.message;
    EXPECT_GT(terrain.GetHeightAt(43, 40), 30.0f);
    EXPECT_EQ(terrain.heightData[static_cast<std::size_t>(40) * terrain.columns + 38],
        before[static_cast<std::size_t>(40) * terrain.columns + 38]);
    EXPECT_EQ(terrain.heightData[0], before[0]);
    EXPECT_TRUE(terrain.IsHoleCell(38, 40));
    const auto shaped = terrain.heightData;
    ASSERT_TRUE(m_undo.Undo());
    EXPECT_EQ(terrain.heightData, before);
    const auto repeated = editor::InvokeOperator(Context(), "terrain.relief.massif", args);
    ASSERT_TRUE(repeated.ok) << repeated.message;
    EXPECT_EQ(terrain.heightData, shaped);
}

TEST_F(TerrainAuthoringTest, RiverCarvePreviewsDepthAndPreservesProtectedGroundThroughUndo)
{
    auto& ground = MakeTerrain();
    auto& terrain = *ground.GetComponent<scene::TerrainComponent>();
    terrain.Resize(21, 13);
    terrain.cellSize = 1.0f;
    for (int z = 0; z < terrain.rows; ++z)
        for (int x = 0; x < terrain.columns; ++x)
            ASSERT_TRUE(terrain.SetHeightAtGrid(x, z, 5.0f));
    ASSERT_TRUE(terrain.SetHoleCell(11, 5, true));
    ASSERT_TRUE(terrain.PaintLayerAtGrid(9, 7, 2, 1.0f));
    auto& river = MakeRiver(4.0f, 3.0f, 2.0f);
    auto& water = *river.GetComponent<scene::WaterComponent>();
    for (auto& point : water.splinePoints) point.width = 4.0f;
    const auto before = terrain.heightData;
    const auto beforeSplat = terrain.splatWeights;
    editor::OpArgs args;
    args.Set("node", river.instanceId);
    args.Set("terrain", ground.instanceId);
    args.Set("endFade", 2.0f);
    args.Set("bankWidth", 2.0f);
    args.Set("maxCut", 2.0f);
    args.Set("bankLayer", 1);
    args.Set("protectedLayer", 2);
    const auto preview = editor::InvokeOperator(Context(), "terrain.river.carve.preview", args);
    ASSERT_TRUE(preview.ok) << preview.message;
    EXPECT_FALSE(preview.data.Find("withinLimit")->AsBool());
    EXPECT_GT(preview.data.Find("protectedVertices")->AsInt64(), 0);
    EXPECT_GT(preview.data.Find("paintedVertices")->AsInt64(), 0);
    EXPECT_EQ(terrain.heightData, before);
    EXPECT_EQ(terrain.splatWeights, beforeSplat);
    const auto rejected = editor::InvokeOperator(Context(), "terrain.river.carve", args);
    EXPECT_EQ(rejected.errorCode, "RIVER_CUT_LIMIT");
    EXPECT_EQ(m_undo.GetHistorySize(), 0u);
    EXPECT_EQ(terrain.heightData, before);

    args.Set("maxCut", 12.0f);
    const auto carved = editor::InvokeOperator(Context(), "terrain.river.carve", args);
    ASSERT_TRUE(carved.ok) << carved.message;
    EXPECT_NEAR(terrain.GetHeightAt(10, 5), 1.5f, 0.0001f);
    EXPECT_NEAR(terrain.GetHeightAt(10, 7), 3.5f, 0.0001f);
    EXPECT_NEAR(terrain.GetHeightAt(2, 5), 5.0f, 0.0001f);
    EXPECT_NEAR(terrain.GetHeightAt(11, 5), 5.0f, 0.0001f);
    EXPECT_NEAR(terrain.GetHeightAt(9, 7), 5.0f, 0.0001f);
    EXPECT_FLOAT_EQ(terrain.GetLayerWeightAtGrid(9, 7, 2), 1.0f);
    EXPECT_GT(terrain.GetLayerWeightAtGrid(10, 7, 1), 0.9f);
    EXPECT_TRUE(terrain.IsHoleCell(11, 5));
    const auto repeated = editor::InvokeOperator(Context(), "terrain.river.carve", args);
    EXPECT_TRUE(repeated.ok);
    EXPECT_TRUE(repeated.noChange);
    ASSERT_EQ(m_undo.GetHistorySize(), 1u);
    ASSERT_TRUE(m_undo.Undo());
    EXPECT_EQ(terrain.heightData, before);
    EXPECT_EQ(terrain.splatWeights, beforeSplat);
}

TEST_F(TerrainAuthoringTest, DrainageFitUsesDownhillRouteAndAccumulatesArea)
{
    editor::RegisterControlPointOperators(m_registry);
    auto& ground = MakeTerrain();
    auto& terrain = *ground.GetComponent<scene::TerrainComponent>();
    terrain.Resize(9, 9);
    terrain.cellSize = 1.0f;
    for (int z = 0; z < 9; ++z)
        for (int x = 0; x < 9; ++x)
            ASSERT_TRUE(terrain.SetHeightAtGrid(x, z, static_cast<float>(8 - z + 2 * std::abs(x - 4))));
    editor::OpArgs args;
    args.Set("node", ground.instanceId);
    args.Set("seed", math::Vector3{4, 0, 4});
    args.Set("width", 8.0f);
    args.Set("depth", 8.0f);
    args.Set("spacing", 1.0f);
    args.Set("minArea", 0.0f);
    const auto result = editor::InvokeOperator(Context(), "terrain.drainage.inspect", args);
    ASSERT_TRUE(result.ok) << result.message;
    const auto* seed = result.data.Find("seed");
    const auto* outlet = result.data.Find("outlet");
    ASSERT_NE(seed, nullptr);
    ASSERT_NE(outlet, nullptr);
    EXPECT_GT(outlet->Find("area")->AsDouble(), seed->Find("area")->AsDouble());
    EXPECT_FLOAT_EQ(outlet->Find("ground")->AsVec3().z, 8.0f);

    auto& owner = AttachedScene()->CreateGameObject("River");
    auto& water = owner.AddComponent<scene::WaterComponent>();
    args.Set("node", owner.instanceId);
    args.Set("terrain", ground.instanceId);
    const auto fit = editor::InvokeOperator(Context(), "control_points.fit_river_drainage", args);
    ASSERT_TRUE(fit.ok) << fit.message;
    ASSERT_EQ(water.splinePoints.size(), 5u);
    for (std::size_t i = 1; i < water.splinePoints.size(); ++i) {
        EXPECT_LT(water.splinePoints[i].localPosition.y, water.splinePoints[i - 1].localPosition.y);
        EXPECT_GE(water.splinePoints[i].width, water.splinePoints[i - 1].width);
    }
    ASSERT_TRUE(m_undo.Undo());
    EXPECT_EQ(water.surfaceShape, scene::WaterSurfaceShape::RECTANGLE);
}

TEST_F(TerrainAuthoringTest, RiverProfileDoesNotUseTheDepressionFillAsItsSurface)
{
    auto& ground = MakeTerrain();
    auto& terrain = *ground.GetComponent<scene::TerrainComponent>();
    terrain.Resize(9, 9);
    terrain.cellSize = 1.0f;
    for (int z = 0; z < 9; ++z)
        for (int x = 0; x < 9; ++x) {
            const auto radius = (std::max)(std::abs(x - 4), std::abs(z - 4));
            ASSERT_TRUE(terrain.SetHeightAtGrid(x, z, radius == 2 ? 20.0f : radius < 2 ? 5.0f : 2.0f));
        }
    editor::OpArgs args;
    args.Set("node", ground.instanceId);
    args.Set("seed", math::Vector3{4, 0, 4});
    args.Set("width", 8.0f);
    args.Set("depth", 8.0f);
    args.Set("spacing", 1.0f);
    args.Set("minArea", 0.0f);
    const auto result = editor::InvokeOperator(Context(), "terrain.drainage.inspect", args);
    ASSERT_TRUE(result.ok) << result.message;
    const auto* seed = result.data.Find("seed");
    ASSERT_NE(seed, nullptr);
    EXPECT_NEAR(seed->Find("waterHeight")->AsDouble(), 20.0, 0.01);
    EXPECT_NEAR(seed->Find("riverSurfaceHeight")->AsDouble(), 5.05, 0.01);
    EXPECT_LT(seed->Find("depth")->AsDouble(), 1.0);
    const auto& edges = result.data.Find("edges")->AsArray();
    for (const auto& edge : edges) {
        EXPECT_LE(edge.Find("riverSurfaceHeight")->AsDouble(), edge.Find("ground")->AsVec3().y + 0.051f);
        const auto downstream = edge.Find("downstream")->AsInt();
        for (const auto& other : edges)
            if (other.Find("index")->AsInt() == downstream)
                EXPECT_LE(other.Find("riverSurfaceHeight")->AsDouble(), edge.Find("riverSurfaceHeight")->AsDouble());
    }
}

TEST_F(TerrainAuthoringTest, WaterfallRuleRequiresAnActualCliffAndALakeFedReach)
{
    std::array sections{
        editor::TerrainRiverCarveSection{{0, 0}, 15.05f, 2.0f, 1.0f, true, false, false, 15.0f},
        editor::TerrainRiverCarveSection{{4, 0}, 14.05f, 2.0f, 1.0f, false, false, false, 14.0f},
        editor::TerrainRiverCarveSection{{8, 0}, 6.05f, 2.0f, 1.0f, false, false, false, 6.0f},
        editor::TerrainRiverCarveSection{{12, 0}, 5.05f, 2.0f, 1.0f, false, false, false, 5.0f}};
    EXPECT_TRUE(editor::FindTerrainWaterfallLips(sections, 3.0f, 0.85f).empty());
    sections[1].sourceLakeHeight = 13.0f;
    EXPECT_TRUE(editor::FindTerrainWaterfallLips(sections, 3.0f, 0.85f).empty());
    sections[1].sourceLakeHeight = 16.0f;
    EXPECT_EQ(editor::FindTerrainWaterfallLips(sections, 3.0f, 0.85f), std::vector<std::size_t>{1});
    EXPECT_TRUE(editor::FindTerrainWaterfallLips(sections, 9.0f, 0.85f).empty());
    EXPECT_TRUE(editor::FindTerrainWaterfallLips(sections, 3.0f, 2.1f).empty());
    sections[2].groundHeight = 12.0f;
    EXPECT_TRUE(editor::FindTerrainWaterfallLips(sections, 3.0f, 0.85f).empty());
    sections[2].groundHeight = 6.0f;
    sections[2].beginsReach = true;
    EXPECT_TRUE(editor::FindTerrainWaterfallLips(sections, 3.0f, 0.85f).empty());
}

TEST_F(TerrainAuthoringTest, DrainageCrossesTerrainTileBoundary)
{
    editor::RegisterControlPointOperators(m_registry);
    auto& upstream = MakeTerrain("Upstream");
    auto& downstream = MakeTerrain("Downstream");
    downstream.transform.position.z = 4.0f;
    for (auto* tile : {&upstream, &downstream}) {
        auto& terrain = *tile->GetComponent<scene::TerrainComponent>();
        terrain.Resize(9, 5);
        terrain.cellSize = 1.0f;
        for (int z = 0; z < 5; ++z)
            for (int x = 0; x < 9; ++x)
                ASSERT_TRUE(terrain.SetHeightAtGrid(x, z,
                    8.0f - tile->transform.position.z - z + 2.0f * std::abs(x - 4)));
    }
    auto tiles = editor::OpData::MakeArray();
    tiles.Push(upstream.instanceId);
    tiles.Push(downstream.instanceId);
    editor::OpArgs args;
    args.Set("terrainTiles", tiles);
    args.Set("seed", math::Vector3{4, 0, 4});
    args.Set("width", 8.0f);
    args.Set("depth", 8.0f);
    args.Set("spacing", 1.0f);
    args.Set("minArea", 0.0f);
    const auto query = editor::InvokeOperator(Context(), "terrain.drainage.inspect", args);
    ASSERT_TRUE(query.ok) << query.message;
    EXPECT_EQ(query.data.Find("tileCount")->AsInt(), 2);
    EXPECT_FLOAT_EQ(query.data.Find("outlet")->Find("ground")->AsVec3().z, 8.0f);

    auto& river = AttachedScene()->CreateGameObject("River across tiles");
    auto& water = river.AddComponent<scene::WaterComponent>();
    args.Set("node", river.instanceId);
    const auto fit = editor::InvokeOperator(Context(), "control_points.fit_river_drainage", args);
    ASSERT_TRUE(fit.ok) << fit.message;
    ASSERT_FALSE(water.splinePoints.empty());
    EXPECT_FLOAT_EQ(water.splinePoints.back().localPosition.z, 8.0f);
}

TEST_F(TerrainAuthoringTest, LakeUsesSpillHeightAndKeepsTheIslandDry)
{
    editor::RegisterControlPointOperators(m_registry);
    auto& ground = MakeTerrain();
    auto& terrain = *ground.GetComponent<scene::TerrainComponent>();
    terrain.Resize(9, 9);
    terrain.cellSize = 1.0f;
    for (int z = 0; z < 9; ++z)
        for (int x = 0; x < 9; ++x) {
            const bool basin = x >= 2 && x <= 6 && z >= 2 && z <= 6;
            const float height = x == 5 && z == 4 ? 6.0f : (basin ? 0.0f : 3.0f);
            ASSERT_TRUE(terrain.SetHeightAtGrid(x, z, height));
        }
    auto& owner = AttachedScene()->CreateGameObject("Lake");
    auto& water = owner.AddComponent<scene::WaterComponent>();
    editor::OpArgs args;
    args.Set("node", owner.instanceId);
    args.Set("terrain", ground.instanceId);
    args.Set("seed", math::Vector3{4, 0, 4});
    args.Set("width", 8.0f);
    args.Set("depth", 8.0f);
    args.Set("spacing", 1.0f);
    args.Set("tolerance", 0.05f);
    args.Set("useSpillLevel", true);
    args.Set("freeboard", 0.5f);
    const auto result = editor::InvokeOperator(Context(), "control_points.fit_lake_shore", args);
    ASSERT_TRUE(result.ok) << result.message;
    EXPECT_EQ(result.data.Find("islandCount")->AsInt(), 1);
    EXPECT_NEAR(water.lakePlaneHeight, 2.5f, 0.001f);
    scene::WaterSplineSnapshot mesh;
    ASSERT_TRUE(scene::BuildWaterSplineSnapshot(water.surfaceShape, water.splinePoints,
        water.splineSamplesPerSegment, owner.transform, mesh, water.buoyancyDepth, water.splineMeshCellSize, water.lakeIslandPoints));
    scene::WaterSplineSample sample;
    EXPECT_TRUE(scene::TrySampleWaterSpline(mesh, 4, 4, sample));
    EXPECT_FALSE(scene::TrySampleWaterSpline(mesh, 5, 4, sample));
    ASSERT_TRUE(m_undo.Undo());
    EXPECT_TRUE(water.lakeIslandPoints.empty());
}

TEST_F(TerrainAuthoringTest, FlatLowlandAndSnowlineUseWorldHeight)
{
    auto& go = MakeTerrain();
    auto& terrain = *go.GetComponent<scene::TerrainComponent>();
    editor::TerrainBiomeSettings settings;
    settings.snowHeight = 50.0f;
    settings.snowFade = 20.0f;
    ASSERT_TRUE(editor::BakeTerrainBiome(terrain, go.transform, settings));
    EXPECT_FLOAT_EQ(terrain.GetLayerWeightAtGrid(1, 1, 0), 1.0f);
    go.transform.position.y = 50.0f;
    scene::FlushWorldTransforms(*AttachedScene());
    ASSERT_TRUE(editor::BakeTerrainBiome(terrain, go.transform, settings));
    EXPECT_NEAR(terrain.GetLayerWeightAtGrid(1, 1, 0), 0.5f, 1.0f / 255.0f);
    EXPECT_NEAR(terrain.GetLayerWeightAtGrid(1, 1, 2), 0.5f, 1.0f / 255.0f);
    go.transform.position.y = 60.0f;
    scene::FlushWorldTransforms(*AttachedScene());
    ASSERT_TRUE(editor::BakeTerrainBiome(terrain, go.transform, settings));
    EXPECT_FLOAT_EQ(terrain.GetLayerWeightAtGrid(1, 1, 2), 1.0f);
}

TEST_F(TerrainAuthoringTest, SteepSlopeLeavesRockAndPreservesHeightAndHoles)
{
    auto& go = MakeTerrain();
    auto& terrain = *go.GetComponent<scene::TerrainComponent>();
    for (int z = 0; z < 3; ++z)
        for (int x = 0; x < 3; ++x) terrain.heightData[z * 3 + x] = static_cast<float>(x) * 0.2f;
    ASSERT_TRUE(terrain.SetHoleCell(0, 0, true));
    const auto heights = terrain.heightData;
    const auto holes = terrain.holeData;
    editor::TerrainBiomeSettings settings;
    settings.snowHeight = -100.0f;
    ASSERT_TRUE(editor::BakeTerrainBiome(terrain, go.transform, settings));
    EXPECT_FLOAT_EQ(terrain.GetLayerWeightAtGrid(1, 1, 1), 1.0f);
    EXPECT_EQ(terrain.heightData, heights);
    EXPECT_EQ(terrain.holeData, holes);
}

TEST_F(TerrainAuthoringTest, InvalidSettingsAndSingularTransformLeaveSplatUnchanged)
{
    auto& go = MakeTerrain();
    auto& terrain = *go.GetComponent<scene::TerrainComponent>();
    const auto indices = terrain.splatIndices;
    const auto weights = terrain.splatWeights;
    editor::TerrainBiomeSettings settings;
    settings.snowLayer = settings.rockLayer;
    EXPECT_FALSE(editor::BakeTerrainBiome(terrain, go.transform, settings));
    settings.snowLayer = 2;
    settings.snowHeight = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(editor::BakeTerrainBiome(terrain, go.transform, settings));
    settings.snowHeight = 10.0f;
    go.transform.worldScale.y = 0.0f;
    EXPECT_FALSE(editor::BakeTerrainBiome(terrain, go.transform, settings));
    EXPECT_EQ(terrain.splatIndices, indices);
    EXPECT_EQ(terrain.splatWeights, weights);
}

TEST_F(TerrainAuthoringTest, WorldSlopeIncludesNonuniformScaleAndRotation)
{
    auto& go = MakeTerrain();
    auto& terrain = *go.GetComponent<scene::TerrainComponent>();
    for (int z = 0; z < 3; ++z)
        for (int x = 0; x < 3; ++x) terrain.heightData[z * 3 + x] = static_cast<float>(x) * 0.2f;
    go.transform.worldScale = {4.0f, 1.0f, 1.0f};
    editor::TerrainBiomeSettings settings;
    ASSERT_TRUE(editor::BakeTerrainBiome(terrain, go.transform, settings));
    EXPECT_FLOAT_EQ(terrain.GetLayerWeightAtGrid(1, 1, 0), 1.0f);
    go.transform.worldRotation = math::Quaternion::FromAxisAngle({1.0f, 0.0f, 0.0f}, 1.0471975512f);
    ASSERT_TRUE(editor::BakeTerrainBiome(terrain, go.transform, settings));
    EXPECT_FLOAT_EQ(terrain.GetLayerWeightAtGrid(1, 1, 1), 1.0f);
}

TEST_F(TerrainAuthoringTest, BiomeOperatorCreatesOneUndoAndDoesNotTouchOtherTerrain)
{
    auto& first = MakeTerrain("First");
    auto& second = MakeTerrain("Second");
    editor::OpArgs args;
    args.Set("node", first.instanceId);
    args.Set("snowHeight", -100.0f);
    ASSERT_TRUE(editor::InvokeOperator(Context(), "terrain.biome.bake", args).ok);
    EXPECT_EQ(m_undo.GetHistorySize(), 1u);
    EXPECT_FLOAT_EQ(first.GetComponent<scene::TerrainComponent>()->GetLayerWeightAtGrid(1, 1, 2), 1.0f);
    EXPECT_FLOAT_EQ(second.GetComponent<scene::TerrainComponent>()->GetLayerWeightAtGrid(1, 1, 0), 1.0f);
    m_undo.Undo();
    EXPECT_FLOAT_EQ(first.GetComponent<scene::TerrainComponent>()->GetLayerWeightAtGrid(1, 1, 0), 1.0f);
    m_undo.Redo();
    EXPECT_FLOAT_EQ(first.GetComponent<scene::TerrainComponent>()->GetLayerWeightAtGrid(1, 1, 2), 1.0f);
}

TEST_F(TerrainAuthoringTest, FailedImportDoesNotApplyResizeOrCreateUndo)
{
    auto& go = MakeTerrain();
    editor::OpArgs args;
    args.Set("node", go.instanceId);
    args.Set("path", File("missing.png").string());
    args.Set("columns", 65);
    args.Set("rows", 65);
    args.Set("maxHeight", 640.0f);
    EXPECT_FALSE(editor::InvokeOperator(Context(), "terrain.heightmap.import", args).ok);
    EXPECT_EQ(go.GetComponent<scene::TerrainComponent>()->columns, 3);
    EXPECT_FLOAT_EQ(go.GetComponent<scene::TerrainComponent>()->maxHeight, 100.0f);
    EXPECT_EQ(m_undo.GetHistorySize(), 0u);
}

TEST_F(TerrainAuthoringTest, ReplacedTerrainRejectsUndoWithoutMovingHistory)
{
    auto& go = MakeTerrain();
    editor::OpArgs args;
    args.Set("node", go.instanceId);
    args.Set("snowHeight", -100.0f);
    ASSERT_TRUE(editor::InvokeOperator(Context(), "terrain.biome.bake", args).ok);
    const auto cursor = m_undo.GetCursor();
    go.RemoveComponent<scene::TerrainComponent>();
    auto& replacement = go.AddComponent<scene::TerrainComponent>();
    replacement.InitFlat(12.0f);
    const auto heights = replacement.heightData;
    EXPECT_FALSE(m_undo.Undo());
    EXPECT_EQ(m_undo.GetCursor(), cursor);
    EXPECT_EQ(replacement.heightData, heights);
}

TEST_F(TerrainAuthoringTest, ExternalTerrainEditRejectsUndoWithoutOverwritingIt)
{
    auto& go = MakeTerrain();
    editor::OpArgs args;
    args.Set("node", go.instanceId);
    args.Set("snowHeight", -100.0f);
    ASSERT_TRUE(editor::InvokeOperator(Context(), "terrain.biome.bake", args).ok);
    auto& terrain = *go.GetComponent<scene::TerrainComponent>();
    terrain.heightData[4] = 0.35f;
    const auto cursor = m_undo.GetCursor();
    EXPECT_FALSE(m_undo.Undo());
    EXPECT_EQ(m_undo.GetCursor(), cursor);
    EXPECT_FLOAT_EQ(terrain.heightData[4], 0.35f);
    EXPECT_FLOAT_EQ(terrain.GetLayerWeightAtGrid(1, 1, 2), 1.0f);
}

TEST_F(TerrainAuthoringTest, UndoAndRedoPreserveSavedReferenceAndRuntimeRevisions)
{
    auto& go = MakeTerrain();
    editor::OpArgs args;
    args.Set("node", go.instanceId);
    args.Set("snowHeight", -100.0f);
    ASSERT_TRUE(editor::InvokeOperator(Context(), "terrain.biome.bake", args).ok);
    auto& terrain = *go.GetComponent<scene::TerrainComponent>();
    terrain.terrainAssetPath = "guid:saved-terrain|Assets/Terrain/Saved.terrain";
    terrain.surfaceRevision = 100;
    terrain.vegetationRevision = 200;
    terrain.heightDirty = terrain.colliderDirty = terrain.splatDirty = false;
    ASSERT_TRUE(m_undo.Undo());
    EXPECT_EQ(terrain.terrainAssetPath, "guid:saved-terrain|Assets/Terrain/Saved.terrain");
    EXPECT_EQ(terrain.surfaceRevision, 100u);
    EXPECT_EQ(terrain.vegetationRevision, 200u);
    EXPECT_TRUE(terrain.splatDirty);
    EXPECT_FALSE(terrain.heightDirty);
    EXPECT_FALSE(terrain.colliderDirty);
    terrain.vegetationRevision = 201;
    terrain.splatDirty = false;
    ASSERT_TRUE(m_undo.Redo());
    EXPECT_EQ(terrain.terrainAssetPath, "guid:saved-terrain|Assets/Terrain/Saved.terrain");
    EXPECT_EQ(terrain.surfaceRevision, 100u);
    EXPECT_EQ(terrain.vegetationRevision, 201u);
    EXPECT_TRUE(terrain.splatDirty);
    EXPECT_FALSE(terrain.heightDirty);
    EXPECT_FALSE(terrain.colliderDirty);
}

TEST_F(TerrainAuthoringTest, SixteenBitImportRetainsAdjacentSamplesAndBipolarMapping)
{
    auto& go = MakeTerrain();
    auto& terrain = *go.GetComponent<scene::TerrainComponent>();
    terrain.heightData = {0.0f, 10000.0f / 65535.0f, 10001.0f / 65535.0f,
        0.5f, 1.0f, 0.75f, 0.1f, 0.2f, 0.3f};
    const auto expected = terrain.heightData;
    const auto path = File("Height16.png").string();
    ASSERT_TRUE(scene::SaveHeightMapToFile(path, terrain, true));
    editor::OpArgs args;
    args.Set("node", go.instanceId);
    args.Set("path", path);
    args.Set("unipolar", false);
    ASSERT_TRUE(editor::InvokeOperator(Context(), "terrain.heightmap.import", args).ok);
    ASSERT_EQ(terrain.heightData.size(), expected.size());
    for (size_t i = 0; i < expected.size(); ++i)
        EXPECT_NEAR(terrain.heightData[i], expected[i] * 2.0f - 1.0f, 2.0f / 65535.0f);
    EXPECT_GT(terrain.heightData[2], terrain.heightData[1]);
    m_undo.Undo();
    EXPECT_EQ(terrain.heightData, expected);
}

TEST_F(TerrainAuthoringTest, MaskWhitePaintsAndBlackPreservesWithoutChangingHeight)
{
    auto& go = MakeTerrain();
    auto& terrain = *go.GetComponent<scene::TerrainComponent>();
    scene::TerrainComponent mask = terrain;
    mask.heightData[0] = 1.0f;
    const auto path = File("Mask.png").string();
    ASSERT_TRUE(scene::SaveHeightMapToFile(path, mask));
    const auto heights = terrain.heightData;
    terrain.heightDirty = terrain.colliderDirty = false;
    const auto revision = terrain.surfaceRevision;
    editor::OpArgs args;
    args.Set("node", go.instanceId);
    args.Set("path", path);
    args.Set("layer", 2);
    ASSERT_TRUE(editor::InvokeOperator(Context(), "terrain.layer.import_mask", args).ok);
    EXPECT_FLOAT_EQ(terrain.GetLayerWeightAtGrid(0, 0, 2), 1.0f);
    EXPECT_FLOAT_EQ(terrain.GetLayerWeightAtGrid(1, 0, 0), 1.0f);
    EXPECT_EQ(terrain.heightData, heights);
    EXPECT_FALSE(terrain.heightDirty);
    EXPECT_FALSE(terrain.colliderDirty);
    EXPECT_EQ(terrain.surfaceRevision, revision);
    m_undo.Undo();
    EXPECT_FLOAT_EQ(terrain.GetLayerWeightAtGrid(0, 0, 0), 1.0f);
    EXPECT_FALSE(terrain.heightDirty);
    EXPECT_FALSE(terrain.colliderDirty);
    EXPECT_EQ(terrain.surfaceRevision, revision);
}

TEST_F(TerrainAuthoringTest, HeightStampChangesOnlyItsRectangleAndPreservesOtherAuthoring)
{
    auto& go = MakeTerrain();
    auto& terrain = *go.GetComponent<scene::TerrainComponent>();
    terrain.Resize(7, 7);
    terrain.InitFlat(5.0f);
    ASSERT_TRUE(terrain.SetHoleCell(0, 0, true));
    const auto before = terrain;
    scene::TerrainComponent image;
    image.columns = image.rows = 2;
    image.heightData = {0.0f, 1.0f, 0.0f, 1.0f};
    editor::TerrainHeightStampSettings settings;
    settings.centerLocal = {30.0f, 0.0f, 30.0f};
    settings.heightScale = 20.0f;
    settings.edgeFade = 0.0f;
    settings.blend = editor::TerrainHeightStampBlend::REPLACE;
    ASSERT_TRUE(editor::ApplyTerrainHeightStamp(terrain, image, settings));
    EXPECT_FLOAT_EQ(terrain.GetHeightAt(20.0f, 30.0f), 0.0f);
    EXPECT_NEAR(terrain.GetHeightAt(30.0f, 30.0f), 10.0f, 0.0001f);
    EXPECT_NEAR(terrain.GetHeightAt(40.0f, 30.0f), 20.0f, 0.0001f);
    for (int z = 0; z < 7; ++z)
        for (int x = 0; x < 7; ++x)
            if (x < 2 || x > 4 || z < 2 || z > 4)
                EXPECT_FLOAT_EQ(terrain.heightData[z * 7 + x], before.heightData[z * 7 + x]);
    EXPECT_EQ(terrain.splatWeights, before.splatWeights);
    EXPECT_EQ(terrain.splatIndices, before.splatIndices);
    EXPECT_EQ(terrain.holeData, before.holeData);
    EXPECT_EQ(terrain.layerMaterials, before.layerMaterials);
    EXPECT_TRUE(terrain.heightDirty);
    EXPECT_TRUE(terrain.colliderDirty);
}

TEST_F(TerrainAuthoringTest, RotatedRectangularStampUsesImageOrientationAndClipsAtTerrainEdge)
{
    auto& terrain = *MakeTerrain().GetComponent<scene::TerrainComponent>();
    terrain.Resize(7, 7);
    scene::TerrainComponent image;
    image.columns = image.rows = 2;
    image.heightData = {0.0f, 1.0f, 0.0f, 1.0f};
    editor::TerrainHeightStampSettings settings;
    settings.centerLocal = {0.0f, 0.0f, 30.0f};
    settings.width = 40.0f;
    settings.depth = 20.0f;
    settings.heightScale = 20.0f;
    settings.rotationDegrees = 90.0f;
    settings.edgeFade = 0.0f;
    ASSERT_TRUE(editor::ApplyTerrainHeightStamp(terrain, image, settings));
    EXPECT_NEAR(terrain.GetHeightAt(0.0f, 20.0f), 15.0f, 0.0001f);
    EXPECT_NEAR(terrain.GetHeightAt(0.0f, 40.0f), 5.0f, 0.0001f);
    EXPECT_FLOAT_EQ(terrain.GetHeightAt(20.0f, 30.0f), 0.0f);
    EXPECT_FLOAT_EQ(terrain.GetHeightAt(0.0f, 0.0f), 0.0f);
}

TEST_F(TerrainAuthoringTest, StampFadeBlendsIntoExistingTerrainAndSupportsHeightBlendModes)
{
    auto& terrain = *MakeTerrain().GetComponent<scene::TerrainComponent>();
    terrain.Resize(9, 9);
    terrain.InitFlat(10.0f);
    scene::TerrainComponent image;
    image.columns = image.rows = 2;
    image.heightData.assign(4, 1.0f);
    editor::TerrainHeightStampSettings settings;
    settings.centerLocal = {40.0f, 0.0f, 40.0f};
    settings.width = settings.depth = 80.0f;
    settings.heightScale = 20.0f;
    settings.edgeFade = 0.25f;
    settings.strength = 0.5f;
    ASSERT_TRUE(editor::ApplyTerrainHeightStamp(terrain, image, settings));
    EXPECT_NEAR(terrain.GetHeightAt(0.0f, 40.0f), 10.0f, 0.0001f);
    EXPECT_NEAR(terrain.GetHeightAt(10.0f, 40.0f), 15.0f, 0.0001f);
    EXPECT_NEAR(terrain.GetHeightAt(40.0f, 40.0f), 20.0f, 0.0001f);
    settings.blend = editor::TerrainHeightStampBlend::MAXIMUM;
    settings.heightScale = 5.0f;
    ASSERT_TRUE(editor::ApplyTerrainHeightStamp(terrain, image, settings));
    EXPECT_NEAR(terrain.GetHeightAt(40.0f, 40.0f), 20.0f, 0.0001f);
    settings.blend = editor::TerrainHeightStampBlend::MINIMUM;
    ASSERT_TRUE(editor::ApplyTerrainHeightStamp(terrain, image, settings));
    EXPECT_NEAR(terrain.GetHeightAt(40.0f, 40.0f), 12.5f, 0.0001f);
    settings.heightScale = -1000.0f;
    settings.strength = 1.0f;
    ASSERT_TRUE(editor::ApplyTerrainHeightStamp(terrain, image, settings));
    EXPECT_FLOAT_EQ(terrain.GetHeightAt(40.0f, 40.0f), -terrain.maxHeight);
}

TEST_F(TerrainAuthoringTest, InvalidStampLeavesAllHeightsAndRevisionUnchanged)
{
    auto& terrain = *MakeTerrain().GetComponent<scene::TerrainComponent>();
    const auto heights = terrain.heightData;
    const auto revision = terrain.surfaceRevision;
    scene::TerrainComponent image;
    image.columns = image.rows = 2;
    image.heightData.assign(4, 1.0f);
    editor::TerrainHeightStampSettings settings;
    settings.width = 0.0f;
    EXPECT_FALSE(editor::ApplyTerrainHeightStamp(terrain, image, settings));
    settings.width = 20.0f;
    settings.rotationDegrees = std::numeric_limits<float>::infinity();
    EXPECT_FALSE(editor::ApplyTerrainHeightStamp(terrain, image, settings));
    settings.rotationDegrees = 0.0f;
    image.heightData.back() = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(editor::ApplyTerrainHeightStamp(terrain, image, settings));
    EXPECT_EQ(terrain.heightData, heights);
    EXPECT_EQ(terrain.surfaceRevision, revision);
}

TEST_F(TerrainAuthoringTest, StampOperatorTransformsWorldCenterAndPersistsWithOneUndo)
{
    auto& go = MakeTerrain("Stamped");
    auto& other = MakeTerrain("Untouched");
    auto& terrain = *go.GetComponent<scene::TerrainComponent>();
    go.transform.position = {100.0f, 20.0f, 200.0f};
    go.transform.scale = {2.0f, 3.0f, 4.0f};
    go.transform.rotation = math::Quaternion::FromAxisAngle(math::Vector3::UP, 0.5f);
    scene::FlushWorldTransforms(*AttachedScene());
    scene::TerrainComponent image = terrain;
    image.heightData.assign(image.VertexCount(), 1.0f);
    const auto imagePath = File("Stamp16.png").string();
    ASSERT_TRUE(scene::SaveHeightMapToFile(imagePath, image));
    editor::OpArgs args;
    args.Set("node", go.instanceId);
    args.Set("path", imagePath);
    args.Set("center", editor::ToTerrainWorld(go.transform, {10.0f, 0.0f, 10.0f}));
    args.Set("width", 10.0f);
    args.Set("depth", 10.0f);
    ASSERT_TRUE(editor::InvokeOperator(Context(), "terrain.heightmap.stamp", args).ok);
    const auto stamped = terrain.heightData;
    EXPECT_NEAR(terrain.GetHeightAt(10.0f, 10.0f), 10.0f, 0.0001f);
    EXPECT_FLOAT_EQ(terrain.GetHeightAt(0.0f, 0.0f), 0.0f);
    EXPECT_FLOAT_EQ(other.GetComponent<scene::TerrainComponent>()->GetHeightAt(10.0f, 10.0f), 0.0f);
    EXPECT_EQ(m_undo.GetHistorySize(), 1u);
    const auto terrainPath = File("Stamped.terrain").string();
    ASSERT_TRUE(scene::TerrainAssetSerializer::Save(terrain, terrainPath));
    scene::TerrainComponent loaded;
    ASSERT_TRUE(scene::TerrainAssetSerializer::Load(terrainPath, loaded));
    EXPECT_EQ(loaded.heightData, stamped);
    terrain.surfaceRevision = 100;
    ASSERT_TRUE(m_undo.Undo());
    EXPECT_FLOAT_EQ(terrain.GetHeightAt(10.0f, 10.0f), 0.0f);
    EXPECT_EQ(terrain.surfaceRevision, 101u);
    ASSERT_TRUE(m_undo.Redo());
    EXPECT_EQ(terrain.heightData, stamped);
    EXPECT_EQ(terrain.surfaceRevision, 102u);
}

TEST_F(TerrainAuthoringTest, StampOperatorKeepsSixteenBitPrecisionAndBipolarHeights)
{
    auto& go = MakeTerrain();
    auto& terrain = *go.GetComponent<scene::TerrainComponent>();
    scene::TerrainComponent image = terrain;
    image.heightData = {0.0f, 10000.0f / 65535.0f, 10001.0f / 65535.0f,
        0.5f, 1.0f, 0.75f, 0.1f, 0.2f, 0.3f};
    const auto path = File("StampPrecision.png").string();
    ASSERT_TRUE(scene::SaveHeightMapToFile(path, image));
    editor::OpArgs args;
    args.Set("node", go.instanceId);
    args.Set("path", path);
    args.Set("center", math::Vector3{10.0f, 0.0f, 10.0f});
    args.Set("edgeFade", 0.0f);
    args.Set("heightScale", 100.0f);
    args.Set("unipolar", false);
    args.Set("blend", std::string{"replace"});
    ASSERT_TRUE(editor::InvokeOperator(Context(), "terrain.heightmap.stamp", args).ok);
    EXPECT_GT(terrain.heightData[2], terrain.heightData[1]);
    for (size_t i = 0; i < image.heightData.size(); ++i)
        EXPECT_NEAR(terrain.heightData[i], image.heightData[i] * 2.0f - 1.0f, 2.0f / 65535.0f);
}

TEST_F(TerrainAuthoringTest, FailedAndNoOpStampsDoNotDirtyTerrainOrCreateHistory)
{
    auto& go = MakeTerrain();
    auto& terrain = *go.GetComponent<scene::TerrainComponent>();
    const auto before = terrain;
    bool dirty = false;
    Context().markSceneDirty = [&dirty] { dirty = true; };
    editor::OpArgs args;
    args.Set("node", go.instanceId);
    args.Set("path", File("MissingStamp.png").string());
    args.Set("center", math::Vector3{10.0f, 0.0f, 10.0f});
    EXPECT_FALSE(editor::InvokeOperator(Context(), "terrain.heightmap.stamp", args).ok);
    const auto path = File("EmptyStamp.png").string();
    ASSERT_TRUE(scene::SaveHeightMapToFile(path, terrain));
    args.Set("path", path);
    auto result = editor::InvokeOperator(Context(), "terrain.heightmap.stamp", args);
    EXPECT_TRUE(result.ok);
    EXPECT_TRUE(result.noChange);
    args.Set("width", -1.0f);
    EXPECT_FALSE(editor::InvokeOperator(Context(), "terrain.heightmap.stamp", args).ok);
    args.Set("width", 20.0f);
    go.transform.worldScale.y = 0.0f;
    EXPECT_FALSE(editor::InvokeOperator(Context(), "terrain.heightmap.stamp", args).ok);
    EXPECT_EQ(terrain.heightData, before.heightData);
    EXPECT_EQ(terrain.surfaceRevision, before.surfaceRevision);
    EXPECT_EQ(m_undo.GetHistorySize(), 0u);
    EXPECT_FALSE(dirty);
    Context().markSceneDirty = [] {};
}

TEST_F(TerrainAuthoringTest, TerrainMapsSeparatePlanarSlopesFromLocalRelief)
{
    auto& ground = MakeTerrain();
    auto& terrain = *ground.GetComponent<scene::TerrainComponent>();
    const std::array surfaces{editor::TerrainAnalysisSurface{&terrain, &ground.transform}};
    editor::TerrainMapSettings settings;
    settings.radius = 10.0f;
    editor::TerrainMapRaster raster;
    for (int z = 0; z < 3; ++z)
        for (int x = 0; x < 3; ++x) ASSERT_TRUE(terrain.SetHeightAtGrid(x, z, x * 10.0f));
    ASSERT_TRUE(editor::BuildTerrainMap(surfaces, settings, editor::TerrainMapChannel::SLOPE, raster));
    EXPECT_NEAR(raster.Sample(0.5f, 0.5f), 0.5f, 0.001f);
    ASSERT_TRUE(editor::BuildTerrainMap(surfaces, settings, editor::TerrainMapChannel::CAVITY, raster));
    EXPECT_NEAR(raster.Sample(0.5f, 0.5f), 0.5f, 0.001f);
    ASSERT_TRUE(editor::BuildTerrainMap(surfaces, settings, editor::TerrainMapChannel::ROUGHNESS, raster));
    EXPECT_NEAR(raster.Sample(0.5f, 0.5f), 0.0f, 0.001f);
    ASSERT_TRUE(terrain.SetHeightAtGrid(1, 1, 5.0f));
    ASSERT_TRUE(editor::BuildTerrainMap(surfaces, settings, editor::TerrainMapChannel::CAVITY, raster));
    EXPECT_GT(raster.Sample(0.5f, 0.5f), 0.75f);
    ASSERT_TRUE(editor::BuildTerrainMap(surfaces, settings, editor::TerrainMapChannel::ROUGHNESS, raster));
    EXPECT_GT(raster.Sample(0.5f, 0.5f), 0.5f);
}

TEST_F(TerrainAuthoringTest, TerrainFlowMapCarriesUpstreamAreaAcrossTileEdges)
{
    auto& upstream = MakeTerrain("Upstream");
    auto& downstream = MakeTerrain("Downstream");
    downstream.transform.position.x = 20.0f;
    scene::FlushWorldTransforms(*AttachedScene());
    auto& first = *upstream.GetComponent<scene::TerrainComponent>();
    auto& second = *downstream.GetComponent<scene::TerrainComponent>();
    for (int z = 0; z < 3; ++z) {
        for (int x = 0; x < 3; ++x) {
            const float height = 80.0f - 10.0f * x + (z == 1 ? 0.0f : 10.0f);
            ASSERT_TRUE(first.SetHeightAtGrid(x, z, height));
            ASSERT_TRUE(second.SetHeightAtGrid(x, z, height - 20.0f));
        }
    }
    const editor::TerrainAnalysisSurface up{&first, &upstream.transform}, down{&second, &downstream.transform};
    const editor::TerrainMapSettings settings;
    editor::TerrainMapRaster a, b, local;
    ASSERT_TRUE(editor::BuildTerrainMap(std::array{up, down}, settings, editor::TerrainMapChannel::FLOW, a));
    ASSERT_TRUE(editor::BuildTerrainMap(std::array{down, up}, settings, editor::TerrainMapChannel::FLOW, b));
    ASSERT_TRUE(editor::BuildTerrainMap(std::array{down}, settings, editor::TerrainMapChannel::FLOW, local));
    EXPECT_GT(a.Sample(1.0f, 0.5f), 0.0f);
    EXPECT_FLOAT_EQ(a.Sample(1.0f, 0.5f), b.Sample(0.0f, 0.5f));
    EXPECT_GT(b.Sample(0.5f, 0.5f), local.Sample(0.5f, 0.5f));
}

TEST_F(TerrainAuthoringTest, DensityMapBakePreservesHoleBordersAndSupportsUndo)
{
    auto& ground = MakeTerrain();
    auto& terrain = *ground.GetComponent<scene::TerrainComponent>();
    ASSERT_TRUE(terrain.SetHoleCell(0, 0, true));
    const auto before = terrain;
    editor::OpArgs args;
    args.Set("node", ground.instanceId);
    args.Set("channel", std::string("Density"));
    for (std::size_t i = 0; i < editor::TerrainMapSettings{}.preferences.size(); ++i)
        args.Set(editor::TERRAIN_MAP_NAMES[i], 0.0f);
    const auto preview = editor::InvokeOperator(Context(), "terrain.maps.preview", args);
    ASSERT_TRUE(preview.ok) << preview.message;
    const auto* pixels = preview.data.Find("previewGray8Hex");
    ASSERT_NE(pixels, nullptr);
    EXPECT_TRUE(pixels->AsString().starts_with("00"));
    EXPECT_TRUE(pixels->AsString().ends_with("ff"));
    EXPECT_EQ(m_undo.GetHistorySize(), 0u);

    Context().projectRoot = ProjectRoot().generic_string();
    auto exportArgs = args;
    exportArgs.Set("path", std::string("Assets/Density.png"));
    const auto exported = editor::InvokeOperator(Context(), "terrain.maps.export", exportArgs);
    ASSERT_TRUE(exported.ok) << exported.message;
    ASSERT_TRUE(std::filesystem::is_regular_file(File("Assets/Density.png.meta")));
    scene::TerrainComponent image;
    image.columns = terrain.columns;
    image.rows = terrain.rows;
    ASSERT_TRUE(scene::LoadHeightMapFromFile(File("Assets/Density.png").generic_string(), image));
    EXPECT_FLOAT_EQ(image.heightData.front(), 0.0f);
    EXPECT_FLOAT_EQ(image.heightData.back(), 1.0f);
    EXPECT_EQ(m_undo.GetHistorySize(), 0u);

    args.Set("layer", 1);
    const auto baked = editor::InvokeOperator(Context(), "terrain.maps.bake", args);
    ASSERT_TRUE(baked.ok) << baked.message;
    EXPECT_FLOAT_EQ(terrain.GetLayerWeightAtGrid(1, 1, 1), 0.0f);
    EXPECT_FLOAT_EQ(terrain.GetLayerWeightAtGrid(2, 2, 1), 1.0f);
    EXPECT_EQ(terrain.heightData, before.heightData);
    ASSERT_TRUE(m_undo.Undo());
    EXPECT_EQ(terrain.splatIndices, before.splatIndices);
    EXPECT_EQ(terrain.splatWeights, before.splatWeights);
}

TEST_F(TerrainAuthoringTest, RefinedFlowCarveCarriesCatchmentFromOutsideTheDetailRegion)
{
    auto& ground = MakeTerrain();
    auto& terrain = *ground.GetComponent<scene::TerrainComponent>();
    terrain.cellSize = 2.0f;
    terrain.maxHeight = 200.0f;
    terrain.Resize(513, 65);
    for (int z = 0; z < terrain.rows; ++z)
        for (int x = 0; x < terrain.columns; ++x)
            terrain.heightData[static_cast<std::size_t>(z) * terrain.columns + x] = (150.0f - 0.08f * x + 0.9f * std::abs(z - 32)) / terrain.maxHeight;
    const std::array sources{editor::TerrainAnalysisSurface{&terrain, &ground.transform}};
    editor::TerrainFlowCarveSettings settings;
    settings.region = {{512, 0, 64}, 64.0f};
    settings.minArea = 5000.0f;
    editor::TerrainRiverCarvePlan plan;
    std::string diagnostic;
    int reaches = 0;
    ASSERT_TRUE(editor::PlanTerrainFlowCarve(sources, settings, plan, reaches, diagnostic)) << diagnostic;
    EXPECT_FLOAT_EQ(plan.analysisSpacing, 2.0f);
    EXPECT_GT(reaches, 0);
    EXPECT_GT(plan.deepestCut, 0.0f);
    EXPECT_FALSE(plan.vertices.empty());
}

TEST_F(TerrainAuthoringTest, FlowChannelPreviewAndCutLimitKeepTerrainUnchangedUntilApply)
{
    auto& ground = MakeTerrain();
    auto& terrain = *ground.GetComponent<scene::TerrainComponent>();
    terrain.cellSize = 2.0f;
    terrain.Resize(13, 13);
    for (int z = 0; z < terrain.rows; ++z)
        for (int x = 0; x < terrain.columns; ++x)
            terrain.heightData[static_cast<std::size_t>(z) * terrain.columns + x] = (20.0f - 0.2f * x + std::abs(z - 6)) / terrain.maxHeight;
    const auto before = terrain.heightData;
    editor::OpArgs args;
    args.Set("node", ground.instanceId);
    args.Set("flowSize", 0.0f);
    args.Set("minArea", 15.0f);
    args.Set("maxCut", 0.01f);
    const auto preview = editor::InvokeOperator(Context(), "terrain.flow.carve.preview", args);
    ASSERT_TRUE(preview.ok) << preview.message;
    EXPECT_GT(preview.data.Find("changedVertices")->AsInt(), 0);
    EXPECT_FALSE(preview.data.Find("withinLimit")->AsBool());
    EXPECT_FALSE(editor::InvokeOperator(Context(), "terrain.flow.carve", args).ok);
    EXPECT_EQ(terrain.heightData, before);
    EXPECT_EQ(m_undo.GetHistorySize(), 0u);
    args.Set("maxCut", 20.0f);
    const auto carved = editor::InvokeOperator(Context(), "terrain.flow.carve", args);
    ASSERT_TRUE(carved.ok) << carved.message;
    EXPECT_NE(terrain.heightData, before);
    EXPECT_EQ(terrain.heightData.front(), before.front());
    ASSERT_TRUE(m_undo.Undo());
    EXPECT_EQ(terrain.heightData, before);
}

TEST_F(TerrainAuthoringTest, GentleFlowBankDepositsAboveWaterWithinTheRequestedBudget)
{
    auto& ground = MakeTerrain();
    auto& terrain = *ground.GetComponent<scene::TerrainComponent>();
    terrain.cellSize = 2.0f;
    terrain.Resize(9, 9);
    terrain.InitFlat(2.0f);
    const std::array sections{editor::TerrainRiverCarveSection{{2, 8}, 1.8f, 2.0f, 0.4f},
        editor::TerrainRiverCarveSection{{14, 8}, 1.8f, 2.0f, 0.4f}};
    editor::TerrainRiverCarveSettings settings;
    settings.bankWidth = 4.0f;
    settings.endFade = 0.0f;
    settings.depositHeight = 0.5f;
    settings.gentleBankLayer = 1;
    editor::TerrainRiverCarvePlan plan;
    ASSERT_TRUE(editor::PlanTerrainRiverCarve(terrain, ground.transform, sections, settings, plan));
    EXPECT_GT(plan.highestDeposit, 0.0f);
    EXPECT_LE(plan.highestDeposit, settings.depositHeight + 0.0001f);
    EXPECT_GT(plan.deepestCut, 0.0f);
    EXPECT_FALSE(plan.materials.empty());
}

TEST_F(TerrainAuthoringTest, TerrainMaterialRulesBlendTogetherAndProtectPaintedWeights)
{
    auto& ground = MakeTerrain();
    auto& terrain = *ground.GetComponent<scene::TerrainComponent>();
    scene::terrain_splat::BlendToward(terrain.splatIndices.data(), terrain.splatWeights.data(), 2, 1.0f);
    const auto before = terrain.splatWeights;
    const std::array sources{editor::TerrainAnalysisSurface{&terrain, &ground.transform}};
    editor::TerrainMaterialBakeSettings settings;
    settings.rules = {{0, 1.0f, {}}, {1, 3.0f, {}}};
    settings.protectedLayers = {2};
    editor::TerrainMapRaster mask{3, 3, {0, 1, 0, 0, 0, 0, 0, 0, 0}};
    settings.protection = &mask;
    ASSERT_TRUE(editor::BakeTerrainMaterials(terrain, sources, editor::TerrainMapSettings{}, settings));
    EXPECT_FLOAT_EQ(terrain.GetLayerWeightAtGrid(0, 0, 2), 1.0f);
    EXPECT_FLOAT_EQ(terrain.GetLayerWeightAtGrid(1, 0, 0), 1.0f);
    EXPECT_NEAR(terrain.GetLayerWeightAtGrid(2, 0, 1), 0.75f, 1.0f / 255.0f);
    EXPECT_NE(terrain.splatWeights, before);
}

TEST_F(TerrainAuthoringTest, HabitatPatchesAgreeOnTheSharedWorldSpaceTileEdge)
{
    auto& west = MakeTerrain("West");
    auto& east = MakeTerrain("East");
    east.transform.position.x = 20.0f;
    editor::OpArgs args;
    args.Set("channel", std::string("Density"));
    for (std::size_t i = 0; i < editor::TerrainMapSettings{}.preferences.size(); ++i)
        args.Set(editor::TERRAIN_MAP_NAMES[i], 0.0f);
    args.Set("patchScale", 17.0f);
    args.Set("patchMin", 0.38f);
    args.Set("patchMax", 0.75f);
    args.Set("node", west.instanceId);
    const auto left = editor::InvokeOperator(Context(), "terrain.maps.preview", args);
    ASSERT_TRUE(left.ok) << left.message;
    args.Set("node", east.instanceId);
    const auto right = editor::InvokeOperator(Context(), "terrain.maps.preview", args);
    ASSERT_TRUE(right.ok) << right.message;
    const auto& a = left.data.Find("previewGray8Hex")->AsString();
    const auto& b = right.data.Find("previewGray8Hex")->AsString();
    constexpr int SIZE = editor::TERRAIN_MAP_PREVIEW_SIZE;
    for (int row = 0; row < SIZE; ++row)
        EXPECT_EQ(a.substr(2 * ((row + 1) * SIZE - 1), 2), b.substr(2 * row * SIZE, 2));
    args.Set("patchSeed", 42);
    const auto different = editor::InvokeOperator(Context(), "terrain.maps.preview", args);
    ASSERT_TRUE(different.ok) << different.message;
    EXPECT_NE(b, different.data.Find("previewGray8Hex")->AsString());
}

TEST_F(TerrainAuthoringTest, FlowNetworkSharesBankHeightsAndUndoesRiverCreationWithTerrain)
{
    auto& west = MakeTerrain("West");
    auto& east = MakeTerrain("East");
    east.transform.position.x = 24.0f;
    auto& a = *west.GetComponent<scene::TerrainComponent>();
    auto& b = *east.GetComponent<scene::TerrainComponent>();
    a.cellSize = b.cellSize = 2.0f;
    b.maxHeight = 200.0f;
    a.Resize(13, 5);
    b.Resize(13, 5);
    for (int z = 0; z < 5; ++z) {
        for (int x = 0; x < 13; ++x) {
            const float height = 24.0f - x * 0.5f + 4.0f * std::abs(z - 2);
            ASSERT_TRUE(a.SetHeightAtGrid(x, z, height));
            ASSERT_TRUE(b.SetHeightAtGrid(x, z, height - 6.0f));
        }
    }
    const auto protectedOffset = std::size_t{13} * scene::TERRAIN_SPLAT_SLOTS;
    scene::terrain_splat::BlendToward(b.splatIndices.data() + protectedOffset, b.splatWeights.data() + protectedOffset, 2, 1.0f);
    editor::OpArgs args;
    args.Set("node", west.instanceId);
    args.Set("flowSize", 0.0f);
    args.Set("minArea", 32.0f);
    args.Set("protectedLayer", 2);
    const auto preview = editor::InvokeOperator(Context(), "terrain.flow.network.preview", args);
    ASSERT_TRUE(preview.ok) << preview.message;
    EXPECT_EQ(preview.data.Find("tileCount")->AsInt(), 2);
    EXPECT_GT(preview.data.Find("riverCount")->AsInt(), 0);
    EXPECT_FALSE(preview.data.Find("cut")->AsArray().empty());
    EXPECT_FALSE(preview.data.Find("waterLines")->AsArray().empty());
    args.Set("expected", preview.data.Find("expected")->AsString());
    args.Set("bankWidth", 7.0f);
    EXPECT_EQ(editor::InvokeOperator(Context(), "terrain.flow.network.apply", args).errorCode, "STALE_PREVIEW");
    args.Set("bankWidth", 8.0f);
    const float old = a.heightData[0];
    a.heightData[0] += 0.001f;
    EXPECT_EQ(editor::InvokeOperator(Context(), "terrain.flow.network.apply", args).errorCode, "STALE_PREVIEW");
    a.heightData[0] = old;
    EXPECT_EQ(m_undo.GetHistorySize(), 0u);
    const auto beforeA = a.heightData, beforeB = b.heightData;
    const auto applied = editor::InvokeOperator(Context(), "terrain.flow.network.apply", args);
    ASSERT_TRUE(applied.ok) << applied.message;
    ASSERT_EQ(m_undo.GetHistorySize(), 1u);
    EXPECT_NE(a.heightData, beforeA);
    EXPECT_NE(b.heightData, beforeB);
    EXPECT_LT(a.heightData[2 * 13 + 12], beforeA[2 * 13 + 12]);
    EXPECT_FLOAT_EQ(a.heightData[13 + 12], beforeA[13 + 12]);
    EXPECT_FLOAT_EQ(b.heightData[13], beforeB[13]);
    for (int z = 0; z < 5; ++z) EXPECT_NEAR(a.heightData[z * 13 + 12] * a.maxHeight, b.heightData[z * 13] * b.maxHeight, 0.001f);
    const auto& rivers = applied.data.Find("rivers")->AsArray();
    ASSERT_FALSE(rivers.empty());
    for (const auto& guid : rivers) {
        auto* river = AttachedScene()->FindByGuid(guid.AsString());
        ASSERT_NE(river, nullptr);
        EXPECT_EQ(river->GetComponent<scene::WaterComponent>()->surfaceShape, scene::WaterSurfaceShape::SPLINE_RIVER);
    }
    EXPECT_EQ(editor::InvokeOperator(Context(), "terrain.flow.network.apply", args).errorCode, "STALE_PREVIEW");
    ASSERT_TRUE(m_undo.Undo()) << m_undo.LastDiagnostic();
    EXPECT_EQ(a.heightData, beforeA);
    EXPECT_EQ(b.heightData, beforeB);
    for (const auto& guid : rivers) EXPECT_EQ(AttachedScene()->FindByGuid(guid.AsString()), nullptr);
    ASSERT_TRUE(m_undo.Redo()) << m_undo.LastDiagnostic();
    for (const auto& guid : rivers) EXPECT_NE(AttachedScene()->FindByGuid(guid.AsString()), nullptr);
}

TEST_F(TerrainAuthoringTest, FlowNetworkBuildsAndUndoesAConnectedWaterfall)
{
    auto& owner = MakeTerrain("Cliff valley");
    auto& terrain = *owner.GetComponent<scene::TerrainComponent>();
    terrain.Resize(25, 7);
    terrain.cellSize = 2.0f;
    for (int z = 0; z < 7; ++z)
        for (int x = 0; x < 25; ++x)
            ASSERT_TRUE(terrain.SetHeightAtGrid(x, z, 32.0f - 0.25f * x - (x >= 12 ? 8.0f : 0.0f)
                + 4.0f * std::abs(z - 3)));
    editor::OpArgs args;
    args.Set("node", owner.instanceId);
    args.Set("flowSize", 0.0f);
    args.Set("minArea", 100.0f);
    args.Set("maxCut", 24.0f);
    args.Set("buildWaterfalls", true);
    const auto dryPreview = editor::InvokeOperator(Context(), "terrain.flow.network.preview", args);
    ASSERT_TRUE(dryPreview.ok) << dryPreview.message;
    EXPECT_EQ(dryPreview.data.Find("waterfallCount")->AsInt(), 0);
    auto& lakeOwner = AttachedScene()->CreateGameObject("Headwater lake");
    lakeOwner.transform.position.y = 29.85f;
    auto& lake = lakeOwner.AddComponent<scene::WaterComponent>();
    lake.surfaceShape = scene::WaterSurfaceShape::SPLINE_LAKE;
    lake.splineSamplesPerSegment = 1;
    lake.splinePoints = {{1, {11, 0, 4}, 0, 1}, {2, {19.25f, 0, 4}, 0, 1},
        {3, {19.25f, 0, 8}, 0, 1}, {4, {11, 0, 8}, 0, 1}};
    ASSERT_TRUE(terrain.SetHeightAtGrid(8, 3, 29.30f));
    const auto lakeTerrain = terrain.heightData;
    const auto preview = editor::InvokeOperator(Context(), "terrain.flow.network.preview", args);
    ASSERT_TRUE(preview.ok) << preview.message;
    ASSERT_EQ(preview.data.Find("sourceLakeCount")->AsInt(), 1);
    ASSERT_EQ(preview.data.Find("sourceOutletCount")->AsInt(), 1);
    ASSERT_GT(preview.data.Find("waterfallCount")->AsInt(), 0);
    const auto& falls = preview.data.Find("waterfallLines")->AsArray();
    ASSERT_EQ(falls.front().AsArray().size(), 2u);
    const auto lip = falls.front().AsArray()[0].AsVec3();
    const auto foot = falls.front().AsArray()[1].AsVec3();
    EXPECT_GT(lip.y - foot.y, 3.0f);
    args.Set("expected", preview.data.Find("expected")->AsString());
    const auto applied = editor::InvokeOperator(Context(), "terrain.flow.network.apply", args);
    ASSERT_TRUE(applied.ok) << applied.message;
    ASSERT_EQ(m_undo.GetHistorySize(), 1u);
    const auto& ids = applied.data.Find("waterfallVisuals")->AsArray();
    ASSERT_EQ(ids.size(), falls.size());
    auto* curtain = AttachedScene()->FindByGuid(ids[0].AsString());
    ASSERT_NE(curtain, nullptr);
    auto* waterfall = curtain->GetComponent<scene::WaterfallComponent>();
    ASSERT_NE(waterfall, nullptr);
    auto* upstream = waterfall->upstream.Resolve(*AttachedScene());
    auto* downstream = waterfall->downstream.Resolve(*AttachedScene());
    ASSERT_NE(upstream, nullptr);
    ASSERT_NE(downstream, nullptr);
    ASSERT_NE(upstream, downstream);
    scene::WaterfallGeometry geometry;
    const auto readSection = [&](scene::GameObject& node, bool atStart, scene::WaterfallSection& out) {
        const auto* water = node.GetComponent<scene::WaterComponent>();
        scene::WaterSplineSnapshot snapshot;
        return water && scene::BuildWaterSplineSnapshot(water->surfaceShape, water->splinePoints,
            water->splineSamplesPerSegment, node.transform, snapshot, water->buoyancyDepth, 100.0f)
            && scene::GetWaterfallRiverSection(snapshot, atStart, !atStart, out);
    };
    ASSERT_TRUE(readSection(*upstream, false, geometry.source));
    ASSERT_TRUE(readSection(*downstream, true, geometry.receiver));
    EXPECT_VEC3_NEAR(math::Vector3::Lerp(geometry.source.left, geometry.source.right, 0.5f), lip, 0.001f);
    EXPECT_VEC3_NEAR(math::Vector3::Lerp(geometry.receiver.left, geometry.receiver.right, 0.5f), foot, 0.001f);
    scene::MeshBuilder mesh;
    std::uint32_t farIndex = 0;
    ASSERT_TRUE(scene::BuildWaterfallGeometry(geometry, {}, mesh, nullptr, &farIndex));
    ASSERT_GT(farIndex, mesh.IndexCount() - farIndex);
    for (const auto& bank : {geometry.source.left, geometry.source.right, geometry.receiver.left, geometry.receiver.right}) {
        EXPECT_TRUE(std::any_of(mesh.Vertices().begin(), mesh.Vertices().end(), [&](const auto& vertex) {
            return (vertex.position + lip - bank).Length() < 0.001f;
        }));
    }
    EXPECT_FALSE(scene::BuildWaterfallGeometry(geometry, [&](const auto&) { return lip.y + 2.0f; }, mesh));
    ASSERT_TRUE(m_undo.Undo()) << m_undo.LastDiagnostic();
    EXPECT_EQ(terrain.heightData, lakeTerrain);
    for (const auto& id : ids) EXPECT_EQ(AttachedScene()->FindByGuid(id.AsString()), nullptr);
    ASSERT_TRUE(m_undo.Redo()) << m_undo.LastDiagnostic();
    for (const auto& id : ids) EXPECT_NE(AttachedScene()->FindByGuid(id.AsString()), nullptr);
}

TEST_F(TerrainAuthoringTest, RiverBedBetweenTerrainVerticesStillExcavatesTheCenter)
{
    auto& owner = MakeTerrain();
    auto& terrain = *owner.GetComponent<scene::TerrainComponent>();
    terrain.Resize(9, 9);
    terrain.cellSize = 4.0f;
    for (int z = 0; z < terrain.rows; ++z)
        for (int x = 0; x < terrain.columns; ++x)
            ASSERT_TRUE(terrain.SetHeightAtGrid(x, z, 10.0f));
    const std::array sections{editor::TerrainRiverCarveSection{{4, 14}, 10.0f, 0.75f, 1.0f},
        editor::TerrainRiverCarveSection{{28, 14}, 10.0f, 0.75f, 1.0f}};
    editor::TerrainRiverCarveSettings settings;
    settings.endFade = 0.0f;
    settings.bankWidth = 0.0f;
    editor::TerrainRiverCarvePlan plan;
    ASSERT_TRUE(editor::PlanTerrainRiverCarve(terrain, owner.transform, sections, settings, plan));
    scene::TerrainSurfaceSample sample;
    ASSERT_TRUE(scene::TrySampleTerrainWorld(terrain, owner.transform, 16.0f, 14.0f, sample, plan.vertices));
    EXPECT_NEAR(sample.position.y, 9.0f, 0.001f);
    ASSERT_TRUE(scene::TrySampleTerrainWorld(terrain, owner.transform, 16.0f, 20.0f, sample, plan.vertices));
    EXPECT_NEAR(sample.position.y, 10.0f, 0.001f);
}

TEST_F(TerrainAuthoringTest, TerrainRecipeRestoresArgumentsAndRemapsMaterialReferences)
{
    auto& first = MakeTerrain("Recipe source");
    auto& second = MakeTerrain("Recipe target");
    second.GetComponent<scene::TerrainComponent>()->layerMaterials = {"rock", "snow", "grass"};
    Context().projectRoot = ProjectRoot().generic_string();
    auto recipe = editor::OpData::MakeObject(), flow = editor::OpData::MakeObject();
    flow.Set("minArea", 4000.0f);
    flow.Set("rockLayer", 1);
    recipe.Set("flow", std::move(flow));
    auto materials = editor::OpData::MakeObject(), rules = editor::OpData::MakeArray(), rule = editor::OpData::MakeObject();
    rule.Set("layer", 0);
    rule.Set("weight", 2.0f);
    auto preferences = editor::OpData::MakeArray();
    for (float value : {0.0f, -0.25f, 0.0f, 0.5f, 0.0f}) preferences.Push(value);
    rule.Set("preferences", std::move(preferences));
    rules.Push(std::move(rule));
    materials.Set("rules", std::move(rules));
    recipe.Set("materials", std::move(materials));
    auto habitat = editor::OpData::MakeObject();
    habitat.Set("channel", "Density");
    habitat.Set("pathLayer", 2);
    habitat.Set("patchSeed", 19);
    recipe.Set("habitat", std::move(habitat));
    editor::OpArgs save;
    save.Set("path", std::string("Assets/Test.terrainrecipe.toml"));
    save.Set("node", first.instanceId);
    save.Set("recipe", std::move(recipe));
    const auto saved = editor::InvokeOperator(Context(), "terrain.recipe.save", save);
    ASSERT_TRUE(saved.ok) << saved.message;
    EXPECT_TRUE(std::filesystem::is_regular_file(File("Assets/Test.terrainrecipe.toml.meta")));
    editor::OpArgs load;
    load.Set("path", std::string("Assets/Test.terrainrecipe.toml"));
    load.Set("node", second.instanceId);
    const auto loaded = editor::InvokeOperator(Context(), "terrain.recipe.load", load);
    ASSERT_TRUE(loaded.ok) << loaded.message;
    const auto* value = loaded.data.Find("recipe");
    ASSERT_NE(value, nullptr);
    EXPECT_EQ(value->Find("flow")->Find("rockLayer")->AsInt(), 0);
    EXPECT_EQ(value->Find("materials")->Find("rules")->AsArray()[0].Find("layer")->AsInt(), 2);
    EXPECT_EQ(value->Find("habitat")->Find("pathLayer")->AsInt(), 1);
    EXPECT_EQ(value->Find("habitat")->Find("patchSeed")->AsInt(), 19);
    EXPECT_EQ(m_undo.GetHistorySize(), 0u);
}

TEST_F(TerrainAuthoringTest, FlowNetworkWaterPreviewExtendsTributariesOntoTheDownstreamReach)
{
    auto& owner = MakeTerrain();
    auto& terrain = *owner.GetComponent<scene::TerrainComponent>();
    terrain.cellSize = 3.0f;
    terrain.Resize(33, 21);
    for (int x = 0; x < terrain.columns; ++x) {
        const float split = (std::max)(0.0f, 4.0f - 0.25f * x);
        for (int z = 0; z < terrain.rows; ++z) {
            const float valley = (std::min)(std::abs(z - (10.0f - split)), std::abs(z - (10.0f + split)));
            ASSERT_TRUE(terrain.SetHeightAtGrid(x, z, 55.0f - 0.3f * x + 1.5f * valley));
        }
    }
    editor::OpArgs args;
    args.Set("node", owner.instanceId);
    args.Set("flowSize", 0.0f);
    args.Set("minArea", 180.0f);
    args.Set("maxCut", 50.0f);
    const auto preview = editor::InvokeOperator(Context(), "terrain.flow.network.preview", args);
    ASSERT_TRUE(preview.ok) << preview.message;
    const auto& lines = preview.data.Find("waterLines")->AsArray();
    ASSERT_GT(lines.size(), 1u);
    args.Set("expected", preview.data.Find("expected")->AsString());
    const auto applied = editor::InvokeOperator(Context(), "terrain.flow.network.apply", args);
    ASSERT_TRUE(applied.ok) << applied.message;
    std::vector<scene::WaterSplineSnapshot> water;
    const auto& ids = applied.data.Find("rivers")->AsArray();
    ASSERT_EQ(ids.size(), lines.size());
    for (const auto& id : ids) {
        auto* river = AttachedScene()->FindByGuid(id.AsString());
        ASSERT_NE(river, nullptr);
        const auto& component = *river->GetComponent<scene::WaterComponent>();
        ASSERT_TRUE(scene::BuildWaterSplineSnapshot(component.surfaceShape, component.splinePoints,
            component.splineSamplesPerSegment, river->transform, water.emplace_back(), component.buoyancyDepth, component.splineMeshCellSize));
    }
    constexpr std::array OFFSETS{math::Vector2{1, 0}, math::Vector2{1, 1}, math::Vector2{0, 1}, math::Vector2{-1, 1},
        math::Vector2{-1, 0}, math::Vector2{-1, -1}, math::Vector2{0, -1}, math::Vector2{1, -1}};
    bool confluence = false;
    for (std::size_t downstream = 0; downstream < lines.size(); ++downstream) {
        const auto junction = lines[downstream].AsArray().front().AsVec3();
        int tributaries = 0;
        for (std::size_t upstream = 0; upstream < lines.size(); ++upstream) {
            const auto& points = lines[upstream].AsArray();
            if (upstream == downstream || points.size() < 3 || points[points.size() - 2].AsVec3() != junction) continue;
            bool overlap = false;
            for (const auto& offset : OFFSETS) {
                scene::WaterSplineSample a, b;
                const float x = junction.x + 0.08f * offset.x, z = junction.z + 0.08f * offset.y;
                if (scene::TrySampleWaterSpline(water[upstream], x, z, a) && scene::TrySampleWaterSpline(water[downstream], x, z, b)) {
                    EXPECT_NEAR(a.baseHeight, b.baseHeight, 0.1f);
                    overlap = true;
                    break;
                }
            }
            EXPECT_TRUE(overlap);
            if (overlap) ++tributaries;
        }
        confluence |= tributaries > 1;
    }
    EXPECT_TRUE(confluence);
}

} /// @note namespace fbzz::tests
