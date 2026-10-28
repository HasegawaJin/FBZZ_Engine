/// @file    ManagedCompletionTests.cpp
/// @brief   SDK catalog と Inspector 共通設定の実 CoreCLR 接続を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-04
#include <TestKit/TestKit.hpp>
#include <TestKit/Editor/EditorFixture.hpp>
#include <Editor/Ai/EditorBusDispatcher.hpp>
#include <Editor/Ai/EditorBusProtocol.hpp>
#include <Editor/Op/ScriptConfigure.hpp>
#include <Editor/Op/ScriptApiCatalog.hpp>
#include <Editor/Op/OperatorGroups.hpp>
#include <Editor/Util/ManagedScriptSettings.hpp>
#include <Editor/Util/ManagedSettingsCodec.hpp>
#include <Editor/Util/UndoStack.hpp>
#include <Editor/Util/SceneDirtyTracker.hpp>
#include <Editor/Util/SceneIO.hpp>
#include <Editor/PlayModeController.hpp>
#include <Editor/Util/PlaySettingsAdoption.hpp>
#include <Editor/Util/ManagedPrimitiveWidgets.hpp>
#include <Engine/Scene/Managed/ManagedScriptRuntime.hpp>
#include <Engine/Scene/Managed/ManagedScriptAdapter.hpp>
#include <Engine/Scene/ScriptComponent.hpp>
#include <Engine/Scene/ScriptFactory.hpp>
#include <Engine/Scene/SceneSerializer.hpp>
#include <Engine/Scene/ProjectRuntime.hpp>
#include <Engine/Scene/ScriptEvent.hpp>
#include <Engine/Scene/ScriptValidation.hpp>
#include <Engine/Core/Concurrency/TaskSystem.hpp>
#include "../../../Editor/src/Panels/Inspector/ManagedSettingsReflector.hpp"
#include <imgui_internal.h>
#include <limits>

