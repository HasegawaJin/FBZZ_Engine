/// @file    ManagedServiceMotion.cpp
/// @brief   Animator と NavMeshAgent の要求を既存のスクリプト操作へ転送する。
/// @author  Hasegawa Jin
/// @date    2026-11-21
#include "../ManagedServiceRoutes.hpp"
#include "../Physics/ManagedPhysicsBridge.hpp"

#include <Engine/Scene/Components/Animation/AnimatorComponent.hpp>
#include <Engine/Scene/Components/Navigation/NavMeshAgentComponent.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Scripting/Script.hpp>

namespace fbzz::scene::managed_detail {

ManagedBridgeStatus RunManagedAnimatorService(Script& view, GameObject& target, const ManagedBridgeRequest& request,
    const ManagedServiceArguments& arguments, ManagedServiceResult& output)
{
    const auto& p = arguments.values;
    const auto& t = arguments.text;
    const int64_t integer = arguments.integer;
    if (!target.GetComponent<AnimatorComponent>()) {
        return ManagedBridgeStatus::NOT_FOUND;
    }
    switch (static_cast<ManagedAnimatorMethod>(request.index)) {
    case ManagedAnimatorMethod::SET_FLOAT: {
        if (!MatchesServiceArguments(request, arguments, 0u, 1, 1)) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        view.animator.SetFloat(t[0], p[0]);
        break;
    }
    case ManagedAnimatorMethod::GET_FLOAT: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 1)) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        output.values = {view.animator.GetFloat(t[0])};
        break;
    }
    case ManagedAnimatorMethod::SET_INT: {
        if (request.flags != 0u || p.size() != 0 || t.size() != 1) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        if (!FitsInt(integer)) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        if (arguments.relatedId) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        view.animator.SetInt(t[0], static_cast<int>(integer));
        break;
    }
    case ManagedAnimatorMethod::GET_INT: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 1)) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        output.integer = static_cast<int64_t>(view.animator.GetInt(t[0]));
        break;
    }
    case ManagedAnimatorMethod::SET_BOOL: {
        if (request.flags != 0u || p.size() != 0 || t.size() != 1) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        if (integer != 0 && integer != 1) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        if (arguments.relatedId) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        view.animator.SetBool(t[0], integer != 0);
        break;
    }
    case ManagedAnimatorMethod::GET_BOOL: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 1)) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        output.integer = static_cast<int64_t>(view.animator.GetBool(t[0]));
        break;
    }
    case ManagedAnimatorMethod::SET_TRIGGER: {
        if (!MatchesServiceArguments(request, arguments, 0u, 0, 1)) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        view.animator.SetTrigger(t[0]);
        break;
    }
    case ManagedAnimatorMethod::IS_IN_STATE: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 1)) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        output.integer = static_cast<int64_t>(view.animator.IsInState(t[0]));
        break;
    }
    case ManagedAnimatorMethod::GET_NORMALIZED_TIME: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 0)) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        output.values = {view.animator.GetNormalizedTime()};
        break;
    }
    case ManagedAnimatorMethod::GET_CURRENT_STATE: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 0)) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        output.text.emplace_back(view.animator.GetCurrentState());
        break;
    }
    case ManagedAnimatorMethod::GET_BLEND_TO_STATE: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 0)) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        output.text.emplace_back(view.animator.GetBlendToState());
        break;
    }
    case ManagedAnimatorMethod::GET_BLEND_TO_NORMALIZED_TIME: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 0)) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        output.values = {view.animator.GetBlendToNormalizedTime()};
        break;
    }
    case ManagedAnimatorMethod::GET_SPEED: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 0)) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        output.values = {view.animator.GetSpeed()};
        break;
    }
    case ManagedAnimatorMethod::IS_PLAYING: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 0)) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        output.integer = static_cast<int64_t>(view.animator.IsPlaying());
        break;
    }
    case ManagedAnimatorMethod::GET_ROOT_MOTION_DELTA_POSITION: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 0)) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        const auto value = view.animator.GetRootMotionDeltaPosition(); output.values = {value.x, value.y, value.z};
        break;
    }
    case ManagedAnimatorMethod::GET_ROOT_MOTION_DELTA_ROTATION: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 0)) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        const auto value = view.animator.GetRootMotionDeltaRotation(); output.values = {value.x, value.y, value.z, value.w};
        break;
    }
    case ManagedAnimatorMethod::GET_ROOT_MOTION_WORLD_DELTA: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 0)) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        const auto value = view.animator.GetRootMotionWorldDelta(); output.values = {value.x, value.y, value.z};
        break;
    }
    case ManagedAnimatorMethod::GET_ROOT_MOTION_WORLD_VELOCITY: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 0)) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        const auto value = view.animator.GetRootMotionWorldVelocity(); output.values = {value.x, value.y, value.z};
        break;
    }
    case ManagedAnimatorMethod::GET_ROOT_MOTION_DELTA_TIME: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 0)) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        output.values = {view.animator.GetRootMotionDeltaTime()};
        break;
    }
    case ManagedAnimatorMethod::IS_ROOT_MOTION_APPLIED_BY_ENGINE: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 0)) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        output.integer = static_cast<int64_t>(view.animator.IsRootMotionAppliedByEngine());
        break;
    }
    case ManagedAnimatorMethod::GET_ROOT_MOTION_MODE: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 0)) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        output.integer = static_cast<int64_t>(view.animator.GetRootMotionMode());
        break;
    }
    case ManagedAnimatorMethod::GET_ROOT_MOTION_POSITION_SCALE: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 0)) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        output.values = {view.animator.GetRootMotionPositionScale()};
        break;
    }
    case ManagedAnimatorMethod::GET_ROOT_MOTION_ROTATION_SCALE: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 0)) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        output.values = {view.animator.GetRootMotionRotationScale()};
        break;
    }
    case ManagedAnimatorMethod::GET_ROOT_MOTION_NODE_NAME: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 0)) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        output.text.emplace_back(view.animator.GetRootMotionNodeName());
        break;
    }
    case ManagedAnimatorMethod::GET_CURRENT_BLEND_WEIGHTS: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 0)) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        output.blendWeights = target.GetComponent<AnimatorComponent>()->currentBlendWeights;
        break;
    }
    case ManagedAnimatorMethod::SET_SPEED: {
        if (!MatchesServiceArguments(request, arguments, 0u, 1, 0)) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        view.animator.SetSpeed(p[0]);
        break;
    }
    case ManagedAnimatorMethod::SET_LOCAL_TIME_SCALE: {
        if (!MatchesServiceArguments(request, arguments, 0u, 1, 0)) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        if (p[0] < 0 || p[0] > 8) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        view.animator.SetLocalTimeScale(p[0]);
        break;
    }
    case ManagedAnimatorMethod::PLAY: {
        if (!MatchesServiceArguments(request, arguments, 0u, 0, 1)) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        view.animator.Play(t[0]);
        break;
    }
    case ManagedAnimatorMethod::SET_PLAYING: {
        if (request.flags != 0u || p.size() != 0 || t.size() != 0) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        if (integer != 0 && integer != 1) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        if (arguments.relatedId) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        view.animator.SetPlaying(integer != 0);
        break;
    }
    case ManagedAnimatorMethod::SET_LAYER_WEIGHT: {
        if (!MatchesServiceArguments(request, arguments, 0u, 1, 1)) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        view.animator.SetLayerWeight(t[0], p[0]);
        break;
    }
    case ManagedAnimatorMethod::GET_LAYER_WEIGHT: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 1)) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        output.values = {view.animator.GetLayerWeight(t[0])};
        break;
    }
    case ManagedAnimatorMethod::GET_LAYER_STATE: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 1)) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        output.text.emplace_back(view.animator.GetLayerState(t[0]));
        break;
    }
    case ManagedAnimatorMethod::IS_LAYER_IN_STATE: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 2)) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        output.integer = static_cast<int64_t>(view.animator.IsLayerInState(t[0], t[1]));
        break;
    }
    case ManagedAnimatorMethod::PLAY_LAYER_STATE: {
        if (!MatchesServiceArguments(request, arguments, 0u, 0, 2)) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        view.animator.PlayLayerState(t[0], t[1]);
        break;
    }
    case ManagedAnimatorMethod::SET_LAYER_MASK: {
        if (!MatchesServiceArguments(request, arguments, 0u, 0, 2)) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        view.animator.SetLayerMask(t[0], t[1]);
        break;
    }
    case ManagedAnimatorMethod::GET_LAYER_MASK: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 1)) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        output.text.emplace_back(view.animator.GetLayerMask(t[0]));
        break;
    }
    case ManagedAnimatorMethod::PLAY_SLOT: {
        if (request.flags != 0u || p.size() != 3 || t.size() != 3) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        if (integer != 0 && integer != 1) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        if (arguments.relatedId) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        view.animator.PlaySlot(t[0], t[1], t[2], p[0], p[1], p[2], integer != 0);
        break;
    }
    case ManagedAnimatorMethod::STOP_SLOT: {
        if (!MatchesServiceArguments(request, arguments, 0u, 1, 1)) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        view.animator.StopSlot(t[0], p[0]);
        break;
    }
    case ManagedAnimatorMethod::IS_SLOT_PLAYING: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 1)) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        output.integer = static_cast<int64_t>(view.animator.IsSlotPlaying(t[0]));
        break;
    }
    case ManagedAnimatorMethod::GET_SLOT_WEIGHT: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 1)) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        output.values = {view.animator.GetSlotWeight(t[0])};
        break;
    }
    case ManagedAnimatorMethod::SET_SLOT_SPEED: {
        if (!MatchesServiceArguments(request, arguments, 0u, 1, 1)) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        view.animator.SetSlotSpeed(t[0], p[0]);
        break;
    }
    case ManagedAnimatorMethod::GET_SLOT_TIME: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 1)) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        output.values = {view.animator.GetSlotTime(t[0])};
        break;
    }
    case ManagedAnimatorMethod::SET_STATE_SPEED: {
        if (!MatchesServiceArguments(request, arguments, 0u, 1, 1)) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        view.animator.SetStateSpeed(t[0], p[0]);
        break;
    }
    case ManagedAnimatorMethod::GET_STATE_SPEED: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 1)) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        output.values = {view.animator.GetStateSpeed(t[0])};
        break;
    }
    case ManagedAnimatorMethod::SET_MORPH_WEIGHT: {
        if (!MatchesServiceArguments(request, arguments, 0u, 1, 1)) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        view.animator.SetMorphWeight(t[0], p[0]);
        break;
    }
    case ManagedAnimatorMethod::GET_MORPH_WEIGHT: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 1)) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        output.values = {view.animator.GetMorphWeight(t[0])};
        break;
    }
    case ManagedAnimatorMethod::SET_ROOT_MOTION_MODE: {
        if (request.flags != 0u || p.size() != 0 || t.size() != 0) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        if (!FitsInt(integer)) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        if (arguments.relatedId) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        view.animator.SetRootMotionMode(static_cast<int>(integer));
        break;
    }
    case ManagedAnimatorMethod::SET_ROOT_MOTION_POSITION_SCALE: {
        if (!MatchesServiceArguments(request, arguments, 0u, 1, 0)) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        view.animator.SetRootMotionPositionScale(p[0]);
        break;
    }
    case ManagedAnimatorMethod::SET_ROOT_MOTION_ROTATION_SCALE: {
        if (!MatchesServiceArguments(request, arguments, 0u, 1, 0)) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        view.animator.SetRootMotionRotationScale(p[0]);
        break;
    }
    case ManagedAnimatorMethod::SET_ROOT_MOTION_NODE_NAME: {
        if (!MatchesServiceArguments(request, arguments, 0u, 0, 1)) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        view.animator.SetRootMotionNodeName(t[0]);
        break;
    }
    case ManagedAnimatorMethod::SET_UPDATE_LOD: {
        if (request.flags != 0u || p.size() != 4 || !t.empty() || arguments.relatedId
            || (integer != 0 && integer != 1)) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        if (!target.GetComponent<AnimatorComponent>()->SetUpdateLod(integer != 0, p[0], p[1], p[2], p[3])) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        break;
    }
    default: return ManagedBridgeStatus::UNSUPPORTED;
    }
    return ManagedBridgeStatus::OK;
}

