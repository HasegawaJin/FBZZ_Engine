/// @file    ManagedScriptCallbacks.cpp
/// @brief   ネイティブ通知とライフサイクルをマネージド呼び出しへ変換する。
/// @author  Hasegawa Jin
/// @date    2026-11-21
#include "ManagedScriptRuntimeState.hpp"
#include "../Interop/ManagedFieldCodec.hpp"
#include "ManagedScriptJobs.hpp"

#include <Engine/Core/Diagnostics/Logger.hpp>
#include <Engine/Scene/Managed/Runtime/ManagedScriptAdapter.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Scripting/ScriptComponent.hpp>
#include <Engine/Scene/Scripting/ScriptValidation.hpp>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <utility>

namespace fbzz::scene {

namespace {

using managed_detail::FIXED_UPDATE_CALLBACK;
using managed_detail::GIZMO_REGULAR_CALLBACK;
using managed_detail::GIZMO_SELECTED_CALLBACK;
using managed_detail::InstanceRequest;
using managed_detail::LATE_UPDATE_CALLBACK;
using managed_detail::TYPE_LIFECYCLE_METADATA_FLAG;
using managed_detail::UPDATE_CALLBACK;

constexpr size_t MAXIMUM_NOTIFICATION_BYTES = 1024 * 1024;

/// @note Animation と Sequence の先頭 16 バイトは共通。Sequence のみ直後にシーケンス名の長さを続ける。
/// @see Docs/design/csharp-engine-notifications.md
void AppendTimedNotificationHeader(std::string& payload, int32_t integer, float value, float time, uint32_t nameBytes)
{
    managed_detail::Append(payload, integer);
    managed_detail::Append(payload, value);
    managed_detail::Append(payload, time);
    managed_detail::Append(payload, nameBytes);
}

/// @note 通知の配布先を取得した後に除去・無効化された受信者には、マネージドコードを呼ばない。
bool AttachedNotificationReceiver(const ManagedScriptAdapter& adapter)
{
    if (!adapter.HasLiveContext() || !adapter.scene.IsActiveAndEnabled()) {
        return false;
    }
    const auto* scripts = adapter.scene.Self()->GetComponent<ScriptComponent>();
    if (!scripts) {
        return false;
    }
    return std::any_of(scripts->scripts.begin(), scripts->scripts.end(), [&adapter](const ScriptEntry& entry) {
        return entry.script.get() == &adapter && !entry.m_pendingRemoval;
    });
}

}

bool ManagedScriptRuntime::Invoke(ManagedScriptAdapter& adapter, ManagedCallback callback)
{
    return InvokePayload(adapter, callback, {});
}

bool ManagedScriptRuntime::Resume(ManagedScriptAdapter& adapter, bool enabled)
{
    if (std::this_thread::get_id() != m_impl->mainThread) {
        return false;
    }
    m_impl->diagnostic.clear();
    const auto fail = [&] {
        adapter.m_resumeDiagnostic = m_impl->diagnostic;
        return false;
    };
    if (!IsDispatchIdle()
        || adapter.Generation() != m_impl->generation || !BindOwner(adapter) || !adapter.HasLiveContext()) {
        if (m_impl->diagnostic.empty()) {
            m_impl->diagnostic = "Resume requires a current Script at an idle dispatch point.";
        }
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
            if (!m_impl->diagnostic.empty()) {
                m_impl->diagnostic += "\n";
            }
            m_impl->diagnostic += FormatScriptRequirementIssue(issue);
        }
    if (!m_impl->diagnostic.empty()) {
        return fail();
    }
    if (!adapter.ValidateReferences()) {
        return fail();
    }
    CancelOwned(adapter);
    auto request = InstanceRequest(ManagedBridgeOp::RESUME_INSTANCE, adapter.Generation(), adapter.InstanceId(), adapter.OwnerId());
    ManagedBridgeReply reply;
    if (!DispatchPrepared(adapter, request, reply)) {
        return fail();
    }
    // 再開操作で作者 callback を呼ばず、成功済みの開始段階だけを次 Tick に引き継ぐ。
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
    // 現在の Play 世代でコールバックを配送できるインスタンスか確認する。
    if (!Script::IsInPlayMode() || adapter.Generation() != m_impl->generation || !adapter.HasLiveContext()
        || !adapter.scene.IsActiveAndEnabled()) {
        return false;
    }

    // 所有者付き配送はイベントとジョブに限り、ネイティブのシーケンスはイベントとして受け取る。
    if (!token || (callback != ManagedCallback::EVENT && callback != ManagedCallback::JOB)) {
        return false;
    }
    if (nativeSequence && callback != ManagedCallback::EVENT) {
        return false;
    }

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
    m_impl->EndCallback(call, previous);
    if (!success && reply.status != ManagedBridgeStatus::NOT_FOUND) {
        adapter.RecordFault();
    }
    return success;
}

void ManagedScriptRuntime::CancelOwned(ManagedScriptAdapter& adapter)
{
    m_impl->servicesBridge.CancelCursorOwner(adapter.OwnerId());
    m_impl->servicesBridge.CancelOwner(adapter.OwnerId(), adapter.HasLiveContext() ? &adapter : nullptr);
    adapter.m_connectionsActive = false;
    managed_detail::CancelManagedOwner(adapter);
    if (!adapter.InstanceId()) {
        return;
    }
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
            if (&object != info.other) {
                continue;
            }
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
    for (float value : values) {
        managed_detail::Append(payload, value);
    }
    managed_detail::Append(payload, static_cast<uint32_t>(guid.size()));
    payload.append(guid);
    return InvokePayload(adapter, callback, payload);
}

bool ManagedScriptRuntime::InvokeAnimationEvent(ManagedScriptAdapter& adapter, const AnimationEventInfo& info)
{
    const std::string_view name = info.name ? info.name : "";
    if (name.size() > MAXIMUM_NOTIFICATION_BYTES - 16 || !std::isfinite(info.floatParam) || !std::isfinite(info.clipTime)) {
        m_impl->diagnostic = "Animation notification contains invalid values or exceeds its byte budget.";
        return false;
    }
    std::string payload;
    AppendTimedNotificationHeader(payload, info.intParam, info.floatParam, info.clipTime, static_cast<uint32_t>(name.size()));
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
    for (float value : values) {
        managed_detail::Append(payload, value);
    }
    managed_detail::Append(payload, static_cast<uint32_t>(info.appliedByEngine));
    return InvokePayload(adapter, ManagedCallback::ANIMATOR_MOVE, payload);
}

bool ManagedScriptRuntime::InvokeSequenceEvent(ManagedScriptAdapter& adapter, const SequenceEventInfo& info, uint64_t subscription)
{
    if (subscription && !AttachedNotificationReceiver(adapter)) {
        return true;
    }
    const std::string_view name = info.name ? info.name : "";
    const std::string_view sequence = info.sequenceName ? info.sequenceName : "";
    if (name.size() > MAXIMUM_NOTIFICATION_BYTES - 20 || sequence.size() > MAXIMUM_NOTIFICATION_BYTES - 20 - name.size()
        || !std::isfinite(info.floatParam) || !std::isfinite(info.sequenceTime)) {
        m_impl->diagnostic = "Sequence notification contains invalid values or exceeds its byte budget.";
        if (subscription) {
            adapter.RecordFault();
        }
        return false;
    }
    std::string payload;
    AppendTimedNotificationHeader(payload, info.intParam, info.floatParam, info.sequenceTime, static_cast<uint32_t>(name.size()));
    managed_detail::Append(payload, static_cast<uint32_t>(sequence.size()));
    payload.append(name);
    payload.append(sequence);
    return subscription ? InvokeOwned(adapter, ManagedCallback::EVENT, subscription, payload, 0, false, true)
        : InvokePayload(adapter, ManagedCallback::SEQUENCE_EVENT, payload);
}

bool ManagedScriptRuntime::InvokeSequenceFinished(ManagedScriptAdapter& adapter, std::string_view sequenceName)
{
    if (sequenceName.size() > MAXIMUM_NOTIFICATION_BYTES || sequenceName.find('\0') != std::string_view::npos) {
        m_impl->diagnostic = "Sequence completion contains an invalid name or exceeds its byte budget.";
        return false;
    }
    return InvokePayload(adapter, ManagedCallback::SEQUENCE_FINISHED, sequenceName);
}

bool ManagedScriptRuntime::InvokePayload(ManagedScriptAdapter& adapter, ManagedCallback callback, std::string_view payload)
{
    // 個体の失敗へ別個体・以前の要求の診断を帰属させない。
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
            // 不在の更新だけを省き、owner・世代・直接 GUID 変更・失敗・再入は従来の Host 検証へ戻す。
            /// @see https://docs.unity3d.com/6000.0/Documentation/Manual/events-per-frame-optimization.html 不要な native/managed 更新往復の削減。
            if (bound.generation == adapter.Generation() && bound.ownerId == adapter.OwnerId()
                && bound.sceneLifetime == adapter.m_contextSceneGeneration && bound.objectId == adapter.m_objectId
                && bound.entity == adapter.m_contextEntity && bound.guid == object->instanceId
                && bound.scriptBirth == adapter.m_referenceBirth) {
                return true;
            }
        }
    }
    if (!BindOwner(adapter)) {
        return false;
    }
    if (callback >= ManagedCallback::ANIMATION_EVENT && callback <= ManagedCallback::NAV_TARGET_LOST
        && (!Script::IsInPlayMode() || !AttachedNotificationReceiver(adapter))) {
        return true;
    }
    if (callback == ManagedCallback::DISABLE || callback == ManagedCallback::DESTROY) {
        CancelOwned(adapter);
    }
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
    m_impl->EndCallback(call, previous);
    return succeeded;
}