namespace fbzz::tests {
struct ManagedSettingsReflectorTestAccess {
    static void Queue(editor::ManagedSettingsReflector& reflector, const char* path, editor::OpData value)
    {
        editor::OpData patch = editor::OpData::MakeObject(); patch.Set("path", path); patch.Set("value", std::move(value));
        reflector.QueuePatch(std::move(patch));
    }
    static bool HasConflict(const editor::ManagedSettingsReflector& reflector) { return reflector.m_draft->revisionConflict; }
    static bool HasPendingPatches(const editor::ManagedSettingsReflector& reflector)
    { return !reflector.m_draft->createPatches.AsArray().empty(); }
    static uint64_t EditSession(const editor::ManagedSettingsReflector& reflector) { return reflector.m_draft->editSession; }
    static std::shared_ptr<editor::ManagedSettingsDraft> Draft(const editor::ManagedSettingsReflector& reflector)
    { return reflector.m_draft; }
    static uint64_t Generation(const editor::ManagedSettingsReflector& reflector) { return reflector.m_draft->target.generation; }
    static std::size_t PinnedRevisionCount(const editor::ManagedSettingsReflector& reflector)
    { return reflector.m_draft->targetRevisions.size(); }
    static const std::string& FirstRevision(const editor::ManagedSettingsReflector& reflector)
    { return reflector.m_draft->targetRevisions.front(); }
    static void HoldInput(editor::ManagedSettingsReflector& reflector, const char* path, const char* field)
    {
        ImGuiContext* context = ImGui::GetCurrentContext();
        if (!context || !context->CurrentWindow) return;
        ImGui::PushID(path);
        ImGui::PushID(field);
        const unsigned int id = context->CurrentWindow->GetID("##primitive");
        ImGui::PopID();
        ImGui::PopID();
        ImGui::SetActiveID(id, context->CurrentWindow);
        reflector.m_draft->activeId = id;
    }
    static void ReleaseInput(editor::ManagedSettingsReflector& reflector)
    {
        if (ImGui::GetCurrentContext() && ImGui::GetActiveID() == reflector.m_draft->activeId) ImGui::ClearActiveID();
        reflector.m_draft->activeId = 0;
    }
    static void LeaveActiveInput(editor::ManagedSettingsReflector& reflector)
    {
        if (ImGui::GetCurrentContext() && ImGui::GetActiveID() == reflector.m_draft->activeId) ImGui::ClearActiveID();
    }
    static std::size_t StaleCount(const editor::ManagedSettingsReflector& reflector)
    { return reflector.m_draft->staleDrafts.size(); }
    static int64_t StaleInt(const editor::ManagedSettingsReflector& reflector, std::size_t draftIndex, std::size_t patchIndex)
    { return reflector.m_draft->staleDrafts[draftIndex]->createPatches.AsArray()[patchIndex].Find("value")->AsInt64(); }
    static uint64_t StaleGeneration(const editor::ManagedSettingsReflector& reflector, std::size_t index)
    { return reflector.m_draft->staleDrafts[index]->target.generation; }
    static uint64_t StaleEditSession(const editor::ManagedSettingsReflector& reflector, std::size_t index)
    { return reflector.m_draft->staleDrafts[index]->editSession; }
    static unsigned int StaleActiveId(const editor::ManagedSettingsReflector& reflector, std::size_t index)
    { return reflector.m_draft->staleDrafts[index]->activeId; }
    static bool StaleSchemaContains(const editor::ManagedSettingsReflector& reflector, std::size_t index, const char* name)
    { return reflector.m_draft->staleDrafts[index]->schemas.contains(name); }
    static void DiscardStale(editor::ManagedSettingsReflector& reflector) { reflector.DiscardStaleDrafts(); }
    static std::string CopyStale(editor::ManagedSettingsReflector& reflector) { return reflector.CopyStaleDrafts(); }
    static int64_t PendingInt(const editor::ManagedSettingsReflector& reflector, std::size_t index)
    { return reflector.m_draft->createPatches.AsArray()[index].Find("value")->AsInt64(); }
    static std::string ReferenceLabel(const editor::ManagedSettingsReflector& reflector,
        const editor::OpData& schema, const editor::OpData& address)
    { return reflector.ComponentReferenceLabel(schema, address); }
    static void DrawRuntimeFields(editor::ManagedSettingsReflector& reflector) { reflector.DrawFields(false); }
};
namespace {

struct IgnoreFields : scene::IReflector {
    void Field(const char*, float&) override {}
    void Field(const char*, int&) override {}
    void Field(const char*, bool&) override {}
    void Field(const char*, math::Vector2&) override {}
    void Field(const char*, math::Vector3&) override {}
    void Field(const char*, math::Vector4&) override {}
    void Field(const char*, std::string&) override {}
    void Field(const char*, math::Quaternion&) override {}
};
struct ImGuiFrame {
    ImGuiContext* previous = ImGui::GetCurrentContext();
    ImGuiContext* current = ImGui::CreateContext();
    explicit ImGuiFrame(float viewportHeight = 700.0f)
    {
        ImGui::SetCurrentContext(current);
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr; io.DisplaySize = {1000.0f, viewportHeight > 700.0f ? viewportHeight + 100.0f : 800.0f}; io.DeltaTime = 1.0f / 60.0f;
        unsigned char* pixels = nullptr; int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({0.0f, 0.0f}); ImGui::SetNextWindowSize({900.0f, viewportHeight});
        ImGui::Begin("Managed settings draft regression");
    }
    ~ImGuiFrame()
    {
        ImGui::End(); ImGui::Render();
        ImGui::DestroyContext(current); ImGui::SetCurrentContext(previous);
    }
};
} /// @note namespace
class ManagedCompletionTest : public testkit::EditorFixture {
protected:
    editor::OperatorRegistry m_registry;
    editor::UndoStack m_undo;
    void SetUp() override
    {
        EditorFixture::SetUp(); AttachScene();
        Context().operators = &m_registry; Context().undoStack = &m_undo;
        editor::RegisterScriptConfigureOperators(m_registry);
        editor::RegisterScriptApiCatalogOperator(m_registry);
        editor::RegisterScriptResumeOperators(m_registry);
    }
    void TearDown() override
    {
        m_undo.Clear(); m_registry.Clear();
        scene::ManagedScriptRuntime::Instance().Unload(AttachedScene());
        EditorFixture::TearDown();
    }
    void Load()
    {
        auto& runtime = scene::ManagedScriptRuntime::Instance();
        ASSERT_TRUE(runtime.Initialize(FBZZ_MANAGED_HOST_TEST_DIRECTORY)) << runtime.LastDiagnostic();
        ASSERT_TRUE(runtime.Load(FBZZ_MANAGED_FIXTURE_TEST_ASSEMBLY, true)) << runtime.LastDiagnostic();
    }
    std::unique_ptr<scene::Script> Create(scene::GameObject& owner, std::string_view name, const std::string& savedScriptId)
    {
        std::string type;
        if (!scene::ScriptFactory::ResolveTypeName(name, type)) return nullptr;
        auto script = scene::ScriptFactory::Create(type, savedScriptId);
        if (script) script->SetContext(AttachedScene(), &owner);
        return script;
    }
    editor::ai::JsonValue Query(const std::string& type, const std::string& member)
    {
        editor::ai::JsonValue payload = editor::ai::JsonValue::MakeObject();
        payload.Set("t", editor::ai::JsonValue("script.api.catalog"));
        payload.Set("type", editor::ai::JsonValue(type)); payload.Set("member", editor::ai::JsonValue(member));
        editor::ai::JsonValue request = editor::ai::JsonValue::MakeObject();
        request.Set("protocol", editor::ai::JsonValue(editor::ai::kEditorProtocol));
        request.Set("id", editor::ai::JsonValue("sdk-api-test")); request.Set("kind", editor::ai::JsonValue("query"));
        request.Set("dryRun", editor::ai::JsonValue(false)); request.Set("payload", payload);
        editor::ai::EditorBusDispatcher dispatcher(Context());
        const auto response = editor::ai::ParseJson(dispatcher.Handle(editor::ai::SerializeJson(request)));
        EXPECT_TRUE(response.has_value());
        return response.value_or(editor::ai::JsonValue{});
    }
};

TEST_F(ManagedCompletionTest, KeepsTheEditorAndDirtyBaselineWhenManagedSnapshotSerializationFails)
{
    Load();
    auto& owner = AttachedScene()->CreateGameObject("InvalidSnapshot");
    auto& entry = owner.AddComponent<scene::ScriptComponent>().scripts.emplace_back();
    entry.script = Create(owner, "ManagedFixtures.SaveFailureComponent", entry.savedScriptId);
    ASSERT_NE(entry.script, nullptr);
    auto* adapter = scene::ManagedScriptRuntime::Instance().FindAdapter(entry.script.get());
    ASSERT_NE(adapter, nullptr);
    editor::SceneDirtyTracker tracker;
    tracker.CaptureClean(*AttachedScene());
    ASSERT_FALSE(tracker.IsDirty());
    ASSERT_NE(entry.serialized, nullptr);
    const std::string preserved = entry.serialized->fieldsToml;
    ASSERT_TRUE(adapter->ApplySettingsJson("{\"failureMode\":1}"));
    editor::PlayModeController play;

    play.Play(*AttachedScene());
    tracker.CaptureClean(*AttachedScene());

    EXPECT_TRUE(play.IsInEditor());
    EXPECT_FALSE(play.HasPendingRestore());
    EXPECT_TRUE(editor::SceneIO::Serialize(*AttachedScene()).empty());
    EXPECT_EQ(entry.serialized->fieldsToml, preserved);
    EXPECT_TRUE(tracker.IsDirty());
    EXPECT_TRUE(tracker.Evaluate(*AttachedScene()));
    ASSERT_TRUE(adapter->ApplySettingsJson("{\"failureMode\":0,\"settings\":{\"value\":1,\"label\":\"valid\",\"next\":null}}"));
    EXPECT_FALSE(tracker.Evaluate(*AttachedScene()));
}

TEST_F(ManagedCompletionTest, PlayParksOriginalObjectsRuntimeAndManagedUndoWithoutRecreation)
{
    Load(); auto& owner = AttachedScene()->CreateGameObject("ParkedOwner");
    const auto ownerId = owner.GetID(); const auto lifetime = AttachedScene()->GetRenderSceneGeneration(); const std::string guid = owner.instanceId;
    auto& entry = owner.AddComponent<scene::ScriptComponent>().scripts.emplace_back();
    entry.script = Create(owner, "ManagedFixtures.HealthComponent", entry.savedScriptId); ASSERT_NE(entry.script, nullptr);
    auto* original = scene::ManagedScriptRuntime::Instance().FindAdapter(entry.script.get()); ASSERT_NE(original, nullptr);
    const uint64_t instance = original->InstanceId(), generation = original->Generation(); const std::string savedId = entry.savedScriptId;
    auto target = editor::CaptureManagedSettingsTarget(Context(), owner, entry); editor::OpData fields = editor::OpData::MakeObject(); fields.Set("health", 4);
    ASSERT_TRUE(editor::ApplyManagedInspectorFields(Context(), target, fields).ok); const auto cursor = m_undo.GetCursor();
    scene::ProjectRuntime editRuntime; editRuntime.BindExternalScene(AttachedScene()); const auto* originalPhysics = &editRuntime.GetPhysicsWorld();
    int editEvents = 0, playEvents = 0;
    auto originalSubscription = scene::ScriptEventBus::SubscribeRaw(entry.script.get(), "park-test", [&editEvents](const void*) { editEvents++; });
    editor::PlayModeController play; play.Play(*AttachedScene()); ASSERT_TRUE(play.IsPlaying()) << play.Diagnostic();
    ASSERT_NE(play.PlayScene(), AttachedScene()); ASSERT_NE(play.PlayRuntime(), &editRuntime); EXPECT_NE(&play.PlayRuntime()->GetPhysicsWorld(), originalPhysics);
    EXPECT_TRUE(AttachedScene()->IsExecutionParked()); EXPECT_EQ(AttachedScene()->GetRenderSceneGeneration(), lifetime); EXPECT_EQ(AttachedScene()->GetGameObject(ownerId), &owner);
    auto* cloneOwner = play.PlayScene()->FindByGuid(guid); ASSERT_NE(cloneOwner, nullptr);
    auto& cloneEntry = cloneOwner->GetComponent<scene::ScriptComponent>()->scripts.front(); EXPECT_EQ(cloneEntry.savedScriptId, savedId);
    auto* clone = scene::ManagedScriptRuntime::Instance().FindAdapter(cloneEntry.script.get()); ASSERT_NE(clone, nullptr); EXPECT_NE(clone->InstanceId(), instance);
    auto cloneSubscription = scene::ScriptEventBus::SubscribeRaw(cloneEntry.script.get(), "park-test", [&playEvents](const void*) { playEvents++; });
    scene::ScriptEventBus::PublishRaw("park-test", nullptr); EXPECT_EQ(editEvents, 0); EXPECT_EQ(playEvents, 1);
    ASSERT_TRUE(clone->ApplySettingsJson("{\"health\":1}")); EXPECT_EQ(m_undo.GetCursor(), cursor);
    play.Stop(*play.PlayScene()); ASSERT_TRUE(play.HasPendingRestore()); ASSERT_TRUE(play.ApplyPendingRestore(*AttachedScene())) << play.Diagnostic();
    EXPECT_FALSE(AttachedScene()->IsExecutionParked()); EXPECT_TRUE(play.IsInEditor()); EXPECT_EQ(play.PlayRuntime(), nullptr); EXPECT_EQ(play.PlayScene(), nullptr);
    EXPECT_EQ(AttachedScene()->GetRenderSceneGeneration(), lifetime); EXPECT_EQ(AttachedScene()->GetGameObject(ownerId), &owner);
    EXPECT_EQ(scene::ManagedScriptRuntime::Instance().FindAdapter(entry.script.get()), original); EXPECT_EQ(original->InstanceId(), instance); EXPECT_EQ(original->Generation(), generation);
    EXPECT_EQ(&editRuntime.GetPhysicsWorld(), originalPhysics); EXPECT_EQ(m_undo.GetCursor(), cursor); EXPECT_EQ(m_undo.GetHistorySize(), 1u);
    scene::ScriptEventBus::PublishRaw("park-test", nullptr); EXPECT_EQ(editEvents, 1); EXPECT_EQ(playEvents, 1);
    ASSERT_TRUE(m_undo.Undo()) << m_undo.LastDiagnostic(); ASSERT_TRUE(m_undo.Redo()) << m_undo.LastDiagnostic();
    std::string json; ASSERT_TRUE(original->ReadSettingsPage("/health", 0, 1, json)); editor::OpData page; ASSERT_TRUE(editor::ReadManagedSettingsJson(json, page));
    EXPECT_EQ(page.Find("value")->AsInt(), 4);
    scene::ScriptEventBus::Unsubscribe(originalSubscription); scene::ScriptEventBus::Unsubscribe(cloneSubscription);
}

TEST_F(ManagedCompletionTest, PassivePlayCaptureAndFailedCloneValidationNeverMutateEditCallbacksOrSnapshots)
{
    Load(); auto& owner = AttachedScene()->CreateGameObject("PassiveParkSource");
    auto& entry = owner.AddComponent<scene::ScriptComponent>().scripts.emplace_back();
    entry.script = Create(owner, "ManagedFixtures.SaveFailureComponent", entry.savedScriptId); ASSERT_NE(entry.script, nullptr);
    auto* original = scene::ManagedScriptRuntime::Instance().FindAdapter(entry.script.get()); ASSERT_NE(original, nullptr);
    ASSERT_TRUE(original->ApplySettingsJson("{\"failureMode\":1}"));
    entry.serialized = std::make_unique<scene::SerializedScriptData>(scene::SerializedScriptData{entry.script->GetTypeName(), "preserve = 17\n", true, entry.savedScriptId});
    const std::string previous = entry.serialized->fieldsToml;
    std::string diagnostic; EXPECT_FALSE(scene::SceneSerializer::CapturePassiveClone(*AttachedScene(), diagnostic).empty());
    EXPECT_EQ(entry.serialized->fieldsToml, previous);
    editor::PlayModeController play; play.Play(*AttachedScene()); EXPECT_TRUE(play.IsInEditor()); EXPECT_FALSE(play.Diagnostic().empty()); EXPECT_FALSE(AttachedScene()->IsExecutionParked());
    EXPECT_EQ(entry.serialized->fieldsToml, previous); EXPECT_EQ(scene::ManagedScriptRuntime::Instance().FindAdapter(entry.script.get()), original);
    std::string json; ASSERT_TRUE(original->ReadSettingsPage("/settings/value", 0, 1, json)); editor::OpData page; ASSERT_TRUE(editor::ReadManagedSettingsJson(json, page));
    EXPECT_EQ(page.Find("value")->IsFloat() ? page.Find("value")->AsFloat() : static_cast<float>(page.Find("value")->AsInt()), 1.0f);
}

TEST_F(ManagedCompletionTest, CommonEnabledControlSavesExactScriptAndSupportsCheckedUndo)
{
    Load();
    auto& owner = AttachedScene()->CreateGameObject("EnabledOwner");
    auto& component = owner.AddComponent<scene::ScriptComponent>();
    for (int index = 0; index < 2; ++index) {
        auto& entry = component.scripts.emplace_back();
        entry.script = Create(owner, "ManagedFixtures.HealthComponent", entry.savedScriptId);
        ASSERT_NE(entry.script, nullptr);
    }
    const auto target = editor::CaptureManagedSettingsTarget(Context(), owner, component.scripts[1]);
    editor::OpArgs args;
    args.Set("node", owner.instanceId); args.Set("scriptId", target.scriptId); args.Set("enabled", false);
    int dirtyCalls = 0;
    Context().markSceneDirty = [&dirtyCalls] { ++dirtyCalls; };

    ASSERT_TRUE(editor::CanInvokeOperator(Context(), "script.set_enabled", args));
    auto disabled = editor::InvokeOperator(Context(), "script.set_enabled", args);
    ASSERT_TRUE(disabled.ok) << disabled.message;
    EXPECT_FALSE(disabled.data.Find("enabled")->AsBool());
    EXPECT_TRUE(component.scripts[0].script->enabled);
    EXPECT_FALSE(component.scripts[1].script->enabled);
    EXPECT_EQ(m_undo.GetHistorySize(), 1U);
    EXPECT_TRUE(Context().sceneDirty); EXPECT_EQ(dirtyCalls, 1);
    ASSERT_TRUE(editor::InvokeOperator(Context(), "script.set_enabled", args).noChange);
    EXPECT_EQ(m_undo.GetHistorySize(), 1U); EXPECT_EQ(dirtyCalls, 1);

    const std::string saved = scene::SceneSerializer::SaveToText(*AttachedScene());
    ASSERT_FALSE(saved.empty());
    auto restored = scene::SceneSerializer::LoadDataFromText(saved, "");
    ASSERT_NE(restored, nullptr);
    auto* restoredOwner = restored->FindByGuid(owner.instanceId);
    ASSERT_NE(restoredOwner, nullptr);
    auto* restoredScripts = restoredOwner->GetComponent<scene::ScriptComponent>();
    ASSERT_NE(restoredScripts, nullptr); ASSERT_EQ(restoredScripts->scripts.size(), 2U);
    ASSERT_NE(restoredScripts->scripts[0].script, nullptr);
    ASSERT_NE(restoredScripts->scripts[1].script, nullptr);
    EXPECT_TRUE(restoredScripts->scripts[0].script->enabled);
    EXPECT_FALSE(restoredScripts->scripts[1].script->enabled);

    ASSERT_TRUE(m_undo.Undo()) << m_undo.LastDiagnostic();
    EXPECT_TRUE(component.scripts[1].script->enabled);
    ASSERT_TRUE(m_undo.Redo()) << m_undo.LastDiagnostic();
    EXPECT_FALSE(component.scripts[1].script->enabled);
    component.scripts[1].script = Create(owner, "ManagedFixtures.HealthComponent", component.scripts[1].savedScriptId);
    ASSERT_NE(component.scripts[1].script, nullptr);
    const auto cursor = m_undo.GetCursor();
    EXPECT_FALSE(m_undo.Undo()); EXPECT_EQ(m_undo.GetCursor(), cursor);
    EXPECT_TRUE(component.scripts[1].script->enabled);
    EXPECT_FALSE(editor::InvokeOperator(Context(), "script.set_enabled", args).ok);
    Context().markSceneDirty = {};
}

TEST_F(ManagedCompletionTest, PlayEnabledControlUsesTemporaryHistoryAndRestoresOriginalOnStop)
{
    Load();
    auto& owner = AttachedScene()->CreateGameObject("PlayEnabledOwner");
    auto& entry = owner.AddComponent<scene::ScriptComponent>().scripts.emplace_back();
    entry.script = Create(owner, "ManagedFixtures.HealthComponent", entry.savedScriptId);
    ASSERT_NE(entry.script, nullptr);
    const std::string guid = owner.instanceId;
    Context().sceneDirty = false;
    int dirtyCalls = 0;
    editor::PlayModeController play;
    play.Play(*AttachedScene()); ASSERT_TRUE(play.IsPlaying()) << play.Diagnostic();
    struct RestoreContext {
        editor::EditorContext& ctx; scene::Scene* original; editor::UndoStack& undo;
        ~RestoreContext() { ctx.activeScene = original; ctx.playMode = nullptr; ctx.undoStack = &undo; ctx.markSceneDirty = {}; }
    } restore{Context(), AttachedScene(), m_undo};
    Context().activeScene = play.PlayScene(); Context().playMode = &play; Context().undoStack = play.TemporaryUndo();
    Context().markSceneDirty = [&dirtyCalls] { ++dirtyCalls; };
    auto* clone = play.PlayScene()->FindByGuid(guid); ASSERT_NE(clone, nullptr);
    auto& cloneEntry = clone->GetComponent<scene::ScriptComponent>()->scripts.front();
    const auto target = editor::CaptureManagedSettingsTarget(Context(), *clone, cloneEntry);
    editor::OpArgs args;
    args.Set("node", guid); args.Set("scriptId", target.scriptId); args.Set("enabled", false);

    ASSERT_TRUE(editor::InvokeOperator(Context(), "script.set_enabled", args).ok);
    EXPECT_FALSE(cloneEntry.script->enabled); EXPECT_TRUE(entry.script->enabled);
    EXPECT_EQ(play.TemporaryUndo()->GetHistorySize(), 1U); EXPECT_EQ(m_undo.GetHistorySize(), 0U);
    ASSERT_TRUE(play.TemporaryUndo()->Undo()) << play.TemporaryUndo()->LastDiagnostic();
    EXPECT_TRUE(cloneEntry.script->enabled);
    ASSERT_TRUE(play.TemporaryUndo()->Redo()) << play.TemporaryUndo()->LastDiagnostic();
    EXPECT_FALSE(cloneEntry.script->enabled);
    EXPECT_FALSE(Context().sceneDirty); EXPECT_EQ(dirtyCalls, 0);

    play.Stop(*play.PlayScene()); Context().undoStack = &m_undo;
    ASSERT_TRUE(play.ApplyPendingRestore(*AttachedScene())) << play.Diagnostic();
    Context().activeScene = AttachedScene();
    EXPECT_TRUE(entry.script->enabled); EXPECT_EQ(m_undo.GetHistorySize(), 0U);
    EXPECT_FALSE(Context().sceneDirty); EXPECT_EQ(dirtyCalls, 0);
    EXPECT_FALSE(editor::InvokeOperator(Context(), "script.set_enabled", args).ok);
}

TEST_F(ManagedCompletionTest, EnabledControlCannotBypassFaultRecovery)
{
    Load();
    auto& owner = AttachedScene()->CreateGameObject("FaultedEnabledOwner");
    auto& entry = owner.AddComponent<scene::ScriptComponent>().scripts.emplace_back();
    entry.script = Create(owner, "ManagedFixtures.FaultRecoveryComponent", entry.savedScriptId);
    auto* adapter = scene::ManagedScriptRuntime::Instance().FindAdapter(entry.script.get());
    ASSERT_NE(adapter, nullptr); ASSERT_TRUE(adapter->RefreshOwnerBinding());
    ASSERT_TRUE(adapter->ApplySettingsJson("{\"target\":\"" + owner.instanceId + "\",\"failPhase\":4}"));
    adapter->OnAwake(); adapter->OnUpdate();
    ASSERT_TRUE(adapter->IsRuntimeFaulted());
    const auto diagnostic = adapter->FaultDiagnostic();
    const auto target = editor::CaptureManagedSettingsTarget(Context(), owner, entry);
    editor::OpArgs args;
    args.Set("node", owner.instanceId); args.Set("scriptId", target.scriptId); args.Set("enabled", true);

    EXPECT_FALSE(editor::CanInvokeOperator(Context(), "script.set_enabled", args));
    EXPECT_FALSE(editor::InvokeOperator(Context(), "script.set_enabled", args).ok);
    EXPECT_FALSE(adapter->enabled); EXPECT_TRUE(adapter->IsRuntimeFaulted());
    EXPECT_EQ(adapter->FaultDiagnostic(), diagnostic); EXPECT_EQ(m_undo.GetHistorySize(), 0U);
}

TEST_F(ManagedCompletionTest, LiveInspectorRefreshesCallbackValuesWithoutManualAction)
{
    Load();
    auto& owner = AttachedScene()->CreateGameObject("LiveInspectorOwner");
    auto& entry = owner.AddComponent<scene::ScriptComponent>().scripts.emplace_back();
    entry.script = Create(owner, "ManagedFixtures.FaultRecoveryComponent", entry.savedScriptId);
    auto* original = scene::ManagedScriptRuntime::Instance().FindAdapter(entry.script.get());
    ASSERT_NE(original, nullptr); ASSERT_TRUE(original->RefreshOwnerBinding());
    ASSERT_TRUE(original->ApplySettingsJson("{\"target\":\"" + owner.instanceId + "\"}"));
    const std::string guid = owner.instanceId;
    editor::PlayModeController play;
    play.Play(*AttachedScene()); ASSERT_TRUE(play.IsPlaying()) << play.Diagnostic();
    struct RestoreContext {
        editor::EditorContext& ctx; scene::Scene* original; editor::UndoStack& undo;
        ~RestoreContext() { ctx.activeScene = original; ctx.playMode = nullptr; ctx.undoStack = &undo; }
    } restore{Context(), AttachedScene(), m_undo};
    Context().activeScene = play.PlayScene(); Context().playMode = &play; Context().undoStack = play.TemporaryUndo();
    auto* clone = play.PlayScene()->FindByGuid(guid); ASSERT_NE(clone, nullptr);
    auto& cloneEntry = clone->GetComponent<scene::ScriptComponent>()->scripts.front();
    auto* adapter = scene::ManagedScriptRuntime::Instance().FindAdapter(cloneEntry.script.get());
    ASSERT_NE(adapter, nullptr); ASSERT_TRUE(adapter->RefreshOwnerBinding());
    editor::ManagedSettingsReflector reflector(Context(), *clone, cloneEntry);
    const auto draft = ManagedSettingsReflectorTestAccess::Draft(reflector);
    const auto draw = [&]() { ImGuiFrame frame; reflector.Draw(); reflector.Finish(); };

    draw();
    ASSERT_TRUE(draft->pages.contains("/updateCount#0:0"));
    const int before = draft->pages.at("/updateCount#0:0").Find("value")->AsInt();
    uint64_t revisionBefore = 0, revisionAfter = 0;
    ASSERT_TRUE(adapter->ReadSettingsRevision(revisionBefore));
    adapter->OnUpdate();
    ASSERT_FALSE(adapter->IsRuntimeFaulted()) << adapter->FaultDiagnostic();
    ASSERT_TRUE(adapter->ReadSettingsRevision(revisionAfter));
    EXPECT_EQ(revisionAfter, revisionBefore);

    draw();
    ASSERT_TRUE(draft->pages.contains("/updateCount#0:0"));
    EXPECT_EQ(draft->pages.at("/updateCount#0:0").Find("value")->AsInt(), before + 1);
}

TEST_F(ManagedCompletionTest, MixedEnabledSelectionUsesOneUndoAndPreservesIndividualStates)
{
    Load();
    std::vector<editor::ManagedSettingsTarget> targets;
    std::vector<scene::Script*> scripts;
    for (int index = 0; index < 2; ++index) {
        auto& owner = AttachedScene()->CreateGameObject("MixedEnabled" + std::to_string(index));
        auto& entry = owner.AddComponent<scene::ScriptComponent>().scripts.emplace_back();
        entry.script = Create(owner, "ManagedFixtures.HealthComponent", entry.savedScriptId);
        ASSERT_NE(entry.script, nullptr);
        targets.push_back(editor::CaptureManagedSettingsTarget(Context(), owner, entry)); scripts.push_back(entry.script.get());
    }
    ASSERT_TRUE(editor::ApplyManagedInspectorEnabled(Context(), {targets.front()}, false).ok);
    m_undo.Clear();

    const auto mixed = editor::ReadManagedInspectorEnabled(Context(), targets);
    EXPECT_TRUE(mixed.mixed); EXPECT_TRUE(mixed.editable); EXPECT_TRUE(mixed.diagnostic.empty());
    ASSERT_TRUE(editor::ApplyManagedInspectorEnabled(Context(), targets, true).ok);
    EXPECT_TRUE(scripts[0]->enabled); EXPECT_TRUE(scripts[1]->enabled);
    EXPECT_EQ(m_undo.GetHistorySize(), 1U);
    ASSERT_TRUE(m_undo.Undo()) << m_undo.LastDiagnostic();
    EXPECT_FALSE(scripts[0]->enabled); EXPECT_TRUE(scripts[1]->enabled);
    ASSERT_TRUE(m_undo.Redo()) << m_undo.LastDiagnostic();
    EXPECT_TRUE(scripts[0]->enabled); EXPECT_TRUE(scripts[1]->enabled);
    ASSERT_TRUE(editor::ApplyManagedInspectorEnabled(Context(), targets, false).ok);
    EXPECT_FALSE(scripts[0]->enabled); EXPECT_FALSE(scripts[1]->enabled);
    const auto disabled = editor::ReadManagedInspectorEnabled(Context(), targets);
    EXPECT_FALSE(disabled.mixed); EXPECT_FALSE(disabled.enabled); EXPECT_TRUE(disabled.editable);

    Context().managedBuildBusy = true;
    const auto busy = editor::ReadManagedInspectorEnabled(Context(), targets);
    EXPECT_FALSE(busy.editable); EXPECT_NE(busy.diagnostic.find("ビルド中"), std::string::npos);
    Context().managedBuildBusy = false;
    EXPECT_FALSE(editor::ApplyManagedInspectorEnabled(Context(), {targets.front(), targets.front()}, true).ok);
    EXPECT_FALSE(scripts[0]->enabled); EXPECT_FALSE(scripts[1]->enabled);
}

TEST_F(ManagedCompletionTest, EnabledBatchRejectsFaultedTargetsAndConflictedUndoAtomically)
{
    Load();
    std::vector<editor::ManagedSettingsTarget> targets;
    auto& healthy = AttachedScene()->CreateGameObject("HealthyEnabled");
    auto& first = healthy.AddComponent<scene::ScriptComponent>().scripts.emplace_back();
    first.script = Create(healthy, "ManagedFixtures.HealthComponent", first.savedScriptId);
    ASSERT_NE(first.script, nullptr);
    targets.push_back(editor::CaptureManagedSettingsTarget(Context(), healthy, first));
    auto& faulted = AttachedScene()->CreateGameObject("FaultedEnabled");
    auto& second = faulted.AddComponent<scene::ScriptComponent>().scripts.emplace_back();
    second.script = Create(faulted, "ManagedFixtures.FaultRecoveryComponent", second.savedScriptId);
    auto* adapter = scene::ManagedScriptRuntime::Instance().FindAdapter(second.script.get());
    ASSERT_NE(adapter, nullptr); ASSERT_TRUE(adapter->RefreshOwnerBinding());
    ASSERT_TRUE(adapter->ApplySettingsJson("{\"target\":\"" + faulted.instanceId + "\",\"failPhase\":4}"));
    targets.push_back(editor::CaptureManagedSettingsTarget(Context(), faulted, second));
    ASSERT_TRUE(editor::ApplyManagedInspectorEnabled(Context(), targets, false).ok);
    ASSERT_EQ(m_undo.GetHistorySize(), 1U);
    second.script = Create(faulted, "ManagedFixtures.FaultRecoveryComponent", second.savedScriptId);
    ASSERT_NE(second.script, nullptr);
    const auto cursor = m_undo.GetCursor();
    EXPECT_FALSE(m_undo.Undo()); EXPECT_EQ(m_undo.GetCursor(), cursor);
    EXPECT_FALSE(first.script->enabled); EXPECT_TRUE(second.script->enabled);
    m_undo.Clear();
    adapter = scene::ManagedScriptRuntime::Instance().FindAdapter(second.script.get());
    ASSERT_TRUE(adapter->RefreshOwnerBinding());
    ASSERT_TRUE(adapter->ApplySettingsJson("{\"target\":\"" + faulted.instanceId + "\",\"failPhase\":4}"));
    adapter->OnAwake(); adapter->OnUpdate(); ASSERT_TRUE(adapter->IsRuntimeFaulted());
    targets[1] = editor::CaptureManagedSettingsTarget(Context(), faulted, second);

    const auto state = editor::ReadManagedInspectorEnabled(Context(), targets);
    EXPECT_FALSE(state.editable); EXPECT_NE(state.diagnostic.find("停止原因"), std::string::npos);
    EXPECT_FALSE(editor::ApplyManagedInspectorEnabled(Context(), targets, true).ok);
    EXPECT_FALSE(first.script->enabled); EXPECT_FALSE(second.script->enabled);
    EXPECT_EQ(m_undo.GetHistorySize(), 0U);
}

TEST_F(ManagedCompletionTest, InspectorSkipsOffscreenValuesAndRefreshesThemWhenVisible)
{
    Load();
    auto& owner = AttachedScene()->CreateGameObject("ClippedInspector");
    auto& entry = owner.AddComponent<scene::ScriptComponent>().scripts.emplace_back();
    entry.script = Create(owner, "ManagedFixtures.HealthComponent", entry.savedScriptId);
    auto* adapter = scene::ManagedScriptRuntime::Instance().FindAdapter(entry.script.get());
    ASSERT_NE(adapter, nullptr);
    editor::ManagedSettingsReflector reflector(Context(), owner, entry);
    const auto draft = ManagedSettingsReflectorTestAccess::Draft(reflector);
    {
        ImGuiFrame frame;
        ImGui::SetCursorPosY(2000.0f);
        reflector.Draw(); reflector.Finish();
        EXPECT_TRUE(draft->pages.empty());
    }
    ASSERT_TRUE(adapter->ApplySettingsJson("{\"health\":7}"));
    {
        ImGuiFrame frame;
        reflector.Draw(); reflector.Finish();
        ASSERT_TRUE(draft->pages.contains("/health#0:0"));
        EXPECT_EQ(draft->pages.at("/health#0:0").Find("value")->AsInt(), 7);
    }
    EXPECT_EQ(m_undo.GetHistorySize(), 0U);
}

TEST_F(ManagedCompletionTest, CollapsedDiagnosticsKeepLongMessagesOutOfTheSettingsLayout)
{
    ImGuiFrame frame;
    const float firstY = ImGui::GetCursorPosY();
    editor::DrawManagedDiagnostic("短い停止理由", "short diagnostic");
    const float secondY = ImGui::GetCursorPosY();
    editor::DrawManagedDiagnostic("長い停止理由", std::string(16000, 'x'));
    EXPECT_NEAR(ImGui::GetCursorPosY() - secondY, secondY - firstY, 0.01f);
}

TEST_F(ManagedCompletionTest, MixedHeaderActivationEnablesAllWhileUnavailableChecksRemainReadOnly)
{
    ImGuiFrame frame;
    const auto activate = [&frame](const char* label) {
        ImGui::PushID(label);
        const ImGuiID id = ImGui::GetID("##en");
        ImGui::PopID();
        frame.current->NavActivateId = id;
        frame.current->NavActivateDownId = id;
        frame.current->NavActivatePressedId = id;
    };
    bool enabled = true;
    activate("Mixed header");
    const auto mixed = editor::widgets::ComponentHeader("Mixed header", IM_COL32(120, 190, 255, 255), &enabled, true, {}, {true, true, nullptr});
    EXPECT_TRUE(mixed.enabledChanged); EXPECT_TRUE(enabled);

    enabled = false;
    activate("Unavailable header");
    const auto unavailable = editor::widgets::ComponentHeader("Unavailable header", IM_COL32(120, 190, 255, 255), &enabled, true, {}, {false, false, "停止原因を修復してください"});
    EXPECT_FALSE(unavailable.enabledChanged); EXPECT_FALSE(enabled); EXPECT_TRUE(unavailable.open);
}

TEST_F(ManagedCompletionTest, PlayManagedSettingsUseTemporaryCheckedHistoryAndDiscardPreviewsOnStop)
{
    Load(); auto& owner = AttachedScene()->CreateGameObject("TemporarySettingsOwner");
    auto& entry = owner.AddComponent<scene::ScriptComponent>().scripts.emplace_back();
    entry.script = Create(owner, "ManagedFixtures.HealthComponent", entry.savedScriptId); ASSERT_NE(entry.script, nullptr);
    const std::string guid = owner.instanceId;
    auto originalTarget = editor::CaptureManagedSettingsTarget(Context(), owner, entry);
    editor::OpData fields = editor::OpData::MakeObject(); fields.Set("health", 4);
    ASSERT_TRUE(editor::ApplyManagedInspectorFields(Context(), originalTarget, fields).ok);
    const auto editCursor = m_undo.GetCursor(); Context().sceneDirty = false;
    int dirtyCalls = 0;
    editor::PlayModeController play; play.Play(*AttachedScene()); ASSERT_TRUE(play.IsPlaying()) << play.Diagnostic();
    struct RestoreContext {
        editor::EditorContext& ctx; scene::Scene* original; editor::UndoStack& undo;
        ~RestoreContext() { ctx.activeScene = original; ctx.playMode = nullptr; ctx.undoStack = &undo; ctx.markSceneDirty = {}; }
    } restore{Context(), AttachedScene(), m_undo};
    Context().activeScene = play.PlayScene(); Context().playMode = &play; Context().undoStack = play.TemporaryUndo();
    Context().markSceneDirty = [&dirtyCalls] { ++dirtyCalls; };
    auto* cloneOwner = play.PlayScene()->FindByGuid(guid); ASSERT_NE(cloneOwner, nullptr);
    auto& cloneEntry = cloneOwner->GetComponent<scene::ScriptComponent>()->scripts.front();
    auto cloneTarget = editor::CaptureManagedSettingsTarget(Context(), *cloneOwner, cloneEntry);
    fields.Set("health", 1);
    ASSERT_TRUE(editor::ApplyManagedInspectorFields(Context(), cloneTarget, fields).ok);
    EXPECT_EQ(play.TemporaryUndo()->GetHistorySize(), 1U); EXPECT_EQ(m_undo.GetCursor(), editCursor);
    EXPECT_TRUE(play.TemporarySettingsChanged()); EXPECT_FALSE(Context().sceneDirty); EXPECT_EQ(dirtyCalls, 0);
    ASSERT_TRUE(play.TemporaryUndo()->Undo()) << play.TemporaryUndo()->LastDiagnostic();
    ASSERT_TRUE(play.TemporaryUndo()->Redo()) << play.TemporaryUndo()->LastDiagnostic();
    EXPECT_FALSE(Context().sceneDirty); EXPECT_EQ(dirtyCalls, 0);
    int unsupportedExecutions = 0;
    editor::EditorOperator native; native.id = "test.native_mutation"; native.kind = editor::OpKind::Mutation;
    native.exec = [&unsupportedExecutions](editor::OpContext&, const editor::OpArgs&) { ++unsupportedExecutions; return editor::OpResult::Ok(); };
    m_registry.Register(std::move(native));
    EXPECT_FALSE(editor::InvokeOperator(Context(), "test.native_mutation").ok); EXPECT_EQ(unsupportedExecutions, 0);
    editor::OpData selector = editor::OpData::MakeObject(); selector.Set("id", guid);
    editor::OpData settings = editor::OpData::MakeObject(); settings.Set("objects", selector); settings.Set("script", cloneTarget.type);
    settings.Set("scriptId", cloneTarget.scriptId); fields.Set("health", 2); settings.Set("fields", fields);
    editor::OpArgs preview; preview.Set("settings", settings);
    auto prepared = editor::InvokeOperator(Context(), "script.configure.preview", preview); ASSERT_TRUE(prepared.ok) << prepared.message;
    const std::string token = prepared.data.Find("plan")->AsString();
    play.Stop(*play.PlayScene()); Context().undoStack = &m_undo;
    ASSERT_TRUE(play.ApplyPendingRestore(*AttachedScene())) << play.Diagnostic();
    Context().activeScene = AttachedScene();
    EXPECT_EQ(play.TemporaryUndo(), nullptr); EXPECT_EQ(m_undo.GetCursor(), editCursor);
    EXPECT_FALSE(Context().sceneDirty); EXPECT_EQ(dirtyCalls, 0);
    editor::OpArgs apply; apply.Set("plan", token);
    auto expired = editor::InvokeOperator(Context(), "script.configure.apply", apply);
    EXPECT_FALSE(expired.ok); EXPECT_EQ(expired.errorCode, "EXPIRED_PLAN");
    ASSERT_TRUE(m_undo.Undo()) << m_undo.LastDiagnostic(); ASSERT_TRUE(m_undo.Redo()) << m_undo.LastDiagnostic();
    auto* original = scene::ManagedScriptRuntime::Instance().FindAdapter(entry.script.get());
    std::string json; ASSERT_TRUE(original->ReadSettingsPage("/health", 0, 1, json)); editor::OpData page; ASSERT_TRUE(editor::ReadManagedSettingsJson(json, page));
    EXPECT_EQ(page.Find("value")->AsInt(), 4);
}

TEST_F(ManagedCompletionTest, ExactManagedBatchSelectsDuplicateSameTypeInstancesAndPreservesHistoryOnConflict)
{
    Load(); auto& owner = AttachedScene()->CreateGameObject("DuplicateBatchOwner");
    auto& component = owner.AddComponent<scene::ScriptComponent>();
    for (int index = 0; index < 2; ++index) { auto& entry = component.scripts.emplace_back(); entry.script = Create(owner, "ManagedFixtures.HealthComponent", entry.savedScriptId); ASSERT_NE(entry.script, nullptr); }
    auto* first = scene::ManagedScriptRuntime::Instance().FindAdapter(component.scripts[0].script.get());
    auto* second = scene::ManagedScriptRuntime::Instance().FindAdapter(component.scripts[1].script.get());
    ASSERT_TRUE(second->ApplySettingsJson("{\"health\":7}"));
    const auto health = [](scene::ManagedScriptAdapter* adapter) {
        std::string json; editor::OpData page;
        if (!adapter->ReadSettingsPage("/health", 0, 1, json) || !editor::ReadManagedSettingsJson(json, page)) return -1;
        return page.Find("value")->AsInt();
    };
    std::vector<editor::ManagedSettingsEdit> edits;
    for (size_t index = 0; index < component.scripts.size(); ++index) {
        editor::ManagedSettingsEdit edit; edit.target = editor::CaptureManagedSettingsTarget(Context(), owner, component.scripts[index]);
        editor::OpData patch = editor::OpData::MakeObject(); patch.Set("path", "/health"); patch.Set("value", index == 0 ? editor::OpData(2) : editor::OpData("invalid-int"));
        edit.patches.Push(std::move(patch)); edits.push_back(std::move(edit));
    }
    auto rejected = editor::ApplyManagedInspectorBatch(Context(), edits); EXPECT_FALSE(rejected.ok);
    EXPECT_EQ(health(first), 10); EXPECT_EQ(health(second), 7); EXPECT_EQ(m_undo.GetHistorySize(), 0U);
    editor::OpData patch = editor::OpData::MakeObject(); patch.Set("path", "/health"); patch.Set("value", 6);
    edits[1].patches = editor::OpData::MakeArray(); edits[1].patches.Push(patch);
    auto applied = editor::ApplyManagedInspectorBatch(Context(), edits); ASSERT_TRUE(applied.ok) << applied.errorCode << ": " << applied.message;
    EXPECT_EQ(health(first), 2); EXPECT_EQ(health(second), 6); EXPECT_EQ(m_undo.GetHistorySize(), 1U);
    ASSERT_TRUE(second->ApplySettingsJson("{\"health\":9}")); const auto cursor = m_undo.GetCursor(), revision = m_undo.GetRevision();
    EXPECT_FALSE(m_undo.Undo()); EXPECT_EQ(m_undo.GetCursor(), cursor); EXPECT_EQ(m_undo.GetRevision(), revision);
    EXPECT_EQ(health(first), 2); EXPECT_EQ(health(second), 9);
    ASSERT_TRUE(second->ApplySettingsJson("{\"health\":6}"));
    ASSERT_TRUE(m_undo.Undo()) << m_undo.LastDiagnostic(); EXPECT_EQ(health(first), 10); EXPECT_EQ(health(second), 7);
    ASSERT_TRUE(m_undo.Redo()) << m_undo.LastDiagnostic(); EXPECT_EQ(health(first), 2); EXPECT_EQ(health(second), 6);
}

TEST_F(ManagedCompletionTest, MixedSettingsPagesIgnoreEnvelopeMetadataAndCompareOnlyReturnedValuesAndExactReferences)
{
    editor::OpData first, equal, mixed;
    ASSERT_TRUE(editor::ReadManagedSettingsJson(R"({"path":"/values","sample":"request","generation":"1","instanceId":"11","counter":"8","kind":"list","total":40001,"start":39997,"returned":4,"items":[39997,39998,39999,40000],"references":{}})", first));
    ASSERT_TRUE(editor::ReadManagedSettingsJson(R"({"counter":"99","instanceId":"22","generation":"2","sample":"snapshot","path":"/values","references":{},"items":[39997,39998,39999,40000],"returned":4,"start":39997,"total":40001,"kind":"list"})", equal));
    EXPECT_TRUE(editor::ManagedSettingsPagesEqual(first, equal));
    mixed = equal; mixed.Set("items", editor::OpData::MakeArray());
    EXPECT_FALSE(editor::ManagedSettingsPagesEqual(first, mixed));
    mixed = equal; mixed.Set("total", 40002); EXPECT_FALSE(editor::ManagedSettingsPagesEqual(first, mixed));
    editor::OpData references = editor::OpData::MakeObject(), identity = editor::OpData::MakeObject();
    identity.Set("guid", "same-guid"); identity.Set("objectId", "123"); identity.Set("sceneLifetime", "9"); references.Set("/values/39997", identity);
    first.Set("references", references); equal.Set("references", references); EXPECT_TRUE(editor::ManagedSettingsPagesEqual(first, equal));
    identity.Set("objectId", "456"); references.Set("/values/39997", identity); equal.Set("references", references);
    EXPECT_FALSE(editor::ManagedSettingsPagesEqual(first, equal));
    ASSERT_TRUE(editor::ReadManagedSettingsJson(R"({"kind":"object","total":1,"start":0,"returned":1,"instanceId":"1","items":[{"name":"payload","value":{"sample":"user-value","instanceId":"user-key"}}],"references":{}})", first));
    equal = first; equal.Set("instanceId", "another-target"); EXPECT_TRUE(editor::ManagedSettingsPagesEqual(first, equal));
    ASSERT_TRUE(editor::ReadManagedSettingsJson(R"({"kind":"object","total":1,"start":0,"returned":1,"instanceId":"another-target","items":[{"name":"payload","value":{"sample":"changed-user-value","instanceId":"user-key"}}],"references":{}})", equal));
    EXPECT_FALSE(editor::ManagedSettingsPagesEqual(first, equal));
}

TEST_F(ManagedCompletionTest, SharedPrimitiveWidgetsReadWholeRealTokensAndTypedRangeBounds)
{
    editor::OpData values; ASSERT_TRUE(editor::ReadManagedSettingsJson(R"({"one":1,"two":2,"minimum":0,"maximum":100})", values));
    double number = -9;
    for (const auto& [key, expected] : std::vector<std::pair<const char*, double>>{{"one", 1}, {"two", 2}, {"minimum", 0}, {"maximum", 100}}) {
        ASSERT_TRUE(editor::TryReadManagedPrimitiveReal(*values.Find(key), number)); EXPECT_DOUBLE_EQ(number, expected);
    }
    number = -9; EXPECT_FALSE(editor::TryReadManagedPrimitiveReal(editor::OpData(true), number)); EXPECT_DOUBLE_EQ(number, -9);
    EXPECT_FALSE(editor::TryReadManagedPrimitiveReal(editor::OpData("9223372036854775807"), number)); EXPECT_DOUBLE_EQ(number, -9);
    ImGuiFrame frame; editor::ImGuiReflector ui; editor::OpData schema = editor::OpData::MakeObject();
    schema.Set("valueKind", "float"); schema.Set("minimum", 0); schema.Set("maximum", 100);
    editor::OpData value(1); ASSERT_TRUE(editor::DrawManagedPrimitive(ui, "whole", schema, value));
    EXPECT_NE(frame.current->LastItemData.ID, 0U); EXPECT_FALSE(ui.m_changed); EXPECT_EQ(value.AsInt64(), 1);
    schema.Set("valueKind", "double"); value = editor::OpData(2); ASSERT_TRUE(editor::DrawManagedPrimitive(ui, "double", schema, value));
    EXPECT_NE(frame.current->LastItemData.ID, 0U); EXPECT_FALSE(ui.m_changed); EXPECT_EQ(value.AsInt64(), 2);
}

TEST_F(ManagedCompletionTest, ExplicitPlayCaptureStopReviewAdoptionAndCheckedUndoPreserveOriginalSettings)
{
    Load(); auto& owner = AttachedScene()->CreateGameObject("AdoptionOwner"); const std::string guid = owner.instanceId;
    auto& entry = owner.AddComponent<scene::ScriptComponent>().scripts.emplace_back();
    entry.script = Create(owner, "ManagedFixtures.HealthComponent", entry.savedScriptId); ASSERT_NE(entry.script, nullptr);
    const std::string saved = entry.savedScriptId; auto* original = scene::ManagedScriptRuntime::Instance().FindAdapter(entry.script.get());
    ASSERT_TRUE(original->ApplySettingsJson("{\"health\":4}"));
    editor::PlayModeController play; play.Play(*AttachedScene()); ASSERT_TRUE(play.IsPlaying()) << play.Diagnostic();
    struct Restore { editor::EditorContext& ctx; scene::Scene* source; editor::UndoStack& undo; ~Restore() { ctx.activeScene = source; ctx.editScene = source; ctx.undoStack = &undo; ctx.playMode = nullptr; } } restore{Context(), AttachedScene(), m_undo};
    Context().playMode = &play; Context().activeScene = play.PlayScene(); Context().undoStack = play.TemporaryUndo();
    auto* cloneOwner = play.PlayScene()->FindByGuid(guid); auto& cloneEntry = cloneOwner->GetComponent<scene::ScriptComponent>()->scripts.front();
    auto* clone = scene::ManagedScriptRuntime::Instance().FindAdapter(cloneEntry.script.get()); ASSERT_TRUE(clone->ApplySettingsJson("{\"health\":2}"));
    editor::OpData paths = editor::OpData::MakeArray(); paths.Push("/health"); editor::OpData target = editor::OpData::MakeObject();
    target.Set("ownerGuid", guid); target.Set("savedScriptId", saved); target.Set("paths", paths);
    editor::OpData targets = editor::OpData::MakeArray(); targets.Push(target); editor::OpData settings = editor::OpData::MakeObject(); settings.Set("targets", targets);
    editor::OpArgs capture; capture.Set("settings", settings); auto held = editor::InvokeOperator(Context(), "script.play_changes.capture", capture); ASSERT_TRUE(held.ok) << held.message;
    editor::OpArgs args; args.Set("adoptionId", held.data.Find("adoptionId")->AsString());
    play.Stop(*play.PlayScene()); Context().undoStack = &m_undo; ASSERT_TRUE(play.ApplyPendingRestore(*AttachedScene())); Context().activeScene = AttachedScene();
    std::string json; editor::OpData page; ASSERT_TRUE(original->ReadSettingsPage("/health", 0, 1, json)); ASSERT_TRUE(editor::ReadManagedSettingsJson(json, page)); EXPECT_EQ(page.Find("value")->AsInt(), 4);
    editor::OpData options = editor::OpData::MakeObject(); options.Set("action", "read"); options.Set("leafIndex", 0); options.Set("count", 1); args.Set("options", options);
    auto frozen = editor::InvokeOperator(Context(), "script.play_changes.review", args); ASSERT_TRUE(frozen.ok) << frozen.message; EXPECT_EQ(frozen.data.Find("value")->AsInt(), 2);
    auto prepared = editor::InvokeOperator(Context(), "script.play_changes.prepare", args); ASSERT_TRUE(prepared.ok) << prepared.errorCode << ": " << prepared.message;
    editor::OpArgs apply; apply.Set("plan", prepared.data.Find("plan")->AsString());
    auto adopted = editor::InvokeOperator(Context(), "script.configure.apply", apply); ASSERT_TRUE(adopted.ok) << adopted.message; EXPECT_EQ(m_undo.GetHistorySize(), 1U);
    auto replay = editor::InvokeOperator(Context(), "script.play_changes.prepare", args); ASSERT_TRUE(replay.ok); EXPECT_TRUE(replay.noChange); EXPECT_EQ(m_undo.GetHistorySize(), 1U);
    ASSERT_TRUE(m_undo.Undo()) << m_undo.LastDiagnostic(); ASSERT_TRUE(original->ReadSettingsPage("/health", 0, 1, json)); ASSERT_TRUE(editor::ReadManagedSettingsJson(json, page)); EXPECT_EQ(page.Find("value")->AsInt(), 4);
    ASSERT_TRUE(m_undo.Redo()) << m_undo.LastDiagnostic(); ASSERT_TRUE(original->ReadSettingsPage("/health", 0, 1, json)); ASSERT_TRUE(editor::ReadManagedSettingsJson(json, page)); EXPECT_EQ(page.Find("value")->AsInt(), 2);
    ASSERT_TRUE(editor::InvokeOperator(Context(), "script.play_changes.discard", args).ok); ASSERT_TRUE(m_undo.Undo()) << m_undo.LastDiagnostic(); ASSERT_TRUE(m_undo.Redo()) << m_undo.LastDiagnostic();
    std::string diagnostic; const auto savedText = scene::SceneSerializer::SaveToText(*AttachedScene(), {}, diagnostic); ASSERT_FALSE(savedText.empty()) << diagnostic;
    auto reopened = scene::SceneSerializer::LoadDataFromText(savedText, File("adopted-settings.scene").generic_string()); ASSERT_NE(reopened, nullptr); auto* reloadedOwner = reopened->FindByGuid(guid); ASSERT_NE(reloadedOwner, nullptr);
    auto* reloaded = scene::ManagedScriptRuntime::Instance().FindAdapter(reloadedOwner->GetComponent<scene::ScriptComponent>()->scripts.front().script.get());
    ASSERT_TRUE(reloaded->ReadSettingsPage("/health", 0, 1, json)); ASSERT_TRUE(editor::ReadManagedSettingsJson(json, page)); EXPECT_EQ(page.Find("value")->AsInt(), 2);
}

TEST_F(ManagedCompletionTest, CapturedPlayReferencesRejectRecreatedSameGuidAndRemainReviewableAfterStop)
{
    Load(); auto& owner = AttachedScene()->CreateGameObject("ReferenceAdoptionOwner"); auto& reference = AttachedScene()->CreateGameObject("InitialReference");
    const std::string ownerGuid = owner.instanceId, referenceGuid = reference.instanceId;
    auto& entry = owner.AddComponent<scene::ScriptComponent>().scripts.emplace_back(); entry.script = Create(owner, "ManagedFixtures.ReferenceComponent", entry.savedScriptId); ASSERT_NE(entry.script, nullptr);
    auto* original = scene::ManagedScriptRuntime::Instance().FindAdapter(entry.script.get()); ASSERT_TRUE(original->ApplySettingsJson("{\"target\":\"" + referenceGuid + "\"}"));
    editor::PlayModeController play; play.Play(*AttachedScene()); ASSERT_TRUE(play.IsPlaying()) << play.Diagnostic();
    struct Restore { editor::EditorContext& ctx; scene::Scene* source; editor::UndoStack& undo; ~Restore() { ctx.activeScene = source; ctx.undoStack = &undo; ctx.playMode = nullptr; } } restore{Context(), AttachedScene(), m_undo};
    Context().playMode = &play; Context().activeScene = play.PlayScene(); Context().undoStack = play.TemporaryUndo();
    auto* previous = play.PlayScene()->FindByGuid(referenceGuid); ASSERT_NE(previous, nullptr);
    scene::GameObject::Destroy(*previous); play.PlayScene()->FlushDestroyQueue(0.0f); ASSERT_EQ(play.PlayScene()->FindByGuid(referenceGuid), nullptr);
    auto& replacement = play.PlayScene()->CreateGameObject("Replacement"); replacement.instanceId = referenceGuid;
    auto* clone = scene::ManagedScriptRuntime::Instance().FindAdapter(play.PlayScene()->FindByGuid(ownerGuid)->GetComponent<scene::ScriptComponent>()->scripts.front().script.get());
    ASSERT_TRUE(clone->ApplySettingsJson("{\"target\":\"" + referenceGuid + "\"}", scene::ManagedScriptAdapter::SettingsValidationPurpose::AUTHORING, {"/target"}));
    editor::OpData path = editor::OpData::MakeArray(); path.Push("/target"); editor::OpData target = editor::OpData::MakeObject(); target.Set("ownerGuid", ownerGuid); target.Set("savedScriptId", entry.savedScriptId); target.Set("paths", path);
    editor::OpData targets = editor::OpData::MakeArray(); targets.Push(target); editor::OpData settings = editor::OpData::MakeObject(); settings.Set("targets", targets); editor::OpArgs args; args.Set("settings", settings);
    auto held = editor::InvokeOperator(Context(), "script.play_changes.capture", args); ASSERT_TRUE(held.ok) << held.message; EXPECT_FALSE(held.data.Find("readyForAdoption")->AsBool());
    args.Set("adoptionId", held.data.Find("adoptionId")->AsString()); play.Stop(*play.PlayScene()); Context().undoStack = &m_undo; ASSERT_TRUE(play.ApplyPendingRestore(*AttachedScene())); Context().activeScene = AttachedScene();
    EXPECT_FALSE(editor::InvokeOperator(Context(), "script.play_changes.prepare", args).ok); EXPECT_EQ(m_undo.GetHistorySize(), 0U);
    editor::OpData options = editor::OpData::MakeObject(); options.Set("action", "read"); options.Set("leafIndex", 0); options.Set("count", 1); args.Set("options", options);
    EXPECT_TRUE(editor::InvokeOperator(Context(), "script.play_changes.review", args).ok); EXPECT_NE(AttachedScene()->FindByGuid(referenceGuid), nullptr);
}

TEST_F(ManagedCompletionTest, SdkCatalogFiltersAllOverloadsWithoutExecutingGameCallbacks)
{
    Load();
    const auto response = Query("Fbzz.ScriptScene", "Create");
    ASSERT_NE(response.Find("ok"), nullptr);
    ASSERT_TRUE(response.Find("ok")->AsBool()) << editor::ai::SerializeJson(response);
    const auto* result = response.Find("result"); ASSERT_NE(result, nullptr);
    const auto* types = result->Find("types"); ASSERT_NE(types, nullptr);
    ASSERT_EQ(types->AsArray().size(), 1u);
    ASSERT_EQ(types->AsArray().front().Find("members")->AsArray().size(), 2u);
    EXPECT_TRUE(Query("Fbzz.FutureUnsupportedApi", "Run").Find("ok") != nullptr);
    EXPECT_FALSE(Query("Fbzz.FutureUnsupportedApi", "Run").Find("ok")->AsBool());
}

TEST_F(ManagedCompletionTest, InspectorUsesExactInstanceAndCommonValidationWithOneUndo)
{
    Load();
    auto& owner = AttachedScene()->CreateGameObject("HealthOwner");
    auto& scripts = owner.AddComponent<scene::ScriptComponent>();
    for (int index = 0; index < 2; ++index) { auto& added = scripts.scripts.emplace_back(); added.script = Create(owner, "ManagedFixtures.HealthComponent", added.savedScriptId); }
    ASSERT_NE(scripts.scripts[0].script, nullptr); ASSERT_NE(scripts.scripts[1].script, nullptr);
    const auto target = editor::CaptureManagedSettingsTarget(Context(), owner, scripts.scripts[1]);
    editor::OpData fields = editor::OpData::MakeObject(); fields.Set("health", 4);
    auto applied = editor::ApplyManagedInspectorFields(Context(), target, fields);
    ASSERT_TRUE(applied.ok) << applied.errorCode << ": " << applied.message;
    struct Read final : IgnoreFields {
        int health = -1;
        void Field(const char* name, int& value) override { if (std::string_view(name) == "health") health = value; }
    } first, second;
    scripts.scripts[0].script->Reflect(first); scripts.scripts[1].script->Reflect(second);
    EXPECT_EQ(first.health, 10); EXPECT_EQ(second.health, 4);
    m_undo.Undo();
    scripts.scripts[1].script->Reflect(second); EXPECT_EQ(second.health, 10);
    m_undo.Redo();
    scripts.scripts[1].script->Reflect(second); EXPECT_EQ(second.health, 4);
    fields.Set("health", 2.5f);
    EXPECT_FALSE(editor::ApplyManagedInspectorFields(Context(), target, fields).ok);
    scripts.scripts[1].script->Reflect(second); EXPECT_EQ(second.health, 4);
    scripts.scripts[1].script = Create(owner, "ManagedFixtures.HealthComponent", scripts.scripts[1].savedScriptId);
    ASSERT_NE(scripts.scripts[1].script, nullptr);
    fields.Set("health", 9);
    EXPECT_FALSE(editor::ApplyManagedInspectorFields(Context(), target, fields).ok);
    scripts.scripts[1].script->Reflect(second); EXPECT_EQ(second.health, 10);
}

TEST_F(ManagedCompletionTest, InspectorAndAiAuthorIncompleteRequiredSettingsButStartValidationRemainsStrict)
{
    Load();
    auto& targetObject = AttachedScene()->CreateGameObject("RequiredTarget");
    const std::string guid = targetObject.instanceId;
    auto& owner = AttachedScene()->CreateGameObject("RequiredAuthor");
    auto& entry = owner.AddComponent<scene::ScriptComponent>().scripts.emplace_back();
    entry.script = Create(owner, "ManagedFixtures.RequiredReferenceComponent", entry.savedScriptId); ASSERT_NE(entry.script, nullptr);
    auto* adapter = scene::ManagedScriptRuntime::Instance().FindAdapter(entry.script.get()); ASSERT_NE(adapter, nullptr);
    const auto exact = editor::CaptureManagedSettingsTarget(Context(), owner, entry);
    const auto edit = [&](const std::string& json) {
        editor::OpData fields;
        if (!editor::ReadManagedSettingsJson(json, fields)) return editor::OpResult::Err("BAD_TEST_JSON", "Invalid test JSON");
        return editor::ApplyManagedInspectorFields(Context(), exact, fields);
    };
    auto result = edit("{\"settings\":{\"target\":\"" + guid + "\",\"targets\":[],\"list\":[],\"speed\":2}}");
    ASSERT_TRUE(result.ok) << result.message;
    EXPECT_FALSE(adapter->ValidateReferences());
    EXPECT_FALSE(scene::SceneSerializer::SaveToText(*AttachedScene()).empty());
    result = edit("{\"settings\":{\"target\":\"" + guid + "\",\"targets\":[\"" + guid + "\"],\"list\":[\"" + guid + "\"],\"speed\":2}}");
    ASSERT_TRUE(result.ok) << result.message;
    EXPECT_FALSE(adapter->ValidateReferences());
    result = edit("{\"requiredList\":[\"" + guid + "\"]}");
    ASSERT_TRUE(result.ok) << result.message;
    EXPECT_TRUE(adapter->ValidateReferences());
    m_undo.Undo(); EXPECT_FALSE(adapter->ValidateReferences());
    m_undo.Redo(); EXPECT_TRUE(adapter->ValidateReferences());
    result = edit("{\"settings\":{\"speed\":11},\"requiredList\":[]}");
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(adapter->ValidateReferences());
    result = edit("{\"requiredList\":[]}"); ASSERT_TRUE(result.ok) << result.message;
    EXPECT_FALSE(scene::ValidateScriptRequirementsForStart(owner, *entry.script));
    EXPECT_TRUE(entry.script->RequirementsBlocked());
}

TEST_F(ManagedCompletionTest, NestedInspectorProjectionPreservesLiveAndExpiredReferenceIdentities)
{
    Load();
    auto& targetObject = AttachedScene()->CreateGameObject("NestedTarget");
    const std::string guid = targetObject.instanceId;
    const auto targetId = targetObject.GetID();
    auto& owner = AttachedScene()->CreateGameObject("NestedAuthor");
    auto& entry = owner.AddComponent<scene::ScriptComponent>().scripts.emplace_back();
    entry.script = Create(owner, "ManagedFixtures.RequiredReferenceComponent", entry.savedScriptId); ASSERT_NE(entry.script, nullptr);
    auto* adapter = scene::ManagedScriptRuntime::Instance().FindAdapter(entry.script.get()); ASSERT_NE(adapter, nullptr);
    ASSERT_TRUE(adapter->ApplySettingsJson("{\"settings\":{\"target\":\"" + guid + "\",\"targets\":[\"" + guid + "\"],\"list\":[\"" + guid + "\"]}}",
        scene::ManagedScriptAdapter::SettingsValidationPurpose::AUTHORING));
    struct Projection final : IgnoreFields {
        editor::OpData settings;
        std::vector<scene::ScriptObjectReference> references;
        bool valid = false;
        void ObjectField(const char* name, scene::IScriptSerializable& value) override
        { if (std::string_view(name) == "settings") valid = editor::ReadManagedStructuredSetting(value, settings, &references); }
    } projection;
    adapter->ReflectReadOnly(projection); ASSERT_TRUE(projection.valid);
    ASSERT_EQ(projection.references.size(), 3u);
    for (const auto& reference : projection.references) { EXPECT_EQ(reference.guid, guid); EXPECT_EQ(reference.id, targetId); }
    editor::ManagedStructuredReferencePaths paths;
    ASSERT_TRUE(editor::IndexManagedStructuredReferences(projection.settings, projection.references, paths));
    EXPECT_EQ(paths.at("/list/0").id, targetId);
    EXPECT_EQ(paths.at("/target").id, targetId);
    EXPECT_EQ(paths.at("/targets/0").id, targetId);
    ASSERT_TRUE(AttachedScene()->DestroyGameObject(targetId));
    auto& replacement = AttachedScene()->CreateGameObject("Replacement"); replacement.instanceId = guid;
    projection.references.clear(); projection.valid = false;
    adapter->ReflectReadOnly(projection); ASSERT_TRUE(projection.valid);
    ASSERT_EQ(projection.references.size(), 3u);
    for (const auto& reference : projection.references) { EXPECT_EQ(reference.guid, guid); EXPECT_FALSE(reference.id.IsValid()); }
    ASSERT_TRUE(editor::IndexManagedStructuredReferences(projection.settings, projection.references, paths));
    EXPECT_FALSE(paths.at("/target").id.IsValid());
}

TEST_F(ManagedCompletionTest, StructuredReferencePathsRemainStableAcrossHiddenGroups)
{
    const auto first = AttachedScene()->CreateGameObject("First").GetID();
    const auto second = AttachedScene()->CreateGameObject("Second").GetID();
    editor::OpData value;
    ASSERT_TRUE(editor::ReadManagedSettingsJson(R"({"collapsed":{"items":[{"$go":"first"}]},"a/b~":{"$go":"second"}})", value));
    std::vector<scene::ScriptObjectReference> references{{"first", first}, {"second", second}};
    editor::ManagedStructuredReferencePaths paths;
    ASSERT_TRUE(editor::IndexManagedStructuredReferences(value, references, paths));
    EXPECT_EQ(paths.at("/a~1b~0").id, second);
    EXPECT_EQ(paths.at("/collapsed/items/0").id, first);
    std::swap(references[0], references[1]);
    EXPECT_FALSE(editor::IndexManagedStructuredReferences(value, references, paths));
    EXPECT_EQ(paths.at("/a~1b~0").id, second);
}

TEST_F(ManagedCompletionTest, CommonConfigurePreservesExpiredSiblingsUntilExplicitReferenceRebind)
{
    Load();
    auto& targetObject = AttachedScene()->CreateGameObject("OriginalTarget");
    const std::string guid = targetObject.instanceId;
    const auto expiredId = targetObject.GetID();
    auto& owner = AttachedScene()->CreateGameObject("ReferenceAuthor");
    auto& entry = owner.AddComponent<scene::ScriptComponent>().scripts.emplace_back();
    entry.script = Create(owner, "ManagedFixtures.RequiredReferenceComponent", entry.savedScriptId); ASSERT_NE(entry.script, nullptr);
    auto* adapter = scene::ManagedScriptRuntime::Instance().FindAdapter(entry.script.get()); ASSERT_NE(adapter, nullptr);
    const auto exact = editor::CaptureManagedSettingsTarget(Context(), owner, entry);
    const std::string initial = "{\"settings\":{\"target\":\"" + guid + "\",\"targets\":[\"" + guid + "\"],\"list\":[\"" + guid + "\"],\"speed\":2}}";
    editor::OpData fields;
    ASSERT_TRUE(editor::ReadManagedSettingsJson(initial, fields));
    ASSERT_TRUE(editor::ApplyManagedInspectorFields(Context(), exact, fields).ok);
    ASSERT_TRUE(AttachedScene()->DestroyGameObject(expiredId));
    auto& replacement = AttachedScene()->CreateGameObject("SameGuidReplacement"); replacement.instanceId = guid;
    const auto replacementId = replacement.GetID();
    struct Read final : IgnoreFields {
        editor::OpData settings;
        std::vector<scene::ScriptObjectReference> references;
        bool valid = false;
        void ObjectField(const char* name, scene::IScriptSerializable& value) override
        { if (std::string_view(name) == "settings") valid = editor::ReadManagedStructuredSetting(value, settings, &references); }
    } read;
    const auto projection = [&]() {
        read.valid = false; read.references.clear(); adapter->ReflectReadOnly(read);
        editor::ManagedStructuredReferencePaths paths;
        EXPECT_TRUE(read.valid); EXPECT_TRUE(editor::IndexManagedStructuredReferences(read.settings, read.references, paths));
        return paths;
    };
    auto paths = projection(); ASSERT_EQ(paths.size(), 3u); EXPECT_FALSE(paths.at("/target").id.IsValid());
    const std::string numericEdit = "{\"settings\":{\"target\":\"" + guid + "\",\"targets\":[\"" + guid + "\"],\"list\":[\"" + guid + "\"],\"speed\":3}}";
    ASSERT_TRUE(editor::ReadManagedSettingsJson(numericEdit, fields));
    auto edited = editor::ApplyManagedInspectorFields(Context(), exact, fields); ASSERT_TRUE(edited.ok) << edited.message;
    paths = projection();
    for (const auto& [path, reference] : paths) { (void)path; EXPECT_FALSE(reference.id.IsValid()); }
    const auto* speed = read.settings.Find("speed"); ASSERT_NE(speed, nullptr);
    EXPECT_FLOAT_EQ(speed->IsInt() ? static_cast<float>(speed->AsInt()) : speed->AsFloat(), 3.0f);
    auto invalid = editor::ApplyManagedInspectorFields(Context(), exact, fields, {"/settings/speed"}); EXPECT_FALSE(invalid.ok);
    paths = projection(); EXPECT_FALSE(paths.at("/target").id.IsValid());
    auto rebound = editor::ApplyManagedInspectorFields(Context(), exact, fields, {"/settings/target"});
    ASSERT_TRUE(rebound.ok) << rebound.message;
    paths = projection();
    EXPECT_EQ(paths.at("/target").id, replacementId);
    EXPECT_FALSE(paths.at("/list/0").id.IsValid()); EXPECT_FALSE(paths.at("/targets/0").id.IsValid());
}

TEST_F(ManagedCompletionTest, CommonConfigureCollectionMovesPreserveExpiredTokensAndRejectAmbiguity)
{
    Load();
    auto& original = AttachedScene()->CreateGameObject("MovingExpiredTarget");
    const auto expiredId = original.GetID(); const std::string guid = original.instanceId;
    auto& live = AttachedScene()->CreateGameObject("RetainedLiveTarget");
    const auto liveId = live.GetID(); const std::string liveGuid = live.instanceId;
    auto& owner = AttachedScene()->CreateGameObject("CollectionAuthor");
    auto& entry = owner.AddComponent<scene::ScriptComponent>().scripts.emplace_back();
    entry.script = Create(owner, "ManagedFixtures.RequiredReferenceComponent", entry.savedScriptId); ASSERT_NE(entry.script, nullptr);
    auto* adapter = scene::ManagedScriptRuntime::Instance().FindAdapter(entry.script.get()); ASSERT_NE(adapter, nullptr);
    const auto exact = editor::CaptureManagedSettingsTarget(Context(), owner, entry);
    const auto edit = [&](const std::string& json, const std::vector<std::string>& rebind = std::vector<std::string>{}) {
        editor::OpData fields;
        if (!editor::ReadManagedSettingsJson(json, fields)) return editor::OpResult::Err("BAD_TEST_JSON", "Invalid test JSON");
        return editor::ApplyManagedInspectorFields(Context(), exact, fields, rebind);
    };
    const std::string initial = "{\"settings\":{\"target\":\"" + guid + "\",\"targets\":[\"" + guid + "\",\"" + liveGuid
        + "\"],\"list\":[\"" + guid + "\",\"" + liveGuid + "\"],\"speed\":2}}";
    ASSERT_TRUE(edit(initial).ok);
    ASSERT_TRUE(AttachedScene()->DestroyGameObject(expiredId));
    auto& replacement = AttachedScene()->CreateGameObject("MovingSameGuidReplacement"); replacement.instanceId = guid;
    const auto replacementId = replacement.GetID();
    struct Read final : IgnoreFields {
        editor::OpData settings; std::vector<scene::ScriptObjectReference> references; bool valid = false;
        void ObjectField(const char* name, scene::IScriptSerializable& value) override
        { if (std::string_view(name) == "settings") valid = editor::ReadManagedStructuredSetting(value, settings, &references); }
    } read;
    const auto projection = [&]() {
        read.valid = false; read.references.clear(); adapter->ReflectReadOnly(read);
        editor::ManagedStructuredReferencePaths paths;
        EXPECT_TRUE(read.valid); EXPECT_TRUE(editor::IndexManagedStructuredReferences(read.settings, read.references, paths)); return paths;
    };
    const std::string reordered = "{\"settings\":{\"target\":\"" + guid + "\",\"targets\":[\"" + liveGuid + "\",\"" + guid
        + "\"],\"list\":[\"" + liveGuid + "\",\"" + guid + "\"],\"speed\":3}}";
    auto result = edit(reordered); ASSERT_TRUE(result.ok) << result.message;
    auto paths = projection(); ASSERT_EQ(paths.size(), 5u);
    EXPECT_EQ(paths.at("/list/0").id, liveId); EXPECT_EQ(paths.at("/targets/0").id, liveId);
    EXPECT_FALSE(paths.at("/list/1").id.IsValid()); EXPECT_FALSE(paths.at("/targets/1").id.IsValid());
    EXPECT_FALSE(paths.at("/target").id.IsValid());
    result = edit(reordered, {"/settings/list/1"}); ASSERT_TRUE(result.ok) << result.message;
    paths = projection(); EXPECT_EQ(paths.at("/list/1").id, replacementId); EXPECT_FALSE(paths.at("/target").id.IsValid());
    /// @note The saved GUIDs match but lifetime tokens differ; moves alone must receive an explicit error instead of silently becoming a no-change edit.
    editor::OpData movesOnly = editor::OpData::MakeArray();
    for (const auto& [from, to] : {std::pair{"/settings/target", "/settings/list/1"}, std::pair{"/settings/list/1", "/settings/target"}}) {
        editor::OpData move = editor::OpData::MakeObject(); move.Set("fromPath", from); move.Set("toPath", to); movesOnly.Push(std::move(move));
    }
    const auto historyBeforeMoves = m_undo.GetHistorySize();
    result = editor::ApplyManagedInspectorFields(Context(), exact, editor::OpData::MakeObject(), {}, movesOnly);
    EXPECT_FALSE(result.ok); EXPECT_FALSE(result.noChange); EXPECT_EQ(result.errorCode, "REFERENCE_MOVES_REQUIRE_VALUES");
    EXPECT_EQ(m_undo.GetHistorySize(), historyBeforeMoves);
    paths = projection(); EXPECT_EQ(paths.at("/list/1").id, replacementId); EXPECT_FALSE(paths.at("/target").id.IsValid());
    const std::string ambiguousMove = "{\"settings\":{\"target\":\"" + guid + "\",\"targets\":[\"" + liveGuid + "\",\"" + guid
        + "\"],\"list\":[\"" + guid + "\"],\"speed\":4}}";
    result = edit(ambiguousMove); EXPECT_FALSE(result.ok);
    paths = projection(); EXPECT_EQ(paths.at("/list/1").id, replacementId); EXPECT_EQ(paths.at("/list/0").id, liveId);
    EXPECT_FALSE(paths.at("/target").id.IsValid());
}

TEST_F(ManagedCompletionTest, ReadOnlyManagedReflectionNeverWritesEditedTemporaryValues)
{
    Load();
    auto& owner = AttachedScene()->CreateGameObject("HealthOwner");
    auto& entry = owner.AddComponent<scene::ScriptComponent>().scripts.emplace_back();
    entry.script = Create(owner, "ManagedFixtures.HealthComponent", entry.savedScriptId);
    ASSERT_NE(entry.script, nullptr);
    entry.script->SetContext(AttachedScene(), &owner);
    struct Edit final : IgnoreFields { void Field(const char*, int& value) override { value = 1; } } edit;
    auto* adapter = scene::ManagedScriptRuntime::Instance().FindAdapter(entry.script.get()); ASSERT_NE(adapter, nullptr);
    adapter->ReflectReadOnly(edit);
    struct Read final : IgnoreFields { int value = 0; void Field(const char*, int& field) override { value = field; } } read;
    adapter->ReflectReadOnly(read); EXPECT_EQ(read.value, 10);
}

TEST_F(ManagedCompletionTest, QuaternionStructuredAndAssetSettingsSurviveConfigureSaveAndReconfigure)
{
    Load();
    auto& targetObject = AttachedScene()->CreateGameObject("Target");
    const std::string targetGuid = targetObject.instanceId;
    auto& owner = AttachedScene()->CreateGameObject("CompletionOwner");
    const std::string ownerGuid = owner.instanceId;
    auto& entry = owner.AddComponent<scene::ScriptComponent>().scripts.emplace_back();
    entry.script = Create(owner, "ManagedFixtures.CompletionComponent", entry.savedScriptId); ASSERT_NE(entry.script, nullptr);
    const auto exact = editor::CaptureManagedSettingsTarget(Context(), owner, entry);
    editor::OpData fields;
    ASSERT_TRUE(editor::ReadManagedSettingsJson(
        "{\"rotation\":[0,0,0,2],\"weapon\":\"Assets/Data/Weapon.fzdata\",\"settings\":{\"label\":\"configured\",\"weights\":[0.5,2,5],\"target\":{\"$go\":\"" + targetGuid + "\"}}}", fields));
    const auto applied = editor::ApplyManagedInspectorFields(Context(), exact, fields);
    ASSERT_TRUE(applied.ok) << applied.errorCode << ": " << applied.message;
    struct Read final : IgnoreFields {
        math::Quaternion rotation{};
        std::string asset;
        editor::OpData settings;
        bool valid = false;
        void Field(const char* name, math::Quaternion& value) override { if (std::string_view(name) == "rotation") rotation = value; }
        void Field(const char* name, scene::DataAssetRef& value) override { if (std::string_view(name) == "weapon") asset = value.path; }
        void ObjectField(const char* name, scene::IScriptSerializable& value) override { if (std::string_view(name) == "settings") valid = editor::ReadManagedStructuredSetting(value, settings); }
    } read;
    auto* adapter = scene::ManagedScriptRuntime::Instance().FindAdapter(entry.script.get()); ASSERT_NE(adapter, nullptr);
    adapter->ReflectReadOnly(read);
    ASSERT_TRUE(read.valid); EXPECT_FLOAT_EQ(read.rotation.w, 2.0f); EXPECT_EQ(read.asset, "Assets/Data/Weapon.fzdata");
    ASSERT_NE(read.settings.Find("target"), nullptr); ASSERT_NE(read.settings.Find("target")->Find("$go"), nullptr);
    EXPECT_EQ(read.settings.Find("target")->Find("$go")->AsString(), targetGuid);
    ASSERT_NE(read.settings.Find("weights"), nullptr); ASSERT_EQ(read.settings.Find("weights")->AsArray().size(), 3u);
    m_undo.Undo();
    adapter->ReflectReadOnly(read); EXPECT_FLOAT_EQ(read.rotation.w, 1.0f); EXPECT_TRUE(read.asset.empty());
    ASSERT_NE(read.settings.Find("label"), nullptr); EXPECT_EQ(read.settings.Find("label")->AsString(), "default");
    m_undo.Redo();
    adapter->ReflectReadOnly(read); EXPECT_FLOAT_EQ(read.rotation.w, 2.0f); EXPECT_EQ(read.settings.Find("label")->AsString(), "configured");
    const std::string text = scene::SceneSerializer::SaveToText(*AttachedScene()); ASSERT_FALSE(text.empty());
    auto restored = scene::SceneSerializer::LoadDataFromText(text, File("completion.scene").generic_string()); ASSERT_NE(restored, nullptr);
    m_undo.Clear(); *AttachedScene() = std::move(*restored);
    auto* restoredOwner = AttachedScene()->FindByGuid(ownerGuid); ASSERT_NE(restoredOwner, nullptr);
    auto* scripts = restoredOwner->GetComponent<scene::ScriptComponent>(); ASSERT_NE(scripts, nullptr); ASSERT_EQ(scripts->scripts.size(), 1u);
    auto* restoredAdapter = scene::ManagedScriptRuntime::Instance().FindAdapter(scripts->scripts[0].script.get()); ASSERT_NE(restoredAdapter, nullptr);
    ASSERT_TRUE(restoredAdapter->RefreshOwnerBinding()); restoredAdapter->ReflectReadOnly(read);
    ASSERT_TRUE(read.valid); EXPECT_FLOAT_EQ(read.rotation.w, 2.0f); EXPECT_EQ(read.asset, "Assets/Data/Weapon.fzdata");
    EXPECT_EQ(read.settings.Find("target")->Find("$go")->AsString(), targetGuid);
    editor::OpData followUp = editor::OpData::MakeObject(); followUp.Set("awakeCount", 7);
    const auto restoredExact = editor::CaptureManagedSettingsTarget(Context(), *restoredOwner, scripts->scripts[0]);
    const auto reapplied = editor::ApplyManagedInspectorFields(Context(), restoredExact, followUp);
    ASSERT_TRUE(reapplied.ok) << reapplied.errorCode << ": " << reapplied.message;
    restoredAdapter->ReflectReadOnly(read); EXPECT_FLOAT_EQ(read.rotation.w, 2.0f); EXPECT_EQ(read.settings.Find("label")->AsString(), "configured");
    fields.Set("rotation", editor::OpData("wrong-shape"));
    EXPECT_FALSE(editor::ApplyManagedInspectorFields(Context(), restoredExact, fields).ok);
    restoredAdapter->ReflectReadOnly(read); EXPECT_FLOAT_EQ(read.rotation.w, 2.0f);
}

TEST_F(ManagedCompletionTest, BatchPreparationResolvesReferencesButNeverGrantsEngineAuthorityToCandidateCallbacks)
{
    Load();
    auto& first = AttachedScene()->CreateGameObject("AuthorA");
    auto& second = AttachedScene()->CreateGameObject("AuthorB");
    auto& targetObject = AttachedScene()->CreateGameObject("AuthorTarget");
    auto& firstEntry = first.AddComponent<scene::ScriptComponent>().scripts.emplace_back();
    auto& secondEntry = second.AddComponent<scene::ScriptComponent>().scripts.emplace_back();
    firstEntry.script = Create(first, "ManagedFixtures.AuthoringTransactionComponent", firstEntry.savedScriptId);
    secondEntry.script = Create(second, "ManagedFixtures.AuthoringTransactionComponent", secondEntry.savedScriptId);
    auto& runtime = scene::ManagedScriptRuntime::Instance();
    auto* firstAdapter = runtime.FindAdapter(firstEntry.script.get());
    auto* secondAdapter = runtime.FindAdapter(secondEntry.script.get());
    ASSERT_NE(firstAdapter, nullptr); ASSERT_NE(secondAdapter, nullptr);
    ASSERT_TRUE(secondAdapter->ApplySettingsJson("{\"touchEngine\":true}"));
    editor::OpData owners = editor::OpData::MakeArray(); owners.Push("AuthorA"); owners.Push("AuthorB");
    editor::OpData fields = editor::OpData::MakeObject(); fields.Set("amount", 25); fields.Set("target", "AuthorTarget");
    editor::OpData settings = editor::OpData::MakeObject(); settings.Set("objects", owners);
    settings.Set("script", "ManagedFixtures.AuthoringTransactionComponent"); settings.Set("fields", fields);
    editor::OpArgs args; args.Set("settings", settings);
    Context().sceneDirty = false;
    auto prepared = editor::InvokeOperator(Context(), "script.configure.preview", args);
    EXPECT_FALSE(prepared.ok);
    EXPECT_NE(prepared.message.find("CS_AUTHORING_CALLBACK"), std::string::npos);
    EXPECT_NE(prepared.message.find("AuthoringTransactionComponent"), std::string::npos);
    EXPECT_NE(prepared.message.find(".cs:line"), std::string::npos);
    EXPECT_TRUE(runtime.IsDispatchIdle()); EXPECT_EQ(m_undo.GetHistorySize(), 0u); EXPECT_FALSE(Context().sceneDirty);
    struct Read final : IgnoreFields {
        int amount = -1, coupled = -1;
        scene::EntityID target = scene::EntityID::INVALID;
        void Field(const char* name, int& value) override {
            if (std::string_view(name) == "amount") amount = value;
            if (std::string_view(name) == "coupled") coupled = value;
        }
        void ObjectReferenceField(const char* name, scene::ScriptObjectReference& value) override {
            if (std::string_view(name) == "target") target = value.id;
        }
    } read;
    firstAdapter->ReflectReadOnly(read); EXPECT_EQ(read.amount, 2); EXPECT_EQ(read.coupled, 4); EXPECT_FALSE(read.target.IsValid());
    secondAdapter->ReflectReadOnly(read); EXPECT_EQ(read.amount, 2); EXPECT_EQ(read.coupled, 4); EXPECT_FALSE(read.target.IsValid());
    ASSERT_TRUE(secondAdapter->ApplySettingsJson("{\"touchEngine\":false}"));
    prepared = editor::InvokeOperator(Context(), "script.configure.preview", args); ASSERT_TRUE(prepared.ok) << prepared.message;
    editor::OpArgs commit; commit.Set("plan", prepared.data.Find("plan")->AsString());
    const auto result = editor::InvokeOperator(Context(), "script.configure.apply", commit); ASSERT_TRUE(result.ok) << result.message;
    firstAdapter->ReflectReadOnly(read); EXPECT_EQ(read.amount, 10); EXPECT_EQ(read.coupled, 20); EXPECT_EQ(read.target, targetObject.GetID());
    secondAdapter->ReflectReadOnly(read); EXPECT_EQ(read.amount, 10); EXPECT_EQ(read.coupled, 20); EXPECT_EQ(read.target, targetObject.GetID());
    EXPECT_TRUE(runtime.IsDispatchIdle()); EXPECT_EQ(m_undo.GetHistorySize(), 1u);
    m_undo.Undo(); firstAdapter->ReflectReadOnly(read); EXPECT_EQ(read.amount, 2); EXPECT_EQ(read.coupled, 4); EXPECT_FALSE(read.target.IsValid());
}

TEST_F(ManagedCompletionTest, RecoverableInspectorFailureKeepsSessionForCorrectionAndStaleDraftCannotRebase)
{
    Load(); auto& owner = AttachedScene()->CreateGameObject("DraftRetryOwner");
    auto& entry = owner.AddComponent<scene::ScriptComponent>().scripts.emplace_back();
    entry.script = Create(owner, "ManagedFixtures.AuthoringTransactionComponent", entry.savedScriptId); ASSERT_NE(entry.script, nullptr);
    auto* adapter = scene::ManagedScriptRuntime::Instance().FindAdapter(entry.script.get()); ASSERT_NE(adapter, nullptr);
    ImGuiFrame frame;
    {
        editor::ManagedSettingsReflector reflector(Context(), owner, entry);
        ManagedSettingsReflectorTestAccess::Queue(reflector, "/touchEngine", editor::OpData(true));
        const uint64_t originalSession = ManagedSettingsReflectorTestAccess::EditSession(reflector);
        ASSERT_NE(originalSession, 0u);
        const std::string originalRevision = ManagedSettingsReflectorTestAccess::FirstRevision(reflector);
        reflector.Finish();
        EXPECT_FALSE(ManagedSettingsReflectorTestAccess::HasConflict(reflector));
        EXPECT_TRUE(ManagedSettingsReflectorTestAccess::HasPendingPatches(reflector));
        EXPECT_EQ(ManagedSettingsReflectorTestAccess::EditSession(reflector), 0u);
        EXPECT_EQ(ManagedSettingsReflectorTestAccess::FirstRevision(reflector), originalRevision);
        ManagedSettingsReflectorTestAccess::Queue(reflector, "/touchEngine", editor::OpData(false));
        const uint64_t retrySession = ManagedSettingsReflectorTestAccess::EditSession(reflector);
        EXPECT_NE(retrySession, 0u);
        EXPECT_NE(retrySession, originalSession);
        EXPECT_EQ(ManagedSettingsReflectorTestAccess::FirstRevision(reflector), originalRevision);
        reflector.Finish();
        EXPECT_FALSE(ManagedSettingsReflectorTestAccess::HasPendingPatches(reflector));
        EXPECT_FALSE(ManagedSettingsReflectorTestAccess::HasConflict(reflector));
    }
    std::string snapshotJson; ASSERT_TRUE(adapter->ReadSettingsSnapshot(snapshotJson)); editor::OpData snapshot;
    ASSERT_TRUE(editor::ReadManagedSettingsJson(snapshotJson, snapshot));
    EXPECT_FALSE(snapshot.Find("settings")->Find("touchEngine")->AsBool());
    ASSERT_EQ(m_undo.GetHistorySize(), 1u);

    auto& staleOwner = AttachedScene()->CreateGameObject("StaleDraftOwner");
    auto& staleEntry = staleOwner.AddComponent<scene::ScriptComponent>().scripts.emplace_back();
    staleEntry.script = Create(staleOwner, "ManagedFixtures.AuthoringTransactionComponent", staleEntry.savedScriptId);
    ASSERT_NE(staleEntry.script, nullptr);
    auto* staleAdapter = scene::ManagedScriptRuntime::Instance().FindAdapter(staleEntry.script.get()); ASSERT_NE(staleAdapter, nullptr);
    {
        editor::ManagedSettingsReflector reflector(Context(), staleOwner, staleEntry);
        ManagedSettingsReflectorTestAccess::Queue(reflector, "/amount", editor::OpData(3));
        const int64_t staleValue = ManagedSettingsReflectorTestAccess::PendingInt(reflector, 0);
        ASSERT_TRUE(staleAdapter->ApplySettingsJson("{\"amount\":8}"));
        reflector.Finish();
        EXPECT_TRUE(ManagedSettingsReflectorTestAccess::HasConflict(reflector));
        EXPECT_TRUE(ManagedSettingsReflectorTestAccess::HasPendingPatches(reflector));
        ManagedSettingsReflectorTestAccess::Queue(reflector, "/amount", editor::OpData(4));
        EXPECT_EQ(ManagedSettingsReflectorTestAccess::PendingInt(reflector, 0), staleValue);
        EXPECT_TRUE(ManagedSettingsReflectorTestAccess::HasConflict(reflector));
    }
    ASSERT_TRUE(staleAdapter->ReadSettingsSnapshot(snapshotJson)); ASSERT_TRUE(editor::ReadManagedSettingsJson(snapshotJson, snapshot));
    const auto* staleSettings = snapshot.Find("settings"); ASSERT_NE(staleSettings, nullptr);
    const auto* staleAmount = staleSettings->Find("amount"); ASSERT_NE(staleAmount, nullptr);
    EXPECT_EQ(staleAmount->AsInt(), 8);
}

TEST_F(ManagedCompletionTest, LeafUndoPreservesNestedSiblingsAndHistoryOnConflicts)
{
    Load(); auto& owner = AttachedScene()->CreateGameObject("LeafUndoOwner");
    auto& entry = owner.AddComponent<scene::ScriptComponent>().scripts.emplace_back();
    entry.script = Create(owner, "ManagedFixtures.CompletionComponent", entry.savedScriptId); ASSERT_NE(entry.script, nullptr);
    auto* adapter = scene::ManagedScriptRuntime::Instance().FindAdapter(entry.script.get()); ASSERT_NE(adapter, nullptr);
    const auto target = editor::CaptureManagedSettingsTarget(Context(), owner, entry);
    editor::OpData patches = editor::OpData::MakeArray(), patch = editor::OpData::MakeObject();
    patch.Set("path", "/settings/label"); patch.Set("value", "edited"); patches.Push(std::move(patch));
    const auto applied = editor::ApplyManagedInspectorFields(Context(), target, editor::OpData::MakeObject(), {}, {}, {}, patches);
    ASSERT_TRUE(applied.ok) << applied.message;
    ASSERT_TRUE(adapter->ApplySettingsJson("{\"settings\":{\"label\":\"edited\",\"weights\":[3,4],\"target\":null}}"));
    ASSERT_TRUE(m_undo.Undo()) << m_undo.LastDiagnostic();
    std::string json; ASSERT_TRUE(adapter->ReadSettingsSnapshot(json)); editor::OpData state; ASSERT_TRUE(editor::ReadManagedSettingsJson(json, state));
    const auto* settings = state.Find("settings")->Find("settings"); ASSERT_NE(settings, nullptr);
    EXPECT_EQ(settings->Find("label")->AsString(), "default"); EXPECT_EQ(settings->Find("weights")->AsArray()[0].AsInt(), 3);
    ASSERT_TRUE(m_undo.Redo()) << m_undo.LastDiagnostic();
    ASSERT_TRUE(adapter->ApplySettingsJson("{\"settings\":{\"label\":\"newer\",\"weights\":[3,4],\"target\":null}}"));
    const auto cursor = m_undo.GetCursor(), revision = m_undo.GetRevision(); Context().sceneDirty = false;
    EXPECT_FALSE(m_undo.Undo()); EXPECT_EQ(m_undo.GetCursor(), cursor); EXPECT_EQ(m_undo.GetRevision(), revision); EXPECT_EQ(m_undo.GetHistorySize(), 1u);
    EXPECT_FALSE(Context().sceneDirty); EXPECT_FALSE(m_undo.LastDiagnostic().empty());
    ASSERT_TRUE(adapter->ReadSettingsSnapshot(json)); ASSERT_TRUE(editor::ReadManagedSettingsJson(json, state));
    EXPECT_EQ(state.Find("settings")->Find("settings")->Find("label")->AsString(), "newer");
}
TEST_F(ManagedCompletionTest, LargeSettingsCanBeEditedAfterDiscardAndHistoryClearWithoutReceiptRevival)
{
    Load(); auto& owner = AttachedScene()->CreateGameObject("BudgetOwner");
    auto& entry = owner.AddComponent<scene::ScriptComponent>().scripts.emplace_back();
    entry.script = Create(owner, "ManagedFixtures.CompletionComponent", entry.savedScriptId); ASSERT_NE(entry.script, nullptr);
    auto* adapter = scene::ManagedScriptRuntime::Instance().FindAdapter(entry.script.get()); ASSERT_NE(adapter, nullptr);
    editor::OpData large; ASSERT_TRUE(editor::ReadManagedSettingsJson("{\"settings\":{\"label\":\"" + std::string(120000, 'x') + "\",\"weights\":[1,2],\"target\":null}}", large));
    std::string json; ASSERT_TRUE(editor::WriteOpDataJson(large, json)); ASSERT_TRUE(adapter->ApplySettingsJson(json));
    const auto target = editor::CaptureManagedSettingsTarget(Context(), owner, entry);
    std::string last;
    for (int index = 1; index <= 24; ++index) {
        editor::OpData fields = editor::OpData::MakeObject(); fields.Set("awakeCount", index);
        const auto result = editor::ApplyManagedInspectorFields(Context(), target, fields); ASSERT_TRUE(result.ok) << result.message;
        last = result.data.Find("plan")->AsString();
    }
    EXPECT_EQ(m_undo.GetHistorySize(), 24u); m_undo.Clear();
    editor::OpArgs replay; replay.Set("plan", last);
    const auto replayed = editor::InvokeOperator(Context(), "script.configure.apply", replay);
    ASSERT_TRUE(replayed.ok); EXPECT_TRUE(replayed.noChange); EXPECT_FALSE(replayed.data.Find("historyRetained")->AsBool()); EXPECT_EQ(m_undo.GetHistorySize(), 0u);
    editor::OpData selector = editor::OpData::MakeObject(); selector.Set("id", owner.instanceId);
    editor::OpData fields = editor::OpData::MakeObject(); fields.Set("awakeCount", 100);
    editor::OpData settings = editor::OpData::MakeObject(); settings.Set("objects", selector); settings.Set("script", target.type); settings.Set("scriptId", target.scriptId); settings.Set("fields", fields);
    for (int index = 0; index < 16; ++index) {
        editor::OpArgs request; request.Set("settings", settings); const auto prepared = editor::InvokeOperator(Context(), "script.configure.preview", request); ASSERT_TRUE(prepared.ok) << prepared.message;
        editor::OpArgs discard; discard.Set("plan", prepared.data.Find("plan")->AsString()); EXPECT_TRUE(editor::InvokeOperator(Context(), "script.configure.discard", discard).ok);
        EXPECT_FALSE(editor::InvokeOperator(Context(), "script.configure.apply", discard).ok);
    }
    const auto next = editor::ApplyManagedInspectorFields(Context(), target, fields); ASSERT_TRUE(next.ok) << next.message; EXPECT_EQ(m_undo.GetHistorySize(), 1u);
}

TEST_F(ManagedCompletionTest, CallerSizedSettingsSaveReloadAndLaterLeafUndoBeyondFormerLimits)
{
    Load(); auto& owner = AttachedScene()->CreateGameObject("LargeSettingsOwner");
    const std::string ownerGuid = owner.instanceId;
    auto& entry = owner.AddComponent<scene::ScriptComponent>().scripts.emplace_back();
    entry.script = Create(owner, "ManagedFixtures.CapacitySettingsComponent", entry.savedScriptId); ASSERT_NE(entry.script, nullptr);
    const std::string savedId = entry.savedScriptId;
    auto* adapter = scene::ManagedScriptRuntime::Instance().FindAdapter(entry.script.get()); ASSERT_NE(adapter, nullptr);
    const auto target = editor::CaptureManagedSettingsTarget(Context(), owner, entry);
    editor::OpData patches = editor::OpData::MakeArray(), patch = editor::OpData::MakeObject();
    patch.Set("path", "/values/40000"); patch.Set("value", -123); patches.Push(std::move(patch));
    const auto edited = editor::ApplyManagedInspectorFields(Context(), target, editor::OpData::MakeObject(), {}, {}, {}, patches);
    ASSERT_TRUE(edited.ok) << edited.message;
    const auto read = [&]() {
        std::string wire; EXPECT_TRUE(adapter->ReadSettingsSnapshot(wire)); editor::OpData snapshot;
        EXPECT_TRUE(editor::ReadManagedSettingsJson(wire, snapshot)); return snapshot;
    };
    auto snapshot = read(); const auto* fields = snapshot.Find("settings"); ASSERT_NE(fields, nullptr);
    ASSERT_EQ(fields->Find("values")->AsArray().size(), 40001u); EXPECT_EQ(fields->Find("values")->AsArray()[40000].AsInt(), -123);
    EXPECT_EQ(fields->Find("values")->AsArray()[4096].AsInt(), 4096); EXPECT_EQ(fields->Find("values")->AsArray()[32768].AsInt(), 32768);
    ASSERT_EQ(fields->Find("list")->AsArray().size(), 40001u); EXPECT_EQ(fields->Find("list")->AsArray()[40000].AsInt(), 40000);
    EXPECT_EQ(fields->Find("text")->AsString().size(), 1024u * 1024u + 23);
    ASSERT_TRUE(m_undo.Undo()) << m_undo.LastDiagnostic(); snapshot = read(); EXPECT_EQ(snapshot.Find("settings")->Find("values")->AsArray()[40000].AsInt(), 40000);
    ASSERT_TRUE(m_undo.Redo()) << m_undo.LastDiagnostic(); snapshot = read(); EXPECT_EQ(snapshot.Find("settings")->Find("values")->AsArray()[40000].AsInt(), -123);
    const std::string saved = scene::SceneSerializer::SaveToText(*AttachedScene()); ASSERT_GT(saved.size(), 1024u * 1024u);
    auto restored = scene::SceneSerializer::LoadDataFromText(saved, File("large-settings.scene").generic_string()); ASSERT_NE(restored, nullptr);
    m_undo.Clear(); *AttachedScene() = std::move(*restored);
    auto* restoredOwner = AttachedScene()->FindByGuid(ownerGuid); ASSERT_NE(restoredOwner, nullptr);
    auto* scripts = restoredOwner->GetComponent<scene::ScriptComponent>(); ASSERT_NE(scripts, nullptr); ASSERT_EQ(scripts->scripts.size(), 1u);
    EXPECT_EQ(scripts->scripts[0].savedScriptId, savedId);
    adapter = scene::ManagedScriptRuntime::Instance().FindAdapter(scripts->scripts[0].script.get()); ASSERT_NE(adapter, nullptr);
    snapshot = read(); fields = snapshot.Find("settings"); ASSERT_NE(fields, nullptr);
    ASSERT_EQ(fields->Find("values")->AsArray().size(), 40001u); EXPECT_EQ(fields->Find("values")->AsArray()[40000].AsInt(), -123);
    ASSERT_EQ(fields->Find("list")->AsArray().size(), 40001u); EXPECT_EQ(fields->Find("list")->AsArray()[40000].AsInt(), 40000);
    EXPECT_EQ(fields->Find("text")->AsString().size(), 1024u * 1024u + 23);
}

TEST_F(ManagedCompletionTest, SettingsPageCursorFollowsActualRangesAndPreservesBackNavigation)
{
    editor::ManagedSettingsPageCursor cursor;
    const auto page = [](int64_t start, int64_t returned) {
        editor::OpData value = editor::OpData::MakeObject();
        value.Set("start", start); value.Set("returned", returned); value.Set("nextStart", start + returned);
        return value;
    };
    const editor::OpData first = page(0, 63);
    ASSERT_TRUE(cursor.CanAdvance(first)); ASSERT_TRUE(cursor.Advance(first)); EXPECT_EQ(cursor.Start(), 63u);
    const editor::OpData second = page(63, 64);
    ASSERT_TRUE(cursor.Advance(second)); EXPECT_EQ(cursor.Start(), 127u); EXPECT_EQ(cursor.PageNumber(), 3u);
    ASSERT_TRUE(cursor.Back()); EXPECT_EQ(cursor.Start(), 63u); EXPECT_TRUE(cursor.HasVisitedNext());
    const editor::OpData shortened = page(63, 17);
    ASSERT_TRUE(cursor.Advance(shortened)); EXPECT_EQ(cursor.Start(), 80u); EXPECT_FALSE(cursor.HasVisitedNext());
    ASSERT_TRUE(cursor.Back()); EXPECT_EQ(cursor.Start(), 63u);
    cursor.InvalidateFuture(); EXPECT_FALSE(cursor.HasVisitedNext());
    EXPECT_FALSE(cursor.CanAdvance(page(62, 64)));
}

TEST_F(ManagedCompletionTest, MultiplePagedTextReplacementsUndoRedoAndSurviveSaveReload)
{
    Load(); auto& owner = AttachedScene()->CreateGameObject("PagedTextOwner");
    const std::string ownerGuid = owner.instanceId;
    auto& entry = owner.AddComponent<scene::ScriptComponent>().scripts.emplace_back();
    entry.script = Create(owner, "ManagedFixtures.CapacitySettingsComponent", entry.savedScriptId); ASSERT_NE(entry.script, nullptr);
    auto* adapter = scene::ManagedScriptRuntime::Instance().FindAdapter(entry.script.get()); ASSERT_NE(adapter, nullptr);
    const std::string emoji = "\xF0\x9F\x98\x80";
    const std::string initialText = std::string(63, 'x') + emoji + std::string(1024u * 1024u, 'x');
    editor::OpData initialSettings = editor::OpData::MakeObject(); initialSettings.Set("text", initialText);
    std::string initialJson; ASSERT_TRUE(editor::WriteOpDataJson(initialSettings, initialJson)); ASSERT_TRUE(adapter->ApplySettingsJson(initialJson));
    const auto target = editor::CaptureManagedSettingsTarget(Context(), owner, entry);
    editor::OpData patches = editor::OpData::MakeArray();
    editor::OpData tail = editor::OpData::MakeObject(); tail.Set("path", "/text"); tail.Set("action", "text_replace");
    tail.Set("index", 63); tail.Set("count", 64); tail.Set("value", "tail" + emoji); patches.Push(std::move(tail));
    editor::OpData head = editor::OpData::MakeObject(); head.Set("path", "/text"); head.Set("action", "text_replace");
    head.Set("index", 0); head.Set("count", 63); head.Set("value", "head-page"); patches.Push(std::move(head));
    const auto edited = editor::ApplyManagedInspectorFields(Context(), target, editor::OpData::MakeObject(), {}, {}, {}, patches);
    ASSERT_TRUE(edited.ok) << edited.message;
    std::string json; ASSERT_TRUE(adapter->ReadSettingsSnapshot(json)); editor::OpData snapshot;
    ASSERT_TRUE(editor::ReadManagedSettingsJson(json, snapshot));
    const std::string changed = snapshot.Find("settings")->Find("text")->AsString();
    EXPECT_TRUE(changed.starts_with(std::string("head-page") + "tail" + emoji));
    EXPECT_EQ(changed.substr(9, 8), "tail" + emoji);
    ASSERT_TRUE(m_undo.Undo()) << m_undo.LastDiagnostic();
    ASSERT_TRUE(adapter->ReadSettingsSnapshot(json)); ASSERT_TRUE(editor::ReadManagedSettingsJson(json, snapshot));
    EXPECT_EQ(snapshot.Find("settings")->Find("text")->AsString(), initialText);
    ASSERT_TRUE(m_undo.Redo()) << m_undo.LastDiagnostic();
    const std::string saved = scene::SceneSerializer::SaveToText(*AttachedScene());
    auto restored = scene::SceneSerializer::LoadDataFromText(saved, File("paged-text.scene").generic_string()); ASSERT_NE(restored, nullptr);
    *AttachedScene() = std::move(*restored);
    auto* restoredOwner = AttachedScene()->FindByGuid(ownerGuid); ASSERT_NE(restoredOwner, nullptr);
    auto* scripts = restoredOwner->GetComponent<scene::ScriptComponent>(); ASSERT_NE(scripts, nullptr);
    adapter = scene::ManagedScriptRuntime::Instance().FindAdapter(scripts->scripts[0].script.get()); ASSERT_NE(adapter, nullptr);
    ASSERT_TRUE(adapter->ReadSettingsSnapshot(json)); ASSERT_TRUE(editor::ReadManagedSettingsJson(json, snapshot));
    const std::string roundTrip = snapshot.Find("settings")->Find("text")->AsString();
    EXPECT_TRUE(roundTrip.starts_with(std::string("head-page") + "tail" + emoji));
    EXPECT_EQ(roundTrip.substr(9, 8), "tail" + emoji);
}

TEST_F(ManagedCompletionTest, VisibleSettingsPageAndInspectorNeverValidateUnexpandedLargeParents)
{
    Load(); auto& owner = AttachedScene()->CreateGameObject("PagedDisplayOwner");
    auto& entry = owner.AddComponent<scene::ScriptComponent>().scripts.emplace_back();
    entry.script = Create(owner, "ManagedFixtures.CapacitySettingsComponent", entry.savedScriptId); ASSERT_NE(entry.script, nullptr);
    auto* adapter = scene::ManagedScriptRuntime::Instance().FindAdapter(entry.script.get()); ASSERT_NE(adapter, nullptr);
    std::string json; ASSERT_TRUE(adapter->ReadSettingsPage("/values", 39997, 64, json)); EXPECT_LT(json.size(), 4096u);
    editor::OpData page; ASSERT_TRUE(editor::ReadManagedSettingsJson(json, page));
    EXPECT_EQ(page.Find("sample")->AsString(), "request"); EXPECT_EQ(page.Find("total")->AsInt64(), 40001); ASSERT_EQ(page.Find("items")->AsArray().size(), 4u);
    EXPECT_EQ(page.Find("items")->AsArray().back().AsInt(), 40000);
    ASSERT_TRUE(adapter->ReadSettingsPage("/text", 0, 64, json)); EXPECT_LT(json.size(), 4096u);
    ASSERT_TRUE(editor::ReadManagedSettingsJson(json, page)); EXPECT_EQ(page.Find("text")->AsString().size(), 64u);
    ImGuiFrame frame;
    for (int index = 0; index < 3; ++index) { editor::ManagedSettingsReflector reflector(Context(), owner, entry); reflector.Draw(); reflector.Finish(); }
    EXPECT_EQ(m_undo.GetHistorySize(), 0u);
}

TEST_F(ManagedCompletionTest, InspectorKeepsSettingsVisibleAndReadsRuntimeStateOnlyWhenExpanded)
{
    Load(); auto& owner = AttachedScene()->CreateGameObject("SettingsOwner");
    auto& entry = owner.AddComponent<scene::ScriptComponent>().scripts.emplace_back();
    entry.script = Create(owner, "ManagedFixtures.SettingsComponent", entry.savedScriptId); ASSERT_NE(entry.script, nullptr);
    auto* adapter = scene::ManagedScriptRuntime::Instance().FindAdapter(entry.script.get()); ASSERT_NE(adapter, nullptr);
    const std::string before = scene::SceneSerializer::SaveToText(*AttachedScene());
    ImGuiFrame frame;
    editor::ManagedSettingsReflector reflector(Context(), owner, entry);
    reflector.Draw(); reflector.Finish();
    const auto draft = ManagedSettingsReflectorTestAccess::Draft(reflector);
    const auto wasRead = [&](const std::string& key) {
        return draft->pages.contains(editor::ManagedSettingChildPath("", key) + "#0:0");
    };
    for (const auto& field : adapter->Fields()) {
        const bool visibleSetting = (field.flags & scene::kManagedFieldSetting) && !(field.flags & scene::kManagedFieldHidden);
        EXPECT_EQ(wasRead(field.name), visibleSetting) << field.name;
    }
    EXPECT_FALSE(wasRead("stableSpeed"));
    ImGui::GetStateStorage()->SetInt(ImGui::GetID("実行時の状態（読み取り専用）##managed_runtime_state"), 1);
    reflector.Draw(); reflector.Finish();
    for (const auto& field : adapter->Fields()) {
        const bool inspectable = (field.flags & scene::kManagedFieldObservation) && !(field.flags & scene::kManagedFieldHidden);
        EXPECT_EQ(wasRead(field.name), inspectable) << field.name;
    }
    EXPECT_EQ(m_undo.GetHistorySize(), 0u);
    EXPECT_EQ(scene::SceneSerializer::SaveToText(*AttachedScene()), before);
    const auto target = editor::CaptureManagedSettingsTarget(Context(), owner, entry);
    editor::OpData fields = editor::OpData::MakeObject(); fields.Set("stableSpeed", 7.0f);
    ASSERT_TRUE(editor::ApplyManagedInspectorFields(Context(), target, fields).ok);
    ASSERT_TRUE(m_undo.Undo()); ASSERT_TRUE(m_undo.Redo());
    std::string snapshot; ASSERT_TRUE(adapter->ReadSettingsSnapshot(snapshot)); editor::OpData saved;
    ASSERT_TRUE(editor::ReadManagedSettingsJson(snapshot, saved));
    const auto* savedSettings = saved.Find("settings"); ASSERT_NE(savedSettings, nullptr);
    const auto* savedSpeed = savedSettings->Find("stableSpeed"); ASSERT_NE(savedSpeed, nullptr);
    double restoredSpeed = 0.0; ASSERT_TRUE(editor::TryReadManagedPrimitiveReal(*savedSpeed, restoredSpeed));
    EXPECT_DOUBLE_EQ(restoredSpeed, 7.0);
}

TEST_F(ManagedCompletionTest, InspectorRuntimeCollectionsRemainBrowsableWithoutAuthoringControls)
{
    Load(); auto& owner = AttachedScene()->CreateGameObject("RuntimeCollectionsOwner");
    auto& entry = owner.AddComponent<scene::ScriptComponent>().scripts.emplace_back();
    entry.script = Create(owner, "ManagedFixtures.InspectorPresentationComponent", entry.savedScriptId); ASSERT_NE(entry.script, nullptr);
    auto* adapter = scene::ManagedScriptRuntime::Instance().FindAdapter(entry.script.get()); ASSERT_NE(adapter, nullptr);
    const std::string before = scene::SceneSerializer::SaveToText(*AttachedScene());
    ImGuiFrame frame(1800.0f); editor::ManagedSettingsReflector reflector(Context(), owner, entry);
    reflector.Draw(); reflector.Finish();
    auto draft = ManagedSettingsReflectorTestAccess::Draft(reflector);
    EXPECT_TRUE(draft->pages.contains("/m_authoredGain#0:0"));
    EXPECT_FALSE(draft->pages.contains("/hiddenValue#0:0"));
    for (const auto& field : adapter->Fields()) {
        if (field.name == "m_authoredGain" || field.name == "precision" || field.name == "counts")
            EXPECT_NE(field.flags & scene::kManagedFieldSetting, 0u) << field.name;
        if (field.name.ends_with("observedCounts") || field.name.ends_with("observedTargets") || field.name.ends_with("status") || field.name.ends_with("m_cachedDirection"))
            EXPECT_EQ(field.flags & scene::kManagedFieldSetting, 0u) << field.name;
        if (!(field.flags & scene::kManagedFieldSetting))
            EXPECT_FALSE(draft->pages.contains(editor::ManagedSettingChildPath("", field.name) + "#0:0")) << field.name;
        EXPECT_EQ(field.name.find("AutomaticProperty"), std::string::npos);
        EXPECT_EQ(field.name.find("GetterTrap"), std::string::npos);
        EXPECT_EQ(field.name.find("getterReads"), std::string::npos);
    }
    ImGui::GetStateStorage()->SetInt(ImGui::GetID("実行時の状態（読み取り専用）##managed_runtime_state"), 1);
    ImGui::SetCursorPos({10.0f, 10.0f});
    reflector.Draw(); reflector.Finish();
    for (const auto& field : adapter->Fields()) {
        if (!(field.flags & scene::kManagedFieldSetting) && (field.flags & scene::kManagedFieldObservation))
            EXPECT_TRUE(draft->pages.contains(editor::ManagedSettingChildPath("", field.name) + "#0:0")) << field.name;
        if (field.name.ends_with("m_unsupportedCache")) EXPECT_FALSE(draft->pages.contains(editor::ManagedSettingChildPath("", field.name) + "#0:0"));
    }
    ImGui::SetCursorPos({10.0f, 10.0f});
    ImGui::LogToBuffer();
    ManagedSettingsReflectorTestAccess::DrawRuntimeFields(reflector); reflector.Finish();
    /// @note ImGui::LogFinish clears the internal buffer even for buffer-only logging.
    const std::string labels(frame.current->LogBuffer.c_str()); ImGui::LogFinish();
    EXPECT_NE(labels.find("observedCounts"), std::string::npos);
    EXPECT_EQ(labels.find("追加"), std::string::npos);
    EXPECT_EQ(labels.find("削除"), std::string::npos);
    EXPECT_EQ(labels.find("上へ"), std::string::npos);
    EXPECT_EQ(labels.find("下へ"), std::string::npos);
    EXPECT_EQ(labels.find("作成"), std::string::npos);
    EXPECT_EQ(labels.find("null にする"), std::string::npos);
    EXPECT_EQ(labels.find("generatedValue"), std::string::npos);
    EXPECT_EQ(labels.find("hiddenValue"), std::string::npos);
    EXPECT_EQ(m_undo.GetHistorySize(), 0u);
    EXPECT_EQ(scene::SceneSerializer::SaveToText(*AttachedScene()), before);
}

TEST_F(ManagedCompletionTest, InspectorMultiSelectionKeepsInheritedSettingsAndDoesNotReadPrivateStateByDefault)
{
    Load();
    std::vector<std::pair<scene::GameObject*, const scene::ScriptEntry*>> selection;
    for (int index = 0; index < 2; ++index) {
        auto& owner = AttachedScene()->CreateGameObject("InheritedSettingsOwner" + std::to_string(index));
        auto& entry = owner.AddComponent<scene::ScriptComponent>().scripts.emplace_back();
        entry.script = Create(owner, "ManagedFixtures.InheritedCallbackComponent", entry.savedScriptId); ASSERT_NE(entry.script, nullptr);
        selection.emplace_back(&owner, &entry);
    }
    const std::string before = scene::SceneSerializer::SaveToText(*AttachedScene());
    ImGuiFrame frame; editor::ManagedSettingsReflector reflector(Context(), selection); reflector.Draw(); reflector.Finish();
    const auto draft = ManagedSettingsReflectorTestAccess::Draft(reflector);
    EXPECT_TRUE(draft->pages.contains("/baseSpeed#0:0"));
    EXPECT_EQ(draft->pages.size(), 1u);
    EXPECT_TRUE(draft->mixedPaths.empty());
    EXPECT_EQ(m_undo.GetHistorySize(), 0u);
    EXPECT_EQ(scene::SceneSerializer::SaveToText(*AttachedScene()), before);
}

TEST_F(ManagedCompletionTest, ComponentReferenceLabelsUseNamesAndExactDuplicateOrdinalsWithoutChangingAddresses)
{
    Load(); auto& owner = AttachedScene()->CreateGameObject("Fish School"); const std::string ownerGuid = owner.instanceId;
    auto& scripts = owner.AddComponent<scene::ScriptComponent>().scripts;
    for (int index = 0; index < 2; ++index) {
        auto& entry = scripts.emplace_back();
        entry.script = Create(owner, "ManagedFixtures.HealthComponent", entry.savedScriptId); ASSERT_NE(entry.script, nullptr);
    }
    auto& inspector = AttachedScene()->CreateGameObject("ReferenceOwner");
    auto& entry = inspector.AddComponent<scene::ScriptComponent>().scripts.emplace_back();
    entry.script = Create(inspector, "ManagedFixtures.TypedReferenceComponent", entry.savedScriptId); ASSERT_NE(entry.script, nullptr);
    editor::ManagedSettingsReflector reflector(Context(), inspector, entry);
    editor::OpData schema = editor::OpData::MakeObject(); schema.Set("targetType", "Fbzz.Component");
    editor::OpData address = editor::OpData::MakeObject(); address.Set("kind", "script"); address.Set("ownerGuid", ownerGuid);
    address.Set("typeKey", scripts.front().script->GetTypeName());
    for (size_t index = 0; index < scripts.size(); ++index) {
        address.Set("savedScriptId", scripts[index].savedScriptId);
        std::string before; ASSERT_TRUE(editor::WriteOpDataJson(address, before));
        const std::string label = ManagedSettingsReflectorTestAccess::ReferenceLabel(reflector, schema, address);
        EXPECT_EQ(label, "Fish School / " + scene::ScriptFactory::DisplayName(scripts[index].script->GetTypeName()) + " (" + std::to_string(index + 1) + ")");
        EXPECT_EQ(label.find(ownerGuid), std::string::npos); EXPECT_EQ(label.find(scripts[index].savedScriptId), std::string::npos);
        std::string after; ASSERT_TRUE(editor::WriteOpDataJson(address, after)); EXPECT_EQ(after, before);
    }
    address.Set("typeKey", "ManagedFixtures.HealthComponent"); address.Set("savedScriptId", scripts.front().savedScriptId);
    EXPECT_EQ(ManagedSettingsReflectorTestAccess::ReferenceLabel(reflector, schema, address),
        "Fish School / " + scene::ScriptFactory::DisplayName(scripts.front().script->GetTypeName()) + " (1)");
    address.Set("savedScriptId", "missing-script");
    EXPECT_NE(ManagedSettingsReflectorTestAccess::ReferenceLabel(reflector, schema, address).find("見つかりません"), std::string::npos);
    address.Set("ownerGuid", "missing-owner");
    EXPECT_NE(ManagedSettingsReflectorTestAccess::ReferenceLabel(reflector, schema, address).find("見つかりません"), std::string::npos);
    editor::OpData native = editor::OpData::MakeObject(); native.Set("kind", 0); native.Set("type", "Fbzz.Transform"); native.Set("nativeName", "TransformComponent");
    editor::OpData kinds = editor::OpData::MakeArray(); kinds.Push(native); schema.Set("nativeKinds", kinds);
    address.Set("kind", "native"); address.Set("componentKind", 0); address.Set("ownerGuid", ownerGuid);
    EXPECT_EQ(ManagedSettingsReflectorTestAccess::ReferenceLabel(reflector, schema, address), "Fish School / Transform");
    native.Set("kind", 1); native.Set("type", "Fbzz.Rigidbody"); native.Set("nativeName", "RigidBodyComponent");
    kinds = editor::OpData::MakeArray(); kinds.Push(native); schema.Set("nativeKinds", kinds); address.Set("componentKind", 1);
    EXPECT_EQ(ManagedSettingsReflectorTestAccess::ReferenceLabel(reflector, schema, address), "Fish School / (見つかりません) / Rigid Body");
    EXPECT_EQ(m_undo.GetHistorySize(), 0u);
}

TEST_F(ManagedCompletionTest, InspectorRetainsPendingInputsAcrossCacheEvictionAndSessionBudget)
{
    Load();
    ImGuiFrame frame;
    std::vector<std::pair<scene::GameObject*, scene::ScriptEntry*>> selections;
    selections.reserve(70);
    for (int index = 0; index < 70; ++index) {
        auto& owner = AttachedScene()->CreateGameObject("DraftSelection" + std::to_string(index));
        auto& entry = owner.AddComponent<scene::ScriptComponent>().scripts.emplace_back();
        entry.script = Create(owner, "ManagedFixtures.HealthComponent", entry.savedScriptId);
        ASSERT_NE(entry.script, nullptr);
        selections.emplace_back(&owner, &entry);
    }

    std::shared_ptr<editor::ManagedSettingsDraft> previous;
    for (int index = 0; index < 34; ++index) {
        auto& [owner, entry] = selections[static_cast<std::size_t>(index)];
        editor::ManagedSettingsReflector reflector(Context(), *owner, *entry);
        reflector.Draw();
        if (previous) {
            EXPECT_EQ(previous->editSession, 0u);
            EXPECT_EQ(previous->activeId, 0u);
            EXPECT_EQ(ImGui::GetActiveID(), 0u);
        }
        ManagedSettingsReflectorTestAccess::Queue(reflector, "/health", editor::OpData(index + 20));
        ASSERT_NE(ManagedSettingsReflectorTestAccess::EditSession(reflector), 0u);
        ManagedSettingsReflectorTestAccess::HoldInput(reflector, "/health", "health");
        previous = ManagedSettingsReflectorTestAccess::Draft(reflector);
    }

    for (int index = 34; index < 70; ++index) {
        auto& [owner, entry] = selections[static_cast<std::size_t>(index)];
        editor::ManagedSettingsReflector reflector(Context(), *owner, *entry);
        reflector.Draw();
        reflector.Finish();
    }

    for (int index = 0; index < 34; ++index) {
        auto& [owner, entry] = selections[static_cast<std::size_t>(index)];
        editor::ManagedSettingsReflector reflector(Context(), *owner, *entry);
        reflector.Draw();
        EXPECT_EQ(ManagedSettingsReflectorTestAccess::PendingInt(reflector, 0), index + 20);
        EXPECT_EQ(ManagedSettingsReflectorTestAccess::EditSession(reflector), 0u);
        ASSERT_EQ(ManagedSettingsReflectorTestAccess::PinnedRevisionCount(reflector), 1u);
        ManagedSettingsReflectorTestAccess::ReleaseInput(reflector);
        reflector.Finish();
        auto* adapter = scene::ManagedScriptRuntime::Instance().FindAdapter(entry->script.get()); ASSERT_NE(adapter, nullptr);
        std::string json; editor::OpData page;
        ASSERT_TRUE(adapter->ReadSettingsPage("/health", 0, 1, json));
        ASSERT_TRUE(editor::ReadManagedSettingsJson(json, page));
        EXPECT_EQ(page.Find("value")->AsInt(), index + 20);
    }

    std::vector<std::pair<scene::GameObject*, const scene::ScriptEntry*>> multiple{
        {selections[68].first, selections[68].second}, {selections[69].first, selections[69].second}};
    {
        editor::ManagedSettingsReflector reflector(Context(), multiple);
        reflector.Draw();
        ManagedSettingsReflectorTestAccess::Queue(reflector, "/health", editor::OpData(77));
        ASSERT_NE(ManagedSettingsReflectorTestAccess::EditSession(reflector), 0u);
        ManagedSettingsReflectorTestAccess::HoldInput(reflector, "/health", "health");
        previous = ManagedSettingsReflectorTestAccess::Draft(reflector);
    }
    {
        editor::ManagedSettingsReflector other(Context(), *selections[67].first, *selections[67].second);
        other.Draw();
        EXPECT_EQ(previous->editSession, 0u);
        EXPECT_EQ(previous->activeId, 0u);
    }
    {
        editor::ManagedSettingsReflector reflector(Context(), multiple);
        reflector.Draw();
        EXPECT_EQ(ManagedSettingsReflectorTestAccess::PendingInt(reflector, 0), 77);
        EXPECT_EQ(ManagedSettingsReflectorTestAccess::EditSession(reflector), 0u);
        ManagedSettingsReflectorTestAccess::ReleaseInput(reflector);
        reflector.Finish();
    }
    for (std::size_t index : {68u, 69u}) {
        auto* adapter = scene::ManagedScriptRuntime::Instance().FindAdapter(selections[index].second->script.get()); ASSERT_NE(adapter, nullptr);
        std::string json; editor::OpData page;
        ASSERT_TRUE(adapter->ReadSettingsPage("/health", 0, 1, json));
        ASSERT_TRUE(editor::ReadManagedSettingsJson(json, page));
        EXPECT_EQ(page.Find("value")->AsInt(), 77);
    }
}

TEST_F(ManagedCompletionTest, InspectorHotReloadArchivesDirtyInputAndReleasesOldWidget)
{
    Load();
    auto& owner = AttachedScene()->CreateGameObject("ReloadDraftOwner");
    auto& entry = owner.AddComponent<scene::ScriptComponent>().scripts.emplace_back();
    entry.script = Create(owner, "ManagedFixtures.AuthoringTransactionComponent", entry.savedScriptId);
    ASSERT_NE(entry.script, nullptr);
    ImGuiFrame frame;
    uint64_t oldGeneration = 0;
    {
        editor::ManagedSettingsReflector reflector(Context(), owner, entry);
        reflector.Draw();
        oldGeneration = ManagedSettingsReflectorTestAccess::Generation(reflector);
        ManagedSettingsReflectorTestAccess::Queue(reflector, "/amount", editor::OpData(7));
        ManagedSettingsReflectorTestAccess::HoldInput(reflector, "/amount", "amount");
        ASSERT_NE(ImGui::GetActiveID(), 0u);
    }

    ASSERT_TRUE(scene::ManagedScriptRuntime::Instance().Reload(*AttachedScene(), FBZZ_MANAGED_FIXTURE_TEST_ASSEMBLY, true));
    auto* adapter = scene::ManagedScriptRuntime::Instance().FindAdapter(entry.script.get()); ASSERT_NE(adapter, nullptr);
    {
        editor::ManagedSettingsReflector reflector(Context(), owner, entry);
        ASSERT_NE(ManagedSettingsReflectorTestAccess::Generation(reflector), oldGeneration);
        reflector.Draw();
        EXPECT_EQ(ImGui::GetActiveID(), 0u);
        ASSERT_EQ(ManagedSettingsReflectorTestAccess::StaleCount(reflector), 1u);
        EXPECT_EQ(ManagedSettingsReflectorTestAccess::StaleGeneration(reflector, 0), oldGeneration);
        EXPECT_EQ(ManagedSettingsReflectorTestAccess::StaleInt(reflector, 0, 0), 7);
        EXPECT_EQ(ManagedSettingsReflectorTestAccess::StaleEditSession(reflector, 0), 0u);
        EXPECT_EQ(ManagedSettingsReflectorTestAccess::StaleActiveId(reflector, 0), 0u);
        EXPECT_TRUE(ManagedSettingsReflectorTestAccess::StaleSchemaContains(reflector, 0, "amount"));
        const std::string archived = ManagedSettingsReflectorTestAccess::CopyStale(reflector);
        EXPECT_NE(archived.find("/amount"), std::string::npos);
        EXPECT_NE(archived.find("\"value\":7"), std::string::npos);
        reflector.Finish();
        std::string json; editor::OpData page;
        ASSERT_TRUE(adapter->ReadSettingsPage("/amount", 0, 1, json));
        ASSERT_TRUE(editor::ReadManagedSettingsJson(json, page));
        EXPECT_EQ(page.Find("value")->AsInt(), 2);
        ManagedSettingsReflectorTestAccess::DiscardStale(reflector);
        EXPECT_EQ(ManagedSettingsReflectorTestAccess::StaleCount(reflector), 0u);
    }
}

TEST_F(ManagedCompletionTest, SchemaSnapshotKeepsSettingWidgetsEditableAndLegacyReadOnlyProjectionDisabled)
{
    Load();
    auto& owner = AttachedScene()->CreateGameObject("DraftOwner");
    auto& entry = owner.AddComponent<scene::ScriptComponent>().scripts.emplace_back();
    entry.script = Create(owner, "ManagedFixtures.HealthComponent", entry.savedScriptId); ASSERT_NE(entry.script, nullptr);
    auto* adapter = scene::ManagedScriptRuntime::Instance().FindAdapter(entry.script.get()); ASSERT_NE(adapter, nullptr);
    ImGuiFrame frame;
    editor::ManagedSettingsReflector reflector(Context(), owner, entry);
    reflector.Draw();
    EXPECT_EQ(frame.current->LastItemData.ItemFlags & ImGuiItemFlags_Disabled, 0);
    int value = 10;
    reflector.BeginField("health", "Health"); reflector.SetFieldReadOnly(true);
    reflector.Field("health", value);
    EXPECT_NE(frame.current->LastItemData.ItemFlags & ImGuiItemFlags_Disabled, 0);
    EXPECT_TRUE(reflector.FieldReadOnly()); EXPECT_EQ(value, 10);
    reflector.BeginField("runtimeObservation", "Runtime observation"); reflector.SetFieldReadOnly(true);
    reflector.Field("runtimeObservation", value);
    EXPECT_NE(frame.current->LastItemData.ItemFlags & ImGuiItemFlags_Disabled, 0);
    reflector.BeginField("health", "Health"); reflector.SetFieldReadOnly(true); reflector.SetFieldEnabled(false);
    reflector.Field("health", value);
    EXPECT_NE(frame.current->LastItemData.ItemFlags & ImGuiItemFlags_Disabled, 0);
    struct Read final : IgnoreFields { int health = -1; void Field(const char*, int& field) override { health = field; } } read;
    adapter->ReflectReadOnly(read); EXPECT_EQ(read.health, 10); EXPECT_EQ(m_undo.GetHistorySize(), 0u);
}

TEST_F(ManagedCompletionTest, ManagedSettingsJsonRejectsMalformedAndUnboundedValuesWithoutPartialOutput)
{
    editor::OpData output("unchanged");
    for (const std::string_view malformed : {"{\"x\":1,\"x\":2}", "[1,]", "01", "+1", "1e309", "9223372036854775808", "\"\\uD800\"", "\"\\uDC00\"", "{\"x\":NaN}", "null true"}) {
        EXPECT_FALSE(editor::ReadManagedSettingsJson(malformed, output)) << malformed;
        EXPECT_TRUE(output.IsString()); EXPECT_EQ(output.AsString(), "unchanged");
    }
    const std::string deep(66, '['); EXPECT_FALSE(editor::ReadManagedSettingsJson(deep + "0" + std::string(66, ']'), output));
    EXPECT_FALSE(editor::ReadManagedSettingsJson(std::string(1024u * 1024u + 1, ' '), output));
    editor::OpData oversized;
    ASSERT_TRUE(editor::ReadManagedSettingsJson("\"" + std::string(1024u * 1024u + 23, 'x') + "\"", oversized));
    EXPECT_EQ(oversized.AsString().size(), 1024u * 1024u + 23);
    const std::string invalidUtf8{'"', static_cast<char>(0xC0), static_cast<char>(0x80), '"'};
    EXPECT_FALSE(editor::ReadManagedSettingsJson(invalidUtf8, output)); EXPECT_EQ(output.AsString(), "unchanged");
    ASSERT_TRUE(editor::ReadManagedSettingsJson("1e99", output));
    ASSERT_TRUE(output.IsFloat()); EXPECT_EQ(output.AsDouble(), 1e99);
    float narrowed = 7.0f;
    EXPECT_FALSE(output.TryGetFloat(narrowed)); EXPECT_EQ(narrowed, 7.0f);
    ASSERT_TRUE(editor::ReadManagedSettingsJson("{\"minimum\":-2147483648,\"text\":\"\\u65E5\\uD83D\\uDE00\",\"values\":[0.25,true,null]}", output));
    ASSERT_NE(output.Find("minimum"), nullptr); EXPECT_EQ(output.Find("minimum")->AsInt(), std::numeric_limits<int>::min());
    ASSERT_NE(output.Find("text"), nullptr); EXPECT_EQ(output.Find("text")->AsString(), "\xE6\x97\xA5\xF0\x9F\x98\x80");
    ASSERT_NE(output.Find("values"), nullptr); EXPECT_EQ(output.Find("values")->AsArray().size(), 3u);
}
TEST_F(ManagedCompletionTest, InspectorAndAiUseTheSameExplicitResumeAndReadOnlyFaultStatus)
{
    Load();
    auto& object = AttachedScene()->CreateGameObject("Recovery");
    auto& entry = object.AddComponent<scene::ScriptComponent>().scripts.emplace_back();
    entry.script = Create(object, "ManagedFixtures.FaultRecoveryComponent", entry.savedScriptId);
    ASSERT_NE(entry.script, nullptr);
    auto& runtime = scene::ManagedScriptRuntime::Instance();
    auto* adapter = runtime.FindAdapter(entry.script.get());
    ASSERT_NE(adapter, nullptr); ASSERT_TRUE(adapter->RefreshOwnerBinding());
    ASSERT_TRUE(adapter->ApplySettingsJson("{\"target\":\"" + object.instanceId + "\",\"failPhase\":4}"));
    adapter->OnAwake(); adapter->OnUpdate();
    ASSERT_TRUE(adapter->IsRuntimeFaulted());
    const auto diagnostic = adapter->FaultDiagnostic();
    const auto scriptId = editor::CaptureManagedSettingsTarget(Context(), object, entry).scriptId;
    editor::OpArgs target;
    target.Set("node", object.instanceId); target.Set("scriptId", scriptId);
    ASSERT_TRUE(editor::CanInvokeOperator(Context(), "script.resume", target));
    auto wrongTarget = target;
    wrongTarget.Set("scriptId", adapter->InspectionId());
    EXPECT_FALSE(editor::CanInvokeOperator(Context(), "script.resume", wrongTarget));
    entry.m_pendingRemoval = true;
    EXPECT_FALSE(editor::CanInvokeOperator(Context(), "script.resume", target));
    entry.m_pendingRemoval = false;
    const auto status = editor::InvokeOperator(Context(), "script.status", target);
    ASSERT_TRUE(status.ok); ASSERT_NE(status.data.Find("diagnostic"), nullptr);
    EXPECT_EQ(status.data.Find("diagnostic")->AsString(), diagnostic);

    const auto bus = [&](const char* op, bool query) {
        editor::ai::JsonValue args = editor::ai::JsonValue::MakeObject();
        args.Set("node", editor::ai::JsonValue(object.instanceId));
        args.Set("scriptId", editor::ai::JsonValue(scriptId));
        editor::ai::JsonValue payload = editor::ai::JsonValue::MakeObject();
        payload.Set("t", editor::ai::JsonValue(query ? "editor.op.query" : "editor.op.invoke"));
        payload.Set("id", editor::ai::JsonValue(op)); payload.Set("args", std::move(args));
        editor::ai::JsonValue request = editor::ai::JsonValue::MakeObject();
        request.Set("protocol", editor::ai::JsonValue(editor::ai::kEditorProtocol));
        request.Set("id", editor::ai::JsonValue("resume-regression"));
        request.Set("kind", editor::ai::JsonValue(query ? "query" : "command"));
        request.Set("dryRun", editor::ai::JsonValue(false)); request.Set("payload", std::move(payload));
        editor::ai::EditorBusDispatcher dispatcher(Context());
        auto response = editor::ai::ParseJson(dispatcher.Handle(editor::ai::SerializeJson(request)));
        EXPECT_TRUE(response.has_value());
        return response.value_or(editor::ai::JsonValue{});
    };
    auto observed = bus("script.status", true);
    ASSERT_NE(observed.Find("ok"), nullptr); EXPECT_TRUE(observed.Find("ok")->AsBool());
    EXPECT_TRUE(adapter->IsRuntimeFaulted());
    auto readOnlyResume = bus("script.resume", true);
    ASSERT_NE(readOnlyResume.Find("ok"), nullptr); EXPECT_FALSE(readOnlyResume.Find("ok")->AsBool());
    EXPECT_TRUE(adapter->IsRuntimeFaulted());
    Context().managedBuildBusy = true;
    EXPECT_FALSE(editor::CanInvokeOperator(Context(), "script.resume", target));
    Context().managedBuildBusy = false;
    ASSERT_TRUE(adapter->ApplySettingsJson(R"({"target":null,"failPhase":0})",
        scene::ManagedScriptAdapter::SettingsValidationPurpose::AUTHORING));
    auto rejected = bus("script.resume", false);
    ASSERT_NE(rejected.Find("ok"), nullptr); EXPECT_FALSE(rejected.Find("ok")->AsBool());
    EXPECT_TRUE(adapter->IsRuntimeFaulted()); EXPECT_EQ(adapter->FaultDiagnostic(), diagnostic);
    ASSERT_TRUE(adapter->ApplySettingsJson("{\"target\":\"" + object.instanceId + "\"}"));
    auto resumed = bus("script.resume", false);
    ASSERT_NE(resumed.Find("ok"), nullptr); EXPECT_TRUE(resumed.Find("ok")->AsBool());
    EXPECT_FALSE(adapter->IsRuntimeFaulted()); EXPECT_TRUE(adapter->enabled);
    EXPECT_TRUE(adapter->FaultDiagnostic().empty());
    EXPECT_TRUE(entry.m_awoken); EXPECT_FALSE(entry.m_started);
    EXPECT_FALSE(editor::CanInvokeOperator(Context(), "script.resume", target));
    EXPECT_FALSE(m_undo.CanUndo());
}
TEST_F(ManagedCompletionTest, InspectorEditorActionUsesCandidatePreviewAndUndoWithoutRepeatingTheMethod)
{
    Load();
    auto& owner = AttachedScene()->CreateGameObject("EditorActionOwner");
    auto& entry = owner.AddComponent<scene::ScriptComponent>().scripts.emplace_back();
    entry.script = Create(owner, "ManagedFixtures.CompletionComponent", entry.savedScriptId);
    ASSERT_NE(entry.script, nullptr);
    auto* adapter = scene::ManagedScriptRuntime::Instance().FindAdapter(entry.script.get());
    ASSERT_NE(adapter, nullptr);
    const auto target = editor::CaptureManagedSettingsTarget(Context(), owner, entry);
    const auto read = [&](const char* key) {
        std::string json;
        EXPECT_TRUE(adapter->ReadSettingsSnapshot(json));
        editor::OpData snapshot;
        EXPECT_TRUE(editor::ReadManagedSettingsJson(json, snapshot));
        return snapshot.Find("settings")->Find(key)->AsInt64();
    };
    EXPECT_EQ(read("actionCalls"), 0);
    const auto applied = editor::ApplyManagedInspectorAction(Context(), target, "double-value");
    ASSERT_TRUE(applied.ok) << applied.errorCode << ": " << applied.message;
    EXPECT_EQ(read("actionCalls"), 1); EXPECT_EQ(read("actionValue"), 4); EXPECT_EQ(read("actionCoupled"), 5);
    m_undo.Undo();
    EXPECT_EQ(read("actionCalls"), 0); EXPECT_EQ(read("actionValue"), 2); EXPECT_EQ(read("actionCoupled"), 4);
    m_undo.Redo();
    EXPECT_EQ(read("actionCalls"), 1); EXPECT_EQ(read("actionValue"), 4); EXPECT_EQ(read("actionCoupled"), 5);
    editor::OpData fields = editor::OpData::MakeObject(); fields.Set("failAction", true);
    ASSERT_TRUE(editor::ApplyManagedInspectorFields(Context(), target, fields).ok);
    EXPECT_FALSE(editor::ApplyManagedInspectorAction(Context(), target, "double-value").ok);
    EXPECT_EQ(read("actionCalls"), 1); EXPECT_EQ(read("actionValue"), 4);
    EXPECT_FALSE(editor::ApplyManagedInspectorAction(Context(), target, "absent-action").ok);
    EXPECT_EQ(read("actionCalls"), 1);
}

TEST_F(ManagedCompletionTest, ConfigurePreviewAndReviewPagesBoundLargeValuesWithoutChangingCommitOrUndo)
{
    Load();
    auto& first = AttachedScene()->CreateGameObject("BoundedReviewA"); first.tag = "BoundedReview";
    auto& second = AttachedScene()->CreateGameObject("BoundedReviewB"); second.tag = "BoundedReview";
    editor::OpData objects = editor::OpData::MakeObject(); objects.Set("scope", "scene"); objects.Set("tag", "BoundedReview");
    editor::OpData settings = editor::OpData::MakeObject(); settings.Set("objects", std::move(objects));
    settings.Set("script", "ManagedFixtures.CapacitySettingsComponent"); settings.Set("fields", editor::OpData::MakeObject()); settings.Set("mode", "add");
    editor::OpArgs args; args.Set("settings", std::move(settings));
    const auto preview = editor::InvokeOperator(Context(), "script.configure.preview", args);
    ASSERT_TRUE(preview.ok) << preview.message;
    std::string external;
    ASSERT_TRUE(editor::WriteOpDataJson(preview.data, external)); EXPECT_LE(external.size(), 8192u);
    EXPECT_TRUE(preview.data.Find("detailsDeferred")->AsBool()); EXPECT_EQ(preview.data.Find("totalTargets")->AsInt64(), 2);
    ASSERT_NE(preview.data.Find("plan"), nullptr);
    const std::string token = preview.data.Find("plan")->AsString();
    editor::OpData options = editor::OpData::MakeObject(); options.Set("mode", "changes"); options.Set("targetIndex", 0);
    options.Set("changeCount", 1); options.Set("wireBytes", 4096);
    editor::OpArgs reviewArgs; reviewArgs.Set("plan", token); reviewArgs.Set("options", options);
    const auto review = editor::InvokeOperator(Context(), "script.configure.review", reviewArgs);
    ASSERT_TRUE(review.ok) << review.message;
    ASSERT_TRUE(editor::WriteOpDataJson(review.data, external)); EXPECT_LE(external.size(), 4096u);
    const auto* target = review.data.Find("target"); ASSERT_NE(target, nullptr);
    EXPECT_GE(target->Find("changedPathCount")->AsInt64(), 3);
    ASSERT_EQ(target->Find("changes")->AsArray().size(), 1u);
    EXPECT_NE(target->Find("nextChangeOffset"), nullptr);
    const auto& change = target->Find("changes")->AsArray().front();
    const std::string path = change.Find("path")->AsString();
    options = editor::OpData::MakeObject(); options.Set("mode", "value"); options.Set("targetIndex", 0);
    options.Set("changeIndex", change.Find("changeIndex")->AsInt64()); options.Set("side", "after"); options.Set("path", path);
    options.Set("start", path == "/text" ? 1024 * 1024 : 39998); options.Set("count", 3); options.Set("wireBytes", 4096);
    reviewArgs.Set("options", options);
    const auto values = editor::InvokeOperator(Context(), "script.configure.review", reviewArgs);
    ASSERT_TRUE(values.ok) << values.message;
    ASSERT_TRUE(editor::WriteOpDataJson(values.data, external)); EXPECT_LE(external.size(), 4096u);
    editor::OpArgs apply; apply.Set("plan", token);
    const auto applied = editor::InvokeOperator(Context(), "script.configure.apply", apply);
    ASSERT_TRUE(applied.ok) << applied.message;
    ASSERT_TRUE(editor::WriteOpDataJson(applied.data, external)); EXPECT_LE(external.size(), 8192u);
    auto* scripts = first.GetComponent<scene::ScriptComponent>(); ASSERT_NE(scripts, nullptr);
    auto* adapter = scene::ManagedScriptRuntime::Instance().FindAdapter(scripts->scripts.front().script.get()); ASSERT_NE(adapter, nullptr);
    std::string snapshotJson; ASSERT_TRUE(adapter->ReadSettingsSnapshot(snapshotJson));
    editor::OpData snapshot; ASSERT_TRUE(editor::ReadManagedSettingsJson(snapshotJson, snapshot));
    ASSERT_EQ(snapshot.Find("settings")->Find("values")->AsArray().size(), 40001u);
    EXPECT_EQ(snapshot.Find("settings")->Find("text")->AsString().size(), 1024u * 1024u + 23);
    ASSERT_TRUE(m_undo.Undo());
    scripts = first.GetComponent<scene::ScriptComponent>(); EXPECT_TRUE(!scripts || scripts->scripts.empty());
    ASSERT_TRUE(m_undo.Redo()); EXPECT_EQ(first.GetComponent<scene::ScriptComponent>()->scripts.size(), 1u);
}

TEST_F(ManagedCompletionTest, LargePlayAdoptionPagesLaterValuesAndSurvivesUndoSaveAndReopen)
{
    Load(); auto& owner = AttachedScene()->CreateGameObject("CapacityAdoptionOwner"); const std::string guid = owner.instanceId;
    auto& entry = owner.AddComponent<scene::ScriptComponent>().scripts.emplace_back();
    entry.script = Create(owner, "ManagedFixtures.CapacitySettingsComponent", entry.savedScriptId); ASSERT_NE(entry.script, nullptr);
    const std::string saved = entry.savedScriptId; auto* original = scene::ManagedScriptRuntime::Instance().FindAdapter(entry.script.get());
    editor::PlayModeController play; play.Play(*AttachedScene()); ASSERT_TRUE(play.IsPlaying()) << play.Diagnostic();
    struct Restore { editor::EditorContext& ctx; scene::Scene* source; editor::UndoStack& undo; ~Restore() { ctx.activeScene = source; ctx.editScene = source; ctx.undoStack = &undo; ctx.playMode = nullptr; } } restore{Context(), AttachedScene(), m_undo};
    Context().playMode = &play; Context().activeScene = play.PlayScene(); Context().undoStack = play.TemporaryUndo();
    auto* cloneOwner = play.PlayScene()->FindByGuid(guid); ASSERT_NE(cloneOwner, nullptr);
    auto& cloneEntry = cloneOwner->GetComponent<scene::ScriptComponent>()->scripts.front();
    const auto target = editor::CaptureManagedSettingsTarget(Context(), *cloneOwner, cloneEntry);
    editor::OpData patches = editor::OpData::MakeArray(), patch = editor::OpData::MakeObject();
    patch.Set("path", "/values/40000"); patch.Set("value", -991); patches.Push(patch);
    patch.Set("path", "/list/40000"); patch.Set("value", -992); patches.Push(patch);
    patch = editor::OpData::MakeObject(); patch.Set("path", "/text"); patch.Set("action", "text_replace");
    patch.Set("index", 1048598); patch.Set("count", 1); patch.Set("value", "Z"); patches.Push(patch);
    auto edited = editor::ApplyManagedInspectorFields(Context(), target, editor::OpData::MakeObject(), {}, {}, {}, patches);
    ASSERT_TRUE(edited.ok) << edited.message; ASSERT_EQ(play.TemporaryUndo()->GetHistorySize(), 1U); EXPECT_EQ(m_undo.GetHistorySize(), 0U);
    editor::OpData paths = editor::OpData::MakeArray(); paths.Push("/values"); paths.Push("/list"); paths.Push("/text");
    editor::OpData address = editor::OpData::MakeObject(); address.Set("ownerGuid", guid); address.Set("savedScriptId", saved); address.Set("paths", paths);
    editor::OpData targets = editor::OpData::MakeArray(); targets.Push(address); editor::OpData settings = editor::OpData::MakeObject(); settings.Set("targets", targets);
    editor::OpArgs args; args.Set("settings", settings); auto capture = editor::InvokeOperator(Context(), "script.play_changes.capture", args);
    ASSERT_TRUE(capture.ok) << capture.message; EXPECT_TRUE(capture.data.Find("readyForAdoption")->AsBool());
    args.Set("adoptionId", capture.data.Find("adoptionId")->AsString());
    play.Stop(*play.PlayScene()); Context().undoStack = &m_undo; ASSERT_TRUE(play.ApplyPendingRestore(*AttachedScene())); Context().activeScene = AttachedScene();
    const auto read = [&](scene::ManagedScriptAdapter& source, const char* path, uint64_t start = 0, uint32_t count = 1) {
        std::string wire; editor::OpData page; EXPECT_TRUE(source.ReadSettingsPage(path, start, count, wire)); EXPECT_TRUE(editor::ReadManagedSettingsJson(wire, page)); return page;
    };
    EXPECT_EQ(read(*original, "/values/40000").Find("value")->AsInt(), 40000);
    EXPECT_EQ(read(*original, "/list/40000").Find("value")->AsInt(), 40000);
    EXPECT_EQ(read(*original, "/text", 1048598).Find("text")->AsString(), "x");
    editor::OpData options = editor::OpData::MakeObject(); options.Set("action", "read"); options.Set("leafIndex", 0); options.Set("start", 39997); options.Set("count", 16); options.Set("packetBytes", 4096); args.Set("options", options);
    auto frozen = editor::InvokeOperator(Context(), "script.play_changes.review", args); ASSERT_TRUE(frozen.ok) << frozen.message;
    EXPECT_EQ(frozen.data.Find("total")->AsInt64(), 40001); ASSERT_EQ(frozen.data.Find("items")->AsArray().size(), 4U); EXPECT_EQ(frozen.data.Find("items")->AsArray().back().AsInt(), -991);
    options.Set("leafIndex", 2); options.Set("start", 1048598); options.Set("count", 64); args.Set("options", options);
    frozen = editor::InvokeOperator(Context(), "script.play_changes.review", args); ASSERT_TRUE(frozen.ok) << frozen.message; EXPECT_EQ(frozen.data.Find("text")->AsString(), "Z");
    auto prepared = editor::InvokeOperator(Context(), "script.play_changes.prepare", args); ASSERT_TRUE(prepared.ok) << prepared.message;
    editor::OpArgs apply; apply.Set("plan", prepared.data.Find("plan")->AsString()); auto adopted = editor::InvokeOperator(Context(), "script.configure.apply", apply);
    ASSERT_TRUE(adopted.ok) << adopted.message; EXPECT_EQ(m_undo.GetHistorySize(), 1U);
    EXPECT_EQ(read(*original, "/values/40000").Find("value")->AsInt(), -991); EXPECT_EQ(read(*original, "/list/40000").Find("value")->AsInt(), -992);
    EXPECT_EQ(read(*original, "/text", 1048598).Find("text")->AsString(), "Z");
    ASSERT_TRUE(m_undo.Undo()) << m_undo.LastDiagnostic(); EXPECT_EQ(read(*original, "/values/40000").Find("value")->AsInt(), 40000); EXPECT_EQ(read(*original, "/text", 1048598).Find("text")->AsString(), "x");
    ASSERT_TRUE(m_undo.Redo()) << m_undo.LastDiagnostic(); EXPECT_EQ(read(*original, "/values/40000").Find("value")->AsInt(), -991); EXPECT_EQ(read(*original, "/text", 1048598).Find("text")->AsString(), "Z");
    const auto serialized = scene::SceneSerializer::SaveToText(*AttachedScene()); ASSERT_GT(serialized.size(), 1024U * 1024U);
    auto reopened = scene::SceneSerializer::LoadDataFromText(serialized, File("large-adoption.scene").generic_string()); ASSERT_NE(reopened, nullptr);
    auto* reopenedOwner = reopened->FindByGuid(guid); ASSERT_NE(reopenedOwner, nullptr); auto& reopenedEntry = reopenedOwner->GetComponent<scene::ScriptComponent>()->scripts.front(); EXPECT_EQ(reopenedEntry.savedScriptId, saved);
    auto* reloaded = scene::ManagedScriptRuntime::Instance().FindAdapter(reopenedEntry.script.get()); ASSERT_NE(reloaded, nullptr);
    const auto late = read(*reloaded, "/values", 39997, 16); EXPECT_EQ(late.Find("total")->AsInt64(), 40001); EXPECT_EQ(late.Find("items")->AsArray().back().AsInt(), -991);
    EXPECT_EQ(read(*reloaded, "/list/40000").Find("value")->AsInt(), -992); EXPECT_EQ(read(*reloaded, "/text", 1048598).Find("text")->AsString(), "Z");
}

TEST_F(ManagedCompletionTest, ConflictingAdoptionKeepsDraftReviewableAndRetryUsesOneCheckedUndo)
{
    Load(); auto& owner = AttachedScene()->CreateGameObject("AdoptionConflictOwner"); const std::string guid = owner.instanceId;
    auto& entry = owner.AddComponent<scene::ScriptComponent>().scripts.emplace_back();
    entry.script = Create(owner, "ManagedFixtures.HealthComponent", entry.savedScriptId); ASSERT_NE(entry.script, nullptr);
    const std::string saved = entry.savedScriptId; auto* original = scene::ManagedScriptRuntime::Instance().FindAdapter(entry.script.get());
    ASSERT_TRUE(original->ApplySettingsJson("{\"health\":4}"));
    editor::PlayModeController play; play.Play(*AttachedScene()); ASSERT_TRUE(play.IsPlaying()) << play.Diagnostic();
    struct Restore { editor::EditorContext& ctx; scene::Scene* source; editor::UndoStack& undo; ~Restore() { ctx.activeScene = source; ctx.editScene = source; ctx.undoStack = &undo; ctx.playMode = nullptr; } } restore{Context(), AttachedScene(), m_undo};
    Context().playMode = &play; Context().activeScene = play.PlayScene(); Context().undoStack = play.TemporaryUndo();
    auto* clone = scene::ManagedScriptRuntime::Instance().FindAdapter(play.PlayScene()->FindByGuid(guid)->GetComponent<scene::ScriptComponent>()->scripts.front().script.get());
    ASSERT_TRUE(clone->ApplySettingsJson("{\"health\":2}"));
    editor::OpData paths = editor::OpData::MakeArray(); paths.Push("/health"); editor::OpData target = editor::OpData::MakeObject();
    target.Set("ownerGuid", guid); target.Set("savedScriptId", saved); target.Set("paths", paths);
    editor::OpData targets = editor::OpData::MakeArray(); targets.Push(target); editor::OpData settings = editor::OpData::MakeObject(); settings.Set("targets", targets);
    editor::OpArgs args; args.Set("settings", settings); auto held = editor::InvokeOperator(Context(), "script.play_changes.capture", args); ASSERT_TRUE(held.ok) << held.message;
    args.Set("adoptionId", held.data.Find("adoptionId")->AsString());
    play.Stop(*play.PlayScene()); Context().undoStack = &m_undo; ASSERT_TRUE(play.ApplyPendingRestore(*AttachedScene())); Context().activeScene = AttachedScene();
    auto prepared = editor::InvokeOperator(Context(), "script.play_changes.prepare", args); ASSERT_TRUE(prepared.ok) << prepared.message;
    editor::OpArgs apply; apply.Set("plan", prepared.data.Find("plan")->AsString());
    ASSERT_TRUE(original->ApplySettingsJson("{\"health\":7}"));
    auto conflict = editor::InvokeOperator(Context(), "script.configure.apply", apply); EXPECT_FALSE(conflict.ok); EXPECT_EQ(m_undo.GetHistorySize(), 0U);
    std::string wire; editor::OpData page; ASSERT_TRUE(original->ReadSettingsPage("/health", 0, 1, wire)); ASSERT_TRUE(editor::ReadManagedSettingsJson(wire, page)); EXPECT_EQ(page.Find("value")->AsInt(), 7);
    editor::OpData options = editor::OpData::MakeObject(); options.Set("action", "read"); options.Set("leafIndex", 0); args.Set("options", options);
    auto review = editor::InvokeOperator(Context(), "script.play_changes.review", args); ASSERT_TRUE(review.ok) << review.message; EXPECT_EQ(review.data.Find("value")->AsInt(), 2);
    prepared = editor::InvokeOperator(Context(), "script.play_changes.prepare", args); ASSERT_TRUE(prepared.ok) << prepared.message; apply.Set("plan", prepared.data.Find("plan")->AsString());
    auto adopted = editor::InvokeOperator(Context(), "script.configure.apply", apply); ASSERT_TRUE(adopted.ok) << adopted.message; EXPECT_EQ(m_undo.GetHistorySize(), 1U);
    ASSERT_TRUE(m_undo.Undo()) << m_undo.LastDiagnostic(); ASSERT_TRUE(original->ReadSettingsPage("/health", 0, 1, wire)); ASSERT_TRUE(editor::ReadManagedSettingsJson(wire, page)); EXPECT_EQ(page.Find("value")->AsInt(), 7);
    ASSERT_TRUE(m_undo.Redo()) << m_undo.LastDiagnostic(); ASSERT_TRUE(original->ReadSettingsPage("/health", 0, 1, wire)); ASSERT_TRUE(editor::ReadManagedSettingsJson(wire, page)); EXPECT_EQ(page.Find("value")->AsInt(), 2);
}

TEST_F(ManagedCompletionTest, CallerSizedAtomicSettingsBatchBeyondFormerTargetLimitKeepsConflictHistory)
{
    Load(); auto& owner = AttachedScene()->CreateGameObject("BatchCapacityOwner");
    auto& scripts = owner.AddComponent<scene::ScriptComponent>(); scripts.scripts.reserve(257);
    editor::OpData targets = editor::OpData::MakeArray();
    for (int index = 0; index < 257; ++index) {
        auto& entry = scripts.scripts.emplace_back(); entry.script = Create(owner, "ManagedFixtures.HealthComponent", entry.savedScriptId); ASSERT_NE(entry.script, nullptr);
        editor::OpData target = editor::OpData::MakeObject(); target.Set("ownerGuid", owner.instanceId); target.Set("savedScriptId", entry.savedScriptId); targets.Push(target);
    }
    editor::OpData fields = editor::OpData::MakeObject(); fields.Set("health", 2);
    editor::OpData settings = editor::OpData::MakeObject(); settings.Set("targets", targets); settings.Set("script", "ManagedFixtures.HealthComponent"); settings.Set("fields", fields);
    editor::OpArgs args; args.Set("settings", settings); auto prepared = editor::InvokeOperator(Context(), "script.configure.preview", args); ASSERT_TRUE(prepared.ok) << prepared.message;
    editor::OpArgs apply; apply.Set("plan", prepared.data.Find("plan")->AsString()); auto edited = editor::InvokeOperator(Context(), "script.configure.apply", apply); ASSERT_TRUE(edited.ok) << edited.message;
    EXPECT_EQ(m_undo.GetHistorySize(), 1U);
    const auto health = [&](size_t index) {
        auto* adapter = scene::ManagedScriptRuntime::Instance().FindAdapter(scripts.scripts[index].script.get());
        std::string wire; editor::OpData page; EXPECT_TRUE(adapter->ReadSettingsPage("/health", 0, 1, wire)); EXPECT_TRUE(editor::ReadManagedSettingsJson(wire, page));
        const auto* value = page.Find("value"); return value ? value->AsInt(-1) : -1;
    };
    for (size_t index = 0; index < scripts.scripts.size(); ++index) EXPECT_EQ(health(index), 2) << index;
    ASSERT_TRUE(m_undo.Undo()) << m_undo.LastDiagnostic();
    for (size_t index = 0; index < scripts.scripts.size(); ++index) EXPECT_EQ(health(index), 10) << index;
    ASSERT_TRUE(m_undo.Redo()) << m_undo.LastDiagnostic();
    for (size_t index = 0; index < scripts.scripts.size(); ++index) EXPECT_EQ(health(index), 2) << index;
    auto* last = scene::ManagedScriptRuntime::Instance().FindAdapter(scripts.scripts.back().script.get()); ASSERT_TRUE(last->ApplySettingsJson("{\"health\":7}"));
    EXPECT_FALSE(m_undo.Undo()); EXPECT_EQ(m_undo.GetHistorySize(), 1U); EXPECT_TRUE(m_undo.CanUndo()); EXPECT_FALSE(m_undo.CanRedo());
    EXPECT_EQ(health(0), 2); EXPECT_EQ(health(255), 2); EXPECT_EQ(health(256), 7);
}

TEST_F(ManagedCompletionTest, ExactPrimitiveValuesConfigureUndoSaveAndReopenWithoutNumericNarrowing)
{
    Load(); auto& owner = AttachedScene()->CreateGameObject("PrimitiveOwner"); const std::string guid = owner.instanceId;
    auto& entry = owner.AddComponent<scene::ScriptComponent>().scripts.emplace_back();
    entry.script = Create(owner, "ManagedFixtures.PrimitiveSettingsComponent", entry.savedScriptId); ASSERT_NE(entry.script, nullptr);
    const std::string saved = entry.savedScriptId;
    auto* adapter = scene::ManagedScriptRuntime::Instance().FindAdapter(entry.script.get()); ASSERT_NE(adapter, nullptr);
    const auto target = editor::CaptureManagedSettingsTarget(Context(), owner, entry);
    const auto read = [](scene::ManagedScriptAdapter& source, const char* path) {
        std::string json; editor::OpData page; EXPECT_TRUE(source.ReadSettingsPage(path, 0, 1, json)); EXPECT_TRUE(editor::ReadManagedSettingsJson(json, page));
        const auto* value = page.Find("value"); return value ? *value : editor::OpData{};
    };
    EXPECT_EQ(read(*adapter, "/signed").AsString(), "-9223372036854775808");
    EXPECT_EQ(read(*adapter, "/unsigned").AsString(), "18446744073709551615");
    EXPECT_EQ(read(*adapter, "/amount").AsString(), "79228162514264337593543950335");
    EXPECT_EQ(read(*adapter, "/code").AsInt64(), 57343);
    EXPECT_DOUBLE_EQ(read(*adapter, "/real").AsDouble(), std::numeric_limits<double>::max());
    {
        ImGuiFrame frame; editor::ManagedSettingsReflector reflector(Context(), owner, entry); reflector.Draw(); reflector.Finish();
        EXPECT_EQ(m_undo.GetHistorySize(), 0U);
    }
    editor::OpData fields = editor::OpData::MakeObject(); fields.Set("signed", "9007199254740993"); fields.Set("unsigned", "18446744073709551614");
    fields.Set("amount", "-12345678901234567890.12345678"); fields.Set("real", 1e300); fields.Set("code", 55296);
    fields.Set("options", "9223372036854775809"); fields.Set("signed8", -7); fields.Set("unsigned8", 254); fields.Set("signed16", -32767); fields.Set("unsigned16", 65534);
    editor::OpData patches = editor::OpData::MakeArray(), patch = editor::OpData::MakeObject();
    patch.Set("path", "/signedPath/1"); patch.Set("value", "-9223372036854775808"); patches.Push(patch);
    patch.Set("path", "/unsignedPath/1"); patch.Set("value", "18446744073709551614"); patches.Push(patch);
    patch.Set("path", "/groups/0/code"); patch.Set("value", 55297); patches.Push(patch);
    patch.Set("path", "/groups/0/amount"); patch.Set("value", "0.0000000000000000000000000001"); patches.Push(patch);
    patch.Set("path", "/groups/0/real"); patch.Set("value", 1e299); patches.Push(patch);
    auto edited = editor::ApplyManagedInspectorFields(Context(), target, fields, {}, {}, {}, patches); ASSERT_TRUE(edited.ok) << edited.message;
    const auto check = [&](scene::ManagedScriptAdapter& source) {
        EXPECT_EQ(read(source, "/signed").AsString(), "9007199254740993"); EXPECT_EQ(read(source, "/unsigned").AsString(), "18446744073709551614");
        EXPECT_EQ(read(source, "/amount").AsString(), "-12345678901234567890.12345678"); EXPECT_DOUBLE_EQ(read(source, "/real").AsDouble(), 1e300);
        EXPECT_EQ(read(source, "/code").AsInt64(), 55296); EXPECT_EQ(read(source, "/options").AsString(), "9223372036854775809");
        EXPECT_EQ(read(source, "/signed8").AsInt64(), -7); EXPECT_EQ(read(source, "/unsigned8").AsInt64(), 254);
        EXPECT_EQ(read(source, "/signed16").AsInt64(), -32767); EXPECT_EQ(read(source, "/unsigned16").AsInt64(), 65534);
        EXPECT_EQ(read(source, "/signedPath/1").AsString(), "-9223372036854775808"); EXPECT_EQ(read(source, "/unsignedPath/1").AsString(), "18446744073709551614");
        EXPECT_EQ(read(source, "/groups/0/code").AsInt64(), 55297); EXPECT_EQ(read(source, "/groups/0/amount").AsString(), "0.0000000000000000000000000001");
        EXPECT_DOUBLE_EQ(read(source, "/groups/0/real").AsDouble(), 1e299);
    };
    check(*adapter); EXPECT_EQ(m_undo.GetHistorySize(), 1U);
    ASSERT_TRUE(m_undo.Undo()) << m_undo.LastDiagnostic(); EXPECT_EQ(read(*adapter, "/signed").AsString(), "-9223372036854775808");
    EXPECT_EQ(read(*adapter, "/amount").AsString(), "79228162514264337593543950335"); EXPECT_EQ(read(*adapter, "/code").AsInt64(), 57343);
    ASSERT_TRUE(m_undo.Redo()) << m_undo.LastDiagnostic(); check(*adapter);
    editor::OpData invalid = editor::OpData::MakeObject(); invalid.Set("signed", int64_t{9007199254740993});
    EXPECT_FALSE(editor::ApplyManagedInspectorFields(Context(), target, invalid).ok); EXPECT_EQ(m_undo.GetHistorySize(), 1U); check(*adapter);
    invalid = editor::OpData::MakeObject(); invalid.Set("options", "9223372036854775810");
    EXPECT_FALSE(editor::ApplyManagedInspectorFields(Context(), target, invalid).ok); EXPECT_EQ(m_undo.GetHistorySize(), 1U); check(*adapter);
    std::string diagnostic; const auto text = scene::SceneSerializer::SaveToText(*AttachedScene(), {}, diagnostic); ASSERT_FALSE(text.empty()) << diagnostic;
    auto reopened = scene::SceneSerializer::LoadDataFromText(text, File("exact-primitives.scene").generic_string()); ASSERT_NE(reopened, nullptr);
    auto* restoredOwner = reopened->FindByGuid(guid); ASSERT_NE(restoredOwner, nullptr); auto& restoredEntry = restoredOwner->GetComponent<scene::ScriptComponent>()->scripts.front();
    EXPECT_EQ(restoredEntry.savedScriptId, saved); auto* restored = scene::ManagedScriptRuntime::Instance().FindAdapter(restoredEntry.script.get()); ASSERT_NE(restored, nullptr); check(*restored);
}

} /// @note namespace fbzz::tests
