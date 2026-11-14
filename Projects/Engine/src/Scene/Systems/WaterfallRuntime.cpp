/// @file    WaterfallRuntime.cpp
/// @brief   Connected waterfall geometry, pooled effects and shared waterline constraints.
/// @author  Hasegawa Jin
/// @date    2026-11-11
#include "WaterfallRuntime.hpp"
#include "WaterfallFluidRuntime.hpp"
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Components/ParticleEmitter.hpp>
#include <Engine/Scene/Components/ProceduralMeshComponent.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Engine/Scene/Components/WaterfallComponent.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Engine/Scene/Systems/WaterSystem.hpp>
#include <Engine/Scene/TerrainSurfaceQuery.hpp>
#include <Engine/Scene/WaterSurfaceQuery.hpp>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>

namespace fbzz::scene {

namespace {
using math::Vector3;

constexpr float FALL_GRAVITY = 9.81f;
constexpr char WATERFALL_SURFACE[] = "guid:4d940c1f438a4b968274ef36d2da9598|Assets/Materials/Water/WaterfallSurface.mat";
constexpr char WATERFALL_MIST[] = "guid:a981ccf072514ac0bcb4f728ef106d30|Assets/Materials/Water/WaterfallVolumeMist.mat";
constexpr char WATERFALL_SPRAY[] = "guid:49a93eeef615529c94bf8b1ce6cc766e|Assets/Materials/Water/WaterfallMist.mat";
constexpr std::array PART_NAMES{"Waterfall Surface", "Impact Spray", "Falling Spray", "Lip Spray", "Waterfall Mist"};

Vector3 Midpoint(const Vector3& left, const Vector3& right)
{
    return left + (right - left) * 0.5f;
}

Vector3 Horizontal(Vector3 value)
{
    value.y = 0.0f;
    return value;
}

bool Finite(const Vector3& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool Finite(const WaterfallSection& section)
{
    return Finite(section.left) && Finite(section.right) && Finite(section.insideLeft) && Finite(section.insideRight)
        && Finite(section.velocity) && std::isfinite(section.depth) && section.depth >= 0.0f;
}

float FiniteAmount(float value)
{
    return std::isfinite(value) ? std::clamp(value, 0.0f, 2.0f) : 1.0f;
}

Vector3 Center(const WaterfallSection& section) { return Midpoint(section.left, section.right); }
Vector3 InnerCenter(const WaterfallSection& section) { return Midpoint(section.insideLeft, section.insideRight); }
} /// @note namespace

bool GetWaterfallRiverSection(const WaterSplineSnapshot& snapshot, bool atStart, bool incoming, WaterfallSection& out)
{
    const auto& outline = snapshot.outlineIndices;
    if (snapshot.shape != WaterSurfaceShape::SPLINE_RIVER || outline.size() < 4 || outline.size() % 2 != 0) return false;
    const std::size_t count = outline.size() / 2;
    const std::size_t end = atStart ? 0 : count - 1;
    const std::size_t inside = atStart ? 1 : count - 2;
    const bool reverse = atStart == incoming;
    const auto vertex = [&](std::size_t index, bool left) -> const WaterSplineVertex& {
        return snapshot.vertices[outline[left != reverse ? index : outline.size() - 1 - index]];
    };
    const auto& left = vertex(end, true);
    const auto& right = vertex(end, false);
    const float width = (right.worldPosition - left.worldPosition).Length();
    const float reach = (vertex(inside, true).worldPosition - left.worldPosition).Length();
    if (!std::isfinite(width) || width < 0.02f || !std::isfinite(reach) || reach < 0.001f) return false;
    const float blend = (std::min)(0.85f, (std::max)(0.25f, width * (incoming ? 0.18f : 0.65f)) / reach);
    out = {left.worldPosition, right.worldPosition,
        Vector3::Lerp(left.worldPosition, vertex(inside, true).worldPosition, blend),
        Vector3::Lerp(right.worldPosition, vertex(inside, false).worldPosition, blend),
        Midpoint(left.worldVelocity, right.worldVelocity), std::lerp(left.depth, right.depth, 0.5f)};
    return true;
}

bool BuildWaterfallGeometry(const WaterfallGeometry& geometry,
    const std::function<float(const Vector3&)>& ground, MeshBuilder& out, Vector3* midFall, std::uint32_t* farIndexStart)
{
    const auto& source = geometry.source;
    const auto& receiver = geometry.receiver;
    if (!Finite(source) || !Finite(receiver)) return false;
    const Vector3 start = Center(source), end = Center(receiver);
    const Vector3 delta = Horizontal(end - start);
    const float height = start.y - end.y, run = delta.Length();
    const float width = (source.right - source.left).Length();
    const float endWidth = (receiver.right - receiver.left).Length();
    if (!Finite(start) || !Finite(end) || !std::isfinite(width) || !std::isfinite(endWidth)
        || height < 0.3f || height > 2000.0f || run > 2000.0f || width < 0.02f || endWidth < 0.02f
        || width > 2000.0f || endWidth > 2000.0f) return false;
    const Vector3 forward = delta.NormalizedOr(Horizontal(start - InnerCenter(source)).NormalizedOr(Vector3::FORWARD));
    const auto along = [&](const Vector3& tangent) {
        const auto heading = Horizontal(tangent).NormalizedOr(forward);
        return Vector3::Dot(heading, forward) > 0.05f ? heading : forward;
    };
    const float entrySpeed = std::clamp(source.velocity.Length(), 0.35f, 35.0f);
    const float travel = std::sqrt(2.0f * height / FALL_GRAVITY);
    const float launch = std::clamp(entrySpeed * travel, run * 0.55f, run * 1.45f);
    const Vector3 sourceTangent = along(start - InnerCenter(source)) * launch;
    const Vector3 receiverTangent = along(InnerCenter(receiver) - end) * (2.0f * run - launch);
    const auto sourceAxis = (source.right - source.left).NormalizedOr(Vector3::RIGHT);
    const auto receiverAxis = (receiver.right - receiver.left).NormalizedOr(sourceAxis);
    if (Vector3::Dot(sourceAxis, receiverAxis) < -0.25f) return false;
    const int rings = std::clamp(static_cast<int>(std::ceil(std::sqrt(height + run) * 2.2f)), 14, 56);
    const int sides = std::clamp(static_cast<int>(std::ceil(std::sqrt(width) * 2.0f)), 8, 20);
    const int clearanceSamples = std::clamp(static_cast<int>(std::ceil((std::max)(width, endWidth) * 2.0f)), 8, 48);
    const float breakup = FiniteAmount(geometry.breakup);
    const float thickness = std::clamp(source.depth * 0.55f, 0.025f, (std::max)(0.035f, width * 0.22f));
    const float animationAmplitude = geometry.rockAttached ? 0.025f
        : std::clamp(thickness * 0.24f, 0.035f, WATERFALL_VERTEX_MARGIN * 0.5f);
    MeshBuilder candidate;
    candidate.Reserve(static_cast<std::size_t>((rings + 3) * (sides + 1) + 4),
        static_cast<std::size_t>((rings + 2) * sides * 6 + 6));
    Vector3 middle = Midpoint(start, end);
    float previousY = start.y;
    /// @see https://doi.org/10.1061/(ASCE)HY.1943-7900.0001188 Ma et al., growth of disturbances in a falling water jet.
    for (int ring = -1; ring <= rings + 1; ++ring) {
        const float t = std::clamp(static_cast<float>(ring) / static_cast<float>(rings), 0.0f, 1.0f);
        const float blend = t * t * (3.0f - 2.0f * t);
        const Vector3 axis = Vector3::Lerp(sourceAxis, receiverAxis, blend).NormalizedOr(sourceAxis);
        const float halfWidth = std::lerp(width, endWidth, blend) * 0.5f;
        const float remaining = 1.0f - t;
        Vector3 center = start + delta * blend + sourceTangent * (t * remaining * remaining)
            - receiverTangent * (t * t * remaining);
        center.y = start.y - height * (geometry.rockAttached ? t : t * t);
        if (ring < 0) center = InnerCenter(source);
        if (ring == rings) center = end;
        if (ring > rings) center = InnerCenter(receiver);
        const float envelope = 4.0f * t * (1.0f - t);
        const auto horizontalTangent = delta * (6.0f * t * remaining)
            + sourceTangent * (remaining * (1.0f - 3.0f * t)) + receiverTangent * (t * (3.0f * t - 2.0f));
        const Vector3 sectionNormal = Vector3::Cross(axis, horizontalTangent
            - Vector3::UP * (height * (std::max)(2.0f * t, 0.05f))).NormalizedOr(-forward);
        const float radius = thickness * envelope * std::lerp(1.0f, 0.54f, t);
        if (ground && ring > 0 && ring < rings) {
            float lift = -std::numeric_limits<float>::infinity();
            for (int sample = 0; sample <= clearanceSamples; ++sample) {
                const float across = 2.0f * static_cast<float>(sample) / static_cast<float>(clearanceSamples) - 1.0f;
                const Vector3 lower = center + axis * (across * halfWidth)
                    - sectionNormal * (radius * std::sqrt((std::max)(0.0f, 1.0f - across * across)));
                const Vector3 upper = lower + sectionNormal * (2.0f * radius
                    * std::sqrt((std::max)(0.0f, 1.0f - across * across)));
                lift = (std::max)({lift, ground(lower) - lower.y, ground(upper) - upper.y});
            }
            /// @note A shallow intersection at the lip can be submerged by the upper river band.
            const float clearance = std::lerp(0.28f, 0.10f, envelope);
            if (lift > clearance && !geometry.rockAttached) return false;
            if (geometry.rockAttached && std::isfinite(lift))
                center.y += (std::max)(lift + 0.025f, -(std::min)(height * 0.08f, 1.0f) * envelope);
            if (center.y > previousY + clearance) return false;
            previousY = center.y;
        }
        if (ring == rings / 2) middle = center;
        for (int side = 0; side <= sides; ++side) {
            const float angle = 2.0f * std::numbers::pi_v<float> * static_cast<float>(side) / static_cast<float>(sides);
            const float across = std::cos(angle), around = std::sin(angle);
            const float u = (across + 1.0f) * 0.5f;
            Vector3 position = center + axis * (across * halfWidth) + sectionNormal * (around * radius);
            if (ring <= 0) position = ring < 0 ? Vector3::Lerp(source.insideLeft, source.insideRight, u)
                : Vector3::Lerp(source.left, source.right, u);
            if (ring >= rings) position = ring > rings ? Vector3::Lerp(receiver.insideLeft, receiver.insideRight, u)
                : Vector3::Lerp(receiver.left, receiver.right, u);
            const auto normal = (axis * across + sectionNormal * around).NormalizedOr(sectionNormal);
            const float band = ring < 0 || ring > rings ? 0.0f : 1.0f;
            const auto index = candidate.AddVertex(position - start, normal, {u, t}, {travel * t, breakup, band, 0.0f});
            candidate.Vertices()[index].tangent = {animationAmplitude, 0.0f, 0.0f};
            if (ring >= 0 && side > 0) {
                const auto stride = static_cast<std::uint32_t>(sides + 1);
                candidate.AddQuad(index - stride - 1u, index - 1u, index, index - stride);
            }
        }
    }

    const float flowSpeed = std::clamp(receiver.velocity.Length(), 0.25f, 25.0f);
    const float impact = std::sqrt(entrySpeed * entrySpeed + 2.0f * FALL_GRAVITY * height);
    const float foamLength = (InnerCenter(receiver) - end).Length();
    const math::Vector4 foam{flowSpeed, std::clamp(impact / 18.0f, 0.35f, 2.0f), (std::max)(0.25f, foamLength), 1.0f};
    const Vector3 foamLift{0.0f, 0.025f, 0.0f};
    const auto first = candidate.AddVertex(receiver.left - start + foamLift, Vector3::UP, {0.0f, 0.0f}, foam);
    candidate.AddVertex(receiver.right - start + foamLift, Vector3::UP, {1.0f, 0.0f}, foam);
    candidate.AddVertex(receiver.insideRight - start + foamLift, Vector3::UP, {1.0f, 1.0f}, foam);
    candidate.AddVertex(receiver.insideLeft - start + foamLift, Vector3::UP, {0.0f, 1.0f}, foam);
    candidate.AddQuad(first, first + 1, first + 2, first + 3);
    if (farIndexStart) {
        *farIndexStart = candidate.IndexCount();
        /// @note Both index ranges reuse the same seam vertices and foam. Terrain-hugging falls retain all trajectory rings.
        const auto vertex = [&](int ring, int side) { return static_cast<std::uint32_t>((ring + 1) * (sides + 1) + side); };
        const int ringStep = geometry.rockAttached ? 1 : 3;
        for (int row = -1; row < rings + 1;) {
            const int next = row < 0 || row == rings ? row + 1 : (std::min)(row + ringStep, rings);
            for (int side = 0; side < sides;) {
                const int edge = (std::min)(side + 2, sides);
                candidate.AddQuad(vertex(row, side), vertex(next, side), vertex(next, edge), vertex(row, edge));
                side = edge;
            }
            row = next;
        }
        candidate.AddQuad(first, first + 1, first + 2, first + 3);
    }
    if (midFall) *midFall = middle;
    out = std::move(candidate);
    return true;
}

namespace {
bool ReadWaterfallConnections(Scene& scene, GameObject& owner, WaterfallComponent& waterfall, WaterfallGeometry& geometry)
{
    auto* upper = waterfall.upstream.Resolve(scene);
    auto* lower = waterfall.downstream.Resolve(scene);
    auto* source = upper ? upper->GetComponent<WaterComponent>() : nullptr;
    auto* receiver = lower ? lower->GetComponent<WaterComponent>() : nullptr;
    if (!source || !receiver || upper == lower || !source->enabled || !receiver->enabled
        || !upper->activeInHierarchy() || !lower->activeInHierarchy()
        || !PrepareWaterSplineSnapshot(scene, upper->GetID(), *source, upper->transform)
        || !PrepareWaterSplineSnapshot(scene, lower->GetID(), *receiver, lower->transform)
        || !source->splineSnapshot
        || !GetWaterfallRiverSection(*source->splineSnapshot, waterfall.upstreamAtStart, true, geometry.source)) return false;
    geometry.breakup = FiniteAmount(waterfall.breakupAmount);
    geometry.rockAttached = waterfall.rockAttached;
    if (receiver->splineSnapshot && receiver->splineSnapshot->shape == WaterSurfaceShape::SPLINE_RIVER) {
        return GetWaterfallRiverSection(*receiver->splineSnapshot, waterfall.downstreamAtStart, false, geometry.receiver);
    }

    const Vector3 hint = owner.transform.worldPosition;
    const Vector3 lip = Center(geometry.source);
    const Vector3 forward = Horizontal(hint - lip).NormalizedOr(Horizontal(lip - InnerCenter(geometry.source))
        .NormalizedOr(Vector3::FORWARD));
    const Vector3 right = Vector3::Cross(Vector3::UP, forward);
    const float halfWidth = (geometry.source.right - geometry.source.left).Length() * 0.5f;
    const auto sample = [&](Vector3 point, Vector3& surface, float& depth, Vector3& velocity) {
        WaterSurfaceHeightSample height;
        if (!TrySampleWaterSurfaceHeightWorld(*receiver, lower->transform, point, 0.0f, height)
            || !height.insideSurface || height.excluded) return false;
        surface = {point.x, height.position.y - height.displacement, point.z};
        depth = height.waterDepthLimit;
        WaterSplineSample base;
        if (receiver->splineSnapshot && TrySampleWaterSpline(*receiver->splineSnapshot, point.x, point.z, base)) {
            surface.y = base.baseHeight;
            depth = base.depth;
            velocity = base.worldVelocity;
        } else {
            velocity = {receiver->current.x, 0.0f, receiver->current.y};
        }
        return Finite(surface) && depth > 0.0f;
    };
    /// @note A lake/rectangle has no incoming cross-section. Fit the requested impact band entirely inside the wet footprint.
    for (float fraction : {1.0f, 0.7f, 0.45f, 0.25f}) {
        const float span = halfWidth * fraction;
        const Vector3 back = forward * (std::max)(span, 0.4f);
        WaterfallSection section;
        Vector3 center, ignoredVelocity;
        float ignoredDepth;
        if (!sample(hint, center, section.depth, section.velocity)
            || !sample(hint - right * span, section.left, ignoredDepth, ignoredVelocity)
            || !sample(hint + right * span, section.right, ignoredDepth, ignoredVelocity)
            || !sample(hint + back - right * span, section.insideLeft, ignoredDepth, ignoredVelocity)
            || !sample(hint + back + right * span, section.insideRight, ignoredDepth, ignoredVelocity)) continue;
        bool wet = true;
        for (float along : {0.0f, 0.5f, 1.0f}) for (float across : {0.25f, 0.5f, 0.75f}) {
            if (!sample(hint + back * along + right * ((across * 2.0f - 1.0f) * span),
                    center, ignoredDepth, ignoredVelocity)) wet = false;
        }
        if (!wet) continue;
        geometry.receiver = section;
        return true;
    }
    return false;
}

void ConfigureWaterfallEmitter(ParticleEmitter& emitter, std::size_t part, float width, float height,
    float surfaceY, float impactSpeed, float amount, std::uint32_t seed)
{
    auto& settings = emitter.settings;
    const bool mist = part == 4;
    const bool falling = part == 2;
    const bool lip = part == 3;
    settings.materialPath = mist ? WATERFALL_MIST : WATERFALL_SPRAY;
    settings.enabled = amount > 0.0f;
    settings.playing = true;
    settings.loop = true;
    settings.shape = ParticleEmitterShape::Box;
    settings.randomSeed = seed;
    const float spread = std::clamp(width * 0.045f, 0.12f, 4.0f);
    settings.boxExtents = {width * (mist ? 0.37f : 0.46f), spread * (falling ? 0.75f : 0.15f), spread};
    settings.receiveFlowFields = true;
    settings.flowCoupling = mist ? 0.7f : 0.12f;
    const float kick = std::clamp(impactSpeed * (part == 1 ? 0.39f : 0.23f), 1.1f, 12.0f);
    settings.emitVelocity = mist ? Vector3{0.0f, kick * 0.13f, kick * 0.23f}
        : Vector3{0.0f, falling ? -impactSpeed * 0.46f : kick * (lip ? 0.2f : 0.9f), kick * (falling ? 0.2f : 0.74f)};
    settings.velocitySpread = mist ? kick * 0.14f : kick * (falling ? 0.52f : 0.7f);
    settings.gravity = {0.0f, mist ? 0.23f : -FALL_GRAVITY, 0.0f};
    if (!mist) {
        settings.collisionMode = ParticleCollisionMode::Plane;
        settings.collisionPlaneY = surfaceY;
        settings.collisionRadius = 0.018f;
        settings.collisionResponse = ParticleCollisionResponse::Kill;
    }
    settings.lifetime = mist ? 2.15f : (falling ? std::clamp(std::sqrt(height) * 0.14f, 0.35f, 1.65f) : 0.78f);
    settings.lifetimeRandom = 0.28f;
    settings.renderMode = mist ? ParticleRenderMode::Billboard : ParticleRenderMode::StretchedBillboard;
    settings.stretchedVelocityScale = 0.10f;
    settings.stretchedLengthScale = 1.15f;
    settings.sizeStart = mist ? spread * 1.25f
        : std::clamp(width * (part == 1 ? 0.015f : 0.010f), part == 1 ? 0.078f : 0.055f, 0.24f);
    settings.sizeEnd = mist ? spread * 5.0f : settings.sizeStart * 0.2f;
    settings.colorStart = {0.87f, 0.95f, 1.0f, mist ? 0.29f : 0.83f};
    settings.colorEnd = {0.76f, 0.9f, 0.96f, 0.0f};
    settings.renderPriority = mist ? 23 : 24;
    settings.sortMode = ParticleSortMode::BackToFront;
    settings.maxParticles = mist ? 20 : (falling ? 176 : 136);
    settings.emitRate = amount * std::clamp(width * (mist ? 0.75f : (lip ? 5.0f : 11.0f)),
        mist ? 2.0f : 10.0f, mist ? 8.0f : (lip ? 65.0f : 150.0f));
    settings.culling.lodNearDistance = mist ? 25.0f : 55.0f;
    settings.culling.lodFarDistance = mist ? 125.0f : 220.0f;
    settings.culling.lodFarRateScale = mist ? 0.07f : 0.02f;
    settings.culling.screenCoverageThreshold = mist ? 0.001f : 0.00035f;
    settings.culling.cullingBoundsPadding = spread;
    settings.culling.pauseWhenCulled = false;
}

bool EnsureWaterfallParts(Scene& scene, GameObject& owner, WaterfallComponent& waterfall)
{
    const auto missing = std::count_if(waterfall.parts.begin(), waterfall.parts.begin() + PART_NAMES.size(),
        [&](EntityID id) { return !scene.IsValid(id); });
    if (!scene.CanCreateGameObjects(static_cast<std::size_t>(missing))) return false;
    for (std::size_t part = 0; part < PART_NAMES.size(); ++part) {
        if (scene.IsValid(waterfall.parts[part])) continue;
        auto* child = scene.TryCreateGameObject(PART_NAMES[part]);
        if (!child) return false;
        child->SetRuntimeGenerated(true);
        child->SetParent(owner);
        child->AddComponent<WaterfallPart>({part});
        if (part == 0) {
            auto& mesh = child->AddComponent<MeshRenderer>();
            mesh.castShadows = false;
            auto& procedural = child->AddComponent<ProceduralMeshComponent>();
            procedural.materialPath = WATERFALL_SURFACE;
        } else {
            child->AddComponent<ParticleEmitter>();
        }
        waterfall.parts[part] = child->GetID();
    }
    return true;
}

} /// @note namespace

bool ResolveWaterfallGeometry(Scene& scene, GameObject& owner, WaterfallGeometry& out)
{
    auto* waterfall = owner.GetComponent<WaterfallComponent>();
    return waterfall && waterfall->enabled && owner.activeInHierarchy()
        && ReadWaterfallConnections(scene, owner, *waterfall, out);
}

/// @note Identity rotation uses the same subtract/divide and double grid extents as the vertical heightfield query.
/// @note Other poses and custom triangles can reach outside the upright grid and always use the full query.
struct WaterfallTerrainSource {
    const TerrainComponent* terrain;
    const Transform* pose;
    uint64_t revision;
    double width, depth;
    bool canReject;

