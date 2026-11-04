/// @file    ManagedPhysicsBridgeTests.cpp
/// @brief   C# 物理境界の数値・位相・原子性・Scene 所有者検索を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-04
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Engine/Core/Scheduler/SystemContext.hpp>
#include <Engine/Scene/Components/ColliderComponent.hpp>
#include <Engine/Scene/Components/RigidBodyComponent.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Systems/PhysicsSystem.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Math/MathUtils.hpp>
#include <Physics/World.hpp>
#include "../../../Engine/src/Scene/Managed/ManagedPhysicsBridge.hpp"
#include <array>
#include <bit>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>
#include <unordered_map>
#include <unordered_set>

namespace fbzz::tests {

class ManagedPhysicsBridgeTest : public testkit::EngineFixture {
protected:
    scene::Scene m_scene;
    physics::World m_world;
    scene::managed_detail::ManagedPhysicsBridge m_kernel;

    scene::RigidBodyComponent& Body(scene::GameObject& object)
    {
        auto& component = object.AddComponent<scene::RigidBodyComponent>();
        component.rigidBody = std::make_unique<physics::RigidBody>();
        component.rigidBody->m_useGravity = false;
        return component;
    }

    scene::ManagedBridgeStatus BodyRequest(scene::GameObject& object, scene::ManagedBridgeOp op,
        uint32_t selector, const math::Vector3& value = math::Vector3::ZERO,
        bool fixed = true, uint32_t flags = 0)
    {
        scene::ManagedBridgeRequest request;
        request.op = op; request.index = selector; request.flags = flags;
        request.x = value.x; request.y = value.y; request.z = value.z;
        scene::ManagedBridgeReply reply;
        return m_kernel.RunBody(object, request, reply, fixed);
    }

    scene::ManagedBridgeStatus Write(scene::GameObject& object, scene::ManagedPhysicsBodyValue field,
        const math::Vector3& value)
    { return BodyRequest(object, scene::ManagedBridgeOp::PHYSICS_BODY_WRITE, static_cast<uint32_t>(field), value); }

    scene::ManagedBridgeStatus Command(scene::GameObject& object, scene::ManagedPhysicsBodyCommand command,
        const math::Vector3& value = math::Vector3::ZERO, uint32_t flags = 0)
    { return BodyRequest(object, scene::ManagedBridgeOp::PHYSICS_BODY_COMMAND, static_cast<uint32_t>(command), value, true, flags); }

    scene::ManagedBridgeStatus Query(scene::ManagedBridgeOp op, const math::Vector3& origin,
        const math::Vector3& direction, float distance, float radius,
        std::vector<scene::managed_detail::ManagedPhysicsHit>& hits)
    {
        const std::array<float, 4> input{direction.x, direction.y, direction.z, radius};
        scene::ManagedBridgeRequest request;
        request.op = op; request.x = origin.x; request.y = origin.y; request.z = origin.z;
        request.w = op == scene::ManagedBridgeOp::PHYSICS_OVERLAP_SPHERE ? radius : distance;
        if (op != scene::ManagedBridgeOp::PHYSICS_OVERLAP_SPHERE) {
            request.input = std::bit_cast<const char*>(input.data());
            request.inputBytes = op == scene::ManagedBridgeOp::PHYSICS_SPHERE_CAST ? 16u : 12u;
        }
        return m_kernel.Query(m_scene, m_world, request, hits);
    }

