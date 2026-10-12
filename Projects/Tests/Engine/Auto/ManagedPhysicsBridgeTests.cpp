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

} /// @note namespace fbzz::tests
