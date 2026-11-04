/// @file    TerrainGrassOperatorTests.cpp
/// @brief   地形レイヤー草の設定・容量拒否・競合を伴う Undo を検証する。
/// @author  Hasegawa Jin
/// @date    2026-11-04
#include <TestKit/TestKit.hpp>
#include <TestKit/Editor/EditorFixture.hpp>
#include <Editor/Op/OperatorGroups.hpp>
#include <Editor/Util/UndoStack.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Scene/Components/FiberComponent.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Engine/Scene/TerrainHeightMapLoader.hpp>
#include <Tools/TerrainAuthoring.hpp>
#include <filesystem>
#include <limits>

namespace fbzz::tests {
namespace {

class TerrainGrassOperatorTest : public testkit::EditorFixture {
protected:
    void SetUp() override
    {
        EditorFixture::SetUp();
        AttachScene();
        Context().projectRoot = ProjectRoot().generic_string();
        const auto materialPath = File("Assets/Grass.mat");
        asset::MaterialAssetAuthoring material;
        material.params["fiberLength"] = {0.28f};
        ASSERT_TRUE(asset::SaveMaterialAssetToFile(materialPath.generic_string(), material));
        asset::AssetDatabase::Init(File("Assets").generic_string());
        editor::RegisterTerrainGrassOperators(m_registry);
        auto& object = AttachedScene()->CreateGameObject("Terrain");
        m_id = object.GetID();
        m_node = object.instanceId;
        auto& terrain = object.AddComponent<scene::TerrainComponent>();
        terrain.Resize(17, 17);
        terrain.cellSize = 4.0f;
        terrain.maxHeight = 640.0f;
    }

    void TearDown() override
    {
        m_undo.Clear();
        m_registry.Clear();
        asset::AssetDatabase::Shutdown();
        EditorFixture::TearDown();
    }

    editor::OpResult SetupGrass(float density = 4.0f, int layer = 0, float farDistance = 80.0f)
    {
        editor::OpArgs args;
        args.Set("node", m_node);
        args.Set("material", std::string("Assets/Grass.mat"));
        args.Set("terrainLayer", layer);
        args.Set("density", density);
        args.Set("lodFar", farDistance);
        editor::OpContext context{ Context(), m_undo };
        return m_registry.Invoke("terrain.grass.setup", context, args);
    }

