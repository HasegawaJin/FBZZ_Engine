/// @file    ScriptPhysicsProxies.cpp
/// @brief   Physics 領域の Script 転送と関連する内部ヘルパー。
/// @author  Hasegawa Jin
/// @date    2026-10-31
#include "ScriptProxyCommon.hpp"
#include "ScriptPhysicsProxyHelpers.hpp"
#include <Engine/Scene/Script.hpp>
#include <Engine/Input/Input.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Components/ColliderComponent.hpp>
#include <Engine/Scene/Components/RigidBodyComponent.hpp>
#include <Engine/Scene/Components/CharacterControllerComponent.hpp>
#include <Physics/RigidBody.hpp>
#include <Physics/World.hpp>
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
#include <Engine/Scene/Systems/ColliderSync.hpp>
#include <Engine/Core/Logger.hpp>
namespace fbzz::scene {
using namespace script_proxy_detail;
namespace {
physics::RigidBody* ObjectRigidBody(GameObject* go)
{
    auto* component = ObjectComponent<RigidBodyComponent>(go);
    return component && component->enabled ? component->rigidBody.get() : nullptr;
}


GameObject* FindGameObjectByCollider(Scene* scene, const physics::Collider* collider)
{
    if (!scene || !collider) return nullptr;

    auto matches = [collider](const ColliderComponent* component) {
        return component && component->collider.get() == collider;
    };

    for (auto& go : scene->GameObjects()) {
        if (matches(go.GetComponent<AabbColliderComponent>())) return &go;
        if (matches(go.GetComponent<BoxColliderComponent>())) return &go;
        if (matches(go.GetComponent<SphereColliderComponent>())) return &go;
        if (matches(go.GetComponent<CapsuleColliderComponent>())) return &go;
        if (matches(go.GetComponent<CylinderColliderComponent>())) return &go;
        if (matches(go.GetComponent<MeshColliderComponent>())) return &go;
        if (matches(go.GetComponent<ConvexHullColliderComponent>())) return &go;
    }
    return nullptr;
}


RaycastHit ToScriptHit(Scene* scene, const physics::World::RaycastHit& worldHit)
{
    RaycastHit hit{};
    hit.gameObject = FindGameObjectByCollider(scene, worldHit.collider);
    hit.collider = worldHit.collider;
    hit.point = worldHit.point;
    hit.normal = worldHit.normal;
    hit.distance = worldHit.distance;
    return hit;
}



ColliderComponent* SelfAnyCollider(const Script* script)
{
    if (!script) return nullptr;
    if (auto* c = script->GetComponent<BoxColliderComponent>()) return c;
    if (auto* c = script->GetComponent<AabbColliderComponent>()) return c;
    if (auto* c = script->GetComponent<SphereColliderComponent>()) return c;
    if (auto* c = script->GetComponent<CapsuleColliderComponent>()) return c;
    if (auto* c = script->GetComponent<CylinderColliderComponent>()) return c;
    if (auto* c = script->GetComponent<MeshColliderComponent>()) return c;
    if (auto* c = script->GetComponent<ConvexHullColliderComponent>()) return c;
    if (auto* c = script->GetComponent<TerrainColliderComponent>()) return c;
    return nullptr;
}


CharacterControllerComponent* SelfCharacter(const Script* script)
{
    return SelfComponent<CharacterControllerComponent>(script);
}

} /// @note namespace
void ScriptPhysicsProxy::AddForce(const math::Vector3& v) const
{
    if (auto* rb = SelfRigidBody(script)) rb->ApplyForce(v);
}


void ScriptPhysicsProxy::AddImpulse(const math::Vector3& v) const
{
    if (auto* rb = SelfRigidBody(script)) rb->ApplyImpulse(v);
}


void ScriptPhysicsProxy::AddForceAtPoint(const math::Vector3& force, const math::Vector3& worldPoint) const
{
    if (auto* rb = SelfRigidBody(script)) rb->ApplyForceAtPoint(force, worldPoint);
}


bool ScriptPhysicsProxy::HasRigidBody() const
{
    return SelfRigidBody(script) != nullptr;
}


bool ScriptPhysicsProxy::HasRigidBody(GameObject* go) const
{
    return ObjectRigidBody(go) != nullptr;
}


float ScriptPhysicsProxy::GetMass() const
{
    if (auto* rb = SelfRigidBody(script)) return rb->GetMass();
    return 0.0f;
}


void ScriptPhysicsProxy::SetMass(float mass) const
{
    if (auto* rb = SelfRigidBody(script)) rb->SetMass(mass);
}


void ScriptPhysicsProxy::SetStatic(bool isStatic) const
{
    if (auto* rb = SelfRigidBody(script)) {
        rb->m_isStatic = isStatic;
        rb->SetMass(rb->GetMass());
    }
}


bool ScriptPhysicsProxy::IsStatic() const
{
    const auto* rb = SelfRigidBody(script);
    return rb && rb->IsStatic();
}


bool ScriptPhysicsProxy::IsSleeping() const
{
    const auto* rb = SelfRigidBody(script);
    return rb && rb->IsSleeping();
}


float ScriptPhysicsProxy::GetGravityScale() const
{
    if (auto* rb = SelfRigidBody(script)) return rb->m_gravityScale;
    return 1.0f;
}


void ScriptPhysicsProxy::SetGravityScale(float scale) const
{
    if (auto* rb = SelfRigidBody(script)) rb->m_gravityScale = scale;
}


void ScriptPhysicsProxy::SetLocalTimeScale(float scale) const
{
    if (auto* rb = SelfRigidBody(script)) rb->m_timeScale = std::clamp(scale, 0.0f, 8.0f);
}


void ScriptPhysicsProxy::SetFlowCoupling(float coupling) const
{
    auto* component = SelfComponent<RigidBodyComponent>(script);
    if (!component) return;
    /// @note 正本はコンポーネント側 (PhysicsSystem が毎フレーム剛体へ押し込むため、剛体だけに書くと
    /// @note 次の固定ステップで巻き戻る)。剛体にも写さないと効果が 1 ステップ遅れて出る。
    component->flowCoupling = coupling;
    if (component->rigidBody) component->rigidBody->SetFlowCoupling(coupling);
}


float ScriptPhysicsProxy::GetFlowCoupling() const
{
    const auto* component = SelfComponent<RigidBodyComponent>(script);
    return component ? component->flowCoupling : 0.0f;
}


math::Vector3 ScriptPhysicsProxy::GetWorldGravity() const
{
    if (!Script::s_physicsWorld) return { 0.0f, -9.81f, 0.0f };
    return Script::s_physicsWorld->GetGravity();
}


bool ScriptPhysicsProxy::LayersCollide(int a, int b) const
{
    /// @note World が無い (編集中) ときは «ぶつかる» を返す。無いことを «切れている» と
    /// @note 読ませると、Play していない間だけ組み立てが安全側へ倒れて、
    /// @note エディタで見ている当たりと Play 中の当たりが別物になる。
    if (!Script::s_physicsWorld) return true;
    return Script::s_physicsWorld->LayersCollide(a, b);
}


void ScriptPhysicsProxy::SetVelocity(const math::Vector3& v) const
{
    if (auto* rb = SelfRigidBody(script)) rb->SetVelocity(v);
}


math::Vector3 ScriptPhysicsProxy::GetVelocity() const
{
    if (auto* rb = SelfRigidBody(script)) return rb->GetVelocity();
    return math::Vector3::ZERO;
}


void ScriptPhysicsProxy::SetVelocity(GameObject* go, const math::Vector3& v) const
{
    if (auto* rb = ObjectRigidBody(go)) rb->SetVelocity(v);
}


math::Vector3 ScriptPhysicsProxy::GetVelocity(GameObject* go) const
{
    if (auto* rb = ObjectRigidBody(go)) return rb->GetVelocity();
    return math::Vector3::ZERO;
}


void ScriptPhysicsProxy::AddImpulse(GameObject* go, const math::Vector3& v) const
{
    if (auto* rb = ObjectRigidBody(go)) rb->ApplyImpulse(v);
}


float ScriptPhysicsProxy::GetMass(GameObject* go) const
{
    if (auto* rb = ObjectRigidBody(go)) return rb->GetMass();
    return 0.0f;
}


void ScriptPhysicsProxy::SetAngularVelocity(const math::Vector3& v) const
{
    if (auto* rb = SelfRigidBody(script)) rb->SetAngularVelocity(v);
}


math::Vector3 ScriptPhysicsProxy::GetAngularVelocity() const
{
    if (auto* rb = SelfRigidBody(script)) return rb->GetAngularVelocity();
    return math::Vector3::ZERO;
}


void ScriptPhysicsProxy::AddTorque(const math::Vector3& v) const
{
    if (auto* rb = SelfRigidBody(script)) rb->ApplyTorque(v);
}


void ScriptPhysicsProxy::SetFreezePosition(bool x, bool y, bool z) const
{
    if (auto* rb = SelfRigidBody(script)) rb->SetFreezePosition({ x, y, z });
}


void ScriptPhysicsProxy::SetFreezeRotation(bool x, bool y, bool z) const
{
    if (auto* rb = SelfRigidBody(script)) rb->SetFreezeRotation({ x, y, z });
}


bool ScriptPhysicsProxy::Raycast(const math::Vector3& origin, const math::Vector3& dir, float dist, RaycastHit& hit) const
{
    hit = {};
    if (!script || !Script::s_physicsWorld) return false;

    physics::World::RaycastHit worldHit{};
    if (!Script::s_physicsWorld->Raycast(origin, dir, dist, worldHit))
        return false;

    hit = ToScriptHit(script->m_scene, worldHit);
    return true;
}


std::vector<RaycastHit> ScriptPhysicsProxy::RaycastAll(const math::Vector3& origin, const math::Vector3& dir, float dist) const
{
    std::vector<RaycastHit> hits;
    if (!script || !Script::s_physicsWorld) return hits;

    const auto worldHits = Script::s_physicsWorld->RaycastAll(origin, dir, dist);
    hits.reserve(worldHits.size());
    for (const auto& worldHit : worldHits)
        hits.push_back(ToScriptHit(script->m_scene, worldHit));
    return hits;
}


bool ScriptPhysicsProxy::SphereCast(const math::Vector3& center, float radius, const math::Vector3& dir, float dist, RaycastHit& hit) const
{
    hit = {};
    if (!script || !Script::s_physicsWorld) return false;

    physics::World::RaycastHit worldHit{};
    if (!Script::s_physicsWorld->SphereCast(center, radius, dir, dist, worldHit))
        return false;

    hit = ToScriptHit(script->m_scene, worldHit);
    return true;
}


std::vector<GameObject*> ScriptPhysicsProxy::OverlapSphere(const math::Vector3& center, float radius) const
{
    std::vector<GameObject*> result;
    if (!script || !Script::s_physicsWorld) return result;

    const auto overlaps = Script::s_physicsWorld->OverlapSphere(center, radius);
    result.reserve(overlaps.size());
    for (const auto* inst : overlaps) {
        auto* go = inst ? FindGameObjectByCollider(script->m_scene, inst->collider) : nullptr;
        if (go && std::find(result.begin(), result.end(), go) == result.end())
            result.push_back(go);
    }
    return result;
}


void ScriptColliderProxy::SetEnabled(bool enabled) const
{
    if (auto* c = SelfAnyCollider(script)) c->SetEnabled(enabled);
}


void ScriptColliderProxy::SetTrigger(bool trigger) const
{
    if (auto* c = SelfAnyCollider(script)) c->SetTrigger(trigger);
}


void ScriptColliderProxy::SetCenter(const math::Vector3& center) const
{
    if (auto* c = SelfAnyCollider(script)) c->SetCenter(center);
}


bool ScriptColliderProxy::HasCollider() const
{
    return SelfAnyCollider(script) != nullptr;
}


bool ScriptColliderProxy::IsEnabled() const
{
    const auto* c = SelfAnyCollider(script);
    return c && c->enabled;
}


bool ScriptColliderProxy::IsTrigger() const
{
    const auto* c = SelfAnyCollider(script);
    return c && c->isTrigger;
}


math::Vector3 ScriptColliderProxy::GetCenter() const
{
    const auto* c = SelfAnyCollider(script);
    return c ? c->center : math::Vector3::ZERO;
}


bool ScriptColliderProxy::TryGetPrimitiveWorldBounds(GameObject* object,
    math::Vector3& outMin, math::Vector3& outMax)
{
    physics::AABB bounds;
    if (!object || !TryGetPrimitiveColliderBounds(*object, bounds)) return false;
    outMin = bounds.min;
    outMax = bounds.max;
    return true;
}


void ScriptColliderProxy::SetBoxSize(const math::Vector3& size) const
{
    if (!script || !script->m_gameObject) return;
    if (auto* c = script->m_gameObject->GetComponent<BoxColliderComponent>()) c->SetSize(size);
    if (auto* c = script->m_gameObject->GetComponent<AabbColliderComponent>()) c->SetSize(size);
}


void ScriptColliderProxy::SetSphereRadius(float radius) const
{
    if (!script || !script->m_gameObject) return;
    if (auto* c = script->m_gameObject->GetComponent<SphereColliderComponent>()) c->SetRadius(radius);
}


void ScriptColliderProxy::SetCapsule(float radius, float halfHeight) const
{
    if (!script || !script->m_gameObject) return;
    if (auto* c = script->m_gameObject->GetComponent<CapsuleColliderComponent>())
        c->SetCapsule(radius, halfHeight);
}


void ScriptColliderProxy::SetCylinder(float radius, float halfHeight) const
{
    if (!script || !script->m_gameObject) return;
    if (auto* c = script->m_gameObject->GetComponent<CylinderColliderComponent>())
        c->SetCylinder(radius, halfHeight);
}


void ScriptColliderProxy::SetMesh(std::string_view meshPath, int meshIndex) const
{
    if (!script || !script->m_gameObject) return;
    if (auto* c = script->m_gameObject->GetComponent<MeshColliderComponent>())
        c->SetMesh(std::string(meshPath), meshIndex);
    if (auto* c = script->m_gameObject->GetComponent<ConvexHullColliderComponent>())
        c->SetMesh(std::string(meshPath), meshIndex);
}


void ScriptColliderProxy::SetFriction(float staticFriction, float dynamicFriction) const
{
    auto* c = SelfAnyCollider(script);
    if (!c) return;
    /// @note 共有アセット参照を外してから書く。残したままだと次のフレームで
    /// @note ResolvePhysicsMaterial() に上書きされ、書いた値が消える。
    c->physicsMaterialPath.clear();
    c->material.staticFriction = staticFriction;
    c->material.dynamicFriction = dynamicFriction;
}


void ScriptColliderProxy::SetRestitution(float restitution) const
{
    auto* c = SelfAnyCollider(script);
    if (!c) return;
    c->physicsMaterialPath.clear();
    c->material.restitution = restitution;
}


void ScriptColliderProxy::SetDensity(float density) const
{
    auto* c = SelfAnyCollider(script);
    if (!c) return;
    c->physicsMaterialPath.clear();
    c->material.density = density;
}


void ScriptColliderProxy::SetPhysicsMaterial(std::string_view assetPath) const
{
    if (auto* c = SelfAnyCollider(script))
        c->SetPhysicsMaterialPath(std::string(assetPath));
}


std::string ScriptColliderProxy::GetPhysicsMaterial() const
{
    const auto* c = SelfAnyCollider(script);
    return c ? c->physicsMaterialPath : std::string{};
}


float ScriptColliderProxy::GetRestitution() const
{
    const auto* c = SelfAnyCollider(script);
    return c ? c->material.restitution : 0.0f;
}


float ScriptColliderProxy::GetStaticFriction() const
{
    const auto* c = SelfAnyCollider(script);
    return c ? c->material.staticFriction : 0.0f;
}


float ScriptColliderProxy::GetDynamicFriction() const
{
    const auto* c = SelfAnyCollider(script);
    return c ? c->material.dynamicFriction : 0.0f;
}


float ScriptColliderProxy::GetDensity() const
{
    const auto* c = SelfAnyCollider(script);
    return c ? c->material.density : 0.0f;
}


bool ScriptColliderProxy::ApplyPhysicsMaterialPreset(std::string_view presetName) const
{
    auto* c = SelfAnyCollider(script);
    if (!c) return false;
    /// @note string_view は終端 NUL を保証しないため、C API へ渡す前に string 化する。
    const std::string name(presetName);
    const auto* preset = physics::PhysicsMaterial::FindPreset(name.c_str());
    if (!preset) {
        FBZZ_LOG_WARN("Unknown physics material preset: %s", name.c_str());
        return false;
    }
    c->SetMaterial(*preset);
    return true;
}


void ScriptCharacterProxy::Tick(float dt) const
{
    auto* cc = SelfCharacter(script);
    if (!cc) return;
    /// @note RigidBody は SelfRigidBody() が script→Component→rigidBody と辿る。
    cc->Tick(SelfRigidBody(script), dt);
}


void ScriptCharacterProxy::Jump(const math::Vector3& impulse) const
{
    auto* cc = SelfCharacter(script);
    if (!cc) return;
    cc->Jump(SelfRigidBody(script), impulse);
}


void ScriptCharacterProxy::JumpAtVelocity(float verticalSpeed) const
{
    auto* cc = SelfCharacter(script);
    if (cc) cc->JumpAtVelocity(SelfRigidBody(script), verticalSpeed);
}


void ScriptCharacterProxy::SetHorizontalVelocity(const math::Vector3& velocity) const
{
    auto* cc = SelfCharacter(script);
    if (cc) cc->SetHorizontalVelocity(SelfRigidBody(script), velocity);
}


void ScriptCharacterProxy::AddHorizontalVelocity(const math::Vector3& velocity) const
{
    auto* cc = SelfCharacter(script);
    if (cc) cc->AddHorizontalVelocity(SelfRigidBody(script), velocity);
}


void ScriptCharacterProxy::Move(const math::Vector3& desiredVelocity,
                                float dt,
                                float acceleration,
                                float deceleration) const
{
    auto* cc = SelfCharacter(script);
    if (cc) cc->Move(SelfRigidBody(script), desiredVelocity, dt, acceleration, deceleration);
}


math::Vector3 ScriptCharacterProxy::GetVelocity() const
{
    auto* cc = SelfCharacter(script);
    return cc ? cc->GetVelocity(SelfRigidBody(script)) : math::Vector3::ZERO;
}


math::Vector3 ScriptCharacterProxy::GetHorizontalVelocity() const
{
    auto* cc = SelfCharacter(script);
    return cc ? cc->GetHorizontalVelocity(SelfRigidBody(script)) : math::Vector3::ZERO;
}


void ScriptCharacterProxy::RegisterGroundContact(const CollisionInfo& info) const
{
    if (auto* cc = SelfCharacter(script)) cc->RegisterGroundContact(info);
}


bool ScriptCharacterProxy::IsGrounded() const
{
    auto* cc = SelfCharacter(script);
    return cc && cc->isGrounded;
}


float ScriptCharacterProxy::GetVerticalSpeed() const
{
    auto* cc = SelfCharacter(script);
    return cc ? cc->verticalSpeed : 0.0f;
}


math::Vector3 ScriptCharacterProxy::GetGroundNormal() const
{
    auto* cc = SelfCharacter(script);
    return cc ? cc->groundNormal : math::Vector3::UP;
}


void ScriptCharacterProxy::ForceGrounded(const math::Vector3& normal) const
{
    if (auto* cc = SelfCharacter(script)) cc->ForceGrounded(normal);
}


void ScriptCharacterProxy::ForceAirborne() const
{
    if (auto* cc = SelfCharacter(script)) cc->ForceAirborne();
}


void ScriptCharacterProxy::UseAutomaticGrounding() const
{
    if (auto* cc = SelfCharacter(script)) cc->UseAutomaticGrounding();
}


void ScriptCharacterProxy::SetEnabled(bool enabled) const
{
    if (auto* cc = SelfCharacter(script)) cc->enabled = enabled;
}


bool ScriptCharacterProxy::IsEnabled() const
{
    const auto* cc = SelfCharacter(script);
    return cc && cc->enabled;
}

/// @brief ScriptMeshProxy

} /// @note namespace fbzz::scene