bool ManagedScriptRuntime::CaptureGizmos(ManagedScriptAdapter& adapter, bool selected,
                                       std::vector<ScriptDebugDrawCommand>& commands, uint32_t capacity)
{
    commands.clear();
    if (!capacity || capacity > (std::numeric_limits<uint32_t>::max() - 4U) / 52U
        || adapter.Generation() != m_impl->generation || !BindOwner(adapter) || !adapter.HasLiveContext()) {
        return false;
    }
    // 不在が確定した callback だけを省き、capacity・世代・owner 接続の拒否は従来と同じ順で行う。
    // callback の入れ子では Host が idle 契約で拒否するため、空描画も従来の query を通す。
    const auto callbacks = m_impl->gizmoCallbacks.find(adapter.m_typeName);
    if (!m_impl->scope.callId && callbacks != m_impl->gizmoCallbacks.end()
        && (callbacks->second & (selected ? GIZMO_SELECTED_CALLBACK : GIZMO_REGULAR_CALLBACK)) == 0) {
        return true;
    }
    const ManagedCallback callback = selected ? ManagedCallback::DRAW_GIZMOS_SELECTED : ManagedCallback::DRAW_GIZMOS;
    const auto previous = m_impl->scope;
    const uint64_t call = m_impl->nextCall++;
    m_impl->scope = {adapter.Generation(), adapter.InstanceId(), adapter.OwnerId(), call, callback};
    auto request = InstanceRequest(ManagedBridgeOp::DRAW_GIZMOS_QUERY, adapter.Generation(), adapter.InstanceId(), adapter.OwnerId());
    request.callId = call; request.index = static_cast<uint32_t>(callback); request.flags = capacity;
    // 最大 command byte 数を先に確保し、作者 draw callback を容量不足で再実行しない。
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
    if (!managed_detail::Read(bytes, offset, count) || count > capacity || bytes.size() != 4U + static_cast<size_t>(count) * 52U) {
        return false;
    }
    std::vector<ScriptDebugDrawCommand> prepared;
    prepared.reserve(count);
    for (uint32_t index = 0; index < count; ++index) {
        constexpr uint32_t LINE_KIND = 0;
        constexpr uint32_t SPHERE_KIND = 1;
        constexpr uint32_t BOX_KIND = 2;
        ScriptDebugDrawCommand command;
        uint32_t kind = 0;
        uint32_t depth = 0;

        // コマンドの種類、形状、描画属性をパケット順に読む。読み取りに失敗した時点で中断する。
        if (!managed_detail::Read(bytes, offset, kind) || kind > BOX_KIND) {
            return false;
        }
        if (!managed_detail::Read(bytes, offset, command.a.x)
            || !managed_detail::Read(bytes, offset, command.a.y) || !managed_detail::Read(bytes, offset, command.a.z)) {
            return false;
        }
        if (!managed_detail::Read(bytes, offset, command.b.x)
            || !managed_detail::Read(bytes, offset, command.b.y) || !managed_detail::Read(bytes, offset, command.b.z)
            || !managed_detail::Read(bytes, offset, command.radius)) {
            return false;
        }
        if (!managed_detail::Read(bytes, offset, command.color.x) || !managed_detail::Read(bytes, offset, command.color.y)
            || !managed_detail::Read(bytes, offset, command.color.z) || !managed_detail::Read(bytes, offset, command.color.w)) {
            return false;
        }
        if (!managed_detail::Read(bytes, offset, depth) || depth > 1) {
            return false;
        }

        // 形状の座標と半径を検証してから、描画色の有効性を確かめる。
        const auto finite = [](const math::Vector3& value) { return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z); };
        if (!finite(command.a) || !finite(command.b) || !std::isfinite(command.radius) || command.radius < 0) {
            return false;
        }
        if (!std::isfinite(command.color.x) || !std::isfinite(command.color.y)
            || !std::isfinite(command.color.z) || !std::isfinite(command.color.w)) {
            return false;
        }
        command.type = ScriptDebugDrawType::Box;
        if (kind == LINE_KIND) {
            command.type = ScriptDebugDrawType::Line;
        } else if (kind == SPHERE_KIND) {
            command.type = ScriptDebugDrawType::Sphere;
        }
        command.halfExtents = command.b;
        command.depthTest = depth != 0;
        if (kind == BOX_KIND && (command.b.x < 0 || command.b.y < 0 || command.b.z < 0)) {
            return false;
        }
        prepared.push_back(command);
    }

    // すべてのコマンドを検証できた場合だけ描画用の一覧を置き換える。
    commands = std::move(prepared);
    return true;
}

}