    scene::GameObject& Target() { return *AttachedScene()->GetGameObject(m_id); }
    editor::OperatorRegistry m_registry;
    editor::UndoStack m_undo;
    scene::EntityID m_id;
    std::string m_node;
};

TEST_F(TerrainGrassOperatorTest, SetsLayerBoundGrassAndUndoRemovesNewComponent)
{
    const auto result = SetupGrass(4.0f, 1);
    ASSERT_TRUE(result.ok) << result.message;
    const auto* fiber = Target().GetComponent<scene::FiberComponent>();
    ASSERT_NE(fiber, nullptr);
    EXPECT_EQ(fiber->m_mode, scene::FiberRenderMode::BLADE);
    EXPECT_EQ(fiber->m_terrainLayer, 1);
    EXPECT_FLOAT_EQ(fiber->m_bladeDensity, 4.0f);
    EXPECT_TRUE(fiber->m_distanceLod);
    EXPECT_EQ(fiber->m_terrainPatchCells, 8);
    EXPECT_TRUE(fiber->m_materialPath.starts_with("guid:"));
    ASSERT_NE(result.data.Find("candidateWindowUpperBound"), nullptr);
    EXPECT_LE(result.data.Find("candidateWindowUpperBound")->AsInt(), 500000);
    ASSERT_TRUE(m_undo.Undo()) << m_undo.LastDiagnostic();
    EXPECT_EQ(Target().GetComponent<scene::FiberComponent>(), nullptr);
    ASSERT_TRUE(m_undo.Redo()) << m_undo.LastDiagnostic();
    ASSERT_NE(Target().GetComponent<scene::FiberComponent>(), nullptr);
    EXPECT_EQ(Target().GetComponent<scene::FiberComponent>()->m_terrainLayer, 1);
}

TEST_F(TerrainGrassOperatorTest, TwoKilometreTerrainAcceptsDenseGrassWithinTheViewWindow)
{
    auto* terrain = Target().GetComponent<scene::TerrainComponent>();
    terrain->Resize(513, 513);
    const auto result = SetupGrass(4.0f);
    ASSERT_TRUE(result.ok) << result.message;
    const auto* fiber = Target().GetComponent<scene::FiberComponent>();
    ASSERT_NE(fiber, nullptr);
    EXPECT_FLOAT_EQ(fiber->m_bladeDensity, 4.0f);
    ASSERT_NE(result.data.Find("candidateWindowUpperBound"), nullptr);
    EXPECT_LE(result.data.Find("candidateWindowUpperBound")->AsInt(), 500000);
    ASSERT_NE(result.data.Find("windowPatchUpperBound"), nullptr);
    EXPECT_LT(result.data.Find("windowPatchUpperBound")->AsInt(), 64 * 64);
}

TEST_F(TerrainGrassOperatorTest, BundledAlpineHeightMapAcceptsDefaultDenseGrassAfterBiomeBake)
{
    auto* terrain = Target().GetComponent<scene::TerrainComponent>();
    terrain->Resize(513, 513);
    const auto heightMap = std::filesystem::path(FBZZ_SOURCE_DIR) / "Assets/Terrain/Alpine/AlpineRange_Height16.png";
    ASSERT_TRUE(scene::LoadHeightMapFromFile(heightMap.generic_string(), *terrain, true));
    ASSERT_TRUE(editor::BakeTerrainBiome(*terrain, Target().transform, {}));
    const auto result = SetupGrass();
    ASSERT_TRUE(result.ok) << result.message;
    ASSERT_NE(result.data.Find("candidateWindowUpperBound"), nullptr);
    EXPECT_GT(result.data.Find("candidateWindowUpperBound")->AsInt(), 200000);
    EXPECT_LE(result.data.Find("candidateWindowUpperBound")->AsInt(), 500000);
    ASSERT_NE(result.data.Find("windowPatchUpperBound"), nullptr);
    EXPECT_EQ(result.data.Find("windowPatchUpperBound")->AsInt(), 49);
    ASSERT_NE(Target().GetComponent<scene::FiberComponent>(), nullptr);
    EXPECT_FLOAT_EQ(Target().GetComponent<scene::FiberComponent>()->m_bladeDensity, 4.0f);
}

TEST_F(TerrainGrassOperatorTest, RejectsTooLargeAViewWindowBeforeAddingGrass)
{
    auto* terrain = Target().GetComponent<scene::TerrainComponent>();
    terrain->Resize(513, 513);
    const auto result = SetupGrass(4.0f, 0, 160.0f);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.errorCode, "BUDGET_EXCEEDED");
    EXPECT_EQ(Target().GetComponent<scene::FiberComponent>(), nullptr);
    EXPECT_EQ(m_undo.GetHistorySize(), 0u);
}

TEST_F(TerrainGrassOperatorTest, RejectsOversizedSinglePatchBeforeAddingGrass)
{
    auto* terrain = Target().GetComponent<scene::TerrainComponent>();
    terrain->Resize(2, 2);
    terrain->cellSize = 1000.0f;
    const auto result = SetupGrass(0.05f);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.errorCode, "BUDGET_EXCEEDED");
    EXPECT_EQ(Target().GetComponent<scene::FiberComponent>(), nullptr);
}

TEST_F(TerrainGrassOperatorTest, RejectsDenseGrassWhenItsDisplayWindowExceedsTheBudget)
{
    auto* terrain = Target().GetComponent<scene::TerrainComponent>();
    terrain->Resize(513, 513);
    const auto result = SetupGrass(8.0f, 0, 120.0f);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.errorCode, "BUDGET_EXCEEDED");
    EXPECT_EQ(Target().GetComponent<scene::FiberComponent>(), nullptr);
    EXPECT_EQ(m_undo.GetHistorySize(), 0u);
}

