/// @file    ManagedScriptInspectionTests.cpp
/// @brief   fault 後の managed 観測と native getter 拒否を実接続で検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-04
#include <TestKit/TestKit.hpp>
#include <TestKit/Editor/EditorFixture.hpp>
#include <Editor/Ai/EditorBusDispatcher.hpp>
#include <Editor/Ai/EditorBusProtocol.hpp>
#include <Engine/Scene/Managed/ManagedScriptAdapter.hpp>
#include <Engine/Scene/Managed/ManagedScriptRuntime.hpp>
#include <Engine/Scene/ScriptComponent.hpp>
#include <Engine/Scene/ScriptFactory.hpp>
#include <Engine/Scene/SceneSerializer.hpp>
#include <algorithm>
#include <string_view>

namespace fbzz::tests {
namespace {

using editor::ai::JsonValue;

const JsonValue* Diagnostic(const JsonValue& result, const char* key, const char* code)
{
    const auto* diagnostics = result.Find("fieldDiagnostics");
    if (!diagnostics) return nullptr;
    for (const auto& diagnostic : diagnostics->AsArray()) {
        const auto* actualKey = diagnostic.Find("key");
        const auto* actualCode = diagnostic.Find("code");
        if (actualKey && actualCode && actualKey->AsString() == key && actualCode->AsString() == code) return &diagnostic;
    }
    return nullptr;
}

const JsonValue* Schema(const JsonValue& result, const char* key, const char* collection = "schema")
{
    const auto* schema = result.Find(collection);
    if (!schema) return nullptr;
    for (const auto& field : schema->AsArray())
        if (const auto* name = field.Find("name"); name && name->AsString() == key) return &field;
    return nullptr;
}

} /// @note namespace

class ManagedScriptInspectionTest : public testkit::EditorFixture {
protected:
    bool m_previousPlay = false;

    void SetUp() override
    {
        EditorFixture::SetUp();
        m_previousPlay = scene::Script::IsInPlayMode();
        scene::Script::SetInPlayMode(false);
        AttachScene();
    }

    void TearDown() override
    {
        scene::ManagedScriptRuntime::Instance().Unload(AttachedScene());
        scene::Script::SetInPlayMode(m_previousPlay);
        EditorFixture::TearDown();
    }

    JsonValue Send(const char* type, const std::string& id, const std::string& scriptId = {}, const JsonValue* options = nullptr)
    {
        JsonValue payload = JsonValue::MakeObject();
        payload.Set("t", JsonValue(type));
        payload.Set("id", JsonValue(id));
        if (std::string_view(type) == "script.catalog") payload.Set("type", JsonValue(id));
        if (!scriptId.empty()) payload.Set("scriptId", JsonValue(scriptId));
        if (options) for (const auto& [key, value] : options->AsObject()) payload.Set(key, value);
        JsonValue request = JsonValue::MakeObject();
        request.Set("protocol", JsonValue(editor::ai::kEditorProtocol));
        request.Set("id", JsonValue("managed-inspection-test"));
        request.Set("kind", JsonValue("query"));
        request.Set("payload", std::move(payload));
        editor::ai::EditorBusDispatcher dispatcher(Context());
        const auto response = editor::ai::ParseJson(dispatcher.Handle(editor::ai::SerializeJson(request)));
        EXPECT_TRUE(response.has_value());
        return response.value_or(JsonValue{});
    }

    std::string Handle(scene::GameObject& object)
    {
        const auto response = Send("node.components", object.instanceId);
        const auto* result = response.Find("result");
        if (!result || !result->Find("scripts") || result->Find("scripts")->AsArray().empty()) return {};
        return result->Find("scripts")->AsArray().front().Find("scriptId")->AsString();
    }