    void Sync(scene::ColliderComponent& component, const math::Vector3& position)
    {
        component.collider->Update(position, math::Quaternion::Identity());
        m_world.SyncCollider({}, {component.collider.get(), nullptr, &component.material, math::Vector3::ZERO, false, 0});
    }
};

namespace {
using QueryHit = scene::managed_detail::ManagedPhysicsHit;
enum class OwnerQueryKind { RAY, ALL, SPHERE, OVERLAP };
template<class T> void AddReferenceOwner(scene::GameObject& object, std::unordered_map<const physics::Collider*,scene::GameObject*>& owners)
{
    const auto* component=object.GetComponent<T>();
    if (component && component->enabled && component->collider) owners.emplace(component->collider.get(),&object);
}
/// @note Old Scene-wide owner oracle compares only accepted query behavior, not private cache state or timing.
std::vector<QueryHit> ExhaustiveOwnerQuery(scene::Scene& queryScene, const physics::World& world, OwnerQueryKind kind,
    uint32_t mask, uint32_t triggers, scene::GameObject* ignored)
{
    std::unordered_map<const physics::Collider*,scene::GameObject*> owners;
    for (auto& object:queryScene.GameObjects()) if (object.activeInHierarchy()) {
        AddReferenceOwner<scene::AabbColliderComponent>(object,owners); AddReferenceOwner<scene::BoxColliderComponent>(object,owners);
        AddReferenceOwner<scene::SphereColliderComponent>(object,owners); AddReferenceOwner<scene::CapsuleColliderComponent>(object,owners);
        AddReferenceOwner<scene::CylinderColliderComponent>(object,owners); AddReferenceOwner<scene::MeshColliderComponent>(object,owners);
        AddReferenceOwner<scene::ConvexHullColliderComponent>(object,owners); AddReferenceOwner<scene::TerrainColliderComponent>(object,owners);
    }
    const auto filter=[&](const physics::ColliderInstance& instance) {
        const auto it=owners.find(instance.collider);
        return it!=owners.end() && it->second!=ignored && it->second->layer>=0 && it->second->layer<32
            && ((mask>>static_cast<uint32_t>(it->second->layer))&1u)!=0
            && (triggers==0 || (triggers==1 ? !instance.isTrigger : instance.isTrigger));
    };
    std::vector<QueryHit> result;
    const auto append=[&](const physics::World::RaycastHit& hit) { result.push_back({owners.at(hit.collider),hit.point,hit.normal,hit.distance}); };
    if (kind==OwnerQueryKind::OVERLAP) {
        std::unordered_set<scene::GameObject*> seen;
        for (const auto* hit:world.OverlapSphere({0,0,8},20,filter)) {
            auto* owner=owners.at(hit->collider); if (seen.insert(owner).second) result.push_back({owner});
        }
    } else if (kind==OwnerQueryKind::ALL) {
        for (const auto& hit:world.RaycastAll({},math::Vector3::FORWARD,100,filter)) append(hit);
    } else {
        physics::World::RaycastHit hit;
        const bool found=kind==OwnerQueryKind::SPHERE ? world.SphereCast({},.25f,math::Vector3::FORWARD,100,hit,filter)
            : world.Raycast({},math::Vector3::FORWARD,100,hit,filter);
        if (found) append(hit);
    }
    return result;
}
void ExpectOwnerQueryEquivalent(scene::Scene& queryScene, const physics::World& world,
    scene::managed_detail::ManagedPhysicsBridge& kernel, scene::GameObject* ignored=nullptr)
{
    for (auto kind:{OwnerQueryKind::RAY,OwnerQueryKind::ALL,OwnerQueryKind::SPHERE,OwnerQueryKind::OVERLAP})
        for (uint32_t mask:std::array<uint32_t,4>{0,1u<<3,1u<<7,UINT32_MAX}) for (uint32_t triggers=0;triggers<3;++triggers) {
            scene::ManagedBridgeRequest request;
            request.op=kind==OwnerQueryKind::RAY ? scene::ManagedBridgeOp::PHYSICS_RAYCAST_FILTERED
                : kind==OwnerQueryKind::ALL ? scene::ManagedBridgeOp::PHYSICS_RAYCAST_ALL_FILTERED
                : kind==OwnerQueryKind::SPHERE ? scene::ManagedBridgeOp::PHYSICS_SPHERE_CAST_FILTERED
                : scene::ManagedBridgeOp::PHYSICS_OVERLAP_SPHERE_FILTERED;
            const uint32_t base=kind==OwnerQueryKind::OVERLAP ? 0u : kind==OwnerQueryKind::SPHERE ? 16u : 12u;
            std::array<char,32> packet{};
            const std::array<float,4> direction{0,0,1,.25f};
            if (base) std::memcpy(packet.data(),direction.data(),base);
            std::memcpy(packet.data()+base,&mask,4); std::memcpy(packet.data()+base+4,&triggers,4);
            request.input=packet.data();request.inputBytes=base+16;
            request.z=kind==OwnerQueryKind::OVERLAP ? 8.f : 0.f;request.w=kind==OwnerQueryKind::OVERLAP ? 20.f : 100.f;
            const auto expected=ExhaustiveOwnerQuery(queryScene,world,kind,mask,triggers,ignored);
            std::vector<QueryHit> actual;
            ASSERT_EQ(kernel.Query(queryScene,world,request,actual,ignored),scene::ManagedBridgeStatus::OK);
            ASSERT_EQ(actual.size(),expected.size());
            for (size_t i=0;i<actual.size();++i) {
                EXPECT_EQ(actual[i].object,expected[i].object);EXPECT_NEAR(actual[i].distance,expected[i].distance,testkit::kTolerance);
                EXPECT_VEC3_NEAR(actual[i].point,expected[i].point,testkit::kTolerance);EXPECT_VEC3_NEAR(actual[i].normal,expected[i].normal,testkit::kTolerance);
            }
        }
}
} /// @note namespace
TEST_F(ManagedPhysicsBridgeTest, BodyValuesMatchNativeLocksSleepStaticAndZeroMass)
{
    auto& object = m_scene.CreateGameObject("Body");
    auto& component = Body(object);
    auto& body = *component.rigidBody;
    ASSERT_EQ(Write(object, scene::ManagedPhysicsBodyValue::MASS, {2, 0, 0}), scene::ManagedBridgeStatus::OK);
    ASSERT_EQ(Command(object, scene::ManagedPhysicsBodyCommand::IMPULSE, {4, 6, 8}), scene::ManagedBridgeStatus::OK);
    EXPECT_VEC3_NEAR(body.GetVelocity(), (math::Vector3{2, 3, 4}), testkit::kTolerance);
    ASSERT_EQ(Command(object, scene::ManagedPhysicsBodyCommand::FREEZE_POSITION, {}, 5), scene::ManagedBridgeStatus::OK);
    EXPECT_VEC3_NEAR(body.GetVelocity(), (math::Vector3{0, 3, 0}), testkit::kTolerance);
    ASSERT_EQ(Write(object, scene::ManagedPhysicsBodyValue::ANGULAR_VELOCITY, {2, 3, 4}), scene::ManagedBridgeStatus::OK);
    ASSERT_EQ(Command(object, scene::ManagedPhysicsBodyCommand::FREEZE_ROTATION, {}, 2), scene::ManagedBridgeStatus::OK);
    EXPECT_VEC3_NEAR(body.GetAngularVelocity(), (math::Vector3{2, 0, 4}), testkit::kTolerance);
    ASSERT_EQ(Write(object, scene::ManagedPhysicsBodyValue::GRAVITY_SCALE, {-2, 0, 0}), scene::ManagedBridgeStatus::OK);
    EXPECT_NEAR(body.m_gravityScale, -2, testkit::kTolerance);
    ASSERT_EQ(Command(object, scene::ManagedPhysicsBodyCommand::SLEEP), scene::ManagedBridgeStatus::OK);
    EXPECT_TRUE(body.IsSleeping());
    ASSERT_EQ(Command(object, scene::ManagedPhysicsBodyCommand::WAKE), scene::ManagedBridgeStatus::OK);
    EXPECT_FALSE(body.IsSleeping());
    ASSERT_EQ(Write(object, scene::ManagedPhysicsBodyValue::IS_STATIC, {1, 0, 0}), scene::ManagedBridgeStatus::OK);
    EXPECT_TRUE(body.IsStatic()); EXPECT_EQ(body.GetInvMass(), 0.0f);
    ASSERT_EQ(Write(object, scene::ManagedPhysicsBodyValue::IS_STATIC, {}), scene::ManagedBridgeStatus::OK);
    EXPECT_NEAR(body.GetInvMass(), 0.5f, testkit::kTolerance);
    ASSERT_EQ(Write(object, scene::ManagedPhysicsBodyValue::MASS, {}), scene::ManagedBridgeStatus::OK);
    EXPECT_EQ(body.GetMass(), 0.0f); EXPECT_EQ(body.GetInvMass(), 0.0f);
}

TEST_F(ManagedPhysicsBridgeTest, RejectsMissingDisabledWrongPhaseAndInvalidInputWithoutMutation)
{
    auto& object = m_scene.CreateGameObject("Body");
    scene::ManagedBridgeRequest read;
    read.op = scene::ManagedBridgeOp::PHYSICS_BODY_READ;
    read.index = static_cast<uint32_t>(scene::ManagedPhysicsBodyValue::EXISTS);
    scene::ManagedBridgeReply reply;
    EXPECT_EQ(m_kernel.RunBody(object, read, reply, false), scene::ManagedBridgeStatus::OK);
    EXPECT_EQ(reply.integer, 0);
    auto& component = Body(object);
    auto& body = *component.rigidBody;
    component.enabled = false;
    EXPECT_EQ(m_kernel.RunBody(object, read, reply, false), scene::ManagedBridgeStatus::OK);
    EXPECT_EQ(reply.integer, 1);
    EXPECT_EQ(Write(object, scene::ManagedPhysicsBodyValue::VELOCITY, {1, 2, 3}), scene::ManagedBridgeStatus::NOT_FOUND);
    component.enabled = true;
    EXPECT_EQ(BodyRequest(object, scene::ManagedBridgeOp::PHYSICS_BODY_WRITE,
        static_cast<uint32_t>(scene::ManagedPhysicsBodyValue::VELOCITY), {1, 2, 3}, false), scene::ManagedBridgeStatus::INVALID_SCOPE);
    EXPECT_EQ(Write(object, scene::ManagedPhysicsBodyValue::MASS, {-1, 0, 0}), scene::ManagedBridgeStatus::INVALID_ARGUMENT);
    EXPECT_EQ(Write(object, scene::ManagedPhysicsBodyValue::MASS, {std::numeric_limits<float>::denorm_min(), 0, 0}), scene::ManagedBridgeStatus::INVALID_ARGUMENT);
    EXPECT_EQ(Write(object, scene::ManagedPhysicsBodyValue::IS_STATIC, {2, 0, 0}), scene::ManagedBridgeStatus::INVALID_ARGUMENT);
    EXPECT_EQ(Write(object, scene::ManagedPhysicsBodyValue::VELOCITY, {std::numeric_limits<float>::infinity(), 0, 0}), scene::ManagedBridgeStatus::INVALID_ARGUMENT);
    EXPECT_EQ(Command(object, scene::ManagedPhysicsBodyCommand::FREEZE_POSITION, {}, 8), scene::ManagedBridgeStatus::INVALID_ARGUMENT);
    EXPECT_EQ(BodyRequest(object, scene::ManagedBridgeOp::PHYSICS_BODY_COMMAND, 999), scene::ManagedBridgeStatus::INVALID_ARGUMENT);
    EXPECT_EQ(body.GetMass(), 1.0f); EXPECT_FALSE(body.IsStatic());
    EXPECT_VEC3_NEAR(body.GetVelocity(), math::Vector3::ZERO, testkit::kTolerance);
}

TEST_F(ManagedPhysicsBridgeTest, RejectsAccumulationAndImpulseOverflowBeforeTouchingLiveState)
{
    auto& object = m_scene.CreateGameObject("Body");
    auto& body = *Body(object).rigidBody;
    const float maximum = std::numeric_limits<float>::max();
    ASSERT_EQ(Command(object, scene::ManagedPhysicsBodyCommand::FORCE, {maximum, 0, 0}), scene::ManagedBridgeStatus::OK);
    EXPECT_EQ(Command(object, scene::ManagedPhysicsBodyCommand::FORCE, {maximum, 0, 0}), scene::ManagedBridgeStatus::INVALID_ARGUMENT);
    EXPECT_TRUE(body.IsFiniteState());
    body.Integrate(0.25f);
    EXPECT_NEAR(body.GetVelocity().x / maximum, 0.25f, testkit::kTolerance);
    ASSERT_EQ(Write(object, scene::ManagedPhysicsBodyValue::VELOCITY, {maximum, 0, 0}), scene::ManagedBridgeStatus::OK);
    EXPECT_EQ(Command(object, scene::ManagedPhysicsBodyCommand::IMPULSE, {maximum, 0, 0}), scene::ManagedBridgeStatus::INVALID_ARGUMENT);
    EXPECT_EQ(body.GetVelocity().x, maximum);
    body.SetVelocity({});
    ASSERT_EQ(Command(object, scene::ManagedPhysicsBodyCommand::TORQUE, {maximum, 0, 0}), scene::ManagedBridgeStatus::OK);
    EXPECT_EQ(Command(object, scene::ManagedPhysicsBodyCommand::TORQUE, {maximum, 0, 0}), scene::ManagedBridgeStatus::INVALID_ARGUMENT);
    EXPECT_TRUE(body.IsFiniteState());
    physics::RigidBody expected = body;
    expected.Integrate(1.0e-20f); body.Integrate(1.0e-20f);
    EXPECT_VEC3_NEAR(body.GetAngularVelocity(), expected.GetAngularVelocity(), testkit::kTolerance);
    body.Sleep();
    const std::array<float, 3> point{maximum, maximum, maximum};
    scene::ManagedBridgeRequest request;
    request.op = scene::ManagedBridgeOp::PHYSICS_BODY_COMMAND;
    request.index = static_cast<uint32_t>(scene::ManagedPhysicsBodyCommand::FORCE_AT_POINT);
    request.x = maximum; request.input = std::bit_cast<const char*>(point.data()); request.inputBytes = 12;
    scene::ManagedBridgeReply reply;
    EXPECT_EQ(m_kernel.RunBody(object, request, reply, true), scene::ManagedBridgeStatus::INVALID_ARGUMENT);
    EXPECT_TRUE(body.IsSleeping()); EXPECT_TRUE(body.IsFiniteState());
}

TEST_F(ManagedPhysicsBridgeTest, MassAndStaticRefreshOnlyLiveSceneOwnedInertiaSources)
{
    auto& object = m_scene.CreateGameObject("Body");
    auto& body = *Body(object).rigidBody;
    auto& sphere = object.AddComponent<scene::SphereColliderComponent>();
    body.SetInertiaFromCollider(sphere.collider.get());
    ASSERT_EQ(Write(object, scene::ManagedPhysicsBodyValue::MASS, {2, 0, 0}), scene::ManagedBridgeStatus::OK);
    EXPECT_EQ(body.GetInertiaCollider(), sphere.collider.get());
    body.ApplyAngularImpulse({1, 0, 0});
    EXPECT_VEC3_NEAR(body.GetAngularVelocity(), (math::Vector3{5, 0, 0}), testkit::kTolerance);
    object.RemoveComponent<scene::SphereColliderComponent>();
    body.SetAngularVelocity({});
    ASSERT_EQ(Write(object, scene::ManagedPhysicsBodyValue::MASS, {4, 0, 0}), scene::ManagedBridgeStatus::OK);
    EXPECT_EQ(body.GetInertiaCollider(), nullptr);
    body.ApplyAngularImpulse({1, 0, 0});
    EXPECT_VEC3_NEAR(body.GetAngularVelocity(), (math::Vector3{0.25f, 0, 0}), testkit::kTolerance);
    auto& replacement = object.AddComponent<scene::SphereColliderComponent>();
    body.SetInertiaFromCollider(replacement.collider.get());
    const physics::Collider* oldSource = body.GetInertiaCollider();
    replacement.SetRadius(2);
    ASSERT_NE(oldSource, replacement.collider.get());
    ASSERT_EQ(Write(object, scene::ManagedPhysicsBodyValue::IS_STATIC, {1, 0, 0}), scene::ManagedBridgeStatus::OK);
    EXPECT_EQ(body.GetInertiaCollider(), nullptr); EXPECT_TRUE(body.IsStatic()); EXPECT_TRUE(body.IsFiniteState());
    ASSERT_EQ(Write(object, scene::ManagedPhysicsBodyValue::IS_STATIC, {}), scene::ManagedBridgeStatus::OK);
    body.SetAngularVelocity({}); body.ApplyAngularImpulse({1, 0, 0});
    EXPECT_VEC3_NEAR(body.GetAngularVelocity(), (math::Vector3{0.25f, 0, 0}), testkit::kTolerance);
    body.SetInertiaFromCollider(replacement.collider.get());
    ASSERT_EQ(Write(object, scene::ManagedPhysicsBodyValue::MASS, {2, 0, 0}), scene::ManagedBridgeStatus::OK);
    body.SetAngularVelocity({}); body.ApplyAngularImpulse({1, 0, 0});
    EXPECT_VEC3_NEAR(body.GetAngularVelocity(), (math::Vector3{0.3125f, 0, 0}), testkit::kTolerance);
}

TEST_F(ManagedPhysicsBridgeTest, TeleportSynchronizesHierarchyLocksAndSubsequentPhysicsStep)
{
    auto& parent = m_scene.CreateGameObject("Parent");
    auto& target = m_scene.CreateGameObject("Body");
    auto& child = m_scene.CreateGameObject("Child");
    ASSERT_TRUE(target.SetParent(&parent)); ASSERT_TRUE(child.SetParent(&target));
    parent.transform.position = {10, 2, 3}; parent.transform.scale = {2, 3, 4};
    parent.transform.rotation = math::Quaternion::FromEuler({0.2f, -0.4f, 0.1f});
    target.transform.position = {1, 2, 3}; child.transform.position = {1, 0, 0}; child.SetActive(false);
    scene::FlushWorldTransforms(m_scene);
    auto& component = Body(target);
    auto& body = *component.rigidBody;
    body.SetPosition(target.transform.worldPosition); body.SetRotation(target.transform.worldRotation);
    body.SetVelocity({1, 2, 3}); body.SetFreezePosition({false, true, false});
    const float frozenY = body.GetPosition().y;
    const math::Quaternion localRotation = target.transform.rotation;
    const math::Vector3 localScale = target.transform.scale;
    ASSERT_EQ(Command(target, scene::ManagedPhysicsBodyCommand::TELEPORT, {20, 40, 60}), scene::ManagedBridgeStatus::OK);
    EXPECT_VEC3_NEAR(body.GetPosition(), (math::Vector3{20, frozenY, 60}), testkit::kTolerance);
    EXPECT_VEC3_NEAR(target.transform.worldPosition, body.GetPosition(), testkit::kTolerance);
    EXPECT_QUAT_NEAR(target.transform.rotation, localRotation, testkit::kTolerance);
    EXPECT_VEC3_NEAR(target.transform.scale, localScale, testkit::kTolerance);
    EXPECT_TRUE(component.hasPhysicsSyncState); EXPECT_TRUE(component.hasPhysicsPoseHistory);
    EXPECT_VEC3_NEAR(component.lastPhysicsPosition, target.transform.worldPosition, testkit::kTolerance);
    scene::Transform expectedChild = child.transform;
    scene::UpdateWorldTransform(expectedChild, &target.transform);
    EXPECT_VEC3_NEAR(child.transform.worldPosition, expectedChild.worldPosition, testkit::kTolerance);
    scene::PhysicsSystem system;
    SystemContext context{m_scene, m_world, nullptr, nullptr, 0.01f, 0.01f, true, true};
    system.Update(context);
    EXPECT_VEC3_NEAR(body.GetPosition(), (math::Vector3{20.01f, frozenY, 60.03f}), 1.0e-4f);
    EXPECT_VEC3_NEAR(target.transform.worldPosition, body.GetPosition(), 1.0e-4f);
    EXPECT_VEC3_NEAR(body.GetVelocity(), (math::Vector3{1, 0, 3}), testkit::kTolerance);
}

TEST_F(ManagedPhysicsBridgeTest, TeleportRejectsZeroScaleProjectionAndDescendantOverflowAtomically)
{
    auto& parent = m_scene.CreateGameObject("Parent");
    auto& target = m_scene.CreateGameObject("Body");
    auto& child = m_scene.CreateGameObject("Child");
    ASSERT_TRUE(target.SetParent(&parent)); ASSERT_TRUE(child.SetParent(&target));
    parent.transform.scale = {0, 1, 1}; target.transform.position = {3, 0, 0};
    scene::FlushWorldTransforms(m_scene);
    auto& component = Body(target);
    component.rigidBody->SetPosition(target.transform.worldPosition);
    const scene::Transform oldTarget = target.transform;
    EXPECT_EQ(Command(target, scene::ManagedPhysicsBodyCommand::TELEPORT, {1, 0, 0}), scene::ManagedBridgeStatus::INVALID_ARGUMENT);
    EXPECT_VEC3_NEAR(target.transform.position, oldTarget.position, testkit::kTolerance);
    EXPECT_VEC3_NEAR(component.rigidBody->GetPosition(), math::Vector3::ZERO, testkit::kTolerance);
    EXPECT_FALSE(component.hasPhysicsSyncState);
    parent.transform.scale = {1, 1, 1}; child.transform.position = {std::numeric_limits<float>::max(), 0, 0};
    target.transform.worldPosition = {123, 0, 0}; parent.transform.worldPosition = {321, 0, 0};
    const math::Vector3 oldParent = parent.transform.worldPosition;
    EXPECT_EQ(Command(target, scene::ManagedPhysicsBodyCommand::TELEPORT, {std::numeric_limits<float>::max(), 0, 0}), scene::ManagedBridgeStatus::INVALID_ARGUMENT);
    EXPECT_VEC3_NEAR(target.transform.worldPosition, (math::Vector3{123, 0, 0}), testkit::kTolerance);
    EXPECT_VEC3_NEAR(parent.transform.worldPosition, oldParent, testkit::kTolerance);
    EXPECT_VEC3_NEAR(component.rigidBody->GetPosition(), math::Vector3::ZERO, testkit::kTolerance);
}

TEST_F(ManagedPhysicsBridgeTest, TeleportZeroScaleDoesNotRelaxProjectionAtLargeWorldCoordinates)
{
    auto& parent = m_scene.CreateGameObject("Parent");
    auto& target = m_scene.CreateGameObject("Body");
    ASSERT_TRUE(target.SetParent(&parent));
    parent.transform.position = {1.0e6f, 0, 0}; parent.transform.scale = {0, 1, 1};
    target.transform.position = {3, 0, 0}; scene::FlushWorldTransforms(m_scene);
    auto& component = Body(target);
    component.rigidBody->SetPosition(target.transform.worldPosition);
    EXPECT_EQ(Command(target, scene::ManagedPhysicsBodyCommand::TELEPORT, {1.0e6f + 1, 0, 0}), scene::ManagedBridgeStatus::INVALID_ARGUMENT);
    EXPECT_VEC3_NEAR(target.transform.worldPosition, (math::Vector3{1.0e6f, 0, 0}), testkit::kTolerance);
    EXPECT_VEC3_NEAR(component.rigidBody->GetPosition(), target.transform.worldPosition, testkit::kTolerance);
    EXPECT_VEC3_NEAR(target.transform.position, (math::Vector3{3, 0, 0}), testkit::kTolerance);
    EXPECT_FALSE(component.hasPhysicsSyncState);
}

TEST_F(ManagedPhysicsBridgeTest, WorldReadsValidateLayerBoundsAndPreserveCollisionMatrix)
{
    m_world.SetGravity({1, -2, 3});
    LayerCollisionMatrix matrix; matrix.Set(3, 7, false); m_world.SetCollisionMatrix(matrix);
    scene::ManagedBridgeRequest request; scene::ManagedBridgeReply reply;
    request.op = scene::ManagedBridgeOp::PHYSICS_WORLD_READ;
    ASSERT_EQ(m_kernel.RunWorld(m_world, request, reply), scene::ManagedBridgeStatus::OK);
    EXPECT_VEC3_NEAR((math::Vector3{reply.x, reply.y, reply.z}), (math::Vector3{1, -2, 3}), testkit::kTolerance);
    request.index = static_cast<uint32_t>(scene::ManagedPhysicsWorldValue::LAYERS_COLLIDE);
    request.x = 3; request.y = 7;
    ASSERT_EQ(m_kernel.RunWorld(m_world, request, reply), scene::ManagedBridgeStatus::OK); EXPECT_EQ(reply.integer, 0);
    request.x = 32;
    EXPECT_EQ(m_kernel.RunWorld(m_world, request, reply), scene::ManagedBridgeStatus::INVALID_ARGUMENT);
    request.x = 3.5f;
    EXPECT_EQ(m_kernel.RunWorld(m_world, request, reply), scene::ManagedBridgeStatus::INVALID_ARGUMENT);
    EXPECT_FALSE(m_world.LayersCollide(3, 7));
}

TEST_F(ManagedPhysicsBridgeTest, QueriesKeepShapeHitsButDeduplicateOverlapAndFilterSceneOwnership)
{
    auto& object = m_scene.CreateGameObject("TwoShapes");
    auto& sphere = object.AddComponent<scene::SphereColliderComponent>();
    auto& box = object.AddComponent<scene::BoxColliderComponent>();
    auto& inactive = m_scene.CreateGameObject("Inactive");
    auto& inactiveShape = inactive.AddComponent<scene::SphereColliderComponent>(); inactive.SetActive(false);
    auto& disabled = m_scene.CreateGameObject("Disabled");
    auto& disabledShape = disabled.AddComponent<scene::SphereColliderComponent>(); disabledShape.enabled = false;
    physics::SphereCollider foreign(1); foreign.Update({0, 0, 1}, math::Quaternion::Identity());
    m_world.BeginSceneSync(); Sync(sphere, {0, 0, 5}); Sync(box, {0, 0, 5});
    Sync(inactiveShape, {0, 0, 2}); Sync(disabledShape, {0, 0, 3});
    m_world.SyncCollider({}, {&foreign, nullptr, nullptr, math::Vector3::ZERO, false, 0}); m_world.EndSceneSync();
    std::vector<scene::managed_detail::ManagedPhysicsHit> hits;
    ASSERT_EQ(Query(scene::ManagedBridgeOp::PHYSICS_RAYCAST_ALL, {}, math::Vector3::FORWARD, 100, 0, hits), scene::ManagedBridgeStatus::OK);
    ASSERT_EQ(hits.size(), 2u); EXPECT_EQ(hits[0].object, &object); EXPECT_EQ(hits[1].object, &object);
    EXPECT_NEAR(hits[0].distance, 4.5f, testkit::kTolerance);
    ASSERT_EQ(Query(scene::ManagedBridgeOp::PHYSICS_RAYCAST, {}, math::Vector3::FORWARD, 4.5f, 0, hits), scene::ManagedBridgeStatus::OK);
    ASSERT_EQ(hits.size(), 1u); EXPECT_NEAR(hits[0].distance, 4.5f, testkit::kTolerance);
    ASSERT_EQ(Query(scene::ManagedBridgeOp::PHYSICS_SPHERE_CAST, {}, math::Vector3::FORWARD, 100, 0.5f, hits), scene::ManagedBridgeStatus::OK);
    ASSERT_EQ(hits.size(), 1u); EXPECT_NEAR(hits[0].distance, 4, testkit::kTolerance);
    EXPECT_VEC3_NEAR(hits[0].point, (math::Vector3{0, 0, 4}), testkit::kTolerance);
    ASSERT_EQ(Query(scene::ManagedBridgeOp::PHYSICS_OVERLAP_SPHERE, {0, 0, 5}, {}, 0, 1, hits), scene::ManagedBridgeStatus::OK);
    ASSERT_EQ(hits.size(), 1u); EXPECT_EQ(hits[0].object, &object);
}

TEST_F(ManagedPhysicsBridgeTest, QueriesRejectMalformedInputAndIgnoreRemovedColliderPointers)
{
    auto& object = m_scene.CreateGameObject("Deleted");
    auto& sphere = object.AddComponent<scene::SphereColliderComponent>();
    m_world.BeginSceneSync(); Sync(sphere, {0, 0, 5}); m_world.EndSceneSync();
    std::vector<scene::managed_detail::ManagedPhysicsHit> hits{{&object, {}, {}, 12}};
    EXPECT_EQ(Query(scene::ManagedBridgeOp::PHYSICS_RAYCAST, {}, {std::numeric_limits<float>::max(), 0, 0}, 10, 0, hits), scene::ManagedBridgeStatus::INVALID_ARGUMENT);
    ASSERT_EQ(hits.size(), 1u); EXPECT_EQ(hits[0].distance, 12.0f);
    EXPECT_EQ(Query(scene::ManagedBridgeOp::PHYSICS_SPHERE_CAST, {}, math::Vector3::FORWARD, 10, -1, hits), scene::ManagedBridgeStatus::INVALID_ARGUMENT);
    ASSERT_TRUE(m_scene.DestroyGameObject(object.GetID())); m_scene.FlushDestroyQueue(0);
    ASSERT_EQ(Query(scene::ManagedBridgeOp::PHYSICS_RAYCAST, {}, math::Vector3::FORWARD, 10, 0, hits), scene::ManagedBridgeStatus::OK);
    EXPECT_TRUE(hits.empty());
    ASSERT_EQ(Query(scene::ManagedBridgeOp::PHYSICS_OVERLAP_SPHERE, {0, 0, 5}, {}, 0, 10, hits), scene::ManagedBridgeStatus::OK);
    EXPECT_TRUE(hits.empty());
}

TEST_F(ManagedPhysicsBridgeTest, LargestFiniteNoHitAndDegenerateNormalsNeverBecomePhantomHits)
{
    auto& object = m_scene.CreateGameObject("OffAxis");
    auto& sphere = object.AddComponent<scene::SphereColliderComponent>();
    m_world.BeginSceneSync(); Sync(sphere, {20, 0, 5}); m_world.EndSceneSync();
    std::vector<scene::managed_detail::ManagedPhysicsHit> hits;
    const float maximum = std::numeric_limits<float>::max();
    ASSERT_EQ(Query(scene::ManagedBridgeOp::PHYSICS_RAYCAST, {}, math::Vector3::FORWARD, maximum, 0, hits), scene::ManagedBridgeStatus::OK);
    EXPECT_TRUE(hits.empty());
    ASSERT_EQ(Query(scene::ManagedBridgeOp::PHYSICS_SPHERE_CAST, {}, math::Vector3::FORWARD, maximum, 1, hits), scene::ManagedBridgeStatus::OK);
    EXPECT_TRUE(hits.empty());
    ASSERT_EQ(Query(scene::ManagedBridgeOp::PHYSICS_RAYCAST, {}, {}, maximum, 0, hits), scene::ManagedBridgeStatus::OK);
    EXPECT_TRUE(hits.empty());
    auto* native = static_cast<physics::SphereCollider*>(sphere.collider.get()); native->m_radius = 0;
    native->Update({}, math::Quaternion::Identity());
    ASSERT_EQ(Query(scene::ManagedBridgeOp::PHYSICS_RAYCAST, {}, math::Vector3::FORWARD, 10, 0, hits), scene::ManagedBridgeStatus::OK);
    EXPECT_TRUE(hits.empty());
    ASSERT_EQ(Query(scene::ManagedBridgeOp::PHYSICS_SPHERE_CAST, {}, math::Vector3::FORWARD, 10, 0, hits), scene::ManagedBridgeStatus::OK);
    EXPECT_TRUE(hits.empty());
}

TEST_F(ManagedPhysicsBridgeTest, CandidateOwnersMatchOldFiltersClosestAllSphereAndCompoundOverlap)
{
    auto& near=m_scene.CreateGameObject("Compound");near.layer=3;
    auto& sphere=near.AddComponent<scene::SphereColliderComponent>();auto& box=near.AddComponent<scene::BoxColliderComponent>();
    auto& far=m_scene.CreateGameObject("Trigger");far.layer=7;auto& trigger=far.AddComponent<scene::SphereColliderComponent>();
    auto& disabled=m_scene.CreateGameObject("Disabled");auto& disabledShape=disabled.AddComponent<scene::SphereColliderComponent>();disabledShape.enabled=false;
    auto& inactive=m_scene.CreateGameObject("Inactive");auto& inactiveShape=inactive.AddComponent<scene::SphereColliderComponent>();inactive.SetActive(false);
    physics::SphereCollider foreign(.2f);foreign.Update({0,0,1},math::Quaternion::Identity());
    m_world.BeginSceneSync();Sync(sphere,{0,0,5});Sync(box,{0,0,6});Sync(disabledShape,{0,0,2});Sync(inactiveShape,{0,0,3});
    trigger.collider->Update({0,0,9},math::Quaternion::Identity());
    m_world.SyncCollider({}, {trigger.collider.get(),nullptr,&trigger.material,{},true,7});
    m_world.SyncCollider({}, {&foreign,nullptr,nullptr,{},false,0});m_world.EndSceneSync();
    ExpectOwnerQueryEquivalent(m_scene,m_world,m_kernel);ExpectOwnerQueryEquivalent(m_scene,m_world,m_kernel,&near);
    near.layer=7;trigger.enabled=false;inactive.SetActive(true);
    ExpectOwnerQueryEquivalent(m_scene,m_world,m_kernel);
}
TEST_F(ManagedPhysicsBridgeTest, CandidateOwnersDoNotSurviveRemovalTypeReplacementSceneClearOrExchange)
{
    auto& object=m_scene.CreateGameObject("Replace");object.layer=3;
    auto& sphere=object.AddComponent<scene::SphereColliderComponent>();
    m_world.BeginSceneSync();Sync(sphere,{0,0,5});m_world.EndSceneSync();
    ExpectOwnerQueryEquivalent(m_scene,m_world,m_kernel);
    auto oldShape=std::move(sphere.collider);object.RemoveComponent<scene::SphereColliderComponent>();
    ExpectOwnerQueryEquivalent(m_scene,m_world,m_kernel);
    auto& box=object.AddComponent<scene::BoxColliderComponent>();
    m_world.BeginSceneSync();Sync(box,{0,0,5});m_world.EndSceneSync();
    ExpectOwnerQueryEquivalent(m_scene,m_world,m_kernel);
    scene::Scene other;
    ExpectOwnerQueryEquivalent(other,m_world,m_kernel);
    auto retained=std::move(box.collider);m_scene.Clear();
    ExpectOwnerQueryEquivalent(m_scene,m_world,m_kernel);
    auto& replacement=m_scene.CreateGameObject("New scene lifetime");replacement.layer=7;
    auto& newShape=replacement.AddComponent<scene::SphereColliderComponent>();
    m_world.BeginSceneSync();Sync(newShape,{0,0,8});m_world.EndSceneSync();
    ExpectOwnerQueryEquivalent(m_scene,m_world,m_kernel);
}
TEST_F(ManagedPhysicsBridgeTest, BroadCandidateFallbackMatchesOldMapWithoutScanningDecorationObjects)
{
    for (int i=0;i<128;++i) m_scene.CreateGameObject("Decoration");
    m_world.BeginSceneSync();
    for (int i=0;i<12;++i) {
        auto& object=m_scene.CreateGameObject("Collider");object.layer=i%2==0 ? 3 : 7;
        auto& sphere=object.AddComponent<scene::SphereColliderComponent>();Sync(sphere,{0,0,static_cast<float>(i+2)});
    }
    m_world.EndSceneSync();
    ExpectOwnerQueryEquivalent(m_scene,m_world,m_kernel);
}
class PhysicsSpatialDebugBridgeTest : public ManagedPhysicsBridgeTest {
protected:
    scene::PhysicsSystem m_system;

