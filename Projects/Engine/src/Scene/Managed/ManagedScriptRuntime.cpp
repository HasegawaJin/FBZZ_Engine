/// @file    ManagedScriptRuntime.cpp
/// @brief   .NET Host 起動、候補世代の切り替え、native Script bridge。
/// @author  Hasegawa Jin
/// @date    2026-10-04
#include <Engine/Scene/Managed/ManagedScriptRuntime.hpp>
#include <Engine/Scene/Managed/ManagedScriptAdapter.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/ScriptComponent.hpp>
#include <Engine/Scene/ScriptFactory.hpp>
#include <Engine/Scene/ScriptValidation.hpp>
#include <Engine/Scene/ScriptEvent.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Engine/Core/Application.hpp>
#include "ManagedObjectLifecycle.hpp"
#include "ManagedObjectControls.hpp"
#include "ManagedObjectRegistry.hpp"
#include "ManagedDataAssets.hpp"
#include "ManagedPhysicsBridge.hpp"
#include "ManagedServicesBridge.hpp"
#include "ManagedScriptJobs.hpp"
#include "ManagedInputBridge.hpp"
#include <Engine/Scene/TomlReflector.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Input/KeyCode.hpp>
#include "ManagedFieldCodec.hpp"
#include "ManagedTransformBatch.hpp"
#include "ManagedBatchTransfers.hpp"
#include "ManagedAiCrowdTransfer.hpp"
#include "ManagedAiPoseTransfer.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
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

constexpr std::string_view kTypePrefix = "cs-internal:";
constexpr uint32_t TYPE_GIZMO_METADATA_FLAG = 2U;
constexpr uint32_t TYPE_LIFECYCLE_METADATA_FLAG = 4U;
constexpr uint32_t UPDATE_CALLBACK = 8U;
constexpr uint32_t FIXED_UPDATE_CALLBACK = 16U;
constexpr uint32_t LATE_UPDATE_CALLBACK = 32U;
constexpr uint32_t LIFECYCLE_CALLBACK_MASK = UPDATE_CALLBACK | FIXED_UPDATE_CALLBACK | LATE_UPDATE_CALLBACK;
constexpr uint32_t GIZMO_REGULAR_CALLBACK = 1U;
constexpr uint32_t GIZMO_SELECTED_CALLBACK = 2U;
constexpr uint32_t GIZMO_CALLBACK_MASK = GIZMO_REGULAR_CALLBACK | GIZMO_SELECTED_CALLBACK;

/// @note Notification の配送 snapshot 後に除去・無効化された受信者へ managed code を呼ばない。
bool AttachedNotificationReceiver(const ManagedScriptAdapter& adapter)
{
    if (!adapter.HasLiveContext() || !adapter.scene.IsActiveAndEnabled()) return false;
    const auto* scripts = adapter.scene.Self()->GetComponent<ScriptComponent>();
    if (!scripts) return false;
    return std::any_of(scripts->scripts.begin(), scripts->scripts.end(), [&adapter](const ScriptEntry& entry) {
        return entry.script.get() == &adapter && !entry.m_pendingRemoval;
    });
}

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

/// @note Connect may read its world and register owned work, but it cannot change the world or synchronously publish side effects.
bool IsConnectionOperation(const ManagedBridgeRequest& request)
{
    switch (request.op) {
    case ManagedBridgeOp::OBJECT_IS_ALIVE:
    case ManagedBridgeOp::RESOLVE_GUID:
    case ManagedBridgeOp::RESOLVE_COMPONENT_REFERENCE:
    case ManagedBridgeOp::MANAGED_COMPONENTS:
    case ManagedBridgeOp::OBJECT_SELF:
    case ManagedBridgeOp::OBJECT_NAME:
    case ManagedBridgeOp::SCENE_FIND_NAME:
    case ManagedBridgeOp::FIND_CHILD_PATH:
    case ManagedBridgeOp::TRANSFORM_READ:
    case ManagedBridgeOp::TRANSFORM_READ_ROTATION:
    case ManagedBridgeOp::TRANSFORM_READ_SCALE:
    case ManagedBridgeOp::TRANSFORM_READ_DIRECTION:
    case ManagedBridgeOp::OBJECTS_READ_WORLD_POSITIONS:
    case ManagedBridgeOp::OBJECT_READ:
    case ManagedBridgeOp::COMPONENT_READ:
    case ManagedBridgeOp::SCRIPT_ENABLED_READ:
    case ManagedBridgeOp::SCENE_QUERY:
    case ManagedBridgeOp::SCENE_CAN_CREATE:
    case ManagedBridgeOp::INPUT_KEY:
    case ManagedBridgeOp::INPUT_ACTION:
    case ManagedBridgeOp::INPUT_QUERY:
    case ManagedBridgeOp::PHYSICS_BODY_READ:
    case ManagedBridgeOp::PHYSICS_BODY_READ_ROTATION:
    case ManagedBridgeOp::PHYSICS_WORLD_READ:
    case ManagedBridgeOp::PHYSICS_RAYCAST:
    case ManagedBridgeOp::PHYSICS_SPHERE_CAST:
    case ManagedBridgeOp::PHYSICS_RAYCAST_ALL:
    case ManagedBridgeOp::PHYSICS_OVERLAP_SPHERE:
    case ManagedBridgeOp::PHYSICS_RAYCAST_FILTERED:
    case ManagedBridgeOp::PHYSICS_SPHERE_CAST_FILTERED:
    case ManagedBridgeOp::PHYSICS_RAYCAST_ALL_FILTERED:
    case ManagedBridgeOp::PHYSICS_OVERLAP_SPHERE_FILTERED:
    case ManagedBridgeOp::COLLIDER_READ:
    case ManagedBridgeOp::READ_ASSET:
    case ManagedBridgeOp::REGISTER_DELAY:
    case ManagedBridgeOp::REGISTER_TWEEN:
    case ManagedBridgeOp::CANCEL_JOB:
    case ManagedBridgeOp::CANCEL_OWNER:
    case ManagedBridgeOp::SUBSCRIBE_EVENT:
    case ManagedBridgeOp::UNSUBSCRIBE_EVENT:
        return true;
    case ManagedBridgeOp::SERVICE_ANIMATOR:
    case ManagedBridgeOp::SERVICE_AUDIO:
    case ManagedBridgeOp::SERVICE_UI:
    case ManagedBridgeOp::SERVICE_CAMERA:
    case ManagedBridgeOp::SERVICE_NAVIGATION:
    case ManagedBridgeOp::SERVICE_CHARACTER:
    case ManagedBridgeOp::SERVICE_APPLICATION:
    case ManagedBridgeOp::SERVICE_LIGHT:
    case ManagedBridgeOp::SERVICE_PARTICLE:
    case ManagedBridgeOp::SERVICE_SCENE:
    case ManagedBridgeOp::SERVICE_MATERIAL:
    case ManagedBridgeOp::SERVICE_SAVE:
    case ManagedBridgeOp::SERVICE_CONFIG:
    case ManagedBridgeOp::SERVICE_TERRAIN:
    case ManagedBridgeOp::SERVICE_WATER:
    case ManagedBridgeOp::SERVICE_VFX:
    case ManagedBridgeOp::SERVICE_SEQUENCE:
    case ManagedBridgeOp::SERVICE_RENDER:
    case ManagedBridgeOp::SERVICE_DISPLAY:
    case ManagedBridgeOp::SERVICE_JOINT:
    case ManagedBridgeOp::SERVICE_ASSETS:
    case ManagedBridgeOp::SERVICE_LINE:
    case ManagedBridgeOp::SERVICE_SPRITE:
    case ManagedBridgeOp::SERVICE_SORTING_GROUP:
    case ManagedBridgeOp::SERVICE_TRAIL:
    case ManagedBridgeOp::SERVICE_MESH_TRAIL:
    case ManagedBridgeOp::SERVICE_MESH:
    case ManagedBridgeOp::SERVICE_SKINNED_MESH:
    case ManagedBridgeOp::SERVICE_MOTION_WARP:
    case ManagedBridgeOp::SERVICE_IK:
    case ManagedBridgeOp::SERVICE_RAGDOLL:
    case ManagedBridgeOp::SERVICE_SPRING_BONE:
    case ManagedBridgeOp::SERVICE_UI_ANIMATOR:
    case ManagedBridgeOp::SERVICE_PATROL:
    case ManagedBridgeOp::SERVICE_SPLINE_FOLLOWER:
    case ManagedBridgeOp::SERVICE_CAMERA_SHAKE:
    case ManagedBridgeOp::SERVICE_VIRTUAL_CAMERA:
    case ManagedBridgeOp::SERVICE_AUDIO_MIXER_SEND:
    case ManagedBridgeOp::SERVICE_FLOW_FIELD:
    case ManagedBridgeOp::SERVICE_INSTANCES:
        return (request.flags & kManagedInternalMode) != 0;
    case ManagedBridgeOp::SERVICE_CURSOR:
        return (request.flags & kManagedInternalMode) != 0 || request.index == 0;
    default:
        return false;
    }
}

/// @note 描画候補の scope は world 読み取りだけに限定し、CLR Script 取得や予約・通知を認めない。
bool IsDrawOperation(const ManagedBridgeRequest& request)
{
    if (request.op == ManagedBridgeOp::MANAGED_COMPONENTS || request.op == ManagedBridgeOp::SCRIPT_ENABLED_READ
        || request.op == ManagedBridgeOp::READ_ASSET
        || (request.op >= ManagedBridgeOp::REGISTER_DELAY && request.op <= ManagedBridgeOp::PUBLISH_EVENT)) return false;
    if (request.op == ManagedBridgeOp::RESOLVE_COMPONENT_REFERENCE && request.index == UINT32_MAX) return false;
    if (request.op == ManagedBridgeOp::SERVICE_SAVE || request.op == ManagedBridgeOp::SERVICE_CONFIG
        || request.op == ManagedBridgeOp::SERVICE_DISPLAY || request.op == ManagedBridgeOp::SERVICE_JOINT
        || request.op == ManagedBridgeOp::SERVICE_ASSETS) return false;
    if (request.op >= ManagedBridgeOp::SERVICE_ANIMATOR && request.op <= ManagedBridgeOp::SERVICE_RENDER)
        return (request.flags & 1U) != 0;
    if (request.op >= ManagedBridgeOp::SERVICE_LINE && request.op <= ManagedBridgeOp::SERVICE_INSTANCES)
        return (request.flags & 1U) != 0;
    return IsConnectionOperation(request);
}

ManagedBridgeRequest InstanceRequest(ManagedBridgeOp op, uint64_t generation, uint64_t instanceId, uint64_t ownerId)
{
    ManagedBridgeRequest request;
    request.op = op;
    request.generation = generation;
    request.instanceId = instanceId;
    request.ownerId = ownerId;
    return request;
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
        if (descriptor == descriptors.end()) continue;
        if (descriptor->kind != field.value.valueKind) return false;
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
        if (!runtime.DispatchPrepared(adapter, request, reply)) return false;
    }
    return true;
}

void PreserveUnknownFields(const ManagedScriptAdapter& adapter, std::string_view preservedToml, toml::table& fields)
{
    if (preservedToml.empty()) return;
    const auto preserved = toml::parse(preservedToml);
    if (!preserved) return;
    for (const auto& [key, value] : preserved.table()) {
        const std::string_view savedKey = key.str();
        const bool recognized = std::any_of(adapter.Fields().begin(), adapter.Fields().end(), [&](const ManagedFieldDescriptor& field) {
            return field.name == savedKey || std::find(field.previousKeys.begin(), field.previousKeys.end(), savedKey) != field.previousKeys.end();
        });
        if (!recognized) fields.insert(key, value);
    }
}

bool ReadSavedFields(const SerializedScriptData& saved, const std::vector<ManagedFieldDescriptor>& descriptors,
                     std::vector<ManagedFieldValue>& fields)
{
    if (saved.fieldsToml.empty()) return true;
    auto parsed = toml::parse(saved.fieldsToml);
    if (!parsed) return false;
    for (const auto& descriptor : descriptors) {
        if (!(descriptor.flags & kManagedFieldSetting)) continue;
        const toml::node* node = parsed.table().get(descriptor.name);
        if (!node)
            for (const std::string& previous : descriptor.previousKeys)
                if ((node = parsed.table().get(previous))) break;
        if (!node) continue;
        ManagedFieldValue field;
        field.name = descriptor.name;
        field.value.valueKind = descriptor.kind;
        if (descriptor.kind == ManagedValueKind::NONE) continue;
        if (!managed_detail::ReadSavedField(*node, field)) return false;
        fields.push_back(std::move(field));
    }
    return true;
}

} /// @note namespace

