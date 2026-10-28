/// @file    ManagedComponentServices.cpp
/// @brief   公開 runtime Component の入力を検証し既存 row/proxy に接続する。
/// @author  Hasegawa Jin
/// @date    2026-10-06
#include "ManagedComponentServices.hpp"
#include "ManagedServicesBridge.hpp"
#include "ManagedObjectControls.hpp"
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Engine/Scene/Components/MotionWarpComponent.hpp>
#include <Engine/Scene/Components/IKSolverComponent.hpp>
#include <Engine/Scene/Components/RagdollComponent.hpp>
#include <Engine/Scene/Components/SpringBoneComponent.hpp>
#include <Engine/Scene/Components/UIAnimator.hpp>
#include <Engine/Scene/Components/NavMeshPatrolComponent.hpp>
#include <Engine/Scene/Components/SplineComponents.hpp>
#include <Engine/Scene/Components/CameraRigComponents.hpp>
#include <Engine/Scene/Components/AudioSpatialComponents.hpp>
#include <Engine/Scene/Fields/FlowField.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>
namespace fbzz::scene::managed_detail {
namespace {
enum class IntegerMode { ZERO, FLAG, SIGNED, INDEX, MASK };
constexpr size_t ANY_TEXT = std::numeric_limits<size_t>::max();
bool Integer(int64_t value, IntegerMode mode)
{
    switch (mode) {
    case IntegerMode::ZERO: return value == 0;
    case IntegerMode::FLAG: return value == 0 || value == 1;
    case IntegerMode::SIGNED: return value >= INT32_MIN && value <= INT32_MAX;
    case IntegerMode::INDEX: return value >= 0 && value <= INT32_MAX;
    case IntegerMode::MASK: return value >= 0 && value <= UINT32_MAX;
    }
    return false;
}
template<typename T>
T* Row(GameObject& target, ManagedNativeComponentKind kind)
{
    return ManagedObjectControls::IsPendingComponentRemoval(target, static_cast<uint32_t>(kind))
        ? nullptr : target.GetComponent<T>();
}
bool Names(std::span<const std::string> names)
{ return std::all_of(names.begin(), names.end(), [](const std::string& value) { return !value.empty() && value.find('\0') == std::string::npos; }); }
} /// @note namespace
bool IsComponentServiceRead(ManagedBridgeOp operation, uint32_t selector)
{
    switch (operation) {
    case ManagedBridgeOp::SERVICE_MOTION_WARP: return selector == 0 || (selector >= 2 && selector <= 4) || selector == 12 || selector == 13;
    case ManagedBridgeOp::SERVICE_IK: return selector == 0 || selector == 2 || selector == 3 || selector == 5;
    case ManagedBridgeOp::SERVICE_RAGDOLL: return selector == 0 || (selector >= 2 && selector <= 12);
    case ManagedBridgeOp::SERVICE_SPRING_BONE: return selector == 0 || selector == 3 || selector == 4 || selector == 6;
    case ManagedBridgeOp::SERVICE_UI_ANIMATOR: return selector == 0 || selector == 2;
    case ManagedBridgeOp::SERVICE_PATROL: return selector == 0 || selector == 2 || selector == 3 || selector == 4 || selector == 6 || selector == 12;
    case ManagedBridgeOp::SERVICE_SPLINE_FOLLOWER: return selector == 0 || selector == 2 || selector == 3 || selector == 5 || selector == 9 || selector == 11 || selector == 13 || selector == 15;
    case ManagedBridgeOp::SERVICE_CAMERA_SHAKE: return selector == 0 || selector == 2 || selector == 3 || selector == 7 || selector == 8;
    case ManagedBridgeOp::SERVICE_VIRTUAL_CAMERA: return selector == 0 || selector == 2 || selector == 3 || selector == 5 || selector == 7;
    case ManagedBridgeOp::SERVICE_AUDIO_MIXER_SEND: return selector == 0 || selector == 2 || selector == 3;
    case ManagedBridgeOp::SERVICE_FLOW_FIELD: return selector % 2 == 0 && selector <= 16;
    default: return false;
    }
}
ManagedBridgeStatus RunComponentServices(Scene& scene, Script& view, GameObject& target, GameObject* related,
    const ManagedBridgeRequest& request, int64_t integer, std::span<const float> p,
    std::span<const std::string> t, ManagedServiceResult& result, bool fixedUpdate)
{
    if (request.flags > 1 || !std::all_of(p.begin(), p.end(), [](float value) { return std::isfinite(value); })) return ManagedBridgeStatus::INVALID_ARGUMENT;
    const bool read = request.flags == 1;
    if (read != IsComponentServiceRead(request.op, request.index)) return ManagedBridgeStatus::INVALID_ARGUMENT;
    if (!read && request.op == ManagedBridgeOp::SERVICE_RAGDOLL && (!fixedUpdate || !Script::IsInPlayMode())) return ManagedBridgeStatus::INVALID_SCOPE;
    const bool editVisual = request.op == ManagedBridgeOp::SERVICE_UI_ANIMATOR || request.op == ManagedBridgeOp::SERVICE_FLOW_FIELD
        || request.op == ManagedBridgeOp::SERVICE_VIRTUAL_CAMERA;
    if (!read && !editVisual && !Script::IsInPlayMode()) return ManagedBridgeStatus::INVALID_SCOPE;
    const auto args = [&](size_t floats, size_t strings = 0, IntegerMode mode = IntegerMode::ZERO, bool allowRelated = false) {
        return p.size() == floats && (strings == ANY_TEXT || t.size() == strings) && Integer(integer, mode)
            && (!related || (allowRelated && related->GetScene() == &scene && scene.GetGameObject(related->GetID()) == related));
    };
    const auto v3 = [&](size_t offset = 0) { return math::Vector3{p[offset], p[offset + 1], p[offset + 2]}; };
    ManagedServiceResult output;
    switch (request.op) {
    case ManagedBridgeOp::SERVICE_MOTION_WARP: {
        auto* row = Row<MotionWarpComponent>(target, ManagedNativeComponentKind::MOTION_WARP);
        if (!row) return ManagedBridgeStatus::NOT_FOUND;
        switch (request.index) {
        case 0: case 2: case 3: case 4: case 12: case 13:
            if (!args(0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
            if (request.index == 0) output.integer = row->enabled;
            if (request.index == 2) output.integer = view.motionWarp.IsWarping();
            if (request.index == 3) output.values = {view.motionWarp.GetRemainingTime()};
            if (request.index == 4) output.values = {view.motionWarp.GetRemainingDistance()};
            if (request.index == 12) output.object = view.motionWarp.GetTargetPoint();
            if (request.index == 13) {
                const auto mode = view.motionWarp.GetTargetPointMode();
                if (mode != MotionWarpPointMode::CAPTURED && mode != MotionWarpPointMode::LIVE) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.integer = static_cast<int64_t>(mode);
            }
            break;
        case 1:
            if (!args(0, 0, IntegerMode::FLAG)) return ManagedBridgeStatus::INVALID_ARGUMENT;
            view.motionWarp.SetEnabled(integer != 0); break;
        case 5:
            if (!args(4) || p[3] <= 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
            view.motionWarp.WarpTo(v3(), p[3]); break;
        case 6: {
            if (!args(8) || p[7] <= 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
            const math::Quaternion rotation{p[3], p[4], p[5], p[6]};
            const float length = rotation.Length();
            if (!std::isfinite(length) || length <= math::EPSILON) return ManagedBridgeStatus::INVALID_ARGUMENT;
            view.motionWarp.WarpToPose(v3(), rotation, p[7]); break;
        }
        case 7:
            if (!args(3) || std::any_of(p.begin(), p.end(), [](float value) { return value < 0 || value > 1; })) return ManagedBridgeStatus::INVALID_ARGUMENT;
            view.motionWarp.SetAxisWeight(v3()); break;
        case 8:
            if (!args(1) || p[0] < 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
            view.motionWarp.SetMaxSpeed(p[0]); break;
        case 9:
            if (!args(0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
            view.motionWarp.Cancel(); break;
        case 10:
            if (!args(0, 0, IntegerMode::FLAG, true)) return ManagedBridgeStatus::INVALID_ARGUMENT;
            if (!view.motionWarp.SetTargetPoint(related ? related->GetID() : EntityID::INVALID,
                static_cast<MotionWarpPointMode>(integer))) return ManagedBridgeStatus::INVALID_ARGUMENT;
            break;
        case 11:
            if (!args(1) || p[0] <= 0.0f) return ManagedBridgeStatus::INVALID_ARGUMENT;
            output.integer = view.motionWarp.WarpToTarget(p[0]); break;
        default: return ManagedBridgeStatus::UNSUPPORTED;
        }
        break;
    }
    case ManagedBridgeOp::SERVICE_IK: {
        auto* row = Row<IKSolverComponent>(target, ManagedNativeComponentKind::IK_SOLVER);
        if (!row) return ManagedBridgeStatus::NOT_FOUND;
        if (request.index == 0) { if (!args(0)) return ManagedBridgeStatus::INVALID_ARGUMENT; output.integer = row->enabled; }
        else if (request.index == 1 || request.index == 7) {
            if (!args(0, 0, IntegerMode::FLAG)) return ManagedBridgeStatus::INVALID_ARGUMENT;
            if (request.index == 1) view.ik.SetEnabled(integer != 0); else view.ik.SetAllEnabled(integer != 0);
        } else if (request.index == 2 || request.index == 3 || request.index == 5) {
            if (!args(0, 1) || t[0].empty()) return ManagedBridgeStatus::INVALID_ARGUMENT;
            if (request.index == 2) output.integer = view.ik.HasChain(t[0]);
            if (request.index == 3) output.integer = view.ik.IsChainEnabled(t[0]);
            if (request.index == 5) output.values = {view.ik.GetChainWeight(t[0])};
        } else if (request.index == 4) {
            if (!args(0, 1, IntegerMode::FLAG) || t[0].empty()) return ManagedBridgeStatus::INVALID_ARGUMENT;
            view.ik.SetChainEnabled(t[0], integer != 0);
        } else if (request.index == 6) {
            if (!args(1, 1) || t[0].empty()) return ManagedBridgeStatus::INVALID_ARGUMENT;
            view.ik.SetChainWeight(t[0], p[0]);
        } else if (request.index == 8 || request.index == 9) {
            if (!args(0, 1, IntegerMode::ZERO, true) || t[0].empty()) return ManagedBridgeStatus::INVALID_ARGUMENT;
            if (request.index == 8) view.ik.SetChainTarget(t[0], related ? related->GetID() : EntityID::INVALID);
            else view.ik.SetChainPole(t[0], related ? related->GetID() : EntityID::INVALID);
        } else return ManagedBridgeStatus::UNSUPPORTED;
        break;
    }
    case ManagedBridgeOp::SERVICE_RAGDOLL: {
        auto* row = Row<RagdollComponent>(target, ManagedNativeComponentKind::RAGDOLL);
        if (!row) return ManagedBridgeStatus::NOT_FOUND;
        if (read) {
            if (!args(0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
            switch (request.index) {
            case 0: output.integer = row->enabled; break;
            case 2: output.integer = view.ragdoll.IsActive(); break;
            case 3: output.integer = view.ragdoll.IsStanding(); break;
            case 4: output.values = {view.ragdoll.GetWeight()}; break;
            case 5: output.values = {view.ragdoll.GetDeviation()}; break;
            case 6:
                if (static_cast<int>(row->runtimeStatus) < 0 || static_cast<int>(row->runtimeStatus) > 7) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.text = {view.ragdoll.GetStatus()}; break;
            case 7: output.integer = view.ragdoll.GetParticleCount(); break;
            case 8: output.integer = view.ragdoll.GetSaturatedJointCount(); break;
            case 9: output.integer = view.ragdoll.GetLimitedJointCount(); break;
            case 10: output.integer = view.ragdoll.GetContactCount(); break;
            case 11: output.integer = view.ragdoll.GetUnmatchedBoneCount(); break;
            case 12: output.integer = view.ragdoll.GetRootCount(); break;
            default: return ManagedBridgeStatus::UNSUPPORTED;
            }
            break;
        }
        switch (request.index) {
        case 1:
            if (!args(0, 0, IntegerMode::FLAG)) return ManagedBridgeStatus::INVALID_ARGUMENT;
            view.ragdoll.SetEnabled(integer != 0); break;
        case 13:
            if (!args(3) || p[0] < 0 || p[1] < 0 || p[1] > 1 || p[2] < 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
            view.ragdoll.Begin(p[0], p[1], p[2]); break;
        case 14:
            if (!args(1) || p[0] < 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
            view.ragdoll.BeginActive(p[0]); break;
        case 15:
            if (!args(0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
            view.ragdoll.End(); break;
        case 16: case 20:
            if (!args(3)) return ManagedBridgeStatus::INVALID_ARGUMENT;
            if (request.index == 16) view.ragdoll.Push(v3()); else view.ragdoll.PushAngular(v3());
            break;
        case 17: case 21:
            if (!args(7) || p[6] < 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
            if (request.index == 17) view.ragdoll.PushAt(v3(), v3(3), p[6]); else view.ragdoll.PushAngularAt(v3(), v3(3), p[6]);
            break;
        case 18:
            if (!args(0, 1, IntegerMode::INDEX) || t[0].find('\0') != std::string::npos) return ManagedBridgeStatus::INVALID_ARGUMENT;
            view.ragdoll.SetRoot(t[0].c_str(), static_cast<int>(integer)); break;
        case 19:
            if (!args(0, ANY_TEXT, IntegerMode::INDEX) || !Names(t)) return ManagedBridgeStatus::INVALID_ARGUMENT;
            view.ragdoll.SetRoots(std::vector<std::string>(t.begin(), t.end()), static_cast<int>(integer)); break;
        case 22: case 26:
            if (!args(1)) return ManagedBridgeStatus::INVALID_ARGUMENT;
            if (request.index == 22) view.ragdoll.SetGravity(p[0]); else view.ragdoll.SetCollapse(p[0]);
            break;
        case 23: case 25:
            if (!args(2)) return ManagedBridgeStatus::INVALID_ARGUMENT;
            if (request.index == 23) view.ragdoll.SetBlend(p[0], p[1]); else view.ragdoll.SetRecovery(p[0], p[1]);
            break;
        case 24:
            if (!args(3)) return ManagedBridgeStatus::INVALID_ARGUMENT;
            view.ragdoll.SetMuscle(p[0], p[1], p[2]); break;
        case 27:
            if (!args(2, 0, IntegerMode::FLAG)) return ManagedBridgeStatus::INVALID_ARGUMENT;
            view.ragdoll.SetStandingGuard(integer != 0, p[0], p[1]); break;
        case 28:
            if (!args(0, 0, IntegerMode::INDEX) || integer > 1) return ManagedBridgeStatus::INVALID_ARGUMENT;
            view.ragdoll.SetProfile(static_cast<ScriptRagdollProfile>(integer)); break;
        case 29:
            if (!args(0, ANY_TEXT) || !Names(t)) return ManagedBridgeStatus::INVALID_ARGUMENT;
            view.ragdoll.SetExcludedBranches(std::vector<std::string>(t.begin(), t.end())); break;
        case 30:
            if (!args(3)) return ManagedBridgeStatus::INVALID_ARGUMENT;
            view.ragdoll.SetRootAnchor(p[0], p[1], p[2]); break;
        case 31:
            if (!args(0, 0, IntegerMode::MASK) || integer > 15) return ManagedBridgeStatus::INVALID_ARGUMENT;
            view.ragdoll.SetContacts((integer & 1) != 0, (integer & 2) != 0, (integer & 4) != 0, (integer & 8) != 0); break;
        case 32:
            if (!args(1, 0, IntegerMode::FLAG)) return ManagedBridgeStatus::INVALID_ARGUMENT;
            view.ragdoll.SetGroundPlane(integer != 0, p[0]); break;
        default: return ManagedBridgeStatus::UNSUPPORTED;
        }
        break;
    }
    case ManagedBridgeOp::SERVICE_SPRING_BONE: {
        auto* row = Row<SpringBoneComponent>(target, ManagedNativeComponentKind::SPRING_BONE);
        if (!row) return ManagedBridgeStatus::NOT_FOUND;
        switch (request.index) {
        case 0:
            if (!args(0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
            output.integer = row->enabled; break;
        case 1:
            if (!args(0, 0, IntegerMode::FLAG)) return ManagedBridgeStatus::INVALID_ARGUMENT;
            view.springBone.SetEnabled(integer != 0); break;
        case 2:
            if (!args(0, 1, IntegerMode::INDEX) || t[0].empty()) return ManagedBridgeStatus::INVALID_ARGUMENT;
            view.springBone.EnsureChain(t[0], static_cast<int>(integer)); break;
        case 3: case 4: case 6:
            if (!args(0, 1) || t[0].empty()) return ManagedBridgeStatus::INVALID_ARGUMENT;
            if (request.index == 3) output.integer = view.springBone.HasChain(t[0]);
            if (request.index == 4) output.integer = view.springBone.IsChainEnabled(t[0]);
            if (request.index == 6) output.values = {view.springBone.GetWeight(t[0])};
            break;
        case 5:
            if (!args(0, 1, IntegerMode::FLAG) || t[0].empty()) return ManagedBridgeStatus::INVALID_ARGUMENT;
            view.springBone.SetChainEnabled(t[0], integer != 0); break;
        case 7: case 9: case 10:
            if (!args(1, 1) || t[0].empty()) return ManagedBridgeStatus::INVALID_ARGUMENT;
            if (request.index == 7) view.springBone.SetWeight(t[0], p[0]);
            if (request.index == 9) view.springBone.SetLimitAngle(t[0], p[0]);
            if (request.index == 10) view.springBone.SetRadius(t[0], p[0]);
            break;
        case 8:
            if (!args(2, 1) || t[0].empty()) return ManagedBridgeStatus::INVALID_ARGUMENT;
            view.springBone.SetSpring(t[0], p[0], p[1]); break;
        case 11:
            if (!args(4, 1) || t[0].empty()) return ManagedBridgeStatus::INVALID_ARGUMENT;
            view.springBone.SetForce(t[0], v3(), p[3]); break;
        case 12:
            if (!args(0, 1) || t[0].empty()) return ManagedBridgeStatus::INVALID_ARGUMENT;
            view.springBone.ClearForce(t[0]); break;
        case 13:
            if (!args(0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
            view.springBone.ResetAll(); break;
        default: return ManagedBridgeStatus::UNSUPPORTED;
        }
        break;
    }
    case ManagedBridgeOp::SERVICE_UI_ANIMATOR: {
        auto* row = Row<UIAnimator>(target, ManagedNativeComponentKind::UI_ANIMATOR);
        if (!row) return ManagedBridgeStatus::NOT_FOUND;
        if (request.index == 0 || request.index == 2) {
            if (!args(0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
            output.integer = request.index == 0 ? row->enabled : row->IsPlaying();
        } else if (request.index == 1) {
            if (!args(0, 0, IntegerMode::FLAG)) return ManagedBridgeStatus::INVALID_ARGUMENT;
            row->enabled = integer != 0;
        } else if (request.index >= 3 && request.index <= 8) {
            const size_t count = request.index == 3 ? 9 : request.index <= 5 ? 5 : 3;
            if (!args(count, request.index == 8 ? 1 : 0, IntegerMode::MASK) || integer > 15 || p.back() < 0
                || (request.index == 8 && t[0].empty())) return ManagedBridgeStatus::INVALID_ARGUMENT;
            const auto easing = static_cast<UIEasingType>(integer & 3); const bool loop = (integer & 4) != 0, pingPong = (integer & 8) != 0;
            if (request.index == 3) row->PlayColor({p[0], p[1], p[2], p[3]}, {p[4], p[5], p[6], p[7]}, p[8], easing, loop, pingPong);
            if (request.index == 4) row->PlayPosition({p[0], p[1]}, {p[2], p[3]}, p[4], easing, loop, pingPong);
            if (request.index == 5) row->PlayScale({p[0], p[1]}, {p[2], p[3]}, p[4], easing, loop, pingPong);
            if (request.index == 6) row->PlayRotation(p[0], p[1], p[2], easing, loop, pingPong);
            if (request.index == 7) row->PlayFill(p[0], p[1], p[2], easing, loop, pingPong);
            if (request.index == 8) row->PlayMaterialFloat(t[0], p[0], p[1], p[2], easing, loop, pingPong);
        } else if (request.index == 9) {
            if (!args(1) || p[0] < 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
            row->SetDelay(p[0]);
        } else if (request.index == 10) {
            if (!args(0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
            row->StopAll();
        } else if (request.index == 11) {
            if (!args(0, 0, IntegerMode::INDEX) || integer > 5) return ManagedBridgeStatus::INVALID_ARGUMENT;
            switch (integer) { case 0: row->StopColor(); break; case 1: row->StopPosition(); break; case 2: row->StopScale(); break;
            case 3: row->StopRotation(); break; case 4: row->StopFill(); break; case 5: row->StopMaterial(); break; }
        } else return ManagedBridgeStatus::UNSUPPORTED;
        break;
    }
    case ManagedBridgeOp::SERVICE_PATROL: {
        auto* row = Row<NavMeshPatrolComponent>(target, ManagedNativeComponentKind::NAV_MESH_PATROL);
        if (!row) return ManagedBridgeStatus::NOT_FOUND;
        switch (request.index) {
        case 0: case 2: case 3: case 4: case 6:
            if (!args(0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
            if (request.index == 0) output.integer = row->enabled;
            if (request.index == 2) output.integer = view.patrol.GetCurrentIndex();
            if (request.index == 3) output.integer = static_cast<int64_t>(view.patrol.GetWaypointCount());
            if (request.index == 4) {
                const int mode = static_cast<int>(row->mode); if (mode < 0 || mode > 1) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.integer = mode;
            }
            if (request.index == 6) output.values = {row->waitTime};
            break;
        case 1:
            if (!args(0, 0, IntegerMode::FLAG)) return ManagedBridgeStatus::INVALID_ARGUMENT;
            view.patrol.SetEnabled(integer != 0); break;
        case 5:
            if (!args(0, 0, IntegerMode::INDEX) || integer > 1) return ManagedBridgeStatus::INVALID_ARGUMENT;
            view.patrol.SetMode(static_cast<ScriptPatrolMode>(integer)); break;
        case 7:
            if (!args(1)) return ManagedBridgeStatus::INVALID_ARGUMENT;
            view.patrol.SetWaitTime(p[0]); break;
        case 8:
            if (!args(0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
            view.patrol.ClearWaypoints(); break;
        case 9:
            if (!args(5)) return ManagedBridgeStatus::INVALID_ARGUMENT;
            view.patrol.AddWaypoint(v3(), p[3], p[4]); break;
        case 10:
            if (!args(3, 0, IntegerMode::INDEX)) return ManagedBridgeStatus::INVALID_ARGUMENT;
            output.integer = view.patrol.SetWaypoint(static_cast<size_t>(integer), v3()); break;
        case 11:
            if (!args(0, 0, IntegerMode::INDEX)) return ManagedBridgeStatus::INVALID_ARGUMENT;
            view.patrol.Restart(static_cast<size_t>(integer)); break;
        case 12: {
            if (!args(0, 0, IntegerMode::INDEX)) return ManagedBridgeStatus::INVALID_ARGUMENT;
            math::Vector3 point; output.integer = view.patrol.GetWaypoint(static_cast<size_t>(integer), point);
            if (output.integer) output.values = {point.x, point.y, point.z}; break;
        }
        default: return ManagedBridgeStatus::UNSUPPORTED;
        }
        break;
    }
    case ManagedBridgeOp::SERVICE_SPLINE_FOLLOWER: {
        auto* row = Row<SplineFollowerComponent>(target, ManagedNativeComponentKind::SPLINE_FOLLOWER);
        if (!row) return ManagedBridgeStatus::NOT_FOUND;
        if (request.index == 0 || request.index == 2 || request.index == 3 || request.index == 5 || request.index == 9
            || request.index == 11 || request.index == 13 || request.index == 15) {
            if (!args(0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
            if (request.index == 0) output.integer = row->enabled;
            if (request.index == 2) output.integer = row->playing;
            if (request.index == 3) output.values = {row->speed};
            if (request.index == 5) output.values = {row->normalizedPosition};
            if (request.index == 9) {
                const int mode = static_cast<int>(row->wrapMode); if (mode < 0 || mode > 2) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.integer = mode;
            }
            if (request.index == 11) output.integer = row->orientToPath;
            if (request.index == 13) output.integer = row->reverse;
            if (request.index == 15) output.object = row->spline.Resolve(scene);
        } else if (request.index == 1 || request.index == 7 || request.index == 12 || request.index == 14) {
            if (!args(0, 0, IntegerMode::FLAG)) return ManagedBridgeStatus::INVALID_ARGUMENT;
            if (request.index == 1) row->enabled = integer != 0;
            if (request.index == 7) output.integer = view.gameplay.PlaySpline(integer != 0);
            if (request.index == 12) row->orientToPath = integer != 0;
            if (request.index == 14) row->reverse = integer != 0;
        } else if (request.index == 4 || request.index == 6) {
            if (!args(1)) return ManagedBridgeStatus::INVALID_ARGUMENT;
            if (request.index == 4) row->speed = p[0]; else output.integer = view.gameplay.SetSplinePosition(p[0]);
        } else if (request.index == 8) {
            if (!args(0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
            output.integer = view.gameplay.PauseSpline();
        } else if (request.index == 10) {
            if (!args(0, 0, IntegerMode::INDEX) || integer > 2) return ManagedBridgeStatus::INVALID_ARGUMENT;
            row->wrapMode = static_cast<SplineWrapMode>(integer);
        } else if (request.index == 16) {
            if (!args(0, 0, IntegerMode::ZERO, true) || (related && !related->GetComponent<SplineComponent>())) return ManagedBridgeStatus::INVALID_ARGUMENT;
            row->spline.id = related ? related->GetID() : EntityID::INVALID;
        } else return ManagedBridgeStatus::UNSUPPORTED;
        break;
    }
    case ManagedBridgeOp::SERVICE_CAMERA_SHAKE: {
        auto* row = Row<CameraShakeComponent>(target, ManagedNativeComponentKind::CAMERA_SHAKE);
        if (!row) return ManagedBridgeStatus::NOT_FOUND;
        switch (request.index) {
        case 0: case 2: case 3: case 7: case 8:
            if (!args(0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
            if (request.index == 0) output.integer = row->enabled;
            if (request.index == 2) output.integer = row->playing;
            if (request.index == 3) output.values = {row->elapsed};
            if (request.index == 7) output.values = {row->amplitude, row->rotationAmplitudeDegrees, row->frequency, row->duration, row->falloffPower};
            if (request.index == 8) output.integer = row->seed;
            break;
        case 1:
            if (!args(0, 0, IntegerMode::FLAG)) return ManagedBridgeStatus::INVALID_ARGUMENT;
            row->enabled = integer != 0; break;
        case 4:
            if (!args(2, 0, IntegerMode::SIGNED)) return ManagedBridgeStatus::INVALID_ARGUMENT;
            output.integer = view.gameplay.StartCameraShake(p[0], p[1], static_cast<int>(integer)); break;
        case 5:
            if (!args(0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
            row->playing = false; break;
        case 6:
            if (!args(3) || p[0] < 0 || p[1] < 0 || p[2] < 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
            row->rotationAmplitudeDegrees = p[0]; row->frequency = p[1]; row->falloffPower = p[2]; break;
        default: return ManagedBridgeStatus::UNSUPPORTED;
        }
        break;
    }
    case ManagedBridgeOp::SERVICE_VIRTUAL_CAMERA: {
        auto* row = Row<VirtualCameraComponent>(target, ManagedNativeComponentKind::VIRTUAL_CAMERA);
        if (!row) return ManagedBridgeStatus::NOT_FOUND;
        switch (request.index) {
        case 0: case 2: case 3: case 5: case 7:
            if (!args(0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
            if (request.index == 0) output.integer = row->enabled;
            if (request.index == 2) output.integer = row->active;
            if (request.index == 3) output.integer = row->priority;
            if (request.index == 5) output.values = {row->fovY};
            if (request.index == 7) output.values = {row->blendInTime};
            break;
        case 1:
            if (!args(0, 0, IntegerMode::FLAG)) return ManagedBridgeStatus::INVALID_ARGUMENT;
            row->enabled = integer != 0; break;
        case 4:
            if (!args(0, 0, IntegerMode::SIGNED)) return ManagedBridgeStatus::INVALID_ARGUMENT;
            output.integer = view.gameplay.SetVirtualCameraPriority(static_cast<int>(integer)); break;
        case 6: case 8:
            if (!args(1)) return ManagedBridgeStatus::INVALID_ARGUMENT;
            if (request.index == 6) row->fovY = std::clamp(p[0], 1.0f, 179.0f); else row->blendInTime = (std::max)(0.0f, p[0]);
            break;
        default: return ManagedBridgeStatus::UNSUPPORTED;
        }
        break;
    }
    case ManagedBridgeOp::SERVICE_AUDIO_MIXER_SEND: {
        auto* row = Row<AudioMixerSendComponent>(target, ManagedNativeComponentKind::AUDIO_MIXER_SEND);
        if (!row) return ManagedBridgeStatus::NOT_FOUND;
        switch (request.index) {
        case 0: case 2: case 3:
            if (!args(0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
            if (request.index == 0) output.integer = row->enabled;
            if (request.index == 2) output.text = {row->busName};
            if (request.index == 3) output.values = {row->sendLevel};
            break;
        case 1:
            if (!args(0, 0, IntegerMode::FLAG)) return ManagedBridgeStatus::INVALID_ARGUMENT;
            row->enabled = integer != 0; break;
        case 4:
            if (!args(1, 1) || t[0].empty()) return ManagedBridgeStatus::INVALID_ARGUMENT;
            output.integer = view.gameplay.SetAudioMixerSend(t[0], p[0]); break;
        default: return ManagedBridgeStatus::UNSUPPORTED;
        }
        break;
    }
    case ManagedBridgeOp::SERVICE_FLOW_FIELD: {
        auto* row = Row<FlowField>(target, ManagedNativeComponentKind::FLOW_FIELD);
        if (!row) return ManagedBridgeStatus::NOT_FOUND;
        if (request.index == 16) {
            if (!args(0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
            output.integer = static_cast<int64_t>(row->forces.size()); break;
        }
        if (row->forces.empty()) return ManagedBridgeStatus::NOT_FOUND;
        auto& force = row->forces.front();
        if (read) {
            if (!args(0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
            switch (request.index) {
            case 0: output.integer = force.enabled; break;
            case 2: {
                const int type = static_cast<int>(force.fieldType); if (type < 0 || type > 6) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.integer = type; break;
            }
            case 4: output.values = {force.strength}; break;
            case 6: output.values = {force.radius}; break;
            case 8: output.values = {force.direction.x, force.direction.y, force.direction.z}; break;
            case 10: output.values = {force.noiseFrequency, force.noiseSpeed}; break;
            case 12: output.integer = force.channels; break;
            default: return ManagedBridgeStatus::UNSUPPORTED;
            }
        } else switch (request.index) {
        case 1:
            if (!args(0, 0, IntegerMode::FLAG)) return ManagedBridgeStatus::INVALID_ARGUMENT;
            view.flowField.SetEnabled(integer != 0); break;
        case 3:
            if (!args(0, 0, IntegerMode::INDEX) || integer > 4) return ManagedBridgeStatus::INVALID_ARGUMENT;
            view.flowField.SetType(static_cast<ScriptFlowFieldType>(integer)); break;
        case 5:
            if (!args(1)) return ManagedBridgeStatus::INVALID_ARGUMENT;
            view.flowField.SetSpeed(p[0]); break;
        case 7:
            if (!args(2)) return ManagedBridgeStatus::INVALID_ARGUMENT;
            view.flowField.SetRadius(p[0], p[1]); break;
        case 9:
            if (!args(3)) return ManagedBridgeStatus::INVALID_ARGUMENT;
            view.flowField.SetDirection(v3()); break;
        case 11:
            if (!args(2)) return ManagedBridgeStatus::INVALID_ARGUMENT;
            view.flowField.SetTurbulence(p[0], p[1]); break;
        case 13:
            if (!args(0, 0, IntegerMode::MASK)) return ManagedBridgeStatus::INVALID_ARGUMENT;
            view.flowField.SetChannels(static_cast<uint32_t>(integer)); break;
        default: return ManagedBridgeStatus::UNSUPPORTED;
        }
        break;
    }
    default: return ManagedBridgeStatus::UNSUPPORTED;
    }
    if (!std::all_of(output.values.begin(), output.values.end(), [](float value) { return std::isfinite(value); })) return ManagedBridgeStatus::INVALID_ARGUMENT;
    result = std::move(output); return ManagedBridgeStatus::OK;
}
} /// @note namespace fbzz::scene::managed_detail
