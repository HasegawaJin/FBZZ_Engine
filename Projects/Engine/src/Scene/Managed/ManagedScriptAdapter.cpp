/// @file    ManagedScriptAdapter.cpp
/// @brief   managed Script の lifecycle と primitive 反射の native 転送。
/// @author  Hasegawa Jin
/// @date    2026-10-04
#include <Engine/Scene/Managed/ManagedScriptAdapter.hpp>
#include <Engine/Scene/Managed/ManagedScriptRuntime.hpp>
#include <Engine/Scene/Scene.hpp>
#include "ManagedFieldCodec.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace fbzz::scene {
namespace {
std::string AuthoringInput(std::string_view fields, const std::vector<std::string>& rebindReferences)
{
    std::string result = "{\"fields\":"; result.append(fields); result.append(",\"rebind\":[");
    constexpr char hexadecimal[] = "0123456789abcdef";
    bool first = true;
    for (const std::string& path : rebindReferences) {
        if (!first) result.push_back(',');
        first = false; result.push_back('"');
        for (unsigned char value : path) {
            if (value == '"' || value == '\\') { result.push_back('\\'); result.push_back(static_cast<char>(value)); }
            else if (value < 32) { result.append("\\u00"); result.push_back(hexadecimal[value >> 4]); result.push_back(hexadecimal[value & 15]); }
            else result.push_back(static_cast<char>(value));
        }
        result.push_back('"');
    }
    result.append("]}"); return result;
}
} /// @note namespace

ManagedScriptAdapter::ManagedScriptAdapter(ManagedScriptRuntime& runtime, std::string typeName,
                                         uint64_t generation, uint64_t instanceId, uint64_t ownerId, bool executeAlways, std::string savedScriptId)
    : m_runtime(runtime), m_typeName(std::move(typeName)), m_savedScriptId(std::move(savedScriptId)), m_generation(generation),
      m_instanceId(instanceId), m_ownerId(ownerId), m_executeAlways(executeAlways)
{
    m_referenceBirth = InspectionId();
}

ManagedScriptAdapter::~ManagedScriptAdapter() { m_runtime.CancelOwned(*this); m_runtime.Release(*this); }

bool ManagedScriptAdapter::HasLiveContext() const
{
    return m_scene && m_boundScene == m_scene && m_boundObject == m_gameObject && m_contextSceneGeneration
        && m_scene->GetRenderSceneGeneration() == m_contextSceneGeneration
        && m_scene->GetGameObject(m_contextEntity) == m_gameObject;
}

bool ManagedScriptAdapter::RefreshOwnerBinding() { return m_runtime.BindOwner(*this); }

bool ManagedScriptAdapter::ValidateSettingsJson(std::string_view json, SettingsValidationPurpose purpose,
    const std::vector<std::string>& rebindReferences)
{
    if (purpose != SettingsValidationPurpose::AUTHORING && !rebindReferences.empty()) return false;
    const std::string authoring = purpose == SettingsValidationPurpose::AUTHORING ? AuthoringInput(json, rebindReferences) : std::string{};
    if (purpose == SettingsValidationPurpose::AUTHORING) json = authoring;
    ManagedBridgeRequest request;
    request.op = ManagedBridgeOp::VALIDATE_FIELDS;
    request.flags = static_cast<uint32_t>(purpose);
    request.input = json.data();
    request.inputBytes = static_cast<uint32_t>(json.size());
    ManagedBridgeReply reply;
    return m_runtime.DispatchPrepared(*this, request, reply);
}

