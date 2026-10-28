/// @file    WaterInspectorRuntimeTests.cpp
/// @brief   WaterのInspectorメニュー・Clipboard・Undoがownerの接触実行状態を共有しないことを検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-09
#include <TestKit/TestKit.hpp>
#include <TestKit/Editor/EditorFixture.hpp>
#include <Editor/Panels/InspectorPanel.hpp>
#include <Editor/PlayModeController.hpp>
#include <Editor/Op/OperatorGroups.hpp>
#include <Editor/Util/Selection.hpp>
#include <Editor/Util/UndoStack.hpp>
#include <Editor/Util/WaterAuthoringValues.hpp>
#include <Engine/Core/Scheduler/SystemContext.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Scene/Components/WaterComponent.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Engine/Scene/Systems/WaterInteractionSystem.hpp>
#include <Engine/Scene/Systems/WaterSystem.hpp>
#include <Physics/World.hpp>
#include <imgui.h>
#include <imgui_internal.h>
#include <memory>
#include <string>

namespace fbzz::tests {
namespace {
class WaterRuntimeInspector final : public editor::InspectorPanel {
public:
    void Draw(editor::EditorContext& context) { OnRenderContent(context); }
};

class WaterInspectorRuntimeTest : public testkit::EditorFixture {
protected:
    void SetUp() override
    {
        EditorFixture::SetUp();
        AttachScene();
        Context().undoStack = &m_undo;
        m_previous = ImGui::GetCurrentContext();
        m_imgui = ImGui::CreateContext();
        ImGui::SetCurrentContext(m_imgui);
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = {1800, 8000};
        io.DeltaTime = 1.0f / 60;
        io.ConfigInputTrickleEventQueue = false;
        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    }
    void TearDown() override
    {
        if (Context().activeScene) scene::ClearWaterInteractions(*Context().activeScene);
        m_panel.OnShutdown();
        m_play.Cancel();
        Context().playMode = nullptr;
        Context().undoStack = nullptr;
        ImGui::DestroyContext(m_imgui);
        ImGui::SetCurrentContext(m_previous);
        EditorFixture::TearDown();
    }
    scene::EntityID Water(const char* name, float x)
    {
        auto& source = *Context().activeScene;
        auto& object = source.CreateGameObject(name);
        const auto id = object.GetID();
        auto& water = object.AddComponent<scene::WaterComponent>();
        water.enableGerstnerWaves = false;
        scene::FlushWorldTransforms(source);
        scene::PrepareWaterSceneSurfaces(source);
        EXPECT_TRUE(scene::QueueWaterInteraction(source, id,
            {scene::WaterInteractionKind::BUBBLES, {x, -1, 0}, {}, 0.2f, 0.5f, 5}));
        Tick();
        return id;
    }
    void Tick()
    {
        ++Time::frameCount;
        SystemContext context{*Context().activeScene, m_world, nullptr, nullptr, 0.1f, 0.1f, true, true};
        m_system.Update(context);
    }
    void Frame(scene::EntityID id, const char* action, bool popup)
    {
        editor::SelectEntity(Context(), id);
        ImGuiWindow* popupWindow = popup && !m_imgui->OpenPopupStack.empty()
            ? m_imgui->OpenPopupStack.back().Window : nullptr;
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize({1800, 8000});
        ImGui::Begin("Water runtime inspector test", nullptr,
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize);
        ImGuiID activate = 0;
        if (action && popupWindow) activate = popupWindow->GetID(action);
        else if (action) {
            ImGui::PushID("Water");
            ImGui::PushID("Water");
            activate = ImGui::GetID(action);
            ImGui::PopID();
            ImGui::PopID();
        }
        if (activate) {
            m_imgui->NavActivateId = activate;
            m_imgui->NavActivateDownId = activate;
            m_imgui->NavActivatePressedId = activate;
            m_imgui->NavActivateFlags = ImGuiActivateFlags_None;
        }
        ImGui::LogToBuffer();
        m_panel.Draw(Context());
        m_lastDrawText = m_imgui->LogBuffer.c_str();
        ImGui::LogFinish();
        ImGui::End();
        ImGui::Render();
    }
    void Menu(scene::EntityID id, const char* action)
    {
        Frame(id, "##opt", false);
        ASSERT_FALSE(m_imgui->OpenPopupStack.empty()) << m_lastDrawText;
        ASSERT_NE(m_imgui->OpenPopupStack.back().Window, nullptr);
        Frame(id, action, true);
    }
    ImGuiContext* m_previous = nullptr;
    ImGuiContext* m_imgui = nullptr;
    WaterRuntimeInspector m_panel;
    editor::UndoStack m_undo;
    editor::PlayModeController m_play;
    physics::World m_world;
    scene::WaterInteractionSystem m_system;
    std::string m_lastDrawText;
};
}

TEST_F(WaterInspectorRuntimeTest, CapturedClipboardValuesHaveNoRuntimeAndApplyPreservesTargetEffects)
{
    const auto sourceId = Water("Source", 1);
    const auto targetId = Water("Target", -1);
    auto* source = Context().activeScene->GetComponent<scene::WaterComponent>(sourceId);
    auto* target = Context().activeScene->GetComponent<scene::WaterComponent>(targetId);
    const auto frame = scene::WaterInteractionSnapshot(*target);
    const auto emitters = target->interactionRuntime.emitters;
    source->interactionSettings.bubbleLifetime = 9;
    const auto captured = editor::CaptureWaterAuthoringValues(*source);
    EXPECT_EQ(captured.interactionRuntime.sceneStore, nullptr);
    EXPECT_EQ(scene::WaterInteractionSnapshot(captured), nullptr);
    EXPECT_TRUE(captured.ripples.empty());
    editor::ApplyWaterAuthoringValues(*target, captured);
    EXPECT_FLOAT_EQ(target->interactionSettings.bubbleLifetime, 9);
    EXPECT_EQ(scene::WaterInteractionSnapshot(*target), frame);
    EXPECT_EQ(target->interactionRuntime.emitters[0].id, emitters[0].id);
    EXPECT_EQ(frame->owner, targetId);
}

TEST_F(WaterInspectorRuntimeTest, RegisteredResetAndUndoPreserveCurrentWaterRuntime)
{
    const auto id = Water("Target", -1);
    auto* water = Context().activeScene->GetComponent<scene::WaterComponent>(id);
    water->extentX = 24;
    const auto emitter = water->interactionRuntime.emitters[0].id;
    const auto frame = scene::WaterInteractionSnapshot(*water);
    editor::OperatorRegistry registry;
    editor::RegisterInspectorOperators(registry);
    Context().operators = &registry;
    editor::OpArgs args;
    args.Set("node", Context().activeScene->GetGameObject(id)->instanceId);
    args.Set("component", std::string("WaterComponent"));
    const auto result = editor::InvokeOperator(Context(), "component.reset", args);
    Context().operators = nullptr;
    ASSERT_TRUE(result.ok) << result.message;
    EXPECT_EQ(water->interactionRuntime.emitters[0].id, emitter);
    EXPECT_EQ(scene::WaterInteractionSnapshot(*water), frame);
    Tick();
    const auto current = scene::WaterInteractionSnapshot(*water);
    ASSERT_TRUE(m_undo.Undo());
    EXPECT_FLOAT_EQ(water->extentX, 24);
    EXPECT_EQ(scene::WaterInteractionSnapshot(*water), current);
}

TEST_F(WaterInspectorRuntimeTest, ResetMenuUndoAndRedoRetainTheCurrentTargetRuntimeRatherThanItsOldSnapshot)
{
    const auto id = Water("Target", -1);
    auto* water = Context().activeScene->GetComponent<scene::WaterComponent>(id);
    water->extentX = 24;
    const auto emitter = water->interactionRuntime.emitters[0].id;
    Menu(id, "Reset");
    water = Context().activeScene->GetComponent<scene::WaterComponent>(id);
    EXPECT_FLOAT_EQ(water->extentX, 80);
    EXPECT_EQ(water->interactionRuntime.emitters[0].id, emitter);
    ASSERT_EQ(m_undo.GetHistorySize(), 1u);
    Tick();
    const auto current = scene::WaterInteractionSnapshot(*water);
    ASSERT_TRUE(m_undo.Undo());
    EXPECT_FLOAT_EQ(water->extentX, 24);
    EXPECT_EQ(scene::WaterInteractionSnapshot(*water), current);
    ASSERT_TRUE(m_undo.Redo());
    EXPECT_FLOAT_EQ(water->extentX, 80);
    EXPECT_EQ(scene::WaterInteractionSnapshot(*water), current);
}

TEST_F(WaterInspectorRuntimeTest, EditClipboardPasteNeverSharesAnotherOwnersFrameOrOrphansItsEmitter)
{
    const auto a = Water("Source", 1);
    const auto b = Water("Target", -1);
    auto& source = *Context().activeScene;
    auto* first = source.GetComponent<scene::WaterComponent>(a);
    auto* second = source.GetComponent<scene::WaterComponent>(b);
    first->interactionSettings.minimumMotionSpeed = 9;
    const auto ownFrame = scene::WaterInteractionSnapshot(*second);
    const auto ownEmitter = second->interactionRuntime.emitters[0].id;
    const auto objectCount = source.GameObjectCount();
    Menu(a, "Copy Component");
    Menu(b, "Paste Component Values");
    second = source.GetComponent<scene::WaterComponent>(b);
    EXPECT_FLOAT_EQ(second->interactionSettings.minimumMotionSpeed, 9);
    EXPECT_EQ(scene::WaterInteractionSnapshot(*second), ownFrame);
    EXPECT_EQ(second->interactionRuntime.emitters[0].id, ownEmitter);
    ASSERT_TRUE(scene::QueueWaterInteraction(source, b,
        {scene::WaterInteractionKind::BUBBLES, {-1, -1, 0}, {}, 0.2f, 0.5f, 5}));
    Tick();
    EXPECT_EQ(source.GameObjectCount(), objectCount);
    EXPECT_EQ(second->interactionRuntime.emitters[0].id, ownEmitter);
    const auto current = scene::WaterInteractionSnapshot(*second);
    ASSERT_TRUE(Context().undoStack->Undo());
    EXPECT_EQ(scene::WaterInteractionSnapshot(*second), current);
}

TEST_F(WaterInspectorRuntimeTest, PlayShowsOnlyManagedSettingsAndDoesNotOfferNativeWaterMenus)
{
    auto& edit = *AttachedScene();
    auto& object = edit.CreateGameObject("Water");
    const auto original = object.GetID();
    auto& originalWater = object.AddComponent<scene::WaterComponent>();
    originalWater.enableGerstnerWaves = false;
    originalWater.interactionSettings.bubbleMaterialPath.clear();
    originalWater.interactionSettings.sedimentMaterialPath.clear();
    m_play.Play(edit);
    ASSERT_TRUE(m_play.IsPlaying()) << m_play.Diagnostic();
    Context().activeScene = m_play.PlayScene();
    Context().playMode = &m_play;
    Context().undoStack = m_play.TemporaryUndo();
    const auto id = Context().activeScene->Find("Water")->GetID();
    scene::FlushWorldTransforms(*Context().activeScene);
    scene::PrepareWaterSceneSurfaces(*Context().activeScene);
    ASSERT_TRUE(scene::QueueWaterInteraction(*Context().activeScene, id,
        {scene::WaterInteractionKind::BUBBLES, {0, -1, 0}, {}, 0.2f, 0.5f, 5}));
    Tick();
    auto* water = Context().activeScene->GetComponent<scene::WaterComponent>(id);
    const auto before = scene::WaterInteractionSnapshot(*water);
    Frame(id, nullptr, false);
    EXPECT_NE(m_lastDrawText.find("Play の C# 設定は一時編集"), std::string::npos);
    EXPECT_EQ(m_lastDrawText.find("Water Type (.mat)"), std::string::npos);
    EXPECT_TRUE(m_imgui->OpenPopupStack.empty());
    EXPECT_EQ(scene::WaterInteractionSnapshot(*water), before);
    EXPECT_EQ(Context().undoStack->GetHistorySize(), 0u);
    m_play.Stop(edit);
    Context().activeScene = &edit;
    ASSERT_TRUE(m_play.ApplyPendingRestore(edit));
    EXPECT_EQ(edit.GetGameObject(original), &object);
    EXPECT_EQ(scene::WaterInteractionSnapshot(originalWater), nullptr);
}
}
