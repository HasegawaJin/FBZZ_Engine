/// @file    ManagedScriptRuntimeState.hpp
/// @brief   世代・個体・呼び出しスコープを共有するマネージドランタイムの非公開状態。
/// @author  Hasegawa Jin
/// @date    2026-11-21
#pragma once

#include <Engine/Scene/Managed/Runtime/ManagedScriptRuntime.hpp>
#include "../Services/AI/ManagedAiCrowdTransfer.hpp"
#include "../Services/AI/ManagedAiPoseTransfer.hpp"
#include "../Objects/ManagedBatchTransfers.hpp"
#include "../Objects/ManagedObjectRegistry.hpp"
#include "../Services/Physics/ManagedPhysicsBridge.hpp"
#include "../Services/ManagedServicesBridge.hpp"
#include "../Objects/ManagedTransformBatch.hpp"
#include <thread>
#include <unordered_map>
#include <unordered_set>

struct HINSTANCE__;

namespace fbzz::scene {

namespace managed_detail {

inline constexpr uint32_t TYPE_GIZMO_METADATA_FLAG = 2U;
inline constexpr uint32_t TYPE_LIFECYCLE_METADATA_FLAG = 4U;
inline constexpr uint32_t UPDATE_CALLBACK = 8U;
inline constexpr uint32_t FIXED_UPDATE_CALLBACK = 16U;
inline constexpr uint32_t LATE_UPDATE_CALLBACK = 32U;
inline constexpr uint32_t LIFECYCLE_CALLBACK_MASK = UPDATE_CALLBACK | FIXED_UPDATE_CALLBACK | LATE_UPDATE_CALLBACK;
inline constexpr uint32_t GIZMO_REGULAR_CALLBACK = 1U;
inline constexpr uint32_t GIZMO_SELECTED_CALLBACK = 2U;
inline constexpr uint32_t GIZMO_CALLBACK_MASK = GIZMO_REGULAR_CALLBACK | GIZMO_SELECTED_CALLBACK;

inline ManagedBridgeRequest InstanceRequest(ManagedBridgeOp op, uint64_t generation, uint64_t instanceId, uint64_t ownerId)
{
    ManagedBridgeRequest request;
    request.op = op;
    request.generation = generation;
    request.instanceId = instanceId;
    request.ownerId = ownerId;
    return request;
}

}

struct ManagedScriptRuntime::Impl {
    managed_detail::ManagedPhysicsBridge physicsBridge;
    managed_detail::ManagedServicesBridge servicesBridge;
    ManagedBridgeDispatch dispatch = nullptr;
    HINSTANCE__* hostModule = nullptr;
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
    /// @note 一時転送と読み取りを終了してから、同期再入前のスコープへ戻す。
    void EndCallback(uint64_t callId, const Scope& previous)
    {
        servicesBridge.EndCall(callId);
        batchTransfers.EndCall(callId);
        std::erase_if(assetReads, [callId](const auto& item) { return item.second.caller.callId == callId; });
        scope = previous;
    }
};

}
