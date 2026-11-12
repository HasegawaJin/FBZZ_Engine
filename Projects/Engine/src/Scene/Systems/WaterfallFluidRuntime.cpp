/// @file    WaterfallFluidRuntime.cpp
/// @brief   Snapshot collision inputs and play bounded, asynchronously loaded waterfall mesh frames.
/// @author  Hasegawa Jin
/// @date    2026-11-11
#include "WaterfallFluidRuntime.hpp"
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Asset/AssetPath.hpp>
#include <Engine/Core/Concurrency/TaskSystem.hpp>
#include <Engine/Scene/Components/ColliderComponent.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Components/ProceduralMeshComponent.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Engine/Scene/Components/WaterfallComponent.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Engine/Scene/TerrainSurfaceQuery.hpp>
#include <Engine/Scene/WaterfallFluid.hpp>
#include <Graphics/Renderer/Camera.hpp>
#include <Math/Frustum.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <utility>

namespace fbzz::scene {
namespace {
using math::Vector3;
constexpr float GRAVITY = 9.81f;
constexpr std::size_t FLUID_PART = 5;
constexpr char SURFACE_MATERIAL[] = "guid:4d940c1f438a4b968274ef36d2da9598|Assets/Materials/Water/WaterfallSurface.mat";

struct InputStamp {
    std::uint64_t value = 1469598103934665603ull;
    void Add(std::uint64_t word) { value = (value ^ word) * 1099511628211ull; }
    void AddFloat(float number)
    {
        /// @note A submillimeter tolerance keeps round trips through the scene format from invalidating the clip.
        Add(std::isfinite(number) ? static_cast<std::uint64_t>(std::llround(std::clamp(number, -1.0e8f, 1.0e8f)
            * 10000.0)) : std::numeric_limits<std::uint64_t>::max());
    }
    void AddVector(const Vector3& v) { AddFloat(v.x); AddFloat(v.y); AddFloat(v.z); }
};

Vector3 Center(const Vector3& a, const Vector3& b) { return (a + b) * 0.5f; }

float HalfSize(const WaterfallComponent& waterfall)
{
    return std::isfinite(waterfall.fluidSize) ? std::clamp(waterfall.fluidSize, 1.0f, 48.0f) * 0.5f : 4.0f;
}

Vector3 PatchOrigin(const WaterfallGeometry& geometry, float half)
{
    return Center(geometry.receiver.left, geometry.receiver.right) + Vector3::UP * (half * 0.42f);
}

bool AddObstacle(GameObject& rock, const Vector3& origin, float half, asset::WaterfallFluidObstacle& out)
{
    const auto& pose = rock.transform;
    const Vector3 scale{std::abs(pose.worldScale.x), std::abs(pose.worldScale.y), std::abs(pose.worldScale.z)};
    const auto center = [&](const Vector3& local) {
        const auto scaled = Vector3{local.x * pose.worldScale.x, local.y * pose.worldScale.y, local.z * pose.worldScale.z};
        return (pose.worldPosition + pose.worldRotation * scaled - origin) / half;
    };
    const auto box = [&](const Vector3& size, const Vector3& offset, const math::Quaternion& rotation) {
        out.center = center(offset);
        out.rotation = rotation;
        out.halfSize = Vector3{size.x * scale.x, size.y * scale.y, size.z * scale.z} * (0.5f / half);
        return (std::min)({out.halfSize.x, out.halfSize.y, out.halfSize.z}) > 0.0f;
    };
    if (const auto* sphere = rock.GetComponent<SphereColliderComponent>(); sphere && sphere->enabled && !sphere->isTrigger) {
        out.center = center(sphere->center);
        out.sphereRadius = sphere->radius * (std::max)({scale.x, scale.y, scale.z}) / half;
        return out.sphereRadius > 0.0f;
    }
    if (const auto* oriented = rock.GetComponent<BoxColliderComponent>(); oriented && oriented->enabled && !oriented->isTrigger)
        return box(oriented->size, oriented->center, pose.worldRotation);
    if (const auto* aligned = rock.GetComponent<AabbColliderComponent>(); aligned && aligned->enabled && !aligned->isTrigger)
        return box(aligned->size, aligned->center, math::Quaternion::Identity());
    return false;
}

bool CaptureObstacles(Scene& scene, const WaterfallComponent& waterfall, const Vector3& origin, float half,
    std::vector<asset::WaterfallFluidObstacle>& out)
{
    if (waterfall.fluidRocks.size() > 16) return false;
    for (const auto& reference : waterfall.fluidRocks) {
        if (!reference.IsAssigned()) continue;
        auto* rock = reference.Resolve(scene);
        asset::WaterfallFluidObstacle obstacle;
        if (!rock || !rock->activeInHierarchy() || !AddObstacle(*rock, origin, half, obstacle)) return false;
        out.push_back(obstacle);
    }
    return true;
}

void StampObstacles(InputStamp& stamp, const std::vector<asset::WaterfallFluidObstacle>& obstacles)
{
    stamp.Add(obstacles.size());
    for (const auto& rock : obstacles) {
        stamp.AddVector(rock.center);
        stamp.AddVector(rock.halfSize);
        stamp.AddFloat(rock.sphereRadius);
        stamp.AddFloat(rock.rotation.x);
        stamp.AddFloat(rock.rotation.y);
        stamp.AddFloat(rock.rotation.z);
        stamp.AddFloat(rock.rotation.w);
    }
}

void StampSettings(InputStamp& stamp, const WaterfallComponent& waterfall)
{
    stamp.AddFloat(HalfSize(waterfall));
    stamp.Add(waterfall.fluidGrid);
    stamp.Add(waterfall.fluidEmission);
    stamp.Add(waterfall.fluidFrames);
}

bool Capture(Scene& scene, const WaterfallComponent& waterfall, const WaterfallGeometry& geometry,
    asset::WaterfallFluidBakeInput& out, std::string& error)
{
    asset::WaterfallFluidBakeInput input;
    const float half = HalfSize(waterfall);
    const Vector3 lip = Center(geometry.source.left, geometry.source.right);
    const Vector3 foot = Center(geometry.receiver.left, geometry.receiver.right);
    const float height = lip.y - foot.y;
    if (!std::isfinite(height) || height < 0.3f || height > 2000.0f
        || !std::isfinite((lip - foot).LengthSq())) {
        error = "The fluid bake needs a valid downhill connection.";
        return false;
    }
    const auto origin = PatchOrigin(geometry, half);
    input.grid = std::clamp(waterfall.fluidGrid, 24, 64);
    input.particlesPerSecond = std::clamp(waterfall.fluidEmission, 100, 3000);
    input.frameCount = std::clamp(waterfall.fluidFrames, 8, 120);
    input.gravity = GRAVITY / half;
    input.receiverHeight = (foot.y - origin.y) / half;
    const float sourceHeight = (std::min)(height * 0.83f, half * 0.94f);
    const float approach = std::sqrt(1.0f - sourceHeight / height);
    const auto heading = Vector3{foot.x - lip.x, 0.0f, foot.z - lip.z};
    const float run = heading.Length();
    const auto forward = heading.NormalizedOr(Vector3::FORWARD);
    const auto aligned = [&](Vector3 tangent) {
        tangent.y = 0.0f;
        const auto normalized = tangent.NormalizedOr(forward);
        return Vector3::Dot(normalized, forward) > 0.05f ? normalized : forward;
    };
    const float travel = std::sqrt(2.0f * height / GRAVITY);
    const float launch = std::clamp(geometry.source.velocity.Length() * travel, run * 0.55f, run * 1.45f);
    const auto from = aligned(lip - Center(geometry.source.insideLeft, geometry.source.insideRight)) * launch;
    const auto to = aligned(Center(geometry.receiver.insideLeft, geometry.receiver.insideRight) - foot) * (2.0f * run - launch);
    const float t = geometry.rockAttached ? 1.0f - sourceHeight / height : approach;
    const float left = 1.0f - t, blend = t * t * (3.0f - 2.0f * t);
    auto inlet = lip + heading * blend + from * (t * left * left) - to * (t * t * left);
    inlet.y = foot.y + sourceHeight;
    input.inlet = (inlet - origin) / half;
    if (std::abs(input.inlet.x) > 0.88f || std::abs(input.inlet.z) > 0.88f) {
        error = "The incoming curtain is outside the fluid region. Increase its size.";
        return false;
    }
    input.inletVelocity = (heading * (6.0f * t * left) + from * (left * (1.0f - 3.0f * t))
        + to * (t * (3.0f * t - 2.0f))) / (travel * half);
    input.inletVelocity.y = -std::sqrt(2.0f * GRAVITY * (height - sourceHeight)) / half;
    input.inletAxis = Vector3::Lerp(geometry.source.right - geometry.source.left,
        geometry.receiver.right - geometry.receiver.left, blend).NormalizedOr(Vector3::RIGHT);
    const float edge = 0.93f - (std::max)(std::abs(input.inlet.x), std::abs(input.inlet.z));
    const float width = std::lerp((geometry.source.right - geometry.source.left).Length(),
        (geometry.receiver.right - geometry.receiver.left).Length(), blend);
    input.inletHalfWidth = std::clamp(width * 0.5f / half, 0.025f, (std::max)(edge, 0.025f));
    if (!CaptureObstacles(scene, waterfall, origin, half, input.obstacles)) {
        error = "Assign up to 16 active rocks with an enabled Sphere, Box, or AABB collider.";
        return false;
    }

    InputStamp stamp;
    StampSettings(stamp, waterfall);
    stamp.AddVector(origin);
    stamp.AddVector(input.inlet);
    stamp.AddVector(input.inletVelocity);
    stamp.AddVector(input.inletAxis);
    stamp.AddFloat(input.inletHalfWidth);
    StampObstacles(stamp, input.obstacles);
    const auto terrains = scene.GetEntities<TerrainComponent>();
    input.ground.reserve(static_cast<std::size_t>(input.grid * input.grid));
    for (int z = 0; z < input.grid; ++z)
        for (int x = 0; x < input.grid; ++x) {
            const float worldX = origin.x + half * (2.0f * x / (input.grid - 1) - 1.0f);
            const float worldZ = origin.z + half * (2.0f * z / (input.grid - 1) - 1.0f);
            float heightSample = -2.0f;
            for (auto id : terrains) {
                const auto* tile = scene.GetGameObject(id);
                const auto* terrain = scene.GetComponent<TerrainComponent>(id);
                TerrainSurfaceSample sample;
                if (tile && tile->activeInHierarchy() && terrain->enabled
                    && TrySampleTerrainWorld(*terrain, tile->transform, worldX, worldZ, sample))
                    heightSample = (std::max)(heightSample, (sample.position.y - origin.y) / half);
            }
            input.ground.push_back(heightSample);
            stamp.AddFloat(heightSample);
        }
    input.stamp = stamp.value;
    out = std::move(input);
    error.clear();
    return true;
}

GameObject* MakeFluidPart(Scene& scene, GameObject& owner, WaterfallComponent& waterfall, const Vector3& origin)
{
    auto* part = scene.TryCreateGameObject("Waterfall 3D Fluid");
    if (!part) return nullptr;
    part->SetRuntimeGenerated(true);
    part->SetParent(owner);
    part->layer = owner.layer;
    SetWorldPose(*part, origin, math::Quaternion::Identity(), Vector3::ONE);
    part->AddComponent<WaterfallPart>({FLUID_PART});
    part->AddComponent<MeshRenderer>().castShadows = false;
    part->AddComponent<ProceduralMeshComponent>().materialPath = SURFACE_MATERIAL;
    waterfall.parts[FLUID_PART] = part->GetID();
    return part;
}
} /// @note namespace

bool CaptureWaterfallFluidBake(Scene& scene, GameObject& owner, asset::WaterfallFluidBakeInput& out, std::string& error)
{
    WaterfallGeometry geometry;
    if (!ResolveWaterfallGeometry(scene, owner, geometry)) {
        error = "Connect an upstream Spline River and receiving water surface before baking.";
        return false;
    }
    return Capture(scene, *owner.GetComponent<WaterfallComponent>(), geometry, out, error);
}

void UpdateWaterfallFluid(Scene& scene, GameObject& owner, WaterfallComponent& waterfall,
    std::uint64_t terrainRevision, bool waterChanged, float dt)
{
    auto* part = scene.GetGameObject(waterfall.parts[FLUID_PART]);
    const auto hide = [&] { if (part) part->SetActive(false); };
    const bool requested = std::exchange(waterfall.fluidViewDemand, false);
    if (waterfall.fluidClip.empty()) {
        hide();
        waterfall.playingFluidClip.clear();
        waterfall.fluidStatus.clear();
        return;
    }
    if (!requested) {
        hide();
        /// @note A deferred refresh must still compare the connection when the patch comes back into view.
        if (waterChanged) waterfall.playingFluidClip.clear();
        return;
    }
    const float half = HalfSize(waterfall);
    const auto origin = PatchOrigin(waterfall.geometry, half);
    InputStamp revision;
    revision.Add(terrainRevision);
    StampSettings(revision, waterfall);
    std::vector<asset::WaterfallFluidObstacle> obstacles;
    const bool validObstacles = CaptureObstacles(scene, waterfall, origin, half, obstacles);
    revision.Add(validObstacles);
    StampObstacles(revision, obstacles);
    const auto assets = asset::AssetDatabase::Revision();
    if (waterChanged || waterfall.playingFluidClip != waterfall.fluidClip || revision.value != waterfall.fluidInputRevision
        || (waterfall.fluidLoadFailed && assets != waterfall.fluidAssetRevision)) {
        hide();
        waterfall.playingFluidClip = waterfall.fluidClip;
        waterfall.fluidInputRevision = revision.value;
        waterfall.fluidAssetRevision = assets;
        waterfall.fluidFrameJob = {};
        waterfall.fluidFrame = -1;
        waterfall.fluidFrameCount = 0;
        waterfall.fluidTime = 0.0f;
        waterfall.resolvedFluidPath = asset::ResolveAssetPath(waterfall.fluidClip);
        asset::WaterfallFluidBakeInput input;
        waterfall.fluidLoadFailed = !Capture(scene, waterfall, waterfall.geometry, input, waterfall.fluidStatus)
            || waterfall.resolvedFluidPath.empty();
        if (waterfall.fluidLoadFailed) {
            if (waterfall.fluidStatus.empty()) waterfall.fluidStatus = "3D fluid clip is missing.";
            return;
        }
        waterfall.fluidStamp = input.stamp;
        waterfall.fluidStatus = "Loading 3D fluid clip...";
    }
    if (waterfall.fluidLoadFailed) return;
    if (part && waterfall.fluidFrame >= 0) part->SetActive(true);
    if (waterfall.fluidFrameCount > 0 && std::isfinite(dt) && dt > 0.0f) {
        waterfall.fluidTime = std::fmod(waterfall.fluidTime + dt,
            static_cast<float>(waterfall.fluidFrameCount) / waterfall.fluidFps);
    }
    if (waterfall.fluidFrameJob.valid()) {
        if (waterfall.fluidFrameJob.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        const auto frame = waterfall.fluidFrameJob.get();
        if (frame.frame < 0) {
            hide();
            waterfall.fluidLoadFailed = true;
            waterfall.fluidStatus = "3D fluid clip is missing, invalid, or out of date. Bake it again.";
            return;
        }
        if (!part) part = MakeFluidPart(scene, owner, waterfall, origin);
        if (!part) {
            waterfall.fluidStatus = "Insufficient scene capacity for the 3D liquid surface.";
            return;
        }
        part->SetActive(true);
        part->layer = owner.layer;
        SetWorldPose(*part, origin, math::Quaternion::Identity(), Vector3::ONE);
        auto& mesh = *part->GetComponent<ProceduralMeshComponent>();
        mesh.builder.Clear();
        mesh.builder.Reserve(frame.vertices.size(), frame.indices.size());
        for (const auto& v : frame.vertices)
            mesh.builder.AddVertex(v.position * half, v.normal, {v.position.x * 0.5f + 0.5f, v.position.y * 0.5f + 0.5f},
                {0.0f, 0.0f, v.coverage, 2.0f});
        mesh.builder.Indices().assign(frame.indices.begin(), frame.indices.end());
        mesh.dirty = MeshDirty::All;
        part->GetComponent<WaterfallPart>()->pendingFarIndexStart = std::numeric_limits<std::uint32_t>::max();
        waterfall.fluidFrame = frame.frame;
        waterfall.fluidFrameCount = frame.frameCount;
        waterfall.fluidFps = frame.framesPerSecond;
        waterfall.fluidStatus = "Playing baked 3D fluid";
    }
    const int request = waterfall.fluidFrameCount > 0
        ? (std::min)(static_cast<int>(waterfall.fluidTime * waterfall.fluidFps), waterfall.fluidFrameCount - 1) : 0;
    if (request == waterfall.fluidFrame) return;
    waterfall.fluidFrameJob = TaskSystem::Submit([path = waterfall.resolvedFluidPath, stamp = waterfall.fluidStamp, request] {
        asset::WaterfallFluidFrame frame;
        static_cast<void>(asset::LoadWaterfallFluidFrame(path, stamp, request, frame));
        return frame;
    });
}

void RequestWaterfallFluidForView(Scene& scene, const renderer::Camera& camera, const math::Frustum& frustum, int height)
{
    const float projection = std::abs(camera.GetProjectionMatrix().m[1][1]) * static_cast<float>(height);
    for (const auto id : scene.GetEntities<WaterfallComponent>()) {
        auto& waterfall = *scene.GetComponent<WaterfallComponent>(id);
        if (waterfall.fluidViewDemand || waterfall.fluidClip.empty() || !waterfall.prepared || !waterfall.valid) continue;
        const float half = HalfSize(waterfall);
        const float radius = half * 1.733f;
        const auto center = PatchOrigin(waterfall.geometry, half);
        const float depth = camera.m_projection == renderer::ProjectionMode::Orthographic ? 1.0f
            : (std::max)(Vector3::Dot(center - camera.m_position, camera.GetForward()) - radius, camera.m_near);
        waterfall.fluidViewDemand = radius * projection >= depth * 80.0f && frustum.IntersectsSphere(center, radius);
    }
}
} /// @note namespace fbzz::scene