    void SetUp() override
    {
        ManagedPhysicsBridgeTest::SetUp();
        scene::PhysicsSystem::InvalidateSpatialDebugSnapshot();
        m_world.SetGravity(math::Vector3::ZERO);
        m_world.SetSubsteps(1);
    }

    void TearDown() override
    {
        scene::PhysicsSystem::InvalidateSpatialDebugSnapshot();
        scene::Script::SetPhysicsWorld(nullptr);
        ManagedPhysicsBridgeTest::TearDown();
    }

    scene::GameObject& DebugCollider(const std::string& name)
    {
        auto& object = m_scene.CreateGameObject(name);
        object.AddComponent<scene::SphereColliderComponent>();
        Body(object).rigidBody->m_allowSleeping = false;
        return object;
    }

    scene::PhysicsSpatialDebugRequest Request(scene::EntityID selected = {})
    {
        scene::PhysicsSpatialDebugRequest request;
        request.sceneGeneration = m_scene.GetRenderSceneGeneration();
        request.selectedEntity = selected;
        request.physics.enabled = true;
        request.physics.bounds = true;
        return request;
    }

    void StepDebug(scene::PhysicsSystem& system)
    {
        SystemContext context{m_scene, m_world, nullptr, nullptr, 0.02f, 0.02f, true, true};
        system.Update(context);
    }
};

TEST_F(PhysicsSpatialDebugBridgeTest, ReusesRequestRevisionAndRetiresChangedOrWrongScenePublications)
{
    auto& object = DebugCollider("Debug collider");
    auto request = Request(object.GetID());
    const uint64_t revision = scene::PhysicsSystem::SetSpatialDebugRequest(request);
    EXPECT_EQ(scene::PhysicsSystem::SetSpatialDebugRequest(request), revision);
    request.physics.selected = {123u, 456u};
    request.physics.selectedWorldEpoch = 999u;
    EXPECT_EQ(scene::PhysicsSystem::SetSpatialDebugRequest(request), revision);
    EXPECT_EQ(scene::PhysicsSystem::GetSpatialDebugSnapshot(request.sceneGeneration), nullptr);
    StepDebug(m_system);
    const auto first = scene::PhysicsSystem::GetSpatialDebugSnapshot(request.sceneGeneration);
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first->revision, revision);
    EXPECT_EQ(scene::PhysicsSystem::GetSpatialDebugSnapshot(request.sceneGeneration + 1u), nullptr);