struct ManagedScriptRuntime::Impl {
    managed_detail::ManagedPhysicsBridge physicsBridge;
    managed_detail::ManagedServicesBridge servicesBridge;
    ManagedBridgeDispatch dispatch = nullptr;
    HMODULE hostModule = nullptr;
    std::filesystem::path hostDirectory;
    std::thread::id mainThread = std::this_thread::get_id();
    uint64_t generation = 0;
    uint64_t nextGeneration = 1;
    uint64_t nextOwner = 1;
    uint64_t nextCall = 1;
    std::string diagnostic;
    bool collectionPending = false;
    std::vector<std::string> types;
    std::unordered_map<std::string, bool> executeAlways;
    std::unordered_map<std::string, bool> candidateExecuteAlways;
    std::unordered_map<std::string, std::string> typeSourceNames;
    std::unordered_map<std::string, std::string> candidateTypeSourceNames;
    std::unordered_map<std::string, std::string> typeSources;
    std::unordered_map<std::string, std::string> candidateTypeSources;
    std::unordered_map<std::string, std::string> typeSchemas;
    std::unordered_map<std::string, std::string> candidateTypeSchemas;
    std::unordered_map<std::string, int> executionOrders;
    std::unordered_map<std::string, int> candidateExecutionOrders;
    std::unordered_map<std::string, std::vector<std::string>> requiredComponents;
    std::unordered_map<std::string, std::vector<std::string>> candidateRequiredComponents;
    /// @note 未登録型は metadata 非対応の Host として従来の draw query を行う。値 0 は両 callback の不在を表す。
    std::unordered_map<std::string, uint32_t> gizmoCallbacks;
    std::unordered_map<std::string, uint32_t> candidateGizmoCallbacks;
    /// @note 対応印のない旧 Host は登録せず、不在 mask と未知の情報を区別する。
    std::unordered_map<std::string, uint32_t> lifecycleCallbacks;
    std::unordered_map<std::string, uint32_t> candidateLifecycleCallbacks;
    /// @note Host だけが fault した個体も、新しい個体への交換まで保守 dispatch を保つ。
    std::unordered_set<uint64_t> failedCallbackInstances;
    std::filesystem::path candidateAssemblyPath;
    bool candidateInternalMode = false;
    struct GenerationMetadata {
        std::vector<std::string> types;
        std::unordered_map<std::string, bool> executeAlways;
        std::unordered_map<std::string, std::string> sourceNames;
        std::unordered_map<std::string, std::string> sources;
        std::unordered_map<std::string, std::string> schemas;
        std::unordered_map<std::string, int> executionOrders;
        std::unordered_map<std::string, std::vector<std::string>> requiredComponents;
        std::unordered_map<std::string, uint32_t> gizmoCallbacks;
        std::unordered_map<std::string, uint32_t> lifecycleCallbacks;
        std::filesystem::path assemblyPath;
        bool internalMode;
    };
    std::unordered_map<uint64_t, GenerationMetadata> generationMetadata;
    std::unordered_map<uint64_t, uint32_t> generationLeases;
    std::unordered_map<uint64_t, ManagedScriptAdapter*> adapters;
    std::unordered_map<const Script*, ManagedScriptAdapter*> adapterLookup;
    struct OwnerBinding {
        uint64_t generation;
        uint64_t ownerId;
        uint64_t sceneLifetime;
        uint64_t objectId;
        EntityID entity;
        std::string guid;
        std::string scriptBirth;
        uint32_t lifecycleCallbacks = 0;
    };
    /// @note Host の個体 ID は世代をまたぐ単調 ID。成功した接続だけを記録し、CLR オブジェクトは保持しない。
    std::unordered_map<uint64_t, OwnerBinding> ownerBindings;
    /// @note Scene 外の Undo/preview 個体も所有するため、最後の Adapter の解放まで旧世代の退役を遅らせる。
    std::unordered_set<uint64_t> deferredRetirements;
    ManagedScriptAdapter* preparation = nullptr;
    std::vector<ManagedScriptAdapter*> settingsPreparation;
    managed_detail::ManagedObjectRegistry objects;
    struct OwnedObjects { Scene* scene; uint64_t lifetime; std::vector<EntityID> entities; };
    std::unordered_map<uint64_t, OwnedObjects> ownedObjects;
    managed_detail::ManagedTransformBatch transformBatch;
    managed_detail::ManagedBatchTransfers batchTransfers;
    managed_detail::ManagedAiCrowdTransfer aiCrowdTransfer;
    managed_detail::ManagedAiPoseTransfer aiPoseTransfer;
    uint64_t RegisterObject(Scene& scene, GameObject& object)
    {
        return objects.Register(scene, object);
    }
    GameObject* ResolveObject(Scene& scene, uint64_t token) const
    {
        return objects.Resolve(scene, token);
    }
    struct Scope {
        uint64_t generation = 0;
        uint64_t instanceId = 0;
        uint64_t ownerId = 0;
        uint64_t callId = 0;
        ManagedCallback callback = ManagedCallback::RESET;
    } scope;
    struct AssetRead {
        Scope caller;
        uint64_t sceneLifetime = 0;
        uint64_t objectId = 0;
        std::string bytes;
    };
    std::unordered_map<uint64_t, AssetRead> assetReads;
    uint64_t nextAssetRead = 1;
    bool assetReadIdsExhausted = false;
    void EndAssetReads(uint64_t callId)
    { std::erase_if(assetReads, [callId](const auto& item) { return item.second.caller.callId == callId; }); }
};

ManagedScriptRuntime* ManagedScriptRuntime::m_liveRuntime = nullptr;

ManagedScriptRuntime::ManagedScriptRuntime()
{
    /// @note facade の Script destructor が購読を解除するため、event bus を Runtime より先に初期化して終了時の寿命を保証する。
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
    if (m_liveRuntime == this) m_liveRuntime = nullptr;
}
void ManagedScriptRuntime::RetireScene(Scene& scene)
{
    if (m_liveRuntime) m_liveRuntime->m_impl->objects.RetireScene(scene);
}
void ManagedScriptRuntime::RetireObject(Scene& scene, EntityID entity)
{
    if (m_liveRuntime) m_liveRuntime->m_impl->objects.RetireObject(scene, entity);
}

bool ManagedScriptRuntime::Initialize(const std::filesystem::path& hostDirectory)
{
    m_impl->collectionPending = false;
    if (std::this_thread::get_id() != m_impl->mainThread) return false;
    if (m_impl->dispatch) return true;
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
    if (!m_impl->hostModule) m_impl->hostModule = LoadLibraryW(hostPath.data());
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
        if (context) close(context);
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
        if (m_impl->diagnostic.empty()) m_impl->diagnostic = "Managed Host ABI schema did not match.";
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
    /// @note 不正な callback 要求は CLR へ入る前に拒否し、ユーザーの部分的変更や Host だけの fault を作らない。
    if (request.op == ManagedBridgeOp::INVOKE && (!m_impl->scope.callId || !request.callId
        || request.callId != m_impl->scope.callId || request.generation != m_impl->scope.generation
        || request.instanceId != m_impl->scope.instanceId || request.ownerId != m_impl->scope.ownerId
        || request.index != static_cast<uint32_t>(m_impl->scope.callback))) {
        reply = {}; reply.status = ManagedBridgeStatus::INVALID_SCOPE;
        m_impl->diagnostic = "Managed callback request does not match the active native invocation scope.";
        if (output) output->clear();
        return false;
    }
    /// @note 直接の ABI 操作も成功記録を失効させる。失敗しても再検証し、ownerId 省略の解放も個体 ID で扱う。
    if (request.op == ManagedBridgeOp::BIND_OWNER || request.op == ManagedBridgeOp::RELEASE_INSTANCE)
        m_impl->ownerBindings.erase(request.instanceId);
    else if (request.op == ManagedBridgeOp::RETIRE_GENERATION)
        std::erase_if(m_impl->ownerBindings, [&request](const auto& item) { return item.second.generation == request.generation; });
    else if (request.op == ManagedBridgeOp::SHUTDOWN || request.op == ManagedBridgeOp::INITIALIZE)
        m_impl->ownerBindings.clear();
    const auto invalidateEmptyCallbackSkip = [&] {
        const auto found = m_impl->adapters.find(request.ownerId);
        if (found != m_impl->adapters.end() && found->second->Generation() == request.generation
            && found->second->InstanceId() == request.instanceId) m_impl->failedCallbackInstances.insert(request.instanceId);
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
            if (reply.outputBytes) output->assign(data, reply.outputBytes);
            else output->clear();
        }
        if (status != ManagedBridgeStatus::OK) {
            invalidateEmptyCallbackSkip();
            m_impl->diagnostic = reply.outputBytes ? std::string(data, reply.outputBytes)
                : "Managed Host status " + std::to_string(static_cast<int>(status));
            if (status != ManagedBridgeStatus::NOT_FOUND && status != ManagedBridgeStatus::COLLECTION_PENDING)
                FBZZ_LOG_ERROR("Managed Script: %s", m_impl->diagnostic.c_str());
            return false;
        }
        if (request.op == ManagedBridgeOp::RELEASE_INSTANCE) m_impl->failedCallbackInstances.erase(request.instanceId);
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
    if (error) return false;
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
    if (std::this_thread::get_id() != m_impl->mainThread) return false;
    if (adapter.m_boundScene != adapter.m_scene || adapter.m_boundObject != adapter.m_gameObject) {
        m_impl->servicesBridge.CancelCursorOwner(adapter.OwnerId());
        adapter.m_boundScene = adapter.m_scene;
        adapter.m_boundObject = adapter.m_gameObject;
        adapter.m_contextSceneGeneration = 0;
        adapter.m_contextEntity = EntityID::INVALID;
        adapter.m_objectId = 0;
        if (adapter.m_scene && adapter.m_gameObject) {
            /// @note 非所有 pointer を dereference せず、Scene 所有物との一致を先に確認する。
            for (GameObject& object : adapter.m_scene->GameObjects()) {
                if (&object != adapter.m_gameObject) continue;
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
                && cached.scriptBirth == adapter.m_referenceBirth) return true;
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
    if (std::this_thread::get_id() != m_impl->mainThread) return 0;
    GameObject* object = scene.GetGameObject(entity);
    return object ? m_impl->RegisterObject(scene, *object) : 0;
}

uint64_t ManagedScriptRuntime::ReferenceComponentLifetime(Scene& scene, EntityID entity, uint32_t kind)
{
    if (std::this_thread::get_id() != m_impl->mainThread) return 0;
    GameObject* object = scene.GetGameObject(entity);
    if (!object || managed_detail::ManagedObjectControls::IsPendingComponentRemoval(*object, kind)) return 0;
    bool exists = false; uint64_t lifetime = 0;
    if (managed_detail::ManagedObjectControls::Component(*object, kind, ManagedBridgeOp::COMPONENT_READ, exists, &lifetime) != ManagedBridgeStatus::OK || !exists) return 0;
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
    if (!BindOwner(adapter)) return false;
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
            if (m_impl->diagnostic.empty()) m_impl->diagnostic = "Settings preparation target identity is invalid or duplicated.";
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
    if (reference.guid.empty() || !adapter.HasLiveContext()) return true;
    reference.sceneGeneration = adapter.m_scene->GetRenderSceneGeneration();
    GameObject* match = nullptr;
    for (GameObject& object : adapter.m_scene->GameObjects()) {
        if (object.instanceId != reference.guid) continue;
        if (match) { m_impl->diagnostic = "GameObject GUID is ambiguous: " + reference.guid; return false; }
        match = &object;
    }
    if (match) reference.id = match->GetID();
    return true;
}

bool ManagedScriptRuntime::ResolveObjectId(ManagedScriptAdapter& adapter, uint64_t objectId, ScriptObjectReference& reference)
{
    reference.id = EntityID::INVALID;
    reference.sceneGeneration = 0;
    if (!adapter.HasLiveContext()) return false;
    reference.sceneGeneration = adapter.m_scene->GetRenderSceneGeneration();
    GameObject* object = m_impl->ResolveObject(*adapter.m_scene, objectId);
    if (object && object->instanceId == reference.guid) reference.id = object->GetID();
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
        if (!component) continue;
        for (auto& entry : component->scripts) {
            auto* adapter = FindAdapter(entry.script.get());
            if (!adapter) continue;
            if (!BindOwner(*adapter)) return false;
            std::vector<ManagedFieldValue> values;
            if (!adapter->CaptureFields(values)) return false;
            std::erase_if(values, [](const ManagedFieldValue& value) {
                return value.value.valueKind != ManagedValueKind::GAME_OBJECT && value.value.valueKind != ManagedValueKind::GAME_OBJECT_ARRAY
                    && value.value.valueKind != ManagedValueKind::STRUCTURED;
            });
            if (!RestoreFields(*this, *adapter, adapter->Fields(), values)) return false;
        }
    }
    for (EntityID entity : scene.GetEntities<ScriptComponent>())
        if (auto* component = scene.GetComponent<ScriptComponent>(entity))
            for (auto& entry : component->scripts)
                if (auto* adapter = FindAdapter(entry.script.get()); adapter && !adapter->ResolveSavedComponentReferences()) return false;
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
            if (reply.status == ManagedBridgeStatus::NOT_FOUND && index == 0 && reply.index == 0) break;
            Retire(generation);
            return false;
        }
        if (index == 0) declaredTypes = reply.index;
        /// @note Count comes from the Host's Int32 list representation, not a semantic total-type cutoff; all replies must agree before checked termination.
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
        /// @note GET_TYPE flags=0 の bit 1 は gizmo presence の対応を表し、Integer の bit 0/1 が通常/選択 callback。
        if ((reply.flags & TYPE_GIZMO_METADATA_FLAG) != 0) {
            if (reply.integer < 0 || reply.integer > GIZMO_CALLBACK_MASK) { Retire(generation); return false; }
            m_impl->candidateGizmoCallbacks.emplace(type, static_cast<uint32_t>(reply.integer));
        }
        if ((reply.flags & TYPE_LIFECYCLE_METADATA_FLAG) != 0)
            m_impl->candidateLifecycleCallbacks.emplace(type, reply.flags & (TYPE_LIFECYCLE_METADATA_FLAG | LIFECYCLE_CALLBACK_MASK));
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
            if (!line.empty()) required.push_back(std::move(line));
        }
        m_impl->candidateRequiredComponents.emplace(type, std::move(required));
        if (index + 1 >= count) break;
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
    if (!Dispatch(request, reply)) return 0;
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
    if (generation == m_impl->generation) return true;
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
    if (activated) Retire(previousGeneration);
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
    if (!LoadCandidate(assemblyPath, generation, types, internalMode)) return false;
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
    if (!Dispatch(request, reply)) return;
    if (--lease->second == 0) m_impl->generationLeases.erase(lease);
    Retire(generation);
}

bool ManagedScriptRuntime::Load(const std::filesystem::path& gameAssemblyPath, bool internalMode)
{
    m_impl->collectionPending = false;
    if (IsLoaded()) { m_impl->diagnostic = "Use Reload with the active Scene to replace a managed assembly."; return false; }
    uint64_t generation = 0;
    std::vector<std::string> types;
    if (!LoadCandidate(gameAssemblyPath, generation, types, internalMode)) return false;
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
    if (!Dispatch(request, reply)) return nullptr;
    auto adapter = std::make_unique<ManagedScriptAdapter>(*this, typeName,
        m_impl->generation, reply.instanceId, owner, m_impl->executeAlways[typeName], savedScriptId);
    adapter->m_executionOrder = m_impl->executionOrders.at(typeName);
    adapter->m_requiredComponents = m_impl->requiredComponents.at(typeName);
    m_impl->adapters.emplace(owner, adapter.get());
    m_impl->adapterLookup.emplace(adapter.get(), adapter.get());
    if (!adapter->InitializeFields()) return nullptr;
    return adapter;
}

void ManagedScriptRuntime::Retire(uint64_t generation)
{
    if (!generation || generation == m_impl->generation) return;
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
            [generation](const auto& item) { return item.second->m_generation == generation; }))
        Retire(generation);
}

