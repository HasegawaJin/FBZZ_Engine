/// @file    ScriptJointProxies.cpp
/// @brief   Joint 領域の Script 転送と関連する内部ヘルパー。
/// @author  Hasegawa Jin
/// @date    2026-10-31
#include "ScriptProxyCommon.hpp"
#include "ScriptPhysicsProxyHelpers.hpp"
#include <Engine/Scene/Script.hpp>
#include <Engine/Input/Input.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Components/JointComponent.hpp>
#include <Engine/Scene/EntityRef.hpp>
#include <Physics/RigidBody.hpp>
#include <limits>
#include <type_traits>
#include <string_view>
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cmath>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <Engine/Scene/Systems/JointSync.hpp>
namespace fbzz::scene {
using namespace script_proxy_detail;
namespace {


JointComponent* EnsureJointComponent(const Script* script)
{
    if (script == nullptr) return nullptr;
    if (auto* joint = SelfComponent<JointComponent>(script)) return joint;
    GameObject* self = script->scene.Self();
    if (self == nullptr || !self->IsValid()) return nullptr;
    return &self->AddComponent<JointComponent>();
}

/// @brief Connect*() 共通の下ごしらえ。種別と相手を差し替え、残りは呼び出し側が詰める。
/// @brief target が null / 無効なら関節は作らず nullptr を返す («黙って自分だけ» にしない)。

JointComponent* BeginJointConnect(const Script* script, JointType type, GameObject* target)
{
    if (!target || !target->IsValid()) return nullptr;
    JointComponent* joint = EnsureJointComponent(script);
    if (!joint) return nullptr;
    joint->enabled         = true;
    joint->type            = type;
    joint->connectedBody.id = target->GetID();
    /// @note 相手を名指しした以上、祖先へのフォールバックは切る。
    /// @note 残したままだと、相手が消えた瞬間に «別のものへ勝手に繋ぎ変わる»。
    joint->connectToParent = false;
    joint->chainBodies.clear();
    return joint;
}

/// @brief 距離は «0 以下なら張った瞬間の間隔» という 1 つの規約で通す。

void SetAuthoredJointDistance(JointComponent& joint, float distance)
{
    joint.autoDistance = distance <= 0.0f;
    joint.distance     = joint.autoDistance ? 0.0f : distance;
}


} /// @note namespace
void ScriptJointProxy::ConnectFixed(GameObject* target) const
{
    BeginJointConnect(script, JointType::Fixed, target);
}


void ScriptJointProxy::ConnectDistance(GameObject* target, float distance) const
{
    if (auto* joint = BeginJointConnect(script, JointType::Distance, target))
        SetAuthoredJointDistance(*joint, distance);
}


void ScriptJointProxy::ConnectRope(GameObject* target, float maxLength) const
{
    if (auto* joint = BeginJointConnect(script, JointType::Rope, target))
        SetAuthoredJointDistance(*joint, maxLength);
}


void ScriptJointProxy::ConnectSpring(GameObject* target,
                                     float restLength,
                                     float stiffness,
                                     float damping) const
{
    auto* joint = BeginJointConnect(script, JointType::Spring, target);
    if (!joint) return;
    SetAuthoredJointDistance(*joint, restLength);
    joint->spring  = stiffness < 0.0f ? 0.0f : stiffness;
    joint->damping = damping < 0.0f ? 0.0f : damping;
}


void ScriptJointProxy::ConnectHinge(GameObject* target,
                                    const math::Vector3& axis,
                                    const math::Vector3& anchor,
                                    const math::Vector3& connectedAnchor) const
{
    auto* joint = BeginJointConnect(script, JointType::Hinge, target);
    if (!joint) return;
    joint->axis            = axis;
    joint->anchor          = anchor;
    joint->connectedAnchor = connectedAnchor;
}


void ScriptJointProxy::ConnectSlider(GameObject* target, const math::Vector3& axis) const
{
    if (auto* joint = BeginJointConnect(script, JointType::Slider, target))
        joint->axis = axis;
}


void ScriptJointProxy::ConnectChain(const std::vector<GameObject*>& following,
                                    float segmentLength,
                                    int iterations) const
{
    JointComponent* joint = EnsureJointComponent(script);
    if (!joint) return;
    joint->enabled = true;
    joint->type    = JointType::Chain;
    joint->connectedBody.id = EntityID::INVALID;
    joint->chainBodies.clear();
    joint->chainBodies.reserve(following.size());
    for (GameObject* go : following) {
        /// @note 1 つでも欠けると «どこから先が繋がっていないのか» が分からなくなる。
        /// @note 並びの穴を許さず、丸ごと張らないことで気付ける形にする。
        if (!go || !go->IsValid()) { joint->chainBodies.clear(); return; }
        joint->chainBodies.push_back(EntityRef{ go->GetID() });
    }
    SetAuthoredJointDistance(*joint, segmentLength);
    joint->solverIterations = iterations < 1 ? 1 : iterations;
}


void ScriptJointProxy::Break() const
{
    if (!script || !script->m_gameObject) return;
    if (script->m_gameObject->GetComponent<JointComponent>())
        script->m_gameObject->RemoveComponent<JointComponent>();
}


bool ScriptJointProxy::HasJoint() const
{
    return SelfComponent<JointComponent>(script) != nullptr;
}


bool ScriptJointProxy::IsConnected() const
{
    const auto* joint = SelfComponent<JointComponent>(script);
    return joint && joint->connected;
}


void ScriptJointProxy::SetEnabled(bool enabled) const
{
    if (auto* joint = SelfComponent<JointComponent>(script)) joint->enabled = enabled;
}


bool ScriptJointProxy::IsEnabled() const
{
    const auto* joint = SelfComponent<JointComponent>(script);
    return joint && joint->enabled;
}


void ScriptJointProxy::SetLimits(float lower, float upper) const
{
    auto* joint = SelfComponent<JointComponent>(script);
    if (!joint) return;
    joint->useLimits  = true;
    /// @note 逆に渡されても入れ替えて受ける。Physics 側の SetLimits も min/max を並べ替えるが、
    /// @note Inspector に «下限 > 上限» のまま残ると «可動域が効かない» に見える。
    joint->lowerLimit = std::min(lower, upper);
    joint->upperLimit = std::max(lower, upper);
}


void ScriptJointProxy::ClearLimits() const
{
    if (auto* joint = SelfComponent<JointComponent>(script)) joint->useLimits = false;
}


void ScriptJointProxy::SetMotor(float targetSpeed, float maxTorque) const
{
    auto* joint = SelfComponent<JointComponent>(script);
    if (!joint) return;
    joint->useMotor       = true;
    joint->motorSpeed     = targetSpeed;
    joint->motorMaxTorque = maxTorque < 0.0f ? 0.0f : maxTorque;
}


void ScriptJointProxy::ClearMotor() const
{
    if (auto* joint = SelfComponent<JointComponent>(script)) joint->useMotor = false;
}


void ScriptJointProxy::SetDistance(float distance) const
{
    if (auto* joint = SelfComponent<JointComponent>(script))
        SetAuthoredJointDistance(*joint, distance);
}


float ScriptJointProxy::GetDistance() const
{
    const auto* joint = SelfComponent<JointComponent>(script);
    return joint ? joint->resolvedDistance : 0.0f;
}


float ScriptJointProxy::GetCurrentDistance() const
{
    auto* joint = SelfComponent<JointComponent>(script);
    if (!joint || !script || !script->m_gameObject || !script->m_scene) return 0.0f;
    const physics::RigidBody* self = SelfRigidBody(script);
    const physics::RigidBody* other =
        ResolveJointPartner(*script->m_scene, *script->m_gameObject, *joint);
    if (!self || !other) return 0.0f;
    return (other->GetPosition() - self->GetPosition()).Length();
}


} /// @note namespace fbzz::scene
