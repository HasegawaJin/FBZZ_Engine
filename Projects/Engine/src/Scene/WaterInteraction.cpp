/// @file    WaterInteraction.cpp
/// @brief   Scene寿命と容量を守って接触演出を記録し、Cleanupで粒子と濁りを公開する。
/// @author  Hasegawa Jin
/// @date    2026-10-09
#include <Engine/Scene/WaterInteraction.hpp>
#include <Engine/Scene/Systems/WaterInteractionSystem.hpp>
#include <Engine/Core/Scheduler/SystemContext.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Scene/Components/ParticleEmitter.hpp>
#include <Engine/Scene/Components/RigidBodyComponent.hpp>
#include <Engine/Scene/ScriptComponent.hpp>
#include <Engine/Scene/Components/WaterComponent.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Engine/Scene/Systems/WaterSystem.hpp>
#include <Engine/Scene/Transform.hpp>
#include <Engine/Scene/WaterSurfaceQuery.hpp>
#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <utility>

namespace fbzz::scene {
void QueueWaterSplash(Scene& scene, EntityID owner, const math::Vector3& worldPos, float intensity)
{
    if (!std::isfinite(intensity)) return;
    const float strength = math::Clamp01(intensity);
    WaterInteractionImpulse impulse{WaterInteractionKind::SURFACE_FOAM, worldPos,
        {0.0f, 1.5f + strength * 3.0f, 0.0f}, 0.2f + strength * 0.3f, strength, 1.2f};
    impulse.emitSurfaceRipple = false;
    static_cast<void>(QueueWaterInteraction(scene, owner, impulse));
}

namespace {
uint64_t Key(EntityID id) { return (static_cast<uint64_t>(id.generation) << 32) | id.index; }
bool Finite(math::Vector3 value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}
bool Positive(float value) { return std::isfinite(value) && value > 0; }
bool ValidSettings(const WaterInteractionSettings& settings)
{
    return Positive(settings.minimumMotionSpeed) && Positive(settings.motionInterval)
        && Positive(settings.surfaceBand) && Positive(settings.sedimentMinimumSpeed)
        && Positive(settings.sedimentInterval) && Positive(settings.bubbleLifetime)
        && settings.bubbleLifetime <= 60 && Positive(settings.sedimentLifetime)
        && settings.sedimentLifetime <= 60 && Finite(settings.sedimentScattering)
        && settings.sedimentScattering.x >= 0 && settings.sedimentScattering.y >= 0
        && settings.sedimentScattering.z >= 0 && settings.sedimentScattering.x <= 1000
        && settings.sedimentScattering.y <= 1000 && settings.sedimentScattering.z <= 1000;
}
bool ValidImpulse(const WaterInteractionImpulse& impulse)
{
    const auto kind = static_cast<unsigned>(impulse.kind);
    const double speedSquared = static_cast<double>(impulse.worldVelocity.x) * impulse.worldVelocity.x
        + static_cast<double>(impulse.worldVelocity.y) * impulse.worldVelocity.y
        + static_cast<double>(impulse.worldVelocity.z) * impulse.worldVelocity.z;
    return kind <= static_cast<unsigned>(WaterInteractionKind::SEDIMENT)
        && Finite(impulse.worldPosition) && Finite(impulse.worldVelocity)
        && speedSquared <= 1000000.0 && Positive(impulse.radius) && impulse.radius <= 32
        && std::isfinite(impulse.strength) && impulse.strength > 0 && impulse.strength <= 1
        && Positive(impulse.lifetime) && impulse.lifetime <= 60;
}
struct OwnerEntry {
    EntityID id = EntityID::INVALID;
    uint64_t lifetime = 0;
    std::array<WaterInteractionEmitterState, 2> emitters{};
};
struct BodyEntry {
    EntityID id = EntityID::INVALID;
    uint64_t lifetime = 0;
    EntityID owner = EntityID::INVALID;
    uint64_t ownerLifetime = 0;
    uint64_t seenFrame = 0;
    float motionTimer = 0;
    float sedimentTimer = 0;
    math::Vector3 velocity{};
    math::Vector3 contactPoint{};
    math::Vector3 contactNormal{};
    uint64_t contactFrame = 0;
    float approachSpeed = 0;
    bool hasContact = false;
};
} /// @note namespace

struct WaterInteractionSceneStore {
    uint64_t generation = 0;
    uint64_t physicsStep = 0;
    uint64_t cleanupFrame = 0;
    double accumulatedDt = 0;
    bool invalidated = false;
    std::unordered_map<uint64_t, OwnerEntry> owners;
    std::unordered_map<uint64_t, BodyEntry> bodies;
    std::vector<OwnerEntry> retired;
};

namespace {
/// @note The index is weak; Water runtime and its active Cleanup System own the store, never the Scene or a Physics callback.
std::unordered_map<uint64_t, std::weak_ptr<WaterInteractionSceneStore>> s_stores;

std::shared_ptr<WaterInteractionSceneStore> ExistingStore(const Scene& scene)
{
    const auto found = s_stores.find(scene.GetRenderSceneGeneration());
    return found == s_stores.end() ? nullptr : found->second.lock();
}
std::shared_ptr<WaterInteractionSceneStore> AcquireStore(Scene& scene)
{
    auto store = ExistingStore(scene);
    if (store && !store->invalidated) return store;
    std::erase_if(s_stores, [](const auto& row) { return row.second.expired(); });
    auto created = std::make_shared<WaterInteractionSceneStore>();
    created->generation = scene.GetRenderSceneGeneration();
    if (store) {
        for (const auto& row : store->owners) created->retired.push_back(row.second);
        for (const auto& row : store->retired) created->retired.push_back(row);
    }
    s_stores[created->generation] = created;
    return created;
}
WaterComponent* OwnerWater(Scene& scene, EntityID& id)
{
    WaterComponent* water = scene.GetComponent<WaterComponent>(id);
    GameObject* object = scene.GetGameObject(id);
    if (!water || !object || !water->enabled || !object->activeInHierarchy()) return nullptr;
    const EntityID owner = WaterSurfaceZoneOwner(*water, WaterSurfaceReadView::SIMULATION);
    if (owner.IsValid()) {
        id = owner;
        water = scene.GetComponent<WaterComponent>(id);
        object = scene.GetGameObject(id);
    }
    return water && object && water->enabled && object->activeInHierarchy() ? water : nullptr;
}
bool Allowed(Scene& scene, EntityID owner, const WaterInteractionImpulse& impulse)
{
    const GameObject* object = scene.GetGameObject(owner);
    const WaterComponent* water = scene.GetComponent<WaterComponent>(owner);
    if (!object || !water) return false;
    const auto& pose = WaterSurfaceTransform(*water, object->transform, WaterSurfaceReadView::SIMULATION);
    WaterSurfaceHeightSample sample;
    if (!TrySampleWaterSurfaceHeightWorld(*water, pose, impulse.worldPosition, Time::time,
            sample, WaterSurfaceReadView::SIMULATION) || !sample.insideSurface || sample.excluded) return false;
    return impulse.kind == WaterInteractionKind::SURFACE_FOAM
        ? std::abs(sample.depth) <= water->interactionSettings.surfaceBand : sample.underwater;
}
bool KindEnabled(const WaterInteractionSettings& settings, WaterInteractionKind kind)
{
    return kind == WaterInteractionKind::SURFACE_FOAM ? settings.foamEnabled
        : kind == WaterInteractionKind::BUBBLES ? settings.bubblesEnabled : settings.sedimentEnabled;
}
WaterInteractionRuntime* Runtime(Scene& scene, EntityID owner, WaterComponent& water)
{
    auto store = AcquireStore(scene);
    const uint64_t lifetime = scene.GetComponentLifetime<WaterComponent>(owner);
    auto& runtime = water.interactionRuntime;
    if (runtime.sceneGeneration != store->generation || runtime.owner != owner
        || runtime.ownerLifetime != lifetime || runtime.sceneStore != store) {
        runtime = {};
        runtime.sceneGeneration = store->generation;
        runtime.owner = owner;
        runtime.ownerLifetime = lifetime;
        runtime.sceneStore = store;
        runtime.pending.reserve(WATER_INTERACTION_MAX_PENDING);
        runtime.active.reserve(WATER_INTERACTION_MAX_ACTIVE);
    }
    const auto found = store->owners.find(Key(owner));
    if (found == store->owners.end()) {
        if (store->owners.size() >= WATER_INTERACTION_MAX_OWNERS) return nullptr;
        store->owners.emplace(Key(owner), OwnerEntry{owner, lifetime, {}});
    } else if (found->second.lifetime != lifetime) {
        store->retired.push_back(found->second);
        found->second = {owner, lifetime, {}};
    }
    return &runtime;
}
bool OwnedEmitter(Scene& scene, const WaterInteractionEmitterState& state)
{
    const GameObject* object = scene.GetGameObject(state.id);
    return object && object->runtimeGenerated && !object->GetParent()
        && object->GetChildCount() == 0 && !scene.GetComponent<ScriptComponent>(state.id)
        && scene.GetComponentLifetime<ParticleEmitter>(state.id) == state.lifetime;
}
void Retire(Scene& scene, const OwnerEntry& owner)
{
    for (const auto& emitter : owner.emitters)
        if (OwnedEmitter(scene, emitter)) scene.DestroyGameObject(emitter.id);
}
ParticleEmitter* EnsureEmitter(Scene& scene, EntityID owner, size_t index)
{
    WaterComponent* water = scene.GetComponent<WaterComponent>(owner);
    if (!water) return nullptr;
    auto state = water->interactionRuntime.emitters[index];
    if (OwnedEmitter(scene, state)) {
        ParticleEmitter* emitter = scene.GetComponent<ParticleEmitter>(state.id);
        const auto& path = index == 0 ? water->interactionSettings.bubbleMaterialPath : water->interactionSettings.sedimentMaterialPath;
        emitter->settings.materialPath = path.empty() ? (index == 0 ? WATER_BUBBLE_MATERIAL : WATER_SEDIMENT_MATERIAL) : path;
        emitter->settings.lifetime = index == 0 ? water->interactionSettings.bubbleLifetime : water->interactionSettings.sedimentLifetime;
        return emitter;
    }
    GameObject* created = scene.TryCreateGameObject(index == 0 ? "__WaterBubbles" : "__WaterSediment");
    if (!created) return nullptr;
    const EntityID id = created->GetID();
    created->runtimeGenerated = true;
    created->AddComponent<ParticleEmitter>();
    water = scene.GetComponent<WaterComponent>(owner);
    if (!water) { scene.DestroyGameObject(id); return nullptr; }
    ParticleEmitter* emitter = scene.GetComponent<ParticleEmitter>(id);
    emitter->settings.enabled = true;
    emitter->settings.playing = true;
    emitter->settings.emitRate = 0;
    emitter->settings.maxParticles = static_cast<int>(WATER_INTERACTION_MAX_PARTICLES);
    emitter->settings.simulationMode = ParticleSimulationMode::Cpu;
    emitter->settings.simulationSpace = ParticleSimulationSpace::World;
    emitter->settings.receiveFlowFields = true;
    emitter->settings.emitVelocity = {};
    emitter->settings.velocitySpread = index == 0 ? 0.08f : 0.15f;
    emitter->settings.gravity = index == 0 ? math::Vector3{0, 0.12f, 0} : math::Vector3{};
    emitter->settings.subEmitterInheritVelocity = 1;
    emitter->settings.randomSeed = static_cast<uint32_t>(Key(owner)) ^ (index == 0 ? 0xA341316Cu : 0xC8013EA4u);
    emitter->settings.lifetime = index == 0 ? water->interactionSettings.bubbleLifetime : water->interactionSettings.sedimentLifetime;
    emitter->settings.sizeStart = index == 0 ? 0.08f : 0.2f;
    emitter->settings.sizeEnd = index == 0 ? 0.02f : 0.6f;
    emitter->settings.colorStart = index == 0 ? math::Vector4{0.8f, 0.94f, 1, 0.7f} : math::Vector4{0.5f, 0.35f, 0.17f, 0.25f};
    emitter->settings.colorEnd = index == 0 ? math::Vector4{0.8f, 0.94f, 1, 0} : math::Vector4{0.5f, 0.35f, 0.17f, 0};
    const auto& materialPath = index == 0 ? water->interactionSettings.bubbleMaterialPath : water->interactionSettings.sedimentMaterialPath;
    emitter->settings.materialPath = materialPath.empty()
        ? (index == 0 ? WATER_BUBBLE_MATERIAL : WATER_SEDIMENT_MATERIAL) : materialPath;
    emitter->ResetPlayback();
    state = {id, scene.GetComponentLifetime<ParticleEmitter>(id)};
    water->interactionRuntime.emitters[index] = state;
    water->interactionRuntime.sceneStore->owners[Key(owner)].emitters[index] = state;
    return emitter;
}
void Publish(WaterInteractionRuntime& runtime)
{
    auto frame = std::make_shared<WaterInteractionFrame>();
    frame->sceneGeneration = runtime.sceneGeneration;
    frame->owner = runtime.owner;
    frame->ownerLifetime = runtime.ownerLifetime;
    frame->revision = ++runtime.revision;
    frame->plumes = runtime.active;
    runtime.snapshot = std::move(frame);
}

/// @note The mixed foam/bubble emitter may occupy the surface band; sediment remains inside the physical water volume.
void PruneParticles(Scene& scene, EntityID owner, bool sediment, float dt,
    const WaterInteractionEmitterState& emitterState)
{
    if (!OwnedEmitter(scene, emitterState)) return;
    const auto* water = scene.GetComponent<WaterComponent>(owner);
    const auto* object = scene.GetGameObject(owner);
    auto* emitter = scene.GetComponent<ParticleEmitter>(emitterState.id);
    if (!water || !object || !emitter) return;
    const auto& pose = WaterSurfaceTransform(*water, object->transform, WaterSurfaceReadView::SIMULATION);
    const std::vector<ActiveFlowField> noSceneFields;
    const auto allowed = [&](math::Vector3 position) {
        if (!Finite(position)) return false;
        WaterSurfaceHeightSample sample;
        return TrySampleWaterSurfaceHeightWorld(*water, pose, position, Time::time,
                sample, WaterSurfaceReadView::SIMULATION)
            && sample.insideSurface && !sample.excluded
            && (sample.underwater || (!sediment && std::abs(sample.depth) <= water->interactionSettings.surfaceBand));
    };
    std::erase_if(emitter->runtime.particles, [&](auto& particle) {
        if (!allowed(particle.position)) return true;
        WaterSurfaceSample sample;
        if (!TrySampleWaterSurfaceWorld(*water, pose, noSceneFields, particle.position, Time::time,
                sample, WaterSurfaceReadView::SIMULATION)) return true;
        /// @note Scene fields act once through ParticleSimulator's receiveFlowFields; only the Water/Spline carrier translates positions here.
        particle.position = particle.position + sample.flowVelocity * dt;
        return !allowed(particle.position);
    });
    std::erase_if(emitter->runtime.injectedSpawns, [&](const auto& spawn) { return !allowed(spawn.position); });
}
} /// @note namespace

void WaterInteractionSettings::Reflect(IReflector& reflector)
{
    reflector.Field("enabled", enabled);
    reflector.Field("bubblesEnabled", bubblesEnabled);
    reflector.Field("foamEnabled", foamEnabled);
    reflector.Field("sedimentEnabled", sedimentEnabled);
    reflector.FloatRange("minimumMotionSpeed", minimumMotionSpeed, 0.01f, 100.0f);
    reflector.FloatRange("motionInterval", motionInterval, 0.01f, 10.0f);
    reflector.FloatRange("surfaceBand", surfaceBand, 0.01f, 10.0f);
    reflector.FloatRange("sedimentMinimumSpeed", sedimentMinimumSpeed, 0.01f, 100.0f);
    reflector.FloatRange("sedimentInterval", sedimentInterval, 0.01f, 10.0f);
    reflector.FloatRange("bubbleLifetime", bubbleLifetime, 0.01f, 60.0f);
    reflector.FloatRange("sedimentLifetime", sedimentLifetime, 0.01f, 60.0f);
    reflector.Field("sedimentScattering", sedimentScattering);
    reflector.FileField("bubbleMaterialPath", bubbleMaterialPath, ".mat");
    reflector.FileField("sedimentMaterialPath", sedimentMaterialPath, ".mat");
}

bool QueueWaterInteraction(Scene& scene, EntityID owner, const WaterInteractionImpulse& impulse)
{
    if (!ValidImpulse(impulse)) return false;
    WaterComponent* water = OwnerWater(scene, owner);
    if (!water || !water->interactionSettings.enabled || !ValidSettings(water->interactionSettings)
        || !KindEnabled(water->interactionSettings, impulse.kind) || !Allowed(scene, owner, impulse)) return false;
    auto* runtime = Runtime(scene, owner, *water);
    if (!runtime || runtime->pending.size() >= WATER_INTERACTION_MAX_PENDING) return false;
    runtime->pending.push_back(impulse);
    return true;
}

bool QueueWaterSurfaceInteraction(Scene& scene, EntityID owner, math::Vector3 worldPosition, float strength)
{
    if (!Finite(worldPosition) || !std::isfinite(strength) || strength <= 0 || strength > 1) return false;
    WaterComponent* water = OwnerWater(scene, owner);
    const GameObject* object = scene.GetGameObject(owner);
    if (!water || !object) return false;
    const auto& pose = WaterSurfaceTransform(*water, object->transform, WaterSurfaceReadView::SIMULATION);
    WaterSurfaceHeightSample sample;
    if (!TrySampleWaterSurfaceHeightWorld(*water, pose, worldPosition, Time::time,
            sample, WaterSurfaceReadView::SIMULATION) || !sample.insideSurface || sample.excluded) return false;
    return QueueWaterInteraction(scene, owner, {WaterInteractionKind::SURFACE_FOAM,
        sample.position, {}, 0.3f, strength, water->interactionSettings.bubbleLifetime, false});
}

void BeginWaterInteractionPhysicsStep(Scene& scene, float fixedDt)
{
    if (!Positive(fixedDt)) return;
    auto store = ExistingStore(scene);
    if (!store || store->invalidated) return;
    ++store->physicsStep;
    store->accumulatedDt += fixedDt;
}

void SupplyWaterMotionImpulse(Scene& scene, EntityID owner, EntityID body,
    math::Vector3 worldPosition, math::Vector3 worldVelocity, float dt, bool nearSurface, bool emitSurfaceRipple)
{
    if (!Positive(dt) || !Finite(worldPosition) || !Finite(worldVelocity)) return;
    WaterComponent* water = OwnerWater(scene, owner);
    if (!water || !water->interactionSettings.enabled || !ValidSettings(water->interactionSettings)) return;
    const uint64_t bodyLifetime = scene.GetComponentLifetime<RigidBodyComponent>(body);
    if (!bodyLifetime) return;
    auto* runtime = Runtime(scene, owner, *water);
    if (!runtime) return;
    auto& store = *runtime->sceneStore;
    auto found = store.bodies.find(Key(body));
    if (found == store.bodies.end()) {
        if (store.bodies.size() >= WATER_INTERACTION_MAX_TRACKED_BODIES) return;
        found = store.bodies.emplace(Key(body), BodyEntry{}).first;
    }
    auto& tracked = found->second;
    if (tracked.lifetime != bodyLifetime || tracked.owner != owner || tracked.ownerLifetime != runtime->ownerLifetime)
        tracked = BodyEntry{body, bodyLifetime, owner, runtime->ownerLifetime};
    tracked.seenFrame = store.cleanupFrame;
    tracked.velocity = worldVelocity;
    tracked.motionTimer = (std::max)(tracked.motionTimer - dt, 0.0f);
    tracked.sedimentTimer = (std::max)(tracked.sedimentTimer - dt, 0.0f);
    const double speedSquared = static_cast<double>(worldVelocity.x) * worldVelocity.x
        + static_cast<double>(worldVelocity.y) * worldVelocity.y + static_cast<double>(worldVelocity.z) * worldVelocity.z;
    if (speedSquared < static_cast<double>(water->interactionSettings.minimumMotionSpeed)
            * water->interactionSettings.minimumMotionSpeed || tracked.motionTimer > 0) return;
    const float strength = (std::min)(static_cast<float>(std::sqrt(speedSquared) / 12.0), 0.5f);
    WaterInteractionImpulse impulse{nearSurface ? WaterInteractionKind::SURFACE_FOAM : WaterInteractionKind::BUBBLES,
        worldPosition, worldVelocity * 0.1f, nearSurface ? 0.3f : 0.12f, strength,
        water->interactionSettings.bubbleLifetime, emitSurfaceRipple};
    if (QueueWaterInteraction(scene, owner, impulse)) tracked.motionTimer = water->interactionSettings.motionInterval;
}

void SupplyWaterGroundContact(Scene& scene, EntityID body, math::Vector3 worldPoint, math::Vector3 worldNormal,
    float approachSpeed)
{
    if (!Finite(worldPoint) || !Finite(worldNormal) || !std::isfinite(approachSpeed) || approachSpeed < 0) return;
    const double lengthSquared = static_cast<double>(worldNormal.x) * worldNormal.x
        + static_cast<double>(worldNormal.y) * worldNormal.y + static_cast<double>(worldNormal.z) * worldNormal.z;
    if (lengthSquared <= 1.0e-12 || worldNormal.y / std::sqrt(lengthSquared) < 0.5) return;
    const auto store = ExistingStore(scene);
    if (!store || store->invalidated) return;
    const auto found = store->bodies.find(Key(body));
    if (found == store->bodies.end()) return;
    auto& tracked = found->second;
    if (tracked.lifetime != scene.GetComponentLifetime<RigidBodyComponent>(body)
        || tracked.seenFrame + 1 < store->cleanupFrame) return;
    tracked.contactPoint = worldPoint;
    tracked.contactNormal = worldNormal;
    tracked.contactFrame = store->cleanupFrame;
    tracked.approachSpeed = (std::max)(tracked.approachSpeed, approachSpeed);
    tracked.hasContact = true;
}

std::shared_ptr<const WaterInteractionFrame> WaterInteractionSnapshot(const WaterComponent& water)
{
    const auto& runtime = water.interactionRuntime;
    return runtime.sceneStore && !runtime.sceneStore->invalidated ? runtime.snapshot : nullptr;
}

void ResetWaterInteractionRuntime(WaterComponent& water) { water.interactionRuntime = {}; }

void ClearWaterInteractions(Scene& scene)
{
    auto store = ExistingStore(scene);
    if (!store) return;
    for (const auto& row : store->owners) {
        Retire(scene, row.second);
        if (auto* water = scene.GetComponent<WaterComponent>(row.second.id))
            if (scene.GetComponentLifetime<WaterComponent>(row.second.id) == row.second.lifetime)
                ResetWaterInteractionRuntime(*water);
    }
    for (const auto& row : store->retired) Retire(scene, row);
    store->owners.clear();
    store->bodies.clear();
    store->retired.clear();
    store->invalidated = true;
    s_stores.erase(store->generation);
}

void WaterInteractionSystem::Update(SystemContext& context)
{
    auto& scene = context.scene;
    if (m_store && m_store->generation != scene.GetRenderSceneGeneration()) m_store->invalidated = true;
    m_store = ExistingStore(scene);
    if (!m_store) return;
    if (!context.playing) { ClearWaterInteractions(scene); m_store.reset(); return; }
    if (m_store->invalidated) m_store = AcquireStore(scene);
    const auto store = m_store;
    for (const auto& row : store->retired) Retire(scene, row);
    store->retired.clear();
    std::vector<OwnerEntry> owners;
    owners.reserve(store->owners.size());
    for (const auto& row : store->owners) owners.push_back(row.second);
    std::sort(owners.begin(), owners.end(), [](const auto& a, const auto& b) { return Key(a.id) < Key(b.id); });
    if (!context.simulating || !Positive(context.dt)) {
        for (const auto& owner : owners) {
            auto* water = scene.GetComponent<WaterComponent>(owner.id);
            const auto* object = scene.GetGameObject(owner.id);
            if (water && object && scene.GetComponentLifetime<WaterComponent>(owner.id) == owner.lifetime
                && water->enabled && object->activeInHierarchy() && water->interactionSettings.enabled
                && ValidSettings(water->interactionSettings)) continue;
            Retire(scene, owner);
            if (water && scene.GetComponentLifetime<WaterComponent>(owner.id) == owner.lifetime)
                ResetWaterInteractionRuntime(*water);
            store->owners.erase(Key(owner.id));
        }
        return;
    }
    for (auto& row : store->bodies) {
        auto& body = row.second;
        if (!body.hasContact) continue;
        body.hasContact = false;
        const float approachSpeed = body.approachSpeed;
        body.approachSpeed = 0;
        const GameObject* bodyObject = scene.GetGameObject(body.id);
        if (body.contactFrame != store->cleanupFrame || body.sedimentTimer > 0
            || !bodyObject || !bodyObject->activeInHierarchy()
            || body.lifetime != scene.GetComponentLifetime<RigidBodyComponent>(body.id)) continue;
        WaterComponent* water = scene.GetComponent<WaterComponent>(body.owner);
        if (!water || body.ownerLifetime != scene.GetComponentLifetime<WaterComponent>(body.owner)) continue;
        const double tangentSpeedSquared = static_cast<double>(body.velocity.x) * body.velocity.x
            + static_cast<double>(body.velocity.z) * body.velocity.z;
        if (tangentSpeedSquared >= static_cast<double>(water->interactionSettings.sedimentMinimumSpeed)
                * water->interactionSettings.sedimentMinimumSpeed
            || approachSpeed >= water->interactionSettings.sedimentMinimumSpeed) {
            WaterInteractionImpulse impulse{WaterInteractionKind::SEDIMENT, body.contactPoint,
                body.velocity * 0.05f, 0.4f, 0.35f, water->interactionSettings.sedimentLifetime};
            if (QueueWaterInteraction(scene, body.owner, impulse))
                body.sedimentTimer = water->interactionSettings.sedimentInterval;
        }
    }
    const float dt = static_cast<float>((std::min)(store->accumulatedDt > 0 ? store->accumulatedDt : context.dt, 60.0));
    const auto flowFields = scene.FlowFrame().fields;
    store->accumulatedDt = 0;
    for (const OwnerEntry& owner : owners) {
        WaterComponent* water = scene.GetComponent<WaterComponent>(owner.id);
        GameObject* object = scene.GetGameObject(owner.id);
        if (!water || !object || scene.GetComponentLifetime<WaterComponent>(owner.id) != owner.lifetime) {
            Retire(scene, owner);
            store->owners.erase(Key(owner.id));
            continue;
        }
        if (!water->enabled || !object->activeInHierarchy() || !water->interactionSettings.enabled
            || !ValidSettings(water->interactionSettings)) {
            Retire(scene, owner);
            ResetWaterInteractionRuntime(*water);
            store->owners.erase(Key(owner.id));
            continue;
        }
        Runtime(scene, owner.id, *water);
        for (size_t index = 0; index < water->interactionRuntime.emitters.size(); ++index)
            PruneParticles(scene, owner.id, index == 1u, dt, water->interactionRuntime.emitters[index]);
        bool changed = false;
        std::vector<WaterInteractionImpulse> pending;
        {
            auto& runtime = water->interactionRuntime;
            changed = !runtime.active.empty() || !runtime.pending.empty() || !runtime.snapshot;
            for (auto& plume : runtime.active) {
                plume.age += dt;
                WaterSurfaceSample sample;
                const auto& pose = WaterSurfaceTransform(*water, object->transform, WaterSurfaceReadView::SIMULATION);
                if (!TrySampleWaterSurfaceWorld(*water, pose, *flowFields, plume.worldPosition, Time::time,
                        sample, WaterSurfaceReadView::SIMULATION) || !sample.height.insideSurface || sample.height.excluded) {
                    plume.age = plume.lifetime;
                    continue;
                }
                plume.worldPosition = plume.worldPosition + (plume.worldVelocity + sample.flowVelocity) * dt;
                plume.density *= std::exp(-dt * 3.0f / plume.lifetime);
                if (!Finite(plume.worldPosition)) plume.age = plume.lifetime;
            }
            std::erase_if(runtime.active, [](const auto& plume) { return plume.age >= plume.lifetime || plume.density < 0.001f; });
            pending = std::move(runtime.pending);
            runtime.pending.clear();
            runtime.pending.reserve(WATER_INTERACTION_MAX_PENDING);
        }
        for (const auto& impulse : pending) {
            water = scene.GetComponent<WaterComponent>(owner.id);
            object = scene.GetGameObject(owner.id);
            if (!water || !object || scene.GetComponentLifetime<WaterComponent>(owner.id) != owner.lifetime) break;
            if (!Allowed(scene, owner.id, impulse)) continue;
            const bool sediment = impulse.kind == WaterInteractionKind::SEDIMENT;
            {
                auto& active = water->interactionRuntime.active;
                if (active.size() >= WATER_INTERACTION_MAX_ACTIVE) active.erase(active.begin());
                active.push_back({impulse.kind, impulse.worldPosition, impulse.worldVelocity,
                    sediment ? water->interactionSettings.sedimentScattering : math::Vector3{},
                    impulse.radius, impulse.strength, 0, impulse.lifetime});
            }
            if (impulse.kind == WaterInteractionKind::SURFACE_FOAM && impulse.emitSurfaceRipple)
                EmitWaterRipple(*water, object->transform, impulse.worldPosition, impulse.strength);
            ParticleEmitter* emitter = EnsureEmitter(scene, owner.id, sediment ? 1u : 0u);
            water = scene.GetComponent<WaterComponent>(owner.id);
            if (!emitter || !water) continue;
            const size_t occupied = emitter->runtime.particles.size();
            size_t queued = 0;
            for (const auto& spawn : emitter->runtime.injectedSpawns) queued += static_cast<size_t>(spawn.count);
            if (occupied + queued >= WATER_INTERACTION_MAX_PARTICLES) continue;
            const int count = (std::min)(static_cast<int>(WATER_INTERACTION_MAX_PARTICLES - occupied - queued),
                static_cast<int>(2 + impulse.strength * 8));
            const auto velocity = impulse.worldVelocity
                + (sediment || impulse.kind == WaterInteractionKind::SURFACE_FOAM ? math::Vector3{} : math::Vector3{0, 0.4f, 0});
            emitter->runtime.injectedSpawns.push_back({impulse.worldPosition, velocity, count, impulse.lifetime});
        }
        water = scene.GetComponent<WaterComponent>(owner.id);
        if (water && changed) Publish(water->interactionRuntime);
    }
    ++store->cleanupFrame;
    std::erase_if(store->bodies, [&](const auto& row) {
        const auto& body = row.second;
        return body.seenFrame + 1 < store->cleanupFrame
            || scene.GetComponentLifetime<RigidBodyComponent>(body.id) != body.lifetime
            || !store->owners.contains(Key(body.owner));
    });
}
} /// @note namespace fbzz::scene