TEST_F(TerrainGrassOperatorTest, ExtendingTheTerrainDoesNotIncreaseTheViewWindowBudget)
{
    auto* terrain = Target().GetComponent<scene::TerrainComponent>();
    terrain->Resize(513, 513);
    const auto result = SetupGrass();
    ASSERT_TRUE(result.ok) << result.message;
    ASSERT_NE(result.data.Find("candidateWindowUpperBound"), nullptr);
    ASSERT_NE(result.data.Find("windowPatchUpperBound"), nullptr);
    const auto candidates = result.data.Find("candidateWindowUpperBound")->AsInt();
    const auto patches = result.data.Find("windowPatchUpperBound")->AsInt();
    terrain->Resize(1025, 1025);
    const auto larger = SetupGrass();
    ASSERT_TRUE(larger.ok) << larger.message;
    EXPECT_TRUE(larger.noChange);
    ASSERT_NE(larger.data.Find("candidateWindowUpperBound"), nullptr);
    ASSERT_NE(larger.data.Find("windowPatchUpperBound"), nullptr);
    EXPECT_EQ(larger.data.Find("candidateWindowUpperBound")->AsInt(), candidates);
    EXPECT_EQ(larger.data.Find("windowPatchUpperBound")->AsInt(), patches);
    EXPECT_EQ(m_undo.GetHistorySize(), 1u);
}

TEST_F(TerrainGrassOperatorTest, WorldScaleChangesTheLocalCoverageOfTheViewWindow)
{
    Target().GetComponent<scene::TerrainComponent>()->Resize(513, 513);
    const auto initial = SetupGrass();
    ASSERT_TRUE(initial.ok) << initial.message;
    ASSERT_NE(initial.data.Find("candidateWindowUpperBound"), nullptr);
    Target().transform.scale = { 0.5f, 0.5f, 0.5f };
    scene::FlushWorldTransforms(*AttachedScene());
    const auto compressed = SetupGrass();
    EXPECT_FALSE(compressed.ok);
    EXPECT_EQ(compressed.errorCode, "BUDGET_EXCEEDED");
    Target().transform.scale = { 2.0f, 2.0f, 2.0f };
    scene::FlushWorldTransforms(*AttachedScene());
    const auto expanded = SetupGrass();
    ASSERT_TRUE(expanded.ok) << expanded.message;
    ASSERT_NE(expanded.data.Find("candidateWindowUpperBound"), nullptr);
    EXPECT_LT(expanded.data.Find("candidateWindowUpperBound")->AsInt(),
        initial.data.Find("candidateWindowUpperBound")->AsInt());
    EXPECT_EQ(m_undo.GetHistorySize(), 1u);
}

TEST_F(TerrainGrassOperatorTest, RotatedMirroredNonuniformTerrainPreservesTheWindowBudget)
{
    Target().GetComponent<scene::TerrainComponent>()->Resize(513, 513);
    Target().transform.scale = { 2.0f, 3.0f, 0.5f };
    scene::FlushWorldTransforms(*AttachedScene());
    const auto initial = SetupGrass();
    ASSERT_TRUE(initial.ok) << initial.message;
    ASSERT_NE(initial.data.Find("candidateWindowUpperBound"), nullptr);
    Target().transform.position = { 5000.0f, -800.0f, 1200.0f };
    Target().transform.rotation = math::Quaternion::FromEuler({ 0.3f, 0.8f, -0.5f });
    Target().transform.scale = { -2.0f, -3.0f, 0.5f };
    scene::FlushWorldTransforms(*AttachedScene());
    const auto transformed = SetupGrass();
    ASSERT_TRUE(transformed.ok) << transformed.message;
    ASSERT_NE(transformed.data.Find("candidateWindowUpperBound"), nullptr);
    EXPECT_EQ(transformed.data.Find("candidateWindowUpperBound")->AsInt(),
        initial.data.Find("candidateWindowUpperBound")->AsInt());
    EXPECT_TRUE(transformed.noChange);
}