bool ManagedScriptAdapter::ApplySettingsJson(std::string_view json, SettingsValidationPurpose purpose,
    const std::vector<std::string>& rebindReferences)
{
    if (purpose != SettingsValidationPurpose::AUTHORING && !rebindReferences.empty()) return false;
    const std::string authoring = purpose == SettingsValidationPurpose::AUTHORING ? AuthoringInput(json, rebindReferences) : std::string{};
    if (purpose == SettingsValidationPurpose::AUTHORING) json = authoring;
    ManagedBridgeRequest request;
    request.op = ManagedBridgeOp::WRITE_FIELDS;
    request.flags = static_cast<uint32_t>(purpose);
    request.input = json.data();
    request.inputBytes = static_cast<uint32_t>(json.size());
    ManagedBridgeReply reply;
    return m_runtime.DispatchPrepared(*this, request, reply);
}

bool ManagedScriptAdapter::ValidateReferences()
{
    ManagedBridgeRequest request;
    request.op = ManagedBridgeOp::VALIDATE_REFERENCES;
    ManagedBridgeReply reply;
    return m_runtime.DispatchPrepared(*this, request, reply);
}

bool ManagedScriptAdapter::ResolveSavedComponentReferences()
{
    ManagedBridgeRequest request; request.op = ManagedBridgeOp::RESOLVE_SAVED_COMPONENT_REFERENCES;
    ManagedBridgeReply reply;
    return m_runtime.DispatchPrepared(*this, request, reply);
}

bool ManagedScriptAdapter::RestoreSettingsJson(std::string_view json)
{
    ManagedBridgeRequest request;
    request.op = ManagedBridgeOp::WRITE_FIELDS;
    request.flags = 1;
    request.input = json.data();
    request.inputBytes = static_cast<uint32_t>(json.size());
    ManagedBridgeReply reply;
    return m_runtime.DispatchPrepared(*this, request, reply);
}

void ManagedScriptAdapter::Invoke(ManagedCallback callback)
{
    if (callback == ManagedCallback::CONNECT && m_connectionsActive) return;
    if (!m_runtime.Invoke(*this, callback)) RecordFault();
    else if (callback == ManagedCallback::CONNECT) m_connectionsActive = true;
}

void ManagedScriptAdapter::InvokeCollision(ManagedCallback callback, const CollisionInfo& collision)
{
    if (!m_runtime.InvokeCollision(*this, callback, collision)) RecordFault();
}

void ManagedScriptAdapter::RecordFault()
{
    if (m_faultDiagnostic.empty()) {
        m_faultDiagnostic = m_runtime.LastDiagnostic();
        if (m_faultDiagnostic.empty()) m_faultDiagnostic = "Managed callback could not execute in its current lifetime or scope.";
    }
    if (m_faultDiagnostic.find("CS_REQUIRED_COMPONENT") != std::string::npos) BlockForMissingRequirements();
    else { m_runtimeFaulted = true; enabled = false; }
    m_runtime.CancelOwned(*this);
}

void ManagedScriptAdapter::OnDrawGizmos()
{
    std::vector<ScriptDebugDrawCommand> commands;
    if (!m_runtime.CaptureGizmos(*this, false, commands)) return;
    for (const auto& command : commands) {
        gizmo.SetDepthTest(command.depthTest);
        if (command.type == ScriptDebugDrawType::Line) gizmo.DrawLine(command.a, command.b, command.color);
        else if (command.type == ScriptDebugDrawType::Sphere) gizmo.DrawSphere(command.a, command.radius, command.color);
        else if (command.type == ScriptDebugDrawType::Box) gizmo.DrawBox(command.a, command.halfExtents, command.color);
    }
    gizmo.SetDepthTest(false);
}

void ManagedScriptAdapter::OnDrawGizmosSelected()
{
    std::vector<ScriptDebugDrawCommand> commands;
    if (!m_runtime.CaptureGizmos(*this, true, commands)) return;
    for (const auto& command : commands) {
        gizmo.SetDepthTest(command.depthTest);
        if (command.type == ScriptDebugDrawType::Line) gizmo.DrawLine(command.a, command.b, command.color);
        else if (command.type == ScriptDebugDrawType::Sphere) gizmo.DrawSphere(command.a, command.radius, command.color);
        else if (command.type == ScriptDebugDrawType::Box) gizmo.DrawBox(command.a, command.halfExtents, command.color);
    }
    gizmo.SetDepthTest(false);
}

