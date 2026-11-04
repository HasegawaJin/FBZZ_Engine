/// @file    TerrainAuthoringTests.cpp
/// @brief   標高・傾斜の塗り分けと高さ画像・マスクの編集履歴を検証する。
/// @author  Hasegawa Jin
/// @date    2026-11-04
#include <TestKit/TestKit.hpp>
#include <TestKit/Editor/EditorFixture.hpp>
#include <Editor/Op/OperatorGroups.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <Tools/TerrainAuthoring.hpp>
#include <Engine/Scene/TerrainHeightMapLoader.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
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
};

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

} /// @note namespace fbzz::tests
