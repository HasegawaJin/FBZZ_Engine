/// @file    ManagedScriptRuntime.cpp
/// @brief   .NET Host の起動、世代の切り替えとスクリプト個体の所有。
/// @author  Hasegawa Jin
/// @date    2026-10-04
#include "ManagedScriptRuntimeState.hpp"
#include <Engine/Scene/Managed/Runtime/ManagedScriptAdapter.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Scripting/ScriptComponent.hpp>
#include <Engine/Scene/Scripting/ScriptFactory.hpp>
#include <Engine/Scene/Scripting/ScriptValidation.hpp>
#include <Engine/Scene/Scripting/ScriptEvent.hpp>
#include "../Objects/ManagedObjectControls.hpp"
#include <Engine/Scene/Serialization/TomlReflector.hpp>
#include <Engine/Core/Diagnostics/Logger.hpp>
#include "../Interop/ManagedFieldCodec.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <coreclr_delegates.h>
#include <hostfxr.h>
#include <nethost.h>

namespace fbzz::scene {
namespace {

using managed_detail::GIZMO_CALLBACK_MASK;
using managed_detail::InstanceRequest;
using managed_detail::LIFECYCLE_CALLBACK_MASK;
using managed_detail::TYPE_GIZMO_METADATA_FLAG;
using managed_detail::TYPE_LIFECYCLE_METADATA_FLAG;

constexpr std::string_view kTypePrefix = "cs-internal:";
bool IsReadOperation(ManagedBridgeOp op)
{
    return op == ManagedBridgeOp::GET_TYPE || op == ManagedBridgeOp::GET_FIELD
        || op == ManagedBridgeOp::READ_FIELD || op == ManagedBridgeOp::GET_DIAGNOSTICS
        || op == ManagedBridgeOp::DISCOVER_SOURCE || op == ManagedBridgeOp::GET_API_CATALOG || op == ManagedBridgeOp::CAPTURE_RELOAD
        || op == ManagedBridgeOp::GET_SETTINGS_SNAPSHOT || op == ManagedBridgeOp::GET_SETTINGS_REVISION
        || op == ManagedBridgeOp::GET_PREPARED_SETTINGS || op == ManagedBridgeOp::GET_PREPARED_REVIEW
        || op == ManagedBridgeOp::READ_OBSERVATION || op == ManagedBridgeOp::READ_SETTINGS_PAGE
        || op == ManagedBridgeOp::GET_DATA_ASSET_SCHEMA || op == ManagedBridgeOp::READ_DATA_ASSET_PAGE || op == ManagedBridgeOp::READ_PREPARED_DATA_ASSET;
}

bool RestoreFields(ManagedScriptRuntime& runtime, ManagedScriptAdapter& adapter, const std::vector<ManagedFieldDescriptor>& descriptors,
                   const std::vector<ManagedFieldValue>& fields)
{
    for (const auto& field : fields) {
        const auto descriptor = std::find_if(descriptors.begin(), descriptors.end(),
            [&](const ManagedFieldDescriptor& item) {
                return (item.name == field.name || std::find(item.previousKeys.begin(), item.previousKeys.end(), field.name) != item.previousKeys.end())
                    && (item.flags & kManagedFieldSetting);
            });
        if (descriptor == descriptors.end()) {
            continue;
        }
        if (descriptor->kind != field.value.valueKind) {
            return false;
        }
        auto request = InstanceRequest(ManagedBridgeOp::WRITE_FIELD, adapter.Generation(), adapter.InstanceId(), adapter.OwnerId());
        request.index = static_cast<uint32_t>(field.value.valueKind);
        request.integer = field.value.integer;
        request.x = field.value.x;
        request.y = field.value.y;
        request.z = field.value.z;
        request.w = field.value.w;
        request.flags = field.value.flags & kManagedNullValue;
        std::string input = descriptor->name;
        if (field.value.valueKind == ManagedValueKind::STRING || field.value.valueKind == ManagedValueKind::GAME_OBJECT
            || field.value.valueKind == ManagedValueKind::STRUCTURED || field.value.valueKind == ManagedValueKind::ASSET_REFERENCE
            || managed_detail::IsArray(field.value.valueKind)) {
            input.push_back('\0');
            input.append(field.text);
        }
        request.input = input.data();
        request.inputBytes = static_cast<uint32_t>(input.size());
        ManagedBridgeReply reply;
        if (!runtime.DispatchPrepared(adapter, request, reply)) {
            return false;
        }
    }
    return true;
}

void PreserveUnknownFields(const ManagedScriptAdapter& adapter, std::string_view preservedToml, toml::table& fields)
{
    if (preservedToml.empty()) {
        return;
    }
    const auto preserved = toml::parse(preservedToml);
    if (!preserved) {
        return;
    }
    for (const auto& [key, value] : preserved.table()) {
        const std::string_view savedKey = key.str();
        const bool recognized = std::any_of(adapter.Fields().begin(), adapter.Fields().end(), [&](const ManagedFieldDescriptor& field) {
            return field.name == savedKey || std::find(field.previousKeys.begin(), field.previousKeys.end(), savedKey) != field.previousKeys.end();
        });
        if (!recognized) {
            fields.insert(key, value);
        }
    }
}

bool ReadSavedFields(const SerializedScriptData& saved, const std::vector<ManagedFieldDescriptor>& descriptors,
                     std::vector<ManagedFieldValue>& fields)
{
    if (saved.fieldsToml.empty()) {
        return true;
    }
    auto parsed = toml::parse(saved.fieldsToml);
    if (!parsed) {
        return false;
    }
    for (const auto& descriptor : descriptors) {
        if (!(descriptor.flags & kManagedFieldSetting)) {
            continue;
        }
        const toml::node* node = parsed.table().get(descriptor.name);
        if (!node)
            for (const std::string& previous : descriptor.previousKeys)
                if ((node = parsed.table().get(previous))) {
                    break;
                }
        if (!node) {
            continue;
        }
        ManagedFieldValue field;
        field.name = descriptor.name;
        field.value.valueKind = descriptor.kind;
        if (descriptor.kind == ManagedValueKind::NONE) {
            continue;
        }
        if (!managed_detail::ReadSavedField(*node, field)) {
            return false;
        }
        fields.push_back(std::move(field));
    }
    return true;
}

}

ManagedScriptRuntime* ManagedScriptRuntime::m_liveRuntime = nullptr;

ManagedScriptRuntime::ManagedScriptRuntime()
{
    // facade の Script destructor が購読を解除するため、event bus を Runtime より先に初期化して終了時の寿命を保証する。
    /// @see https://eel.is/c++draft/basic.start.term static オブジェクトは初期化完了の逆順で破棄される。
    (void)ScriptEventBus::SubscriptionCount();
    m_impl = std::make_unique<Impl>();
    m_liveRuntime = this;
}

ManagedScriptRuntime& ManagedScriptRuntime::Instance()
{
    static ManagedScriptRuntime runtime;
    return runtime;
}

/// @note CLR が thread state を破棄した後に呼ばれ得るため、static destruction から managed Dispatch/FreeLibrary は実行しない。
ManagedScriptRuntime::~ManagedScriptRuntime()
{
    if (m_liveRuntime == this) {
        m_liveRuntime = nullptr;
    }
}
void ManagedScriptRuntime::RetireScene(Scene& scene)
{
    if (m_liveRuntime) {
        m_liveRuntime->m_impl->objects.RetireScene(scene);
    }
}
void ManagedScriptRuntime::RetireObject(Scene& scene, EntityID entity)
{
    if (m_liveRuntime) {
        m_liveRuntime->m_impl->objects.RetireObject(scene, entity);
    }
}

bool ManagedScriptRuntime::Initialize(const std::filesystem::path& hostDirectory)
{
    m_impl->collectionPending = false;
    if (std::this_thread::get_id() != m_impl->mainThread) {
        return false;
    }
    if (m_impl->dispatch) {
        return true;
    }
    const auto runtimeConfig = hostDirectory / L"Fbzz.ManagedHost.runtimeconfig.json";
    const auto hostAssembly = hostDirectory / L"Fbzz.ManagedHost.dll";
    std::error_code error;
    if (!std::filesystem::is_regular_file(runtimeConfig, error) || !std::filesystem::is_regular_file(hostAssembly, error)) {
        m_impl->diagnostic = "Managed Host DLL/runtimeconfig was not found in " + hostDirectory.generic_string();
        return false;
    }
    size_t pathLength = 0;
    get_hostfxr_path(nullptr, &pathLength, nullptr);
    if (!pathLength) {
        m_impl->diagnostic = "Installed .NET hostfxr could not be located.";
        return false;
    }
    std::vector<char_t> hostPath(pathLength);
    if (get_hostfxr_path(hostPath.data(), &pathLength, nullptr) != 0) {
        m_impl->diagnostic = "Installed .NET hostfxr path lookup failed.";
        return false;
    }
    if (!m_impl->hostModule) {
        m_impl->hostModule = LoadLibraryW(hostPath.data());
    }
    if (!m_impl->hostModule) {
        m_impl->diagnostic = "LoadLibrary failed for .NET hostfxr.";
        return false;
    }
    /// @see https://learn.microsoft.com/en-us/dotnet/core/tutorials/netcore-hosting hostfxr lifetime と runtime delegate。
    const auto initialize = std::bit_cast<hostfxr_initialize_for_runtime_config_fn>(GetProcAddress(m_impl->hostModule, "hostfxr_initialize_for_runtime_config"));
    const auto getDelegate = std::bit_cast<hostfxr_get_runtime_delegate_fn>(GetProcAddress(m_impl->hostModule, "hostfxr_get_runtime_delegate"));
    const auto close = std::bit_cast<hostfxr_close_fn>(GetProcAddress(m_impl->hostModule, "hostfxr_close"));
    if (!initialize || !getDelegate || !close) {
        m_impl->diagnostic = "Installed hostfxr does not export the required hosting functions.";
        return false;
    }
    hostfxr_handle context = nullptr;
    const int initialized = initialize(runtimeConfig.c_str(), nullptr, &context);
    if (initialized < 0 || !context) {
        m_impl->diagnostic = "hostfxr rejected the managed runtime configuration (" + std::to_string(initialized) + ").";
        if (context) {
            close(context);
        }
        return false;
    }
    void* delegate = nullptr;
    const int delegated = getDelegate(context, hdt_load_assembly_and_get_function_pointer, &delegate);
    close(context);
    if (delegated < 0 || !delegate) {
        m_impl->diagnostic = "hostfxr could not acquire the assembly loading delegate.";
        return false;
    }
    const auto loadAssembly = std::bit_cast<load_assembly_and_get_function_pointer_fn>(delegate);
    void* entry = nullptr;
    const int loaded = loadAssembly(hostAssembly.c_str(), L"Fbzz.ManagedHost.EntryPoint, Fbzz.ManagedHost",
        L"Dispatch", UNMANAGEDCALLERSONLY_METHOD, nullptr, &entry);
    if (loaded < 0 || !entry) {
        m_impl->diagnostic = "The managed Host Dispatch entry could not be loaded (" + std::to_string(loaded) + ").";
        return false;
    }
    m_impl->dispatch = std::bit_cast<ManagedBridgeDispatch>(entry);
    ManagedBridgeRequest request;
    request.generation = kManagedBridgeSchemaHash;
    request.integer = std::bit_cast<int64_t>(&ManagedScriptRuntime::NativeDispatch);
    request.index = sizeof(ManagedBridgeRequest);
    request.flags = sizeof(ManagedBridgeReply);
    ManagedBridgeReply reply;
    if (!Dispatch(request, reply) || reply.generation != kManagedBridgeSchemaHash) {
        m_impl->dispatch = nullptr;
        if (m_impl->diagnostic.empty()) {
            m_impl->diagnostic = "Managed Host ABI schema did not match.";
        }
        return false;
    }
    m_impl->hostDirectory = hostDirectory;
    m_impl->diagnostic.clear();
    return true;
}

bool ManagedScriptRuntime::Dispatch(ManagedBridgeRequest request, ManagedBridgeReply& reply, std::string* output)
{
    if (!m_impl->dispatch || std::this_thread::get_id() != m_impl->mainThread) {
        reply.status = ManagedBridgeStatus::INVALID_SCOPE;
        return false;
    }
    // 不正な callback 要求は CLR へ入る前に拒否し、ユーザーの部分的変更や Host だけの fault を作らない。
    if (request.op == ManagedBridgeOp::INVOKE && (!m_impl->scope.callId || !request.callId
        || request.callId != m_impl->scope.callId || request.generation != m_impl->scope.generation
        || request.instanceId != m_impl->scope.instanceId || request.ownerId != m_impl->scope.ownerId
        || request.index != static_cast<uint32_t>(m_impl->scope.callback))) {
        reply = {}; reply.status = ManagedBridgeStatus::INVALID_SCOPE;
        m_impl->diagnostic = "Managed callback request does not match the active native invocation scope.";
        if (output) {
            output->clear();
        }
        return false;
    }
    // 直接の ABI 操作も成功記録を失効させる。失敗しても再検証し、ownerId 省略の解放も個体 ID で扱う。
    if (request.op == ManagedBridgeOp::BIND_OWNER || request.op == ManagedBridgeOp::RELEASE_INSTANCE) {
        m_impl->ownerBindings.erase(request.instanceId);
    }
    else if (request.op == ManagedBridgeOp::RETIRE_GENERATION)
        std::erase_if(m_impl->ownerBindings, [&request](const auto& item) { return item.second.generation == request.generation; });
    else if (request.op == ManagedBridgeOp::SHUTDOWN || request.op == ManagedBridgeOp::INITIALIZE)
        m_impl->ownerBindings.clear();
    const auto invalidateEmptyCallbackSkip = [&] {
        const auto found = m_impl->adapters.find(request.ownerId);
        if (found != m_impl->adapters.end() && found->second->Generation() == request.generation
            && found->second->InstanceId() == request.instanceId) {
            m_impl->failedCallbackInstances.insert(request.instanceId);
        }
    };
    std::array<char, 8192> buffer;
    std::vector<char> expanded;
    char* data = buffer.data();
    uint32_t capacity = static_cast<uint32_t>(buffer.size());
    for (;;) {
        request.output = data;
        request.outputCapacity = capacity;
        reply = {};
        const auto status = static_cast<ManagedBridgeStatus>(m_impl->dispatch(&request, &reply));
        if (status != reply.status) {
            invalidateEmptyCallbackSkip();
            reply.status = ManagedBridgeStatus::ABI_MISMATCH;
            m_impl->diagnostic = "Managed Host returned inconsistent status values.";
            return false;
        }
        if (status == ManagedBridgeStatus::BUFFER_TOO_SMALL && IsReadOperation(request.op)
            && reply.outputBytes > capacity) {
            expanded.resize(reply.outputBytes);
            data = expanded.data();
            capacity = static_cast<uint32_t>(expanded.size());
            continue;
        }
        if (reply.outputBytes > capacity) {
            invalidateEmptyCallbackSkip();
            m_impl->diagnostic = "Managed Host output exceeded the caller buffer.";
            return false;
        }
        if (output) {
            if (reply.outputBytes) {
                output->assign(data, reply.outputBytes);
            }
            else {
                output->clear();
            }
        }
        if (status != ManagedBridgeStatus::OK) {
            invalidateEmptyCallbackSkip();
            m_impl->diagnostic = reply.outputBytes ? std::string(data, reply.outputBytes)
                : "Managed Host status " + std::to_string(static_cast<int>(status));
            if (status != ManagedBridgeStatus::NOT_FOUND && status != ManagedBridgeStatus::COLLECTION_PENDING)
                FBZZ_LOG_ERROR("Managed Script: %s", m_impl->diagnostic.c_str());
            return false;
        }
        if (request.op == ManagedBridgeOp::RELEASE_INSTANCE) {
            m_impl->failedCallbackInstances.erase(request.instanceId);
        }
        return true;
    }
}

bool ManagedScriptRuntime::IsLoaded() const { return m_impl->generation != 0; }
bool ManagedScriptRuntime::IsCollectionPending() const { return m_impl->collectionPending; }
const std::string& ManagedScriptRuntime::LastDiagnostic() const { return m_impl->diagnostic; }
bool ManagedScriptRuntime::IsDispatchIdle() const
{
    return std::this_thread::get_id() == m_impl->mainThread && !m_impl->scope.callId && !m_impl->preparation && m_impl->settingsPreparation.empty();
}
uint64_t ManagedScriptRuntime::GetAppliedGeneration() const { return m_impl->generation; }
std::string ManagedScriptRuntime::GetTypeSourceName(std::string_view typeKey) const
{
    const auto found = m_impl->typeSourceNames.find(std::string(typeKey));
    return found == m_impl->typeSourceNames.end() ? std::string(typeKey) : found->second;
}

std::string ManagedScriptRuntime::GetTypeSourcesJson(std::string_view typeKey) const
{
    const auto found = m_impl->typeSources.find(std::string(typeKey));
    return found == m_impl->typeSources.end() ? "[]" : found->second;
}

ManagedScriptAdapter* ManagedScriptRuntime::FindAdapter(const Script* script) const
{
    const auto found = m_impl->adapterLookup.find(script);
    return found == m_impl->adapterLookup.end() ? nullptr : found->second;
}

std::string ManagedScriptRuntime::GetTypeSchemaJson(std::string_view typeKey) const
{
    const auto schema = m_impl->typeSchemas.find(std::string(typeKey));
    return schema == m_impl->typeSchemas.end() ? std::string{} : schema->second;
}

bool ManagedScriptRuntime::DiscoverSourceCatalog(const std::filesystem::path& assemblyPath, std::string& sourceJson)
{
    std::error_code error;
    const auto absolute = std::filesystem::absolute(assemblyPath, error);
    if (error) {
        return false;
    }
    const auto utf8 = absolute.u8string();
    const std::string input(utf8.begin(), utf8.end());
    ManagedBridgeRequest request;
    request.op = ManagedBridgeOp::DISCOVER_SOURCE;
    request.input = input.data();
    request.inputBytes = static_cast<uint32_t>(input.size());
    ManagedBridgeReply reply;
    return Dispatch(request, reply, &sourceJson);
}

bool ManagedScriptRuntime::GetApiCatalog(std::string& json)
{
    ManagedBridgeRequest request;
    request.op = ManagedBridgeOp::GET_API_CATALOG;
    ManagedBridgeReply reply;
    return Dispatch(request, reply, &json);
}

bool ManagedScriptRuntime::BindOwner(ManagedScriptAdapter& adapter)
{
    if (std::this_thread::get_id() != m_impl->mainThread) {
        return false;
    }
    if (adapter.m_boundScene != adapter.m_scene || adapter.m_boundObject != adapter.m_gameObject) {
        m_impl->servicesBridge.CancelCursorOwner(adapter.OwnerId());
        adapter.m_boundScene = adapter.m_scene;
        adapter.m_boundObject = adapter.m_gameObject;
        adapter.m_contextSceneGeneration = 0;
        adapter.m_contextEntity = EntityID::INVALID;
        adapter.m_objectId = 0;
        if (adapter.m_scene && adapter.m_gameObject) {
            // 非所有 pointer を dereference せず、Scene 所有物との一致を先に確認する。
            for (GameObject& object : adapter.m_scene->GameObjects()) {
                if (&object != adapter.m_gameObject) {
                    continue;
                }
                adapter.m_contextSceneGeneration = adapter.m_scene->GetRenderSceneGeneration();
                adapter.m_contextEntity = object.GetID();
                adapter.m_objectId = m_impl->RegisterObject(*adapter.m_scene, object);
                break;
            }
        }
    }
    GameObject* object = adapter.HasLiveContext() ? adapter.m_scene->GetGameObject(adapter.m_contextEntity) : nullptr;
    const bool cacheable = object && m_impl->dispatch && !m_impl->scope.callId && adapter.Generation() == m_impl->generation;
    if (cacheable) {
        const auto found = m_impl->ownerBindings.find(adapter.InstanceId());
        if (found != m_impl->ownerBindings.end()) {
            const auto& cached = found->second;
            if (cached.generation == adapter.Generation() && cached.ownerId == adapter.OwnerId()
                && cached.sceneLifetime == adapter.m_contextSceneGeneration && cached.objectId == adapter.m_objectId
                && cached.entity == adapter.m_contextEntity && cached.guid == object->instanceId
                && cached.scriptBirth == adapter.m_referenceBirth) {
                return true;
            }
        }
    }
    const std::string guid = object ? object->instanceId : std::string{};
    const std::string binding = guid + '\n' + adapter.m_referenceBirth;
    auto request = InstanceRequest(ManagedBridgeOp::BIND_OWNER, adapter.Generation(), adapter.InstanceId(), adapter.OwnerId());
    request.integer = object ? std::bit_cast<int64_t>(adapter.m_objectId) : 0;
    request.callId = object ? adapter.m_contextSceneGeneration : 0;
    request.input = binding.data();
    request.inputBytes = static_cast<uint32_t>(binding.size());
    ManagedBridgeReply reply;
    const bool succeeded = Dispatch(request, reply);
    if (succeeded && cacheable) {
        const auto callbacks = m_impl->lifecycleCallbacks.find(adapter.m_typeName);
        const uint32_t presence = callbacks == m_impl->lifecycleCallbacks.end() ? 0U : callbacks->second;
        m_impl->ownerBindings.insert_or_assign(adapter.InstanceId(), Impl::OwnerBinding{adapter.Generation(), adapter.OwnerId(),
            adapter.m_contextSceneGeneration, adapter.m_objectId, adapter.m_contextEntity, guid, adapter.m_referenceBirth, presence});
    }
    return succeeded;
}

uint64_t ManagedScriptRuntime::ReferenceObjectId(Scene& scene, EntityID entity)
{
    if (std::this_thread::get_id() != m_impl->mainThread) {
        return 0;
    }
    GameObject* object = scene.GetGameObject(entity);
    return object ? m_impl->RegisterObject(scene, *object) : 0;
}

uint64_t ManagedScriptRuntime::ReferenceComponentLifetime(Scene& scene, EntityID entity, uint32_t kind)
{
    if (std::this_thread::get_id() != m_impl->mainThread) {
        return 0;
    }
    GameObject* object = scene.GetGameObject(entity);
    if (!object || managed_detail::ManagedObjectControls::IsPendingComponentRemoval(*object, kind)) {
        return 0;
    }
    bool exists = false; uint64_t lifetime = 0;
    if (managed_detail::ManagedObjectControls::Component(*object, kind, ManagedBridgeOp::COMPONENT_READ, exists, &lifetime) != ManagedBridgeStatus::OK || !exists) {
        return 0;
    }
    return kind == 0 ? m_impl->RegisterObject(scene, *object) : lifetime;
}

bool ManagedScriptRuntime::DispatchPrepared(ManagedScriptAdapter& adapter, ManagedBridgeRequest request,
                                          ManagedBridgeReply& reply, std::string* output)
{
    if (request.op != ManagedBridgeOp::WRITE_FIELD && request.op != ManagedBridgeOp::VALIDATE_FIELDS
        && request.op != ManagedBridgeOp::WRITE_FIELDS && request.op != ManagedBridgeOp::VALIDATE_REFERENCES
        && request.op != ManagedBridgeOp::RESTORE_RELOAD && request.op != ManagedBridgeOp::RESUME_INSTANCE
        && request.op != ManagedBridgeOp::RESOLVE_SAVED_COMPONENT_REFERENCES) {
        reply.status = ManagedBridgeStatus::INVALID_ARGUMENT;
        return false;
    }
    if (!BindOwner(adapter)) {
        return false;
    }
    auto* previous = m_impl->preparation;
    m_impl->preparation = &adapter;
    request.generation = adapter.Generation();
    request.instanceId = adapter.InstanceId();
    request.ownerId = adapter.OwnerId();
    request.callId = 0;
    const bool succeeded = Dispatch(request, reply, output);
    m_impl->preparation = previous;
    return succeeded;
}

bool ManagedScriptRuntime::DispatchPreparedSettings(const std::vector<ManagedScriptAdapter*>& adapters,
    ManagedBridgeRequest request, ManagedBridgeReply& reply)
{
    if (!IsDispatchIdle() || (request.op != ManagedBridgeOp::PREPARE_SETTINGS && request.op != ManagedBridgeOp::PREPARE_SETTINGS_EDIT
        && request.op != ManagedBridgeOp::PREPARE_SETTINGS_ADOPTION) || adapters.empty()) {
        m_impl->diagnostic = "Settings preparation requires idle main-thread scope and a nonempty batch of exact targets.";
        reply.status = ManagedBridgeStatus::INVALID_SCOPE;
        return false;
    }
    std::unordered_set<uint64_t> identities;
    for (ManagedScriptAdapter* adapter : adapters) {
        if (!adapter || adapter->Generation() != request.generation || !identities.insert(adapter->InstanceId()).second || !BindOwner(*adapter)) {
            if (m_impl->diagnostic.empty()) {
                m_impl->diagnostic = "Settings preparation target identity is invalid or duplicated.";
            }
            reply.status = ManagedBridgeStatus::INVALID_ARGUMENT;
            return false;
        }
    }
    m_impl->settingsPreparation = adapters;
    const bool succeeded = Dispatch(request, reply);
    m_impl->settingsPreparation.clear();
    return succeeded;
}

bool ManagedScriptRuntime::ResolveReference(ManagedScriptAdapter& adapter, ScriptObjectReference& reference)
{
    reference.id = EntityID::INVALID;
    reference.sceneGeneration = 0;
    if (reference.guid.empty() || !adapter.HasLiveContext()) {
        return true;
    }
    reference.sceneGeneration = adapter.m_scene->GetRenderSceneGeneration();
    GameObject* match = nullptr;
    for (GameObject& object : adapter.m_scene->GameObjects()) {
        if (object.instanceId != reference.guid) {
            continue;
        }
        if (match) { m_impl->diagnostic = "GameObject GUID is ambiguous: " + reference.guid; return false; }
        match = &object;
    }
    if (match) {
        reference.id = match->GetID();
    }
    return true;
}

bool ManagedScriptRuntime::ResolveObjectId(ManagedScriptAdapter& adapter, uint64_t objectId, ScriptObjectReference& reference)
{
    reference.id = EntityID::INVALID;
    reference.sceneGeneration = 0;
    if (!adapter.HasLiveContext()) {
        return false;
    }
    reference.sceneGeneration = adapter.m_scene->GetRenderSceneGeneration();
    GameObject* object = m_impl->ResolveObject(*adapter.m_scene, objectId);
    if (object && object->instanceId == reference.guid) {
        reference.id = object->GetID();
    }
    return true;
}

bool ManagedScriptRuntime::RestoreSettingsToml(ManagedScriptAdapter& adapter, std::string_view fieldsToml)
{
    SerializedScriptData saved{adapter.GetTypeName(), std::string(fieldsToml), adapter.enabled};
    std::vector<ManagedFieldValue> values;
    if (!ReadSavedFields(saved, adapter.Fields(), values)) {
        m_impl->diagnostic = "Managed saved fields do not match the candidate schema.";
        return false;
    }
    return RestoreFields(*this, adapter, adapter.Fields(), values);
}

bool ManagedScriptRuntime::RebindSceneReferences(Scene& scene)
{
    for (EntityID entity : scene.GetEntities<ScriptComponent>()) {
        auto* component = scene.GetComponent<ScriptComponent>(entity);
        if (!component) {
            continue;
        }
        for (auto& entry : component->scripts) {
            auto* adapter = FindAdapter(entry.script.get());
            if (!adapter) {
                continue;
            }
            if (!BindOwner(*adapter)) {
                return false;
            }
            std::vector<ManagedFieldValue> values;
            if (!adapter->CaptureFields(values)) {
                return false;
            }
            std::erase_if(values, [](const ManagedFieldValue& value) {
                return value.value.valueKind != ManagedValueKind::GAME_OBJECT && value.value.valueKind != ManagedValueKind::GAME_OBJECT_ARRAY
                    && value.value.valueKind != ManagedValueKind::STRUCTURED;
            });
            if (!RestoreFields(*this, *adapter, adapter->Fields(), values)) {
                return false;
            }
        }
    }
    for (EntityID entity : scene.GetEntities<ScriptComponent>())
        if (auto* component = scene.GetComponent<ScriptComponent>(entity))
            for (auto& entry : component->scripts)
                if (auto* adapter = FindAdapter(entry.script.get()); adapter && !adapter->ResolveSavedComponentReferences()) {
                    return false;
                }
    return true;
}

bool ManagedScriptRuntime::LoadCandidate(const std::filesystem::path& path, uint64_t& generation,
                                       std::vector<std::string>& types, bool internalMode)
{
    m_impl->collectionPending = false;
    if (!m_impl->dispatch) { m_impl->diagnostic = "Managed Host has not been initialized."; return false; }
    std::error_code error;
    const auto absolute = std::filesystem::absolute(path, error);
    if (error || !std::filesystem::is_regular_file(absolute, error)) {
        m_impl->diagnostic = "Managed game assembly was not found: " + path.generic_string();
        return false;
    }
    const auto utf8Path = absolute.u8string();
    const std::string input(utf8Path.begin(), utf8Path.end());
    ManagedBridgeRequest request;
    request.op = ManagedBridgeOp::LOAD_CANDIDATE;
    request.generation = m_impl->nextGeneration++;
    request.flags = internalMode ? 1U : 0U;
    request.input = input.data();
    request.inputBytes = static_cast<uint32_t>(input.size());
    ManagedBridgeReply reply;
    if (!Dispatch(request, reply)) {
        m_impl->collectionPending = reply.status == ManagedBridgeStatus::COLLECTION_PENDING;
        return false;
    }
    generation = reply.generation;
    if (!generation || generation != request.generation) {
        m_impl->diagnostic = "Managed Host returned an invalid candidate generation.";
        return false;
    }
    m_impl->candidateExecuteAlways.clear();
    m_impl->candidateTypeSourceNames.clear();
    m_impl->candidateTypeSources.clear();
    m_impl->candidateTypeSchemas.clear();
    m_impl->candidateExecutionOrders.clear();
    m_impl->candidateRequiredComponents.clear();
    m_impl->candidateGizmoCallbacks.clear();
    m_impl->candidateLifecycleCallbacks.clear();
    uint32_t declaredTypes = 0;
    for (uint32_t index = 0;; ++index) {
        request = {};
        request.op = ManagedBridgeOp::GET_TYPE;
        request.generation = generation;
        request.index = index;
        std::string type;
        if (!Dispatch(request, reply, &type)) {
            if (reply.status == ManagedBridgeStatus::NOT_FOUND && index == 0 && reply.index == 0) {
                break;
            }
            Retire(generation);
            return false;
        }
        if (index == 0) {
            declaredTypes = reply.index;
        }
        // 件数は Host の Int32 リスト表現に由来し、型の総数を制限する仕様値ではない。検証済みの終端まで、すべての応答で件数が一致する必要がある。
        if (type.empty() || declaredTypes == 0 || declaredTypes > static_cast<uint32_t>(std::numeric_limits<int>::max())
            || reply.index != declaredTypes || index >= declaredTypes) { Retire(generation); return false; }
        if (!type.starts_with("cs:") && !type.starts_with(kTypePrefix)) {
            m_impl->diagnostic = "Managed Script type key conflicts with an existing registration: " + type;
            const std::string diagnostic = m_impl->diagnostic;
            Retire(generation);
            m_impl->diagnostic = diagnostic;
            return false;
        }
        types.push_back(type);
        m_impl->candidateExecuteAlways.insert_or_assign(type, (reply.flags & 1U) != 0);
        // GET_TYPE flags=0 の bit 1 は gizmo presence の対応を表し、Integer の bit 0/1 が通常/選択 callback。
        if ((reply.flags & TYPE_GIZMO_METADATA_FLAG) != 0) {
            if (reply.integer < 0 || reply.integer > GIZMO_CALLBACK_MASK) { Retire(generation); return false; }
            m_impl->candidateGizmoCallbacks.emplace(type, static_cast<uint32_t>(reply.integer));
        }
        if ((reply.flags & TYPE_LIFECYCLE_METADATA_FLAG) != 0) {
            m_impl->candidateLifecycleCallbacks.emplace(type, reply.flags & (TYPE_LIFECYCLE_METADATA_FLAG | LIFECYCLE_CALLBACK_MASK));
        }
        const uint32_t count = reply.index;
        request.flags = 1;
        std::string sourceName;
        if (!Dispatch(request, reply, &sourceName) || reply.index != declaredTypes || sourceName.empty()) {
            Retire(generation);
            return false;
        }
        m_impl->candidateTypeSourceNames.emplace(type, std::move(sourceName));
        request.flags = 2;
        std::string sourceJson;
        if (!Dispatch(request, reply, &sourceJson) || reply.index != declaredTypes) { Retire(generation); return false; }
        m_impl->candidateTypeSources.emplace(type, std::move(sourceJson));
        request.flags = 3;
        std::string schemaJson;
        if (!Dispatch(request, reply, &schemaJson) || reply.index != declaredTypes) { Retire(generation); return false; }
        m_impl->candidateTypeSchemas.emplace(type, std::move(schemaJson));
        request.flags = 4;
        std::string requirements;
        if (!Dispatch(request, reply, &requirements) || reply.index != declaredTypes || reply.integer < std::numeric_limits<int>::min()
            || reply.integer > std::numeric_limits<int>::max()) { Retire(generation); return false; }
        m_impl->candidateExecutionOrders.emplace(type, static_cast<int>(reply.integer));
        std::vector<std::string> required;
        std::istringstream lines(requirements);
        for (std::string line; std::getline(lines, line);) {
            if (!line.empty()) {
                required.push_back(std::move(line));
            }
        }
        m_impl->candidateRequiredComponents.emplace(type, std::move(required));
        if (index + 1 >= count) {
            break;
        }
    }
    request = {};
    request.op = ManagedBridgeOp::GET_DIAGNOSTICS;
    request.generation = generation;
    std::string diagnostics;
    if (!Dispatch(request, reply, &diagnostics)) { Retire(generation); return false; }
    m_impl->diagnostic = std::move(diagnostics);
    m_impl->candidateAssemblyPath = absolute;
    m_impl->candidateInternalMode = internalMode;
    return true;
}

bool ManagedScriptRuntime::Publish(uint64_t generation, const std::vector<std::string>& types)
{
    for (const auto& type : types) {
        const auto name = m_impl->candidateTypeSourceNames.find(type);
        if (name == m_impl->candidateTypeSourceNames.end()) {
            m_impl->diagnostic = "Managed candidate registration conflicts with an existing type: " + type;
            return false;
        }
    }
    const auto registerType = [this](const std::string& type, const std::string& sourceName) {
        return ScriptFactory::RegisterManaged(type, [this, type](const std::string& savedScriptId) { return CreateAdapter(type, savedScriptId); })
            && ScriptFactory::SetDisplayName(type, sourceName)
            && ScriptFactory::RegisterAlias(std::string(kTypePrefix) + sourceName, type);
    };
    ScriptFactory::UnregisterManaged();
    for (const auto& type : types) {
        if (!registerType(type, m_impl->candidateTypeSourceNames.at(type))) {
            ScriptFactory::UnregisterManaged();
            for (const auto& previous : m_impl->types)
                if (!registerType(previous, m_impl->typeSourceNames.at(previous)))
                    FBZZ_LOG_ERROR("Managed previous factory registration could not be restored: %s", previous.c_str());
            m_impl->diagnostic = "Managed candidate registration failed: " + type;
            return false;
        }
    }
    m_impl->generation = generation;
    m_impl->types = types;
    m_impl->executeAlways = m_impl->candidateExecuteAlways;
    m_impl->typeSourceNames = m_impl->candidateTypeSourceNames;
    m_impl->typeSources = m_impl->candidateTypeSources;
    m_impl->typeSchemas = m_impl->candidateTypeSchemas;
    m_impl->executionOrders = m_impl->candidateExecutionOrders;
    m_impl->requiredComponents = m_impl->candidateRequiredComponents;
    m_impl->gizmoCallbacks = m_impl->candidateGizmoCallbacks;
    m_impl->lifecycleCallbacks = m_impl->candidateLifecycleCallbacks;
    m_impl->generationMetadata.insert_or_assign(generation, Impl::GenerationMetadata{m_impl->types, m_impl->executeAlways,
        m_impl->typeSourceNames, m_impl->typeSources, m_impl->typeSchemas, m_impl->executionOrders, m_impl->requiredComponents, m_impl->gizmoCallbacks, m_impl->lifecycleCallbacks,
        m_impl->candidateAssemblyPath, m_impl->candidateInternalMode});
    return true;
}

uint64_t ManagedScriptRuntime::RetainAppliedGeneration()
{
    const uint64_t generation = m_impl->generation;
    if (!generation || !IsDispatchIdle()) { m_impl->diagnostic = "Generation retention requires an applied generation at an idle main-thread boundary."; return 0; }
    auto request = InstanceRequest(ManagedBridgeOp::RETAIN_GENERATION, generation, 0, 0);
    ManagedBridgeReply reply;
    if (!Dispatch(request, reply)) {
        return 0;
    }
    ++m_impl->generationLeases[generation];
    return generation;
}

bool ManagedScriptRuntime::ActivateRetainedGeneration(uint64_t generation)
{
    if (!generation || !IsDispatchIdle() || !m_impl->generationLeases.contains(generation)) {
        m_impl->diagnostic = "Generation activation requires an owned retained generation at an idle main-thread boundary.";
        return false;
    }
    const auto found = m_impl->generationMetadata.find(generation);
    if (found == m_impl->generationMetadata.end()) { m_impl->diagnostic = "Retained generation metadata is absent."; return false; }
    if (generation == m_impl->generation) {
        return true;
    }
    const uint64_t previousGeneration = m_impl->generation;
    const auto candidateAlways = std::move(m_impl->candidateExecuteAlways);
    const auto candidateNames = std::move(m_impl->candidateTypeSourceNames);
    const auto candidateSources = std::move(m_impl->candidateTypeSources);
    const auto candidateSchemas = std::move(m_impl->candidateTypeSchemas);
    const auto candidateOrders = std::move(m_impl->candidateExecutionOrders);
    const auto candidateRequired = std::move(m_impl->candidateRequiredComponents);
    const auto candidateGizmos = std::move(m_impl->candidateGizmoCallbacks);
    const auto candidateCallbacks = std::move(m_impl->candidateLifecycleCallbacks);
    const auto candidatePath = std::move(m_impl->candidateAssemblyPath);
    const bool candidateInternalMode = m_impl->candidateInternalMode;
    const auto metadata = found->second;
    m_impl->candidateExecuteAlways = metadata.executeAlways; m_impl->candidateTypeSourceNames = metadata.sourceNames;
    m_impl->candidateTypeSources = metadata.sources; m_impl->candidateTypeSchemas = metadata.schemas;
    m_impl->candidateExecutionOrders = metadata.executionOrders; m_impl->candidateRequiredComponents = metadata.requiredComponents;
    m_impl->candidateGizmoCallbacks = metadata.gizmoCallbacks;
    m_impl->candidateLifecycleCallbacks = metadata.lifecycleCallbacks;
    m_impl->candidateAssemblyPath = metadata.assemblyPath; m_impl->candidateInternalMode = metadata.internalMode;
    const bool activated = Publish(generation, metadata.types);
    m_impl->candidateExecuteAlways = candidateAlways; m_impl->candidateTypeSourceNames = candidateNames;
    m_impl->candidateTypeSources = candidateSources; m_impl->candidateTypeSchemas = candidateSchemas;
    m_impl->candidateExecutionOrders = candidateOrders; m_impl->candidateRequiredComponents = candidateRequired;
    m_impl->candidateGizmoCallbacks = candidateGizmos;
    m_impl->candidateLifecycleCallbacks = candidateCallbacks;
    m_impl->candidateAssemblyPath = candidatePath; m_impl->candidateInternalMode = candidateInternalMode;
    if (activated) {
        Retire(previousGeneration);
    }
    return activated;
}

bool ManagedScriptRuntime::ForkRetainedGenerationForPlay(uint64_t originalGeneration)
{
    if (!IsDispatchIdle() || originalGeneration != m_impl->generation || !m_impl->generationLeases.contains(originalGeneration)) {
        m_impl->diagnostic = "Play generation fork requires the active retained Edit generation at an idle boundary.";
        return false;
    }
    const auto found = m_impl->generationMetadata.find(originalGeneration);
    if (found == m_impl->generationMetadata.end() || found->second.assemblyPath.empty()) {
        m_impl->diagnostic = "Retained Edit assembly metadata is unavailable.";
        return false;
    }
    const auto assemblyPath = found->second.assemblyPath;
    const bool internalMode = found->second.internalMode;
    uint64_t generation = 0;
    std::vector<std::string> types;
    if (!LoadCandidate(assemblyPath, generation, types, internalMode)) {
        return false;
    }
    if (!Publish(generation, types)) { Retire(generation); return false; }
    return true;
}

void ManagedScriptRuntime::ReleaseRetainedGeneration(uint64_t generation)
{
    if (!IsDispatchIdle()) { m_impl->diagnostic = "Generation lease release requires an idle main-thread boundary."; return; }
    const auto lease = m_impl->generationLeases.find(generation);
    if (lease == m_impl->generationLeases.end()) { m_impl->diagnostic = "Generation has no owned native lease to release."; return; }
    auto request = InstanceRequest(ManagedBridgeOp::RELEASE_GENERATION, generation, 0, 0);
    ManagedBridgeReply reply;
    if (!Dispatch(request, reply)) {
        return;
    }
    if (--lease->second == 0) {
        m_impl->generationLeases.erase(lease);
    }
    Retire(generation);
}

bool ManagedScriptRuntime::Load(const std::filesystem::path& gameAssemblyPath, bool internalMode)
{
    m_impl->collectionPending = false;
    if (IsLoaded()) { m_impl->diagnostic = "Use Reload with the active Scene to replace a managed assembly."; return false; }
    uint64_t generation = 0;
    std::vector<std::string> types;
    if (!LoadCandidate(gameAssemblyPath, generation, types, internalMode)) {
        return false;
    }
    if (!Publish(generation, types)) { Retire(generation); return false; }
    return true;
}

std::unique_ptr<Script> ManagedScriptRuntime::CreateAdapter(const std::string& typeName, const std::string& savedScriptId)
{
    const uint64_t owner = m_impl->nextOwner++;
    auto request = InstanceRequest(ManagedBridgeOp::CREATE, m_impl->generation, 0, owner);
    const std::string creation = typeName + '\n' + savedScriptId;
    request.input = creation.data();
    request.inputBytes = static_cast<uint32_t>(creation.size());
    ManagedBridgeReply reply;
    if (!Dispatch(request, reply)) {
        return nullptr;
    }
    auto adapter = std::make_unique<ManagedScriptAdapter>(*this, typeName,
        m_impl->generation, reply.instanceId, owner, m_impl->executeAlways[typeName], savedScriptId);
    adapter->m_executionOrder = m_impl->executionOrders.at(typeName);
    adapter->m_requiredComponents = m_impl->requiredComponents.at(typeName);
    m_impl->adapters.emplace(owner, adapter.get());
    m_impl->adapterLookup.emplace(adapter.get(), adapter.get());
    if (!adapter->InitializeFields()) {
        return nullptr;
    }
    return adapter;
}

void ManagedScriptRuntime::Retire(uint64_t generation)
{
    if (!generation || generation == m_impl->generation) {
        return;
    }
    if (m_impl->generationLeases.contains(generation) || std::any_of(m_impl->adapters.begin(), m_impl->adapters.end(),
        [generation](const auto& item) { return item.second->Generation() == generation; })) {
        m_impl->deferredRetirements.insert(generation);
        return;
    }
    m_impl->servicesBridge.RetireGeneration(generation);
    m_impl->batchTransfers.RetireGeneration(generation);
    ManagedBridgeRequest request;
    request.op = ManagedBridgeOp::RETIRE_GENERATION;
    request.generation = generation;
    ManagedBridgeReply reply;
    if (Dispatch(request, reply)) { m_impl->deferredRetirements.erase(generation); m_impl->generationMetadata.erase(generation); }
}

void ManagedScriptRuntime::Release(ManagedScriptAdapter& adapter)
{
    m_impl->ownerBindings.erase(adapter.InstanceId());
    ReleaseOwnedObjects(adapter);
    const uint64_t generation = adapter.m_generation;
    const uint64_t instance = adapter.m_instanceId;
    if (adapter.m_instanceId) {
        auto request = InstanceRequest(ManagedBridgeOp::RELEASE_INSTANCE, adapter.m_generation, adapter.m_instanceId, adapter.m_ownerId);
        ManagedBridgeReply reply;
        Dispatch(request, reply);
        adapter.m_instanceId = 0;
    }
    m_impl->failedCallbackInstances.erase(instance);
    m_impl->adapters.erase(adapter.m_ownerId);
    m_impl->adapterLookup.erase(&adapter);
    if (generation != m_impl->generation && m_impl->deferredRetirements.contains(generation)
        && std::none_of(m_impl->adapters.begin(), m_impl->adapters.end(),
            [generation](const auto& item) { return item.second->m_generation == generation; })) {
        Retire(generation);
    }
}

void ManagedScriptRuntime::ReleaseOwnedObjects(ManagedScriptAdapter& adapter)
{
    m_impl->servicesBridge.CancelOwner(adapter.OwnerId(), adapter.HasLiveContext() ? &adapter : nullptr);
    m_impl->batchTransfers.CancelOwner(adapter.OwnerId());
    const auto found = m_impl->ownedObjects.find(adapter.OwnerId());
    if (found == m_impl->ownedObjects.end()) {
        return;
    }
    if (adapter.HasLiveContext() && found->second.scene == adapter.m_scene
        && found->second.lifetime == adapter.m_scene->GetRenderSceneGeneration())
        for (EntityID entity : found->second.entities)
            if (GameObject* object = adapter.m_scene->GetGameObject(entity)) {
                GameObject::Destroy(*object);
            }
    m_impl->ownedObjects.erase(found);
}

bool ManagedScriptRuntime::Reload(Scene& scene, const std::filesystem::path& gameAssemblyPath, bool internalMode)
{
    m_impl->collectionPending = false;
    const bool inPlay = Script::IsInPlayMode();
    if (m_impl->scope.callId) { m_impl->diagnostic = "Managed replacement requires an idle callback boundary."; return false; }
    for (const EntityID entity : scene.GetEntities<ScriptComponent>()) {
        const auto* component = scene.GetComponent<ScriptComponent>(entity);
        std::unordered_set<std::string> savedIds;
        for (const auto& entry : component->scripts) {
            const auto* adapter = FindAdapter(entry.script.get());
            if (!ScriptFactory::IsValidSavedScriptId(entry.savedScriptId) || !savedIds.insert(entry.savedScriptId).second
                || (adapter && adapter->SavedScriptId() != entry.savedScriptId)) {
                m_impl->diagnostic = "Managed reload rejected an invalid, duplicate or mismatched saved Script identity.";
                return false;
            }
        }
    }
    uint64_t generation = 0;
    std::vector<std::string> types;
    if (!LoadCandidate(gameAssemblyPath, generation, types, internalMode)) {
        return false;
    }
    struct Prepared {
        EntityID entity{};
        std::string inspectionId;
        std::string savedScriptId;
        ManagedScriptAdapter* existing = nullptr;
        std::unique_ptr<ManagedScriptAdapter> candidate;
        std::unique_ptr<SerializedScriptData> missing;
        std::unique_ptr<SerializedScriptData> saved;
        std::string reloadState;
    };
    std::vector<Prepared> prepared;
    const auto findAdapter = [&](Script* script) -> ManagedScriptAdapter* {
        for (const auto& [owner, adapter] : m_impl->adapters)
            if (adapter == script) {
                return adapter;
            }
        return nullptr;
    };
    // 利用者のコールバックでスクリプト配列が拡張される場合があるため、コールバックをまたいで保持するのは安定した識別情報だけ。
    const auto resolveEntry = [&](const Prepared& item) -> ScriptEntry* {
        auto* component = scene.GetComponent<ScriptComponent>(item.entity);
        if (!component) {
            return nullptr;
        }
        for (auto& entry : component->scripts)
            if (entry.inspectionId == item.inspectionId && entry.savedScriptId == item.savedScriptId
                && (!item.existing || entry.script.get() == item.existing)) {
                return &entry;
            }
        return nullptr;
    };
    const auto discard = [&] {
        const std::string diagnostic = m_impl->diagnostic;
        for (auto& item : prepared) {
            if (!item.candidate) {
                continue;
            }
            auto request = InstanceRequest(ManagedBridgeOp::RELEASE_INSTANCE, generation,
                item.candidate->m_instanceId, item.candidate->m_ownerId);
            ManagedBridgeReply reply;
            Dispatch(request, reply);
            item.candidate->m_instanceId = 0;
            item.candidate->m_ownerId = 0;
        }
        prepared.clear();
        Retire(generation);
        m_impl->diagnostic = diagnostic;
    };
    for (EntityID entity : scene.GetEntities<ScriptComponent>()) {
        auto* component = scene.GetComponent<ScriptComponent>(entity);
        auto* object = scene.GetGameObject(entity);
        if (!component || !object) {
            continue;
        }
        for (auto& entry : component->scripts) {
            if (entry.m_pendingRemoval) {
                continue;
            }
            auto* existing = findAdapter(entry.script.get());
            const std::string key = existing ? existing->m_typeName : entry.serialized ? entry.serialized->type : std::string{};
            if (!key.starts_with(kTypePrefix) && !key.starts_with("cs:")) {
                continue;
            }
            Prepared item;
            item.entity = entity;
            item.inspectionId = entry.inspectionId;
            item.savedScriptId = entry.savedScriptId;
            item.existing = existing;
            std::string type = key;
            if (key.starts_with(kTypePrefix) && std::find(types.begin(), types.end(), key) == types.end()) {
                const std::string oldName = key.substr(kTypePrefix.size());
                for (const auto& [candidateKey, sourceName] : m_impl->candidateTypeSourceNames)
                    if (sourceName == oldName) { type = candidateKey; break; }
            }
            std::vector<ManagedFieldValue> fields;
            if (existing && !existing->CaptureFields(fields)) { discard(); return false; }
            if (existing) {
                auto capture = InstanceRequest(ManagedBridgeOp::CAPTURE_RELOAD, existing->Generation(), existing->InstanceId(), existing->OwnerId());
                capture.flags = inPlay ? 0 : 1;
                ManagedBridgeReply captured;
                if (!Dispatch(capture, captured, &item.reloadState)) { discard(); return false; }
            }
            if (existing) {
                toml::table table;
                util::TomlWriteReflector writer(table);
                existing->Reflect(writer);
                PreserveUnknownFields(*existing, entry.serialized ? entry.serialized->fieldsToml : std::string{}, table);
                std::ostringstream text;
                text << table;
                item.saved = std::make_unique<SerializedScriptData>(SerializedScriptData{type, text.str(), existing->enabled, entry.savedScriptId});
            }
            if (std::find(types.begin(), types.end(), type) == types.end()) {
                if (existing) {
                    item.missing = std::move(item.saved);
                    item.missing->type = key;
                }
                prepared.push_back(std::move(item));
                continue;
            }
            const uint64_t owner = existing ? existing->m_ownerId : m_impl->nextOwner++;
            auto request = InstanceRequest(ManagedBridgeOp::CREATE, generation, 0, owner);
            const std::string creation = type + '\n' + entry.savedScriptId;
            request.input = creation.data();
            request.inputBytes = static_cast<uint32_t>(creation.size());
            ManagedBridgeReply reply;
            if (!Dispatch(request, reply)) { discard(); return false; }
            item.candidate = std::make_unique<ManagedScriptAdapter>(*this, type, generation, reply.instanceId,
                owner, m_impl->candidateExecuteAlways[type], entry.savedScriptId);
            item.candidate->m_executionOrder = m_impl->candidateExecutionOrders.at(type);
            if (existing) {
                item.candidate->m_referenceBirth = existing->m_referenceBirth;
            }
            item.candidate->m_requiredComponents = m_impl->candidateRequiredComponents.at(type);
            item.candidate->SetContext(&scene, object);
            prepared.push_back(std::move(item));
            auto& candidate = *prepared.back().candidate;
            if (!candidate.InitializeFields()) { discard(); return false; }
            if (!existing && entry.serialized && !ReadSavedFields(*entry.serialized, candidate.m_fields, fields)) {
                m_impl->diagnostic = "Managed saved primitive fields do not match the candidate schema.";
                discard();
                return false;
            }
            if (!RestoreFields(*this, candidate, candidate.m_fields, fields)) {
                if (m_impl->diagnostic.empty()) {
                    m_impl->diagnostic = "Managed candidate field restore failed.";
                }
                discard();
                return false;
            }
        }
    }
    // Component cache が新世代の全個体を指せるよう、構築完了後に状態を復元する。失敗しても旧個体は未変更。
    for (auto& item : prepared) {
        if (!item.candidate) {
            continue;
        }
        if (!item.reloadState.empty()) {
            auto restore = InstanceRequest(ManagedBridgeOp::RESTORE_RELOAD, generation, item.candidate->InstanceId(), item.candidate->OwnerId());
            restore.input = item.reloadState.data();
            restore.inputBytes = static_cast<uint32_t>(item.reloadState.size());
            ManagedBridgeReply reply;
            if (!DispatchPrepared(*item.candidate, restore, reply)) { discard(); return false; }
        }
        if (!item.candidate->ResolveSavedComponentReferences()) { discard(); return false; }
        // 停止済み entry の fault を snapshot が保持する。復元は許すが user callback は再実行しない。
        if (!inPlay && (!item.existing || !item.existing->IsRuntimeFaulted())
            && !Invoke(*item.candidate, ManagedCallback::AFTER_DESERIALIZE)) { discard(); return false; }
        if (!item.candidate->ValidateReferences()) { discard(); return false; }
    }
    for (const auto& item : prepared) {
        if (resolveEntry(item)) {
            continue;
        }
        m_impl->diagnostic = "Managed reload target changed before publication.";
        discard();
        return false;
    }
    const uint64_t oldGeneration = m_impl->generation;
    if (!Publish(generation, types)) { discard(); return false; }
    for (auto& item : prepared) {
        auto* entry = resolveEntry(item);
        if (!entry) {
            continue;
        }
        if (item.candidate && item.existing) {
            if (inPlay && !item.existing->m_runtimeFaulted) {
                item.existing->Invoke(ManagedCallback::RELOAD_RETIRED);
            }
            entry = resolveEntry(item);
            if (!entry) {
                continue;
            }
            if (inPlay) { CancelOwned(*item.existing); ReleaseOwnedObjects(*item.existing); }
            auto request = InstanceRequest(ManagedBridgeOp::RELEASE_INSTANCE, item.existing->m_generation,
                item.existing->m_instanceId, item.existing->m_ownerId);
            ManagedBridgeReply reply;
            Dispatch(request, reply);
            item.existing->CommitInstance(generation, item.candidate->m_instanceId, std::move(item.candidate->m_fields), inPlay);
            item.existing->m_executeAlways = item.candidate->m_executeAlways;
            item.existing->m_executionOrder = item.candidate->m_executionOrder;
            item.existing->m_requiredComponents = std::move(item.candidate->m_requiredComponents);
            item.existing->m_typeName = item.candidate->m_typeName;
            entry->serialized = std::move(item.saved);
            item.candidate->m_instanceId = 0;
            item.candidate->m_ownerId = 0;
        } else if (item.candidate) {
            item.candidate->enabled = entry->serialized ? entry->serialized->enabled : true;
            m_impl->adapters.emplace(item.candidate->m_ownerId, item.candidate.get());
            m_impl->adapterLookup.emplace(item.candidate.get(), item.candidate.get());
            entry->script = std::move(item.candidate);
        } else if (item.missing) {
            entry->serialized = std::move(item.missing);
            entry->script.reset();
        }
        if (!inPlay || !item.existing) {
            entry->m_awoken = false;
            entry->m_started = false;
        }
    }
    for (auto& item : prepared) {
        auto* entry = resolveEntry(item);
        if (inPlay && item.existing && entry && !item.existing->m_runtimeFaulted) {
            item.existing->Invoke(ManagedCallback::AFTER_RELOAD);
            entry = resolveEntry(item);
            if (entry && entry->m_started && item.existing->enabled && !item.existing->m_runtimeFaulted) {
                item.existing->Invoke(ManagedCallback::CONNECT);
            }
        }
    }
    const bool oldReferenced = std::any_of(m_impl->adapters.begin(), m_impl->adapters.end(),
        [oldGeneration](const auto& item) { return item.second->m_generation == oldGeneration; });
    if (oldReferenced) {
        m_impl->deferredRetirements.insert(oldGeneration);
    }
    else {
        Retire(oldGeneration);
    }
    return true;
}

void ManagedScriptRuntime::Unload(Scene* scene)
{
    m_impl->collectionPending = false;
    if (scene) {
        for (EntityID entity : scene->GetEntities<ScriptComponent>()) {
            auto* component = scene->GetComponent<ScriptComponent>(entity);
            if (!component) {
                continue;
            }
            for (auto& entry : component->scripts) {
                ManagedScriptAdapter* adapter = nullptr;
                for (const auto& [owner, candidate] : m_impl->adapters)
                    if (candidate == entry.script.get()) { adapter = candidate; break; }
                if (!adapter) {
                    continue;
                }
                toml::table table;
                util::TomlWriteReflector reflector(table);
                adapter->Reflect(reflector);
                PreserveUnknownFields(*adapter, entry.serialized ? entry.serialized->fieldsToml : std::string{}, table);
                std::ostringstream text;
                text << table;
                entry.serialized = std::make_unique<SerializedScriptData>(SerializedScriptData{adapter->m_typeName, text.str(), adapter->enabled, entry.savedScriptId});
                entry.script.reset();
                entry.m_awoken = false;
                entry.m_started = false;
            }
        }
    }
    ScriptFactory::UnregisterManaged();
    const uint64_t generation = m_impl->generation;
    m_impl->generation = 0;
    m_impl->types.clear();
    m_impl->executeAlways.clear();
    m_impl->typeSourceNames.clear();
    m_impl->typeSources.clear();
    m_impl->typeSchemas.clear();
    m_impl->executionOrders.clear();
    m_impl->requiredComponents.clear();
    m_impl->gizmoCallbacks.clear();
    m_impl->lifecycleCallbacks.clear();
    const bool referenced = std::any_of(m_impl->adapters.begin(), m_impl->adapters.end(),
        [generation](const auto& item) { return item.second->m_generation == generation; });
    if (referenced) {
        m_impl->deferredRetirements.insert(generation);
    }
    else {
        Retire(generation);
    }
}

bool ManagedScriptRuntime::Shutdown()
{
    m_impl->collectionPending = false;
    if (std::this_thread::get_id() != m_impl->mainThread || m_impl->scope.callId
        || m_impl->generation || !m_impl->adapters.empty() || !m_impl->generationLeases.empty()) {
        m_impl->diagnostic = "Managed Host shutdown requires an idle main thread and all project generations/Adapters unloaded.";
        return false;
    }
    if (!m_impl->dispatch) {
        return true;
    }
    ManagedBridgeRequest request;
    request.op = ManagedBridgeOp::SHUTDOWN;
    ManagedBridgeReply reply;
    if (!Dispatch(request, reply)) {
        return false;
    }
    m_impl->dispatch = nullptr;
    m_impl->candidateExecuteAlways.clear();
    m_impl->candidateTypeSourceNames.clear();
    m_impl->candidateTypeSources.clear();
    m_impl->deferredRetirements.clear();
    m_impl->candidateGizmoCallbacks.clear();
    m_impl->candidateLifecycleCallbacks.clear();
    m_impl->objects.Clear();
    m_impl->diagnostic.clear();
    return true;
}


}