void ManagedScriptAdapter::OnAwake()
{
    Invoke(ManagedCallback::AWAKE);
    if (!m_runtimeFaulted && !RequirementsBlocked()) m_managedAwoken = true;
}
void ManagedScriptAdapter::OnStart()
{
    Invoke(ManagedCallback::START);
    if (!m_runtimeFaulted && !RequirementsBlocked()) { m_managedStarted = true; if (enabled) Invoke(ManagedCallback::CONNECT); }
}
void ManagedScriptAdapter::OnEnable()
{
    Invoke(ManagedCallback::ENABLE);
    if (m_managedStarted && !m_runtimeFaulted && !m_connectionsActive) Invoke(ManagedCallback::CONNECT);
}
void ManagedScriptAdapter::OnDisable() { Invoke(ManagedCallback::DISABLE); m_runtime.CancelOwned(*this); }
void ManagedScriptAdapter::OnUpdate() { Invoke(ManagedCallback::UPDATE); }
void ManagedScriptAdapter::OnFixedUpdate() { Invoke(ManagedCallback::FIXED_UPDATE); }
void ManagedScriptAdapter::OnLateUpdate() { Invoke(ManagedCallback::LATE_UPDATE); }
void ManagedScriptAdapter::OnDestroy()
{
    m_runtime.CancelOwned(*this);
    Invoke(ManagedCallback::DESTROY);
    m_managedAwoken = false;
    m_managedStarted = false;
}
void ManagedScriptAdapter::Reset() { Invoke(ManagedCallback::RESET); }
void ManagedScriptAdapter::OnValidate() { Invoke(ManagedCallback::VALIDATE); }
void ManagedScriptAdapter::OnBeforeSerialize() { Invoke(ManagedCallback::BEFORE_SERIALIZE); }
void ManagedScriptAdapter::OnAfterDeserialize() { Invoke(ManagedCallback::AFTER_DESERIALIZE); }
void ManagedScriptAdapter::OnCollisionEnter(const CollisionInfo& collision) { InvokeCollision(ManagedCallback::COLLISION_ENTER, collision); }
void ManagedScriptAdapter::OnCollisionStay(const CollisionInfo& collision) { InvokeCollision(ManagedCallback::COLLISION_STAY, collision); }
void ManagedScriptAdapter::OnCollisionExit(const CollisionInfo& collision) { InvokeCollision(ManagedCallback::COLLISION_EXIT, collision); }
void ManagedScriptAdapter::OnTriggerEnter(const CollisionInfo& collision) { InvokeCollision(ManagedCallback::TRIGGER_ENTER, collision); }
void ManagedScriptAdapter::OnTriggerStay(const CollisionInfo& collision) { InvokeCollision(ManagedCallback::TRIGGER_STAY, collision); }
void ManagedScriptAdapter::OnTriggerExit(const CollisionInfo& collision) { InvokeCollision(ManagedCallback::TRIGGER_EXIT, collision); }

