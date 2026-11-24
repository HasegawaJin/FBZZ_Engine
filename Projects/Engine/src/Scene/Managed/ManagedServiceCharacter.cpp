/// @file    ManagedServiceCharacter.cpp
/// @brief   キャラクター操作の位相制約と書き換え前のコピー検証。
/// @author  Hasegawa Jin
/// @date    2026-11-21
#include "ManagedServiceRoutes.hpp"

#include <Engine/Scene/Components/CharacterControllerComponent.hpp>
#include <Engine/Scene/Components/RigidBodyComponent.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <cmath>
#include <iterator>

namespace fbzz::scene::managed_detail {

namespace {

bool FiniteVector(const math::Vector3& value)
{ return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z); }

}

ManagedBridgeStatus RunManagedCharacterService(GameObject& target, const ManagedBridgeRequest& request, const ManagedServiceArguments& arguments,
    ManagedServiceResult& output, bool fixedUpdate, bool collisionCallback)
{
    auto* controller = target.GetComponent<CharacterControllerComponent>();
    if (!controller) return ManagedBridgeStatus::NOT_FOUND;
    const uint32_t method = request.index;
    constexpr size_t counts[] = {1, 3, 1, 3, 3, 6, 0, 0, 0, 0, 0, 3, 0, 0, 0, 0, 3};
    if (method >= std::size(counts)) return ManagedBridgeStatus::UNSUPPORTED;
    const bool readOnly = (method >= 6 && method <= 10) || method == 15;
    if (request.flags != (readOnly ? 1u : 0u) || arguments.values.size() != counts[method] || !arguments.text.empty()
        || arguments.relatedId || (method == 14 ? arguments.integer != 0 && arguments.integer != 1 : arguments.integer != 0))
        return ManagedBridgeStatus::INVALID_ARGUMENT;
    if (!readOnly && !fixedUpdate && !(method == 16 && collisionCallback)) return ManagedBridgeStatus::INVALID_SCOPE;
    auto* component = target.GetComponent<RigidBodyComponent>();
    physics::RigidBody* body = component && component->enabled ? component->rigidBody.get() : nullptr;
    if (readOnly) {
        if (method == 6 || method == 7 || method == 10) {
            const auto value = method == 6 ? controller->GetVelocity(body) : method == 7 ? controller->GetHorizontalVelocity(body) : controller->groundNormal;
            output.values = {value.x, value.y, value.z};
        } else if (method == 8) output.integer = controller->isGrounded ? 1 : 0;
        else if (method == 9) output.values = {controller->verticalSpeed};
        else output.integer = controller->enabled ? 1 : 0;
        return ManagedBridgeStatus::OK;
    }
    if (method <= 5 && !body) return ManagedBridgeStatus::NOT_FOUND;
    if (!controller->IsFiniteState() || (body && !body->IsFiniteState())) return ManagedBridgeStatus::INVALID_ARGUMENT;
    CharacterControllerComponent candidate = *controller;
    physics::RigidBody bodyCandidate = body ? *body : physics::RigidBody{};
    physics::RigidBody* bodyView = body ? &bodyCandidate : nullptr;
    const auto& p = arguments.values;
    math::Vector3 normal = math::Vector3::ZERO;
    switch (method) {
    case 0:
        if (p[0] < 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
        candidate.Tick(bodyView, p[0]); break;
    case 1: candidate.Jump(bodyView, {p[0], p[1], p[2]}); break;
    case 2:
        if (!std::isfinite((p[0] - bodyCandidate.GetVelocity().y) * bodyCandidate.GetMass())) return ManagedBridgeStatus::INVALID_ARGUMENT;
        candidate.JumpAtVelocity(bodyView, p[0]); break;
    case 3: candidate.SetHorizontalVelocity(bodyView, {p[0], p[1], p[2]}); break;
    case 4: candidate.AddHorizontalVelocity(bodyView, {p[0], p[1], p[2]}); break;
    case 5:
        if (p[3] < 0 || !std::isfinite(math::Vector3{p[0], p[1], p[2]}.LengthSq())
            || (p[4] >= 0 && !std::isfinite(p[4] * p[3])) || (p[5] >= 0 && !std::isfinite(p[5] * p[3]))) return ManagedBridgeStatus::INVALID_ARGUMENT;
        candidate.Move(bodyView, {p[0], p[1], p[2]}, p[3], p[4], p[5]); break;
    case 11:
        normal = {p[0], p[1], p[2]};
        if (!std::isfinite(normal.LengthSq())) return ManagedBridgeStatus::INVALID_ARGUMENT;
        candidate.ForceGrounded(normal); break;
    case 12: candidate.ForceAirborne(); break;
    case 13: candidate.UseAutomaticGrounding(); break;
    case 14: candidate.enabled = arguments.integer == 1; break;
    case 16: {
        normal = {p[0], p[1], p[2]};
        if (!std::isfinite(normal.LengthSq())) return ManagedBridgeStatus::INVALID_ARGUMENT;
        CollisionInfo contact; contact.contactNormal = normal;
        candidate.RegisterGroundContact(contact);
        /// @note RegisterGroundContact は実 body を GameObject から引くため、コピー検証では最大の速度射影を先に検証する。
        if (body && normal.LengthSq() > math::EPSILON) {
            const math::Vector3 unit = normal.Normalized();
            const float dot = math::Vector3::Dot(bodyCandidate.GetVelocity(), unit);
            if (!std::isfinite(dot) || !FiniteVector(bodyCandidate.GetVelocity() - unit * dot)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        break;
    }
    default: return ManagedBridgeStatus::UNSUPPORTED;
    }
    if (!candidate.IsFiniteState() || (body && !bodyCandidate.IsFiniteState())) return ManagedBridgeStatus::INVALID_ARGUMENT;
    if (method == 16) {
        CollisionInfo contact; contact.self = &target; contact.contactNormal = normal;
        controller->RegisterGroundContact(contact);
    } else {
        *controller = candidate;
        if (body) *body = bodyCandidate;
    }
    return ManagedBridgeStatus::OK;
}

}
