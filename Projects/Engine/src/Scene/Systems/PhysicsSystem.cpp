// FBZZ Engine
// PhysicsSystem.cpp | fbzz::scene
// physics::World を Step し、RigidBodyComponent の結果を Transform に書き戻す
#include "Engine/Scene/Systems/PhysicsSystem.hpp"
#include "Engine/Scene/Scene.hpp"
#include "Engine/Scene/Components/ColliderComponent.hpp"
#include "Engine/Scene/Components/RigidBodyComponent.hpp"
#include "Engine/Scene/Components/VolumeComponent.hpp"
#include "Engine/Scene/ScriptComponent.hpp"
#include <Physics/ColliderVolume.hpp>
#include <Physics/World.hpp>
#include <memory>
#include <unordered_map>
#include <vector>

namespace fbzz::scene {

namespace {

struct ColliderOwner {
    GameObject* gameObject = nullptr;
    ColliderComponent* collider = nullptr;
};

using ColliderOwnerMap = std::unordered_map<const physics::Collider*, ColliderOwner>;
using ScriptCollisionCallback = void (Script::*)(const CollisionInfo&);

void DispatchToScript(Scene& scene,
                      const ColliderOwner& self,
                      const ColliderOwner& other,
                      ScriptCollisionCallback callback)
{
    if (!self.gameObject || !other.gameObject) return;

    auto* scriptComponent = self.gameObject->GetComponent<ScriptComponent>();
    if (!scriptComponent || !scriptComponent->script || !scriptComponent->script->enabled) return;

    scriptComponent->script->SetContext(&scene, self.gameObject);
    CollisionInfo info;
    info.self = self.gameObject;
    info.other = other.gameObject;
    info.selfCollider = self.collider;
    info.otherCollider = other.collider;
    (scriptComponent->script.get()->*callback)(info);
}

void DispatchCollisionEvent(Scene& scene,
                            const ColliderOwnerMap& owners,
                            const physics::CollisionEvent& event,
                            ScriptCollisionCallback callback)
{
    auto ownerA = owners.find(event.colliderA);
    auto ownerB = owners.find(event.colliderB);
    if (ownerA == owners.end() || ownerB == owners.end()) return;

    DispatchToScript(scene, ownerA->second, ownerB->second, callback);
    DispatchToScript(scene, ownerB->second, ownerA->second, callback);
}

void DispatchCollisionEvents(Scene& scene,
                             physics::World& world,
                             const ColliderOwnerMap& owners)
{
    for (const auto& event : world.GetEnterEvents())
    {
        DispatchCollisionEvent(scene, owners, event, event.isTrigger
            ? &Script::OnTriggerEnter
            : &Script::OnCollisionEnter);
    }

    for (const auto& event : world.GetStayEvents())
    {
        DispatchCollisionEvent(scene, owners, event, event.isTrigger
            ? &Script::OnTriggerStay
            : &Script::OnCollisionStay);
    }

    for (const auto& event : world.GetExitEvents())
    {
        DispatchCollisionEvent(scene, owners, event, event.isTrigger
            ? &Script::OnTriggerExit
            : &Script::OnCollisionExit);
    }
}

} // namespace

void PhysicsSystem(Scene& scene, physics::World& world, float dt) {
    std::vector<std::shared_ptr<physics::RigidBody>> bodies;
    std::vector<physics::ColliderInstance> colliders;
    std::vector<std::shared_ptr<physics::Volume>> volumes;
    ColliderOwnerMap colliderOwners;

    for (auto [tf, rb] : scene.View<Transform, RigidBodyComponent>()) {
        if (!rb.enabled || !rb.rigidBody) continue;
        rb.rigidBody->SetPosition(tf.position);
        rb.rigidBody->SetRotation(tf.rotation);
        bodies.push_back(rb.rigidBody);
    }

    for (auto& go : scene.GameObjects()) {
        auto* col = go.GetComponent<ColliderComponent>();
        if (!col || !col->enabled || !col->collider) continue;

        auto* rb = go.GetComponent<RigidBodyComponent>();
        physics::RigidBody* body = rb && rb->enabled && rb->rigidBody ? rb->rigidBody.get() : nullptr;

        col->collider->Update(go.transform.position, go.transform.rotation);
        if (body)
            body->SetInertiaFromCollider(col->collider.get());

        colliders.push_back({ col->collider, body, &col->material, col->isTrigger, go.layer });
        colliderOwners[col->collider.get()] = { &go, col };

        auto* volume = go.GetComponent<VolumeComponent>();
        if (volume && volume->enabled && col->isTrigger) {
            if (volume->duration >= 0.0f && volume->elapsed >= volume->duration) continue;
            if (volume->duration >= 0.0f) volume->elapsed += dt;

            physics::VolumeSettings settings;
            settings.type = volume->type;
            settings.gravity = volume->gravity;
            settings.magneticField = volume->magneticField;
            settings.swirlStrength = volume->swirlStrength;
            settings.inwardStrength = volume->inwardStrength;
            settings.liftStrength = volume->liftStrength;
            settings.buoyancy = volume->buoyancy;
            settings.drag = volume->drag;
            settings.explosionImpulse = volume->explosionImpulse;
            settings.timeScale = volume->timeScale;
            settings.duration = volume->duration;
            volumes.push_back(std::make_shared<physics::ColliderVolume>(col->collider, settings));
        }
    }

    world.SetBodies(std::move(bodies));
    world.SetColliders(std::move(colliders));
    world.SetVolumes(std::move(volumes));
    world.Step(dt);
    DispatchCollisionEvents(scene, world, colliderOwners);

    for (auto [tf, rb] : scene.View<Transform, RigidBodyComponent>()) {
        if (!rb.enabled || !rb.rigidBody) continue;
        tf.localPosition = rb.rigidBody->GetPosition();
        tf.localRotation = rb.rigidBody->GetRotation();
        tf.position = tf.localPosition;
        tf.rotation = tf.localRotation;
    }
}

} // namespace fbzz::scene