    request.physics.maxProxies = 1;
    EXPECT_GT(scene::PhysicsSystem::SetSpatialDebugRequest(request), revision);
    EXPECT_EQ(scene::PhysicsSystem::GetSpatialDebugSnapshot(request.sceneGeneration), nullptr);
    StepDebug(m_system);
    ASSERT_NE(scene::PhysicsSystem::GetSpatialDebugSnapshot(request.sceneGeneration), nullptr);
    ++request.sceneGeneration;
    scene::PhysicsSystem::SetSpatialDebugRequest(request);
    StepDebug(m_system);
    EXPECT_EQ(scene::PhysicsSystem::GetSpatialDebugSnapshot(request.sceneGeneration), nullptr);
    EXPECT_EQ(m_world.GetBroadPhaseDebugSnapshot(), nullptr);
}

TEST_F(PhysicsSpatialDebugBridgeTest, ResolvesFirstActiveSyncedColliderAndRejectsStaleOrNonColliderSelection)
{
    auto& object = DebugCollider("Compound debug collider");
    object.AddComponent<scene::AabbColliderComponent>().enabled = false;
    object.AddComponent<scene::BoxColliderComponent>();
    auto request = Request(object.GetID());
    request.physics.selectedOnly = true;
    scene::PhysicsSystem::SetSpatialDebugRequest(request);
    StepDebug(m_system);
    const auto publication = scene::PhysicsSystem::GetSpatialDebugSnapshot(request.sceneGeneration);
    ASSERT_NE(publication, nullptr);
    ASSERT_NE(publication->physics, nullptr);
    const auto expected = object.GetComponent<scene::BoxColliderComponent>()->colliderHandle;
    EXPECT_TRUE(expected.IsValid());
    EXPECT_EQ(publication->physics->request.selected.slot, expected.slot);
    EXPECT_EQ(publication->physics->request.selected.generation, expected.generation);
    EXPECT_EQ(publication->physics->request.selectedWorldEpoch, m_world.GetBroadPhaseDebugWorldEpoch());
    EXPECT_TRUE(publication->physics->selectionValid);

    ++request.selectedEntity.generation;
    scene::PhysicsSystem::SetSpatialDebugRequest(request);
    StepDebug(m_system);
    auto invalid = scene::PhysicsSystem::GetSpatialDebugSnapshot(request.sceneGeneration);
    ASSERT_NE(invalid, nullptr);
    EXPECT_FALSE(invalid->physics->selectionValid);
    EXPECT_TRUE(invalid->physics->nodes.empty());
    EXPECT_TRUE(invalid->physics->proxies.empty());
    EXPECT_TRUE(invalid->physics->pairs.empty());

    request.selectedEntity = m_scene.CreateGameObject("No collider").GetID();
    scene::PhysicsSystem::SetSpatialDebugRequest(request);
    StepDebug(m_system);
    invalid = scene::PhysicsSystem::GetSpatialDebugSnapshot(request.sceneGeneration);
    ASSERT_NE(invalid, nullptr);
    EXPECT_FALSE(invalid->physics->selectionValid);
    EXPECT_TRUE(invalid->physics->proxies.empty());
}

