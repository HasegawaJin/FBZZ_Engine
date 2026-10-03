/// @file    SceneEditCapacityTests.cpp
/// @brief   エディターの複製・貼り付け・UI 作成が必要数を先行確認する契約。
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
#include <memory>
#include <string>

namespace fbzz::tests {
namespace {

class EditorCapacityProbe final : public scene::Script {
public:
    int runtimeCounter = 37;
    int serializeCalls = 0;
    void OnBeforeSerialize() override { ++serializeCalls; }
};

} /// @note namespace

class SceneEditCapacityTest : public testkit::EditorFixture {
protected:
    bool FillLeaving(scene::Scene& target, std::size_t available)
    {
        while (target.RemainingEntityCapacity() > available)
            if (!target.TryCreateGameObject("Existing")) return false;
        return true;
    }

    void TrackDirty()
    {
        Context().markSceneDirty = [this] { ++m_dirtyCalls; };
    }

    int m_dirtyCalls = 0;
};

TEST_F(SceneEditCapacityTest, DuplicateAndPasteRejectWholeHierarchyBeforeChangingSelection)
{
    auto& target = AttachScene();
    auto& root = target.CreateGameObject("Root");
    auto& child = target.CreateGameObject("Child");
    child.SetParent(root);
    ASSERT_EQ(child.GetParent(), &root);
    Context().selectedEntities = {root.GetID(), child.GetID()};
    editor::CopySelectedToClipboard(Context());
    auto probe = std::make_unique<EditorCapacityProbe>();
    auto* existingScript = probe.get();
    probe->SetContext(&target, &root);
    scene::ScriptEntry entry;
    entry.script = std::move(probe);
    root.AddComponent<scene::ScriptComponent>().scripts.push_back(std::move(entry));
    ASSERT_TRUE(editor::HasGameObjectClipboard());
    ASSERT_TRUE(FillLeaving(target, 1));
    TrackDirty();
    const auto selection = Context().selectedEntities;
    const auto count = target.GameObjectCount();

    EXPECT_EQ(editor::MakeDuplicateSelectedCommand(Context()), nullptr);
    EXPECT_EQ(editor::DuplicateHierarchyRecursive(Context(), root.GetID(), {}, true), scene::EntityID::INVALID);
    EXPECT_EQ(editor::MakePasteClipboardCommand(Context()), nullptr);

    EXPECT_EQ(target.GameObjectCount(), count);
    EXPECT_EQ(target.RemainingEntityCapacity(), 1u);
    EXPECT_EQ(Context().selectedEntities, selection);
    EXPECT_EQ(root.GetChildCount(), 1);
    EXPECT_EQ(child.GetParent(), &root);
    EXPECT_EQ(m_dirtyCalls, 0);
    EXPECT_EQ(existingScript->runtimeCounter, 37);
    EXPECT_EQ(existingScript->serializeCalls, 0);
    EXPECT_EQ(root.GetComponent<scene::ScriptComponent>()->scripts.front().script.get(), existingScript);
    EXPECT_EQ(target.Find("Root (Clone)"), nullptr);
    EXPECT_EQ(target.Find("Root (Copy)"), nullptr);
}

TEST_F(SceneEditCapacityTest, ParentAndChildSelectionDuplicatesOnlyTheOriginalHierarchyAtBoundary)
{
    auto& target = AttachScene();
    auto& root = target.CreateGameObject("Root");
    auto& child = target.CreateGameObject("Child");
    child.SetParent(root);
    ASSERT_EQ(child.GetParent(), &root);
    Context().selectedEntities = {child.GetID(), root.GetID()};
    ASSERT_TRUE(FillLeaving(target, 2));
    TrackDirty();

    (void)editor::MakeDuplicateSelectedCommand(Context());

    EXPECT_EQ(target.RemainingEntityCapacity(), 0u);
    auto* duplicate = target.Find("Root (Clone)");
    ASSERT_NE(duplicate, nullptr);
    ASSERT_EQ(duplicate->GetChildCount(), 1);
    EXPECT_EQ(duplicate->GetChild(0)->name, "Child");
    EXPECT_EQ(root.GetChildCount(), 1);
    ASSERT_EQ(Context().selectedEntities.size(), 1u);
    EXPECT_EQ(Context().selectedEntities.front(), duplicate->GetID());
    EXPECT_EQ(m_dirtyCalls, 1);
}

TEST_F(SceneEditCapacityTest, UIPresetAccountsForAutomaticCanvasBeforeCreation)
{
    auto& target = AttachScene();
    auto& existing = target.CreateGameObject("ExistingSelection");
    Context().selectedEntities = {existing.GetID()};
    ASSERT_TRUE(FillLeaving(target, 2));
    TrackDirty();
    editor::CreateObjectRequest request;
    request.key = "ui.button";
    std::string code;
    std::string message;

    EXPECT_FALSE(editor::ValidateCreateObjectRequest(Context(), request, code, message));
    EXPECT_EQ(code, "SCENE_CAPACITY");
    EXPECT_EQ(editor::MakeCreateObjectCommand(Context(), request, "Create Button", true), nullptr);

    EXPECT_EQ(target.RemainingEntityCapacity(), 2u);
    EXPECT_EQ(target.Find("Canvas"), nullptr);
    EXPECT_EQ(target.Find("Button"), nullptr);
    EXPECT_FALSE(Context().activeUICanvas.IsValid());
    EXPECT_EQ(Context().selectedEntities.front(), existing.GetID());
    EXPECT_EQ(m_dirtyCalls, 0);
}

TEST_F(SceneEditCapacityTest, CapacityFailureIsNotReportedAsSettingsOnlyPresetSuccess)
{
    auto& target = AttachScene();
    ASSERT_TRUE(FillLeaving(target, 0));
    TrackDirty();
    editor::CreateObjectRequest request;
    request.key = "empty";
    int notifications = 0;
    auto notify = [&notifications](const std::vector<std::string>&) { ++notifications; };

    EXPECT_EQ(editor::MakeCreateObjectCommand(Context(), request, "Create Empty", true, notify), nullptr);
    const auto* weather = editor::FindObjectPreset("env.weather");
    ASSERT_NE(weather, nullptr);
    EXPECT_EQ(weather->create(Context()), nullptr);
    EXPECT_EQ(notifications, 0);
    EXPECT_EQ(m_dirtyCalls, 0);
    EXPECT_EQ(target.GameObjectCount(), scene::Scene::MAX_ENTITIES);
}

TEST_F(SceneEditCapacityTest, ExistingCanvasAllowsButtonToUseTwoRemainingSlots)
{
    auto& target = AttachScene();
    auto& canvas = target.CreateGameObject("Canvas");
    canvas.AddComponent<scene::UICanvas>();
    Context().activeUICanvas = canvas.GetID();
    ASSERT_TRUE(FillLeaving(target, 2));
    TrackDirty();
    editor::CreateObjectRequest request;
    request.key = "ui.button";

    ASSERT_NE(editor::MakeCreateObjectCommand(Context(), request, "Create Button", true), nullptr);

    EXPECT_EQ(target.RemainingEntityCapacity(), 0u);
    auto* button = target.Find("Button");
    ASSERT_NE(button, nullptr);
    EXPECT_EQ(button->GetParent(), &canvas);
    EXPECT_EQ(button->GetChildCount(), 1);
    EXPECT_EQ(Context().activeUICanvas, canvas.GetID());
    EXPECT_EQ(m_dirtyCalls, 1);
}

} /// @note namespace fbzz::tests
