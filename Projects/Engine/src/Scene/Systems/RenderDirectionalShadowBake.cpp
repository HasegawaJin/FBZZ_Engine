/// @file    RenderDirectionalShadowBake.cpp
/// @brief   方向ライトのプリセットを準備し、検証済みの静的深度を Play へ引き継ぐ。
/// @author  Hasegawa Jin
/// @date    2026-11-07
#include "RenderDirectionalShadowBake.hpp"
#include <Engine/Asset/AnimationSampling.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/BakeFingerprint.hpp>
#include <Engine/Asset/ChunkFile.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Scene/Components/LightComponent.hpp>
#include <Engine/Scene/Components/SequencePlayerComponent.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Systems/DirectionalShadowBakeSystem.hpp>
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <Engine/Scene/Systems/RenderSystem.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Core/Logger.hpp>
#include <Graphics/Pipeline/DirectionalShadowBake.hpp>
#include <Graphics/Renderer/IBuffer.hpp>
#include <Graphics/Renderer/OwnedResource.hpp>
#include <Graphics/Renderer/RenderScene.hpp>
#include <Graphics/Renderer/ResourceManager.hpp>
#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstring>
#include <optional>
#include <unordered_map>

namespace fbzz::scene {
namespace {

using DirectionalBakeData = renderer::DirectionalShadowBakeData;
using Status = DirectionalShadowBakeStatus;
constexpr uint32_t SHADOW_CACHE_KIND = 0x33424453u;
constexpr uint32_t METADATA_CHUNK = 1;
constexpr uint32_t DEPTH_CHUNK = 2;
constexpr float EVENING_RAY_Y = -0.20791169f;
constexpr float DIRECTION_TOLERANCE_SQUARED = 1.0e-12f;
constexpr size_t MAX_CASTER_BUFFER_BYTES = 64u * 1024u * 1024u;
constexpr size_t MAX_TRACKED_CASTER_BUFFERS = 4096;
constexpr uint32_t MIN_BAKE_RESOLUTION = 64;
constexpr uint32_t MAX_BAKE_RESOLUTION = 4096;
constexpr float TARGET_BAKE_TEXEL_METERS = 1.0f;
constexpr uint64_t SNAPSHOT_WAIT_FRAMES = 120;

bool IsFinite(const math::Vector3& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

math::Vector3 BakeDirection(const math::Vector3& value)
{
    const float lengthSquared = value.LengthSq();
    return IsFinite(value) && std::isfinite(lengthSquared) && lengthSquared > 1.0e-12f
        ? value / std::sqrt(lengthSquared) : math::Vector3{};
}

bool SameDirection(const math::Vector3& left, const math::Vector3& right)
{
    return (left - right).LengthSq() <= DIRECTION_TOLERANCE_SQUARED;
}

EntityID SelectBakeLight(Scene& scene)
{
    EntityID selected = EntityID::INVALID;
    for (EntityID id : scene.GetEntities<LightComponent>()) {
        const auto* object = scene.GetGameObject(id);
        const auto* light = scene.GetComponent<LightComponent>(id);
        if (!object || !light || !object->activeInHierarchy() || !light->enabled
            || light->type != LightComponent::Type::Directional) continue;
        /// @note ExtractRenderLights と同じ最後の有効な方向ライトを使う。
        selected = light->castShadows && light->shadowStrength > 0.0f
            && light->shadowMode == LightComponent::ShadowMode::AUTO_BAKE ? id : EntityID::INVALID;
    }
    return selected;
}

struct RotationTrack {
    const asset::SequenceTrack* track = nullptr;
    math::Quaternion parent;
    math::Quaternion base;
    double duration = 0.0;
};

RotationTrack FindSunRotationTrack(Scene& scene, GameObject& sun)
{
    RotationTrack result;
    for (EntityID id : scene.GetEntities<SequencePlayerComponent>()) {
        const auto* owner = scene.GetGameObject(id);
        const auto* player = scene.GetComponent<SequencePlayerComponent>(id);
        if (!owner || !owner->activeInHierarchy() || !player || !player->enabled) continue;
        const asset::SequenceAsset* sequence = player->authoringSequence.get();
        if (!sequence && !player->sequencePath.empty()) {
            const auto handle = asset::AssetManager::Load<asset::SequenceAsset>(player->sequencePath);
            sequence = asset::AssetManager::Get<asset::SequenceAsset>(handle);
        }
        if (!sequence) continue;
        for (size_t index = 0; index < sequence->tracks.size(); ++index) {
            const auto& track = sequence->tracks[index];
            if (track.muted || track.type != asset::SequenceTrackType::Transform
                || track.rotations.empty() || track.binding.empty()) continue;
            const GameObject* target = track.binding == "$self" ? owner : nullptr;
            if (track.binding != "$self") {
                const auto binding = std::find_if(player->bindings.begin(), player->bindings.end(),
                    [&](const SequenceBinding& value) { return value.key == track.binding; });
                if (binding != player->bindings.end()) target = binding->target.Resolve(scene);
            }
            if (target != &sun) continue;
            if (result.track) return {};
            result.track = &track;
            result.duration = sequence->GetDurationSeconds();
            if (const auto* parent = sun.GetParent()) result.parent = parent->transform.worldRotation;
            result.base = sun.transform.rotation;
            if (player->appliedSequence == sequence && index < player->trackRuntime.size()
                && player->trackRuntime[index].hasSnapshot)
                result.base = player->trackRuntime[index].baseRotation;
        }
    }
    return result;
}

math::Vector3 SampleSunDirection(const RotationTrack& input, double time)
{
    const auto& track = *input.track;
    auto rotation = asset::SampleQuaternionKeys(track.rotations, time, math::Quaternion::Identity(), track.interp);
    if (track.space == asset::SequenceTransformSpace::RelativeToStart) rotation = input.base * rotation;
    if (track.space != asset::SequenceTransformSpace::World) rotation = input.parent * rotation;
    return BakeDirection(rotation * math::Vector3::FORWARD);
}

math::Vector3 SampleEvening(const RotationTrack& track)
{
    if (!track.track || !std::isfinite(track.duration) || track.duration <= 0.0) return {};
    constexpr int STEPS = 96;
    double previousTime = 0.0;
    auto previous = SampleSunDirection(track, 0.0);
    for (int step = 1; step <= STEPS; ++step) {
        const double time = track.duration * static_cast<double>(step) / STEPS;
        const auto current = SampleSunDirection(track, time);
        if (previous.y < EVENING_RAY_Y && current.y >= EVENING_RAY_Y) {
            double low = previousTime;
            double high = time;
            for (int iteration = 0; iteration < 16; ++iteration) {
                const double middle = (low + high) * 0.5;
                if (SampleSunDirection(track, middle).y < EVENING_RAY_Y) low = middle;
                else high = middle;
            }
            return SampleSunDirection(track, (low + high) * 0.5);
        }
        previousTime = time;
        previous = current;
    }
    return {};
}

std::array<math::Vector3, 2> ResolvePresets(Scene& scene, GameObject& object, const LightComponent& light)
{
    auto day = BakeDirection(light.shadowBakeDayDirection);
    auto evening = BakeDirection(light.shadowBakeEveningDirection);
    if (day.LengthSq() > 0.0f && evening.LengthSq() > 0.0f) return {day, evening};
    const auto track = FindSunRotationTrack(scene, object);
    if (day.LengthSq() == 0.0f && track.track) day = SampleSunDirection(track, 0.0);
    if (day.LengthSq() == 0.0f) day = BakeDirection(object.transform.Forward());
    if (evening.LengthSq() == 0.0f) evening = SampleEvening(track);
    if (evening.LengthSq() == 0.0f) {
        const auto horizontal = math::Vector3{day.x, 0.0f, day.z}.NormalizedOr(math::Vector3::FORWARD);
        evening = BakeDirection(horizontal * std::sqrt(1.0f - EVENING_RAY_Y * EVENING_RAY_Y)
            + math::Vector3{0.0f, EVENING_RAY_Y, 0.0f});
    }
    return {day, evening};
}

struct PendingBake {
    uint64_t sceneGeneration = 0;
    uint64_t requestedFrame = 0;
    EntityID light = EntityID::INVALID;
    std::filesystem::path directory;
    std::string diskOwner;
    std::array<math::Vector3, 2> directions;
    std::shared_ptr<const renderer::RenderScene> snapshot;
    renderer::ShadowBounds coverage;
    uint64_t resourceOwner = 0, resourceEpoch = 0;
    uint32_t mask = 0;
    size_t nextPreset = 0;
    Status status = Status::NOT_REQUIRED;
};

struct ResidentBake {
    std::string content;
    std::string diskKey;
    DirectionalBakeData data;
    renderer::OwnedResource<renderer::TextureTag> texture;
};

struct DirectionMotion {
    uint64_t sceneGeneration = 0;
    EntityID light = EntityID::INVALID;
    uint64_t frame = 0;
    math::Vector3 direction;
    bool stationary = false;
};

struct BufferFingerprint {
    renderer::ResourceHandle<renderer::BufferTag> handle;
    uint64_t version = 0;
    std::string content;
};

thread_local PendingBake s_pending;
thread_local std::array<ResidentBake, 2> s_resident;
thread_local std::array<DirectionMotion, 4> s_motion;
thread_local std::unordered_map<uint32_t, BufferFingerprint> s_bufferFingerprints;
thread_local uint64_t s_bufferOwner = 0;

bool SunStationary(uint64_t sceneGeneration, EntityID light, const math::Vector3& direction)
{
    const uint64_t frame = Time::frameCount;
    auto motion = std::find_if(s_motion.begin(), s_motion.end(), [&](const DirectionMotion& value) {
        return value.sceneGeneration == sceneGeneration && value.light == light;
    });
    if (motion == s_motion.end()) {
        motion = std::min_element(s_motion.begin(), s_motion.end(),
            [](const DirectionMotion& left, const DirectionMotion& right) { return left.frame < right.frame; });
        *motion = {sceneGeneration, light, frame, direction, false};
        return false;
    }
    if (!SameDirection(motion->direction, direction)) motion->stationary = false;
    else if (frame != motion->frame) motion->stationary = frame > 0 && motion->frame == frame - 1;
    motion->frame = frame;
    motion->direction = direction;
    return motion->stationary;
}

bool AddFiniteMatrix(asset::BakeFingerprint& hash, const math::Matrix4& matrix)
{
    for (const auto& row : matrix.m) {
        for (float value : row) if (!std::isfinite(value)) return false;
        hash.Add(std::span<const float>(row));
    }
    return true;
}

std::string BufferContent(renderer::ResourceManager& resources, renderer::ResourceHandle<renderer::BufferTag> handle,
    bool allowRead)
{
    renderer::BufferResourceMetadata metadata;
    if (!resources.GetBufferMetadata(handle, metadata) || !metadata.live || !metadata.cpuReadable
        || metadata.gpuWritable || metadata.contentVersion == 0 || metadata.sizeBytes == 0
        || metadata.sizeBytes > MAX_CASTER_BUFFER_BYTES) return {};
    if (s_bufferOwner != resources.GetInstanceId()) {
        s_bufferFingerprints.clear();
        s_bufferOwner = resources.GetInstanceId();
    }
    const auto found = s_bufferFingerprints.find(handle.id);
    if (found != s_bufferFingerprints.end() && found->second.handle == handle
        && found->second.version == metadata.contentVersion) return found->second.content;
    if (!allowRead || (found == s_bufferFingerprints.end()
        && s_bufferFingerprints.size() >= MAX_TRACKED_CASTER_BUFFERS)) return {};
    const auto* buffer = resources.Get(handle);
    if (!buffer) return {};
    std::vector<uint8_t> bytes(metadata.sizeBytes);
    if (!buffer->CopyData(0, bytes.size(), bytes.data())) return {};
    renderer::BufferResourceMetadata verified;
    if (!resources.GetBufferMetadata(handle, verified) || !verified.live
        || verified.contentVersion != metadata.contentVersion) return {};
    auto fingerprint = asset::BakeFingerprintOf(bytes);
    s_bufferFingerprints[handle.id] = {handle, metadata.contentVersion, fingerprint};
    return fingerprint;
}

bool LayerVisible(uint32_t layer, uint32_t mask)
{
    return layer < 32 && (mask & (uint32_t{1} << layer)) != 0;
}

std::string StaticContent(const renderer::RenderScene& scene, renderer::ResourceManager& resources,
    uint32_t mask, bool includeMeshes, bool allowRead)
{
    std::vector<std::string> parts;
    const auto meshObjects = includeMeshes ? std::span<const uint32_t>(scene.shadowBakeMeshObjects)
        : std::span<const uint32_t>{};
    parts.reserve(scene.terrains.size() + meshObjects.size());
    for (const auto& terrain : scene.terrains) {
        if (!LayerVisible(terrain.layer, mask) || !renderer::IsDirectionalShadowBakeTerrain(terrain)) continue;
        if (terrain.shadowBakeGeometryHash == 0) return {};
        asset::BakeFingerprint hash;
        hash.Add("terrain");
        hash.Add(static_cast<uint64_t>(terrain.layer));
        hash.Add(terrain.shadowBakeGeometryHash);
        if (!AddFiniteMatrix(hash, terrain.constants.worldMatrix)) return {};
        parts.push_back(hash.Finish());
    }
    const auto objects = scene.Objects();
    for (uint32_t ordinal : meshObjects) {
        if (ordinal >= objects.size()) return {};
        const auto& object = objects[ordinal];
        if (!LayerVisible(object.layer, mask)) continue;
        if (object.firstItem > scene.items.size() || object.itemCount > scene.items.size() - object.firstItem) return {};
        for (const auto& item : std::span<const renderer::RenderMeshItem>(scene.items).subspan(object.firstItem, object.itemCount)) {
            if (!renderer::IsDirectionalShadowBakeMesh(object, item)) continue;
            const std::string vertices = BufferContent(resources, item.vertexBuffer, allowRead);
            const std::string indices = BufferContent(resources, item.indexBuffer, allowRead);
            if (vertices.empty() || indices.empty() || !std::isfinite(item.material.depthBiasSlope)) return {};
            asset::BakeFingerprint hash;
            hash.Add("mesh");
            hash.Add(static_cast<uint64_t>(object.layer));
            if (!AddFiniteMatrix(hash, object.world)) return {};
            hash.Add(vertices);
            hash.Add(indices);
            hash.Add(static_cast<uint64_t>(item.vertexStride));
            hash.Add(static_cast<uint64_t>(item.indexCount));
            hash.Add(static_cast<uint64_t>(item.material.doubleSided));
            hash.Add(static_cast<uint64_t>(item.material.depthBias));
            hash.Add(std::span<const float>(&item.material.depthBiasSlope, 1));
            parts.push_back(hash.Finish());
        }
    }
    if (parts.empty()) return {};
    std::sort(parts.begin(), parts.end());
    asset::BakeFingerprint content;
    for (const auto& part : parts) content.Add(part);
    return content.Finish();
}

std::string MakeDiskKey(const std::string& content, const DirectionalBakeData& expected, const char* backend)
{
    asset::BakeFingerprint key;
    key.Add(static_cast<uint64_t>(SHADOW_CACHE_KIND));
    key.Add(content);
    if (backend) key.Add(backend);
    key.Add(static_cast<uint64_t>(expected.resolution));
    key.Add(static_cast<uint64_t>(expected.cullingMask));
    const std::array values{expected.lightDirection.x, expected.lightDirection.y, expected.lightDirection.z,
        expected.coverage.center.x, expected.coverage.center.y, expected.coverage.center.z, expected.coverage.radius};
    key.Add(std::span<const float>(values));
    return key.Finish();
}

uint64_t SourceStamp(const std::string& key)
{
    uint64_t stamp = 0;
    std::from_chars(key.data(), key.data() + key.size(), stamp, 16);
    return stamp;
}

bool ReadBake(const std::string& path, const std::string& key, const DirectionalBakeData& expected,
    DirectionalBakeData& out)
{
    asset::ChunkFileReader file;
    if (!file.Open(path) || file.Header().contentKind != SHADOW_CACHE_KIND
        || file.Header().sourceStamp != SourceStamp(key)) return false;
    const auto* metadata = file.Find(METADATA_CHUNK);
    const auto* depth = file.Find(DEPTH_CHUNK);
    std::array<float, 7> values{};
    const auto depthSize = static_cast<uint64_t>(expected.resolution) * expected.resolution * 4;
    if (!metadata || metadata->rawSize != sizeof(values) || !depth || depth->rawSize != depthSize) return false;
    std::vector<uint8_t> bytes;
    DirectionalBakeData result = expected;
    if (!file.ReadChunk(METADATA_CHUNK, bytes) || !file.ReadChunk(DEPTH_CHUNK, result.depthRgba)) return false;
    std::memcpy(values.data(), bytes.data(), sizeof(values));
    result.lightDirection = {values[0], values[1], values[2]};
    result.coverage = {{values[3], values[4], values[5]}, values[6], true};
    if (!IsFinite(result.lightDirection) || !SameDirection(result.lightDirection, expected.lightDirection)
        || (result.coverage.center - expected.coverage.center).LengthSq() != 0.0f
        || result.coverage.radius != expected.coverage.radius) return false;
    out = std::move(result);
    return true;
}

void WriteBake(const std::string& path, const std::string& key, DirectionalBakeData& data)
{
    const std::array values{data.lightDirection.x, data.lightDirection.y, data.lightDirection.z,
        data.coverage.center.x, data.coverage.center.y, data.coverage.center.z, data.coverage.radius};
    std::vector<uint8_t> metadata(sizeof(values));
    std::memcpy(metadata.data(), values.data(), sizeof(values));
    asset::ChunkFileWriter file;
    file.AddChunk(METADATA_CHUNK, std::move(metadata));
    file.AddChunk(DEPTH_CHUNK, std::move(data.depthRgba));
    if (!file.WriteAtomically(path, SHADOW_CACHE_KIND, SourceStamp(key)))
        FBZZ_LOG_WARN("DirectionalShadowBake: disk cache could not be saved: %s", path.c_str());
}

bool BakeOutsideFrame(renderer::IRenderer& renderer, renderer::ResourceManager& resources,
    const renderer::RenderScene& snapshot, const DirectionalBakeData& expected, DirectionalBakeData& data)
{
    /// @note ThreadedRenderer::EndFrame が CPU 記録枠を閉じる。失敗経路も通常フレームへ戻す。
    renderer.WaitForCpuFrame();
    renderer.CancelCpuFrame();
    renderer.BeginCpuFrame(Time::frameCount);
    const bool baked = renderer::BakeDirectionalStaticShadow(renderer, resources, snapshot, expected.coverage,
        expected.lightDirection, expected.resolution, expected.cullingMask, data);
    renderer.WaitForCpuFrame();
    renderer.CancelCpuFrame();
    renderer.BeginCpuFrame(Time::frameCount);
    return baked;
}

bool PreparePreset(renderer::IRenderer& renderer, renderer::ResourceManager& resources,
    const std::string& content, size_t preset, uint32_t resolution)
{
    const DirectionalBakeData expected{s_pending.directions[preset], s_pending.coverage, resolution, s_pending.mask, {}};
    const std::string key = MakeDiskKey(content, expected, renderer.GetBackendName());
    const auto found = std::find_if(s_resident.begin(), s_resident.end(), [&](const ResidentBake& entry) {
        return entry.diskKey == key && resources.Get(entry.texture.Get());
    });
    if (found != s_resident.end()) {
        if (found != s_resident.begin() + static_cast<std::ptrdiff_t>(preset))
            std::swap(*found, s_resident[preset]);
        return true;
    }
    DirectionalBakeData data;
    const char* label = preset == 0 ? "day" : "evening";
    const auto path = util::FileSystem::PathToUtf8(s_pending.directory / (s_pending.diskOwner + "-" + label + ".fzshadow"));
    bool fromDisk = ReadBake(path, key, expected, data);
    renderer::OwnedResource<renderer::TextureTag> texture;
    if (fromDisk) texture.Reset(renderer::UploadDirectionalShadowBake(resources, data));
    if (!texture.IsValid()) {
        fromDisk = false;
        if (!BakeOutsideFrame(renderer, resources, *s_pending.snapshot, expected, data)) return false;
        const bool includeMeshes = renderer::DirectionalShadowBakeIncludesMeshes(expected.coverage, resolution);
        if (StaticContent(*s_pending.snapshot, resources, expected.cullingMask, includeMeshes, false) != content) return false;
        texture.Reset(renderer::UploadDirectionalShadowBake(resources, data));
    }
    if (!texture.IsValid()) return false;
    if (!fromDisk) WriteBake(path, key, data);
    std::vector<uint8_t>().swap(data.depthRgba);
    s_resident[preset] = {content, key, std::move(data), std::move(texture)};
    FBZZ_LOG_INFO("DirectionalShadowBake: %s %s", label, fromDisk ? "loaded" : "generated");
    return true;
}

} /// @note namespace

DirectionalShadowBakeStatus RequestDirectionalShadowBake(Scene& scene, const std::filesystem::path& cacheDirectory)
{
    const auto lightId = SelectBakeLight(scene);
    auto* object = scene.GetGameObject(lightId);
    const auto* light = scene.GetComponent<LightComponent>(lightId);
    if (!object || !light) return Status::NOT_REQUIRED;
    if (s_pending.sceneGeneration == scene.GetRenderSceneGeneration() && s_pending.light == lightId
        && s_pending.status == Status::PENDING) return Status::PENDING;
    s_pending = {};
    s_pending.sceneGeneration = scene.GetRenderSceneGeneration();
    s_pending.requestedFrame = Time::frameCount;
    s_pending.light = lightId;
    s_pending.directory = cacheDirectory;
    asset::BakeFingerprint diskOwner;
    diskOwner.Add(object->instanceId);
    s_pending.diskOwner = diskOwner.Finish();
    s_pending.directions = ResolvePresets(scene, *object, *light);
    s_pending.status = cacheDirectory.empty() || s_pending.directions[0].LengthSq() == 0.0f
        || s_pending.directions[1].LengthSq() == 0.0f ? Status::UNAVAILABLE : Status::PENDING;
    return s_pending.status;
}

DirectionalShadowBakeStatus GetDirectionalShadowBakeStatus(const Scene& scene)
{
    return s_pending.sceneGeneration == scene.GetRenderSceneGeneration() ? s_pending.status : Status::NOT_REQUIRED;
}

void CancelDirectionalShadowBake(const Scene& scene)
{
    if (s_pending.sceneGeneration == scene.GetRenderSceneGeneration() && s_pending.status == Status::PENDING)
        s_pending = {};
}

void ProcessDirectionalShadowBake(Scene& scene, renderer::IRenderer& renderer,
    renderer::ResourceManager& resources, const renderer::RenderSettings& settings)
{
    if (GetDirectionalShadowBakeStatus(scene) != Status::PENDING) return;
    if (!settings.shadowEnabled || settings.shadow.cascadeCount < 2 || SelectBakeLight(scene) != s_pending.light) {
        s_pending.status = Status::UNAVAILABLE;
        s_pending.snapshot.reset();
        return;
    }
    if (!s_pending.snapshot) {
        /// @note Play must still start when a Game View cannot supply a render snapshot.
        if (Time::frameCount - s_pending.requestedFrame >= SNAPSHOT_WAIT_FRAMES)
            s_pending.status = Status::UNAVAILABLE;
        return;
    }
    if (s_pending.resourceOwner != resources.GetInstanceId() || s_pending.resourceEpoch != resources.GetResetVersion()) {
        s_pending.snapshot.reset();
        return;
    }
    const uint32_t farTile = settings.shadow.mapResolution / 2;
    if (farTile < MIN_BAKE_RESOLUTION || farTile > MAX_BAKE_RESOLUTION || !s_pending.coverage.valid
        || !std::isfinite(s_pending.coverage.radius) || s_pending.coverage.radius < 1.0f) {
        s_pending.status = Status::UNAVAILABLE;
        s_pending.snapshot.reset();
        return;
    }
    const float idealResolution = (s_pending.coverage.radius * 2.0f) / TARGET_BAKE_TEXEL_METERS;
    const auto cappedResolution = static_cast<uint32_t>(std::ceil(std::min(idealResolution,
        static_cast<float>(MAX_BAKE_RESOLUTION))));
    const uint32_t resolution = std::bit_ceil(std::max(farTile, cappedResolution));
    const bool includeMeshes = renderer::DirectionalShadowBakeIncludesMeshes(s_pending.coverage, resolution);
    const std::string content = StaticContent(*s_pending.snapshot, resources, s_pending.mask, includeMeshes, true);
    if (content.empty()
        || !PreparePreset(renderer, resources, content, s_pending.nextPreset, resolution)) {
        s_pending.status = Status::UNAVAILABLE;
        s_pending.snapshot.reset();
        FBZZ_LOG_WARN("DirectionalShadowBake: static shadow unavailable; realtime shadows remain active");
        return;
    }
    ++s_pending.nextPreset;
    if (s_pending.nextPreset == s_pending.directions.size()) {
        s_pending.status = Status::READY;
        s_pending.snapshot.reset();
    }
}

void PrepareDirectionalShadowBakeForRender(RenderPassContext& context, EntityID light,
    const renderer::ShadowBounds& sceneBounds, const math::Vector3& lightDirection,
    float shadowBias, const renderer::PreparedShadows& shadows)
{
    if (!context.renderScene) return;
    const uint64_t sceneGeneration = context.scene.GetRenderSceneGeneration();
    const auto direction = BakeDirection(lightDirection);
    const bool stationary = SunStationary(sceneGeneration, light, direction);
    const bool gameView = !context.uiOptions || context.uiOptions->targetView == UIRenderTargetView::GameViewport;
    if (s_pending.status == Status::PENDING && s_pending.sceneGeneration == sceneGeneration
        && s_pending.light == light && gameView && !s_pending.snapshot) {
        s_pending.snapshot = context.renderScene;
        s_pending.coverage = sceneBounds;
        /// @note Bounded motion of realtime receivers can stay inside this fixed projection.
        s_pending.coverage.radius = sceneBounds.radius * 1.125f + 1.0f;
        s_pending.resourceOwner = context.resources.GetInstanceId();
        s_pending.resourceEpoch = context.resources.GetResetVersion();
        s_pending.mask = context.cullingMask;
    }
    if (!stationary || direction.LengthSq() == 0.0f) return;
    for (const auto& entry : s_resident) {
        if (!entry.texture.IsValid() || entry.data.cullingMask != context.cullingMask
            || !SameDirection(direction, entry.data.lightDirection)
            || !context.resources.Get(entry.texture.Get())) continue;
        const bool includeMeshes = renderer::DirectionalShadowBakeIncludesMeshes(entry.data.coverage, entry.data.resolution);
        if (StaticContent(*context.renderScene, context.resources, context.cullingMask, includeMeshes, false) != entry.content)
            continue;
        if (renderer::PrepareDirectionalShadowBakeView(context.resources, entry.data, entry.texture.Get(),
            sceneBounds, direction, context.cullingMask, shadowBias, shadows, context.directionalShadowBake)) return;
    }
}

} /// @note namespace fbzz::scene