TEST_F(PhysicsSpatialDebugBridgeTest, DisableRetiresPublicationAndReplacementOwnerDisablesWorldCapture)
{
    auto& object = DebugCollider("Debug collider");
    const auto request = Request(object.GetID());
    scene::PhysicsSystem::SetSpatialDebugRequest(request);
    StepDebug(m_system);
    ASSERT_NE(scene::PhysicsSystem::GetSpatialDebugSnapshot(request.sceneGeneration), nullptr);
    ASSERT_NE(m_world.GetBroadPhaseDebugSnapshot(), nullptr);

    scene::PhysicsSystem::InvalidateSpatialDebugSnapshot();
    EXPECT_EQ(scene::PhysicsSystem::GetSpatialDebugSnapshot(request.sceneGeneration), nullptr);
    scene::PhysicsSystem replacementOwner;
    StepDebug(replacementOwner);
    EXPECT_EQ(m_world.GetBroadPhaseDebugSnapshot(), nullptr);
    EXPECT_EQ(scene::PhysicsSystem::GetSpatialDebugSnapshot(request.sceneGeneration), nullptr);
}

TEST_F(PhysicsSpatialDebugBridgeTest, SceneGenerationChangeRejectsOldPublicationAndRetainedValuesStayImmutable)
{
    auto& object = DebugCollider("Old scene collider");
    const auto request = Request(object.GetID());
    scene::PhysicsSystem::SetSpatialDebugRequest(request);
    StepDebug(m_system);
    const auto retained = scene::PhysicsSystem::GetSpatialDebugSnapshot(request.sceneGeneration);
    ASSERT_NE(retained, nullptr);
    ASSERT_NE(retained->physics, nullptr);
    const uint64_t sequence = retained->physics->sequence;
    const auto proxyCount = retained->physics->proxies.size();
    ASSERT_GT(proxyCount, 0u);
    const auto oldHandle = retained->physics->proxies.front().handle;

    m_scene.Clear();
    const uint64_t currentGeneration = m_scene.GetRenderSceneGeneration();
    ASSERT_NE(currentGeneration, request.sceneGeneration);
    EXPECT_EQ(scene::PhysicsSystem::GetSpatialDebugSnapshot(currentGeneration), nullptr);
    StepDebug(m_system);
    EXPECT_EQ(m_world.GetBroadPhaseDebugSnapshot(), nullptr);
    auto& replacement = DebugCollider("New scene collider");
    scene::PhysicsSystem::SetSpatialDebugRequest(Request(replacement.GetID()));
    EXPECT_EQ(scene::PhysicsSystem::GetSpatialDebugSnapshot(request.sceneGeneration), nullptr);
    StepDebug(m_system);
    const auto current = scene::PhysicsSystem::GetSpatialDebugSnapshot(currentGeneration);
    ASSERT_NE(current, nullptr);
    EXPECT_EQ(current->sceneGeneration, currentGeneration);
    EXPECT_EQ(retained->sceneGeneration, request.sceneGeneration);
    EXPECT_EQ(retained->physics->sequence, sequence);
    EXPECT_EQ(retained->physics->proxies.size(), proxyCount);
    EXPECT_EQ(retained->physics->proxies.front().handle.slot, oldHandle.slot);
    EXPECT_EQ(retained->physics->proxies.front().handle.generation, oldHandle.generation);
}