    scene::ManagedScriptAdapter* Attach(scene::GameObject& object, const char* type)
    {
        auto& entry = object.AddComponent<scene::ScriptComponent>().scripts.emplace_back();
        entry.script = scene::ScriptFactory::Create(type, entry.savedScriptId);
        if (!entry.script) return nullptr;
        entry.script->SetContext(AttachedScene(), &object);
        auto* adapter = scene::ManagedScriptRuntime::Instance().FindAdapter(entry.script.get());
        return adapter && adapter->RefreshOwnerBinding() ? adapter : nullptr;
    }
};

/// @note 一世代内で disabled、fault、field 読み取り失敗、失効参照を検証し、試験都合の GC 待ちを追加しない。
TEST_F(ManagedScriptInspectionTest, ReturnsSafeFieldsAndOriginalFaultWithoutRunningUserGetters)
{
    auto& runtime = scene::ManagedScriptRuntime::Instance();
    ASSERT_TRUE(runtime.Initialize(FBZZ_MANAGED_HOST_TEST_DIRECTORY)) << runtime.LastDiagnostic();
    ASSERT_TRUE(runtime.Load(FBZZ_MANAGED_FIXTURE_TEST_ASSEMBLY, true)) << runtime.LastDiagnostic();
    const auto trap = Send("script.catalog", "ManagedFixtures.DiscoveryConstructorTrapComponent");
    ASSERT_NE(trap.Find("result"), nullptr) << editor::ai::SerializeJson(trap);
    const auto& trapTypes = trap.Find("result")->Find("types")->AsArray(); ASSERT_EQ(trapTypes.size(), 1u);
    EXPECT_EQ(trapTypes.front().Find("defaultSource")->AsString(), "inert_schema");
    ASSERT_NE(Schema(trapTypes.front(), "value", "fields"), nullptr);
    EXPECT_EQ(Schema(trapTypes.front(), "value", "fields")->Find("default")->AsInt(), 0);
    const auto staticTrap = Send("script.catalog", "ManagedFixtures.DiscoveryStaticConstructorTrapComponent");
    ASSERT_NE(staticTrap.Find("result"), nullptr) << editor::ai::SerializeJson(staticTrap);
    const auto& staticTypes = staticTrap.Find("result")->Find("types")->AsArray(); ASSERT_EQ(staticTypes.size(), 1u);
    EXPECT_EQ(staticTypes.front().Find("defaultSource")->AsString(), "inert_schema");
    const auto catalogResponse = Send("script.catalog", "ManagedFixtures.FaultObservationComponent");
    ASSERT_NE(catalogResponse.Find("result"), nullptr) << editor::ai::SerializeJson(catalogResponse);
    const auto& catalogTypes = catalogResponse.Find("result")->Find("types")->AsArray();
    ASSERT_EQ(catalogTypes.size(), 1u);
    const auto& catalog = catalogTypes.front();
    ASSERT_NE(Schema(catalog, "rotation", "fields"), nullptr);
    EXPECT_EQ(Schema(catalog, "rotation", "fields")->Find("type")->AsString(), "quaternion");
    const auto& defaultRotation = Schema(catalog, "rotation", "fields")->Find("default")->AsArray();
    ASSERT_EQ(defaultRotation.size(), 4u);
    EXPECT_EQ(defaultRotation[3].AsNumber(), 1.0);
    ASSERT_NE(Schema(catalog, "settings", "fields"), nullptr);
    const auto& defaultSettings = *Schema(catalog, "settings", "fields")->Find("default");
    EXPECT_TRUE(defaultSettings.IsNull());
    EXPECT_EQ(catalog.Find("defaultSource")->AsString(), "inert_schema");
    EXPECT_EQ(Schema(catalog, "settings", "fields")->Find("codecVersion")->AsInt(), 1);
    ASSERT_NE(Schema(catalog, "settings", "fields")->Find("nested"), nullptr);
    ASSERT_NE(Schema(catalog, "samples", "fields"), nullptr);
    EXPECT_EQ(Schema(catalog, "samples", "fields")->Find("type")->AsString(), "list");
    const auto& defaultSamples = Schema(catalog, "samples", "fields")->Find("default")->AsArray();
    EXPECT_TRUE(defaultSamples.empty());
    ASSERT_NE(Schema(catalog, "optionalSettings", "fields"), nullptr);
    EXPECT_TRUE(Schema(catalog, "optionalSettings", "fields")->Find("default")->IsNull());
    ASSERT_NE(Schema(catalog, "weapon", "fields"), nullptr);
    EXPECT_EQ(Schema(catalog, "weapon", "fields")->Find("type")->AsString(), "dataAssetRef");
    EXPECT_TRUE(Schema(catalog, "weapon", "fields")->Find("default")->AsString().empty());
    EXPECT_EQ(catalog.Find("observationStatus")->AsString(), "schema_only");
    auto& scene = *AttachedScene();
    auto& owner = scene.CreateGameObject("Fault observer");
    auto& target = scene.CreateGameObject("Target");
    const auto targetId = target.GetID();
    const std::string targetGuid = target.instanceId;
    auto* adapter = Attach(owner, "cs-internal:ManagedFixtures.FaultObservationComponent");
    ASSERT_NE(adapter, nullptr);
    JsonValue settings = JsonValue::MakeObject();
    settings.Set("target", JsonValue(targetGuid));
    JsonValue targets = JsonValue::MakeArray();
    targets.Push(JsonValue(targetGuid)); targets.Push(JsonValue{}); targets.Push(JsonValue("missing-object-guid")); targets.Push(JsonValue(targetGuid));
    settings.Set("targets", std::move(targets));
    JsonValue nested = JsonValue::MakeObject();
    nested.Set("label", JsonValue("plain value"));
    nested.Set("target", JsonValue(targetGuid));
    /// @note 構造値の復元は constructor を呼ばないため、保持する値は入力に明示する。
    nested.Set("$version", JsonValue(23));
    JsonValue weights = JsonValue::MakeArray(); weights.Push(JsonValue(1.0)); weights.Push(JsonValue(2.0));
    nested.Set("weights", std::move(weights));
    JsonValue rotations = JsonValue::MakeArray();
    JsonValue rotation = JsonValue::MakeArray();
    for (const double value : {0.0, 0.0, 0.0, 1.0}) rotation.Push(JsonValue(value));
    rotations.Push(std::move(rotation));
    nested.Set("rotations", std::move(rotations));
    settings.Set("settings", std::move(nested));
    settings.Set("weapon", JsonValue("guid:missing-weapon-guid"));
    ASSERT_TRUE(adapter->ApplySettingsJson(editor::ai::SerializeJson(settings))) << runtime.LastDiagnostic();
    adapter->enabled = false;
    const auto handle = Handle(owner);
    ASSERT_FALSE(handle.empty());
    auto response = Send("script.inspect", owner.instanceId, handle);
    ASSERT_NE(response.Find("result"), nullptr) << editor::ai::SerializeJson(response);
    const auto& initial = *response.Find("result");
    EXPECT_EQ(initial.Find("status")->AsString(), "disabled");
    EXPECT_EQ(initial.Find("fields")->Find("scalar")->AsNumber(), 1.0);
    EXPECT_EQ(initial.Find("fields")->Find("getterReads")->AsInt(), 0);
    EXPECT_EQ(initial.Find("fields")->Find("updateCount")->AsInt(), 0);
    EXPECT_EQ(initial.Find("fields")->Find("GetterTrap"), nullptr);
    const auto& initialTargets = initial.Find("fields")->Find("targets")->AsArray();
    ASSERT_EQ(initialTargets.size(), 4u);
    EXPECT_EQ(initialTargets[0].Find("status")->AsString(), "resolved");
    EXPECT_TRUE(initialTargets[1].IsNull());
    EXPECT_EQ(initialTargets[2].Find("guid")->AsString(), "missing-object-guid");
    EXPECT_EQ(initialTargets[3].Find("id")->AsString(), targetGuid);
    ASSERT_NE(Diagnostic(initial, "targets", "MISSING_SCRIPT_REFERENCE"), nullptr);
    ASSERT_NE(Diagnostic(initial, "targets", "MISSING_SCRIPT_REFERENCE")->Find("elementIndex"), nullptr);
    EXPECT_EQ(Diagnostic(initial, "targets", "MISSING_SCRIPT_REFERENCE")->Find("elementIndex")->AsInt(), 2);
    ASSERT_NE(Diagnostic(initial, "ManagedFixtures.FaultObservationComponent.m_unsupported", "UNSUPPORTED_SCRIPT_FIELD"), nullptr);
    ASSERT_NE(Schema(initial, "scalar"), nullptr);
    EXPECT_EQ(Schema(initial, "scalar")->Find("type")->AsString(), "float");
    EXPECT_EQ(Schema(initial, "scalar")->Find("default"), nullptr);
    EXPECT_FALSE(Schema(initial, "scalar")->Find("writable")->AsBool());
    const auto& initialFields = *initial.Find("fields");
    const auto& inspectedSettings = *initialFields.Find("settings");
    EXPECT_EQ(inspectedSettings.Find("label")->AsString(), "plain value");
    EXPECT_EQ(inspectedSettings.Find("$version")->AsInt(), 23);
    EXPECT_EQ(inspectedSettings.Find("json"), nullptr);
    EXPECT_EQ(inspectedSettings.Find("value"), nullptr);
    ASSERT_NE(inspectedSettings.Find("target"), nullptr);
    ASSERT_NE(inspectedSettings.Find("target")->Find("guid"), nullptr);
    EXPECT_EQ(inspectedSettings.Find("target")->Find("guid")->AsString(), targetGuid);
    EXPECT_EQ(inspectedSettings.Find("target")->Find("status")->AsString(), "resolved");
    ASSERT_EQ(inspectedSettings.Find("rotations")->AsArray().size(), 1u);
    EXPECT_EQ(inspectedSettings.Find("rotations")->AsArray()[0].AsArray()[3].AsNumber(), 1.0);
    EXPECT_EQ(initialFields.Find("rotation")->AsArray()[3].AsNumber(), 1.0);
    EXPECT_EQ(initialFields.Find("weapon")->AsString(), "guid:missing-weapon-guid");
    EXPECT_TRUE(initialFields.Find("optionalSettings")->IsNull());
    EXPECT_EQ(Diagnostic(initial, "optionalSettings", "INVALID_MANAGED_STRUCTURED_VALUE"), nullptr);
    EXPECT_EQ(Schema(initial, "samples")->Find("type")->AsString(), "list");
    EXPECT_EQ(Schema(initial, "samples")->Find("count")->AsInt(), 2);
    const int constructorCalls = std::max(inspectedSettings.Find("constructorSnapshot")->AsInt(),
        initialFields.Find("stableSettings")->Find("constructorSnapshot")->AsInt());
    const auto repeated = Send("script.inspect", owner.instanceId, handle);
    ASSERT_NE(repeated.Find("result"), nullptr);
    EXPECT_EQ(repeated.Find("result")->Find("fields")->Find("settings")->Find("constructorSnapshot")->AsInt(),
        inspectedSettings.Find("constructorSnapshot")->AsInt());

    adapter->enabled = true;
    EXPECT_FALSE(adapter->ExecuteCallback(&scene::Script::OnUpdate, "Update"));
    ASSERT_TRUE(adapter->IsRuntimeFaulted());
    EXPECT_NE(adapter->FaultDiagnostic().find("Intentional observation failure"), std::string::npos);
    const std::string originalFault = adapter->FaultDiagnostic();
    response = Send("script.inspect", owner.instanceId, handle);
    ASSERT_NE(response.Find("result"), nullptr) << editor::ai::SerializeJson(response);
    const auto& faulted = *response.Find("result");
    EXPECT_EQ(faulted.Find("status")->AsString(), "faulted");
    EXPECT_EQ(faulted.Find("observationStatus")->AsString(), "partial");
    EXPECT_EQ(faulted.Find("fault")->Find("message")->AsString(), originalFault);
    EXPECT_EQ(faulted.Find("fields")->Find("updateCount")->AsInt(), 1);
    EXPECT_EQ(faulted.Find("fields")->Find("getterReads")->AsInt(), 0);
    for (const char* key : {"scalar", "numbers", "vectors", "text", "badText", "labels", "nullTargets", "rotation", "settings"})
        EXPECT_TRUE(faulted.Find("fields")->Find(key)->IsNull()) << key;
    EXPECT_NE(Diagnostic(faulted, "scalar", "NON_FINITE_SCRIPT_VALUE"), nullptr);
    ASSERT_NE(Diagnostic(faulted, "numbers", "NON_FINITE_SCRIPT_VALUE"), nullptr);
    EXPECT_EQ(Diagnostic(faulted, "numbers", "NON_FINITE_SCRIPT_VALUE")->Find("elementIndex")->AsInt(), 1);
    EXPECT_NE(Diagnostic(faulted, "vectors", "NON_FINITE_SCRIPT_VALUE"), nullptr);
    EXPECT_NE(Diagnostic(faulted, "text", "NULL_SCRIPT_SETTING"), nullptr);
    ASSERT_NE(Diagnostic(faulted, "badText", "MANAGED_FIELD_READ_FAILED"), nullptr);
    EXPECT_FALSE(Diagnostic(faulted, "badText", "MANAGED_FIELD_READ_FAILED")->Find("message")->AsString().empty());
    EXPECT_NE(Diagnostic(faulted, "labels", "NULL_SCRIPT_ARRAY_ELEMENT"), nullptr);
    EXPECT_NE(Diagnostic(faulted, "nullTargets", "NULL_SCRIPT_ARRAY"), nullptr);
    EXPECT_EQ(Schema(faulted, "numbers")->Find("type")->AsString(), "list");
    EXPECT_EQ(Schema(faulted, "numbers")->Find("elementType")->AsString(), "float");
    EXPECT_NE(Diagnostic(faulted, "rotation", "NON_FINITE_SCRIPT_VALUE"), nullptr);
    EXPECT_NE(Diagnostic(faulted, "settings", "MANAGED_FIELD_READ_FAILED"), nullptr);
    EXPECT_EQ(faulted.Find("fields")->Find("nestedConstructorCalls")->AsInt(), constructorCalls);
    EXPECT_EQ(faulted.Find("fields")->Find("nestedGetterReads")->AsInt(), 0);
    EXPECT_EQ(faulted.Find("fields")->Find("stableSettings")->Find("label")->AsString(), "before");
    EXPECT_EQ(faulted.Find("fields")->Find("samples")->AsArray()[1].AsInt(), 5);
    EXPECT_EQ(faulted.Find("fields")->Find("weapon")->AsString(), "guid:missing-weapon-guid");

    EXPECT_TRUE(scene.DestroyGameObject(targetId));
    scene.CreateGameObject("Same GUID, new lifetime").instanceId = targetGuid;
    response = Send("script.inspect", owner.instanceId, handle);
    ASSERT_NE(response.Find("result"), nullptr);
    const auto& expired = *response.Find("result");
    EXPECT_EQ(expired.Find("fields")->Find("target")->Find("status")->AsString(), "missing");
    EXPECT_EQ(expired.Find("fields")->Find("target")->Find("guid")->AsString(), targetGuid);
    EXPECT_NE(Diagnostic(expired, "target", "MISSING_SCRIPT_REFERENCE"), nullptr);
    EXPECT_EQ(expired.Find("fault")->Find("message")->AsString(), originalFault);
    EXPECT_EQ(expired.Find("fields")->Find("getterReads")->AsInt(), 0);
    EXPECT_EQ(expired.Find("fields")->Find("updateCount")->AsInt(), 1);

    auto& healthyOwner = scene.CreateGameObject("Healthy observer");
    auto* healthy = Attach(healthyOwner, "cs-internal:ManagedFixtures.LifecycleComponent");
    ASSERT_NE(healthy, nullptr);
    response = Send("script.inspect", healthyOwner.instanceId, Handle(healthyOwner));
    ASSERT_NE(response.Find("result"), nullptr);
    const auto& healthyResult = *response.Find("result");
    EXPECT_EQ(healthyResult.Find("observationStatus")->AsString(), "complete");
    EXPECT_TRUE(healthyResult.Find("fieldDiagnostics")->AsArray().empty());
    EXPECT_EQ(healthyResult.Find("fault"), nullptr);
    EXPECT_EQ(healthyResult.Find("fields")->Find("speed")->AsNumber(), 4.0);
    EXPECT_EQ(healthyResult.Find("fields")->Find("updateCount")->AsInt(), 0);
}

TEST_F(ManagedScriptInspectionTest, LargeArraySummaryAndSnapshotPagesUseTheActualBusWithoutTruncatingStoredValues)
{
    auto& runtime = scene::ManagedScriptRuntime::Instance();
    ASSERT_TRUE(runtime.Initialize(FBZZ_MANAGED_HOST_TEST_DIRECTORY));
    ASSERT_TRUE(runtime.Load(FBZZ_MANAGED_FIXTURE_TEST_ASSEMBLY, true));
    auto& owner = AttachedScene()->CreateGameObject("PagedObserver");
    auto* adapter = Attach(owner, "cs-internal:ManagedFixtures.CompletionComponent");
    ASSERT_NE(adapter, nullptr);
    std::string values = R"({"settings":{"weights":[)";
    for (int index = 0; index < 50000; ++index) {
        if (index) values += ',';
        values += std::to_string(index);
    }
    values += "]}}";
    ASSERT_TRUE(adapter->ApplySettingsJson(values)) << runtime.LastDiagnostic();
    const std::string handle = Handle(owner);
    ASSERT_FALSE(handle.empty());
    const auto summary = Send("script.inspect", owner.instanceId, handle);
    ASSERT_NE(summary.Find("result"), nullptr) << editor::ai::SerializeJson(summary);
    const auto* deferred = summary.Find("result")->Find("fields")->Find("settings")->Find("weights");
    ASSERT_NE(deferred, nullptr);
    EXPECT_EQ(deferred->Find("total")->AsInt64(), 50000);
    EXPECT_EQ(deferred->Find("$deferred")->AsString(), "/settings/weights");
    EXPECT_FALSE(summary.Find("result")->Find("snapshot")->AsBool());
    JsonValue page = JsonValue::MakeObject();
    page.Set("path", JsonValue("/settings/weights")); page.Set("start", JsonValue(45000)); page.Set("count", JsonValue(3));
    const auto first = Send("script.inspect", owner.instanceId, handle, &page);
    ASSERT_NE(first.Find("result"), nullptr) << editor::ai::SerializeJson(first);
    const auto* observation = first.Find("result")->Find("observation");
    ASSERT_NE(observation, nullptr);
    EXPECT_EQ(observation->Find("items")->AsArray().front().AsInt(), 45000);
    EXPECT_EQ(observation->Find("nextStart")->AsInt64(), 45003);
    const std::string token = observation->Find("observationId")->AsString();
    ASSERT_TRUE(adapter->ApplySettingsJson(R"({"settings":{"weights":[-1]}})"));
    page.Set("observationId", JsonValue(token)); page.Set("start", JsonValue(45003));
    const auto next = Send("script.inspect", owner.instanceId, handle, &page);
    ASSERT_NE(next.Find("result"), nullptr) << editor::ai::SerializeJson(next);
    EXPECT_EQ(next.Find("result")->Find("observation")->Find("items")->AsArray().front().AsInt(), 45003);
    JsonValue close = JsonValue::MakeObject(); close.Set("observationId", JsonValue(token)); close.Set("close", JsonValue(true));
    const auto closed = Send("script.inspect", owner.instanceId, handle, &close);
    ASSERT_NE(closed.Find("result"), nullptr);
    EXPECT_TRUE(closed.Find("result")->Find("closed")->AsBool());
    EXPECT_EQ(Send("script.inspect", owner.instanceId, handle, &page).Find("result"), nullptr);
}

TEST_F(ManagedScriptInspectionTest, PublicSavedScriptIdsReacquireDuplicateTypesAfterReloadAndSceneRestore)
{
    auto& runtime = scene::ManagedScriptRuntime::Instance();
    ASSERT_TRUE(runtime.Initialize(FBZZ_MANAGED_HOST_TEST_DIRECTORY));
    ASSERT_TRUE(runtime.Load(FBZZ_MANAGED_FIXTURE_TEST_ASSEMBLY, true));
    auto& owner = AttachedScene()->CreateGameObject("DuplicatePublicIdentity");
    const std::string ownerGuid = owner.instanceId;
    auto& scripts = owner.AddComponent<scene::ScriptComponent>();
    for (const int health : {3, 7}) {
        auto& entry = scripts.scripts.emplace_back();
        entry.script = scene::ScriptFactory::Create("cs-internal:ManagedFixtures.HealthComponent", entry.savedScriptId);
        ASSERT_NE(entry.script, nullptr);
        entry.script->SetContext(AttachedScene(), &owner);
        auto* adapter = runtime.FindAdapter(entry.script.get()); ASSERT_NE(adapter, nullptr);
        ASSERT_TRUE(adapter->ApplySettingsJson("{\"health\":" + std::to_string(health) + "}"));
    }
    const auto initial = Send("node.components", ownerGuid);
    ASSERT_NE(initial.Find("result"), nullptr);
    const auto& rows = initial.Find("result")->Find("scripts")->AsArray();
    ASSERT_EQ(rows.size(), 2u);
    ASSERT_NE(rows[0].Find("savedScriptId"), nullptr); ASSERT_NE(rows[1].Find("savedScriptId"), nullptr);
    const std::string first = rows[0].Find("savedScriptId")->AsString(), second = rows[1].Find("savedScriptId")->AsString();
    const std::string originalHandle = rows[0].Find("scriptId")->AsString();
    EXPECT_FALSE(first.empty()); EXPECT_NE(first, second);
    EXPECT_NE(rows[0].Find("scriptId")->AsString(), rows[1].Find("scriptId")->AsString());
    const auto verify = [&]() {
        const auto listed = Send("node.components", ownerGuid);
        EXPECT_NE(listed.Find("result"), nullptr);
        if (!listed.Find("result")) return;
        const auto& current = listed.Find("result")->Find("scripts")->AsArray();
        EXPECT_EQ(current.size(), 2u);
        for (const auto& row : current) {
            const auto* persistent = row.Find("savedScriptId");
            EXPECT_NE(persistent, nullptr); if (!persistent) continue;
            const std::string saved = persistent->AsString();
            EXPECT_TRUE(saved == first || saved == second);
            const auto inspected = Send("script.inspect", ownerGuid, row.Find("scriptId")->AsString());
            EXPECT_NE(inspected.Find("result"), nullptr); if (!inspected.Find("result")) continue;
            EXPECT_EQ(inspected.Find("result")->Find("savedScriptId")->AsString(), saved);
            EXPECT_EQ(inspected.Find("result")->Find("fields")->Find("health")->AsInt(), saved == first ? 3 : 7);
        }
    };
    /// @note Editor fixture の公開 assembly を新 ALC に再読込し、保存 ID と個体 handle の境界を検証する。
    ASSERT_TRUE(runtime.Reload(*AttachedScene(), FBZZ_MANAGED_FIXTURE_TEST_ASSEMBLY, true));
    verify();
    const std::string text = scene::SceneSerializer::SaveToText(*AttachedScene()); ASSERT_FALSE(text.empty());
    auto restored = scene::SceneSerializer::LoadDataFromText(text, "duplicate-identities.fbzz"); ASSERT_NE(restored, nullptr);
    *AttachedScene() = std::move(*restored);
    verify();
    const auto current = Send("node.components", ownerGuid);
    ASSERT_NE(current.Find("result"), nullptr);
    EXPECT_NE(current.Find("result")->Find("scripts")->AsArray().front().Find("scriptId")->AsString(), originalHandle);
}

} /// @note namespace fbzz::tests