    bool Overlaps(const math::Vector4& bounds) const
    {
        if (!canReject) return true;
        const auto axisOverlaps = [](float low, float high, float origin, float scale, double extent) {
            const float first = (low - origin) / scale;
            const float last = (high - origin) / scale;
            return (std::max)(first, last) >= 0 && (std::min)(first, last) <= extent;
        };
        return axisOverlaps(bounds.x, bounds.z, pose->worldPosition.x, pose->worldScale.x, width)
            && axisOverlaps(bounds.y, bounds.w, pose->worldPosition.z, pose->worldScale.z, depth);
    }
};

void UpdateWaterfallRuntime(Scene& scene, float dt)
{
    std::vector<EntityID> orphans;
    for (const EntityID id : scene.GetEntities<WaterfallPart>()) {
        const auto* child = scene.GetGameObject(id);
        auto* parent = child ? child->GetParent() : nullptr;
        const auto* owner = parent ? parent->GetComponent<WaterfallComponent>() : nullptr;
        const auto* part = scene.GetComponent<WaterfallPart>(id);
        if (!owner || part->index >= owner->parts.size() || owner->parts[part->index] != id) orphans.push_back(id);
    }
    if (!orphans.empty()) scene.DestroyGameObjects(orphans);
    const auto owners = scene.GetEntities<WaterfallComponent>();
    if (owners.empty()) return;

    constexpr uint64_t HASH_BASIS = 1469598103934665603ull;
    const auto hash = [](uint64_t& value, uint64_t word) { value = (value ^ word) * 1099511628211ull; };
    uint64_t terrainRevision = HASH_BASIS;
    std::vector<WaterfallTerrainSource> sources;
    const auto terrains = scene.GetEntities<TerrainComponent>();
    sources.reserve(terrains.size());
    for (const auto id : terrains) {
        const auto* object = scene.GetGameObject(id);
        const auto* terrain = scene.GetComponent<TerrainComponent>(id);
        const bool enabled = object->activeInHierarchy() && terrain->enabled;
        uint64_t stamp = HASH_BASIS;
        const auto add = [&](uint64_t word) { hash(terrainRevision, word); hash(stamp, word); };
        add(id.index);
        add(id.generation);
        add(terrain->surfaceRevision);
        add(enabled);
        const auto& pose = object->transform;
        for (const auto value : std::array{pose.worldPosition.x, pose.worldPosition.y, pose.worldPosition.z,
            pose.worldRotation.x, pose.worldRotation.y, pose.worldRotation.z, pose.worldRotation.w,
            pose.worldScale.x, pose.worldScale.y, pose.worldScale.z}) add(std::bit_cast<uint32_t>(value));
        if (!enabled) continue;
        hash(stamp, scene.GetComponentLifetime<TerrainComponent>(id));
        const auto& rotation = pose.worldRotation;
        const bool canReject = terrain->surfacePositions.empty() && terrain->surfaceIndices.empty()
            && terrain->columns >= 2 && terrain->rows >= 2 && std::isfinite(terrain->cellSize) && terrain->cellSize > 0
            && rotation.x == 0 && rotation.y == 0 && rotation.z == 0 && rotation.w == 1 && Finite(pose.worldPosition)
            && Finite(pose.worldScale) && pose.worldScale.x != 0 && pose.worldScale.z != 0;
        sources.push_back({terrain, &pose, stamp,
            (static_cast<double>(terrain->columns) - 1) * terrain->cellSize,
            (static_cast<double>(terrain->rows) - 1) * terrain->cellSize, canReject});
    }
    const auto curtainRevision = [&](const WaterfallComponent& waterfall) {
        uint64_t revision = HASH_BASIS;
        if (waterfall.hasGroundProbes || !waterfall.groundProbesFinite)
            for (const auto& source : sources)
                if (!waterfall.groundProbesFinite || source.Overlaps(waterfall.groundProbeBounds)) hash(revision, source.revision);
        return revision;
    };

    for (const auto id : owners) {
        auto& owner = *scene.GetGameObject(id);
        auto& waterfall = *scene.GetComponent<WaterfallComponent>(id);
        WaterfallGeometry geometry;
        const bool active = waterfall.enabled && owner.activeInHierarchy();
        const bool resolved = active && ReadWaterfallConnections(scene, owner, waterfall, geometry);
        const auto hide = [&] {
            for (const auto part : waterfall.parts) if (auto* child = scene.GetGameObject(part)) child->SetActive(false);
        };
        if (!resolved) {
            hide();
            waterfall.prepared = false;
            waterfall.status = active ? "Connect an upstream Spline River and a receiving water surface." : "Disabled";
            continue;
        }
        const float spray = FiniteAmount(waterfall.sprayAmount), mist = FiniteAmount(waterfall.mistAmount);
        const bool changed = !waterfall.prepared || waterfall.geometry != geometry
            || waterfall.terrainRevision != curtainRevision(waterfall);
        if (changed) {
            waterfall.geometry = geometry;
            waterfall.prepared = true;
        }
        if (!changed && !waterfall.valid) continue;
        const bool missing = std::any_of(waterfall.parts.begin(), waterfall.parts.begin() + PART_NAMES.size(),
            [&](EntityID part) { return !scene.IsValid(part); });
        MeshBuilder builder;
        if (changed || missing) {
            waterfall.hasGroundProbes = false;
            waterfall.groundProbesFinite = true;
            const auto ground = [&](const Vector3& point) {
                const math::Vector4 bounds{point.x, point.z, point.x, point.z};
                auto& footprint = waterfall.groundProbeBounds;
                if (!std::isfinite(point.x) || !std::isfinite(point.z)) waterfall.groundProbesFinite = false;
                else if (waterfall.hasGroundProbes) {
                    footprint = {(std::min)(footprint.x, point.x), (std::min)(footprint.y, point.z),
                        (std::max)(footprint.z, point.x), (std::max)(footprint.w, point.z)};
                } else {
                    footprint = bounds;
                    waterfall.hasGroundProbes = true;
                }
                float result = -std::numeric_limits<float>::infinity();
                for (const auto& source : sources) {
                    TerrainSurfaceSample sample;
                    if (source.Overlaps(bounds)
                        && TrySampleTerrainWorld(*source.terrain, *source.pose, point.x, point.z, sample))
                        result = (std::max)(result, sample.position.y);
                }
                return result;
            };
            std::uint32_t farIndexStart = 0;
            waterfall.valid = BuildWaterfallGeometry(geometry, ground, builder, &waterfall.midFall, &farIndexStart);
            /// @note Ground affects Y and can abort a build. Every XZ probe through the first failure remains a dependency.
            waterfall.terrainRevision = curtainRevision(waterfall);
            if (!waterfall.valid) {
                hide();
                waterfall.status = "The waterfall intersects terrain or its water connections do not form a downhill path.";
                continue;
            }
            if (!EnsureWaterfallParts(scene, owner, waterfall)) {
                hide();
                waterfall.status = "Insufficient scene capacity for waterfall effects.";
                continue;
            }
            auto* surface = scene.GetComponent<ProceduralMeshComponent>(waterfall.parts[0]);
            scene.GetComponent<WaterfallPart>(waterfall.parts[0])->pendingFarIndexStart = farIndexStart;
            surface->builder = std::move(builder);
            surface->dirty = MeshDirty::All;
            waterfall.status = "Connected";
        }

        const Vector3 lip = Center(geometry.source), foot = Center(geometry.receiver);
        const float height = lip.y - foot.y;
        const float width = (geometry.source.right - geometry.source.left).Length();
        const float impact = std::sqrt(geometry.source.velocity.LengthSq() + 2.0f * FALL_GRAVITY * height);
        const Vector3 direction = Horizontal(InnerCenter(geometry.receiver) - foot)
            .NormalizedOr(Horizontal(foot - lip).NormalizedOr(Vector3::FORWARD));
        const auto yaw = math::Quaternion::FromAxisAngle(Vector3::UP, std::atan2(direction.x, direction.z));
        const std::array anchors{lip, foot + Vector3::UP * 0.08f, waterfall.midFall,
            lip + Vector3::UP * 0.04f, foot + Vector3::UP * 0.35f};
        for (std::size_t part = 0; part < PART_NAMES.size(); ++part) {
            auto& child = *scene.GetGameObject(waterfall.parts[part]);
            child.layer = owner.layer;
            child.SetActive(true);
            const auto rotation = part == 0 ? math::Quaternion::Identity() : yaw;
            if (changed || missing || (child.transform.worldPosition - anchors[part]).LengthSq() > 0.000001f
                || child.transform.worldScale != Vector3::ONE || child.transform.worldRotation != rotation)
                SetWorldPose(child, anchors[part], rotation, Vector3::ONE);
            if (part == 0 || (!changed && !missing && waterfall.spray == spray && waterfall.mist == mist)) continue;
            auto& emitter = *child.GetComponent<ParticleEmitter>();
            const float span = part == 1 || part == 4 ? (geometry.receiver.right - geometry.receiver.left).Length() : width;
            ConfigureWaterfallEmitter(emitter, part, span, height, foot.y, impact, part == 4 ? mist : spray,
                id.index * 1973u + static_cast<std::uint32_t>(part) * 7919u + 1u);
            if (missing) emitter.ResetPlayback();
        }
        waterfall.spray = spray;
        waterfall.mist = mist;
        UpdateWaterfallFluid(scene, owner, waterfall, terrainRevision, changed, dt);
    }
}

void PrepareWaterfallSeams(Scene& scene)
{
    for (const EntityID id : scene.GetEntities<WaterComponent>()) {
        auto* water = scene.GetComponent<WaterComponent>(id);
        if (water->waterfallSeams.front().w != 0.0f) water->waterfallSeams = {};
    }
    const auto add = [&](EntityRef& reference, const WaterfallSection& section) {
        auto* object = reference.Resolve(scene);
        auto* water = object ? ResolveWaterZoneOwner(scene, object->GetID()) : nullptr;
        if (!water || water->waterfallSeams.front().w < 0.0f) return;
        const Vector3 center = Center(section);
        const float radius = (std::max)({Horizontal(section.left - center).Length(),
            Horizontal(section.right - center).Length(), Horizontal(section.insideLeft - center).Length(),
            Horizontal(section.insideRight - center).Length()}) + 0.10f;
        const float fade = (std::max)({1.5f, radius * 0.32f, water->SurfaceHeightBound() * 3.0f});
        auto& seams = water->waterfallSeams;
        const auto slot = std::find_if(seams.begin(), seams.end(), [&](const auto& entry) {
            return entry.w == 0.0f || (entry.x == center.x && entry.y == center.z);
        });
        if (slot != seams.end()) *slot = {center.x, center.z, (std::max)(slot->z, radius), (std::max)(slot->w, radius + fade)};
        else seams.front().w = -1.0f;
    };
    for (const EntityID id : scene.GetEntities<WaterfallComponent>()) {
        const auto* object = scene.GetGameObject(id);
        auto* waterfall = scene.GetComponent<WaterfallComponent>(id);
        if (!waterfall->enabled || !waterfall->prepared || !waterfall->valid || !object->activeInHierarchy()
            || !scene.IsValid(waterfall->parts[0])) continue;
        add(waterfall->upstream, waterfall->geometry.source);
        add(waterfall->downstream, waterfall->geometry.receiver);
    }
}

} /// @note namespace fbzz::scene
