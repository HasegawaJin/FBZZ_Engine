/// @file    AdditionalTargetPointPersistenceTests.cpp
/// @brief   NavMesh・LOD・IK の TargetPoint 参照と旧形式保存の往復を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-08
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>

#include <Engine/Scene/Components/IKSolverComponent.hpp>
#include <Engine/Scene/Components/LODGroupComponent.hpp>
#include <Engine/Scene/Components/NavMeshOffMeshLinkComponent.hpp>
#include <Engine/Scene/Components/NavMeshAgentComponent.hpp>
#include <Engine/Scene/Components/NavMeshPatrolComponent.hpp>
#include <Engine/Scene/Components/TargetPointComponent.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/SceneSerializer.hpp>
#include <Math/Vector3.hpp>

#include <string>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace fbzz::tests {

class AdditionalTargetPointPersistenceTest : public testkit::EngineFixture {
protected:
    struct AuthoredRoute {
        std::string text;
        std::string ownerGuid;
        std::string firstGuid;
        std::string secondGuid;
        std::uint32_t firstRowId = 0;
    };

    AuthoredRoute MakePointObjectRoute()
    {
        scene::Scene source;
        scene::GameObject& first = source.CreateGameObject("FirstTarget");
        first.AddComponent<scene::TargetPointComponent>();
        scene::GameObject& second = source.CreateGameObject("SecondTarget");
        second.AddComponent<scene::TargetPointComponent>();
        scene::GameObject& lodRenderer = source.CreateGameObject("LodRenderer");
        scene::GameObject& owner = source.CreateGameObject("RouteOwner");

        const std::uint64_t generation = source.GetRenderSceneGeneration();
        scene::NavMeshPatrolComponent patrol{};
        patrol.pointSourceMode = scene::ControlPointSourceMode::POINT_OBJECTS;
        patrol.pointRuntimeMode = scene::ControlPointRuntimeMode::BAKED_STATIC;
        patrol.mode = scene::NavMeshPatrolComponent::Mode::PING_PONG;
        patrol.waitTime = 0.4f;
        patrol.liveRepathInterval = 0.75f;
        patrol.liveRepathDistance = 0.2f;
        patrol.waypointBindings = {
            {17, {first.instanceId, first.GetID(), generation}, 1.25f, 3.5f},
            {29, {second.instanceId, second.GetID(), generation}, 2.0f, 5.0f},
        };
        patrol.bakedWaypoints = {
            {17, {1.0f, 2.0f, 3.0f}, 1.25f, 3.5f},
            {29, {4.0f, 5.0f, 6.0f}, 2.0f, 5.0f},
        };
        patrol.bakedSourceMode = scene::ControlPointSourceMode::POINT_OBJECTS;
        patrol.bakedAdapterVersion = scene::NAV_MESH_PATROL_ADAPTER_VERSION;
        patrol.bakedInputSignature = 0x1020304050607080ull;
        patrol.bakeValid = true;
        owner.AddComponent<scene::NavMeshPatrolComponent>(std::move(patrol));

        scene::NavMeshOffMeshLinkComponent link{};
        link.pointSourceMode = scene::ControlPointSourceMode::POINT_OBJECTS;
        link.startTarget = {first.instanceId, first.GetID(), generation};
        link.endTarget = {second.instanceId, second.GetID(), generation};
        link.traversalTime = 1.75f;
        owner.AddComponent<scene::NavMeshOffMeshLinkComponent>(std::move(link));

        scene::NavMeshAgentComponent agent{};
        agent.target.guid = second.instanceId;
        agent.target.id = second.GetID();
        agent.target.sceneGeneration = generation;
        agent.repathInterval = 0.9f;
        owner.AddComponent<scene::NavMeshAgentComponent>(std::move(agent));

        scene::LODGroupComponent lod{};
        lod.centerSourceMode = scene::ControlPointSourceMode::POINT_OBJECTS;
        lod.centerTarget = {first.instanceId, first.GetID(), generation};
        lod.fadeDuration = 0.6f;
        scene::LODLevel lodLevel{};
        lodLevel.screenRelativeHeight = 0.35f;
        lodLevel.renderers.push_back({lodRenderer.instanceId, lodRenderer.GetID()});
        lod.levels.push_back(std::move(lodLevel));
        owner.AddComponent<scene::LODGroupComponent>(std::move(lod));

        scene::IKSolverComponent ik{};
        scene::IKChain chain{};
        chain.targetReference = {second.instanceId, second.GetID(), generation};
        chain.poleReference = {first.instanceId, first.GetID(), generation};
        chain.targetReferenceMigrationPending = false;
        chain.poleReferenceMigrationPending = false;
        chain.targetGuid = second.instanceId;
        chain.poleGuid = first.instanceId;
        chain.targetEntity = second.GetID();
        chain.poleEntity = first.GetID();
        chain.targetName = "IK Target Label";
        ik.chains.push_back(std::move(chain));
        owner.AddComponent<scene::IKSolverComponent>(std::move(ik));

        AuthoredRoute result{};
        result.text = scene::SceneSerializer::SaveToText(source);
        result.ownerGuid = owner.instanceId;
        result.firstGuid = first.instanceId;
        result.secondGuid = second.instanceId;
        result.firstRowId = 17;
        return result;
    }
};