void ManagedScriptAdapter::OnAnimationEvent(const AnimationEventInfo& info)
{ if (Script::IsInPlayMode() && !m_runtime.InvokeAnimationEvent(*this, info)) RecordFault(); }
void ManagedScriptAdapter::OnAnimatorMove(const RootMotionInfo& info)
{ if (Script::IsInPlayMode() && !m_runtime.InvokeAnimatorMove(*this, info)) RecordFault(); }
void ManagedScriptAdapter::OnSequenceEvent(const SequenceEventInfo& info)
{ if (Script::IsInPlayMode() && !m_runtime.InvokeSequenceEvent(*this, info)) RecordFault(); }
void ManagedScriptAdapter::OnSequenceFinished(const char* sequenceName)
{ if (Script::IsInPlayMode() && !m_runtime.InvokeSequenceFinished(*this, sequenceName ? sequenceName : "")) RecordFault(); }
void ManagedScriptAdapter::OnNavMeshDestinationReached()
{ if (Script::IsInPlayMode()) Invoke(ManagedCallback::NAV_DESTINATION_REACHED); }
void ManagedScriptAdapter::OnNavMeshPathFailed()
{ if (Script::IsInPlayMode()) Invoke(ManagedCallback::NAV_PATH_FAILED); }
void ManagedScriptAdapter::OnNavMeshTargetSpotted()
{ if (Script::IsInPlayMode()) Invoke(ManagedCallback::NAV_TARGET_SPOTTED); }
void ManagedScriptAdapter::OnNavMeshTargetLost()
{ if (Script::IsInPlayMode()) Invoke(ManagedCallback::NAV_TARGET_LOST); }

bool ManagedScriptAdapter::InitializeFields()
{
    std::vector<ManagedFieldDescriptor> fields;
    for (uint32_t index = 0;; ++index) {
        ManagedBridgeRequest request;
        request.op = ManagedBridgeOp::GET_FIELD;
        request.generation = m_generation;
        request.instanceId = m_instanceId;
        request.index = index;
        ManagedBridgeReply reply;
        std::string name;
        if (!m_runtime.Dispatch(request, reply, &name)) {
            if (reply.status == ManagedBridgeStatus::NOT_FOUND && index == 0) break;
            return false;
        }
        ManagedFieldDescriptor field;
        field.name = std::move(name);
        field.kind = reply.valueKind;
        field.flags = reply.flags;
        field.minimum = reply.x;
        field.maximum = reply.y;
        if (field.flags & kManagedFieldRange) {
            if (!std::isfinite(field.minimum) || !std::isfinite(field.maximum) || field.minimum > field.maximum) return false;
        }
        request.flags = 1;
        ManagedBridgeReply labelReply;
        if (!m_runtime.Dispatch(request, labelReply, &field.label)) return false;
        request.flags = 2;
        if (!m_runtime.Dispatch(request, labelReply, &field.schemaJson)) return false;
        request.flags = 3;
        std::string keys;
        if (!m_runtime.Dispatch(request, labelReply, &keys) || !managed_detail::DecodeArray(keys, field.previousKeys)) return false;
        fields.push_back(std::move(field));
        if (index + 1 >= reply.index) break;
    }
    m_fields = std::move(fields);
    return true;
}

bool ManagedScriptAdapter::CaptureFields(std::vector<ManagedFieldValue>& fields, bool includeReloadState)
{
    fields.clear();
    for (const auto& field : m_fields) {
        if (!(field.flags & kManagedFieldSetting)
            && !(includeReloadState && (field.flags & kManagedFieldReloadState))) continue;
        ManagedBridgeRequest request;
        request.op = ManagedBridgeOp::READ_FIELD;
        request.generation = m_generation;
        request.instanceId = m_instanceId;
        request.input = field.name.data();
        request.inputBytes = static_cast<uint32_t>(field.name.size());
        ManagedFieldValue value;
        value.name = field.name;
        if (!m_runtime.Dispatch(request, value.value, &value.text)) return false;
        fields.push_back(std::move(value));
    }
    return true;
}

bool ManagedScriptAdapter::ReadSettingsSnapshot(std::string& json)
{
    if (!RefreshOwnerBinding()) return false;
    ManagedBridgeRequest request;
    request.op = ManagedBridgeOp::GET_SETTINGS_SNAPSHOT;
    request.generation = m_generation;
    request.instanceId = m_instanceId;
    ManagedBridgeReply reply;
    return m_runtime.Dispatch(request, reply, &json);
}