TEST_F(TerrainGrassOperatorTest, TinyNonzeroScaleUsesTheWholeSmallTerrainBudget)
{
    Target().transform.scale = { 0.00000001f, 0.00000001f, 0.00000001f };
    scene::FlushWorldTransforms(*AttachedScene());
    const auto result = SetupGrass();
    ASSERT_TRUE(result.ok) << result.message;
    ASSERT_NE(result.data.Find("windowPatchUpperBound"), nullptr);
    EXPECT_EQ(result.data.Find("windowPatchUpperBound")->AsInt(), 4);
}

TEST_F(TerrainGrassOperatorTest, InvalidTransformsDoNotAddGrassOrHistory)
{
    const auto reject = [&] {
        scene::FlushWorldTransforms(*AttachedScene());
        const auto result = SetupGrass();
        EXPECT_FALSE(result.ok);
        EXPECT_EQ(result.errorCode, "BAD_TERRAIN");
        EXPECT_EQ(Target().GetComponent<scene::FiberComponent>(), nullptr);
        EXPECT_EQ(m_undo.GetHistorySize(), 0u);
    };
    Target().transform.scale.y = 0.0f;
    reject();
    Target().transform = {};
    Target().transform.position.x = (std::numeric_limits<float>::infinity)();
    reject();
    Target().transform = {};
    Target().transform.rotation = { 0.0f, 0.0f, 0.0f, 2.0f };
    reject();
    Target().transform = {};
    Target().transform.scale.z = (std::numeric_limits<float>::quiet_NaN)();
    reject();
}

TEST_F(TerrainGrassOperatorTest, OverflowingTerrainGeometryDoesNotAddGrass)
{
    Target().GetComponent<scene::TerrainComponent>()->cellSize = (std::numeric_limits<float>::max)();
    const auto result = SetupGrass();
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.errorCode, "BAD_TERRAIN");
    EXPECT_EQ(Target().GetComponent<scene::FiberComponent>(), nullptr);
    EXPECT_EQ(m_undo.GetHistorySize(), 0u);
}

TEST_F(TerrainGrassOperatorTest, UndoPreservesUnrelatedChangesByRejectingAConflictedComponent)
{
    ASSERT_TRUE(SetupGrass().ok);
    auto* fiber = Target().GetComponent<scene::FiberComponent>();
    fiber->m_colorTint.x = 0.5f;
    EXPECT_FALSE(m_undo.Undo());
    ASSERT_NE(Target().GetComponent<scene::FiberComponent>(), nullptr);
    EXPECT_FLOAT_EQ(Target().GetComponent<scene::FiberComponent>()->m_colorTint.x, 0.5f);
}

TEST_F(TerrainGrassOperatorTest, UndoRestoresExistingSettings)
{
    auto& old = Target().AddComponent<scene::FiberComponent>();
    old.m_mode = scene::FiberRenderMode::SHELL;
    old.m_distanceLod = false;
    old.m_bladeDensity = 0.1f;
    old.m_maskPath = "original-mask";
    old.m_lengthScale = 0.5f;
    ASSERT_TRUE(SetupGrass().ok);
    ASSERT_TRUE(m_undo.Undo()) << m_undo.LastDiagnostic();
    const auto* restored = Target().GetComponent<scene::FiberComponent>();
    ASSERT_NE(restored, nullptr);
    EXPECT_EQ(restored->m_mode, scene::FiberRenderMode::SHELL);
    EXPECT_FALSE(restored->m_distanceLod);
    EXPECT_FLOAT_EQ(restored->m_bladeDensity, 0.1f);
    EXPECT_EQ(restored->m_maskPath, "original-mask");
    EXPECT_FLOAT_EQ(restored->m_lengthScale, 0.5f);
}

TEST_F(TerrainGrassOperatorTest, InvalidLayerDoesNotChangeAnExistingFiber)
{
    auto& old = Target().AddComponent<scene::FiberComponent>();
    old.m_bladeDensity = 0.1f;
    EXPECT_FALSE(SetupGrass(0.05f, 30).ok);
    EXPECT_FLOAT_EQ(Target().GetComponent<scene::FiberComponent>()->m_bladeDensity, 0.1f);
    EXPECT_EQ(m_undo.GetHistorySize(), 0u);
}

} /// @note namespace
} /// @note namespace fbzz::tests
