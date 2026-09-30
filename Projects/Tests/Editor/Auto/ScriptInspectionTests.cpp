/// @file    ScriptInspectionTests.cpp
/// @brief   Script 観測の非保存契約と生存期間ごとの識別を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-26
#include <TestKit/TestKit.hpp>
#include <TestKit/Editor/EditorFixture.hpp>
#include <Editor/Ai/EditorBusDispatcher.hpp>
#include <Editor/Ai/EditorBusProtocol.hpp>
#include <Editor/Ai/JsonReflector.hpp>
#include <Editor/Util/ScriptSnapshot.hpp>
#include <Engine/Scene/ScriptComponent.hpp>
#include <Engine/Scene/ScriptFactory.hpp>
#include <Engine/Scene/TomlReflector.hpp>
#include <limits>
#include "../../Fixtures/UserScripts/HealthComponent.hpp"

namespace fbzz::tests {
namespace {
using editor::ai::JsonValue;

class InspectionProbe : public scene::Script {
    FBZZ_SCRIPT(InspectionProbe)
    FBZZ_FIELD(int, speed, 5, "Speed")
    FBZZ_FIELD_READ_ONLY(int, storedReadOnly, 9, "Stored read only")
    FBZZ_OBSERVE(int, currentHealth, Health(), "Current HP")
    FBZZ_OBSERVE(bool, alive, health > 0, "Alive")
    FBZZ_OBSERVE(float, fraction, fractionValue, "Fraction")
    FBZZ_OBSERVE(std::string, state, std::string("idle"), "State")
    FBZZ_OBSERVE(math::Vector2, direction, math::Vector2{}, "Direction")
    FBZZ_OBSERVE(math::Vector3, position, math::Vector3{}, "Position")
    FBZZ_OBSERVE(math::Vector4, tint, math::Vector4{}, "Tint")
    FBZZ_OBSERVE(math::Quaternion, orientation, math::Quaternion{}, "Orientation")
    FBZZ_FIELD(scene::EntityID, target, scene::EntityID{}, "Target")
    FBZZ_REF(InspectionProbe, typedTarget, "Typed target")
    inline static int reads = 0;
    int health = 73;
    float fractionValue = 0.5f;
    int Health() const { ++reads; return health; }
};
FBZZ_REFLECT(InspectionProbe)

class DuplicateProbe : public scene::Script {
public:
    void Reflect(scene::IReflector& r) override
    {
        int value = 1;
        r.BeginField("same", "First"); r.Field("First", value);
        r.BeginField("same", "Second"); r.Field("Second", value);
    }
};

class NestedProbe : public scene::Script {
public:
    InspectionProbe child;
    void Reflect(scene::IReflector& r) override
    {
        r.BeginField("health", "Health module");
        r.BeginObject("Health module");
        child.Reflect(r);
        r.EndObject();
        r.EndField();
    }
};
} /// namespace

class ScriptInspectionTest : public testkit::EditorFixture {
protected:
    JsonValue Send(std::string type, std::string id = {}, std::string scriptId = {})
    {
        JsonValue payload = JsonValue::MakeObject();
        payload.Set("t", JsonValue(type));
        if (!id.empty()) payload.Set("id", JsonValue(id));
        if (!scriptId.empty()) payload.Set("scriptId", JsonValue(scriptId));
        if (type == "script.catalog") payload.Set("type", JsonValue("InspectionProbe"));
        JsonValue request = JsonValue::MakeObject();
        request.Set("protocol", JsonValue(editor::ai::kEditorProtocol));
        request.Set("id", JsonValue("inspect-test"));
        request.Set("kind", JsonValue("query"));
        request.Set("payload", std::move(payload));
        editor::ai::EditorBusDispatcher dispatcher(Context());
        const auto response = editor::ai::ParseJson(dispatcher.Handle(editor::ai::SerializeJson(request)));
        EXPECT_TRUE(response.has_value());
        return response.value_or(JsonValue{});
    }
    std::string Handle(scene::GameObject& go, std::size_t index = 0)
    {
        const auto reply = Send("node.components", go.instanceId);
        const auto* result = reply.Find("result");
        if (!result || !result->Find("scripts") || result->Find("scripts")->AsArray().size() <= index) {
            ADD_FAILURE() << editor::ai::SerializeJson(reply);
            return {};
        }
        return result->Find("scripts")->AsArray()[index].Find("scriptId")->AsString();
    }
    static std::string Error(const JsonValue& response)
    {
        const auto* error = response.Find("error");
        return error && error->Find("code") ? error->Find("code")->AsString() : "";
    }
};

TEST_F(ScriptInspectionTest, CatalogAndPersistenceNeverEvaluateObservation)
{
    InspectionProbe script;
    InspectionProbe::reads = 0;
    editor::ai::JsonCatalogReflector schema;
    script.Reflect(schema);
    editor::ai::JsonReadReflector storedValues;
    script.Reflect(storedValues);
    toml::table table;
    util::TomlWriteReflector writer(table);
    script.Reflect(writer);
    const std::string snapshot = editor::CaptureScriptSnapshot(script);
    EXPECT_EQ(snapshot.find("currentHealth"), std::string::npos);
    EXPECT_FALSE(table.contains("currentHealth"));
    EXPECT_TRUE(table.contains("storedReadOnly"));
    EXPECT_EQ(storedValues.Result().Find("currentHealth"), nullptr);
    EXPECT_TRUE(editor::ApplyScriptSnapshot(script, "speed = 8\ncurrentHealth = 0\n"));
    EXPECT_EQ(script.speed, 8);
    EXPECT_EQ(script.health, 73);
    EXPECT_EQ(InspectionProbe::reads, 0);
    const auto& observed = schema.Result().AsArray()[2];
    EXPECT_EQ(observed.Find("name")->AsString(), "currentHealth");
    EXPECT_TRUE(observed.Find("readOnly")->AsBool());
    EXPECT_FALSE(observed.Find("stored")->AsBool());
    EXPECT_EQ(observed.Find("default"), nullptr);
}

TEST_F(ScriptInspectionTest, ReadsDisabledInstanceUsingStableKeysAndResolvedReferences)
{
    auto& scene = AttachScene();
    auto& go = scene.CreateGameObject("Probe");
    auto& target = scene.CreateGameObject("Target");
    auto& script = go.AddScript<InspectionProbe>();
    script.enabled = false;
    script.target = target.GetID();
    InspectionProbe::reads = 0;
    const auto handle = Handle(go);
    EXPECT_EQ(InspectionProbe::reads, 0);
    const auto response = Send("script.inspect", go.instanceId, handle);
    ASSERT_NE(response.Find("result"), nullptr) << editor::ai::SerializeJson(response);
    const auto& result = *response.Find("result");
    EXPECT_EQ(result.Find("status")->AsString(), "disabled");
    EXPECT_EQ(result.Find("samplePhase")->AsString(), "request");
    const auto& fields = *result.Find("fields");
    EXPECT_EQ(fields.Find("currentHealth")->AsInt(), 73);
    EXPECT_EQ(fields.Find("Current HP"), nullptr);
    EXPECT_EQ(fields.Find("target")->Find("id")->AsString(), target.instanceId);
    EXPECT_EQ(fields.AsObject().size(), result.Find("schema")->AsArray().size());
    EXPECT_EQ(InspectionProbe::reads, 1);
    for (const auto& field : result.Find("schema")->AsArray()) EXPECT_EQ(field.Find("default"), nullptr);
}

TEST_F(ScriptInspectionTest, HandlesSurviveReorderButRejectRecreatedInstances)
{
    auto& go = AttachScene().CreateGameObject("Probe");
    go.AddScript<InspectionProbe>().health = 12;
    go.AddScript<InspectionProbe>().health = 34;
    const auto first = Handle(go);
    const auto second = Handle(go, 1);
    auto& entries = go.GetComponent<scene::ScriptComponent>()->scripts;
    std::swap(entries[0], entries[1]);
    EXPECT_EQ(Handle(go, 1), first);
    EXPECT_EQ(Handle(go), second);
    EXPECT_EQ(Send("script.inspect", go.instanceId, first).Find("result")->Find("fields")->Find("currentHealth")->AsInt(), 12);
    entries[1].script = std::make_unique<InspectionProbe>();
    EXPECT_EQ(Error(Send("script.inspect", go.instanceId, first)), "STALE_SCRIPT_ID");
    EXPECT_NE(Handle(go, 1), first);
    const auto remaining = Handle(go);
    entries.clear();
    go.AddScript<InspectionProbe>();
    EXPECT_EQ(Error(Send("script.inspect", go.instanceId, remaining)), "STALE_SCRIPT_ID");
}

TEST_F(ScriptInspectionTest, MissingScriptReturnsSavedDataWithoutLiveFields)
{
    auto& go = AttachScene().CreateGameObject("Missing");
    auto& entry = go.AddComponent<scene::ScriptComponent>().scripts.emplace_back();
    entry.serialized = std::make_unique<scene::SerializedScriptData>();
    entry.serialized->type = "UnavailableScript";
    entry.serialized->fieldsToml = "speed = 4";
    const auto response = Send("script.inspect", go.instanceId, Handle(go));
    ASSERT_NE(response.Find("result"), nullptr);
    const auto& result = *response.Find("result");
    EXPECT_EQ(result.Find("status")->AsString(), "missing");
    EXPECT_EQ(result.Find("fields"), nullptr);
    EXPECT_EQ(result.Find("savedValues")->Find("data")->AsString(), "speed = 4");
}

TEST_F(ScriptInspectionTest, RejectsBusyMissingSceneNonFiniteAndDuplicateKeys)
{
    EXPECT_EQ(Error(Send("script.inspect", "node", "script")), "NO_SCENE");
    Context().scriptReloadBusy = true;
    EXPECT_EQ(Error(Send("script.inspect", "node", "script")), "SCRIPT_BUSY");
    EXPECT_EQ(Error(Send("script.catalog")), "SCRIPT_BUSY");
    Context().scriptReloadBusy = false;
    auto& go = AttachScene().CreateGameObject("Probe");
    go.AddScript<InspectionProbe>().fractionValue = std::numeric_limits<float>::infinity();
    EXPECT_EQ(Error(Send("script.inspect", go.instanceId, Handle(go))), "NON_FINITE_SCRIPT_VALUE");
    go.AddScript<DuplicateProbe>();
    EXPECT_EQ(Error(Send("script.inspect", go.instanceId, Handle(go, 1))), "DUPLICATE_SCRIPT_FIELD");
}

TEST_F(ScriptInspectionTest, CatalogUsesRegisteredSchemaWithoutGetterEvaluation)
{
    scene::ScriptFactory::Register<InspectionProbe>();
    InspectionProbe::reads = 0;
    const auto response = Send("script.catalog");
    ASSERT_NE(response.Find("result"), nullptr) << editor::ai::SerializeJson(response);
    EXPECT_EQ(response.Find("result")->Find("types")->AsArray().size(), 1u);
    EXPECT_EQ(InspectionProbe::reads, 0);
}

TEST_F(ScriptInspectionTest, CopyingIdentityDoesNotCopyAnInstanceHandle)
{
    scene::ScriptInspectionIdentity first;
    scene::ScriptInspectionIdentity second(first);
    EXPECT_NE(first.value, second.value);
    const auto before = second.value;
    second = first;
    EXPECT_EQ(second.value, before);
}

TEST_F(ScriptInspectionTest, NestedSchemaKeepsParentMetadataAndDoesNotEvaluateGetters)
{
    NestedProbe script;
    InspectionProbe::reads = 0;
    editor::ai::JsonCatalogReflector schema(false);
    script.Reflect(schema);
    ASSERT_EQ(schema.Result().AsArray().size(), 1u);
    const auto& parent = schema.Result().AsArray()[0];
    EXPECT_EQ(parent.Find("name")->AsString(), "health");
    EXPECT_EQ(parent.Find("displayName")->AsString(), "Health module");
    EXPECT_EQ(parent.Find("fields")->AsArray()[2].Find("name")->AsString(), "currentHealth");
    EXPECT_EQ(InspectionProbe::reads, 0);
    editor::ai::JsonReadReflector values(true);
    script.Reflect(values);
    EXPECT_EQ(values.Result().Find("health")->Find("currentHealth")->AsInt(), 73);
    EXPECT_EQ(InspectionProbe::reads, 1);
}

TEST_F(ScriptInspectionTest, JsonEditsRejectStoredReadOnlyAndNeverEvaluateObservation)
{
    InspectionProbe script;
    InspectionProbe::reads = 0;
    JsonValue value(0);
    editor::ai::JsonWriteReflector readonly("storedReadOnly", value);
    script.Reflect(readonly);
    EXPECT_FALSE(readonly.Applied());
    EXPECT_FALSE(readonly.Error().empty());
    EXPECT_EQ(script.storedReadOnly, 9);
    editor::ai::JsonWriteReflector observation("currentHealth", value);
    script.Reflect(observation);
    EXPECT_FALSE(observation.Applied());
    EXPECT_EQ(script.health, 73);
    EXPECT_EQ(InspectionProbe::reads, 0);
}

TEST_F(ScriptInspectionTest, ReferencesDistinguishMissingTypeMismatchAndAmbiguity)
{
    auto& activeScene = AttachScene();
    auto& go = activeScene.CreateGameObject("Probe");
    auto& target = activeScene.CreateGameObject("Target");
    auto& script = go.AddScript<InspectionProbe>();
    const auto handle = Handle(go);
    const auto status = [&]() {
        const auto response = Send("script.inspect", go.instanceId, handle);
        return response.Find("result")->Find("fields")->Find("typedTarget")->Find("status")->AsString();
    };
    EXPECT_EQ(status(), "none");
    script.typedTarget.ref.id = scene::EntityID{scene::EntityID::INVALID_INDEX - 1, 1};
    EXPECT_EQ(status(), "missing");
    script.typedTarget.ref.id = target.GetID();
    EXPECT_EQ(status(), "typeMismatch");
    target.AddScript<InspectionProbe>();
    EXPECT_EQ(status(), "resolved");
    target.AddScript<InspectionProbe>();
    EXPECT_EQ(status(), "ambiguous");
}

TEST_F(ScriptInspectionTest, HealthSampleExposesActualDamageWithoutDebugCopies)
{
    sandbox::HealthComponent script;
    EXPECT_FALSE(script.TakeDamage(30));
    script.OnStart();
    EXPECT_FALSE(script.TakeDamage(-1));
    EXPECT_TRUE(script.TakeDamage(30));
    editor::ai::JsonReadReflector values(true);
    script.Reflect(values);
    EXPECT_EQ(values.Result().Find("currentHealth")->AsInt(), 70);
    EXPECT_TRUE(values.Result().Find("alive")->AsBool());
    EXPECT_TRUE(script.TakeDamage(100));
    EXPECT_FALSE(script.TakeDamage(1));
    EXPECT_EQ(script.CurrentHealth(), 0);
}

} /// namespace fbzz::tests
