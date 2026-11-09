/// @file    SceneEditCapacityTests.cpp
/// @brief   エディターの複製・貼り付け・UI 作成が旧固定容量を超えて動作する契約。
/// @author  Hasegawa Jin
/// @date    2026-10-02
#include <TestKit/TestKit.hpp>
#include <TestKit/Editor/EditorFixture.hpp>
#include <Editor/Util/ObjectCreation.hpp>
#include <Editor/Util/ObjectPresets.hpp>
#include <Editor/Util/SceneEditUtils.hpp>
#include <Engine/Scene/Components/UICanvas.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Engine/Scene/ScriptComponent.hpp>
#include <array>
#include <memory>
#include <string>
#include <vector>

namespace fbzz::tests {
class SceneEditCapacityTest : public testkit::EditorFixture {
protected:
    bool GrowPopulation(scene::Scene& target)
    {
        while (target.GameObjectCount() < 4097)
            if (!target.TryCreateGameObject("Existing")) return false;
        return true;
    }

    void TrackDirty()
    {
        Context().markSceneDirty = [this] { ++m_dirtyCalls; };
    }

    int m_dirtyCalls = 0;
};

TEST_F(SceneEditCapacityTest, DuplicateAndPastePreserveHierarchyBeyondOldCapacity)
{
    auto& target = AttachScene();
    auto& root = target.CreateGameObject("Root");
    auto& child = target.CreateGameObject("Child");
    child.SetParent(root);
    ASSERT_EQ(child.GetParent(), &root);
    Context().selectedEntities = {root.GetID(), child.GetID()};
    editor::CopySelectedToClipboard(Context());
    ASSERT_TRUE(editor::HasGameObjectClipboard());
    ASSERT_TRUE(GrowPopulation(target));
    TrackDirty();
    const auto count = target.GameObjectCount();

    (void)editor::MakeDuplicateSelectedCommand(Context());
    ASSERT_EQ(target.GameObjectCount(), count + 2);
    (void)editor::MakePasteClipboardCommand(Context());

    EXPECT_EQ(target.GameObjectCount(), count + 4);
    EXPECT_EQ(root.GetChildCount(), 1);
    EXPECT_EQ(child.GetParent(), &root);
    EXPECT_EQ(m_dirtyCalls, 2);
    auto* duplicate = target.Find("Root (Clone)");
    ASSERT_NE(duplicate, nullptr);
    EXPECT_EQ(duplicate->GetChildCount(), 1);
    ASSERT_EQ(Context().selectedEntities.size(), 1u);
    auto* pasted = target.GetGameObject(Context().selectedEntities.front());
    ASSERT_NE(pasted, nullptr);
    EXPECT_EQ(pasted->GetChildCount(), 1);
}

TEST_F(SceneEditCapacityTest, ParentAndChildSelectionDuplicatesOnlyTheOriginalHierarchyAfterGrowth)
{
    auto& target = AttachScene();
    auto& root = target.CreateGameObject("Root");
    auto& child = target.CreateGameObject("Child");
    child.SetParent(root);
    ASSERT_EQ(child.GetParent(), &root);
    Context().selectedEntities = {child.GetID(), root.GetID()};
    ASSERT_TRUE(GrowPopulation(target));
    TrackDirty();

    (void)editor::MakeDuplicateSelectedCommand(Context());

    EXPECT_EQ(target.GameObjectCount(), 4099u);
    auto* duplicate = target.Find("Root (Clone)");
    ASSERT_NE(duplicate, nullptr);
    ASSERT_EQ(duplicate->GetChildCount(), 1);
    EXPECT_EQ(duplicate->GetChild(0)->name, "Child");
    EXPECT_EQ(root.GetChildCount(), 1);
    ASSERT_EQ(Context().selectedEntities.size(), 1u);
    EXPECT_EQ(Context().selectedEntities.front(), duplicate->GetID());
    EXPECT_EQ(m_dirtyCalls, 1);
}

TEST_F(SceneEditCapacityTest, SelectedRootsKeepInputOrderAndIgnoreStaleIdsAndDescendants)
{
    auto& target = AttachScene();
    auto& ancestor = target.CreateGameObject("Ancestor");
    auto& intermediate = target.CreateGameObject("Intermediate");
    intermediate.SetParent(ancestor);
    auto& child = target.CreateGameObject("Child");
    child.SetParent(intermediate);
    auto& sibling = target.CreateGameObject("Sibling");
    sibling.SetParent(intermediate);
    const auto stale = target.CreateGameObject("Removed").GetID();
    ASSERT_TRUE(target.DestroyGameObject(stale));
    const auto unrelated = target.CreateGameObject("Unrelated").GetID();
    ASSERT_EQ(stale.index, unrelated.index);
    const std::array selection{child.GetID(), unrelated, sibling.GetID(), stale, ancestor.GetID(), unrelated};

    EXPECT_EQ(editor::CollectHierarchyRoots(target, selection),
        (std::vector<scene::EntityID>{unrelated, ancestor.GetID()}));
}

TEST_F(SceneEditCapacityTest, UIPresetCreatesAutomaticCanvasBeyondOldCapacity)
{
    auto& target = AttachScene();
    auto& existing = target.CreateGameObject("ExistingSelection");
    Context().selectedEntities = {existing.GetID()};
    ASSERT_TRUE(GrowPopulation(target));
    TrackDirty();
    editor::CreateObjectRequest request;
    request.key = "ui.button";
    std::string code;
    std::string message;

    EXPECT_TRUE(editor::ValidateCreateObjectRequest(Context(), request, code, message));
    ASSERT_NE(editor::MakeCreateObjectCommand(Context(), request, "Create Button", true), nullptr);

    EXPECT_EQ(target.GameObjectCount(), 4100u);
    auto* canvas = target.Find("Canvas");
    auto* button = target.Find("Button");
    ASSERT_NE(canvas, nullptr);
    ASSERT_NE(button, nullptr);
    EXPECT_EQ(button->GetParent(), canvas);
    EXPECT_TRUE(Context().activeUICanvas.IsValid());
    EXPECT_EQ(Context().selectedEntities.front(), button->GetID());
    EXPECT_EQ(m_dirtyCalls, 1);
}

TEST_F(SceneEditCapacityTest, ObjectCreationAndWeatherPresetWorkAfterGrowth)
{
    auto& target = AttachScene();
    ASSERT_TRUE(GrowPopulation(target));
    TrackDirty();
    editor::CreateObjectRequest request;
    request.key = "empty";
    int notifications = 0;
    auto notify = [&notifications](const std::vector<std::string>&) { ++notifications; };

    ASSERT_NE(editor::MakeCreateObjectCommand(Context(), request, "Create Empty", true, notify), nullptr);
    const auto* weather = editor::FindObjectPreset("env.weather");
    ASSERT_NE(weather, nullptr);
    ASSERT_NE(weather->create(Context()), nullptr);
    EXPECT_EQ(notifications, 1);
    EXPECT_EQ(m_dirtyCalls, 1);
    EXPECT_EQ(target.GameObjectCount(), 4100u);
}

TEST_F(SceneEditCapacityTest, ExistingCanvasIsReusedAfterGrowth)
{
    auto& target = AttachScene();
    auto& canvas = target.CreateGameObject("Canvas");
    canvas.AddComponent<scene::UICanvas>();
    Context().activeUICanvas = canvas.GetID();
    ASSERT_TRUE(GrowPopulation(target));
    TrackDirty();
    editor::CreateObjectRequest request;
    request.key = "ui.button";

    ASSERT_NE(editor::MakeCreateObjectCommand(Context(), request, "Create Button", true), nullptr);

    EXPECT_EQ(target.GameObjectCount(), 4099u);
    auto* button = target.Find("Button");
    ASSERT_NE(button, nullptr);
    EXPECT_EQ(button->GetParent(), &canvas);
    EXPECT_EQ(button->GetChildCount(), 1);
    EXPECT_EQ(Context().activeUICanvas, canvas.GetID());
    EXPECT_EQ(m_dirtyCalls, 1);
}

} /// @note namespace fbzz::tests