void ManagedScriptRuntime::ReleaseOwnedObjects(ManagedScriptAdapter& adapter)
{
    m_impl->servicesBridge.CancelOwner(adapter.OwnerId(), adapter.HasLiveContext() ? &adapter : nullptr);
    m_impl->batchTransfers.CancelOwner(adapter.OwnerId());
    const auto found = m_impl->ownedObjects.find(adapter.OwnerId());
    if (found == m_impl->ownedObjects.end()) return;
    if (adapter.HasLiveContext() && found->second.scene == adapter.m_scene
        && found->second.lifetime == adapter.m_scene->GetRenderSceneGeneration())
        for (EntityID entity : found->second.entities)
            if (GameObject* object = adapter.m_scene->GetGameObject(entity)) GameObject::Destroy(*object);
    m_impl->ownedObjects.erase(found);
}

bool ManagedScriptRuntime::Invoke(ManagedScriptAdapter& adapter, ManagedCallback callback)
{
    return InvokePayload(adapter, callback, {});
}

bool ManagedScriptRuntime::Resume(ManagedScriptAdapter& adapter, bool enabled)
{
    if (std::this_thread::get_id() != m_impl->mainThread) return false;
    m_impl->diagnostic.clear();
    const auto fail = [&] {
        adapter.m_resumeDiagnostic = m_impl->diagnostic;
        return false;
    };
    if (!IsDispatchIdle()
        || adapter.Generation() != m_impl->generation || !BindOwner(adapter) || !adapter.HasLiveContext()) {
        if (m_impl->diagnostic.empty()) m_impl->diagnostic = "Resume requires a current Script at an idle dispatch point.";
        return fail();
    }
    if (!adapter.IsRuntimeFaulted() && !adapter.RequirementsBlocked()) {
        m_impl->diagnostic = "The Script is not stopped by an error or missing requirement.";
        return fail();
    }
    auto* component = adapter.m_gameObject->GetComponent<ScriptComponent>();
    ScriptEntry* entry = nullptr;
    if (component)
        for (auto& item : component->scripts)
            if (item.script.get() == &adapter && !item.m_pendingRemoval) { entry = &item; break; }
    if (!entry) { m_impl->diagnostic = "The Script entry is no longer attached to its owner."; return fail(); }
    std::vector<ScriptRequirementIssue> issues;
    CollectScriptRequirementIssues(*adapter.m_gameObject, adapter, issues, false, true);
    m_impl->diagnostic.clear();
    for (const auto& issue : issues)
        if (issue.blocksStart) {
            if (!m_impl->diagnostic.empty()) m_impl->diagnostic += "\n";
            m_impl->diagnostic += FormatScriptRequirementIssue(issue);
        }
    if (!m_impl->diagnostic.empty()) return fail();
    if (!adapter.ValidateReferences()) return fail();
    CancelOwned(adapter);
    auto request = InstanceRequest(ManagedBridgeOp::RESUME_INSTANCE, adapter.Generation(), adapter.InstanceId(), adapter.OwnerId());
    ManagedBridgeReply reply;
    if (!DispatchPrepared(adapter, request, reply)) return fail();
    /// @note 再開操作で作者 callback を呼ばず、成功済みの開始段階だけを次 Tick に引き継ぐ。
    adapter.ResetLifecycleState();
    adapter.m_faultDiagnostic.clear();
    adapter.m_resumeDiagnostic.clear();
    adapter.enabled = enabled;
    entry->m_awoken = adapter.m_managedAwoken;
    entry->m_started = adapter.m_managedStarted;
    return true;
}

bool ManagedScriptRuntime::InvokeOwned(ManagedScriptAdapter& adapter, ManagedCallback callback, uint64_t token,
                                     std::string_view payload, float progress, bool completed, bool nativeSequence)
{
    if (!Script::IsInPlayMode() || adapter.Generation() != m_impl->generation || !adapter.HasLiveContext()
        || !adapter.scene.IsActiveAndEnabled() || !token
        || (callback != ManagedCallback::EVENT && callback != ManagedCallback::JOB)
        || (nativeSequence && callback != ManagedCallback::EVENT)) return false;
    m_impl->diagnostic.clear();
    const auto previous = m_impl->scope;
    const uint64_t call = m_impl->nextCall++;
    m_impl->scope = {adapter.Generation(), adapter.InstanceId(), adapter.OwnerId(), call, callback};
    auto request = InstanceRequest(callback == ManagedCallback::EVENT ? ManagedBridgeOp::INVOKE_EVENT : ManagedBridgeOp::INVOKE_JOB,
        adapter.Generation(), adapter.InstanceId(), adapter.OwnerId());
    request.callId = call; request.integer = std::bit_cast<int64_t>(token);
    request.x = progress; request.flags = nativeSequence ? 2U : completed ? 1U : 0U;
    request.input = payload.data(); request.inputBytes = static_cast<uint32_t>(payload.size());
    ManagedBridgeReply reply;
    const bool success = Dispatch(request, reply);
    m_impl->servicesBridge.EndCall(call);
    m_impl->batchTransfers.EndCall(call);
    m_impl->EndAssetReads(call);
    m_impl->scope = previous;
    if (!success && reply.status != ManagedBridgeStatus::NOT_FOUND) adapter.RecordFault();
    return success;
}

void ManagedScriptRuntime::CancelOwned(ManagedScriptAdapter& adapter)
{
    m_impl->servicesBridge.CancelCursorOwner(adapter.OwnerId());
    m_impl->servicesBridge.CancelOwner(adapter.OwnerId(), adapter.HasLiveContext() ? &adapter : nullptr);
    adapter.m_connectionsActive = false;
    managed_detail::CancelManagedOwner(adapter);
    if (!adapter.InstanceId()) return;
    auto request = InstanceRequest(ManagedBridgeOp::CANCEL_OWNER, adapter.Generation(), adapter.InstanceId(), adapter.OwnerId());
    ManagedBridgeReply reply;
    Dispatch(request, reply);
    request.flags = 1;
    Dispatch(request, reply);
}

bool ManagedScriptRuntime::InvokeCollision(ManagedScriptAdapter& adapter, ManagedCallback callback, const CollisionInfo& info)
{
    m_impl->diagnostic.clear();
    if (!BindOwner(adapter) || !adapter.HasLiveContext() || !Script::IsInPlayMode()
        || callback < ManagedCallback::COLLISION_ENTER || callback > ManagedCallback::TRIGGER_EXIT) {
        m_impl->diagnostic = "Physics callbacks require a live Play instance.";
        return false;
    }
    uint64_t otherId = 0;
    uint64_t lifetime = 0;
    std::string_view guid;
    if (info.other) {
        for (GameObject& object : adapter.m_scene->GameObjects()) {
            if (&object != info.other) continue;
            otherId = m_impl->RegisterObject(*adapter.m_scene, object);
            lifetime = adapter.m_scene->GetRenderSceneGeneration();
            guid = object.instanceId;
            break;
        }
    }
    const std::array values{info.contactPoint.x, info.contactPoint.y, info.contactPoint.z,
        info.contactNormal.x, info.contactNormal.y, info.contactNormal.z, info.contactDepth,
        info.relativeVelocity.x, info.relativeVelocity.y, info.relativeVelocity.z, info.approachSpeed, info.impactImpulse};
    if (std::any_of(values.begin(), values.end(), [](float value) { return !std::isfinite(value); })) {
        m_impl->diagnostic = "Physics callback contains non-finite contact data.";
        return false;
    }
    std::string payload;
    managed_detail::Append(payload, otherId);
    managed_detail::Append(payload, lifetime);
    for (float value : values) managed_detail::Append(payload, value);
    managed_detail::Append(payload, static_cast<uint32_t>(guid.size()));
    payload.append(guid);
    return InvokePayload(adapter, callback, payload);
}

bool ManagedScriptRuntime::InvokeAnimationEvent(ManagedScriptAdapter& adapter, const AnimationEventInfo& info)
{
    const std::string_view name = info.name ? info.name : "";
    if (name.size() > 1024 * 1024 - 16 || !std::isfinite(info.floatParam) || !std::isfinite(info.clipTime)) {
        m_impl->diagnostic = "Animation notification contains invalid values or exceeds its byte budget.";
        return false;
    }
    std::string payload;
    managed_detail::Append(payload, info.intParam);
    managed_detail::Append(payload, info.floatParam);
    managed_detail::Append(payload, info.clipTime);
    managed_detail::Append(payload, static_cast<uint32_t>(name.size()));
    payload.append(name);
    return InvokePayload(adapter, ManagedCallback::ANIMATION_EVENT, payload);
}

bool ManagedScriptRuntime::InvokeAnimatorMove(ManagedScriptAdapter& adapter, const RootMotionInfo& info)
{
    const std::array values{info.deltaPosition.x, info.deltaPosition.y, info.deltaPosition.z,
        info.deltaRotation.x, info.deltaRotation.y, info.deltaRotation.z, info.deltaRotation.w,
        info.worldDeltaPosition.x, info.worldDeltaPosition.y, info.worldDeltaPosition.z,
        info.worldVelocity.x, info.worldVelocity.y, info.worldVelocity.z, info.deltaTime};
    const float rotationLength = info.deltaRotation.x * info.deltaRotation.x + info.deltaRotation.y * info.deltaRotation.y
        + info.deltaRotation.z * info.deltaRotation.z + info.deltaRotation.w * info.deltaRotation.w;
    if (std::any_of(values.begin(), values.end(), [](float value) { return !std::isfinite(value); })
        || info.deltaTime < 0 || !std::isfinite(rotationLength) || rotationLength <= 0) {
        m_impl->diagnostic = "Root motion notification contains invalid motion, time or rotation.";
        return false;
    }
    std::string payload;
    for (float value : values) managed_detail::Append(payload, value);
    managed_detail::Append(payload, static_cast<uint32_t>(info.appliedByEngine));
    return InvokePayload(adapter, ManagedCallback::ANIMATOR_MOVE, payload);
}

bool ManagedScriptRuntime::InvokeSequenceEvent(ManagedScriptAdapter& adapter, const SequenceEventInfo& info, uint64_t subscription)
{
    if (subscription && !AttachedNotificationReceiver(adapter)) return true;
    const std::string_view name = info.name ? info.name : "";
    const std::string_view sequence = info.sequenceName ? info.sequenceName : "";
    if (name.size() > 1024 * 1024 - 20 || sequence.size() > 1024 * 1024 - 20 - name.size()
        || !std::isfinite(info.floatParam) || !std::isfinite(info.sequenceTime)) {
        m_impl->diagnostic = "Sequence notification contains invalid values or exceeds its byte budget.";
        if (subscription) adapter.RecordFault();
        return false;
    }
    std::string payload;
    managed_detail::Append(payload, info.intParam);
    managed_detail::Append(payload, info.floatParam);
    managed_detail::Append(payload, info.sequenceTime);
    managed_detail::Append(payload, static_cast<uint32_t>(name.size()));
    managed_detail::Append(payload, static_cast<uint32_t>(sequence.size()));
    payload.append(name);
    payload.append(sequence);
    return subscription ? InvokeOwned(adapter, ManagedCallback::EVENT, subscription, payload, 0, false, true)
        : InvokePayload(adapter, ManagedCallback::SEQUENCE_EVENT, payload);
}

bool ManagedScriptRuntime::InvokeSequenceFinished(ManagedScriptAdapter& adapter, std::string_view sequenceName)
{
    if (sequenceName.size() > 1024 * 1024 || sequenceName.find('\0') != std::string_view::npos) {
        m_impl->diagnostic = "Sequence completion contains an invalid name or exceeds its byte budget.";
        return false;
    }
    return InvokePayload(adapter, ManagedCallback::SEQUENCE_FINISHED, sequenceName);
}

bool ManagedScriptRuntime::InvokePayload(ManagedScriptAdapter& adapter, ManagedCallback callback, std::string_view payload)
{
    /// @note 個体の失敗へ別個体・以前の要求の診断を帰属させない。
    m_impl->diagnostic.clear();
    if ((callback <= ManagedCallback::LATE_UPDATE || callback >= ManagedCallback::COLLISION_ENTER)
        && adapter.Generation() != m_impl->generation) {
        m_impl->diagnostic = "Managed callback belongs to an unpublished generation.";
        return false;
    }
    const uint32_t updateBit = callback == ManagedCallback::UPDATE ? UPDATE_CALLBACK
        : callback == ManagedCallback::FIXED_UPDATE ? FIXED_UPDATE_CALLBACK
        : callback == ManagedCallback::LATE_UPDATE ? LATE_UPDATE_CALLBACK : 0U;
    if (updateBit && std::this_thread::get_id() == m_impl->mainThread) {
        const auto found = m_impl->ownerBindings.find(adapter.InstanceId());
        if (found != m_impl->ownerBindings.end()
            && (found->second.lifecycleCallbacks & (TYPE_LIFECYCLE_METADATA_FLAG | updateBit)) == TYPE_LIFECYCLE_METADATA_FLAG
            && m_impl->dispatch && IsDispatchIdle() && !adapter.IsRuntimeFaulted() && adapter.HasLiveContext()
            && !m_impl->failedCallbackInstances.contains(adapter.InstanceId())) {
            const auto& bound = found->second;
            const auto* object = adapter.m_scene->GetGameObject(adapter.m_contextEntity);
            /// @note 不在の更新だけを省き、owner・世代・直接 GUID 変更・失敗・再入は従来の Host 検証へ戻す。
            /// @see https://docs.unity3d.com/6000.0/Documentation/Manual/events-per-frame-optimization.html 不要な native/managed 更新往復の削減。
            if (bound.generation == adapter.Generation() && bound.ownerId == adapter.OwnerId()
                && bound.sceneLifetime == adapter.m_contextSceneGeneration && bound.objectId == adapter.m_objectId
                && bound.entity == adapter.m_contextEntity && bound.guid == object->instanceId
                && bound.scriptBirth == adapter.m_referenceBirth) return true;
        }
    }
    if (!BindOwner(adapter)) return false;
    if (callback >= ManagedCallback::ANIMATION_EVENT && callback <= ManagedCallback::NAV_TARGET_LOST
        && (!Script::IsInPlayMode() || !AttachedNotificationReceiver(adapter))) return true;
    if (callback == ManagedCallback::DISABLE || callback == ManagedCallback::DESTROY) CancelOwned(adapter);
    const auto previous = m_impl->scope;
    const uint64_t call = m_impl->nextCall++;
    m_impl->scope = {adapter.m_generation, adapter.m_instanceId, adapter.m_ownerId, call, callback};
    auto request = InstanceRequest(ManagedBridgeOp::INVOKE, adapter.m_generation, adapter.m_instanceId, adapter.m_ownerId);
    request.callId = call;
    request.index = static_cast<uint32_t>(callback);
    request.input = payload.data();
    request.inputBytes = static_cast<uint32_t>(payload.size());
    ManagedBridgeReply reply;
    const bool succeeded = Dispatch(request, reply);
    m_impl->servicesBridge.EndCall(call);
    m_impl->batchTransfers.EndCall(call);
    m_impl->EndAssetReads(call);
    m_impl->scope = previous;
    return succeeded;
}

