/// @file    ScriptHandlers.cpp
/// @brief   Script の実行個体・反射スキーマ・観測値を読み取る Query。
/// @author  Hasegawa Jin
/// @date    2026-09-26
#include "BusInternal.hpp"
#include "ManagedScriptInspection.hpp"
#include <Editor/Ai/JsonReflector.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <Editor/PlayModeController.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/ScriptComponent.hpp>
#include <Engine/Scene/ScriptFactory.hpp>
#include <Engine/Scene/Managed/ManagedScriptRuntime.hpp>
#include <Engine/Scene/Managed/ManagedScriptAdapter.hpp>
#include <algorithm>
#include <cmath>
#include <set>

namespace fbzz::editor::ai::bus {
namespace {

std::string ScriptHandle(const scene::ScriptEntry& entry)
{
    /// @note Entry と実体の両方を含め、同じ Entry への DLL 再生成も検出する。
    return entry.inspectionId + "/" + (entry.script ? entry.script->InspectionId() : "missing");
}

JsonValue ScriptSummary(const scene::ScriptEntry& entry, std::size_t index, bool active)
{
    const auto* script = entry.script.get();
    const bool enabled = script ? script->enabled : entry.serialized && entry.serialized->enabled;
    const char* status = !script ? "missing" : script->IsRuntimeFaulted() ? "faulted"
        : !enabled || !active ? "disabled" : !entry.m_started ? "notStarted" : "ready";
    JsonValue value = JsonValue::MakeObject();
    value.Set("scriptId", JsonValue(ScriptHandle(entry)));
    value.Set("savedScriptId", JsonValue(entry.savedScriptId));
    value.Set("index", JsonValue(static_cast<std::int64_t>(index)));
    value.Set("type", JsonValue(script ? script->GetTypeName()
        : entry.serialized ? entry.serialized->type : ""));
    const std::string typeKey = script ? script->GetTypeName() : entry.serialized ? entry.serialized->type : "";
    value.Set("displayName", JsonValue(scene::ScriptFactory::DisplayName(typeKey)));
    value.Set("status", JsonValue(status));
    value.Set("enabled", JsonValue(enabled));
    value.Set("activeInHierarchy", JsonValue(active));
    value.Set("awoken", JsonValue(entry.m_awoken));
    value.Set("started", JsonValue(entry.m_started));
    return value;
}

bool HasNonFiniteNumber(const JsonValue& value)
{
    if (value.IsNumber()) return !std::isfinite(value.AsNumber());
    for (const auto& child : value.AsArray()) if (HasNonFiniteNumber(child)) return true;
    for (const auto& child : value.AsObject()) if (HasNonFiniteNumber(child.second)) return true;
    return false;
}

bool HasDuplicateFields(const JsonValue& fields)
{
    std::set<std::string> names;
    for (const auto& field : fields.AsArray()) {
        const auto* name = field.Find("name");
        if (name && !names.insert(name->AsString()).second) return true;
        if (const auto* children = field.Find("fields"); children && HasDuplicateFields(*children)) return true;
        if (const auto* children = field.Find("elementFields"); children && HasDuplicateFields(*children)) return true;
    }
    return false;
}

bool IsChangingScripts(const editor::EditorContext& ctx)
{
    return ctx.managedBuildBusy || (ctx.playMode && ctx.playMode->HasPendingRestore());
}

JsonValue ComponentNames(std::span<const std::string> names)
{
    JsonValue value = JsonValue::MakeArray();
    for (const auto& name : names) value.Push(JsonValue(name));
    return value;
}

JsonValue ResolveReference(scene::Scene& activeScene, scene::EntityID entity, const char* typeName)
{
    auto* target = activeScene.GetGameObject(entity);
    const std::string expectedType = typeName ? typeName : "";
    const char* status = target ? "resolved" : entity.IsValid() ? "missing" : "none";
    if (target && !expectedType.empty() && expectedType != "GameObject") {
        const auto component = InspectComponentType(*target, expectedType);
        if (component.known) status = component.present ? "resolved" : "typeMismatch";
        else {
            int matches = 0;
            if (const auto* scripts = target->GetComponent<scene::ScriptComponent>()) {
                for (const auto& entry : scripts->scripts)
                    if (entry.script && entry.script->FbzzAsType(expectedType)) ++matches;
            }
            status = matches == 1 ? "resolved" : matches > 1 ? "ambiguous" : "typeMismatch";
        }
    }
    JsonValue reference = JsonValue::MakeObject();
    reference.Set("id", target ? JsonValue(target->instanceId) : JsonValue{});
    reference.Set("expectedType", JsonValue(expectedType));
    reference.Set("status", JsonValue(status));
    return reference;
}

JsonValue InspectionSchema(const JsonValue& source)
{
    JsonValue fields = source;
    for (auto& field : fields.AsArray()) {
        /// @note readOnly は宣言の契約。writable はこの取得 API から変更できるかを表す。
        field.Set("writable", JsonValue(false));
        for (auto& member : field.AsObject())
            if (member.first == "fields" || member.first == "elementFields")
                member.second = InspectionSchema(member.second);
    }
    return fields;
}

JsonValue ManagedInspectionSchema(const scene::Script& script, const JsonValue& reflected)
{
    JsonValue fields = InspectionSchema(reflected);
    const auto* adapter = scene::ManagedScriptRuntime::Instance().FindAdapter(&script);
    if (!adapter) return fields;
    for (JsonValue& field : fields.AsArray()) {
        const JsonValue* key = field.Find("key");
        if (!key) key = field.Find("name");
        if (!key || !key->IsString()) continue;
        for (const scene::ManagedFieldDescriptor& descriptor : adapter->Fields()) {
            if (descriptor.name != key->AsString()) continue;
            if (const auto metadata = ParseJson(descriptor.schemaJson); metadata && metadata->IsObject())
                field.Set("managedMetadata", *metadata);
            break;
        }
    }
    return fields;
}

void AddManagedSource(JsonValue& result, std::string_view typeKey)
{
    const std::string sources = scene::ManagedScriptRuntime::Instance().GetTypeSourcesJson(typeKey);
    if (sources.empty()) return;
    if (const auto metadata = ParseJson(sources)) {
        if (metadata->IsArray() && metadata->AsArray().size() == 1) {
            if (const JsonValue* declarations = metadata->AsArray().front().Find("sources")) {
                result.Set("sources", *declarations);
                return;
            }
        }
        result.Set("sources", *metadata);
    }
}

JsonValue ResolveObjectReference(scene::Scene& activeScene, const scene::ScriptObjectReference& reference)
{
    if (reference.guid.empty()) return JsonValue{};
    const auto* target = activeScene.GetGameObject(reference.id);
    /// @note 保存 GUID が同じ再生成先へ、失効した runtime 参照の表示を再接続しない。
    if (target && target->instanceId != reference.guid) target = nullptr;
    JsonValue value = JsonValue::MakeObject();
    value.Set("guid", JsonValue(reference.guid));
    value.Set("id", target ? JsonValue(target->instanceId) : JsonValue{});
    value.Set("name", target ? JsonValue(target->name) : JsonValue{});
    value.Set("expectedType", JsonValue("GameObject"));
    value.Set("status", JsonValue(target ? "resolved" : "missing"));
    return value;
}

Outcome DoScriptInspect(BusCall& call)
{
    if (IsChangingScripts(call.ctx)) return Outcome::Err("SCRIPT_BUSY", "Script の再読込・Scene 復元中です");
    auto* activeScene = call.ctx.activeScene;
    if (!activeScene) return Outcome::Err("NO_SCENE", "アクティブシーンがありません");
    const std::string id = StringField(call.payload, "id");
    const std::string handle = StringField(call.payload, "scriptId");
    if (id.empty() || handle.empty()) return Outcome::Err("INVALID_ARGUMENT", "id と scriptId が必要です");
    auto* go = activeScene->FindByGuid(id);
    if (!go) return Outcome::Err("NODE_NOT_FOUND", "NodeId が見つかりません: " + id);
    auto* scripts = go->GetComponent<scene::ScriptComponent>();
    if (scripts) for (std::size_t i = 0; i < scripts->scripts.size(); ++i) {
        auto& entry = scripts->scripts[i];
        if (ScriptHandle(entry) != handle) continue;
        JsonValue result = ScriptSummary(entry, i, go->activeInHierarchy());
        AddManagedSource(result, entry.script ? entry.script->GetTypeName() : entry.serialized ? entry.serialized->type : "");
        result.Set("id", JsonValue(id));
        result.Set("frameIndex", JsonValue(static_cast<std::int64_t>(Time::frameCount)));
        result.Set("managedGeneration", JsonValue(static_cast<std::int64_t>(scene::ManagedScriptRuntime::Instance().GetAppliedGeneration())));
        /// @note DrainRequests の処理時点。フレーム終了後の値であるとは保証しない。
        result.Set("samplePhase", JsonValue("request"));
        if (!entry.script) {
            if (entry.serialized) {
                JsonValue saved = JsonValue::MakeObject();
                saved.Set("format", JsonValue("toml"));
                saved.Set("data", JsonValue(entry.serialized->fieldsToml));
                result.Set("savedValues", std::move(saved));
            }
            return Outcome::Ok(std::move(result));
        }
        auto& script = *entry.script;
        if (auto* adapter = scene::ManagedScriptRuntime::Instance().FindAdapter(&script)) {
            if (call.payload.Find("path") || call.payload.Find("start") || call.payload.Find("count")
                || call.payload.Find("observationId") || call.payload.Find("close") || call.payload.Find("wireBytes")) {
                std::string diagnostic;
                if (!InspectManagedPage(*adapter, call.payload, result, diagnostic)) return Outcome::Err("SCRIPT_OBSERVATION_FAILED", diagnostic);
                return Outcome::Ok(std::move(result));
            }
            InspectManagedFields(*adapter, *activeScene, result);
            return Outcome::Ok(std::move(result));
        }
        if (script.IsRuntimeFaulted()) return Outcome::Err("SCRIPT_FAULTED", "停止した Script の getter は実行しません");
        JsonCatalogReflector schema(false);
        JsonReadReflector values(true, [activeScene](scene::EntityID entity, const char* typeName) {
            return ResolveReference(*activeScene, entity, typeName);
        }, [activeScene](const scene::ScriptObjectReference& reference) {
            return ResolveObjectReference(*activeScene, reference);
        });
        if (!script.ExecuteCallback([&] { script.Reflect(schema); script.Reflect(values); }, "script.inspect"))
            return Outcome::Err("SCRIPT_FAULTED", "Script の観測に失敗しました。Console を確認してください");
        if (!values.Error().empty() || HasDuplicateFields(schema.Result()))
            return Outcome::Err("DUPLICATE_SCRIPT_FIELD", "Script の公開キーが重複しています");
        if (HasNonFiniteNumber(values.Result()))
            return Outcome::Err("NON_FINITE_SCRIPT_VALUE", "Script に NaN または Infinity が含まれています");
        result.Set("fields", values.Result());
        result.Set("schema", ManagedInspectionSchema(script, schema.Result()));
        return Outcome::Ok(std::move(result));
    }
    return Outcome::Err("STALE_SCRIPT_ID", "Script が削除・再生成されました。node_get_components を再取得してください");
}

Outcome DoScriptCatalog(BusCall& call)
{
    if (IsChangingScripts(call.ctx)) return Outcome::Err("SCRIPT_BUSY", "Script の再読込・Scene 復元中です");
    const std::string requested = StringField(call.payload, "type");
    std::string requestedKey;
    if (!requested.empty() && !scene::ScriptFactory::ResolveTypeName(requested, requestedKey))
        return Outcome::Err("SCRIPT_TYPE_NOT_FOUND_OR_AMBIGUOUS", "Script 型名が未登録または曖昧です: " + requested);
    auto names = scene::ScriptFactory::RegisteredTypeNames();
    std::sort(names.begin(), names.end());
    JsonValue types = JsonValue::MakeArray();
    for (const auto& name : names) {
        if (!requested.empty() && name != requestedKey) continue;
        const std::string passiveSchema = scene::ManagedScriptRuntime::Instance().GetTypeSchemaJson(name);
        if (!passiveSchema.empty()) {
            auto type = ParseJson(passiveSchema);
            if (!type || !type->IsObject()) return Outcome::Err("SCRIPT_SCHEMA_FAILED", "C# の型 schema が不正です: " + name);
            AddManagedSource(*type, name);
            types.Push(std::move(*type));
            continue;
        }
        return Outcome::Err("SCRIPT_SCHEMA_FAILED", "C# の受動的な型 schema が見つかりません: " + name);
    }
    if (!requested.empty() && types.AsArray().empty())
        return Outcome::Err("SCRIPT_TYPE_NOT_FOUND", "Script 型が登録されていません: " + requested);
    JsonValue result = JsonValue::MakeObject();
    result.Set("types", std::move(types));
    result.Set("managedGeneration", JsonValue(static_cast<std::int64_t>(scene::ManagedScriptRuntime::Instance().GetAppliedGeneration())));
    return Outcome::Ok(std::move(result));
}

Outcome DoScriptApiCatalog(BusCall& call)
{
    std::string text;
    OpResult catalog = InvokeOperator(call.ctx, "script.api.catalog");
    if (!catalog.ok) return Outcome::Err(catalog.errorCode, catalog.message);
    const OpData* json = catalog.data.Find("catalogJson");
    if (!json || !json->IsString()) return Outcome::Err("SDK_API_INVALID", "SDK catalog Op が JSON を返しませんでした");
    text = json->AsString();
    auto parsed = ParseJson(text);
    if (!parsed || !parsed->IsObject()) return Outcome::Err("SDK_API_INVALID", "ScriptApi catalog の JSON が不正です");
    const std::string requested = StringField(call.payload, "type");
    const std::string memberName = StringField(call.payload, "member");
    const JsonValue* types = parsed->Find("types");
    if (!types || !types->IsArray()) return Outcome::Err("SDK_API_INVALID", "ScriptApi catalog に types がありません");
    JsonValue selected = JsonValue::MakeArray();
    for (JsonValue type : types->AsArray()) {
        const JsonValue* name = type.Find("name");
        const JsonValue* display = type.Find("displayName");
        if (!requested.empty() && (!name || name->AsString() != requested) && (!display || display->AsString() != requested)) continue;
        if (!memberName.empty()) {
            JsonValue members = JsonValue::MakeArray();
            if (const JsonValue* all = type.Find("members")) for (const JsonValue& item : all->AsArray())
                if (const JsonValue* key = item.Find("name"); key && key->AsString() == memberName) members.Push(item);
            if (members.AsArray().empty()) continue;
            type.Set("members", std::move(members));
        }
        selected.Push(std::move(type));
    }
    if ((!requested.empty() || !memberName.empty()) && selected.AsArray().empty())
        return Outcome::Err("SDK_API_NOT_FOUND", "公開済み SDK に指定された型・member がありません");
    if (!requested.empty() && selected.AsArray().size() > 1)
        return Outcome::Err("SDK_API_AMBIGUOUS", "SDK 型の完全名を指定してください");
    parsed->Set("types", std::move(selected));
    return Outcome::Ok(std::move(*parsed));
}

} /// @note namespace

JsonValue InspectScriptList(GameObject& go)
{
    JsonValue result = JsonValue::MakeArray();
    if (const auto* scripts = go.GetComponent<scene::ScriptComponent>()) {
        for (std::size_t i = 0; i < scripts->scripts.size(); ++i)
            result.Push(ScriptSummary(scripts->scripts[i], i, go.activeInHierarchy()));
    }
    return result;
}

void RegisterScriptHandlers(BusHandlerTable& table)
{
    table.AddQuery("script.inspect", DoScriptInspect);
    table.AddQuery("script.catalog", DoScriptCatalog);
    table.AddQuery("script.api.catalog", DoScriptApiCatalog);
}

} /// @note namespace fbzz::editor::ai::bus
