/// @file    ManagedScriptInspection.cpp
/// @brief   停止した managed Script もフィールド単位で診断しながら観測する。
/// @author  Hasegawa Jin
/// @date    2026-10-04
#include "ManagedScriptInspection.hpp"
#include <Editor/Ai/Json.hpp>
#include <Editor/Util/ManagedSettingsCodec.hpp>
#include <Engine/Scene/Managed/ManagedScriptAdapter.hpp>
#include <Engine/Scene/Managed/ManagedScriptRuntime.hpp>
#include <Engine/Scene/Scene.hpp>
#include <bit>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <string_view>

namespace fbzz::editor::ai::bus {
namespace {

static_assert(std::endian::native == std::endian::little);

struct FieldIssue {
    const char* code = "INVALID_MANAGED_FIELD_VALUE";
    std::string message = "Managed field payload is invalid.";
    int64_t elementIndex = -1;
};

void AddIssue(JsonValue& diagnostics, const std::string& key, const FieldIssue& issue)
{
    JsonValue diagnostic = JsonValue::MakeObject();
    diagnostic.Set("key", JsonValue(key));
    diagnostic.Set("code", JsonValue(issue.code));
    diagnostic.Set("message", JsonValue(issue.message));
    if (issue.elementIndex >= 0) diagnostic.Set("elementIndex", JsonValue(issue.elementIndex));
    diagnostics.Push(std::move(diagnostic));
}

template<typename T>
bool ReadWire(std::string_view bytes, size_t& offset, T& value)
{
    if (offset > bytes.size() || sizeof(T) > bytes.size() - offset) return false;
    std::memcpy(&value, bytes.data() + offset, sizeof(T));
    offset += sizeof(T);
    return true;
}

bool IsArray(scene::ManagedValueKind kind)
{
    return kind >= scene::ManagedValueKind::FLOAT_ARRAY && kind <= scene::ManagedValueKind::GAME_OBJECT_ARRAY;
}

const char* KindName(scene::ManagedValueKind kind)
{
    switch (kind) {
    case scene::ManagedValueKind::FLOAT: return "float";
    case scene::ManagedValueKind::INT: return "int";
    case scene::ManagedValueKind::BOOL: return "bool";
    case scene::ManagedValueKind::STRING: return "string";
    case scene::ManagedValueKind::VECTOR3: return "vector3";
    case scene::ManagedValueKind::GAME_OBJECT: return "gameObject";
    case scene::ManagedValueKind::QUATERNION: return "quaternion";
    case scene::ManagedValueKind::STRUCTURED: return "object";
    case scene::ManagedValueKind::ASSET_REFERENCE: return "dataAssetRef";
    default: return "unsupported";
    }
}

JsonValue FieldSchema(const scene::ManagedFieldDescriptor& descriptor)
{
    const bool setting = (descriptor.flags & scene::kManagedFieldSetting) != 0;
    const bool array = IsArray(descriptor.kind);
    const auto elementKind = array ? static_cast<scene::ManagedValueKind>(static_cast<uint32_t>(descriptor.kind) - 6U) : descriptor.kind;
    JsonValue field = JsonValue::MakeObject();
    field.Set("name", JsonValue(descriptor.name));
    field.Set("displayName", JsonValue(descriptor.label));
    field.Set("type", JsonValue(setting ? array ? "list" : KindName(descriptor.kind) : "managed field"));
    field.Set("stored", JsonValue(setting));
    field.Set("role", JsonValue(setting ? "configuration" : "observation"));
    field.Set("visible", JsonValue(!(descriptor.flags & scene::kManagedFieldHidden)));
    field.Set("enabled", JsonValue(setting));
    field.Set("readOnly", JsonValue(!setting));
    field.Set("writable", JsonValue(false));
    if (descriptor.kind == scene::ManagedValueKind::NONE) field.Set("supported", JsonValue(false));
    if (array) field.Set("elementType", JsonValue(KindName(elementKind)));
    if (elementKind == scene::ManagedValueKind::GAME_OBJECT) field.Set("targetType", JsonValue("GameObject"));
    if (descriptor.flags & scene::kManagedFieldRange) {
        JsonValue range = JsonValue::MakeObject();
        range.Set("min", JsonValue(static_cast<double>(descriptor.minimum)));
        range.Set("max", JsonValue(static_cast<double>(descriptor.maximum)));
        field.Set("range", std::move(range));
        field.Set("minimum", JsonValue(static_cast<double>(descriptor.minimum)));
    }
    if (const auto metadata = ParseJson(descriptor.schemaJson); metadata && metadata->IsObject()) {
        field.Set("managedMetadata", *metadata);
        if (descriptor.kind == scene::ManagedValueKind::STRUCTURED) {
            if (const JsonValue* version = metadata->Find("codecVersion")) field.Set("codecVersion", *version);
            if (const JsonValue* nested = metadata->Find("nested"); nested && nested->IsObject()) {
                field.Set("nested", *nested);
                if (const JsonValue* collection = nested->Find("collection"); collection && collection->AsBool()) {
                    if (setting) field.Set("type", JsonValue("list"));
                    if (const JsonValue* element = nested->Find("element")) field.Set("elementSchema", *element);
                }
            }
        }
    }
    return field;
}

bool ReadField(scene::ManagedScriptAdapter& adapter, const scene::ManagedFieldDescriptor& field,
               uint32_t selector, scene::ManagedBridgeReply& reply, std::string& bytes, FieldIssue& issue)
{
    auto& runtime = scene::ManagedScriptRuntime::Instance();
    scene::ManagedBridgeRequest request;
    request.op = scene::ManagedBridgeOp::READ_FIELD;
    request.generation = adapter.Generation();
    request.instanceId = adapter.InstanceId();
    request.input = field.name.data();
    request.inputBytes = static_cast<uint32_t>(field.name.size());
    request.flags = selector;
    if (!runtime.Dispatch(request, reply, &bytes)) {
        issue.code = "MANAGED_FIELD_READ_FAILED";
        issue.message = runtime.LastDiagnostic();
        return false;
    }
    if (reply.valueKind != field.kind) return false;
    return true;
}

bool Number(float value, JsonValue& result, FieldIssue& issue)
{
    if (!std::isfinite(value)) {
        issue.code = "NON_FINITE_SCRIPT_VALUE";
        issue.message = "Field contains NaN or Infinity.";
        return false;
    }
    result = JsonValue(static_cast<double>(value));
    return true;
}

bool Vector(float x, float y, float z, JsonValue& result, FieldIssue& issue)
{
    JsonValue value = JsonValue::MakeArray();
    for (const float component : {x, y, z}) {
        JsonValue number;
        if (!Number(component, number, issue)) return false;
        value.Push(std::move(number));
    }
    result = std::move(value);
    return true;
}

bool Quaternion(float x, float y, float z, float w, JsonValue& result, FieldIssue& issue)
{
    JsonValue value = JsonValue::MakeArray();
    for (const float component : {x, y, z, w}) {
        JsonValue number;
        if (!Number(component, number, issue)) return false;
        value.Push(std::move(number));
    }
    result = std::move(value);
    return true;
}

JsonValue SettingsValue(const OpData& value)
{
    switch (value.GetType()) {
    case OpData::Type::Null: return JsonValue{};
    case OpData::Type::Bool: return JsonValue(value.AsBool());
    case OpData::Type::Int: return JsonValue(value.AsInt64());
    case OpData::Type::Float: return JsonValue(value.AsDouble());
    case OpData::Type::String: return JsonValue(value.AsString());
    case OpData::Type::Array: {
        JsonValue result = JsonValue::MakeArray();
        for (const OpData& item : value.AsArray()) result.Push(SettingsValue(item));
        return result;
    }
    case OpData::Type::Object: {
        JsonValue result = JsonValue::MakeObject();
        for (const auto& [key, item] : value.AsObject()) result.Set(key, SettingsValue(item));
        return result;
    }
    case OpData::Type::Vec3: break;
    }
    return JsonValue{};
}

/// @note 保存用 envelope だけを除き、ユーザー値と typed GO token は変更せず、GUID の再検索もしない。
bool Structured(std::string_view bytes, JsonValue& result, FieldIssue& issue)
{
    OpData envelope;
    if (!ReadManagedSettingsJson(bytes, envelope) || !envelope.IsObject() || envelope.AsObject().size() != 2) {
        issue.code = "INVALID_MANAGED_STRUCTURED_VALUE";
        issue.message = "Structured field requires a bounded versioned JSON envelope.";
        return false;
    }
    const OpData* version = envelope.Find("$version");
    const OpData* value = envelope.Find("value");
    if (!version || !version->IsInt() || version->AsInt64() != 1 || !value) {
        issue.code = "INVALID_MANAGED_STRUCTURED_VALUE";
        issue.message = "Structured field codec version is unsupported.";
        return false;
    }
    result = SettingsValue(*value);
    return true;
}

JsonValue Reference(scene::ManagedScriptAdapter& adapter, scene::Scene* scene, const std::string& guid,
                    uint64_t objectId, const std::string& key, JsonValue& diagnostics, int64_t elementIndex = -1)
{
    if (guid.empty()) return JsonValue{};
    JsonValue value = JsonValue::MakeObject();
    value.Set("guid", JsonValue(guid));
    if (!scene) return value;
    scene::ScriptObjectReference reference{guid, scene::EntityID::INVALID};
    scene::ManagedScriptRuntime::Instance().ResolveObjectId(adapter, objectId, reference);
    const auto* target = scene->GetGameObject(reference.id);
    if (target && target->instanceId != guid) target = nullptr;
    value.Set("id", target ? JsonValue(target->instanceId) : JsonValue{});
    value.Set("name", target ? JsonValue(target->name) : JsonValue{});
    value.Set("expectedType", JsonValue("GameObject"));
    value.Set("status", JsonValue(target ? "resolved" : "missing"));
    if (!target) AddIssue(diagnostics, key, {"MISSING_SCRIPT_REFERENCE", "Stored GameObject reference is missing or expired.", elementIndex});
    return value;
}

bool DecodeSummary(scene::ManagedScriptAdapter& adapter, scene::Scene* scene, const std::string& key,
                   const std::string& bytes, JsonValue& result, JsonValue& diagnostics, FieldIssue& issue)
{
    auto envelope = ParseJson(bytes);
    const JsonValue* value = envelope ? envelope->Find("value") : nullptr;
    if (!value) return false;
    if (const JsonValue* diagnostic = envelope->Find("diagnostic")) {
        if (const JsonValue* code = diagnostic->Find("code"); code && code->IsString()) {
            const std::string& name = code->AsString();
            if (name == "NON_FINITE_SCRIPT_VALUE") issue.code = "NON_FINITE_SCRIPT_VALUE";
            else if (name == "NULL_SCRIPT_SETTING") issue.code = "NULL_SCRIPT_SETTING";
            else if (name == "NULL_SCRIPT_ARRAY") issue.code = "NULL_SCRIPT_ARRAY";
            else if (name == "NULL_SCRIPT_ARRAY_ELEMENT") issue.code = "NULL_SCRIPT_ARRAY_ELEMENT";
            else issue.code = "MANAGED_FIELD_READ_FAILED";
        }
        if (const JsonValue* message = diagnostic->Find("message")) issue.message = message->AsString();
        if (const JsonValue* index = diagnostic->Find("elementIndex")) issue.elementIndex = index->AsInt64(-1);
        return false;
    }
    result = *value;
    const auto project = [&](auto&& self, JsonValue& node, int64_t elementIndex) -> bool {
        if (const JsonValue* reference = node.Find("$observationGo"); reference && reference->AsBool()) {
            const JsonValue* guid = node.Find("guid");
            const JsonValue* identity = node.Find("objectId");
            if (!guid || !guid->IsString() || !identity || !identity->IsString()) return false;
            uint64_t object = 0;
            for (const char digit : identity->AsString()) {
                if (digit < '0' || digit > '9' || object > (UINT64_MAX - static_cast<unsigned>(digit - '0')) / 10) return false;
                object = object * 10 + static_cast<unsigned>(digit - '0');
            }
            node = Reference(adapter, scene, guid->AsString(), object, key, diagnostics, elementIndex);
            return true;
        }
        for (size_t index = 0; index < node.AsArray().size(); ++index)
            if (!self(self, node.AsArray()[index], static_cast<int64_t>(index))) return false;
        for (auto& child : node.AsObject()) if (!self(self, child.second, elementIndex)) return false;
        return true;
    };
    if (!project(project, result, -1)) { issue.message = "Managed summary contains an invalid reference token."; return false; }
    return true;
}

bool DecodeScalar(scene::ManagedScriptAdapter& adapter, scene::Scene* scene, const scene::ManagedFieldDescriptor& field,
                  const scene::ManagedBridgeReply& reply, const std::string& bytes,
                  JsonValue& result, JsonValue& diagnostics, FieldIssue& issue)
{
    if (reply.flags & scene::kManagedNullValue) {
        if (field.kind == scene::ManagedValueKind::GAME_OBJECT) return bytes.empty() && reply.integer == 0;
        if (field.kind == scene::ManagedValueKind::STRING) {
            if (!(field.flags & scene::kManagedFieldSetting)) return true;
            issue.code = "NULL_SCRIPT_SETTING";
            issue.message = "Saved string setting is null.";
        }
        return false;
    }
    switch (field.kind) {
    case scene::ManagedValueKind::FLOAT: return Number(reply.x, result, issue);
    case scene::ManagedValueKind::INT:
        if (reply.integer < std::numeric_limits<int32_t>::min() || reply.integer > std::numeric_limits<int32_t>::max()) return false;
        result = JsonValue(reply.integer); return true;
    case scene::ManagedValueKind::BOOL:
        if (reply.integer != 0 && reply.integer != 1) return false;
        result = JsonValue(reply.integer != 0); return true;
    case scene::ManagedValueKind::STRING: result = JsonValue(bytes); return true;
    case scene::ManagedValueKind::VECTOR3: return Vector(reply.x, reply.y, reply.z, result, issue);
    case scene::ManagedValueKind::QUATERNION: return Quaternion(reply.x, reply.y, reply.z, reply.w, result, issue);
    case scene::ManagedValueKind::STRUCTURED: return Structured(bytes, result, issue);
    case scene::ManagedValueKind::ASSET_REFERENCE:
        if (bytes.find('\0') != std::string::npos) return false;
        result = JsonValue(bytes); return true;
    case scene::ManagedValueKind::GAME_OBJECT:
        if (bytes.empty()) return false;
        result = Reference(adapter, scene, bytes, std::bit_cast<uint64_t>(reply.integer), field.name, diagnostics); return true;
    default: return false;
    }
}

/// @note Host の count32 LE と固定値・UTF8 length32 列だけを復号し、field 全体の検証後に値を公開する。
/// @see https://learn.microsoft.com/en-us/dotnet/api/system.io.binarywriter numeric writes are little-endian。
bool DecodeArray(scene::ManagedScriptAdapter& adapter, scene::Scene* scene, const scene::ManagedFieldDescriptor& field,
                 const scene::ManagedBridgeReply& reply, const std::string& bytes,
                 JsonValue& result, JsonValue& diagnostics, FieldIssue& issue)
{
    if (reply.flags & scene::kManagedNullValue) {
        issue.code = "NULL_SCRIPT_ARRAY";
        issue.message = "Array is null; empty and null arrays are distinct.";
        return false;
    }
    size_t offset = 0;
    uint32_t count = 0;
    if (!ReadWire(bytes, offset, count)) return false;
    std::string identities;
    size_t identityOffset = 0;
    if (field.kind == scene::ManagedValueKind::GAME_OBJECT_ARRAY) {
        scene::ManagedBridgeReply idsReply;
        if (!ReadField(adapter, field, 1, idsReply, identities, issue)) return false;
        uint32_t identityCount = 0;
        if ((idsReply.flags & scene::kManagedNullValue) || !ReadWire(identities, identityOffset, identityCount) || identityCount != count) return false;
    }
    JsonValue array = JsonValue::MakeArray();
    for (uint32_t index = 0; index < count; ++index) {
        JsonValue value;
        issue.elementIndex = index;
        switch (field.kind) {
        case scene::ManagedValueKind::FLOAT_ARRAY: {
            float number = 0;
            if (!ReadWire(bytes, offset, number) || !Number(number, value, issue)) return false;
            break;
        }
        case scene::ManagedValueKind::INT_ARRAY: {
            int32_t number = 0;
            if (!ReadWire(bytes, offset, number)) return false;
            value = JsonValue(number); break;
        }
        case scene::ManagedValueKind::BOOL_ARRAY: {
            uint8_t number = 0;
            if (!ReadWire(bytes, offset, number) || number > 1) return false;
            value = JsonValue(number != 0); break;
        }
        case scene::ManagedValueKind::VECTOR3_ARRAY: {
            float x = 0, y = 0, z = 0;
            if (!ReadWire(bytes, offset, x) || !ReadWire(bytes, offset, y) || !ReadWire(bytes, offset, z) || !Vector(x, y, z, value, issue)) return false;
            break;
        }
        case scene::ManagedValueKind::STRING_ARRAY:
        case scene::ManagedValueKind::GAME_OBJECT_ARRAY: {
            uint32_t length = 0;
            if (!ReadWire(bytes, offset, length)) return false;
            const bool null = length == std::numeric_limits<uint32_t>::max();
            if (null && field.kind == scene::ManagedValueKind::STRING_ARRAY) {
                issue.code = "NULL_SCRIPT_ARRAY_ELEMENT"; issue.message = "String array element is null."; return false;
            }
            if (!null && (offset > bytes.size() || length > bytes.size() - offset)) return false;
            const std::string text = null ? std::string{} : bytes.substr(offset, length);
            if (!null) offset += length;
            if (field.kind == scene::ManagedValueKind::STRING_ARRAY) value = JsonValue(text);
            else {
                uint64_t objectId = 0;
                if (!ReadWire(identities, identityOffset, objectId) || (null && objectId != 0) || (!null && text.empty())) return false;
                value = Reference(adapter, scene, text, objectId, field.name, diagnostics, index);
            }
            break;
        }
        default: return false;
        }
        array.Push(std::move(value));
    }
    issue.elementIndex = -1;
    if (offset != bytes.size() || (!identities.empty() && identityOffset != identities.size())) return false;
    result = std::move(array);
    return true;
}

void ProjectFields(scene::ManagedScriptAdapter& adapter, scene::Scene* scene, JsonValue& result, bool defaults)
{
    JsonValue fields = JsonValue::MakeObject();
    JsonValue schema = JsonValue::MakeArray();
    JsonValue diagnostics = JsonValue::MakeArray();
    /// @note BIND_OWNER は Host metadata 接続だけを更新し、callback・world 操作を実行しない。
    const bool bound = adapter.RefreshOwnerBinding();
    if (adapter.IsRuntimeFaulted()) {
        JsonValue fault = JsonValue::MakeObject();
        fault.Set("message", JsonValue(adapter.FaultDiagnostic()));
        result.Set("fault", std::move(fault));
    }
    for (const auto& descriptor : adapter.Fields()) {
        if (defaults && descriptor.kind == scene::ManagedValueKind::NONE) continue;
        JsonValue value;
        JsonValue fieldSchema = FieldSchema(descriptor);
        FieldIssue issue;
        bool read = false;
        if (descriptor.kind == scene::ManagedValueKind::NONE) {
            issue.code = "UNSUPPORTED_SCRIPT_FIELD";
            issue.message = "Field type has no supported observation codec; its value was not read.";
        } else if (!bound && (descriptor.kind == scene::ManagedValueKind::GAME_OBJECT || descriptor.kind == scene::ManagedValueKind::GAME_OBJECT_ARRAY)) {
            issue.code = "MANAGED_FIELD_READ_FAILED";
            issue.message = "Script owner is not bound to the current Scene.";
        } else {
            scene::ManagedBridgeReply reply;
            std::string bytes;
            if (ReadField(adapter, descriptor, defaults ? 0 : 2, reply, bytes, issue)) {
                if (!defaults) read = DecodeSummary(adapter, scene, descriptor.name, bytes, value, diagnostics, issue);
                else read = IsArray(descriptor.kind)
                    ? DecodeArray(adapter, scene, descriptor, reply, bytes, value, diagnostics, issue)
                    : DecodeScalar(adapter, scene, descriptor, reply, bytes, value, diagnostics, issue);
            }
        }
        if (!read) { value = JsonValue{}; AddIssue(diagnostics, descriptor.name, issue); }
        if ((IsArray(descriptor.kind) || descriptor.kind == scene::ManagedValueKind::STRUCTURED) && value.IsArray())
            fieldSchema.Set("count", JsonValue(static_cast<int64_t>(value.AsArray().size())));
        if (defaults && (descriptor.flags & scene::kManagedFieldSetting)) fieldSchema.Set("default", value);
        fields.Set(descriptor.name, std::move(value));
        schema.Push(std::move(fieldSchema));
    }
    result.Set("observationStatus", JsonValue(diagnostics.AsArray().empty() ? "complete" : "partial"));
    result.Set("fields", std::move(fields));
    result.Set("schema", std::move(schema));
    result.Set("fieldDiagnostics", std::move(diagnostics));
    result.Set("sample", JsonValue("summary"));
    result.Set("snapshot", JsonValue(false));
}

} /// @note namespace

void InspectManagedFields(scene::ManagedScriptAdapter& adapter, scene::Scene& scene, JsonValue& result)
{
    ProjectFields(adapter, &scene, result, false);
}

bool InspectManagedPage(scene::ManagedScriptAdapter& adapter, const JsonValue& options, JsonValue& result, std::string& diagnostic)
{
    auto& runtime = scene::ManagedScriptRuntime::Instance();
    if (!adapter.RefreshOwnerBinding()) { diagnostic = runtime.LastDiagnostic(); return false; }
    uint64_t observation = 0;
    if (const JsonValue* token = options.Find("observationId")) {
        if (!token->IsString() || token->AsString().empty()) { diagnostic = "observationId must be a decimal snapshot identity."; return false; }
        for (const char digit : token->AsString()) {
            if (digit < '0' || digit > '9' || observation > (UINT64_MAX - static_cast<unsigned>(digit - '0')) / 10) {
                diagnostic = "observationId must be a decimal snapshot identity."; return false;
            }
            observation = observation * 10 + static_cast<unsigned>(digit - '0');
        }
        if (!observation) { diagnostic = "observationId must be nonzero."; return false; }
    }
    const bool close = options.Find("close") && options.Find("close")->AsBool();
    scene::ManagedBridgeRequest request;
    request.generation = adapter.Generation(); request.instanceId = adapter.InstanceId(); request.ownerId = adapter.OwnerId();
    scene::ManagedBridgeReply reply;
    if (close) {
        if (!observation) { diagnostic = "close requires observationId."; return false; }
        request.op = scene::ManagedBridgeOp::CLOSE_OBSERVATION; request.integer = std::bit_cast<int64_t>(observation);
        if (!runtime.Dispatch(request, reply)) { diagnostic = runtime.LastDiagnostic(); return false; }
        result.Set("observationId", JsonValue(std::to_string(observation))); result.Set("closed", JsonValue(true)); return true;
    }
    int64_t start = 0;
    int count = 64, wireBytes = 65536;
    std::string path;
    if (const JsonValue* value = options.Find("path")) {
        if (!value->IsString()) { diagnostic = "path must be an RFC 6901 string."; return false; }
        path = value->AsString();
    }
    if ((options.Find("start") && !options.Find("start")->TryGetInt64(start)) || start < 0
        || (options.Find("count") && !options.Find("count")->TryGetInt32(count)) || count < 1
        || (options.Find("wireBytes") && !options.Find("wireBytes")->TryGetInt32(wireBytes)) || wireBytes < 2048) {
        diagnostic = "Page requires nonnegative start, positive count and at least 2048 wire bytes."; return false;
    }
    bool opened = false;
    if (!observation) {
        request.op = scene::ManagedBridgeOp::OPEN_OBSERVATION;
        request.input = path.data(); request.inputBytes = static_cast<uint32_t>(path.size());
        if (!runtime.Dispatch(request, reply)) { diagnostic = runtime.LastDiagnostic(); return false; }
        observation = std::bit_cast<uint64_t>(reply.integer); opened = true;
    }
    JsonValue input = JsonValue::MakeObject();
    input.Set("path", JsonValue(path)); input.Set("start", JsonValue(start)); input.Set("count", JsonValue(count));
    input.Set("wireBytes", JsonValue(wireBytes));
    const std::string json = SerializeJson(input);
    request.op = scene::ManagedBridgeOp::READ_OBSERVATION; request.integer = std::bit_cast<int64_t>(observation);
    request.input = json.data(); request.inputBytes = static_cast<uint32_t>(json.size());
    std::string output;
    if (!runtime.Dispatch(request, reply, &output)) {
        diagnostic = runtime.LastDiagnostic();
        if (opened) { request.op = scene::ManagedBridgeOp::CLOSE_OBSERVATION; runtime.Dispatch(request, reply); }
        return false;
    }
    auto parsed = ParseJson(output);
    if (!parsed || !parsed->IsObject()) {
        diagnostic = "Host returned an invalid observation page.";
        if (opened) { request.op = scene::ManagedBridgeOp::CLOSE_OBSERVATION; runtime.Dispatch(request, reply); }
        return false;
    }
    result.Set("observation", std::move(*parsed));
    result.Set("samplePhase", JsonValue("snapshot"));
    return true;
}

void CatalogManagedFields(scene::ManagedScriptAdapter& adapter, JsonValue& result)
{
    ProjectFields(adapter, nullptr, result, true);
}

} /// @note namespace fbzz::editor::ai::bus
