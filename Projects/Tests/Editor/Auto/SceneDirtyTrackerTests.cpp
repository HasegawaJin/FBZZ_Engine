/// @file    SceneDirtyTrackerTests.cpp
/// @brief   シーンに未保存の変更があるかの判定。
/// @author  Hasegawa Jin
/// @date    2026-09-10
///
/// @note 未保存状態の判定と、バックグラウンド保存へ渡すデータの境界を検証する。
#include <TestKit/TestKit.hpp>
#include <TestKit/Editor/EditorFixture.hpp>

#include <Editor/Util/SceneDirtyTracker.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Util/SceneEditUtils.hpp>
#include <Editor/Util/SceneIO.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/SceneSerializer.hpp>
#include <Engine/Scene/ScriptComponent.hpp>
#include <Engine/Scene/TerrainAssetSerializer.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <array>

namespace fbzz::tests {

using editor::SceneDirtyTracker;

class SceneDirtyTrackerTest : public testkit::EditorFixture {};

TEST_F(SceneDirtyTrackerTest, StartsClean)
{
    SceneDirtyTracker tracker;
    EXPECT_FALSE(tracker.IsDirty());
}

TEST_F(SceneDirtyTrackerTest, StaysCleanWhenNothingChanges)
{
    scene::Scene scene;
    scene.CreateGameObject("Player");

    SceneDirtyTracker tracker;
    tracker.CaptureClean(scene);

    EXPECT_FALSE(tracker.Evaluate(scene));
    EXPECT_FALSE(tracker.IsDirty());
}

TEST_F(SceneDirtyTrackerTest, DetectsAnAddedObject)
{
    scene::Scene scene;
    SceneDirtyTracker tracker;
    tracker.CaptureClean(scene);

    scene.CreateGameObject("Player");

    EXPECT_TRUE(tracker.Evaluate(scene));
}

TEST_F(SceneDirtyTrackerTest, DetectsARename)
{
    scene::Scene scene;
    scene::GameObject& object = scene.CreateGameObject("Player");
    SceneDirtyTracker tracker;
    tracker.CaptureClean(scene);

    object.name = "Enemy";

    EXPECT_TRUE(tracker.Evaluate(scene));
}

TEST_F(SceneDirtyTrackerTest, DetectsAMove)
{
    scene::Scene scene;
    scene::GameObject& object = scene.CreateGameObject("Player");
    SceneDirtyTracker tracker;
    tracker.CaptureClean(scene);

    object.transform.position = { 1.0f, 2.0f, 3.0f };

    EXPECT_TRUE(tracker.Evaluate(scene));
}

TEST_F(SceneDirtyTrackerTest, DetectsAnActiveToggle)
{
    scene::Scene scene;
    scene::GameObject& object = scene.CreateGameObject("Player");
    SceneDirtyTracker tracker;
    tracker.CaptureClean(scene);

    object.SetActive(false);

    EXPECT_TRUE(tracker.Evaluate(scene));
}

TEST_F(SceneDirtyTrackerTest, GoesCleanAgainAfterCapturingTheNewState)
{
    /// @note 保存したら «今の姿» が新しい基準になる。ここが効かないと保存後も促され続ける。
    scene::Scene scene;
    SceneDirtyTracker tracker;
    tracker.CaptureClean(scene);
    scene.CreateGameObject("Player");
    ASSERT_TRUE(tracker.Evaluate(scene));

    tracker.CaptureClean(scene);

    EXPECT_FALSE(tracker.Evaluate(scene));
}

TEST_F(SceneDirtyTrackerTest, ReturnsToCleanWhenTheEditIsUndone)
{
    /// @note 追加して消せば元の姿。Undo で戻したのに «変更あり» のままだと嘘になる。
    scene::Scene scene;
    scene.CreateGameObject("Keep");
    SceneDirtyTracker tracker;
    tracker.CaptureClean(scene);

    scene::GameObject& added = scene.CreateGameObject("Temp");
    ASSERT_TRUE(tracker.Evaluate(scene));

    scene.DestroyGameObject(added.GetID());

    EXPECT_FALSE(tracker.Evaluate(scene));
}

TEST_F(SceneDirtyTrackerTest, MarkDirtyForcesTheFlagWithoutLookingAtTheScene)
{
    scene::Scene scene;
    SceneDirtyTracker tracker;
    tracker.CaptureClean(scene);

    tracker.MarkDirty();

    EXPECT_TRUE(tracker.IsDirty());
}

TEST_F(SceneDirtyTrackerTest, EvaluateOverridesAManualMarkWhenTheSceneIsUnchanged)
{
    /// @note MarkDirty はシリアライズに出ない変更を拾う逃げ道だが、次の Evaluate で
    ///       «中身は同じ» と判定されれば取り消される。この性質を知らずに MarkDirty だけに
    ///       頼ると、保存を促す機会を落とす。
    scene::Scene scene;
    SceneDirtyTracker tracker;
    tracker.CaptureClean(scene);
    tracker.MarkDirty();

    EXPECT_FALSE(tracker.Evaluate(scene));
}

TEST_F(SceneDirtyTrackerTest, ResetForgetsTheBaseline)
{
    scene::Scene scene;
    SceneDirtyTracker tracker;
    tracker.CaptureClean(scene);
    scene.CreateGameObject("Player");
    ASSERT_TRUE(tracker.Evaluate(scene));

    tracker.Reset();

    EXPECT_FALSE(tracker.IsDirty());
    /// @note 基準が無いので、次の Evaluate は «今の姿» を基準として取り直す。
    EXPECT_FALSE(tracker.Evaluate(scene));
}

TEST_F(SceneDirtyTrackerTest, EvaluateWithoutABaselineAdoptsTheCurrentScene)
{
    /// @note 基準を取らないまま呼ばれても «変更あり» と言わない。開いた直後がこれ。
    scene::Scene scene;
    scene.CreateGameObject("Player");

    SceneDirtyTracker tracker;
    EXPECT_FALSE(tracker.Evaluate(scene));

    scene.CreateGameObject("Enemy");
    EXPECT_TRUE(tracker.Evaluate(scene));
}

TEST_F(SceneDirtyTrackerTest, PreparedSaveOwnsTheCapturedValuesAndCleanHash)
{
    const auto path = (ProjectRoot() / "captured.scene").generic_string();
    scene::Scene scene;
    auto& object = scene.CreateGameObject("Object");
    object.SetActive(false);
    auto save = editor::SceneIO::PrepareSave(scene, path, false);
    ASSERT_TRUE(save);
    object.SetActive(true);
    const auto result = save();
    ASSERT_TRUE(result.succeeded);

    auto loaded = scene::SceneSerializer::LoadData(path);
    ASSERT_NE(loaded, nullptr);
    const auto* saved = loaded->FindByGuid(object.instanceId);
    ASSERT_NE(saved, nullptr);
    EXPECT_FALSE(saved->activeSelf());

    SceneDirtyTracker tracker;
    tracker.CaptureCleanHash(result.contentHash);
    EXPECT_FALSE(tracker.Evaluate(*loaded, path));
    EXPECT_TRUE(tracker.Evaluate(scene, path));
}

TEST_F(SceneDirtyTrackerTest, PreparedSaveRejectsAnExternalFileCreatedWhileSaving)
{
    const auto diskPath = ProjectRoot() / "external.scene";
    const auto path = diskPath.generic_string();
    scene::Scene scene;
    scene.CreateGameObject("Editor object");
    auto save = editor::SceneIO::PrepareSave(scene, path, false, util::FileSystem::LastWriteTime(diskPath));
    ASSERT_TRUE(save);
    ASSERT_TRUE(util::FileSystem::WriteText(path, "external content"));

    const auto result = save();
    EXPECT_FALSE(result.succeeded);
    EXPECT_TRUE(result.diskChanged);
    std::string text;
    ASSERT_TRUE(util::FileSystem::ReadText(path, text));
    EXPECT_EQ(text, "external content");
}

TEST_F(SceneDirtyTrackerTest, BackupIncludesTerrainWithoutWritingTheAsset)
{
    const auto path = (ProjectRoot() / "backup.scene").generic_string();
    const auto assetPath = (ProjectRoot() / "shared.terrain").generic_string();
    scene::TerrainComponent existingTerrain;
    existingTerrain.InitFlat(0.0f);
    ASSERT_TRUE(scene::TerrainAssetSerializer::Save(existingTerrain, assetPath));
    scene::Scene scene;
    auto& terrain = scene.CreateGameObject("Ground").AddComponent<scene::TerrainComponent>();
    terrain.terrainAssetPath = assetPath;
    terrain.InitFlat(0.25f);
    auto save = editor::SceneIO::PrepareSave(scene, path, true);
    ASSERT_TRUE(save);
    terrain.InitFlat(0.5f);
    ASSERT_TRUE(save().succeeded);

    scene::TerrainComponent assetTerrain;
    ASSERT_TRUE(scene::TerrainAssetSerializer::Load(assetPath, assetTerrain));
    ASSERT_FALSE(assetTerrain.heightData.empty());
    EXPECT_FLOAT_EQ(assetTerrain.heightData.front(), 0.0f);
    auto loaded = scene::SceneSerializer::LoadData(path);
    ASSERT_NE(loaded, nullptr);
    auto* object = loaded->Find("Ground");
    ASSERT_NE(object, nullptr);
    const auto* restored = object->GetComponent<scene::TerrainComponent>();
    ASSERT_NE(restored, nullptr);
    ASSERT_FALSE(restored->heightData.empty());
    EXPECT_FLOAT_EQ(restored->GetHeightAt(0.0f, 0.0f), 0.25f);
}

TEST_F(SceneDirtyTrackerTest, PrefabSubtreeCaptureDoesNotInspectUnrelatedScripts)
{
    scene::Scene scene;
    auto& root = scene.CreateGameObject("Prefab root");
    auto& child = scene.CreateGameObject("Prefab child");
    child.SetParent(root);
    child.SetActive(false);
    auto& unrelated = scene.CreateGameObject("Unrelated object");
    unrelated.AddComponent<scene::ScriptComponent>().scripts.emplace_back().savedScriptId = "invalid";

    const std::string snapshot = scene::SceneSerializer::CapturePassiveSubtree(scene, root.GetID());
    ASSERT_FALSE(snapshot.empty());
    auto loaded = scene::SceneSerializer::LoadDataFromText(snapshot, {});
    ASSERT_NE(loaded, nullptr);
    ASSERT_EQ(loaded->GameObjectCount(), 2u);
    const auto* savedChild = loaded->FindByGuid(child.instanceId);
    ASSERT_NE(savedChild, nullptr);
    EXPECT_FALSE(savedChild->activeSelf());
    ASSERT_NE(savedChild->GetParent(), nullptr);
    EXPECT_EQ(savedChild->GetParent()->instanceId, root.instanceId);
}

TEST_F(SceneDirtyTrackerTest, BulkActiveEditKeepsOneUndoAndPreservesEditorHiding)
{
    scene::Scene scene;
    editor::UndoStack undo;
    editor::EditorContext context;
    context.activeScene = &scene;
    context.undoStack = &undo;
    bool dirty = false;
    context.markSceneDirty = [&dirty] { dirty = true; };
    auto& object = scene.CreateGameObject("Visible object");
    auto& hidden = scene.CreateGameObject("Hidden in editor");
    hidden.SetActive(false);
    context.editorHiddenGuids.emplace(hidden.instanceId, true);
    const std::array selection{object.GetID(), hidden.GetID()};

    editor::SetGameObjectsActive(context, selection, false, "Disable selection");
    EXPECT_TRUE(dirty);
    EXPECT_FALSE(object.activeSelf());
    EXPECT_FALSE(editor::GetAuthoringActive(context, hidden));
    ASSERT_TRUE(undo.Undo());
    EXPECT_FALSE(undo.CanUndo());
    EXPECT_TRUE(object.activeSelf());
    EXPECT_TRUE(editor::GetAuthoringActive(context, hidden));
    EXPECT_FALSE(hidden.activeSelf());
    ASSERT_TRUE(undo.Redo());
    EXPECT_FALSE(object.activeSelf());
    EXPECT_FALSE(editor::GetAuthoringActive(context, hidden));
    EXPECT_FALSE(hidden.activeSelf());
}

} /// @note namespace fbzz::tests