TEST_F(AdditionalTargetPointPersistenceTest, PointObjectRouteAndTypedReferencesRoundTrip)
{
    const AuthoredRoute saved = MakePointObjectRoute();
    ASSERT_FALSE(saved.text.empty());
    EXPECT_EQ(saved.text.find("waypoints ="), std::string::npos);
    EXPECT_EQ(saved.text.find("waypointWaitTimes ="), std::string::npos);
    EXPECT_EQ(saved.text.find("waypointSpeeds ="), std::string::npos);
    std::unique_ptr<scene::Scene> restored =
        scene::SceneSerializer::LoadDataFromText(saved.text, "");
    ASSERT_NE(restored, nullptr);

    scene::GameObject* owner = restored->FindByGuid(saved.ownerGuid);
    scene::GameObject* first = restored->FindByGuid(saved.firstGuid);
    scene::GameObject* second = restored->FindByGuid(saved.secondGuid);
    ASSERT_NE(owner, nullptr);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);

    const auto* patrol = owner->GetComponent<scene::NavMeshPatrolComponent>();
    ASSERT_NE(patrol, nullptr);
    EXPECT_EQ(patrol->pointSourceMode, scene::ControlPointSourceMode::POINT_OBJECTS);
    EXPECT_EQ(patrol->pointRuntimeMode, scene::ControlPointRuntimeMode::BAKED_STATIC);
    EXPECT_EQ(patrol->mode, scene::NavMeshPatrolComponent::Mode::PING_PONG);
    ASSERT_EQ(patrol->waypointBindings.size(), 2u);
    EXPECT_EQ(patrol->waypointBindings[0].id, saved.firstRowId);
    EXPECT_EQ(patrol->waypointBindings[0].target.guid, saved.firstGuid);
    EXPECT_EQ(patrol->waypointBindings[0].target.id, first->GetID());
    EXPECT_FLOAT_EQ(patrol->waypointBindings[0].waitTime, 1.25f);
    EXPECT_FLOAT_EQ(patrol->waypointBindings[0].speed, 3.5f);
    EXPECT_EQ(patrol->bakedWaypoints.size(), 2u);
    EXPECT_EQ(patrol->bakedSourceMode, scene::ControlPointSourceMode::POINT_OBJECTS);
    EXPECT_EQ(patrol->bakedAdapterVersion, scene::NAV_MESH_PATROL_ADAPTER_VERSION);
    EXPECT_EQ(patrol->bakedInputSignature, 0x1020304050607080ull);
    EXPECT_TRUE(patrol->bakeValid);
    EXPECT_TRUE(patrol->waypoints.empty());

    const auto* link = owner->GetComponent<scene::NavMeshOffMeshLinkComponent>();
    ASSERT_NE(link, nullptr);
    EXPECT_EQ(link->pointSourceMode, scene::ControlPointSourceMode::POINT_OBJECTS);
    EXPECT_EQ(link->startTarget.guid, saved.firstGuid);
    EXPECT_EQ(link->startTarget.id, first->GetID());
    EXPECT_EQ(link->endTarget.guid, saved.secondGuid);
    EXPECT_EQ(link->endTarget.id, second->GetID());
    EXPECT_FLOAT_EQ(link->traversalTime, 1.75f);

    const auto* agent = owner->GetComponent<scene::NavMeshAgentComponent>();
    ASSERT_NE(agent, nullptr);
    EXPECT_EQ(agent->target.guid, saved.secondGuid);
    EXPECT_EQ(agent->target.id, second->GetID());
    EXPECT_FLOAT_EQ(agent->repathInterval, 0.9f);

    const auto* lod = owner->GetComponent<scene::LODGroupComponent>();
    ASSERT_NE(lod, nullptr);
    EXPECT_EQ(lod->centerSourceMode, scene::ControlPointSourceMode::POINT_OBJECTS);
    EXPECT_EQ(lod->centerTarget.guid, saved.firstGuid);
    EXPECT_EQ(lod->centerTarget.id, first->GetID());
    EXPECT_FLOAT_EQ(lod->fadeDuration, 0.6f);
    ASSERT_EQ(lod->levels.size(), 1u);
    EXPECT_FLOAT_EQ(lod->levels[0].screenRelativeHeight, 0.35f);
    ASSERT_EQ(lod->levels[0].renderers.size(), 1u);
    EXPECT_EQ(lod->levels[0].renderers[0].instanceId,
              restored->Find("LodRenderer")->instanceId);

    const auto* ik = owner->GetComponent<scene::IKSolverComponent>();
    ASSERT_NE(ik, nullptr);
    ASSERT_EQ(ik->chains.size(), 1u);
    EXPECT_EQ(ik->chains[0].targetReference.guid, saved.secondGuid);
    EXPECT_EQ(ik->chains[0].targetReference.id, second->GetID());
    EXPECT_EQ(ik->chains[0].poleReference.guid, saved.firstGuid);
    EXPECT_EQ(ik->chains[0].poleReference.id, first->GetID());
    EXPECT_EQ(ik->chains[0].targetName, "IK Target Label");
    EXPECT_FALSE(ik->chains[0].targetReferenceMigrationPending);
    EXPECT_FALSE(ik->chains[0].poleReferenceMigrationPending);
}