bool ManagedScriptRuntime::CaptureGizmos(ManagedScriptAdapter& adapter, bool selected,
                                       std::vector<ScriptDebugDrawCommand>& commands, uint32_t capacity)
{
    commands.clear();
    if (!capacity || capacity > (std::numeric_limits<uint32_t>::max() - 4U) / 52U
        || adapter.Generation() != m_impl->generation || !BindOwner(adapter) || !adapter.HasLiveContext()) return false;
    /// @note 不在が確定した callback だけを省き、capacity・世代・owner 接続の拒否は従来と同じ順で行う。
    /// @note callback の入れ子では Host が idle 契約で拒否するため、空描画も従来の query を通す。
    const auto callbacks = m_impl->gizmoCallbacks.find(adapter.m_typeName);
    if (!m_impl->scope.callId && callbacks != m_impl->gizmoCallbacks.end()
        && (callbacks->second & (selected ? GIZMO_SELECTED_CALLBACK : GIZMO_REGULAR_CALLBACK)) == 0) return true;
    const ManagedCallback callback = selected ? ManagedCallback::DRAW_GIZMOS_SELECTED : ManagedCallback::DRAW_GIZMOS;
    const auto previous = m_impl->scope;
    const uint64_t call = m_impl->nextCall++;
    m_impl->scope = {adapter.Generation(), adapter.InstanceId(), adapter.OwnerId(), call, callback};
    auto request = InstanceRequest(ManagedBridgeOp::DRAW_GIZMOS_QUERY, adapter.Generation(), adapter.InstanceId(), adapter.OwnerId());
    request.callId = call; request.index = static_cast<uint32_t>(callback); request.flags = capacity;
    /// @note 最大 command byte 数を先に確保し、作者 draw callback を容量不足で再実行しない。
    std::vector<char> output(4U + static_cast<size_t>(capacity) * 52U);
    request.output = output.data(); request.outputCapacity = static_cast<uint32_t>(output.size());
    ManagedBridgeReply reply;
    const auto status = static_cast<ManagedBridgeStatus>(m_impl->dispatch(&request, &reply));
    m_impl->scope = previous;
    if (status != ManagedBridgeStatus::OK || reply.status != status || reply.outputBytes > output.size()) {
        m_impl->diagnostic = reply.outputBytes <= output.size() ? std::string(output.data(), reply.outputBytes) : "Managed gizmo output exceeded its command buffer.";
        FBZZ_LOG_WARN("Managed gizmo: %s", m_impl->diagnostic.c_str());
        return false;
    }
    std::string_view bytes(output.data(), reply.outputBytes);
    size_t offset = 0; uint32_t count = 0;
    if (!managed_detail::Read(bytes, offset, count) || count > capacity || bytes.size() != 4U + static_cast<size_t>(count) * 52U) return false;
    std::vector<ScriptDebugDrawCommand> prepared;
    prepared.reserve(count);
    for (uint32_t index = 0; index < count; ++index) {
        ScriptDebugDrawCommand command; uint32_t kind = 0, depth = 0;
        if (!managed_detail::Read(bytes, offset, kind) || kind > 2
            || !managed_detail::Read(bytes, offset, command.a.x) || !managed_detail::Read(bytes, offset, command.a.y) || !managed_detail::Read(bytes, offset, command.a.z)
            || !managed_detail::Read(bytes, offset, command.b.x) || !managed_detail::Read(bytes, offset, command.b.y) || !managed_detail::Read(bytes, offset, command.b.z)
            || !managed_detail::Read(bytes, offset, command.radius) || !managed_detail::Read(bytes, offset, command.color.x)
            || !managed_detail::Read(bytes, offset, command.color.y) || !managed_detail::Read(bytes, offset, command.color.z)
            || !managed_detail::Read(bytes, offset, command.color.w) || !managed_detail::Read(bytes, offset, depth) || depth > 1) return false;
        const auto finite = [](const math::Vector3& value) { return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z); };
        if (!finite(command.a) || !finite(command.b) || !std::isfinite(command.radius) || command.radius < 0
            || !std::isfinite(command.color.x) || !std::isfinite(command.color.y) || !std::isfinite(command.color.z) || !std::isfinite(command.color.w)) return false;
        command.type = kind == 0 ? ScriptDebugDrawType::Line : kind == 1 ? ScriptDebugDrawType::Sphere : ScriptDebugDrawType::Box;
        command.halfExtents = command.b; command.depthTest = depth != 0;
        if (kind == 2 && (command.b.x < 0 || command.b.y < 0 || command.b.z < 0)) return false;
        prepared.push_back(command);
    }
    commands = std::move(prepared);
    return true;
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
    if (!LoadCandidate(gameAssemblyPath, generation, types, internalMode)) return false;
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
            if (adapter == script) return adapter;
        return nullptr;
    };
    /// @note User callbacks may grow the script vector, so only stable identity crosses a callback boundary.
    const auto resolveEntry = [&](const Prepared& item) -> ScriptEntry* {
        auto* component = scene.GetComponent<ScriptComponent>(item.entity);
        if (!component) return nullptr;
        for (auto& entry : component->scripts)
            if (entry.inspectionId == item.inspectionId && entry.savedScriptId == item.savedScriptId
                && (!item.existing || entry.script.get() == item.existing))
                return &entry;
        return nullptr;
    };
    const auto discard = [&] {
        const std::string diagnostic = m_impl->diagnostic;
        for (auto& item : prepared) {
            if (!item.candidate) continue;
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
        if (!component || !object) continue;
        for (auto& entry : component->scripts) {
            if (entry.m_pendingRemoval) continue;
            auto* existing = findAdapter(entry.script.get());
            const std::string key = existing ? existing->m_typeName : entry.serialized ? entry.serialized->type : std::string{};
            if (!key.starts_with(kTypePrefix) && !key.starts_with("cs:")) continue;
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
            if (existing) item.candidate->m_referenceBirth = existing->m_referenceBirth;
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
                if (m_impl->diagnostic.empty()) m_impl->diagnostic = "Managed candidate field restore failed.";
                discard();
                return false;
            }
        }
    }
    /// @note Component cache が新世代の全個体を指せるよう、構築完了後に状態を復元する。失敗しても旧個体は未変更。
    for (auto& item : prepared) {
        if (!item.candidate) continue;
        if (!item.reloadState.empty()) {
            auto restore = InstanceRequest(ManagedBridgeOp::RESTORE_RELOAD, generation, item.candidate->InstanceId(), item.candidate->OwnerId());
            restore.input = item.reloadState.data();
            restore.inputBytes = static_cast<uint32_t>(item.reloadState.size());
            ManagedBridgeReply reply;
            if (!DispatchPrepared(*item.candidate, restore, reply)) { discard(); return false; }
        }
        if (!item.candidate->ResolveSavedComponentReferences()) { discard(); return false; }
        /// @note 停止済み entry の fault を snapshot が保持する。復元は許すが user callback は再実行しない。
        if (!inPlay && (!item.existing || !item.existing->IsRuntimeFaulted())
            && !Invoke(*item.candidate, ManagedCallback::AFTER_DESERIALIZE)) { discard(); return false; }
        if (!item.candidate->ValidateReferences()) { discard(); return false; }
    }
    for (const auto& item : prepared) {
        if (resolveEntry(item)) continue;
        m_impl->diagnostic = "Managed reload target changed before publication.";
        discard();
        return false;
    }
    const uint64_t oldGeneration = m_impl->generation;
    if (!Publish(generation, types)) { discard(); return false; }
    for (auto& item : prepared) {
        auto* entry = resolveEntry(item);
        if (!entry) continue;
        if (item.candidate && item.existing) {
            if (inPlay && !item.existing->m_runtimeFaulted) item.existing->Invoke(ManagedCallback::RELOAD_RETIRED);
            entry = resolveEntry(item);
            if (!entry) continue;
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
            if (entry && entry->m_started && item.existing->enabled && !item.existing->m_runtimeFaulted)
                item.existing->Invoke(ManagedCallback::CONNECT);
        }
    }
    const bool oldReferenced = std::any_of(m_impl->adapters.begin(), m_impl->adapters.end(),
        [oldGeneration](const auto& item) { return item.second->m_generation == oldGeneration; });
    if (oldReferenced) m_impl->deferredRetirements.insert(oldGeneration);
    else Retire(oldGeneration);
    return true;
}