bool ManagedScriptAdapter::ReadSettingsPage(std::string_view path, uint64_t start, uint32_t count, std::string& json)
{
    if (!RefreshOwnerBinding()) return false;
    std::string escaped;
    constexpr char HEX[] = "0123456789abcdef";
    for (const char character : path) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte < 32) { escaped += "\\u00"; escaped += HEX[byte >> 4]; escaped += HEX[byte & 15]; continue; }
        if (character == '"' || character == '\\') escaped += '\\';
        escaped += character;
    }
    const std::string input = "{\"path\":\"" + escaped + "\",\"start\":" + std::to_string(start)
        + ",\"count\":" + std::to_string(count) + ",\"packetBytes\":65536}";
    ManagedBridgeRequest request; request.op = ManagedBridgeOp::READ_SETTINGS_PAGE;
    request.generation = m_generation; request.instanceId = m_instanceId;
    request.input = input.data(); request.inputBytes = static_cast<uint32_t>(input.size());
    ManagedBridgeReply reply; return m_runtime.Dispatch(request, reply, &json);
}
bool ManagedScriptAdapter::OpenSettingsEdit(uint64_t& sessionId, std::string& revision)
{
    if (!RefreshOwnerBinding()) return false;
    ManagedBridgeRequest request; request.op = ManagedBridgeOp::OPEN_SETTINGS_EDIT;
    request.generation = m_generation; request.instanceId = m_instanceId;
    ManagedBridgeReply reply;
    if (!m_runtime.Dispatch(request, reply, &revision)) return false;
    sessionId = std::bit_cast<uint64_t>(reply.integer); return sessionId != 0;
}
void ManagedScriptAdapter::CloseSettingsEdit(uint64_t sessionId, bool retainPrepared)
{
    const std::string input = retainPrepared ? "{\"retainPrepared\":true}" : "{\"retainPrepared\":false}";
    ManagedBridgeRequest request; request.op = ManagedBridgeOp::CLOSE_SETTINGS_EDIT;
    request.generation = m_generation; request.integer = std::bit_cast<int64_t>(sessionId);
    request.input = input.data(); request.inputBytes = static_cast<uint32_t>(input.size());
    ManagedBridgeReply reply; (void)m_runtime.Dispatch(request, reply);
}
bool ManagedScriptAdapter::ReadSettingsRevision(uint64_t& revision)
{
    ManagedBridgeRequest request;
    request.op = ManagedBridgeOp::GET_SETTINGS_REVISION;
    request.generation = m_generation;
    request.instanceId = m_instanceId;
    ManagedBridgeReply reply;
    if (!m_runtime.Dispatch(request, reply)) return false;
    revision = std::bit_cast<uint64_t>(reply.integer);
    return true;
}

void ManagedScriptAdapter::CommitInstance(uint64_t generation, uint64_t instanceId,
                                        std::vector<ManagedFieldDescriptor> fields, bool preserveLifecycle)
{
    const bool faulted = m_runtimeFaulted;
    const bool blocked = RequirementsBlocked();
    const bool wasEnabled = enabled;
    /// @note 初回 enabled 通知だけ張り直し、既存の障害・必須設定ラッチを復帰させない。
    if (!preserveLifecycle) { ResetLifecycleState(); m_managedAwoken = false; m_managedStarted = false; }
    m_connectionsActive = false;
    m_runtimeFaulted = faulted;
    if (blocked) BlockForMissingRequirements();
    enabled = wasEnabled;
    m_generation = generation;
    m_instanceId = instanceId;
    m_fields = std::move(fields);
}

void ManagedScriptAdapter::Reflect(IReflector& reflector) { (void)ReflectFields(reflector, true); }

void ManagedScriptAdapter::ReflectReadOnly(IReflector& reflector) { (void)ReflectFields(reflector, false); }