TEST_F(ManagedPhysicsBridgeTest, IndexedQueriesSurviveComponentRemovalShapeReplacementAndSceneClear)
{
    EXPECT_TRUE(m_world.OverlapSphere({}, 1).empty());
    auto& target = m_scene.CreateGameObject("Target");
    target.AddComponent<scene::SphereColliderComponent>();
    for (int index = 0; index < 31; ++index)
        m_scene.CreateGameObject("Far").AddComponent<scene::SphereColliderComponent>();
    const auto syncScene = [&] {
        m_world.BeginSceneSync();
        int index = 0;
        for (auto& object : m_scene.GameObjects()) {
            auto* sphere = object.GetComponent<scene::SphereColliderComponent>();
            if (sphere) Sync(*sphere, index++ == 0 ? math::Vector3(0, 0, 5) : math::Vector3(100 + index * 4.0f, 0, 5));
        }
        m_world.EndSceneSync();
    };
    const auto expectEmpty = [&] {
        std::vector<scene::managed_detail::ManagedPhysicsHit> hits;
        for (const auto op : {scene::ManagedBridgeOp::PHYSICS_RAYCAST, scene::ManagedBridgeOp::PHYSICS_RAYCAST_ALL,
            scene::ManagedBridgeOp::PHYSICS_SPHERE_CAST, scene::ManagedBridgeOp::PHYSICS_OVERLAP_SPHERE}) {
            ASSERT_EQ(Query(op, {}, math::Vector3::FORWARD, 10, 1, hits), scene::ManagedBridgeStatus::OK);
            EXPECT_TRUE(hits.empty());
        }
    };
    syncScene();
    target.RemoveComponent<scene::SphereColliderComponent>();
    expectEmpty();

    auto& replacement = target.AddComponent<scene::SphereColliderComponent>();
    syncScene();
    replacement.SetRadius(2);
    expectEmpty();

    syncScene();
    m_scene.Clear();
    m_scene.CreateGameObject("NewSceneTarget").AddComponent<scene::SphereColliderComponent>();
    expectEmpty();
    syncScene();
    std::vector<scene::managed_detail::ManagedPhysicsHit> hits;
    ASSERT_EQ(Query(scene::ManagedBridgeOp::PHYSICS_RAYCAST, {}, math::Vector3::FORWARD, 10, 0, hits), scene::ManagedBridgeStatus::OK);
    ASSERT_EQ(hits.size(), 1u);
    EXPECT_EQ(hits[0].object->name, "NewSceneTarget");
}

} /// @note namespace fbzz::tests