TEST_F(AdditionalTargetPointPersistenceTest, CanonicalNullDoesNotReviveLegacyAliases)
{
    scene::Scene source;
    scene::GameObject& target = source.CreateGameObject("LegacyTarget");
    scene::GameObject& owner = source.CreateGameObject("IKOwner");
    scene::IKSolverComponent solver{};
    scene::IKChain canonicalNull{};
    canonicalNull.targetReferenceMigrationPending = false;
    canonicalNull.targetGuid = target.instanceId;
    canonicalNull.targetEntity = target.GetID();
    canonicalNull.targetName = "Stale Alias";
    scene::IKChain legacyNameOnly{};
    legacyNameOnly.targetReferenceMigrationPending = true;
    legacyNameOnly.targetName = target.name;
    solver.chains = {canonicalNull, legacyNameOnly};
    owner.AddComponent<scene::IKSolverComponent>(std::move(solver));

    const std::string text = scene::SceneSerializer::SaveToText(source);
    ASSERT_FALSE(text.empty());
    std::unique_ptr<scene::Scene> restored = scene::SceneSerializer::LoadDataFromText(text, "");
    ASSERT_NE(restored, nullptr);
    scene::GameObject* loadedOwner = restored->Find("IKOwner");
    scene::GameObject* loadedTarget = restored->Find("LegacyTarget");
    ASSERT_NE(loadedOwner, nullptr);
    ASSERT_NE(loadedTarget, nullptr);
    const auto* loaded = loadedOwner->GetComponent<scene::IKSolverComponent>();
    ASSERT_NE(loaded, nullptr);
    ASSERT_EQ(loaded->chains.size(), 2u);
    EXPECT_TRUE(loaded->chains[0].targetReference.guid.empty());
    EXPECT_FALSE(loaded->chains[0].targetEntity.IsValid());
    EXPECT_FALSE(loaded->chains[0].targetReferenceMigrationPending);
    EXPECT_EQ(loaded->chains[0].targetName, "");
    EXPECT_EQ(loaded->chains[1].targetReference.guid, loadedTarget->instanceId);
    EXPECT_EQ(loaded->chains[1].targetEntity, loadedTarget->GetID());
    EXPECT_FALSE(loaded->chains[1].targetReferenceMigrationPending);
}

