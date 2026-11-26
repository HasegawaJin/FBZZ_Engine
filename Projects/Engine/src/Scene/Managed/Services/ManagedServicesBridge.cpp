/// @file    ManagedServicesBridge.cpp
/// @brief   固定値 packet の検証と既存 native ScriptProxy への型付き転送。
/// @author  Hasegawa Jin
/// @date    2026-10-04
#include "ManagedServicesBridge.hpp"
#include "../Interop/ManagedPacketRead.hpp"
#include "../Objects/ManagedObjectControls.hpp"
#include "Persistence/ManagedAuthoringServices.hpp"
#include "Persistence/ManagedStoreTransfers.hpp"
#include "Rendering/ManagedMaterialTransfers.hpp"
#include "Physics/ManagedPhysicsBridge.hpp"
#include "ManagedRuntimeServices.hpp"
#include "Gameplay/ManagedWorldFamilyServices.hpp"
#include "Rendering/ManagedRenderService.hpp"
#include "Physics/ManagedJointService.hpp"
#include "Physics/ManagedJointTransfers.hpp"
#include "Persistence/ManagedAssetServices.hpp"
#include "ManagedComponentServices.hpp"
#include "Physics/ManagedRagdollControls.hpp"
#include "Audio/ManagedServiceAudio.hpp"
#include "ManagedServiceRoutes.hpp"
#include "Physics/ManagedComponentTransfers.hpp"
#include "Rendering/ManagedRenderComponentServices.hpp"
#include "Rendering/ManagedRenderTransfers.hpp"
#include "Rendering/ManagedParticleServices.hpp"
#include <Engine/Scene/Components/UI/UICanvas.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Scripting/Script.hpp>
#include <bit>
#include <charconv>
#include <cmath>

namespace fbzz::scene::managed_detail {
namespace {

class ProxyScript final : public Script {
public:
    /// @note callback を駆動しない facade 専用文脈。Profiler 登録や作者 lifecycle は発生させない。
    void Bind(Scene& scene, GameObject& target)
    { m_scene = &scene; m_gameObject = &target; }
    void Clear() { m_scene = nullptr; m_gameObject = nullptr; }
};

/// @note スクリプトのライフサイクルを駆動せず、経路の終了時に借用した Scene と対象を解除する。
class ProxyBindingScope {
public:
    ProxyBindingScope(ProxyScript& view, Scene& scene, GameObject& target) : m_view(view)
    {
        m_view.Bind(scene, target);
    }

    ~ProxyBindingScope() { m_view.Clear(); }
    ProxyBindingScope(const ProxyBindingScope&) = delete;
    ProxyBindingScope& operator=(const ProxyBindingScope&) = delete;

private:
    ProxyScript& m_view;
};

bool Parse(const ManagedBridgeRequest& request, ManagedServiceArguments& arguments)
{
    if (!request.input || request.inputBytes < 24 || request.inputBytes > 1048576 || request.flags > 1) {
        return false;
    }
    const std::string_view bytes(request.input, request.inputBytes);
    size_t offset = 0;
    uint32_t floats = 0, strings = 0;
    const auto floatCapacity = request.op == ManagedBridgeOp::SERVICE_RAGDOLL && request.index == APPLY_RAGDOLL_DRIVE_PROFILE
        ? 1 + RAGDOLL_DRIVE_PROFILE_MAX_BODIES * RAGDOLL_DRIVE_PROFILE_ENTRY_FLOATS : 128;
    if (!ReadManagedValue(bytes, offset, arguments.relatedId) || !ReadManagedValue(bytes, offset, arguments.integer)
        || !ReadManagedValue(bytes, offset, floats) || !ReadManagedValue(bytes, offset, strings) || floats > floatCapacity || strings > 128) {
        return false;
    }
    arguments.values.resize(floats);
    for (float& value : arguments.values) if (!ReadManagedValue(bytes, offset, value) || !std::isfinite(value)) return false;
    for (uint32_t index = 0; index < strings; ++index) {
        uint32_t length = 0;
        if (!ReadManagedValue(bytes, offset, length) || length > bytes.size() - offset) {
            return false;
        }
        arguments.text.emplace_back(bytes.substr(offset, length)); offset += length;
    }
    return offset == bytes.size();
}

}

struct ManagedServicesBridge::Impl {
    ManagedComponentTransfers componentTransfers;
    ManagedRenderTransfers renderTransfers;
    ManagedAssetServices assetServices;
    ManagedJointTransfers jointTransfers;
    ManagedStoreTransfers storeTransfers;
    ManagedMaterialTransfers materialTransfers;
    ManagedRuntimeServices runtimeServices;
    ProxyScript view;
    ManagedPhysicsBridge physics;
    ManagedServiceAudio audio;
};

ManagedServicesBridge::ManagedServicesBridge() : m_impl(std::make_unique<Impl>()) { }
ManagedServicesBridge::~ManagedServicesBridge() = default;

void ManagedServicesBridge::CancelCursorOwner(uint64_t ownerId) { m_impl->runtimeServices.CancelOwner(ownerId); }

bool ManagedServicesBridge::CursorOwner(const ManagedBridgeRequest& request, uint64_t& ownerId)
{
    ManagedServiceArguments arguments;
    if (!Parse(request, arguments)) {
        return false;
    }
    if (request.index == 0) { ownerId = std::bit_cast<uint64_t>(arguments.integer); return ownerId != 0; }
    if (request.index > 3 || arguments.text.size() != 1) {
        return false;
    }
    const auto& text = arguments.text[0];
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), ownerId);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && ownerId != 0;
}