bool ManagedScriptAdapter::ReflectForSerialization(IReflector& reflector, std::string& diagnostic, bool invokeCallbacks)
{
    diagnostic.clear();
    if (invokeCallbacks && !m_runtimeFaulted && !RequirementsBlocked()
        && !ExecuteProfiledCallback(&Script::OnBeforeSerialize, ScriptCallbackKind::BEFORE_SERIALIZE, "OnBeforeSerialize")) {
        diagnostic = m_typeName + ".OnBeforeSerialize: " + m_faultDiagnostic;
        return false;
    }
    /// @note 保存は欠損した必須参照を保持する。値 codec の検証だけを要求し、設定を変更しない。
    if (!ValidateSettingsJson("{}", SettingsValidationPurpose::SNAPSHOT)) {
        diagnostic = m_typeName + ": " + m_runtime.LastDiagnostic();
        return false;
    }
    return ReflectFields(reflector, false, &diagnostic);
}

bool ManagedScriptAdapter::ReflectFields(IReflector& reflector, bool write, std::string* diagnostic)
{
    const auto fail = [&](const char* field, const char* reason) {
        if (diagnostic) *diagnostic = m_typeName + "." + field + ": " + reason;
        reflector.EndField();
        return false;
    };
    if (!RefreshOwnerBinding()) return fail("owner", m_runtime.LastDiagnostic().c_str());
    for (const auto& field : m_fields) {
        if (field.kind == ManagedValueKind::NONE) continue;
        const bool setting = (field.flags & kManagedFieldSetting) != 0;
        if (!setting && (!(field.flags & kManagedFieldObservation)
            || !reflector.BeginObservation(field.name.c_str(), "managed field"))) continue;
        ManagedBridgeRequest request;
        request.op = ManagedBridgeOp::READ_FIELD;
        request.generation = m_generation;
        request.instanceId = m_instanceId;
        request.input = field.name.data();
        request.inputBytes = static_cast<uint32_t>(field.name.size());
        ManagedBridgeReply value;
        std::string text;
        if (!m_runtime.Dispatch(request, value, &text)) {
            if (diagnostic && setting) return fail(field.name.c_str(), m_runtime.LastDiagnostic().c_str());
            continue;
        }
        reflector.BeginField(field.name.c_str(), field.label.c_str());
        reflector.SetFieldVisible(!(field.flags & kManagedFieldHidden));
        reflector.SetFieldReadOnly(!write || !setting);
        reflector.SetPreviousFieldKeys(field.previousKeys);
        if (field.flags & kManagedFieldRange) {
            reflector.SetFieldMin(field.minimum);
            reflector.SetFieldMax(field.maximum);
        }
        bool changed = false;
        switch (field.kind) {
        case ManagedValueKind::FLOAT: {
            float edited = value.x;
            if (field.flags & kManagedFieldRange)
                reflector.FloatRange(field.name.c_str(), edited, field.minimum, field.maximum);
            else
                reflector.Field(field.name.c_str(), edited);
            changed = edited != value.x;
            value.x = edited;
            break;
        }
        case ManagedValueKind::INT: {
            int edited = static_cast<int>(value.integer);
            if (field.flags & kManagedFieldRange)
                reflector.IntRange(field.name.c_str(), edited,
                    static_cast<int>(std::ceil(std::clamp(static_cast<double>(field.minimum),
                        static_cast<double>(std::numeric_limits<int>::min()), static_cast<double>(std::numeric_limits<int>::max())))),
                    static_cast<int>(std::floor(std::clamp(static_cast<double>(field.maximum),
                        static_cast<double>(std::numeric_limits<int>::min()), static_cast<double>(std::numeric_limits<int>::max())))));
            else
                reflector.Field(field.name.c_str(), edited);
            changed = edited != value.integer;
            value.integer = edited;
            break;
        }
        case ManagedValueKind::BOOL: {
            bool edited = value.integer != 0;
            reflector.Field(field.name.c_str(), edited);
            changed = edited != (value.integer != 0);
            value.integer = edited ? 1 : 0;
            break;
        }
        case ManagedValueKind::STRING: {
            const std::string previous = text;
            reflector.Field(field.name.c_str(), text);
            changed = text != previous;
            break;
        }
        case ManagedValueKind::VECTOR3: {
            math::Vector3 edited{value.x, value.y, value.z};
            reflector.Field(field.name.c_str(), edited);
            changed = edited.x != value.x || edited.y != value.y || edited.z != value.z;
            value.x = edited.x;
            value.y = edited.y;
            value.z = edited.z;
            break;
        }
        case ManagedValueKind::QUATERNION: {
            math::Quaternion edited{value.x, value.y, value.z, value.w};
            reflector.Field(field.name.c_str(), edited);
            changed = edited.x != value.x || edited.y != value.y || edited.z != value.z || edited.w != value.w;
            value.x = edited.x; value.y = edited.y; value.z = edited.z; value.w = edited.w;
            break;
        }
        case ManagedValueKind::STRUCTURED: {
            managed_detail::StructuredValue edited{text};
            if (!edited.Valid()) {
                if (diagnostic && setting) return fail(field.name.c_str(), "Structured reference payload is invalid.");
                continue;
            }
            request.flags = 1;
            ManagedBridgeReply identitiesReply;
            std::string identities;
            std::vector<uint64_t> objectIds;
            if (!m_runtime.Dispatch(request, identitiesReply, &identities)
                || !managed_detail::DecodeArray(identities, objectIds, managed_detail::kMaximumStructuredElements)
                || objectIds.size() != edited.objectReferences.size()) {
                if (diagnostic && setting) return fail(field.name.c_str(), "Structured reference identities could not be read.");
                continue;
            }
            for (size_t index = 0; index < objectIds.size(); ++index)
                m_runtime.ResolveObjectId(*this, objectIds[index], edited.objectReferences[index]);
            request.flags = 0;
            reflector.ObjectField(field.name.c_str(), edited);
            if (!edited.ApplyReferences()) {
                if (diagnostic && setting) return fail(field.name.c_str(), "Structured references could not be preserved.");
                continue;
            }
            changed = edited.json != text || reflector.ResolveObjectReferences();
            text = std::move(edited.json);
            break;
        }
        case ManagedValueKind::ASSET_REFERENCE: {
            DataAssetRef reference{text};
            reflector.Field(field.name.c_str(), reference);
            changed = reference.path != text;
            text = std::move(reference.path);
            break;
        }
        case ManagedValueKind::GAME_OBJECT: {
            ScriptObjectReference reference{text, EntityID::INVALID};
            m_runtime.ResolveObjectId(*this, std::bit_cast<uint64_t>(value.integer), reference);
            const std::string before = reference.guid;
            reflector.ObjectReferenceField(field.name.c_str(), reference);
            changed = before != reference.guid || reflector.ResolveObjectReferences();
            text = std::move(reference.guid);
            value.flags = text.empty() ? kManagedNullValue : 0;
            if (field.flags & kManagedFieldRequired)
                reflector.RequireReference(field.name.c_str(), EntityRef{reference.id}, "");
            break;
        }
        case ManagedValueKind::FLOAT_ARRAY: {
            std::vector<float> values;
            if (!managed_detail::DecodeArray(text, values)) {
                if (diagnostic && setting) return fail(field.name.c_str(), "Array payload is invalid.");
                continue;
            }
            reflector.ListField(field.name.c_str(), values);
            const std::string edited = managed_detail::EncodeArray(values);
            changed = edited != text;
            text = edited;
            break;
        }
        case ManagedValueKind::INT_ARRAY: {
            std::vector<int> values;
            if (!managed_detail::DecodeArray(text, values)) {
                if (diagnostic && setting) return fail(field.name.c_str(), "Array payload is invalid.");
                continue;
            }
            reflector.ListField(field.name.c_str(), values);
            const std::string edited = managed_detail::EncodeArray(values);
            changed = edited != text;
            text = edited;
            break;
        }
        case ManagedValueKind::BOOL_ARRAY: {
            std::vector<bool> values;
            if (!managed_detail::DecodeArray(text, values)) {
                if (diagnostic && setting) return fail(field.name.c_str(), "Array payload is invalid.");
                continue;
            }
            reflector.ListField(field.name.c_str(), values);
            const std::string edited = managed_detail::EncodeArray(values);
            changed = edited != text;
            text = edited;
            break;
        }
        case ManagedValueKind::STRING_ARRAY: {
            std::vector<std::string> values;
            if (!managed_detail::DecodeArray(text, values)) {
                if (diagnostic && setting) return fail(field.name.c_str(), "Array payload is invalid.");
                continue;
            }
            reflector.ListField(field.name.c_str(), values);
            const std::string edited = managed_detail::EncodeArray(values);
            changed = edited != text;
            text = edited;
            break;
        }
        case ManagedValueKind::VECTOR3_ARRAY: {
            std::vector<math::Vector3> values;
            if (!managed_detail::DecodeArray(text, values)) {
                if (diagnostic && setting) return fail(field.name.c_str(), "Array payload is invalid.");
                continue;
            }
            reflector.ListField(field.name.c_str(), values);
            const std::string edited = managed_detail::EncodeArray(values);
            changed = edited != text;
            text = edited;
            break;
        }
        case ManagedValueKind::GAME_OBJECT_ARRAY: {
            std::vector<ScriptObjectReference> values;
            if (!managed_detail::DecodeArray(text, values)) {
                if (diagnostic && setting) return fail(field.name.c_str(), "Array payload is invalid.");
                continue;
            }
            ManagedBridgeRequest identitiesRequest = request;
            identitiesRequest.flags = 1;
            ManagedBridgeReply identitiesReply;
            std::string identities;
            std::vector<uint64_t> objectIds;
            if (!m_runtime.Dispatch(identitiesRequest, identitiesReply, &identities)
                || !managed_detail::DecodeArray(identities, objectIds) || objectIds.size() != values.size()) {
                if (diagnostic && setting) return fail(field.name.c_str(), "Array reference identities could not be read.");
                continue;
            }
            for (size_t index = 0; index < values.size(); ++index)
                m_runtime.ResolveObjectId(*this, objectIds[index], values[index]);
            reflector.ObjectReferenceListField(field.name.c_str(), values);
            const std::string edited = managed_detail::EncodeArray(values);
            changed = edited != text || reflector.ResolveObjectReferences();
            text = edited;
            if (field.flags & kManagedFieldRequired) {
                if (values.empty()) reflector.RequireReference(field.name.c_str(), EntityRef{}, "");
                for (const auto& reference : values) reflector.RequireReference(field.name.c_str(), EntityRef{reference.id}, "");
            }
            break;
        }
        default:
            if (diagnostic && setting) return fail(field.name.c_str(), "Setting codec is unsupported.");
            continue;
        }
        if (!write || !setting || !changed) continue;
        request.op = ManagedBridgeOp::WRITE_FIELD;
        request.index = static_cast<uint32_t>(field.kind);
        request.integer = value.integer;
        request.x = value.x;
        request.y = value.y;
        request.z = value.z;
        request.w = value.w;
        request.flags = value.flags & kManagedNullValue;
        std::string input = field.name;
        if (field.kind == ManagedValueKind::STRING || field.kind == ManagedValueKind::GAME_OBJECT
            || field.kind == ManagedValueKind::STRUCTURED || field.kind == ManagedValueKind::ASSET_REFERENCE
            || managed_detail::IsArray(field.kind)) {
            input.push_back('\0');
            input.append(text);
        }
        request.input = input.data();
        request.inputBytes = static_cast<uint32_t>(input.size());
        ManagedBridgeReply reply;
        m_runtime.DispatchPrepared(*this, request, reply);
    }
    reflector.EndField();
    return true;
}

} /// @note namespace fbzz::scene