TEST_F(AdditionalTargetPointPersistenceTest, AppendLoadsAndResolvesPointObjectRoute)
{
    const AuthoredRoute saved = MakePointObjectRoute();
    ASSERT_FALSE(saved.text.empty());
    scene::Scene destination;
    scene::GameObject& unrelated = destination.CreateGameObject("Unrelated");
    const scene::EntityID unrelatedId = unrelated.GetID();
    std::vector<scene::EntityID> appendedRoots;
    ASSERT_TRUE(scene::SceneSerializer::AppendObjects(
        destination, saved.text, nullptr, appendedRoots));

    scene::GameObject* owner = destination.FindByGuid(saved.ownerGuid);
    scene::GameObject* first = destination.FindByGuid(saved.firstGuid);
    scene::GameObject* second = destination.FindByGuid(saved.secondGuid);
    ASSERT_NE(owner, nullptr);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(destination.GetGameObject(unrelatedId), &unrelated);
    const auto* patrol = owner->GetComponent<scene::NavMeshPatrolComponent>();
    ASSERT_NE(patrol, nullptr);
    ASSERT_EQ(patrol->waypointBindings.size(), 2u);
    EXPECT_EQ(patrol->waypointBindings[0].target.id, first->GetID());
    EXPECT_EQ(patrol->waypointBindings[1].target.id, second->GetID());
    const auto* link = owner->GetComponent<scene::NavMeshOffMeshLinkComponent>();
    ASSERT_NE(link, nullptr);
    EXPECT_EQ(link->startTarget.id, first->GetID());
    EXPECT_EQ(link->endTarget.id, second->GetID());
    const auto* agent = owner->GetComponent<scene::NavMeshAgentComponent>();
    ASSERT_NE(agent, nullptr);
    EXPECT_EQ(agent->target.guid, saved.secondGuid);
    EXPECT_EQ(agent->target.id, second->GetID());
    EXPECT_FLOAT_EQ(agent->repathInterval, 0.9f);
    const auto* lod = owner->GetComponent<scene::LODGroupComponent>();
    ASSERT_NE(lod, nullptr);
    EXPECT_EQ(lod->centerTarget.id, first->GetID());
    const auto* ik = owner->GetComponent<scene::IKSolverComponent>();
    ASSERT_NE(ik, nullptr);
    ASSERT_EQ(ik->chains.size(), 1u);
    EXPECT_EQ(ik->chains[0].targetReference.id, second->GetID());
    EXPECT_EQ(ik->chains[0].poleReference.id, first->GetID());
}

TEST_F(AdditionalTargetPointPersistenceTest, LegacyInlinePatrolArraysRemainRoundTrippable)
{
    scene::Scene source;
    scene::GameObject& owner = source.CreateGameObject("InlineRoute");
    scene::NavMeshPatrolComponent patrol{};
    patrol.mode = scene::NavMeshPatrolComponent::Mode::PING_PONG;
    patrol.waitTime = 0.5f;
    patrol.waypoints = {{1.0f, 2.0f, 3.0f}, {-4.0f, 5.0f, 6.0f}};
    patrol.waypointWaitTimes = {0.25f, 0.75f};
    patrol.waypointSpeeds = {2.0f, 3.0f};
    owner.AddComponent<scene::NavMeshPatrolComponent>(std::move(patrol));
    const std::string text = scene::SceneSerializer::SaveToText(source);
    ASSERT_FALSE(text.empty());

    std::unique_ptr<scene::Scene> restored = scene::SceneSerializer::LoadDataFromText(text, "");
    ASSERT_NE(restored, nullptr);
    scene::GameObject* loadedOwner = restored->Find("InlineRoute");
    ASSERT_NE(loadedOwner, nullptr);
    const auto* loaded = loadedOwner->GetComponent<scene::NavMeshPatrolComponent>();
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->pointSourceMode, scene::ControlPointSourceMode::INLINE);
    EXPECT_EQ(loaded->mode, scene::NavMeshPatrolComponent::Mode::PING_PONG);
    ASSERT_EQ(loaded->waypoints.size(), 2u);
    EXPECT_VEC3_NEAR(loaded->waypoints[0], math::Vector3(1.0f, 2.0f, 3.0f),
                     testkit::kLooseTolerance);
    EXPECT_FLOAT_EQ(loaded->waypointWaitTimes[1], 0.75f);
    EXPECT_FLOAT_EQ(loaded->waypointSpeeds[0], 2.0f);
}

} /// @note namespace fbzz::tests