void ManagedScriptRuntime::Unload(Scene* scene)
{
    m_impl->collectionPending = false;
    if (scene) {
        for (EntityID entity : scene->GetEntities<ScriptComponent>()) {
            auto* component = scene->GetComponent<ScriptComponent>(entity);
            if (!component) continue;
            for (auto& entry : component->scripts) {
                ManagedScriptAdapter* adapter = nullptr;
                for (const auto& [owner, candidate] : m_impl->adapters)
                    if (candidate == entry.script.get()) { adapter = candidate; break; }
                if (!adapter) continue;
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
    if (referenced) m_impl->deferredRetirements.insert(generation);
    else Retire(generation);
}

bool ManagedScriptRuntime::Shutdown()
{
    m_impl->collectionPending = false;
    if (std::this_thread::get_id() != m_impl->mainThread || m_impl->scope.callId
        || m_impl->generation || !m_impl->adapters.empty() || !m_impl->generationLeases.empty()) {
        m_impl->diagnostic = "Managed Host shutdown requires an idle main thread and all project generations/Adapters unloaded.";
        return false;
    }
    if (!m_impl->dispatch) return true;
    ManagedBridgeRequest request;
    request.op = ManagedBridgeOp::SHUTDOWN;
    ManagedBridgeReply reply;
    if (!Dispatch(request, reply)) return false;
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

int32_t __cdecl ManagedScriptRuntime::NativeDispatch(const ManagedBridgeRequest* request, ManagedBridgeReply* reply)
{
    if (!request || !reply) return static_cast<int32_t>(ManagedBridgeStatus::INVALID_ARGUMENT);
    *reply = {};
    const auto finish = [&](ManagedBridgeStatus status) {
        reply->status = status;
        return static_cast<int32_t>(status);
    };
    auto& runtime = Instance();
    auto& impl = *runtime.m_impl;
    if (request->abiVersion != kManagedBridgeAbiVersion) return finish(ManagedBridgeStatus::ABI_MISMATCH);
    if (request->inputBytes && !request->input)
        return finish(ManagedBridgeStatus::INVALID_ARGUMENT);
    if (request->op == ManagedBridgeOp::DIAGNOSTIC) {
        const std::string text(request->input ? request->input : "", request->inputBytes);
        FBZZ_LOG_ERROR("Managed Script: %s", text.c_str());
        return finish(ManagedBridgeStatus::OK);
    }
    const auto output = [&](std::string_view text) {
        if (text.size() > (std::numeric_limits<uint32_t>::max)())
            return finish(ManagedBridgeStatus::INVALID_ARGUMENT);
        reply->outputBytes = static_cast<uint32_t>(text.size());
        if (text.size() > request->outputCapacity || (text.size() && !request->output))
            return finish(ManagedBridgeStatus::BUFFER_TOO_SMALL);
        if (!text.empty()) std::memcpy(request->output, text.data(), text.size());
        return finish(ManagedBridgeStatus::OK);
    };
    const auto resolveGuid = [&](ManagedScriptAdapter& ownerAdapter) {
        ScriptObjectReference reference;
        reference.guid.assign(request->input ? request->input : "", request->inputBytes);
        if (!runtime.ResolveReference(ownerAdapter, reference)) return finish(ManagedBridgeStatus::INVALID_ARGUMENT);
        if (reference.id.IsValid()) {
            GameObject* object = ownerAdapter.m_scene->GetGameObject(reference.id);
            reply->integer = std::bit_cast<int64_t>(impl.RegisterObject(*ownerAdapter.m_scene, *object));
            reply->callId = ownerAdapter.m_scene->GetRenderSceneGeneration();
        }
        reply->valueKind = ManagedValueKind::GAME_OBJECT;
        return output(reference.guid);
    };
    const auto hasScriptEntry = [&](ManagedScriptAdapter& caller, GameObject* target) {
        (void)caller;
        if (request->flags != 2 || request->index != UINT32_MAX || !request->inputBytes || request->inputBytes > 128
            || (target && !target->GetScene()))
            return finish(ManagedBridgeStatus::INVALID_ARGUMENT);
        const std::string_view savedId(request->input, request->inputBytes);
        if (savedId.find('\0') != std::string_view::npos) return finish(ManagedBridgeStatus::INVALID_ARGUMENT);
        reply->integer = 0;
        if (target)
            if (const auto* component = target->GetComponent<ScriptComponent>())
                for (const auto& entry : component->scripts)
                    if (!entry.m_pendingRemoval && entry.savedScriptId == savedId) { reply->integer = 1; break; }
        return finish(ManagedBridgeStatus::OK);
    };
    const auto resolveComponent = [&](ManagedScriptAdapter& ownerAdapter) {
        if (!ownerAdapter.HasLiveContext() || request->flags) return finish(ManagedBridgeStatus::INVALID_SCOPE);
        const std::string_view input(request->input ? request->input : "", request->inputBytes);
        const size_t separator = input.find('\n');
        if (separator == std::string_view::npos || separator == 0 || input.find('\n', separator + 1) != std::string_view::npos
            || input.find('\0') != std::string_view::npos) return finish(ManagedBridgeStatus::INVALID_ARGUMENT);
        const std::string_view guid = input.substr(0, separator), savedId = input.substr(separator + 1);
        GameObject* target = nullptr;
        for (auto& object : ownerAdapter.m_scene->GameObjects()) if (object.instanceId == guid) {
            if (target) return finish(ManagedBridgeStatus::INVALID_ARGUMENT);
            target = &object;
        }
        if (!target) return finish(ManagedBridgeStatus::NOT_FOUND);
        uint64_t lifetime = 0;
        std::string referenceBirth;
        if (request->index == UINT32_MAX) {
            if (!ScriptFactory::IsValidSavedScriptId(savedId)) return finish(ManagedBridgeStatus::INVALID_ARGUMENT);
            auto* scripts = target->GetComponent<ScriptComponent>();
            ManagedScriptAdapter* resolved = nullptr;
            if (scripts) for (auto& entry : scripts->scripts) if (!entry.m_pendingRemoval && entry.savedScriptId == savedId) {
                if (resolved) return finish(ManagedBridgeStatus::INVALID_ARGUMENT);
                resolved = runtime.FindAdapter(entry.script.get());
            }
            if (!resolved) return finish(ManagedBridgeStatus::NOT_FOUND);
            reply->instanceId = resolved->InstanceId();
            referenceBirth = resolved->m_referenceBirth;
        } else {
            if (!savedId.empty()) return finish(ManagedBridgeStatus::INVALID_ARGUMENT);
            if (managed_detail::ManagedObjectControls::IsPendingComponentRemoval(*target, request->index)) return finish(ManagedBridgeStatus::NOT_FOUND);
            bool exists = false;
            const auto status = managed_detail::ManagedObjectControls::Component(*target, request->index, ManagedBridgeOp::COMPONENT_READ, exists, &lifetime);
            if (status != ManagedBridgeStatus::OK) return finish(status);
            if (!exists || (request->index != 0 && !lifetime)) return finish(ManagedBridgeStatus::NOT_FOUND);
        }
        const uint64_t objectId = impl.RegisterObject(*ownerAdapter.m_scene, *target);
        if (request->index == 0) lifetime = objectId;
        reply->integer = std::bit_cast<int64_t>(objectId); reply->callId = ownerAdapter.m_scene->GetRenderSceneGeneration();
        reply->generation = lifetime;
        return output(request->index == UINT32_MAX ? std::string(guid) + '\n' + referenceBirth : std::string(guid));
    };
    ManagedScriptAdapter* preparingAdapter = impl.preparation;
    if (!preparingAdapter && !impl.settingsPreparation.empty()) {
        const auto found = std::find_if(impl.settingsPreparation.begin(), impl.settingsPreparation.end(), [&](const ManagedScriptAdapter* adapter) {
            return request->generation == adapter->Generation() && request->instanceId == adapter->InstanceId() && request->ownerId == adapter->OwnerId();
        });
        if (found != impl.settingsPreparation.end()) preparingAdapter = *found;
    }
    if ((request->op == ManagedBridgeOp::RESOLVE_GUID || request->op == ManagedBridgeOp::RESOLVE_COMPONENT_REFERENCE || request->op == ManagedBridgeOp::OBJECT_IS_ALIVE
        || request->op == ManagedBridgeOp::COMPONENT_READ) && preparingAdapter) {
        auto& preparing = *preparingAdapter;
        if (std::this_thread::get_id() != impl.mainThread || request->callId
            || request->generation != preparing.Generation() || request->instanceId != preparing.InstanceId()
            || request->ownerId != preparing.OwnerId()) return finish(ManagedBridgeStatus::INVALID_SCOPE);
        if (request->op == ManagedBridgeOp::RESOLVE_GUID) return resolveGuid(preparing);
        if (request->op == ManagedBridgeOp::RESOLVE_COMPONENT_REFERENCE) return resolveComponent(preparing);
        if (request->op == ManagedBridgeOp::COMPONENT_READ) {
            GameObject* target = preparing.HasLiveContext()
                ? impl.ResolveObject(*preparing.m_scene, std::bit_cast<uint64_t>(request->integer)) : nullptr;
            if (request->flags == 2) return hasScriptEntry(preparing, target);
            if (request->flags > 1 || request->index == UINT32_MAX || request->inputBytes)
                return finish(ManagedBridgeStatus::INVALID_ARGUMENT);
            if (!request->flags && std::bit_cast<uint64_t>(request->integer) != preparing.m_objectId) return finish(ManagedBridgeStatus::INVALID_SCOPE);
            if (!target) return finish(ManagedBridgeStatus::NOT_FOUND);
            bool exists = false; uint64_t lifetime = 0;
            const auto status = managed_detail::ManagedObjectControls::Component(*target, request->index,
                ManagedBridgeOp::COMPONENT_READ, exists, request->flags ? &lifetime : nullptr);
            if (request->index == 0 && exists) lifetime = std::bit_cast<uint64_t>(request->integer);
            reply->integer = request->flags ? std::bit_cast<int64_t>(lifetime) : (exists ? 1 : 0);
            return finish(status);
        }
        reply->valueKind = ManagedValueKind::BOOL;
        reply->integer = preparing.HasLiveContext()
            && impl.ResolveObject(*preparing.m_scene, std::bit_cast<uint64_t>(request->integer)) ? 1 : 0;
        return finish(ManagedBridgeStatus::OK);
    }
    const auto& scope = impl.scope;
    if (std::this_thread::get_id() != impl.mainThread || !scope.callId || request->callId != scope.callId
        || request->generation != scope.generation || request->instanceId != scope.instanceId || request->ownerId != scope.ownerId)
        return finish(ManagedBridgeStatus::INVALID_SCOPE);
    const std::string_view policy = ManagedCallbackPolicy(scope.callback);
    if (policy == "no_engine_api" || (policy == "subscription_only" && !IsConnectionOperation(*request))
        || (policy == "draw_readonly" && !IsDrawOperation(*request)))
        return finish(ManagedBridgeStatus::INVALID_SCOPE);
    const auto owner = impl.adapters.find(request->ownerId);
    if (owner == impl.adapters.end()) return finish(ManagedBridgeStatus::NOT_FOUND);
    auto& adapter = *owner->second;
    if (adapter.m_generation != request->generation || adapter.m_instanceId != request->instanceId)
        return finish(ManagedBridgeStatus::STALE_GENERATION);
    if (!adapter.HasLiveContext()) return finish(ManagedBridgeStatus::NOT_FOUND);
    if (adapter.Generation() != impl.generation) return finish(ManagedBridgeStatus::STALE_GENERATION);
    Scene& scene = *adapter.m_scene;
    if (request->op == ManagedBridgeOp::RESOLVE_COMPONENT_REFERENCE) return resolveComponent(adapter);
    if (request->op >= ManagedBridgeOp::REGISTER_DELAY && request->op <= ManagedBridgeOp::PUBLISH_EVENT) {
        if (!Script::IsInPlayMode()) return finish(ManagedBridgeStatus::INVALID_SCOPE);
        const std::string_view input(request->input ? request->input : "", request->inputBytes);
        if (request->op == ManagedBridgeOp::PUBLISH_EVENT)
            return finish(managed_detail::PublishManagedEvent(input));
        size_t offset = 0;
        uint64_t receiverId = 0;
        if (!managed_detail::Read(input, offset, receiverId)
            || (request->op != ManagedBridgeOp::SUBSCRIBE_EVENT && input.size() != 8))
            return finish(ManagedBridgeStatus::INVALID_ARGUMENT);
        ManagedScriptAdapter* receiver = nullptr;
        for (const auto& [id, candidate] : impl.adapters)
            if (candidate->InstanceId() == receiverId && candidate->Generation() == impl.generation
                && candidate->HasLiveContext() && candidate->m_scene == &scene) { receiver = candidate; break; }
        if (!receiver) return finish(ManagedBridgeStatus::NOT_FOUND);
        const uint64_t token = std::bit_cast<uint64_t>(request->integer);
        switch (request->op) {
        case ManagedBridgeOp::REGISTER_DELAY:
        case ManagedBridgeOp::REGISTER_TWEEN:
            if (!receiver->scene.IsActiveAndEnabled()) return finish(ManagedBridgeStatus::INVALID_SCOPE);
            return finish(managed_detail::RegisterManagedJob(*receiver, *request));
        case ManagedBridgeOp::SUBSCRIBE_EVENT:
            if (!receiver->scene.IsActiveAndEnabled() || request->flags || request->index)
                return finish(ManagedBridgeStatus::INVALID_SCOPE);
            return finish(managed_detail::SubscribeManagedEvent(*receiver, token, input.substr(offset)));
        case ManagedBridgeOp::CANCEL_JOB:
        case ManagedBridgeOp::UNSUBSCRIBE_EVENT:
            if (!token || request->flags || request->index) return finish(ManagedBridgeStatus::INVALID_ARGUMENT);
            managed_detail::CancelManagedJob(*receiver, token);
            return finish(ManagedBridgeStatus::OK);
        case ManagedBridgeOp::CANCEL_OWNER:
            if (token || request->flags || request->index) return finish(ManagedBridgeStatus::INVALID_ARGUMENT);
            managed_detail::CancelManagedOwner(*receiver);
            return finish(ManagedBridgeStatus::OK);
        default: return finish(ManagedBridgeStatus::UNSUPPORTED);
        }
    }
    if (request->op == ManagedBridgeOp::READ_ASSET) {
        if (request->flags == 1) {
            if (request->integer || request->index || request->reserved0 || request->reserved1 || impl.assetReadIdsExhausted || impl.assetReads.size() >= 32)
                return finish(ManagedBridgeStatus::INVALID_ARGUMENT);
            std::string json;
            const auto status = managed_detail::ReadDataAsset(std::string_view(request->input ? request->input : "", request->inputBytes), json);
            if (status != ManagedBridgeStatus::OK) return finish(status);
            const uint64_t token = impl.nextAssetRead;
            if (token == UINT64_MAX) impl.assetReadIdsExhausted = true; else ++impl.nextAssetRead;
            reply->integer = std::bit_cast<int64_t>(token); reply->generation = json.size();
            impl.assetReads.emplace(token, Impl::AssetRead{scope, scene.GetRenderSceneGeneration(), adapter.m_objectId, std::move(json)});
            return finish(ManagedBridgeStatus::OK);
        }
        const uint64_t token = std::bit_cast<uint64_t>(request->integer);
        const auto found = impl.assetReads.find(token);
        if (found == impl.assetReads.end()) return finish(ManagedBridgeStatus::NOT_FOUND);
        const auto& captured = found->second;
        if (captured.caller.generation != scope.generation || captured.caller.instanceId != scope.instanceId || captured.caller.ownerId != scope.ownerId
            || captured.caller.callId != scope.callId || captured.sceneLifetime != scene.GetRenderSceneGeneration() || captured.objectId != adapter.m_objectId)
            return finish(ManagedBridgeStatus::INVALID_SCOPE);
        if (request->flags == 3) { impl.assetReads.erase(found); return finish(ManagedBridgeStatus::OK); }
        if (request->flags != 2 || request->inputBytes || request->index > 65536) return finish(ManagedBridgeStatus::INVALID_ARGUMENT);
        const uint64_t offset = static_cast<uint64_t>(request->reserved0) | (static_cast<uint64_t>(request->reserved1) << 32);
        if (offset > captured.bytes.size()) return finish(ManagedBridgeStatus::INVALID_ARGUMENT);
        const size_t count = std::min<size_t>(request->index, captured.bytes.size() - static_cast<size_t>(offset));
        reply->generation = captured.bytes.size(); reply->integer = std::bit_cast<int64_t>(offset + count);
        reply->flags = offset + count == captured.bytes.size() ? 1U : 0U;
        return output(std::string_view(captured.bytes).substr(static_cast<size_t>(offset), count));
    }
    const bool lifecycle = request->op == ManagedBridgeOp::SCENE_CREATE || request->op == ManagedBridgeOp::SCENE_INSTANTIATE
        || request->op == ManagedBridgeOp::OBJECT_DESTROY || request->op == ManagedBridgeOp::SCENE_CAN_CREATE
        || request->op == ManagedBridgeOp::OBJECTS_DESTROY;
    const bool physics = request->op >= ManagedBridgeOp::PHYSICS_BODY_READ && request->op <= ManagedBridgeOp::PHYSICS_OVERLAP_SPHERE
        && request->op != ManagedBridgeOp::OBJECTS_DESTROY;
    if ((lifecycle || physics) && !Script::IsInPlayMode()) return finish(ManagedBridgeStatus::INVALID_SCOPE);
    if ((request->op == ManagedBridgeOp::COMPONENT_ADD || request->op == ManagedBridgeOp::COMPONENT_REMOVE
        || request->op == ManagedBridgeOp::SCENE_SPAWN_MANY) && !Script::IsInPlayMode())
        return finish(ManagedBridgeStatus::INVALID_SCOPE);
    if ((request->op == ManagedBridgeOp::OBJECTS_DESTROY || request->op == ManagedBridgeOp::OBJECTS_READ_WORLD_POSITIONS
        || request->op == ManagedBridgeOp::OBJECTS_SET_WORLD_POSITIONS || request->op == ManagedBridgeOp::OBJECTS_TRANSLATE_WORLD) && request->flags >> 8) {
        struct ResolutionContext { Impl& impl; Scene& scene; } context{impl, scene};
        const auto resolve = [](void* state, uint64_t token, GameObject*& object) {
            auto& context = *static_cast<ResolutionContext*>(state);
            const auto found = context.impl.objects.Find(token);
            if (found && found->scene != &context.scene) return ManagedBridgeStatus::SCENE_MISMATCH;
            object = context.impl.ResolveObject(context.scene, token); return ManagedBridgeStatus::OK;
        };
        return finish(impl.batchTransfers.Run(scene, *request, *reply, impl.transformBatch, resolve, &context));
    }
    if (request->op == ManagedBridgeOp::OBJECTS_DESTROY) {
        if (request->flags || !std::isfinite(request->x) || request->x < 0.0f
            || request->inputBytes != static_cast<uint64_t>(request->index) * kManagedBatchReferenceBytes)
            return finish(ManagedBridgeStatus::INVALID_ARGUMENT);
        std::vector<GameObject*> targets;
        targets.reserve(request->index);
        const std::string_view input(request->input ? request->input : "", request->inputBytes);
        size_t offset = 0;
        for (uint32_t index = 0; index < request->index; ++index) {
            uint64_t token = 0;
            uint64_t lifetime = 0;
            if (!managed_detail::Read(input, offset, token) || !managed_detail::Read(input, offset, lifetime)
                || (token != 0 && lifetime == 0)) return finish(ManagedBridgeStatus::INVALID_ARGUMENT);
            if (lifetime && lifetime != scene.GetRenderSceneGeneration()) return finish(ManagedBridgeStatus::SCENE_MISMATCH);
            const auto found = impl.objects.Find(token);
            if (found && found->scene != &scene) return finish(ManagedBridgeStatus::SCENE_MISMATCH);
            targets.push_back(token ? impl.ResolveObject(scene, token) : nullptr);
        }
        return finish(managed_detail::ManagedObjectLifecycle::DestroyMany(scene, targets, request->x, reply->index, reply->flags));
    }
    if (request->op == ManagedBridgeOp::OBJECTS_READ_WORLD_POSITIONS
        || request->op == ManagedBridgeOp::OBJECTS_SET_WORLD_POSITIONS
        || request->op == ManagedBridgeOp::OBJECTS_TRANSLATE_WORLD) {
        struct ResolutionContext { Impl& impl; Scene& scene; } context{impl, scene};
        const auto resolve = [](void* state, uint64_t token, GameObject*& object) {
            auto& context = *static_cast<ResolutionContext*>(state);
            const auto found = context.impl.objects.Find(token);
            if (found && found->scene != &context.scene)
                return ManagedBridgeStatus::SCENE_MISMATCH;
            object = context.impl.ResolveObject(context.scene, token);
            return ManagedBridgeStatus::OK;
        };
        return finish(impl.transformBatch.Run(scene, *request, *reply, resolve, &context));
    }
    if (request->op == ManagedBridgeOp::SERVICE_DISPLAY && request->flags >> 8)
        return finish(impl.batchTransfers.ReadSnapshot(scene, *request, *reply));
    if (request->op == ManagedBridgeOp::AI_CROWD_STEP) {
        if (!Script::IsInPlayMode()) return finish(ManagedBridgeStatus::INVALID_SCOPE);
        if (request->index == 0) return finish(impl.aiCrowdTransfer.Run(*request, *reply));
        if (request->index != 1) return finish(ManagedBridgeStatus::INVALID_ARGUMENT);
        struct ResolutionContext { Impl& impl; Scene& scene; } context{impl, scene};
        const auto resolve = [](void* state, uint64_t token, GameObject*& object) {
            auto& context = *static_cast<ResolutionContext*>(state);
            const auto found = context.impl.objects.Find(token);
            if (found && found->scene != &context.scene) return ManagedBridgeStatus::SCENE_MISMATCH;
            object = context.impl.ResolveObject(context.scene, token);
            return ManagedBridgeStatus::OK;
        };
        return finish(impl.aiPoseTransfer.Run(scene, *request, *reply, resolve, &context));
    }
    GameObject* target = impl.ResolveObject(scene, std::bit_cast<uint64_t>(request->integer));
    if (request->op == ManagedBridgeOp::SERVICE_DISPLAY && request->index == 9) {
        if (request->flags != 1 || !target || target != adapter.m_gameObject || request->inputBytes != 24 || !request->input
            || std::any_of(request->input, request->input + 24, [](char value) { return value != 0; })) return finish(ManagedBridgeStatus::INVALID_ARGUMENT);
        if (!core::Application::Get().GetWindowWidth()) return finish(ManagedBridgeStatus::NOT_FOUND);
        const auto modes = adapter.display.EnumResolutions(); if (modes.size() > UINT32_MAX) return finish(ManagedBridgeStatus::INVALID_ARGUMENT);
        std::string bytes; managed_detail::Append(bytes, static_cast<uint32_t>(modes.size()));
        for (const auto& mode : modes) { managed_detail::Append(bytes, mode.width); managed_detail::Append(bytes, mode.height); }
        reply->index = static_cast<uint32_t>(modes.size()); reply->callId = scene.GetRenderSceneGeneration();
        if (bytes.size() > request->outputCapacity || !request->output) {
            const auto captured = impl.batchTransfers.OpenRead(scene, *request, std::move(bytes), static_cast<uint32_t>(modes.size()), *reply);
            return finish(captured == ManagedBridgeStatus::OK ? ManagedBridgeStatus::BUFFER_TOO_SMALL : captured);
        }
        return output(bytes);
    }
    const auto objectReply = [&](GameObject& object) {
        reply->valueKind = ManagedValueKind::GAME_OBJECT;
        reply->integer = std::bit_cast<int64_t>(impl.RegisterObject(scene, object));
        reply->callId = scene.GetRenderSceneGeneration();
        return output(object.instanceId);
    };
    if ((request->op >= ManagedBridgeOp::COLLIDER_READ && request->op <= ManagedBridgeOp::PHYSICS_BODY_WRITE_ROTATION)
        || (request->op >= ManagedBridgeOp::SERVICE_ANIMATOR && request->op <= ManagedBridgeOp::SERVICE_ASSETS)
        || (request->op >= ManagedBridgeOp::SERVICE_LINE && request->op <= ManagedBridgeOp::SERVICE_INSTANCES)) {
        if (!Script::IsInPlayMode() && request->op <= ManagedBridgeOp::SERVICE_CHARACTER) return finish(ManagedBridgeStatus::INVALID_SCOPE);
        if (!target) return finish(ManagedBridgeStatus::NOT_FOUND);
        const bool paged = request->flags == 257U;
        const bool storeRaw = (request->op == ManagedBridgeOp::SERVICE_SAVE || request->op == ManagedBridgeOp::SERVICE_CONFIG)
            && request->index >= 23 && request->index <= 25;
        const bool materialRaw = request->op == ManagedBridgeOp::SERVICE_MATERIAL && (request->index == 31 || (request->index >= 33 && request->index <= 35));
        const bool jointRaw = request->op == ManagedBridgeOp::SERVICE_JOINT && request->index >= 20 && request->index <= 22;
        const bool textureRaw = request->op == ManagedBridgeOp::SERVICE_ASSETS && request->index >= 12 && request->index <= 14;
        if ((!paged && request->flags > 1) || ((storeRaw || jointRaw || textureRaw) && request->flags) || request->inputBytes < (storeRaw || materialRaw || jointRaw || textureRaw ? 8U : 24U)
            || (paged && (!request->output || !request->outputCapacity || request->outputCapacity > 1048576)))
            return finish(ManagedBridgeStatus::INVALID_ARGUMENT);
        /// @note 変更要求には固定最大応答を先に要求し、成功した変更を容量不足で再送しない。
        if (!(request->flags & 1U) && (!request->output || request->outputCapacity < 1024)) {
            reply->outputBytes = 1024; return finish(ManagedBridgeStatus::BUFFER_TOO_SMALL);
        }
        if (request->op == ManagedBridgeOp::SERVICE_MATERIAL && request->index == 31)
            return finish(impl.servicesBridge.ReadMaterialPacket(scene, adapter, *target, *request, *reply));
        size_t offset = 0; uint64_t relatedId = 0;
        if (!storeRaw && !materialRaw && !jointRaw && !textureRaw && !managed_detail::Read(std::string_view(request->input, request->inputBytes), offset, relatedId)) return finish(ManagedBridgeStatus::INVALID_ARGUMENT);
        const auto found = impl.objects.Find(relatedId);
        if (found && found->scene != &scene) return finish(ManagedBridgeStatus::SCENE_MISMATCH);
        GameObject* related = relatedId ? impl.ResolveObject(scene, relatedId) : nullptr;
        if (relatedId && !related) return finish(ManagedBridgeStatus::NOT_FOUND);
        managed_detail::ManagedServiceResult result;
        auto serviceRequest = *request;
        if (paged) serviceRequest.flags = 1;
        ManagedScriptAdapter* serviceOwner = &adapter;
        if (request->op == ManagedBridgeOp::SERVICE_CURSOR && request->index <= 3) {
            uint64_t receiverOwner = 0;
            if (!managed_detail::ManagedServicesBridge::CursorOwner(serviceRequest, receiverOwner)) return finish(ManagedBridgeStatus::INVALID_ARGUMENT);
            const auto receiver = impl.adapters.find(receiverOwner);
            if (receiver == impl.adapters.end() || receiver->second->Generation() != impl.generation
                || !receiver->second->HasLiveContext() || receiver->second->m_scene != &scene || receiver->second->m_gameObject != target)
                return finish(ManagedBridgeStatus::INVALID_SCOPE);
            serviceOwner = receiver->second;
            if (request->index == 0 && (serviceOwner->IsRuntimeFaulted() || serviceOwner->RequirementsBlocked()
                || !serviceOwner->scene.IsActiveAndEnabled())) return finish(ManagedBridgeStatus::INVALID_SCOPE);
            serviceRequest.ownerId = receiverOwner;
        }
        ManagedBridgeStatus status;
        if (jointRaw) {
            struct ResolveContext { Impl& impl; Scene& scene; } context{impl, scene};
            const auto resolve = [](void* state, uint64_t token, GameObject*& object) {
                auto& context = *static_cast<ResolveContext*>(state); const auto found = context.impl.objects.Find(token);
                if (found && found->scene != &context.scene) return ManagedBridgeStatus::SCENE_MISMATCH;
                object = context.impl.ResolveObject(context.scene, token); return ManagedBridgeStatus::OK;
            };
            status = impl.servicesBridge.RunJointPacket(scene, *target, *request, result, resolve, &context, scope.callback == ManagedCallback::FIXED_UPDATE);
        } else status = impl.servicesBridge.Run(scene, *serviceOwner, *target, related, serviceRequest, result,
            scope.callback == ManagedCallback::FIXED_UPDATE, scope.callback >= ManagedCallback::COLLISION_ENTER && scope.callback <= ManagedCallback::TRIGGER_EXIT);
        if (status != ManagedBridgeStatus::OK) return finish(status);
        const bool blend = request->op == ManagedBridgeOp::SERVICE_ANIMATOR && request->index == 24;
        const size_t valueCount = blend ? result.blendWeights.size() : result.values.size();
        const size_t textCount = blend ? result.blendWeights.size() : result.text.size() + (result.hasBorrowedText ? 1U : 0U);
        if (valueCount > UINT32_MAX || textCount > UINT32_MAX) return finish(ManagedBridgeStatus::INVALID_ARGUMENT);
        if (result.object && (result.object->GetScene() != &scene || scene.GetGameObject(result.object->GetID()) != result.object)) return finish(ManagedBridgeStatus::SCENE_MISMATCH);
        const std::string_view guid = result.object ? std::string_view(result.object->instanceId) : std::string_view{};
        const auto textAt = [&](size_t index) -> std::string_view {
            return blend ? std::string_view(result.blendWeights[index].first) : index == result.text.size() && result.hasBorrowedText ? result.borrowedText : std::string_view(result.text[index]);
        };
        uint64_t total = 20 + static_cast<uint64_t>(valueCount) * 4 + guid.size();
        for (size_t index = 0; index < textCount; ++index) {
            const auto text = textAt(index);
            if (text.size() > UINT32_MAX || UINT64_MAX - total < text.size() + 4) return finish(ManagedBridgeStatus::INVALID_ARGUMENT);
            total += text.size() + 4;
        }
        const uint64_t start = paged ? (static_cast<uint64_t>(request->reserved1) << 32) | request->reserved0 : 0;
        if (start > total) return finish(ManagedBridgeStatus::INVALID_ARGUMENT);
        if (!paged && (total > request->outputCapacity || (total && !request->output))) {
            reply->outputBytes = total > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(total);
            return finish(ManagedBridgeStatus::BUFFER_TOO_SMALL);
        }
        uint64_t cursor = 0;
        uint32_t written = 0;
        /// @note 必要範囲だけを caller buffer へ転送し、全応答 byte string を一時生成しない。
        const auto append = [&](const char* data, size_t length) {
            const uint64_t end = cursor + length;
            if (end > start && written < request->outputCapacity) {
                const size_t skip = start > cursor ? static_cast<size_t>(start - cursor) : 0;
                const size_t bytes = std::min(length - skip, static_cast<size_t>(request->outputCapacity - written));
                if (bytes) std::memcpy(request->output + written, data + skip, bytes);
                written += static_cast<uint32_t>(bytes);
            }
            cursor = end;
        };
        const auto scalar = [&]<typename T>(const T& value) { append(std::bit_cast<const char*>(&value), sizeof(value)); };
        scalar(static_cast<uint32_t>(valueCount)); scalar(static_cast<uint32_t>(textCount));
        for (size_t index = 0; index < valueCount; ++index) scalar(blend ? result.blendWeights[index].second : result.values[index]);
        for (size_t index = 0; index < textCount; ++index) {
            const auto text = textAt(index); scalar(static_cast<uint32_t>(text.size())); append(text.data(), text.size());
        }
        scalar(result.object ? impl.RegisterObject(scene, *result.object) : uint64_t{0});
        scalar(static_cast<uint32_t>(guid.size())); append(guid.data(), guid.size());
        reply->integer = result.integer; reply->callId = scene.GetRenderSceneGeneration();
        reply->outputBytes = written;
        if (paged) { reply->generation = total; reply->flags = start + written == total ? 1U : 0U; }
        return finish(ManagedBridgeStatus::OK);
    }
    switch (request->op) {
    case ManagedBridgeOp::OBJECTS_OWN: {
        if (!Script::IsInPlayMode() || request->flags
            || request->inputBytes != static_cast<uint64_t>(request->index) * 16U) return finish(ManagedBridgeStatus::INVALID_ARGUMENT);
        ManagedScriptAdapter* receiver = nullptr;
        for (const auto& [id, candidate] : impl.adapters)
            if (candidate->InstanceId() == std::bit_cast<uint64_t>(request->integer) && candidate->Generation() == impl.generation
                && candidate->HasLiveContext() && candidate->m_scene == &scene) { receiver = candidate; break; }
        if (!receiver) return finish(ManagedBridgeStatus::NOT_FOUND);
        std::vector<EntityID> entities;
        const auto previous = impl.ownedObjects.find(receiver->OwnerId());
        if (previous != impl.ownedObjects.end() && previous->second.scene == &scene && previous->second.lifetime == scene.GetRenderSceneGeneration())
            for (EntityID entity : previous->second.entities) if (scene.GetGameObject(entity)) entities.push_back(entity);
        size_t offset = 0;
        const std::string_view bytes(request->input ? request->input : "", request->inputBytes);
        for (uint32_t index = 0; index < request->index; ++index) {
            uint64_t token = 0, lifetime = 0;
            if (!managed_detail::Read(bytes, offset, token) || !managed_detail::Read(bytes, offset, lifetime)
                || lifetime != scene.GetRenderSceneGeneration()) return finish(ManagedBridgeStatus::SCENE_MISMATCH);
            GameObject* object = impl.ResolveObject(scene, token);
            if (!object) return finish(ManagedBridgeStatus::NOT_FOUND);
            if (std::find(entities.begin(), entities.end(), object->GetID()) == entities.end()) entities.push_back(object->GetID());
        }
        impl.ownedObjects.insert_or_assign(receiver->OwnerId(), Impl::OwnedObjects{&scene, scene.GetRenderSceneGeneration(), std::move(entities)});
        return finish(ManagedBridgeStatus::OK);
    }
    case ManagedBridgeOp::OBJECT_READ: {
        if (!target) return finish(ManagedBridgeStatus::NOT_FOUND);
        if (request->index == 5) {
            GameObject* parent = target->GetParent();
            return parent ? objectReply(*parent) : finish(ManagedBridgeStatus::NOT_FOUND);
        }
        std::string text;
        const auto status = managed_detail::ManagedObjectControls::Read(*target, request->index, *reply, text);
        return status == ManagedBridgeStatus::OK ? output(text) : finish(status);
    }
    case ManagedBridgeOp::OBJECT_WRITE: {
        if (!target) return finish(ManagedBridgeStatus::NOT_FOUND);
        if (request->flags) return finish(ManagedBridgeStatus::INVALID_ARGUMENT);
        const std::string_view bytes(request->input ? request->input : "", request->inputBytes);
        size_t offset = 0;
        int64_t value = 0;
        if (!managed_detail::Read(bytes, offset, value)) return finish(ManagedBridgeStatus::INVALID_ARGUMENT);
        return finish(managed_detail::ManagedObjectControls::Write(*target, request->index, value, bytes.substr(offset)));
    }
    case ManagedBridgeOp::OBJECT_SET_PARENT: {
        if (!target || request->flags || request->index || request->inputBytes != 8) return finish(ManagedBridgeStatus::INVALID_ARGUMENT);
        size_t offset = 0; uint64_t token = 0;
        const std::string_view bytes(request->input, request->inputBytes);
        if (!managed_detail::Read(bytes, offset, token)) return finish(ManagedBridgeStatus::INVALID_ARGUMENT);
        const auto found = impl.objects.Find(token);
        if (found && found->scene != &scene) return finish(ManagedBridgeStatus::SCENE_MISMATCH);
        GameObject* parent = token ? impl.ResolveObject(scene, token) : nullptr;
        if (token && !parent) return finish(ManagedBridgeStatus::NOT_FOUND);
        reply->integer = managed_detail::ManagedObjectControls::SetParent(*target, parent) ? 1 : 0;
        return finish(ManagedBridgeStatus::OK);
    }
    case ManagedBridgeOp::COMPONENT_READ:
    case ManagedBridgeOp::COMPONENT_ADD:
    case ManagedBridgeOp::COMPONENT_REMOVE: {
        if (request->op == ManagedBridgeOp::COMPONENT_READ && request->flags == 2) return hasScriptEntry(adapter, target);
        if (!target) return finish(ManagedBridgeStatus::NOT_FOUND);
        if (request->index != UINT32_MAX) {
            if (request->flags > (request->op == ManagedBridgeOp::COMPONENT_READ ? 1U : 0U) || request->inputBytes)
                return finish(ManagedBridgeStatus::INVALID_ARGUMENT);
            bool exists = false; uint64_t lifetime = 0;
            const auto status = managed_detail::ManagedObjectControls::Component(*target, request->index, request->op, exists,
                request->flags ? &lifetime : nullptr);
            if (request->index == 0 && exists) lifetime = std::bit_cast<uint64_t>(request->integer);
            reply->integer = request->flags ? std::bit_cast<int64_t>(lifetime) : (exists ? 1 : 0);
            if (status == ManagedBridgeStatus::INVALID_ARGUMENT && request->op == ManagedBridgeOp::COMPONENT_ADD
                && managed_detail::ManagedObjectControls::IsPendingComponentRemoval(*target, request->index)) {
                output("The Component is pending removal; wait until Cleanup completes before adding this type.");
                return finish(status);
            }
            return finish(status);
        }
        if (request->op == ManagedBridgeOp::COMPONENT_ADD) {
            const std::string_view source(request->input ? request->input : "", request->inputBytes);
            std::string type;
            if (!ScriptFactory::ResolveTypeName(source, type)) return finish(ManagedBridgeStatus::NOT_FOUND);
            ScriptEntry entry;
            std::unique_ptr<Script> created = ScriptFactory::Create(type, entry.savedScriptId);
            auto* managed = runtime.FindAdapter(created.get());
            if (!managed) return finish(ManagedBridgeStatus::LOAD_FAILED);
            created->SetContext(&scene, target);
            if (!runtime.BindOwner(*managed)) return finish(ManagedBridgeStatus::LOAD_FAILED);
            const uint64_t instance = managed->InstanceId();
            auto* component = target->GetComponent<ScriptComponent>();
            if (!component) component = &target->AddComponent<ScriptComponent>();
            entry.script = std::move(created);
            component->scripts.push_back(std::move(entry));
            reply->integer = std::bit_cast<int64_t>(instance);
            return finish(ManagedBridgeStatus::OK);
        }
        if (request->op != ManagedBridgeOp::COMPONENT_REMOVE || request->inputBytes != 8) return finish(ManagedBridgeStatus::INVALID_ARGUMENT);
        uint64_t instance = 0; size_t offset = 0;
        if (!managed_detail::Read(std::string_view(request->input, request->inputBytes), offset, instance)) return finish(ManagedBridgeStatus::INVALID_ARGUMENT);
        if (auto* component = target->GetComponent<ScriptComponent>())
            for (ScriptEntry& entry : component->scripts) {
                auto* managed = runtime.FindAdapter(entry.script.get());
                if (!managed || managed->InstanceId() != instance || managed->Generation() != impl.generation) continue;
                entry.m_pendingRemoval = true; managed->enabled = false; runtime.CancelOwned(*managed); reply->integer = 1;
                return finish(ManagedBridgeStatus::OK);
            }
        reply->integer = 0;
        return finish(ManagedBridgeStatus::OK);
    }
    case ManagedBridgeOp::SCRIPT_ENABLED_READ:
    case ManagedBridgeOp::SCRIPT_ENABLED_WRITE: {
        if (request->flags > 1 || request->index || request->inputBytes) return finish(ManagedBridgeStatus::INVALID_ARGUMENT);
        for (const auto& [id, candidate] : impl.adapters) {
            if (candidate->InstanceId() != std::bit_cast<uint64_t>(request->integer) || candidate->Generation() != impl.generation) continue;
            if (!candidate->HasLiveContext() || candidate->m_scene != &scene) return finish(ManagedBridgeStatus::NOT_FOUND);
            auto* component = candidate->m_gameObject->GetComponent<ScriptComponent>();
            if (!component) return finish(ManagedBridgeStatus::NOT_FOUND);
            for (ScriptEntry& entry : component->scripts) {
                if (entry.script.get() != candidate) continue;
                if (request->op == ManagedBridgeOp::SCRIPT_ENABLED_WRITE) {
                    if (entry.m_pendingRemoval) return finish(ManagedBridgeStatus::NOT_FOUND);
                    candidate->enabled = request->flags != 0;
                    if (!candidate->enabled) runtime.CancelOwned(*candidate);
                }
                reply->integer = candidate->enabled && !entry.m_pendingRemoval ? 1 : 0;
                return finish(ManagedBridgeStatus::OK);
            }
        }
        return finish(ManagedBridgeStatus::NOT_FOUND);
    }
    case ManagedBridgeOp::SCENE_QUERY:
    case ManagedBridgeOp::SCENE_SPAWN_MANY: {
        const std::string_view bytes(request->input ? request->input : "", request->inputBytes);
        size_t offset = 0;
        const auto text = [&](std::string_view& value) {
            uint32_t length = 0;
            if (!managed_detail::Read(bytes, offset, length) || length > bytes.size() - offset) return false;
            value = bytes.substr(offset, length); offset += length;
            return value.find('\0') == std::string_view::npos;
        };
        std::vector<GameObject*> objects;
        if (request->op == ManagedBridgeOp::SCENE_QUERY) {
            std::string_view name, tag, part;
            if (request->flags > 1 || !text(name) || !text(tag) || !text(part) || offset != bytes.size())
                return finish(ManagedBridgeStatus::INVALID_ARGUMENT);
            const auto status = managed_detail::ManagedObjectControls::Query(scene, target, request->index, name, tag, part, request->flags != 0, objects);
            if (status != ManagedBridgeStatus::OK) return finish(status);
        } else {
            if (request->flags > 1 || request->integer) return finish(ManagedBridgeStatus::INVALID_ARGUMENT);
            const uint64_t required = 4ULL + static_cast<uint64_t>(request->index) * 76U;
            if (required > UINT32_MAX) return finish(ManagedBridgeStatus::INVALID_ARGUMENT);
            const uint32_t capacity = static_cast<uint32_t>(required);
            if (request->outputCapacity < capacity || !request->output) { reply->outputBytes = capacity; return finish(ManagedBridgeStatus::BUFFER_TOO_SMALL); }
            std::string_view reference;
            if (!text(reference) || bytes.size() - offset != static_cast<size_t>(request->index) * 28U) return finish(ManagedBridgeStatus::INVALID_ARGUMENT);
            std::vector<managed_detail::ManagedObjectLifecycle::SpawnPose> poses(request->index);
            for (auto& pose : poses)
                for (float* value : {&pose.position.x, &pose.position.y, &pose.position.z, &pose.rotation.x, &pose.rotation.y, &pose.rotation.z, &pose.rotation.w})
                    if (!managed_detail::Read(bytes, offset, *value)) return finish(ManagedBridgeStatus::INVALID_ARGUMENT);
            const auto status = managed_detail::ManagedObjectLifecycle::SpawnMany(scene, reference, request->flags != 0, poses, objects);
            if (status != ManagedBridgeStatus::OK) return finish(status);
        }
        std::string encoded;
        managed_detail::Append(encoded, static_cast<uint32_t>(objects.size()));
        for (GameObject* object : objects) {
            if (object->instanceId.size() > 64) return finish(ManagedBridgeStatus::INVALID_ARGUMENT);
            managed_detail::Append(encoded, impl.RegisterObject(scene, *object));
            managed_detail::Append(encoded, static_cast<uint32_t>(object->instanceId.size()));
            encoded.append(object->instanceId);
        }
        reply->index = static_cast<uint32_t>(objects.size()); reply->callId = scene.GetRenderSceneGeneration();
        return output(encoded);
    }
    case ManagedBridgeOp::SCENE_CREATE:
    case ManagedBridgeOp::SCENE_INSTANTIATE: {
        if (request->flags > 1 || request->index || request->integer) return finish(ManagedBridgeStatus::INVALID_ARGUMENT);
        /// @note 変更要求は再送しない。GUID の最大長より大きい出力を生成前に確保させる。
        if (!request->output || request->outputCapacity < 64) { reply->outputBytes = 64; return finish(ManagedBridgeStatus::BUFFER_TOO_SMALL); }
        std::string_view input(request->input ? request->input : "", request->inputBytes);
        math::Vector3 position{request->x, request->y, request->z};
        math::Quaternion rotation = math::Quaternion::Identity();
        if (request->flags) {
            size_t offset = 0;
            if (!managed_detail::Read(input, offset, rotation.x) || !managed_detail::Read(input, offset, rotation.y)
                || !managed_detail::Read(input, offset, rotation.z) || !managed_detail::Read(input, offset, rotation.w))
                return finish(ManagedBridgeStatus::INVALID_ARGUMENT);
            input.remove_prefix(offset);
        }
        GameObject* created = nullptr;
        const auto status = request->op == ManagedBridgeOp::SCENE_CREATE
            ? managed_detail::ManagedObjectLifecycle::Create(scene, input, created, request->flags ? &position : nullptr, request->flags ? &rotation : nullptr)
            : managed_detail::ManagedObjectLifecycle::Instantiate(scene, input, created, request->flags ? &position : nullptr, request->flags ? &rotation : nullptr);
        return status == ManagedBridgeStatus::OK && created ? objectReply(*created) : finish(status);
    }
    case ManagedBridgeOp::SCENE_CAN_CREATE:
        if (request->flags || request->inputBytes || request->integer) return finish(ManagedBridgeStatus::INVALID_ARGUMENT);
        reply->valueKind = ManagedValueKind::BOOL;
        reply->integer = scene.CanCreateGameObjects(request->index) ? 1 : 0;
        break;
    case ManagedBridgeOp::OBJECT_DESTROY:
        if (request->flags || request->index || request->inputBytes) return finish(ManagedBridgeStatus::INVALID_ARGUMENT);
        if (!target) return finish(ManagedBridgeStatus::NOT_FOUND);
        return finish(managed_detail::ManagedObjectLifecycle::Destroy(scene, *target, request->x));
    case ManagedBridgeOp::PHYSICS_BODY_READ:
    case ManagedBridgeOp::PHYSICS_BODY_WRITE:
    case ManagedBridgeOp::PHYSICS_BODY_COMMAND:
        if (!target) return finish(ManagedBridgeStatus::NOT_FOUND);
        return finish(impl.physicsBridge.RunBody(*target, *request, *reply, scope.callback == ManagedCallback::FIXED_UPDATE));
    case ManagedBridgeOp::PHYSICS_WORLD_READ:
        if (!Script::GetPhysicsWorld()) return finish(ManagedBridgeStatus::NOT_FOUND);
        return finish(impl.physicsBridge.RunWorld(*Script::GetPhysicsWorld(), *request, *reply));
    case ManagedBridgeOp::PHYSICS_RAYCAST:
    case ManagedBridgeOp::PHYSICS_SPHERE_CAST:
    case ManagedBridgeOp::PHYSICS_RAYCAST_ALL:
    case ManagedBridgeOp::PHYSICS_OVERLAP_SPHERE:
    case ManagedBridgeOp::PHYSICS_RAYCAST_FILTERED:
    case ManagedBridgeOp::PHYSICS_SPHERE_CAST_FILTERED:
    case ManagedBridgeOp::PHYSICS_RAYCAST_ALL_FILTERED:
    case ManagedBridgeOp::PHYSICS_OVERLAP_SPHERE_FILTERED: {
        if (!Script::IsInPlayMode()) return finish(ManagedBridgeStatus::INVALID_SCOPE);
        if (request->flags >> 8) return finish(impl.batchTransfers.ReadSnapshot(scene, *request, *reply));
        if (!Script::GetPhysicsWorld()) return finish(ManagedBridgeStatus::NOT_FOUND);
        GameObject* ignored = nullptr;
        if (request->op >= ManagedBridgeOp::PHYSICS_RAYCAST_FILTERED && request->op <= ManagedBridgeOp::PHYSICS_OVERLAP_SPHERE_FILTERED) {
            const uint32_t base = request->op == ManagedBridgeOp::PHYSICS_SPHERE_CAST_FILTERED ? 16U
                : request->op == ManagedBridgeOp::PHYSICS_OVERLAP_SPHERE_FILTERED ? 0U : 12U;
            if (request->inputBytes != base + 16U) return finish(ManagedBridgeStatus::INVALID_ARGUMENT);
            size_t offset = base + 8U; uint64_t token = 0;
            if (!managed_detail::Read(std::string_view(request->input, request->inputBytes), offset, token)) return finish(ManagedBridgeStatus::INVALID_ARGUMENT);
            const auto found = impl.objects.Find(token);
            if (found && found->scene != &scene) return finish(ManagedBridgeStatus::SCENE_MISMATCH);
            ignored = token ? impl.ResolveObject(scene, token) : nullptr;
            if (token && !ignored) return finish(ManagedBridgeStatus::NOT_FOUND);
        }
        std::vector<managed_detail::ManagedPhysicsHit> hits;
        const auto status = impl.physicsBridge.Query(scene, *Script::GetPhysicsWorld(), *request, hits, ignored);
        if (status != ManagedBridgeStatus::OK) return finish(status);
        if (hits.size() > UINT32_MAX) return finish(ManagedBridgeStatus::INVALID_ARGUMENT);
        std::string bytes;
        managed_detail::Append(bytes, static_cast<uint32_t>(hits.size()));
        for (const auto& hit : hits) {
            if (!hit.object) return finish(ManagedBridgeStatus::INVALID_ARGUMENT);
            managed_detail::Append(bytes, impl.RegisterObject(scene, *hit.object));
            if (request->op != ManagedBridgeOp::PHYSICS_OVERLAP_SPHERE && request->op != ManagedBridgeOp::PHYSICS_OVERLAP_SPHERE_FILTERED) {
                for (float value : {hit.point.x, hit.point.y, hit.point.z, hit.normal.x, hit.normal.y, hit.normal.z, hit.distance})
                    managed_detail::Append(bytes, value);
            }
            managed_detail::Append(bytes, static_cast<uint32_t>(hit.object->instanceId.size()));
            bytes.append(hit.object->instanceId);
        }
        reply->index = static_cast<uint32_t>(hits.size());
        reply->callId = scene.GetRenderSceneGeneration();
        if (bytes.size() > request->outputCapacity || (!bytes.empty() && !request->output)) {
            const auto captured = impl.batchTransfers.OpenRead(scene, *request, std::move(bytes), static_cast<uint32_t>(hits.size()), *reply);
            return finish(captured == ManagedBridgeStatus::OK ? ManagedBridgeStatus::BUFFER_TOO_SMALL : captured);
        }
        return output(bytes);
    }
    case ManagedBridgeOp::RESOLVE_GUID: return resolveGuid(adapter);
    case ManagedBridgeOp::OBJECT_IS_ALIVE:
        reply->valueKind = ManagedValueKind::BOOL;
        reply->integer = target ? 1 : 0;
        break;
    case ManagedBridgeOp::OBJECT_SELF: return objectReply(*adapter.m_gameObject);
    case ManagedBridgeOp::OBJECT_NAME:
        if (!target) return finish(ManagedBridgeStatus::NOT_FOUND);
        return output(target->name);
    case ManagedBridgeOp::SCENE_FIND_NAME: {
        const std::string_view name(request->input ? request->input : "", request->inputBytes);
        if (name.empty() || request->flags > 1) return finish(ManagedBridgeStatus::INVALID_ARGUMENT);
        GameObject* match = nullptr;
        for (GameObject& object : scene.GameObjects()) {
            if (object.name != name || (!(request->flags & 1U) && !object.activeInHierarchy())) continue;
            if (match) return finish(ManagedBridgeStatus::INVALID_ARGUMENT);
            match = &object;
        }
        return match ? objectReply(*match) : finish(ManagedBridgeStatus::NOT_FOUND);
    }
    case ManagedBridgeOp::FIND_CHILD_PATH: {
        if (!target) return finish(ManagedBridgeStatus::NOT_FOUND);
        if (request->flags > 1) return finish(ManagedBridgeStatus::INVALID_ARGUMENT);
        const std::string_view bytes(request->input ? request->input : "", request->inputBytes);
        size_t offset = 0;
        uint32_t count = 0;
        if (!managed_detail::Read(bytes, offset, count) || !count
            || count > (bytes.size() - offset) / 5) return finish(ManagedBridgeStatus::INVALID_ARGUMENT);
        for (uint32_t index = 0; index < count; ++index) {
            uint32_t length = 0;
            if (!managed_detail::Read(bytes, offset, length) || !length || length > bytes.size() - offset)
                return finish(ManagedBridgeStatus::INVALID_ARGUMENT);
            const std::string_view name = bytes.substr(offset, length);
            offset += length;
            GameObject* match = nullptr;
            for (int childIndex = 0; childIndex < target->GetChildCount(); ++childIndex) {
                GameObject* child = target->GetChild(childIndex);
                if (!child || child->name != name || (!(request->flags & 1U) && !child->activeInHierarchy())) continue;
                if (match) return finish(ManagedBridgeStatus::INVALID_ARGUMENT);
                match = child;
            }
            if (!match) return finish(ManagedBridgeStatus::NOT_FOUND);
            target = match;
        }
        if (offset != bytes.size()) return finish(ManagedBridgeStatus::INVALID_ARGUMENT);
        return objectReply(*target);
    }
    case ManagedBridgeOp::MANAGED_COMPONENTS: {
        if (!target) return finish(ManagedBridgeStatus::NOT_FOUND);
        if (request->flags == 1) {
            if (request->inputBytes || !request->output || request->outputCapacity < 8 || request->outputCapacity > 1048576)
                return finish(ManagedBridgeStatus::INVALID_ARGUMENT);
            auto* component = target->GetComponent<ScriptComponent>();
            const size_t size = component ? component->scripts.size() : 0;
            if (request->index > size) return finish(ManagedBridgeStatus::INVALID_ARGUMENT);
            size_t cursor = request->index;
            uint32_t count = 0;
            const uint32_t capacity = request->outputCapacity / 8;
            while (cursor < size && count < capacity) {
                auto& entry = component->scripts[cursor++];
                ManagedScriptAdapter* item = runtime.FindAdapter(entry.script.get());
                if (!item || item->Generation() != impl.generation || entry.m_pendingRemoval) continue;
                if (item->m_scene != &scene || item->m_gameObject != target) item->SetContext(&scene, target);
                if (!item->HasLiveContext() && !runtime.BindOwner(*item)) continue;
                if (!item->HasLiveContext()) continue;
                const uint64_t instance = item->InstanceId();
                std::memcpy(request->output + count * 8, &instance, 8); ++count;
            }
            reply->index = count; reply->integer = static_cast<int64_t>(cursor);
            reply->flags = cursor == size ? 1U : 0U; reply->outputBytes = count * 8;
            return finish(ManagedBridgeStatus::OK);
        }
        if (request->flags || request->index || request->inputBytes) return finish(ManagedBridgeStatus::INVALID_ARGUMENT);
        std::vector<uint64_t> instances;
        if (auto* component = target->GetComponent<ScriptComponent>()) {
            for (auto& entry : component->scripts) {
                ManagedScriptAdapter* item = runtime.FindAdapter(entry.script.get());
                if (!item || item->Generation() != impl.generation || entry.m_pendingRemoval) continue;
                /// @note inactive/未 Awake の個体も取得できる。所属の接続だけを確定し、callback と保存参照の再解決は行わない。
                if (item->m_scene != &scene || item->m_gameObject != target) item->SetContext(&scene, target);
                if (!item->HasLiveContext() && !runtime.BindOwner(*item)) continue;
                if (item->HasLiveContext()) instances.push_back(item->InstanceId());
            }
        }
        std::string bytes;
        managed_detail::Append(bytes, static_cast<uint32_t>(instances.size()));
        for (uint64_t instance : instances) managed_detail::Append(bytes, instance);
        reply->index = static_cast<uint32_t>(instances.size());
        return output(bytes);
    }
    case ManagedBridgeOp::TRANSFORM_READ: {
        if (!target) return finish(ManagedBridgeStatus::NOT_FOUND);
        if (request->index > static_cast<uint32_t>(ManagedTransformValue::WORLD_POSITION))
            return finish(ManagedBridgeStatus::INVALID_ARGUMENT);
        const auto position = request->index == static_cast<uint32_t>(ManagedTransformValue::LOCAL_POSITION)
            ? target->transform.position : target->transform.worldPosition;
        reply->valueKind = ManagedValueKind::VECTOR3;
        reply->x = position.x;
        reply->y = position.y;
        reply->z = position.z;
        break;
    }
    case ManagedBridgeOp::TRANSFORM_READ_ROTATION:
    case ManagedBridgeOp::TRANSFORM_WRITE_LOCAL_ROTATION:
    case ManagedBridgeOp::TRANSFORM_READ_SCALE:
    case ManagedBridgeOp::TRANSFORM_WRITE_LOCAL_SCALE:
    case ManagedBridgeOp::TRANSFORM_READ_DIRECTION:
    case ManagedBridgeOp::TRANSFORM_ROTATE:
    case ManagedBridgeOp::TRANSFORM_LOOK_AT:
        if (!target) return finish(ManagedBridgeStatus::NOT_FOUND);
        return finish(impl.transformBatch.RunPose(*target, *request, *reply));
    case ManagedBridgeOp::TRANSFORM_WRITE_LOCAL:
    case ManagedBridgeOp::TRANSFORM_WRITE_WORLD:
    case ManagedBridgeOp::TRANSFORM_TRANSLATE_LOCAL: {
        if (!target) return finish(ManagedBridgeStatus::NOT_FOUND);
        return finish(impl.transformBatch.RunPosition(*target, *request));
    }
    case ManagedBridgeOp::TIME:
        if (scope.callback != ManagedCallback::UPDATE && scope.callback != ManagedCallback::FIXED_UPDATE
            && scope.callback != ManagedCallback::LATE_UPDATE
            && !(scope.callback >= ManagedCallback::COLLISION_ENTER && scope.callback <= ManagedCallback::TRIGGER_EXIT))
            return finish(ManagedBridgeStatus::INVALID_SCOPE);
        reply->valueKind = ManagedValueKind::FLOAT;
        reply->x = scope.callback == ManagedCallback::FIXED_UPDATE
            || (scope.callback >= ManagedCallback::COLLISION_ENTER && scope.callback <= ManagedCallback::TRIGGER_EXIT)
            ? adapter.time.FixedDeltaTime() : adapter.time.DeltaTime();
        break;
    case ManagedBridgeOp::INPUT_KEY: {
        if (request->index > static_cast<uint32_t>(ManagedInputValue::UP) || request->integer < 0 || request->integer > 255)
            return finish(ManagedBridgeStatus::INVALID_ARGUMENT);
        const auto key = static_cast<input::KeyCode>(request->integer);
        reply->valueKind = ManagedValueKind::BOOL;
        reply->integer = request->index == static_cast<uint32_t>(ManagedInputValue::HELD) ? adapter.input.GetKey(key)
            : request->index == static_cast<uint32_t>(ManagedInputValue::DOWN) ? adapter.input.GetKeyDown(key) : adapter.input.GetKeyUp(key);
        break;
    }
    case ManagedBridgeOp::INPUT_ACTION: {
        if (request->flags) return finish(ManagedBridgeStatus::INVALID_ARGUMENT);
        const std::string_view action(request->input ? request->input : "", request->inputBytes);
        const auto status = managed_detail::ManagedInputBridge::Read(action, request->index, *reply);
        if (status == ManagedBridgeStatus::NOT_FOUND)
            impl.diagnostic = "Unknown input " + std::string(request->index == static_cast<uint32_t>(ManagedInputValue::AXIS) ? "axis" : "button")
                + " '" + std::string(action) + "'.";
        return finish(status);
    }
    case ManagedBridgeOp::INPUT_QUERY: {
        if (request->flags >> 8) return finish(impl.batchTransfers.ReadSnapshot(scene, *request, *reply));
        if (request->flags > 1 || (request->flags == 1 && (request->index < static_cast<uint32_t>(ManagedInputQuery::BUTTON_NAMES)
            || request->index > static_cast<uint32_t>(ManagedInputQuery::AXIS_NAMES) || request->inputBytes))) return finish(ManagedBridgeStatus::INVALID_ARGUMENT);
        const std::string_view name(request->input ? request->input : "", request->inputBytes);
        std::string names;
        const auto status = managed_detail::ManagedInputBridge::Query(name, request->index, *reply, names);
        if (status != ManagedBridgeStatus::OK) return finish(status);
        if (request->flags == 1) {
            reply->callId = scene.GetRenderSceneGeneration();
            if (names.size() <= request->outputCapacity) return output(names);
            const auto opened = impl.batchTransfers.OpenRead(scene, *request, std::move(names), reply->index, *reply);
            return finish(opened == ManagedBridgeStatus::OK ? ManagedBridgeStatus::BUFFER_TOO_SMALL : opened);
        }
        return names.empty() ? finish(status) : output(names);
    }
    default: return finish(ManagedBridgeStatus::UNSUPPORTED);
    }
    return finish(ManagedBridgeStatus::OK);
}

} /// @note namespace fbzz::scene
