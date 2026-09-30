/// @file    ScriptRequirementReviewTests.cpp
/// @brief   必須設定の修正導線と古い対象への移動拒否を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-29
#include <TestKit/TestKit.hpp>
#include <TestKit/Editor/EditorFixture.hpp>
#include <Editor/ImGuiReflector.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <Engine/Scene/ScriptComponent.hpp>
#include <imgui_internal.h>

namespace fbzz::tests {
namespace {
using scene::GameObject;
class ReviewProbe : public scene::Script {
    FBZZ_SCRIPT(ReviewProbe)
public:
    FBZZ_REQUIRED_REF(GameObject, target, "Target")
};
FBZZ_REFLECT(ReviewProbe)

class ReviewNestedProbe : public scene::Script {
public:
    scene::EntityRef reference;
    void Reflect(scene::IReflector& r) override
    {
        r.BeginField("settings", "Localized settings");
        r.BeginObject("Localized settings");
        r.BeginField("targets", "Localized targets");
        (void)r.BeginObjectList("Localized targets", 1);
        r.BeginObjectElement(0);
        r.BeginField("target", "Localized target");
        r.RequireReference("Localized target", reference, "GameObject");
        r.Field("Localized target", reference);
        r.EndField();
        r.EndObjectElement();
        (void)r.EndObjectList();
        r.EndField();
        r.EndObject();
        r.EndField();
    }
};
} /// @note namespace

class ScriptRequirementReviewTest : public testkit::EditorFixture {
protected:
    editor::OperatorRegistry registry;
    editor::UndoStack undo;
    std::string openedPanel;
    void SetUp() override
    {
        EditorFixture::SetUp();
        Context().operators = &registry;
        Context().undoStack = &undo;
        editor::RegisterScriptRequirementOperators(registry);
        editor::EditorOperator panel;
        panel.id = "panel.focus";
        panel.params = {{"panel", editor::OpParamType::String, "Panel"}};
        panel.exec = [this](editor::OpContext&, const editor::OpArgs& args) {
            openedPanel = args.GetString("panel");
            return editor::OpResult::Ok();
        };
        registry.Register(std::move(panel));
    }
    editor::OpResult Check()
    {
        return editor::InvokeOperator(Context(), "script.requirements.validate");
    }
    editor::OpResult Reveal(int index, int revision = -1)
    {
        editor::OpArgs args;
        args.Set("index", index);
        args.Set("revision", revision < 0 ? Context().scriptRequirementReview.revision : revision);
        return editor::InvokeOperator(Context(), "script.requirements.reveal", args);
    }
};

TEST_F(ScriptRequirementReviewTest, ReportsObjectScriptFieldAndReasonThroughOperator)
{
    auto& go = AttachScene().CreateGameObject("Player");
    auto& script = go.AddScript<ReviewProbe>();
    const auto result = Check();
    ASSERT_TRUE(result.ok);
    ASSERT_EQ(Context().scriptRequirementReview.rows.size(), 1u);
    const auto& row = Context().scriptRequirementReview.rows.front();
    EXPECT_EQ(row.issue.objectName, "Player");
    EXPECT_EQ(row.issue.scriptType, "ReviewProbe");
    EXPECT_EQ(row.issue.fieldKey, "target");
    EXPECT_FALSE(row.issue.reason.empty());
    EXPECT_EQ(row.scriptId, script.InspectionId());
    EXPECT_EQ(openedPanel, "Script Requirements");
    ASSERT_NE(result.data.Find("issues"), nullptr);
    EXPECT_EQ(result.data.Find("issues")->AsArray().size(), 1u);
}

TEST_F(ScriptRequirementReviewTest, DuplicateTypesAndReorderingRevealTheExactInstance)
{
    auto& go = AttachScene().CreateGameObject("Player");
    go.AddScript<ReviewProbe>();
    auto& second = go.AddScript<ReviewProbe>();
    const auto secondId = second.InspectionId();
    ASSERT_TRUE(Check().ok);
    auto& entries = go.GetComponent<scene::ScriptComponent>()->scripts;
    std::swap(entries[0], entries[1]);
    Context().selectedAssetPath = "Assets/example.mat";
    Context().animationGraphSelection.type = editor::EditorContext::AnimationGraphSelection::Type::State;
    ASSERT_TRUE(Reveal(1).ok);
    EXPECT_EQ(Context().PrimarySelected(), go.GetID());
    EXPECT_EQ(Context().hierarchyRevealTarget, go.GetID());
    EXPECT_TRUE(Context().selectedAssetPath.empty());
    EXPECT_EQ(Context().animationGraphSelection.type, editor::EditorContext::AnimationGraphSelection::Type::None);
    EXPECT_EQ(Context().scriptRequirementReview.focus.scriptId, secondId);
    EXPECT_EQ(Context().scriptRequirementReview.focus.field, "target");
    EXPECT_TRUE(Context().scriptRequirementReview.focus.unlockPending);
    EXPECT_TRUE(Context().scriptRequirementReview.focus.scrollPending);
    EXPECT_EQ(openedPanel, "Inspector");
}

TEST_F(ScriptRequirementReviewTest, ReplacedScriptCannotBeSelectedUsingOldResult)
{
    auto& go = AttachScene().CreateGameObject("Player");
    go.AddScript<ReviewProbe>();
    ASSERT_TRUE(Check().ok);
    go.GetComponent<scene::ScriptComponent>()->scripts[0].script = std::make_unique<ReviewProbe>();
    EXPECT_EQ(Reveal(0).errorCode, "STALE_TARGET");
    EXPECT_TRUE(Context().selectedEntities.empty());
}

TEST_F(ScriptRequirementReviewTest, RemovedScriptCannotBeSelectedUsingOldResult)
{
    auto& go = AttachScene().CreateGameObject("Player");
    go.AddScript<ReviewProbe>();
    ASSERT_TRUE(Check().ok);
    go.GetComponent<scene::ScriptComponent>()->scripts.clear();
    EXPECT_EQ(Reveal(0).errorCode, "STALE_TARGET");
}

TEST_F(ScriptRequirementReviewTest, ClearedSceneCannotReuseOldResult)
{
    auto& scene = AttachScene();
    scene.CreateGameObject("Player").AddScript<ReviewProbe>();
    ASSERT_TRUE(Check().ok);
    scene.Clear();
    scene.CreateGameObject("Player").AddScript<ReviewProbe>();
    EXPECT_EQ(Reveal(0).errorCode, "STALE_TARGET");
}

TEST_F(ScriptRequirementReviewTest, DifferentSceneAndPathInvalidateReview)
{
    AttachScene().CreateGameObject("Player").AddScript<ReviewProbe>();
    ASSERT_TRUE(Check().ok);
    Context().currentScenePath = "Other.scene";
    EXPECT_EQ(Reveal(0).errorCode, "STALE_REVIEW");
    Context().currentScenePath.clear();
    scene::Scene other;
    Context().activeScene = &other;
    EXPECT_EQ(Reveal(0).errorCode, "STALE_REVIEW");
    Context().activeScene = AttachedScene();
}

TEST_F(ScriptRequirementReviewTest, FixAndRevalidateClearsIssuesAndInvalidatesOldRows)
{
    auto& go = AttachScene().CreateGameObject("Player");
    auto& script = go.AddScript<ReviewProbe>();
    ASSERT_TRUE(Check().ok);
    const int revision = Context().scriptRequirementReview.revision;
    ASSERT_TRUE(Reveal(0).ok);
    script.target.ref.id = go.GetID();
    ASSERT_TRUE(Check().ok);
    EXPECT_TRUE(Context().scriptRequirementReview.rows.empty());
    EXPECT_TRUE(Context().scriptRequirementReview.focus.scriptId.empty());
    EXPECT_EQ(Reveal(0, revision).errorCode, "STALE_REVIEW");
}

TEST_F(ScriptRequirementReviewTest, UnavailableScriptsAreNotReportedAsFullyChecked)
{
    auto& go = AttachScene().CreateGameObject("Player");
    go.AddComponent<scene::ScriptComponent>().scripts.emplace_back();
    ASSERT_TRUE(Check().ok);
    EXPECT_EQ(Context().scriptRequirementReview.unavailableScripts, 1);
    EXPECT_EQ(Context().scriptRequirementReview.scriptsChecked, 0);
    EXPECT_TRUE(Context().scriptRequirementReview.rows.empty());
}

TEST_F(ScriptRequirementReviewTest, NoSceneAndReloadDisableOperations)
{
    EXPECT_FALSE(Check().ok);
    AttachScene().CreateGameObject("Player").AddScript<ReviewProbe>();
    ASSERT_TRUE(Check().ok);
    Context().scriptReloadBusy = true;
    EXPECT_FALSE(Check().ok);
    EXPECT_FALSE(Reveal(0).ok);
    EXPECT_TRUE(Context().selectedEntities.empty());
}

TEST_F(ScriptRequirementReviewTest, InvalidRowsDoNotChangeSelection)
{
    AttachScene().CreateGameObject("Player").AddScript<ReviewProbe>();
    ASSERT_TRUE(Check().ok);
    EXPECT_EQ(Reveal(-1).errorCode, "BAD_INDEX");
    EXPECT_EQ(Reveal(8).errorCode, "BAD_INDEX");
    EXPECT_TRUE(Context().selectedEntities.empty());
}

TEST_F(ScriptRequirementReviewTest, NestedRequirementUsesPersistentKeysIncludingListIndex)
{
    AttachScene().CreateGameObject("Player").AddScript<ReviewNestedProbe>();
    ASSERT_TRUE(Check().ok);
    ASSERT_EQ(Context().scriptRequirementReview.rows.size(), 1u);
    EXPECT_EQ(Context().scriptRequirementReview.rows[0].issue.fieldKey, "settings.targets.0.target");
}

TEST_F(ScriptRequirementReviewTest, NestedInspectorFocusOpensClosedFoldsWithoutEditingValues)
{
    auto* previous = ImGui::GetCurrentContext();
    auto* imgui = ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = {1000, 800};
    io.DeltaTime = 1.0f / 60;
    unsigned char* pixels = nullptr;
    int width = 0, height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    ReviewNestedProbe script;
    for (int frame = 0; frame < 3; ++frame) {
        ImGui::NewFrame();
        ImGui::SetNextWindowSize({700, 300});
        ImGui::Begin("Requirement inspector");
        editor::ImGuiReflector reflector;
        reflector.m_focusField = "settings.targets.0.target";
        reflector.m_focusScrollRequested = frame == 1;
        if (frame == 0) ImGui::SetNextItemOpen(false, ImGuiCond_Always);
        script.Reflect(reflector);
        EXPECT_EQ(reflector.m_focusFound, frame != 0);
        EXPECT_FALSE(reflector.m_changed);
        EXPECT_FALSE(script.reference.IsValid());
        ImGui::End();
        ImGui::Render();
    }
    ImGui::DestroyContext(imgui);
    ImGui::SetCurrentContext(previous);
}
} /// @note namespace fbzz::tests