void ManagedServicesBridge::CancelOwner(uint64_t ownerId, Script* caller)
{
    m_impl->storeTransfers.CancelOwner(ownerId);
    m_impl->componentTransfers.CancelOwner(ownerId);
    m_impl->renderTransfers.CancelOwner(ownerId);
    m_impl->assetServices.CancelOwner(ownerId, caller);
    m_impl->jointTransfers.CancelOwner(ownerId);
    m_impl->materialTransfers.CancelOwner(ownerId);
    m_impl->runtimeServices.CancelOwner(ownerId);
    m_impl->audio.CancelOwner(ownerId, caller, m_impl->view);
}

void ManagedServicesBridge::RetireGeneration(uint64_t generation)
{
    m_impl->storeTransfers.RetireGeneration(generation);
    m_impl->componentTransfers.RetireGeneration(generation);
    m_impl->renderTransfers.RetireGeneration(generation);
    m_impl->assetServices.RetireGeneration(generation);
    m_impl->jointTransfers.RetireGeneration(generation);
    m_impl->materialTransfers.RetireGeneration(generation);
    m_impl->runtimeServices.RetireGeneration(generation);
    m_impl->audio.RetireGeneration(generation, m_impl->view);
}

ManagedBridgeStatus ManagedServicesBridge::Run(Scene& scene, Script& caller, GameObject& target, GameObject* related,
    const ManagedBridgeRequest& request, ManagedServiceResult& result, bool fixedUpdate, bool collisionCallback)
{
    if (!target.IsValid() || target.GetScene() != &scene || (related && (!related->IsValid() || related->GetScene() != &scene))) {
        return ManagedBridgeStatus::NOT_FOUND;
    }
    const bool store = request.op == ManagedBridgeOp::SERVICE_SAVE || request.op == ManagedBridgeOp::SERVICE_CONFIG;
    if (request.op == ManagedBridgeOp::SERVICE_ASSETS && request.index >= 12 && request.index <= 14) {
        if (related) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        return m_impl->assetServices.Run(scene, caller, target, request, 0, {}, {}, result);
    }
    if (request.op == ManagedBridgeOp::SERVICE_MATERIAL && request.index >= 33 && request.index <= 35) {
        if (related) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        const ProxyBindingScope binding(m_impl->view, scene, target);
        return m_impl->materialTransfers.Run(scene, target, m_impl->view, request, result);
    }
    if (store && request.index >= 23 && request.index <= 25) {
        if (related) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        return m_impl->storeTransfers.Run(scene, target, request, result);
    }
    ManagedServiceArguments arguments;
    if (!Parse(request, arguments)) {
        return ManagedBridgeStatus::INVALID_ARGUMENT;
    }
    const auto& p = arguments.values;
    const auto& t = arguments.text;
    const int64_t integer = arguments.integer;
    if (request.op >= ManagedBridgeOp::SERVICE_LINE && request.op <= ManagedBridgeOp::SERVICE_INSTANCES) {
        if ((arguments.relatedId != 0) != (related != nullptr)) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        const ProxyBindingScope binding(m_impl->view, scene, target);
        if (request.op == ManagedBridgeOp::SERVICE_RAGDOLL && request.index >= 40 && request.index <= 43) {
            if (related) {
                return ManagedBridgeStatus::INVALID_ARGUMENT;
            }
            return m_impl->componentTransfers.Run(scene, target, request, integer, p, t, result, fixedUpdate);
        }
        if ((request.op == ManagedBridgeOp::SERVICE_LINE && request.index >= 50 && request.index <= 53)
            || (request.op == ManagedBridgeOp::SERVICE_MESH && request.index >= 50 && request.index <= 54)) {
            if (related) {
                return ManagedBridgeStatus::INVALID_ARGUMENT;
            }
            return m_impl->renderTransfers.Run(scene, target, m_impl->view, request, integer, p, t, result);
        }
        if (request.op <= ManagedBridgeOp::SERVICE_SKINNED_MESH || request.op==ManagedBridgeOp::SERVICE_INSTANCES) {
            return RunRenderComponentServices(scene, m_impl->view, target, related, request, integer, p, t, result);
        }
        return RunComponentServices(scene, m_impl->view, target, related, request, integer, p, t, result, fixedUpdate);
    }
    if (request.op == ManagedBridgeOp::SERVICE_ASSETS || (request.op == ManagedBridgeOp::SERVICE_MATERIAL && request.index == 42)) {
        if (related || arguments.relatedId) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        return m_impl->assetServices.Run(scene, caller, target, request, integer, p, t, result);
    }
    if (request.op == ManagedBridgeOp::SERVICE_JOINT && request.index == 19) {
        if (related || arguments.relatedId) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        return m_impl->jointTransfers.Begin(scene, target, request, integer, p, t, result, fixedUpdate);
    }
    if (request.op == ManagedBridgeOp::SERVICE_JOINT) {
        if (integer) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        const ProxyBindingScope binding(m_impl->view, scene, target);
        return RunManagedJointService(scene, m_impl->view, target, related, request, p, t, result, fixedUpdate);
    }
    if (request.op == ManagedBridgeOp::SERVICE_RENDER) {
        if (related || arguments.relatedId || caller.scene.Self() != &target) {
            return ManagedBridgeStatus::INVALID_SCOPE;
        }
        const ProxyBindingScope binding(m_impl->view, scene, target);
        return RunManagedRenderService(m_impl->view, request, integer, p, t, result);
    }
    if (request.op == ManagedBridgeOp::SERVICE_DISPLAY) {
        if (related || arguments.relatedId || caller.scene.Self() != &target) {
            return ManagedBridgeStatus::INVALID_SCOPE;
        }
        const ProxyBindingScope binding(m_impl->view, scene, target);
        return RunManagedDisplayService(scene, m_impl->view, request, integer, p, t, result);
    }
    if (request.op == ManagedBridgeOp::SERVICE_MATERIAL && (request.index == 30 || request.index == 32)) {
        if (arguments.relatedId || related) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        const ProxyBindingScope binding(m_impl->view, scene, target);
        return m_impl->materialTransfers.Open(scene, target, m_impl->view, request, integer, p, t, result);
    }
    if (store && (request.index == 22 || request.index == 26)) {
        if (arguments.relatedId || related) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        return m_impl->storeTransfers.Begin(scene, target, request, integer, p, t, result);
    }
    if (request.op >= ManagedBridgeOp::SERVICE_MATERIAL && request.op <= ManagedBridgeOp::SERVICE_CONFIG) {
        if (arguments.relatedId || related) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        const ProxyBindingScope binding(m_impl->view, scene, target);
        return RunManagedAuthoringService(scene, m_impl->view, target, request, integer, p, t, result);
    }
    if (request.op >= ManagedBridgeOp::SERVICE_CURSOR && request.op <= ManagedBridgeOp::SERVICE_SCENE) {
        if (arguments.relatedId || related) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        if (request.op == ManagedBridgeOp::SERVICE_PARTICLE && request.index >= 13) {
            const ProxyBindingScope binding(m_impl->view, scene, target);
            return RunParticleServices(m_impl->view, target, request, integer, p, t, result);
        }
        return m_impl->runtimeServices.Run(scene, caller, target, request, integer, p, t, result);
    }
    if (request.op >= ManagedBridgeOp::SERVICE_TERRAIN && request.op <= ManagedBridgeOp::SERVICE_SEQUENCE) {
        if ((arguments.relatedId != 0) != (related != nullptr)) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        const ProxyBindingScope binding(m_impl->view, scene, target);
        return RunWorldFamilyServices(scene, m_impl->view, target, related, request, integer, p, t, result, fixedUpdate);
    }
    // 所有者サービスも予約除去中の row を proxy 経由で読まない。global/2D 操作は row を要求しない。
    uint32_t componentKind = 0;
    switch (request.op) {
    case ManagedBridgeOp::SERVICE_ANIMATOR: componentKind = 5; break;
    case ManagedBridgeOp::SERVICE_CAMERA: componentKind = 8; break;
    case ManagedBridgeOp::SERVICE_NAVIGATION: componentKind = 9; break;
    case ManagedBridgeOp::SERVICE_CHARACTER: componentKind = 10; break;
    case ManagedBridgeOp::COLLIDER_READ:
    case ManagedBridgeOp::COLLIDER_WRITE:
    case ManagedBridgeOp::COLLIDER_COMMAND:
        if (integer > 0 && integer < 32) {
            componentKind = static_cast<uint32_t>(integer);
        }
        break;
    case ManagedBridgeOp::PHYSICS_BODY_READ_ROTATION:
    case ManagedBridgeOp::PHYSICS_BODY_WRITE_ROTATION: componentKind = 1; break;
    case ManagedBridgeOp::SERVICE_AUDIO:
        if (request.index <= 10 || request.index == 12 || (request.index >= 14 && request.index <= 19)
            || (request.index >= 23 && request.index <= 25) || (request.index >= 35 && request.index <= 38)) {
            componentKind = 6;
        }
        break;
    case ManagedBridgeOp::SERVICE_UI:
        if (request.index <= 5 || request.index == 10 || request.index == 17) {
            componentKind = 13;
        }
        else if (request.index == 6 || request.index == 12 || request.index == 13 || request.index == 18
            || (request.index >= 24 && request.index <= 27)) componentKind = 12;
        else if (request.index == 7 || request.index == 14 || request.index == 15 || request.index == 19
            || request.index == 20 || request.index == 28 || (request.index >= 62 && request.index <= 64)) componentKind = 11;
        else if (request.index >= 29 && request.index <= 33) componentKind = 20;
        else if (request.index >= 34 && request.index <= 37) componentKind = 21;
        else if (request.index >= 38 && request.index <= 41) componentKind = 22;
        else if (request.index >= 42 && request.index <= 47) componentKind = 23;
        else if (request.index >= 48 && request.index <= 51) componentKind = 24;
        else if (request.index >= 55 && request.index <= 57) componentKind = 25;
        else if (request.index >= 58 && request.index <= 61) componentKind = 26;
        else if (request.index >= 76 && request.index <= 79) componentKind = 14;
        else if (request.index >= 65 && request.index <= 73) {
            if (ManagedObjectControls::IsPendingComponentRemoval(target, 11)
                || ManagedObjectControls::IsPendingComponentRemoval(target, 12)) {
                return ManagedBridgeStatus::NOT_FOUND;
            }
        } else if (request.index >= 52 && request.index <= 54) {
            for (uint32_t kind : {13U, 20U, 21U, 23U})
                if (ManagedObjectControls::IsPendingComponentRemoval(target, kind)) {
                    return ManagedBridgeStatus::NOT_FOUND;
                }
        } else if (request.index == 11 || request.index == 21 || request.index == 75) {
            size_t depth = 0;
            for (GameObject* current = &target; current && depth < scene.EntitySlotCount(); current = current->GetParent(), ++depth)
                if (current->GetComponent<UICanvas>()) {
                    if (ManagedObjectControls::IsPendingComponentRemoval(*current, 14)) {
                        return ManagedBridgeStatus::NOT_FOUND;
                    }
                    break;
                }
        }
        break;
    default: break;
    }
    if (componentKind && ManagedObjectControls::IsPendingComponentRemoval(target, componentKind)) {
        return ManagedBridgeStatus::NOT_FOUND;
    }
    ManagedServiceResult output;
    if (request.op == ManagedBridgeOp::COLLIDER_READ || request.op == ManagedBridgeOp::COLLIDER_WRITE) {
        const bool writing = request.op == ManagedBridgeOp::COLLIDER_WRITE;
        if (request.flags != (writing ? 0u : 1u) || integer < 0 || integer > UINT32_MAX || arguments.relatedId) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        const auto status = m_impl->physics.RunCollider(target, static_cast<uint32_t>(integer), request.index, writing, p, t, output, fixedUpdate);
        if (status != ManagedBridgeStatus::OK) {
            return status;
        }
    } else if (request.op == ManagedBridgeOp::COLLIDER_COMMAND) {
        if (request.flags || integer < 0 || integer > UINT32_MAX || arguments.relatedId) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        const auto status = m_impl->physics.RunColliderCommand(target, static_cast<uint32_t>(integer), request.index, p, t, output, fixedUpdate);
        if (status != ManagedBridgeStatus::OK) {
            return status;
        }
    } else if (request.op == ManagedBridgeOp::PHYSICS_BODY_READ_ROTATION || request.op == ManagedBridgeOp::PHYSICS_BODY_WRITE_ROTATION) {
        const bool writing = request.op == ManagedBridgeOp::PHYSICS_BODY_WRITE_ROTATION;
        if (request.flags != (writing ? 0u : 1u) || request.index || integer || arguments.relatedId || !t.empty()) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        const auto status = m_impl->physics.RunRotation(target, writing, p, output, fixedUpdate);
        if (status != ManagedBridgeStatus::OK) {
            return status;
        }
    } else {
        const ProxyBindingScope binding(m_impl->view, scene, target);
        ManagedBridgeStatus status;
        switch (request.op) {
        case ManagedBridgeOp::SERVICE_ANIMATOR:
            status = RunManagedAnimatorService(m_impl->view, target, request, arguments, output);
            break;
        case ManagedBridgeOp::SERVICE_AUDIO:
            status = m_impl->audio.Run(scene, caller, m_impl->view, target, request, arguments, output);
            break;
        case ManagedBridgeOp::SERVICE_UI:
            status = RunManagedUiService(m_impl->view, target, request, arguments, output);
            break;
        case ManagedBridgeOp::SERVICE_CAMERA:
            status = RunManagedCameraService(m_impl->view, target, request, arguments, output);
            break;
        case ManagedBridgeOp::SERVICE_NAVIGATION:
            status = RunManagedNavigationService(m_impl->physics, m_impl->view, target, related, request, arguments,
                output, fixedUpdate);
            break;
        case ManagedBridgeOp::SERVICE_CHARACTER:
            status = RunManagedCharacterService(target, request, arguments, output, fixedUpdate, collisionCallback);
            break;
        default: return ManagedBridgeStatus::UNSUPPORTED;
        }
        if (status != ManagedBridgeStatus::OK) {
            return status;
        }
    }
    for (float value : output.values) if (!std::isfinite(value)) return ManagedBridgeStatus::INVALID_ARGUMENT;
    if (output.object && (!output.object->IsValid() || output.object->GetScene() != &scene)) {
        return ManagedBridgeStatus::NOT_FOUND;
    }
    result = std::move(output);
    return ManagedBridgeStatus::OK;
}
void ManagedServicesBridge::EndCall(uint64_t call)
{
    m_impl->storeTransfers.EndCall(call); m_impl->materialTransfers.EndCall(call); m_impl->jointTransfers.EndCall(call);
    m_impl->componentTransfers.EndCall(call); m_impl->renderTransfers.EndCall(call);
    m_impl->assetServices.EndCall(call);
}
ManagedBridgeStatus ManagedServicesBridge::ReadMaterialPacket(Scene& scene, Script& caller, GameObject& target,
    const ManagedBridgeRequest& request, ManagedBridgeReply& reply)
{
    const ProxyBindingScope binding(m_impl->view, scene, target);
    ManagedServiceResult ignored;
    return m_impl->materialTransfers.Run(scene, target, m_impl->view, request, ignored, &reply);
}

ManagedBridgeStatus ManagedServicesBridge::RunJointPacket(Scene& scene, GameObject& target, const ManagedBridgeRequest& request,
    ManagedServiceResult& result, ManagedBridgeStatus (*resolver)(void*, uint64_t, GameObject*&), void* context, bool fixed)
{ return m_impl->jointTransfers.Run(scene, target, request, result, resolver, context, fixed); }
}
