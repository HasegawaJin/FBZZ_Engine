/// @file    WaterInteractionTests.cpp
/// @brief   接触演出の容量・粒子供給・接地・Pause・Scene寿命を本番境界で検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-09
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Engine/Core/Scheduler/SystemContext.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Scene/Components/ParticleEmitter.hpp>
#include <Engine/Scene/Components/RigidBodyComponent.hpp>
#include <Engine/Scene/Components/WaterComponent.hpp>
#include <Engine/Scene/Components/WaterExclusionComponent.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/SceneSerializer.hpp>
#include <Engine/Scene/Systems/ParticleSimulationRuntime.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Engine/Scene/Systems/WaterInteractionSystem.hpp>
#include <Engine/Scene/Systems/WaterSystem.hpp>
#include <Engine/Scene/WaterInteraction.hpp>
#include <Engine/Scene/Fields/FlowField.hpp>
#include <Physics/RigidBody.hpp>
#include <Physics/World.hpp>
#include <algorithm>
#include <limits>
#include <memory>

namespace fbzz::tests {
namespace {
class WaterInteractionTest : public testkit::EngineFixture {
protected:
    scene::EntityID Water(scene::Scene& source)
    {
        auto& object = source.CreateGameObject("Water");
        const auto id = object.GetID();
        auto& water = object.AddComponent<scene::WaterComponent>();
        water.enableGerstnerWaves = false;
        water.extentX = water.extentZ = 50;
        water.buoyancyDepth = 10;
        scene::FlushWorldTransforms(source);
        scene::PrepareWaterSceneSurfaces(source);
        return id;
    }
    scene::EntityID Body(scene::Scene& source)
    {
        auto& object = source.CreateGameObject("Swimmer");
        const auto id = object.GetID();
        object.transform.position = {0, -1, 0};
        object.AddComponent<scene::RigidBodyComponent>().rigidBody = std::make_unique<physics::RigidBody>();
        scene::FlushWorldTransforms(source);
        return id;
    }
    scene::WaterInteractionImpulse Bubble() const
    {
        return {scene::WaterInteractionKind::BUBBLES, {0, -1, 0}, {0, 0.3f, 0}, 0.2f, 0.5f, 2};
    }
    void Update(scene::Scene& source, scene::WaterInteractionSystem& system,
        float dt = 0.1f, bool playing = true, bool simulating = true)
    {
        ++Time::frameCount;
        SystemContext context{source, m_world, nullptr, nullptr, dt, dt, simulating, playing};
        system.Update(context);
    }
    physics::World m_world;
};
} /// @note namespace

TEST_F(WaterInteractionTest, QueueOnlyChangesValuesAndCleanupFeedsTheExistingParticleSimulator)
{
    scene::Scene source;
    const auto owner = Water(source);
    auto* water = source.GetComponent<scene::WaterComponent>(owner);
    const size_t initialObjects = source.GameObjectCount();
    ASSERT_TRUE(scene::QueueWaterInteraction(source, owner, Bubble()));
    EXPECT_EQ(source.GameObjectCount(), initialObjects);
    ASSERT_EQ(water->interactionRuntime.pending.size(), 1u);
    scene::WaterInteractionSystem system;
    Update(source, system);
    water = source.GetComponent<scene::WaterComponent>(owner);
    const auto snapshot = scene::WaterInteractionSnapshot(*water);
    ASSERT_NE(snapshot, nullptr);
    ASSERT_EQ(snapshot->plumes.size(), 1u);
    EXPECT_EQ(snapshot->owner, owner);
    EXPECT_EQ(snapshot->ownerLifetime, source.GetComponentLifetime<scene::WaterComponent>(owner));
    EXPECT_TRUE(water->interactionRuntime.pending.empty());
    const auto emitterId = water->interactionRuntime.emitters[0].id;
    auto* emitter = source.GetComponent<scene::ParticleEmitter>(emitterId);
    ASSERT_NE(emitter, nullptr);
    ASSERT_EQ(emitter->runtime.injectedSpawns.size(), 1u);
    EXPECT_EQ(emitter->settings.maxParticles, 64);
    EXPECT_EQ(emitter->settings.materialPath, scene::WATER_BUBBLE_MATERIAL);
    EXPECT_TRUE(source.GetGameObject(emitterId)->IsRuntimeGenerated());
    EXPECT_EQ(source.GetGameObject(emitterId)->GetParent(), nullptr);
    scene::UpdateParticleCpuSimulation(source, m_world, 0.1f, 0.1f);
    EXPECT_TRUE(emitter->runtime.injectedSpawns.empty());
    EXPECT_GT(emitter->runtime.particles.size(), 0u);
    EXPECT_LE(emitter->runtime.particles.size(), 64u);
    EXPECT_GT(emitter->runtime.particles.front().position.y, -1.0f);
}

TEST_F(WaterInteractionTest, PendingCapacityAndInvalidInputRejectWithoutChangingTheQueue)
{
    scene::Scene source;
    const auto owner = Water(source);
    for (size_t i = 0; i < scene::WATER_INTERACTION_MAX_PENDING; ++i)
        ASSERT_TRUE(scene::QueueWaterInteraction(source, owner, Bubble()));
    EXPECT_FALSE(scene::QueueWaterInteraction(source, owner, Bubble()));
    auto bad = Bubble();
    bad.worldPosition.x = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(scene::QueueWaterInteraction(source, owner, bad));
    bad = Bubble();
    bad.kind = static_cast<scene::WaterInteractionKind>(255);
    EXPECT_FALSE(scene::QueueWaterInteraction(source, owner, bad));
    bad = Bubble();
    bad.radius = -1;
    EXPECT_FALSE(scene::QueueWaterInteraction(source, owner, bad));
    EXPECT_EQ(source.GetComponent<scene::WaterComponent>(owner)->interactionRuntime.pending.size(), 32u);
    EXPECT_EQ(source.GameObjectCount(), 1u);
}

TEST_F(WaterInteractionTest, ExcludedOutsideAndDisabledWaterNeverReceiveAnImpulse)
{
    scene::Scene source;
    const auto owner = Water(source);
    auto outside = Bubble();
    outside.worldPosition.x = 100;
    EXPECT_FALSE(scene::QueueWaterInteraction(source, owner, outside));
    auto& exclusion = source.CreateGameObject("Dry cabin");
    exclusion.transform.position = {0, -1, 0};
    exclusion.AddComponent<scene::WaterExclusionComponent>().size = {2, 2, 2};
    scene::FlushWorldTransforms(source);
    scene::PrepareWaterSceneSurfaces(source);
    EXPECT_FALSE(scene::QueueWaterInteraction(source, owner, Bubble()));
    source.GetComponent<scene::WaterComponent>(owner)->interactionSettings.enabled = false;
    EXPECT_FALSE(scene::QueueWaterInteraction(source, owner, Bubble()));
    EXPECT_TRUE(source.GetComponent<scene::WaterComponent>(owner)->interactionRuntime.pending.empty());
}

TEST_F(WaterInteractionTest, RepeatedEventsReuseTwoEmittersAndRespectParticleAndPlumeBudgets)
{
    scene::Scene source;
    const auto owner = Water(source);
    scene::WaterInteractionSystem system;
    for (int frame = 0; frame < 3; ++frame) {
        for (int index = 0; index < 32; ++index) {
            auto impulse = Bubble();
            if (index & 1) impulse.kind = scene::WaterInteractionKind::SEDIMENT;
            ASSERT_TRUE(scene::QueueWaterInteraction(source, owner, impulse));
        }
        Update(source, system);
        const auto* water = source.GetComponent<scene::WaterComponent>(owner);
        const auto snapshot = scene::WaterInteractionSnapshot(*water);
        ASSERT_NE(snapshot, nullptr);
        EXPECT_LE(snapshot->plumes.size(), 32u);
        EXPECT_EQ(source.GameObjectCount(), 3u);
        for (const auto& state : water->interactionRuntime.emitters) {
            const auto* emitter = source.GetComponent<scene::ParticleEmitter>(state.id);
            ASSERT_NE(emitter, nullptr);
            size_t pending = 0;
            for (const auto& spawn : emitter->runtime.injectedSpawns) pending += static_cast<size_t>(spawn.count);
            EXPECT_LE(pending + emitter->runtime.particles.size(), 64u);
        }
    }
}

TEST_F(WaterInteractionTest, PausePreservesTheImmutableFrameAndResumeAdvectsAndExpiresItsPlume)
{
    scene::Scene source;
    const auto owner = Water(source);
    auto impulse = Bubble();
    impulse.kind = scene::WaterInteractionKind::SEDIMENT;
    impulse.worldVelocity = {1, 0, 0};
    impulse.lifetime = 0.5f;
    ASSERT_TRUE(scene::QueueWaterInteraction(source, owner, impulse));
    scene::WaterInteractionSystem system;
    Update(source, system);
    auto* water = source.GetComponent<scene::WaterComponent>(owner);
    const auto first = scene::WaterInteractionSnapshot(*water);
    ASSERT_NE(first, nullptr);
    Update(source, system, 0.2f, true, false);
    EXPECT_EQ(scene::WaterInteractionSnapshot(*water), first);
    EXPECT_FLOAT_EQ(first->plumes[0].age, 0);
    Update(source, system, 0.2f);
    const auto moved = scene::WaterInteractionSnapshot(*water);
    ASSERT_EQ(moved->plumes.size(), 1u);
    EXPECT_NE(moved, first);
    EXPECT_NEAR(moved->plumes[0].worldPosition.x, 0.2f, testkit::kTolerance);
    EXPECT_LT(moved->plumes[0].density, first->plumes[0].density);
    EXPECT_FLOAT_EQ(first->plumes[0].worldPosition.x, 0);
    Update(source, system, 0.4f);
    EXPECT_TRUE(scene::WaterInteractionSnapshot(*water)->plumes.empty());
}

TEST_F(WaterInteractionTest, VerifiedContactSurvivesCatchupStepsButDoesNotRepeatWithoutAnotherContact)
{
    scene::Scene source;
    const auto owner = Water(source);
    const auto body = Body(source);
    scene::SupplyWaterMotionImpulse(source, owner, body, {0, -1, 0}, {2, 0, 0}, 0.01f, false);
    for (int step = 0; step < 8; ++step) {
        scene::BeginWaterInteractionPhysicsStep(source, 0.01f);
        scene::SupplyWaterGroundContact(source, body, {0, -2, 0}, {0, 1, 0}, 2);
    }
    scene::WaterInteractionSystem system;
    Update(source, system);
    auto* water = source.GetComponent<scene::WaterComponent>(owner);
    auto snapshot = scene::WaterInteractionSnapshot(*water);
    ASSERT_NE(snapshot, nullptr);
    const auto countSediment = [](const auto& frame) {
        return std::count_if(frame->plumes.begin(), frame->plumes.end(),
            [](const auto& plume) { return plume.kind == scene::WaterInteractionKind::SEDIMENT; });
    };
    EXPECT_EQ(countSediment(snapshot), 1);
    scene::SupplyWaterMotionImpulse(source, owner, body, {0, -1, 0}, {}, 0.5f, false);
    Update(source, system, 0.5f);
    EXPECT_EQ(countSediment(scene::WaterInteractionSnapshot(*water)), 1);
}

TEST_F(WaterInteractionTest, WallContactAndUntrackedActorsCannotInventBedSediment)
{
    scene::Scene source;
    const auto owner = Water(source);
    const auto body = Body(source);
    scene::SupplyWaterGroundContact(source, body, {0, -2, 0}, {0, 1, 0}, 8);
    EXPECT_TRUE(source.GetComponent<scene::WaterComponent>(owner)->interactionRuntime.pending.empty());
    scene::SupplyWaterMotionImpulse(source, owner, body, {0, -1, 0}, {2, 0, 0}, 0.01f, false);
    scene::SupplyWaterGroundContact(source, body, {0, -2, 0}, {1, 0, 0}, 8);
    scene::WaterInteractionSystem system;
    Update(source, system);
    const auto snapshot = scene::WaterInteractionSnapshot(*source.GetComponent<scene::WaterComponent>(owner));
    ASSERT_NE(snapshot, nullptr);
    EXPECT_TRUE(std::none_of(snapshot->plumes.begin(), snapshot->plumes.end(),
        [](const auto& plume) { return plume.kind == scene::WaterInteractionKind::SEDIMENT; }));
}

TEST_F(WaterInteractionTest, MotionIsRateLimitedAndSurfaceFoamUsesTheExistingSurfacePosition)
{
    scene::Scene source;
    const auto owner = Water(source);
    const auto body = Body(source);
    for (int i = 0; i < 20; ++i)
        scene::SupplyWaterMotionImpulse(source, owner, body, {0, -1, 0}, {3, 0, 0}, 0.01f, false);
    auto* water = source.GetComponent<scene::WaterComponent>(owner);
    EXPECT_EQ(water->interactionRuntime.pending.size(), 1u);
    ASSERT_TRUE(scene::QueueWaterSurfaceInteraction(source, owner, {0, 8, 0}, 0.5f));
    EXPECT_NEAR(water->interactionRuntime.pending.back().worldPosition.y, 0, testkit::kTolerance);
    EXPECT_EQ(water->interactionRuntime.pending.back().kind, scene::WaterInteractionKind::SURFACE_FOAM);
}

TEST_F(WaterInteractionTest, OwnerDeletionRetiresItsEmitterAndAReplacementComponentStartsEmpty)
{
    scene::Scene source;
    const auto owner = Water(source);
    ASSERT_TRUE(scene::QueueWaterInteraction(source, owner, Bubble()));
    scene::WaterInteractionSystem system;
    Update(source, system);
    const auto emitter = source.GetComponent<scene::WaterComponent>(owner)->interactionRuntime.emitters[0].id;
    ASSERT_NE(source.GetGameObject(emitter), nullptr);
    ASSERT_TRUE(source.DestroyGameObject(owner));
    Update(source, system);
    EXPECT_EQ(source.GetGameObject(emitter), nullptr);
    const auto replacement = Water(source);
    const auto* water = source.GetComponent<scene::WaterComponent>(replacement);
    EXPECT_TRUE(water->interactionRuntime.pending.empty());
    EXPECT_EQ(scene::WaterInteractionSnapshot(*water), nullptr);
}

TEST_F(WaterInteractionTest, PauseRetiresDeletedRemovedAndDisabledOwnersWithoutAdvancingHealthyEffects)
{
    for (int mutation = 0; mutation < 3; ++mutation) {
        SCOPED_TRACE(mutation);
        scene::Scene source;
        const auto owner = Water(source);
        ASSERT_TRUE(scene::QueueWaterInteraction(source, owner, Bubble()));
        scene::WaterInteractionSystem system;
        Update(source, system);
        auto* water = source.GetComponent<scene::WaterComponent>(owner);
        const auto initial = scene::WaterInteractionSnapshot(*water);
        const auto emitter = water->interactionRuntime.emitters[0].id;
        Update(source, system, 0.2f, true, false);
        EXPECT_EQ(scene::WaterInteractionSnapshot(*water), initial);
        EXPECT_FLOAT_EQ(initial->plumes[0].age, 0);
        if (mutation == 0) ASSERT_TRUE(source.DestroyGameObject(owner));
        if (mutation == 1) source.RemoveComponent<scene::WaterComponent>(owner);
        if (mutation == 2) water->interactionSettings.enabled = false;
        Update(source, system, 0.2f, true, false);
        EXPECT_EQ(source.GetGameObject(emitter), nullptr);
        if (mutation == 2) EXPECT_EQ(scene::WaterInteractionSnapshot(*water), nullptr);
    }
}

TEST_F(WaterInteractionTest, MixedEventLifetimesRemainDistinctInsideOneReusedParticleEmitter)
{
    scene::Scene source;
    const auto owner = Water(source);
    auto shortEvent = Bubble();
    shortEvent.lifetime = 2;
    auto longEvent = Bubble();
    longEvent.lifetime = 5;
    ASSERT_TRUE(scene::QueueWaterInteraction(source, owner, shortEvent));
    ASSERT_TRUE(scene::QueueWaterInteraction(source, owner, longEvent));
    scene::WaterInteractionSystem system;
    Update(source, system);
    const auto emitterId = source.GetComponent<scene::WaterComponent>(owner)->interactionRuntime.emitters[0].id;
    scene::UpdateParticleCpuSimulation(source, m_world, 0.1f, 0.1f);
    auto* emitter = source.GetComponent<scene::ParticleEmitter>(emitterId);
    ASSERT_NE(emitter, nullptr);
    EXPECT_GT(std::count_if(emitter->runtime.particles.begin(), emitter->runtime.particles.end(),
        [](const auto& particle) { return particle.lifetime == 2; }), 0);
    EXPECT_GT(std::count_if(emitter->runtime.particles.begin(), emitter->runtime.particles.end(),
        [](const auto& particle) { return particle.lifetime == 5; }), 0);
    source.GetComponent<scene::WaterComponent>(owner)->interactionSettings.bubbleLifetime = 7;
    ASSERT_TRUE(scene::QueueWaterInteraction(source, owner, Bubble()));
    Update(source, system);
    emitter = source.GetComponent<scene::ParticleEmitter>(emitterId);
    ASSERT_NE(emitter, nullptr);
    EXPECT_FLOAT_EQ(emitter->settings.lifetime, 7);
    EXPECT_EQ(source.GameObjectCount(), 2u);
    ++Time::frameCount;
    scene::UpdateParticleCpuSimulation(source, m_world, 2.5f, 2.6f);
    EXPECT_FALSE(std::any_of(emitter->runtime.particles.begin(), emitter->runtime.particles.end(),
        [](const auto& particle) { return particle.lifetime == 2; }));
    EXPECT_TRUE(std::any_of(emitter->runtime.particles.begin(), emitter->runtime.particles.end(),
        [](const auto& particle) { return particle.lifetime == 5; }));
}

TEST_F(WaterInteractionTest, EmitterCreationAfterLargeSceneGrowthKeepsOwnerAndSnapshotIdentity)
{
    scene::Scene source;
    const auto owner = Water(source);
    const auto lifetime = source.GetComponentLifetime<scene::WaterComponent>(owner);
    for (int i = 0; i < 1000; ++i) source.CreateGameObject("Unrelated");
    ASSERT_TRUE(scene::QueueWaterInteraction(source, owner, Bubble()));
    auto sediment = Bubble();
    sediment.kind = scene::WaterInteractionKind::SEDIMENT;
    ASSERT_TRUE(scene::QueueWaterInteraction(source, owner, sediment));
    auto foam = Bubble();
    foam.kind = scene::WaterInteractionKind::SURFACE_FOAM;
    foam.worldPosition.y = 0;
    ASSERT_TRUE(scene::QueueWaterInteraction(source, owner, foam));
    scene::WaterInteractionSystem system;
    Update(source, system);
    const auto* water = source.GetComponent<scene::WaterComponent>(owner);
    ASSERT_NE(water, nullptr);
    const auto snapshot = scene::WaterInteractionSnapshot(*water);
    ASSERT_NE(snapshot, nullptr);
    EXPECT_EQ(snapshot->owner, owner);
    EXPECT_EQ(snapshot->ownerLifetime, lifetime);
    EXPECT_EQ(source.GameObjectCount(), 1003u);
    ASSERT_EQ(snapshot->plumes.size(), 3u);
    EXPECT_EQ(snapshot->plumes[0].kind, scene::WaterInteractionKind::BUBBLES);
    EXPECT_EQ(snapshot->plumes[1].kind, scene::WaterInteractionKind::SEDIMENT);
    EXPECT_EQ(snapshot->plumes[2].kind, scene::WaterInteractionKind::SURFACE_FOAM);
    ASSERT_EQ(water->ripples.size(), 1u);
    for (const auto& emitterState : water->interactionRuntime.emitters)
        EXPECT_NE(source.GetComponent<scene::ParticleEmitter>(emitterState.id), nullptr);
}

TEST_F(WaterInteractionTest, ExplicitSurfaceFoamFeedsTheExistingRippleFoamPathWithoutDuplicatingNativeRipples)
{
    scene::Scene source;
    const auto owner = Water(source);
    auto foam = Bubble();
    foam.kind = scene::WaterInteractionKind::SURFACE_FOAM;
    foam.worldPosition = {2, 0, 3};
    ASSERT_TRUE(scene::QueueWaterInteraction(source, owner, foam));
    scene::WaterInteractionSystem system;
    Update(source, system);
    auto* water = source.GetComponent<scene::WaterComponent>(owner);
    ASSERT_EQ(water->ripples.size(), 1u);
    EXPECT_VEC2_NEAR(water->ripples[0].center, (math::Vector2{2, 3}), testkit::kTolerance);
    ASSERT_TRUE(scene::QueueWaterSurfaceInteraction(source, owner, {2, 0, 3}, 0.5f));
    Update(source, system);
    EXPECT_EQ(water->ripples.size(), 1u);
}

TEST_F(WaterInteractionTest, SceneDeactivationReturnsGeneratedObjectsAndCannotLeakIntoAnotherScene)
{
    scene::Scene source;
    const auto owner = Water(source);
    ASSERT_TRUE(scene::QueueWaterInteraction(source, owner, Bubble()));
    scene::WaterInteractionSystem system;
    Update(source, system);
    scene::ClearWaterInteractions(source);
    EXPECT_EQ(source.GameObjectCount(), 1u);
    EXPECT_EQ(scene::WaterInteractionSnapshot(*source.GetComponent<scene::WaterComponent>(owner)), nullptr);
    scene::Scene destination;
    const auto other = Water(destination);
    Update(destination, system);
    EXPECT_EQ(destination.GameObjectCount(), 1u);
    EXPECT_EQ(scene::WaterInteractionSnapshot(*destination.GetComponent<scene::WaterComponent>(other)), nullptr);
}

TEST_F(WaterInteractionTest, MovingDryVolumePrunesBothOwnedEmittersOnResumeWithoutAdvancingPause)
{
    scene::Scene source;
    const auto owner = Water(source);
    ASSERT_TRUE(scene::QueueWaterInteraction(source, owner, Bubble()));
    auto sediment = Bubble();
    sediment.kind = scene::WaterInteractionKind::SEDIMENT;
    ASSERT_TRUE(scene::QueueWaterInteraction(source, owner, sediment));
    scene::WaterInteractionSystem system;
    Update(source, system);
    scene::UpdateParticleCpuSimulation(source, m_world, 0.1f, 0.1f);
    const auto states = source.GetComponent<scene::WaterComponent>(owner)->interactionRuntime.emitters;
    for (const auto& state : states) ASSERT_GT(source.GetComponent<scene::ParticleEmitter>(state.id)->runtime.particles.size(), 0u);
    auto& dry = source.CreateGameObject("Moving dry room");
    dry.transform.position = {10, -1, 0};
    dry.AddComponent<scene::WaterExclusionComponent>().size = {6, 6, 6};
    scene::FlushWorldTransforms(source);
    scene::PrepareWaterSceneSurfaces(source);
    const auto dryId = dry.GetID();
    source.GetGameObject(dryId)->transform.position = {0, -1, 0};
    scene::FlushWorldTransforms(source);
    scene::PrepareWaterSceneSurfaces(source);
    Update(source, system, 0.1f, true, false);
    for (const auto& state : states) EXPECT_GT(source.GetComponent<scene::ParticleEmitter>(state.id)->runtime.particles.size(), 0u);
    Update(source, system);
    for (const auto& state : states) {
        const auto* emitter = source.GetComponent<scene::ParticleEmitter>(state.id);
        ASSERT_NE(emitter, nullptr);
        EXPECT_TRUE(emitter->runtime.particles.empty());
        EXPECT_TRUE(emitter->runtime.injectedSpawns.empty());
    }
    source.GetGameObject(owner)->SetActive(false);
    Update(source, system);
    for (const auto& state : states) EXPECT_EQ(source.GetGameObject(state.id), nullptr);
    EXPECT_EQ(scene::WaterInteractionSnapshot(*source.GetComponent<scene::WaterComponent>(owner)), nullptr);
}

TEST_F(WaterInteractionTest, AdvectedParticlesLeavingTheirWaterFootprintArePrunedWithoutCreatingAnotherEmitter)
{
    scene::Scene source;
    const auto owner = Water(source);
    auto impulse = Bubble();
    impulse.worldVelocity = {80, 0, 0};
    ASSERT_TRUE(scene::QueueWaterInteraction(source, owner, impulse));
    scene::WaterInteractionSystem system;
    Update(source, system);
    const auto state = source.GetComponent<scene::WaterComponent>(owner)->interactionRuntime.emitters[0];
    for (int frame = 0; frame < 8; ++frame) {
        ++Time::frameCount;
        scene::UpdateParticleCpuSimulation(source, m_world, 0.1f, (frame + 1) * 0.1f);
    }
    auto* emitter = source.GetComponent<scene::ParticleEmitter>(state.id);
    ASSERT_GT(emitter->runtime.particles.size(), 0u);
    EXPECT_GT(emitter->runtime.particles.front().position.x, 50);
    Update(source, system);
    emitter = source.GetComponent<scene::ParticleEmitter>(state.id);
    ASSERT_NE(emitter, nullptr);
    EXPECT_TRUE(emitter->runtime.particles.empty());
    EXPECT_EQ(source.GameObjectCount(), 2u);
}

TEST_F(WaterInteractionTest, SaveAndPassiveCopyKeepSettingsButOmitRuntimeAndGeneratedEmitters)
{
    scene::Scene source;
    const auto owner = Water(source);
    ASSERT_TRUE(scene::QueueWaterInteraction(source, owner, Bubble()));
    scene::WaterInteractionSystem system;
    Update(source, system);
    source.GetComponent<scene::WaterComponent>(owner)->interactionSettings.motionInterval = 0.75f;
    const auto text = scene::SceneSerializer::SaveToText(source);
    ASSERT_FALSE(text.empty());
    EXPECT_EQ(text.find("__WaterBubbles"), std::string::npos);
    EXPECT_EQ(text.find("interactionRuntime"), std::string::npos);
    std::string diagnostic;
    const auto passive = scene::SceneSerializer::CapturePassiveClone(source, diagnostic);
    auto restored = scene::SceneSerializer::LoadDataFromText(passive, "");
    ASSERT_NE(restored, nullptr) << diagnostic;
    auto* object = restored->Find("Water");
    ASSERT_NE(object, nullptr);
    const auto* water = object->GetComponent<scene::WaterComponent>();
    ASSERT_NE(water, nullptr);
    EXPECT_FLOAT_EQ(water->interactionSettings.motionInterval, 0.75f);
    EXPECT_TRUE(water->interactionRuntime.pending.empty());
    EXPECT_EQ(scene::WaterInteractionSnapshot(*water), nullptr);
    EXPECT_EQ(restored->GetEntities<scene::ParticleEmitter>().size(), 0u);
}

TEST_F(WaterInteractionTest, RectangleCarrierAdvectsPlumesAndOwnedParticlesWhileZeroFlowAndPauseRemainStill)
{
    scene::Scene source;
    const auto owner = Water(source);
    auto impulse = Bubble();
    impulse.worldVelocity = {};
    ASSERT_TRUE(scene::QueueWaterInteraction(source, owner, impulse));
    scene::WaterInteractionSystem system;
    Update(source, system);
    scene::UpdateParticleCpuSimulation(source, m_world, 0.1f, 0.1f);
    auto* water = source.GetComponent<scene::WaterComponent>(owner);
    const auto emitterId = water->interactionRuntime.emitters[0].id;
    Update(source, system, 0.2f);
    EXPECT_NEAR(scene::WaterInteractionSnapshot(*water)->plumes[0].worldPosition.x, 0, testkit::kTolerance);
    const auto before = source.GetComponent<scene::ParticleEmitter>(emitterId)->runtime.particles[0].position;
    water->current = {2, 0};
    Update(source, system, 0.2f, true, false);
    EXPECT_VEC3_NEAR(source.GetComponent<scene::ParticleEmitter>(emitterId)->runtime.particles[0].position, before, testkit::kTolerance);
    Update(source, system, 0.2f);
    EXPECT_NEAR(scene::WaterInteractionSnapshot(*water)->plumes[0].worldPosition.x, 0.4f, testkit::kTolerance);
    const auto after = source.GetComponent<scene::ParticleEmitter>(emitterId)->runtime.particles[0].position;
    EXPECT_NEAR(after.x - before.x, 0.4f, testkit::kTolerance);
}

TEST_F(WaterInteractionTest, SplineLocalFlowReplacesMaterialCurrentForPlumeAndParticleCarrier)
{
    scene::Scene source;
    const auto owner = Water(source);
    auto* water = source.GetComponent<scene::WaterComponent>(owner);
    water->surfaceShape = scene::WaterSurfaceShape::SPLINE_RIVER;
    water->splinePoints = {{1, {-10, 0, 0}, 10, 5, 2}, {2, {10, 0, 0}, 10, 5, 2}};
    water->current = {9, 9};
    scene::PrepareWaterSceneSurfaces(source);
    auto impulse = Bubble();
    impulse.worldVelocity = {};
    ASSERT_TRUE(scene::QueueWaterInteraction(source, owner, impulse));
    scene::WaterInteractionSystem system;
    Update(source, system);
    scene::UpdateParticleCpuSimulation(source, m_world, 0.1f, 0.1f);
    water = source.GetComponent<scene::WaterComponent>(owner);
    const auto emitterId = water->interactionRuntime.emitters[0].id;
    const auto before = source.GetComponent<scene::ParticleEmitter>(emitterId)->runtime.particles[0].position;
    Update(source, system, 0.2f);
    const auto frame = scene::WaterInteractionSnapshot(*water);
    ASSERT_EQ(frame->plumes.size(), 1u);
    EXPECT_NEAR(frame->plumes[0].worldPosition.x, 0.4f, 0.002f);
    EXPECT_NEAR(frame->plumes[0].worldPosition.z, 0, 0.002f);
    const auto after = source.GetComponent<scene::ParticleEmitter>(emitterId)->runtime.particles[0].position;
    EXPECT_NEAR(after.x - before.x, 0.4f, 0.002f);
    EXPECT_NEAR(after.z, before.z, 0.002f);
}

TEST_F(WaterInteractionTest, SceneFlowAdvectsPlumeAndRelaxesParticleVelocityOnceBesideTheWaterCarrier)
{
    scene::Scene source;
    const auto owner = Water(source);
    source.GetComponent<scene::WaterComponent>(owner)->current = {2, 0};
    scene::FlowFieldSettings uniform;
    uniform.fieldType = scene::FlowFieldType::Uniform;
    uniform.radius = 0;
    uniform.direction = {1, 0, 0};
    uniform.strength = 1;
    auto& field = source.CreateGameObject("Carrier field").AddComponent<scene::FlowField>();
    field.forces = {uniform};
    scene::FlushWorldTransforms(source);
    source.InvalidateFlowFrame();
    auto impulse = Bubble();
    impulse.worldVelocity = {};
    ASSERT_TRUE(scene::QueueWaterInteraction(source, owner, impulse));
    scene::WaterInteractionSystem system;
    Update(source, system);
    scene::UpdateParticleCpuSimulation(source, m_world, 0.1f, 0.1f);
    auto* water = source.GetComponent<scene::WaterComponent>(owner);
    const auto emitterId = water->interactionRuntime.emitters[0].id;
    auto* emitter = source.GetComponent<scene::ParticleEmitter>(emitterId);
    const auto before = emitter->runtime.particles[0];
    Update(source, system, 0.2f);
    EXPECT_NEAR(scene::WaterInteractionSnapshot(*water)->plumes[0].worldPosition.x, 0.6f, 0.002f);
    emitter = source.GetComponent<scene::ParticleEmitter>(emitterId);
    const auto carrierPosition = emitter->runtime.particles[0].position;
    EXPECT_NEAR(carrierPosition.x - before.position.x, 0.4f, 0.002f);
    const float expectedVelocity = before.velocity.x + (1.0f - before.velocity.x) * emitter->settings.flowCoupling * 0.1f;
    scene::UpdateParticleCpuSimulation(source, m_world, 0.1f, 0.2f);
    EXPECT_NEAR(emitter->runtime.particles[0].velocity.x, expectedVelocity, 0.002f);
    EXPECT_NEAR(emitter->runtime.particles[0].position.x, carrierPosition.x + expectedVelocity * 0.1f, 0.002f);
}
} /// @note namespace fbzz::tests
