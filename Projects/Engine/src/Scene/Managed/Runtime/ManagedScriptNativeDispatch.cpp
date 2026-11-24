/// @file    ManagedScriptNativeDispatch.cpp
/// @brief   マネージドからの要求の権限と所属を検証し、ネイティブ側の各サービスへ配送する。
/// @author  Hasegawa Jin
/// @date    2026-11-21
#include "ManagedScriptRuntimeState.hpp"
#include "../Services/Persistence/ManagedDataAssets.hpp"
#include "../Interop/ManagedFieldCodec.hpp"
#include "../Services/Gameplay/ManagedInputBridge.hpp"
#include "../Objects/ManagedObjectControls.hpp"
#include "../Objects/ManagedObjectLifecycle.hpp"
#include "ManagedScriptJobs.hpp"

#include <Engine/Core/Platform/Application.hpp>
#include <Engine/Core/Diagnostics/Logger.hpp>
#include <Engine/Input/KeyCode.hpp>
#include <Engine/Scene/Managed/Runtime/ManagedScriptAdapter.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Scripting/ScriptComponent.hpp>
#include <Engine/Scene/Scripting/ScriptEvent.hpp>
#include <Engine/Scene/Scripting/ScriptFactory.hpp>
#include <Engine/Scene/Systems/Transform/TransformSystem.hpp>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace fbzz::scene {

namespace {

/// @note Connect はワールドの読み取りと自分が所有する処理の登録を許す。ワールドの変更と同期的な副作用の公開は禁止する。
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
    const auto hasScriptEntry = [&](GameObject* target) {
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
            if (request->flags == 2) return hasScriptEntry(target);
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
        const bool blend = request->op == ManagedBridgeOp::SERVICE_ANIMATOR
            && request->index == static_cast<uint32_t>(ManagedAnimatorMethod::GET_CURRENT_BLEND_WEIGHTS);
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
        if (request->op == ManagedBridgeOp::COMPONENT_READ && request->flags == 2) return hasScriptEntry(target);
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
    case ManagedBridgeOp::TRANSFORM_WRITE_WORLD_ROTATION:
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

}