ManagedBridgeStatus RunManagedNavigationService(ManagedPhysicsBridge& physics, Script& view, GameObject& target,
    GameObject* related, const ManagedBridgeRequest& request, const ManagedServiceArguments& arguments,
    ManagedServiceResult& output, bool fixedUpdate)
{
    const auto& p = arguments.values;
    const auto& t = arguments.text;
    const int64_t integer = arguments.integer;
    if (!target.GetComponent<NavMeshAgentComponent>()) {
        return ManagedBridgeStatus::NOT_FOUND;
    }
    switch (request.index) {
    case 0: {
        if (!MatchesServiceArguments(request, arguments, 0u, 3, 0)) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        view.navigation.SetDestination(math::Vector3{p[0], p[1], p[2]});
        break;
    }
    case 1: {
        if (!MatchesServiceArguments(request, arguments, 0u, 0, 0)) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        view.navigation.CancelPath();
        break;
    }
    case 2: {
        if (!MatchesServiceArguments(request, arguments, 0u, 0, 0)) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        view.navigation.Pause();
        break;
    }
    case 3: {
        if (!MatchesServiceArguments(request, arguments, 0u, 0, 0)) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        view.navigation.Resume();
        break;
    }
    case 4: {
        if (!MatchesServiceArguments(request, arguments, 0u, 3, 0)) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        const auto status = physics.MoveWorld(target, {p[0], p[1], p[2]}, fixedUpdate);
        if (status != ManagedBridgeStatus::OK) {
            return status;
        }
        target.GetComponent<NavMeshAgentComponent>()->Stop();
        break;
    }
    case 5: {
        if (request.flags != 0u || p.size() != 1 || t.size() != 0) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        if (integer) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        if (!related || !arguments.relatedId) {
            return ManagedBridgeStatus::NOT_FOUND;
        }
        view.navigation.SetTarget(*related, p[0]);
        break;
    }
    case 6: {
        if (!MatchesServiceArguments(request, arguments, 0u, 0, 0)) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        view.navigation.ClearTarget();
        break;
    }
    case 7: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 0)) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        output.integer = static_cast<int64_t>(view.navigation.IsFollowingTarget());
        break;
    }
    case 8: {
        if (!MatchesServiceArguments(request, arguments, 0u, 0, 0)) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        view.navigation.BakeNavMesh();
        break;
    }
    case 9: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 0)) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        output.integer = static_cast<int64_t>(view.navigation.IsPaused());
        break;
    }
    case 10: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 0)) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        output.integer = static_cast<int64_t>(view.navigation.IsMoving());
        break;
    }
    case 11: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 0)) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        output.integer = static_cast<int64_t>(view.navigation.HasPath());
        break;
    }
    case 12: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 0)) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        output.integer = static_cast<int64_t>(view.navigation.HasArrived());
        break;
    }
    case 13: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 0)) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        output.integer = static_cast<int64_t>(view.navigation.IsStuck());
        break;
    }
    case 14: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 0)) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        output.values = {view.navigation.GetRemainingDistance()};
        break;
    }
    case 15: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 0)) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        const auto value = view.navigation.GetVelocity(); output.values = {value.x, value.y, value.z};
        break;
    }
    case 16: {
        if (!MatchesServiceArguments(request, arguments, 0u, 1, 0)) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        view.navigation.SetSpeed(p[0]);
        break;
    }
    case 17: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 0)) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        output.values = {view.navigation.GetSpeed()};
        break;
    }
    case 18: {
        if (!MatchesServiceArguments(request, arguments, 0u, 1, 0)) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        view.navigation.SetAngularSpeed(p[0]);
        break;
    }
    case 19: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 0)) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        output.values = {view.navigation.GetAngularSpeed()};
        break;
    }
    case 20: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 0)) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        const auto value = view.navigation.GetDestination(); output.values = {value.x, value.y, value.z};
        break;
    }
    case 21: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 0)) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        output.values = {view.navigation.GetStoppingDistance()};
        break;
    }
    case 22: {
        if (!MatchesServiceArguments(request, arguments, 0u, 1, 0)) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        view.navigation.SetStoppingDistance(p[0]);
        break;
    }
    case 23: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 0)) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        output.integer = static_cast<int64_t>(view.navigation.GetAreaMask());
        break;
    }
    case 24: {
        if (request.flags != 0u || p.size() != 0 || t.size() != 0) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        if (!FitsInt(integer)) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        if (arguments.relatedId) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        view.navigation.SetAreaMask(static_cast<int>(integer));
        break;
    }
    case 25: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 0)) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        output.integer = static_cast<int64_t>(view.navigation.CanSeeTarget());
        break;
    }
    case 26: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 0)) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        output.object = view.navigation.GetDetectedTarget();
        break;
    }
    default: return ManagedBridgeStatus::UNSUPPORTED;
    }
    return ManagedBridgeStatus::OK;
}

}
