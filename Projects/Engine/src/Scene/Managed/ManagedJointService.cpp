/// @file    ManagedJointService.cpp
/// @brief   Joint の入力を全検証し、既存 row と World 宣言を一度だけ変更する。
/// @author  Hasegawa Jin
/// @date    2026-10-06
#include "ManagedJointService.hpp"
#include "ManagedServicesBridge.hpp"
#include "ManagedObjectControls.hpp"
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Engine/Scene/Components/JointComponent.hpp>
#include <Engine/Scene/Components/RigidBodyComponent.hpp>
#include <cmath>
#include <unordered_set>
namespace fbzz::scene::managed_detail {
namespace {
bool Body(Scene& scene, GameObject& object) {
    return object.IsValid() && object.GetScene() == &scene && scene.GetGameObject(object.GetID()) == &object
        && object.GetComponent<RigidBodyComponent>() && !ManagedObjectControls::IsPendingComponentRemoval(object, 1);
}
void Distance(JointComponent& row, float value) { row.autoDistance = value <= 0; row.distance = row.autoDistance ? 0 : value; }
}
bool PrepareJointChain(Scene& scene, GameObject& owner, std::span<GameObject* const> targets,
    float distance, uint32_t iterations, JointComponent& candidate)
{
    if (!Body(scene, owner) || targets.empty() || !std::isfinite(distance) || !iterations || iterations > INT32_MAX) return false;
    std::unordered_set<GameObject*> seen{&owner}; std::vector<EntityRef> references; references.reserve(targets.size());
    for (GameObject* target : targets) {
        if (!target || !Body(scene, *target) || !seen.insert(target).second) return false;
        references.push_back({target->GetID()});
    }
    candidate.type = JointType::Chain; candidate.enabled = true; candidate.connectedBody.id = EntityID::INVALID;
    candidate.connectToParent = false; candidate.chainBodies = std::move(references); candidate.solverIterations = static_cast<int>(iterations);
    Distance(candidate, distance); candidate.connected = false; return true;
}
ManagedBridgeStatus RunManagedJointService(Scene& scene, Script& proxy, GameObject& owner, GameObject* related,
    const ManagedBridgeRequest& request, std::span<const float> values, std::span<const std::string> text, ManagedServiceResult& result, bool fixed)
{
    const bool read = request.flags == 1;
    const bool expectedRead = request.index == 0 || request.index == 1 || (request.index >= 3 && request.index <= 6);
    const bool connect = request.index >= 7 && request.index <= 12;
    const uint32_t needed = request.index == 2 || request.index == 8 || request.index == 9 || request.index == 18 ? 1U
        : request.index == 10 || request.index == 12 ? 3U : request.index == 11 ? 9U : request.index == 14 || request.index == 16 ? 2U : 0U;
    if (request.flags > 1 || request.index > 18 || read != expectedRead || !text.empty() || values.size() != needed
        || (connect ? !related : related != nullptr)) return ManagedBridgeStatus::INVALID_ARGUMENT;
    if (!read && !fixed) return ManagedBridgeStatus::INVALID_SCOPE;
    auto* row = owner.GetComponent<JointComponent>();
    if (!row || ManagedObjectControls::IsPendingComponentRemoval(owner, static_cast<uint32_t>(ManagedNativeComponentKind::JOINT))) return ManagedBridgeStatus::NOT_FOUND;
    ManagedServiceResult output;
    if (read) {
        if (request.index == 0) output.integer = row->connected;
        else if (request.index == 1) output.integer = row->enabled;
        else if (request.index == 3) output.integer = static_cast<int>(row->type);
        else if (request.index == 4) output.object = scene.GetGameObject(row->connectedBody.id);
        else if (request.index == 5) output.values = {proxy.joint.GetDistance()};
        else output.values = {proxy.joint.GetCurrentDistance()};
    } else {
        for (float value : values) if (!std::isfinite(value)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        if (connect && (!Body(scene, owner) || !Body(scene, *related) || related == &owner)) { output.integer = 0; result = std::move(output); return ManagedBridgeStatus::OK; }
        JointComponent candidate = *row;
        if (connect) {
            candidate.enabled = true; candidate.type = static_cast<JointType>(request.index - 7); candidate.connectedBody.id = related->GetID();
            candidate.connectToParent = false; candidate.chainBodies.clear(); candidate.connected = false;
            if (request.index >= 8 && request.index <= 10) Distance(candidate, values[0]);
            if (request.index == 10) { if (values[1] < 0 || values[2] < 0) return ManagedBridgeStatus::INVALID_ARGUMENT; candidate.spring = values[1]; candidate.damping = values[2]; }
            if (request.index == 11 || request.index == 12) {
                const math::Vector3 axis{values[0], values[1], values[2]}; if (!std::isfinite(axis.LengthSq()) || axis.LengthSq() <= 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                candidate.axis = axis;
                if (request.index == 11) { candidate.anchor = {values[3], values[4], values[5]}; candidate.connectedAnchor = {values[6], values[7], values[8]}; }
            }
            output.integer = 1;
        } else if (request.index == 2) { if (values[0] != 0 && values[0] != 1) return ManagedBridgeStatus::INVALID_ARGUMENT; candidate.enabled = values[0] != 0; }
        else if (request.index == 13) { row->enabled = false; bool exists = false; return ManagedObjectControls::Component(owner, static_cast<uint32_t>(ManagedNativeComponentKind::JOINT), ManagedBridgeOp::COMPONENT_REMOVE, exists); }
        else if (request.index == 14) { candidate.useLimits = true; candidate.lowerLimit = std::min(values[0], values[1]); candidate.upperLimit = std::max(values[0], values[1]); }
        else if (request.index == 15) candidate.useLimits = false;
        else if (request.index == 16) { if (values[1] < 0) return ManagedBridgeStatus::INVALID_ARGUMENT; candidate.useMotor = true; candidate.motorSpeed = values[0]; candidate.motorMaxTorque = values[1]; }
        else if (request.index == 17) candidate.useMotor = false;
        else if (request.index == 18) Distance(candidate, values[0]);
        *row = std::move(candidate);
    }
    result = std::move(output); return ManagedBridgeStatus::OK;
}
} /// @note namespace fbzz::scene::managed_detail
