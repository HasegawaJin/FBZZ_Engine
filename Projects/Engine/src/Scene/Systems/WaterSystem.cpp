/// @file    WaterSystem.cpp
/// @brief   水面の実効波の解決と、剛体の着水 (波紋・しぶき・航跡) の検出。
/// @author  Hasegawa Jin
/// @date    2026-09-11
#include <Engine/Scene/Components/WaterComponent.hpp>
#include <Engine/Scene/Systems/WaterSystem.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/DataAssetRegistry.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Asset/WaterWavesAsset.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Core/Scheduler/SystemContext.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Transform.hpp>
#include <Engine/Scene/WaterSurfaceQuery.hpp>
#include <Engine/Scene/WaterSpline.hpp>
#include <Engine/Scene/WaterZoneSnapshot.hpp>
#include <Engine/Scene/Components/WaterZoneComponent.hpp>
#include <Engine/Scene/Components/TargetPointComponent.hpp>
#include <Engine/Scene/WaterDepthField.hpp>
#include <Engine/Scene/WaterExclusion.hpp>
#include <Engine/Scene/Components/WaterExclusionComponent.hpp>
#include <Engine/Scene/Fields/FlowField.hpp>
#include <Engine/Scene/Fields/FlowFieldFrame.hpp>
#include <Engine/Scene/Components/RigidBodyComponent.hpp>
#include <Engine/Scene/WaterInteraction.hpp>
#include <Math/MathUtils.hpp>
#include <Physics/RigidBody.hpp>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fbzz::scene {

namespace {

constexpr GerstnerWave kDefaultWaves[4] = {
    { {  1.00f,  0.20f }, 0.35f, 14.0f, 0.40f },
    { { -0.30f,  0.95f }, 0.20f, 22.0f, 0.30f },
    { {  0.70f, -0.70f }, 0.15f,  9.0f, 0.25f },
    { { -0.90f,  0.40f }, 0.10f, 18.0f, 0.20f },
};

/// @brief 着水の判定。速度は鉛直成分 [m/s]。
constexpr float kMinEntrySpeed   = 1.0f;  ///< @brief これより遅い沈み込みは «浸かった» だけで波紋を出さない
constexpr float kFullSplashSpeed = 8.0f;  ///< @brief この速さで最大のしぶき
constexpr float kMinExitSpeed    = 2.0f;
/// @brief 航跡: 水面をまたいで横へ進む物体が一定間隔で小さな波紋を落とす。
constexpr float kWakeBand     = 0.6f;  ///< @brief [m] 水面からこの範囲に居る物体だけ
constexpr float kWakeSpeed    = 1.5f;  ///< @brief [m/s]
constexpr float kWakeInterval = 0.25f; ///< @brief [s]

float GetF(const asset::MaterialAssetAuthoring* m, const char* key, float def)
{
    if (!m) return def;
    const auto it = m->params.find(key);
    if (it != m->params.end() && !it->second.empty()) return it->second[0];
    return def;
}

math::Vector2 GetF2(const asset::MaterialAssetAuthoring* m, const char* key, math::Vector2 def)
{
    if (!m) return def;
    const auto it = m->params.find(key);
    if (it != m->params.end() && it->second.size() >= 2)
        return { it->second[0], it->second[1] };
    return def;
}

struct WaterMaterialPreparation {
    std::array<GerstnerWave, 4> bases{};
    float grouping = 0.45f;
    float spread = 0.35f;
    float windResponse = 0.0f;
    math::Vector2 flow = {1.0f, 0.0f};
    float speed = 0.0f;
    float flowLength = 1.0f;
    math::Vector2 sceneCurrent = math::Vector2::ZERO;
};

WaterMaterialPreparation BuildWaterMaterialPreparation(const asset::MaterialAssetAuthoring* material)
{
    WaterMaterialPreparation result;
    for (size_t i = 0; i < result.bases.size(); ++i) {
        const auto& defaults = kDefaultWaves[i];
        auto& wave = result.bases[i];
        wave.direction = GetF2(material, water_keys::kWaveDirection[i], defaults.direction);
        wave.amplitude = (std::max)(GetF(material, water_keys::kWaveAmplitude[i], defaults.amplitude), 0.0f);
        wave.wavelength = (std::max)(GetF(material, water_keys::kWaveWavelength[i], defaults.wavelength), 0.01f);
        wave.steepness = math::Clamp01(GetF(material, water_keys::kWaveSteepness[i], defaults.steepness));
    }
    result.grouping = GetF(material, water_keys::kWaveGrouping, 0.45f);
    result.spread = GetF(material, water_keys::kWaveSpread, 0.35f);
    result.windResponse = GetF(material, water_keys::kWindResponse, 0.0f);
    result.flow = GetF2(material, water_keys::kFlowDirection, {1.0f, 0.0f});
    result.speed = (std::max)(GetF(material, water_keys::kCurrentSpeed, 0.0f), 0.0f);
    result.flowLength = result.flow.Length();
    result.sceneCurrent = std::isfinite(result.flowLength) && std::isfinite(result.speed)
        && result.flowLength > 1.0e-4f
        ? result.flow * (result.speed / result.flowLength) : math::Vector2::ZERO;
    return result;
}

/// @note Only owned immutable publications are keys; mutable authoring, Component state and Scene lifetimes are never cached.
/// @note A value copy survives eviction or nested preparation; every caller still resolves the current publication before lookup.
/// @see Docs/design/water-surface-quality.md 抽出候補と水面材質の共通準備。
WaterMaterialPreparation PrepareWaterMaterial(const asset::MaterialAsset::AuthoringSnapshotPtr& publication)
{
    struct Entry {
        asset::MaterialAsset::AuthoringSnapshotPtr publication;
        WaterMaterialPreparation values;
    };
    struct Cache {
        std::array<Entry, 32> entries{};
        size_t next = 0;
    };
    thread_local Cache cache;
    if (!publication) {
        static const auto defaults = BuildWaterMaterialPreparation(nullptr);
        return defaults;
    }
    for (const auto& entry : cache.entries)
        if (entry.publication == publication) return entry.values;
    const auto values = BuildWaterMaterialPreparation(&publication->Data());
    cache.entries[cache.next] = {publication, values};
    cache.next = (cache.next + 1) % cache.entries.size();
    return values;
}

std::uint64_t HashByte(std::uint64_t hash, std::uint8_t value)
{
    return (hash ^ value) * 1099511628211ULL;
}

std::uint64_t HashU32(std::uint64_t hash, std::uint32_t value)
{
    for (unsigned shift = 0; shift < 32; shift += 8)
        hash = HashByte(hash, static_cast<std::uint8_t>(value >> shift));
    return hash;
}

std::uint64_t HashString(std::uint64_t hash, std::string_view value)
{
    for (const char byte : value) hash = HashByte(hash, static_cast<std::uint8_t>(static_cast<unsigned char>(byte)));
    return HashByte(hash, static_cast<std::uint8_t>(0xffu));
}

std::uint64_t WaveSettingsSignature(std::string_view path,
    const fluid::GerstnerWaveGenerationSettings& settings, float grouping, float spread)
{
    std::uint64_t hash = 14695981039346656037ULL;
    hash = HashString(hash, path);
    hash = HashU32(hash, settings.algorithmVersion);
    hash = HashU32(hash, settings.seed);
    hash = HashU32(hash, settings.waveCount);
    hash = HashU32(hash, std::bit_cast<std::uint32_t>(settings.minimumWavelength));
    hash = HashU32(hash, std::bit_cast<std::uint32_t>(settings.maximumWavelength));
    hash = HashU32(hash, std::bit_cast<std::uint32_t>(settings.minimumAmplitude));
    hash = HashU32(hash, std::bit_cast<std::uint32_t>(settings.maximumAmplitude));
    hash = HashU32(hash, std::bit_cast<std::uint32_t>(settings.directionCenterRadians));
    hash = HashU32(hash, std::bit_cast<std::uint32_t>(settings.directionSpreadRadians));
    hash = HashU32(hash, std::bit_cast<std::uint32_t>(settings.minimumSteepness));
    hash = HashU32(hash, std::bit_cast<std::uint32_t>(settings.maximumSteepness));
    hash = HashU32(hash, std::bit_cast<std::uint32_t>(settings.minimumPhaseOffsetRadians));
    hash = HashU32(hash, std::bit_cast<std::uint32_t>(settings.maximumPhaseOffsetRadians));
    hash = HashU32(hash, std::bit_cast<std::uint32_t>(grouping));
    return HashU32(hash, std::bit_cast<std::uint32_t>(spread));
}

std::uint64_t WaveAuthoringSignature(std::uint64_t hash, const WaterWaveAuthoring& authoring)
{
    hash = HashByte(hash, authoring.overrideHeading ? 1u : 0u);
    hash = HashU32(hash, std::bit_cast<std::uint32_t>(authoring.headingDegrees));
    hash = HashU32(hash, std::bit_cast<std::uint32_t>(authoring.spreadDegrees));
    hash = HashU32(hash, std::bit_cast<std::uint32_t>(authoring.secondaryHeadingDegrees));
    return HashU32(hash, std::bit_cast<std::uint32_t>(authoring.secondaryStrength));
}

const asset::WaterWavesAsset* BuiltInSeaPreset(WaterSeaPreset preset)
{
    struct Sea {
        int count;
        float minLength, maxLength, minAmplitude, maxAmplitude, spread, steepness, grouping;
    };
    static constexpr Sea SEAS[] = {
        {4, 2.5f, 9.0f, 0.008f, 0.025f, 0.60f, 0.20f, 0.08f},
        {6, 5.0f, 24.0f, 0.035f, 0.18f, 0.80f, 0.38f, 0.18f},
        {9, 11.0f, 77.0f, 0.22f, 1.35f, 1.35f, 0.68f, 0.24f},
        {12, 15.0f, 110.0f, 0.45f, 2.4f, 1.75f, 0.85f, 0.38f},
    };
    static const auto assets = [] {
        std::array<asset::WaterWavesAsset, std::size(SEAS)> result;
        for (size_t i = 0; i < result.size(); ++i) {
            const auto& sea = SEAS[i];
            auto& asset = result[i];
            asset.seed = 821;
            asset.waveCount = sea.count;
            asset.minimumWavelength = sea.minLength;
            asset.maximumWavelength = sea.maxLength;
            asset.minimumAmplitude = sea.minAmplitude;
            asset.maximumAmplitude = sea.maxAmplitude;
            asset.directionSpreadRadians = sea.spread;
            asset.minimumSteepness = sea.steepness * 0.4f;
            asset.maximumSteepness = sea.steepness;
            asset.waveGrouping = sea.grouping;
        }
        return result;
    }();
    const auto index = static_cast<size_t>(preset);
    return index > 0 && index <= assets.size() ? &assets[index - 1] : nullptr;
}

float WaveHeadingRadians(float degrees)
{
    return std::isfinite(degrees) ? std::remainder(degrees, 360.0f) * (math::PI / 180.0f) : 0.0f;
}

/// @note The seed and random draw order stay fixed when a heading or spread changes.
/// @see https://dev.epicgames.com/documentation/en-us/unreal-engine/simulating-waves-using-the-water-waves-asset-in-unreal-engine Gerstner direction and angular spread.
/// @see https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Plugins/Water/FGerstnerWaveOctave Direction per Gerstner wave group.
bool GenerateWaveBands(const asset::WaterWavesAsset& source, const WaterWaveAuthoring& authoring,
    std::array<GerstnerWave, fluid::MAX_BASE_GERSTNER_WAVES>& waves, std::uint32_t& count,
    math::Vector2& directionDisplay)
{
    if (!source.GenerateWaves(waves)) return false;
    auto settings = source.GenerationSettings();
    count = settings.waveCount;
    const bool overrideSpread = std::isfinite(authoring.spreadDegrees) && authoring.spreadDegrees >= 0.0f;
    if (authoring.overrideHeading) settings.directionCenterRadians = WaveHeadingRadians(authoring.headingDegrees);
    if (overrideSpread)
        settings.directionSpreadRadians = (std::min)(authoring.spreadDegrees * (math::PI / 180.0f), math::TWO_PI);
    if ((authoring.overrideHeading || overrideSpread) && !fluid::GenerateGerstnerWaves(settings, waves)) return false;
    directionDisplay = {
        std::remainder(settings.directionCenterRadians, math::TWO_PI) * (180.0f / math::PI),
        settings.directionSpreadRadians * (180.0f / math::PI)
    };

    if (count == 0 || !std::isfinite(authoring.secondaryStrength) || authoring.secondaryStrength <= 0.0f) return true;
    constexpr std::uint32_t SECONDARY_COUNT = 3;
    const float strength = math::Clamp01(authoring.secondaryStrength);
    settings.seed ^= 0x9E3779B9u;
    settings.waveCount = SECONDARY_COUNT;
    settings.directionCenterRadians = WaveHeadingRadians(authoring.secondaryHeadingDegrees);
    settings.directionSpreadRadians = 0.45f;
    settings.minimumWavelength = (std::max)(0.01f, settings.minimumWavelength * 0.65f);
    settings.maximumWavelength = (std::max)(settings.minimumWavelength, settings.maximumWavelength * 0.65f);
    settings.minimumAmplitude *= strength;
    settings.maximumAmplitude *= strength;
    std::array<GerstnerWave, fluid::MAX_BASE_GERSTNER_WAVES> secondary{};
    if (!fluid::GenerateGerstnerWaves(settings, secondary)) return false;
    count = (std::min)(count, static_cast<std::uint32_t>(waves.size()) - SECONDARY_COUNT);
    std::copy_n(secondary.begin(), SECONDARY_COUNT, waves.begin() + count);
    count += SECONDARY_COUNT;
    return true;
}

std::uint64_t WaveDiagnosticSignature(std::string_view path, std::string_view reason,
    const fluid::GerstnerWaveGenerationSettings& settings, float grouping, float spread, bool hasSettings)
{
    std::uint64_t hash = WaveSettingsSignature(path, settings, grouping, spread);
    hash = HashString(hash, reason);
    return HashByte(hash, hasSettings ? 1u : 0u);
}

uint64_t BodyKey(EntityID id)
{
    return (static_cast<uint64_t>(id.generation) << 32) | id.index;
}

struct SplashTarget {
    WaterComponent*  water;
    const Transform* transform;
    EntityID owner;
};

/// @brief 水面のワールド実寸 [m] (X, Z)。
math::Vector2 WaterWorldSize(const WaterComponent& water, const Transform& transform)
{
    if (UsesWaterSurfaceGeometry(water)) {
        const math::Vector2 localExtent = WaterSurfaceLocalExtent(water);
        const math::Vector3 halfX = transform.worldRotation
            * math::Vector3{localExtent.x * 0.5f * transform.worldScale.x, 0.0f, 0.0f};
        const math::Vector3 halfZ = transform.worldRotation
            * math::Vector3{0.0f, 0.0f, localExtent.y * 0.5f * transform.worldScale.z};
        return {
            (std::max)(2.0f * (std::abs(halfX.x) + std::abs(halfZ.x)), 0.1f),
            (std::max)(2.0f * (std::abs(halfX.z) + std::abs(halfZ.z)), 0.1f),
        };
    }
    return {
        (std::max)(water.extentX * std::abs(transform.worldScale.x), 0.1f),
        (std::max)(water.extentZ * std::abs(transform.worldScale.z), 0.1f),
    };
}

/// @brief 枠を争う 2 本のうち a が残るか。
/// @note 形を持つものが常に勝ち、形の無いものどうしは速い方が残る。形は «その場に無いと シルエットが変わる» のに対し、質感は 1 本落ちても画として崩れないため。
bool IsStrongerSurfaceFlow(const WaterSurfaceFlow& a, const WaterSurfaceFlow& b)
{
    const float heightA = std::abs(a.height);
    const float heightB = std::abs(b.height);
    if (heightA != heightB) return heightA > heightB;
    return std::abs(a.speed) > std::abs(b.speed);
}

const SplashTarget* FindSplashTarget(const std::vector<SplashTarget>& targets, const math::Vector3& p,
    float time, WaterSurfaceReadView view, WaterSurfaceHeightSample& sample)
{
    for (const SplashTarget& target : targets) {
        if (!WaterSurfaceContainsXZ(*target.water, *target.transform, p.x, p.z, time, view)) continue;
        const float bound = target.water->SurfaceHeightBound(view);
        WaterSurfaceHeightSample candidate;
        if (!TrySampleWaterSurfaceHeightWorld(*target.water, *target.transform, p, time, candidate, view)
            || candidate.excluded || candidate.depth > candidate.waterDepthLimit || candidate.depth < -bound) continue;
        sample = candidate;
        return &target;
    }
    return nullptr;
}

}

GerstnerWave DefaultWaterWave(int index)
{
    if (index < 0 || index >= 4) {
        GerstnerWave none;
        none.amplitude = 0.0f;
        return none;
    }
    return kDefaultWaves[index];
}

const asset::MaterialAsset* LoadWaterMaterial(const WaterComponent& water)
{
    if (water.materialPath.empty()) return nullptr;
    return asset::AssetManager::Get<asset::MaterialAsset>(asset::AssetManager::Load<asset::MaterialAsset>(water.materialPath));
}

static asset::MaterialAsset::AuthoringSnapshotPtr LoadWaterMaterialSnapshot(const WaterComponent& water)
{
    if (water.materialPath.empty()) return nullptr;
    return asset::ReadMaterialAuthoring(asset::AssetManager::Load<asset::MaterialAsset>(water.materialPath));
}

namespace {

void ClearWaterSplineSnapshot(WaterComponent& water)
{
    water.splineSnapshot.reset();
    water.splineSnapshotAttempted = false;
    water.splineSnapshotAttemptSignature = 0;
}

bool PrepareWaterSplineSnapshotFromPoints(WaterComponent& water,
    WaterSurfaceShape shape, std::span<const WaterSplinePoint> points, const Transform& transform,
    std::uint64_t sourceSignature)
{
    if (shape == WaterSurfaceShape::RECTANGLE) {
        ClearWaterSplineSnapshot(water);
        return true;
    }
    const std::uint64_t signature = WaterSplineSnapshotSignature(shape, points,
        water.splineSamplesPerSegment, transform, water.buoyancyDepth, water.splineMeshCellSize, water.lakeIslandPoints);
    const std::uint64_t effectiveSignature = HashU32(
        HashU32(signature, static_cast<std::uint32_t>(sourceSignature)),
        static_cast<std::uint32_t>(sourceSignature >> 32));
    if (water.splineSnapshotAttempted && water.splineSnapshotAttemptSignature == effectiveSignature)
        return water.splineSnapshot && water.splineSnapshot->signature == effectiveSignature;
    water.splineSnapshotAttempted = true;
    water.splineSnapshotAttemptSignature = effectiveSignature;
    WaterSplineSnapshot snapshot;
    if (!BuildWaterSplineSnapshot(shape, points, water.splineSamplesPerSegment,
            transform, snapshot, water.buoyancyDepth, water.splineMeshCellSize, water.lakeIslandPoints)) {
        water.splineSnapshot.reset();
        return false;
    }
    snapshot.signature = effectiveSignature;
    water.splineSnapshot = std::make_shared<const WaterSplineSnapshot>(std::move(snapshot));
    return true;
}

} /// @note namespace

bool PrepareWaterSplineSnapshot(WaterComponent& water, const Transform& transform)
{
    const WaterSurfaceShape shape = WaterSurfaceEffectiveShape(water);
    if (shape == WaterSurfaceShape::RECTANGLE) {
        ClearWaterSplineSnapshot(water);
        return true;
    }
    if (water.pointRuntimeMode == ControlPointRuntimeMode::BAKED_STATIC) {
        if (!water.bakeValid || water.bakedSplinePoints.empty()) {
            ClearWaterSplineSnapshot(water);
            return false;
        }
        return PrepareWaterSplineSnapshotFromPoints(water, water.bakedSurfaceShape,
            water.bakedSplinePoints, transform, water.bakedInputSignature);
    }
    if (water.pointSourceMode != ControlPointSourceMode::INLINE) {
        ClearWaterSplineSnapshot(water);
        return false;
    }
    return PrepareWaterSplineSnapshotFromPoints(water, shape, water.splinePoints, transform, 0);
}

bool PrepareWaterSplineSnapshot(Scene& scene, EntityID ownerId,
    WaterComponent& water, const Transform& transform)
{
    if (WaterSurfaceEffectiveShape(water) == WaterSurfaceShape::RECTANGLE) {
        ClearWaterSplineSnapshot(water);
        return true;
    }
    const WaterControlPointSnapshot controls = ResolveWaterControlPoints(scene, ownerId, water,
        water.simulationReadEnabled ? ControlPointReadContext::RUNTIME : ControlPointReadContext::AUTHORING);
    if (!controls.valid) {
        ClearWaterSplineSnapshot(water);
        const std::uint64_t failedSignature = HashU32(
            HashU32(controls.inputSignature, static_cast<std::uint32_t>(controls.surfaceShape)),
            std::bit_cast<std::uint32_t>(water.splineMeshCellSize));
        water.splineSnapshotAttempted = true;
        water.splineSnapshotAttemptSignature = failedSignature == 0 ? 1 : failedSignature;
        return false;
    }
    const std::uint64_t geometrySourceSignature = water.pointRuntimeMode == ControlPointRuntimeMode::BAKED_STATIC
        ? water.bakedInputSignature : controls.inputSignature;
    return PrepareWaterSplineSnapshotFromPoints(water, controls.surfaceShape, controls.points, transform,
        geometrySourceSignature);
}

void PrepareWaterSplineSnapshots(Scene& scene)
{
    for (const EntityID id : scene.GetEntities<WaterComponent>()) {
        auto* water = scene.GetComponent<WaterComponent>(id);
        GameObject* go = scene.GetGameObject(id);
        if (water && go) static_cast<void>(PrepareWaterSplineSnapshot(scene, id, *water, go->transform));
    }
}

namespace {

bool Finite(const math::Vector3& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

std::uint64_t HashZoneU32(std::uint64_t hash, std::uint32_t value)
{
    return HashU32(hash, value);
}

std::uint64_t HashZoneU64(std::uint64_t hash, std::uint64_t value)
{
    hash = HashU32(hash, static_cast<std::uint32_t>(value));
    return HashU32(hash, static_cast<std::uint32_t>(value >> 32));
}

std::uint64_t HashZoneFloat(std::uint64_t hash, float value)
{
    return HashU32(hash, std::bit_cast<std::uint32_t>(value));
}

std::uint64_t HashZoneVector(std::uint64_t hash, const math::Vector3& value)
{
    hash = HashZoneFloat(hash, value.x);
    hash = HashZoneFloat(hash, value.y);
    return HashZoneFloat(hash, value.z);
}

std::uint64_t HashZoneTransform(std::uint64_t hash, const Transform& transform)
{
    hash = HashZoneVector(hash, transform.worldPosition);
    hash = HashZoneVector(hash, transform.worldScale);
    hash = HashZoneFloat(hash, transform.worldRotation.x);
    hash = HashZoneFloat(hash, transform.worldRotation.y);
    hash = HashZoneFloat(hash, transform.worldRotation.z);
    return HashZoneFloat(hash, transform.worldRotation.w);
}

std::uint64_t HashZoneWater(std::uint64_t hash, const WaterComponent& water,
    const Transform& transform)
{
    hash = HashZoneU32(hash, static_cast<std::uint32_t>(water.surfaceShape));
    hash = HashZoneFloat(hash, water.extentX);
    hash = HashZoneFloat(hash, water.extentZ);
    hash = HashZoneFloat(hash, water.buoyancyDepth);
    hash = HashZoneFloat(hash, water.current.x);
    hash = HashZoneFloat(hash, water.current.y);
    hash = HashZoneFloat(hash, water.splineMeshCellSize);
    hash = HashZoneU64(hash, water.splineSnapshot ? water.splineSnapshot->signature
        : water.splineSnapshotAttemptSignature);
    hash = HashZoneU32(hash, water.enabled ? 1u : 0u);
    return HashZoneTransform(hash, transform);
}

std::uint64_t WaterZoneInputSignature(const WaterZoneComponent& zone, EntityID ownerID,
    const WaterComponent& owner, const Transform& ownerTransform, Scene& scene)
{
    std::uint64_t hash = 14695981039346656037ULL;
    hash = HashZoneU64(hash, (static_cast<std::uint64_t>(ownerID.generation) << 32) | ownerID.index);
    hash = HashZoneU32(hash, zone.enabled ? 1u : 0u);
    hash = HashZoneU32(hash, static_cast<std::uint32_t>(zone.ownerPriority));
    hash = HashZoneFloat(hash, zone.meshCellSize);
    hash = HashZoneWater(hash, owner, ownerTransform);
    hash = HashZoneU64(hash, zone.members.size());
    const std::size_t memberCount = (std::min)(zone.members.size(), WATER_ZONE_MAX_BODIES);
    for (std::size_t i = 0; i < memberCount; ++i) {
        const auto& member = zone.members[i];
        hash = HashZoneU64(hash, (static_cast<std::uint64_t>(member.water.id.generation) << 32)
            | member.water.id.index);
        hash = HashZoneU32(hash, static_cast<std::uint32_t>(member.priority));
        if (GameObject* go = member.water.Resolve(scene)) {
            if (WaterComponent* water = scene.GetComponent<WaterComponent>(go->GetID()))
                hash = HashZoneWater(hash, *water, go->transform);
        }
    }
    hash = HashZoneU64(hash, zone.riverMouths.size());
    const std::size_t mouthCount = (std::min)(zone.riverMouths.size(), WATER_ZONE_MAX_MOUTHS + 1);
    for (std::size_t i = 0; i < mouthCount; ++i) {
        const auto& mouth = zone.riverMouths[i];
        hash = HashZoneU64(hash, (static_cast<std::uint64_t>(mouth.river.id.generation) << 32)
            | mouth.river.id.index);
        hash = HashZoneU32(hash, mouth.atStart ? 1u : 0u);
        hash = HashZoneFloat(hash, mouth.blendLength);
    }
    return hash == 0 ? 1 : hash;
}

bool SupportedZoneSourceTransform(const Transform& transform)
{
    const double rotationLength = transform.worldRotation.Length();
    const auto up = transform.worldRotation * math::Vector3::UP;
    return Finite(transform.worldPosition) && Finite(transform.worldScale)
        && std::abs(transform.worldScale.x) >= 0.0001f
        && std::abs(transform.worldScale.y) >= 0.0001f
        && std::abs(transform.worldScale.z) >= 0.0001f
        && std::isfinite(rotationLength) && std::abs(rotationLength - 1.0) <= 0.001
        && Finite(up) && std::abs(up.x) <= 0.00001f && std::abs(up.z) <= 0.00001f
        && std::abs(std::abs(up.y) - 1.0f) <= 0.00001f;
}

bool BuildRectangleZoneSource(const WaterComponent& water, const Transform& transform,
    std::shared_ptr<const WaterSplineSnapshot>& out)
{
    if (!SupportedZoneSourceTransform(transform) || !std::isfinite(water.extentX)
        || !std::isfinite(water.extentZ) || water.extentX <= 0.0f || water.extentZ <= 0.0f
        || !std::isfinite(water.buoyancyDepth) || water.buoyancyDepth < 0.0f
        || !std::isfinite(water.current.x) || !std::isfinite(water.current.y)) return false;
    WaterSplineSnapshot snapshot;
    snapshot.shape = WaterSurfaceShape::RECTANGLE;
    snapshot.worldBaseY = transform.worldPosition.y;
    const float halfX = water.extentX * 0.5f;
    const float halfZ = water.extentZ * 0.5f;
    const std::array<math::Vector3, 4> local = {{
        {-halfX, 0.0f, -halfZ}, {halfX, 0.0f, -halfZ},
        {halfX, 0.0f, halfZ}, {-halfX, 0.0f, halfZ},
    }};
    for (const auto& point : local) {
        const math::Vector3 scaled{point.x * transform.worldScale.x,
            point.y * transform.worldScale.y, point.z * transform.worldScale.z};
        WaterSplineVertex vertex;
        vertex.localPosition = point;
        vertex.worldPosition = transform.worldPosition + transform.worldRotation * scaled;
        vertex.worldVelocity = {water.current.x, 0.0f, water.current.y};
        vertex.depth = water.buoyancyDepth;
        snapshot.vertices.push_back(vertex);
    }
    snapshot.triangles = {{{0, 1, 2}, {}}, {{0, 2, 3}, {}}};
    snapshot.footprintTriangles = snapshot.triangles;
    const float worldX = water.extentX * std::abs(transform.worldScale.x);
    const float worldZ = water.extentZ * std::abs(transform.worldScale.z);
    const float maximumEdge = std::max(std::hypot(worldX, worldZ), 0.05f);
    if (!FinalizeWaterSplineSnapshot(snapshot, maximumEdge)
        || !HasValidWaterSplineExtents(snapshot, transform)) return false;
    out = std::make_shared<const WaterSplineSnapshot>(std::move(snapshot));
    return true;
}

math::Vector3 ZoneSourceWorldPoint(const WaterSplinePoint& point, const Transform& transform)
{
    const math::Vector3 scaled{point.localPosition.x * transform.worldScale.x,
        point.localPosition.y * transform.worldScale.y, point.localPosition.z * transform.worldScale.z};
    return transform.worldPosition + transform.worldRotation * scaled;
}

} /// @note namespace

void PrepareWaterZoneSnapshots(Scene& scene)
{
    for (const EntityID waterID : scene.GetEntities<WaterComponent>()) {
        if (WaterComponent* water = scene.GetComponent<WaterComponent>(waterID)) {
            water->zoneSnapshot.reset();
            water->zoneOwner = EntityID::INVALID;
            water->zoneSurfaceSelected = false;
        }
    }

    std::unordered_map<std::uint64_t, EntityID> claimed;
    for (const EntityID zoneID : scene.GetEntities<WaterZoneComponent>()) {
        WaterZoneComponent* zone = scene.GetComponent<WaterZoneComponent>(zoneID);
        WaterComponent* owner = scene.GetComponent<WaterComponent>(zoneID);
        GameObject* ownerGO = scene.GetGameObject(zoneID);
        if (!zone) continue;
        if (zone->enabled && owner && ownerGO && ownerGO->activeInHierarchy()
            && !zone->members.empty())
            owner->zoneSurfaceSelected = true;
        if (!zone->enabled || !owner || !ownerGO || !ownerGO->activeInHierarchy()
            || zone->members.empty()) {
            zone->snapshot.reset();
            zone->attempted = false;
            zone->attemptSignature = 0;
            zone->diagnostic.clear();
            continue;
        }

        const std::uint64_t signature = WaterZoneInputSignature(*zone, zoneID, *owner, ownerGO->transform, scene);
        const auto keyFor = [](EntityID id) { return (static_cast<std::uint64_t>(id.generation) << 32) | id.index; };
        std::vector<EntityID> memberIDs;
        memberIDs.reserve((std::min)(zone->members.size(), WATER_ZONE_MAX_BODIES));
        std::unordered_set<std::uint64_t> localMembers;
        std::string invalid;
        if (zone->members.size() + 1 > WATER_ZONE_MAX_BODIES)
            invalid = "member count exceeds the Zone limit";
        else if (zone->riverMouths.size() > WATER_ZONE_MAX_MOUTHS)
            invalid = "river mouth count exceeds the Zone limit";
        else if (claimed.contains(keyFor(zoneID)))
            invalid = "Zone owner is already a member of another Zone";

        for (const auto& member : zone->members) {
            if (!invalid.empty()) break;
            GameObject* memberGO = member.water.Resolve(scene);
            if (!memberGO || memberGO->GetID() == zoneID
                || scene.HasComponent<WaterZoneComponent>(memberGO->GetID())
                || !scene.GetComponent<WaterComponent>(memberGO->GetID())
                || !memberGO->activeInHierarchy()) {
                invalid = "a member reference is missing, nested, inactive, or has no Water";
                break;
            }
            const std::uint64_t memberKey = keyFor(memberGO->GetID());
            if (!localMembers.insert(memberKey).second) {
                invalid = "a Water is listed more than once in this Zone";
                break;
            }
            if (claimed.contains(memberKey)) {
                invalid = "a Water is already claimed by an earlier Zone";
                break;
            }
            memberIDs.push_back(memberGO->GetID());
        }

        if (!invalid.empty()) {
            zone->snapshot.reset();
            zone->attempted = false;
            zone->attemptSignature = 0;
            if (zone->diagnostic != invalid)
                FBZZ_LOG_ERROR("WaterSystem: Zone on entity %u failed: %s", zoneID.index, invalid.c_str());
            zone->diagnostic = invalid;
            continue;
        }

        if (zone->attempted && zone->attemptSignature == signature) {
            if (!zone->snapshot) continue;
            owner->zoneSnapshot = zone->snapshot;
            for (const EntityID memberID : memberIDs) {
                claimed.emplace(keyFor(memberID), zoneID);
                if (WaterComponent* water = scene.GetComponent<WaterComponent>(memberID)) {
                    water->zoneOwner = zoneID;
                    water->ripples.clear();
                    water->surfaceFlowCount = 0;
                }
            }
            continue;
        }

        zone->attempted = true;
        zone->attemptSignature = signature;
        zone->snapshot.reset();
        zone->diagnostic.clear();

        if (invalid.empty()) {
            std::vector<WaterZoneSource> sources;
            std::vector<EntityID> sourceIDs;
            sources.reserve(memberIDs.size() + 1);
            sourceIDs.reserve(memberIDs.size() + 1);
            const auto appendSource = [&](EntityID sourceID, int priority) {
                WaterComponent* water = scene.GetComponent<WaterComponent>(sourceID);
                GameObject* go = scene.GetGameObject(sourceID);
                if (!water || !go || !water->enabled || !go->activeInHierarchy()) return false;
                std::shared_ptr<const WaterSplineSnapshot> geometry;
                const WaterSurfaceShape shape = WaterSurfaceEffectiveShape(*water);
                if (shape == WaterSurfaceShape::RECTANGLE) {
                    if (!BuildRectangleZoneSource(*water, go->transform, geometry)) return false;
                } else {
                    if (!water->splineSnapshot || water->splineSnapshot->shape != shape
                        || !HasValidWaterSplineExtents(*water->splineSnapshot, go->transform)) return false;
                    geometry = water->splineSnapshot;
                }
                sources.push_back({std::move(geometry), priority});
                sourceIDs.push_back(sourceID);
                return true;
            };
            if (!appendSource(zoneID, zone->ownerPriority)) invalid = "Zone owner geometry is invalid";
            for (std::size_t i = 0; invalid.empty() && i < memberIDs.size(); ++i)
                if (!appendSource(memberIDs[i], zone->members[i].priority)) invalid = "a member geometry is invalid";

            std::vector<WaterZoneMouthInput> mouthInputs;
            for (const auto& authoredMouth : zone->riverMouths) {
                if (!invalid.empty()) break;
                GameObject* riverGO = authoredMouth.river.Resolve(scene);
                if (!riverGO || riverGO->GetID() == zoneID) {
                    invalid = "a river mouth reference is missing or is not a member";
                    break;
                }
                const auto sourceIt = std::find(sourceIDs.begin() + 1, sourceIDs.end(), riverGO->GetID());
                if (sourceIt == sourceIDs.end()) {
                    invalid = "a river mouth reference is not a Zone member";
                    break;
                }
                WaterComponent* river = scene.GetComponent<WaterComponent>(riverGO->GetID());
                if (!river || WaterSurfaceEffectiveShape(*river) != WaterSurfaceShape::SPLINE_RIVER) {
                    invalid = "a river mouth source is not a valid spline river";
                    break;
                }
                const WaterControlPointSnapshot controls = ResolveWaterControlPoints(
                    scene, riverGO->GetID(), *river, river->simulationReadEnabled
                        ? ControlPointReadContext::RUNTIME : ControlPointReadContext::AUTHORING);
                if (!controls.valid || controls.points.size() < 2) {
                    invalid = "a river mouth source has invalid control points";
                    break;
                }
                const std::size_t count = controls.points.size();
                const std::size_t endpointIndex = authoredMouth.atStart ? 0 : count - 1;
                const std::size_t inwardIndex = authoredMouth.atStart ? 1 : count - 2;
                const WaterSplinePoint& endpointPoint = controls.points[endpointIndex];
                const math::Vector3 endpoint = ZoneSourceWorldPoint(endpointPoint, riverGO->transform);
                const math::Vector3 localDirection = controls.points[inwardIndex].localPosition
                    - endpointPoint.localPosition;
                const math::Vector3 scaledDirection{localDirection.x * riverGO->transform.worldScale.x,
                    localDirection.y * riverGO->transform.worldScale.y,
                    localDirection.z * riverGO->transform.worldScale.z};
                const math::Vector3 inward = riverGO->transform.worldRotation * scaledDirection;
                const double length = std::hypot(static_cast<double>(inward.x), static_cast<double>(inward.z));
                if (!Finite(endpoint) || !std::isfinite(length) || length <= 1.0e-6) {
                    invalid = "a river mouth endpoint direction is degenerate";
                    break;
                }
                const std::size_t sourceIndex = static_cast<std::size_t>(sourceIt - sourceIDs.begin());
                mouthInputs.push_back({static_cast<std::uint32_t>(sourceIndex), endpoint,
                    {static_cast<float>(inward.x / length), static_cast<float>(inward.z / length)},
                    authoredMouth.blendLength});
            }
            if (invalid.empty()) {
                WaterZoneSnapshot built;
                if (!BuildWaterZoneSnapshot(sources, mouthInputs, ownerGO->transform,
                        owner->buoyancyDepth, zone->meshCellSize, built, invalid)) {
                    if (invalid.empty()) invalid = "Zone numeric snapshot build failed";
                } else {
                    built.geometry.signature = signature;
                    zone->snapshot = std::make_shared<const WaterZoneSnapshot>(std::move(built));
                    zone->diagnostic.clear();
                }
            }
        }

        if (!invalid.empty()) {
            zone->snapshot.reset();
            if (zone->diagnostic != invalid)
                FBZZ_LOG_ERROR("WaterSystem: Zone on entity %u failed: %s", zoneID.index, invalid.c_str());
            zone->diagnostic = invalid;
            continue;
        }

        owner->zoneSnapshot = zone->snapshot;
        for (const EntityID memberID : memberIDs) {
            claimed.emplace(keyFor(memberID), zoneID);
            if (WaterComponent* water = scene.GetComponent<WaterComponent>(memberID)) {
                water->zoneOwner = zoneID;
                water->ripples.clear();
                water->surfaceFlowCount = 0;
            }
        }
    }
}

void PrepareWaterSceneSurfaces(Scene& scene)
{
    PrepareWaterSplineSnapshots(scene);
    for (const EntityID id : scene.GetEntities<WaterComponent>()) {
        WaterComponent* water = scene.GetComponent<WaterComponent>(id);
        if (!water || water->surfaceShape != WaterSurfaceShape::RECTANGLE) continue;
        const auto materialSnapshot = LoadWaterMaterialSnapshot(*water);
        water->current = PrepareWaterMaterial(materialSnapshot).sceneCurrent;
    }
    PrepareWaterZoneSnapshots(scene);
    PrepareWaterExclusionSnapshots(scene);
}

namespace {

void CaptureWaterSimulationSurfaces(Scene& scene, bool unpublishedOnly = false)
{
    for (const EntityID id : scene.GetEntities<WaterComponent>()) {
        WaterComponent* water = scene.GetComponent<WaterComponent>(id);
        const GameObject* go = scene.GetGameObject(id);
        if (!water || !go) continue;
        water->simulationReadEnabled = true;
        if (unpublishedOnly && water->simulationSurfacePublished) continue;
        water->simulationSplineSnapshot = water->splineSnapshot;
        water->simulationZoneSnapshot = water->zoneSnapshot;
        water->simulationExclusionSnapshot = water->exclusionSnapshot;
        water->simulationDepthField = water->depthField;
        water->simulationWaveAttenuationDepth = water->waveAttenuationDepth;
        water->simulationTransform = go->transform;
        water->simulationSurfaceShape = WaterSurfaceEffectiveShape(*water);
        water->simulationZoneSurfaceSelected = water->zoneSurfaceSelected;
        water->simulationZoneOwner = water->zoneOwner;
        water->simulationSurfacePublished = true;
    }
    for (const EntityID id : scene.GetEntities<WaterZoneComponent>()) {
        WaterZoneComponent* zone = scene.GetComponent<WaterZoneComponent>(id);
        const GameObject* go = scene.GetGameObject(id);
        if (!zone || !go) continue;
        if (unpublishedOnly && zone->simulationPublished) continue;
        zone->simulationSnapshot = zone->snapshot;
        zone->simulationOwnerTransform = go->transform;
        zone->simulationPublished = true;
    }
}

} /// @note namespace

void PrepareWaterSimulationSurfaces(Scene& scene)
{
    for (const EntityID id : scene.GetEntities<WaterComponent>()) {
        if (WaterComponent* water = scene.GetComponent<WaterComponent>(id))
            water->simulationReadEnabled = true;
    }
    PrepareWaterSceneSurfaces(scene);
    PrepareWaterDepthFields(scene, WaterSurfaceReadView::LIVE_QUERY);
    CaptureWaterSimulationSurfaces(scene);
}

void EnsureWaterSimulationSurfaces(Scene& scene)
{
    bool needsInitialPublication = false;
    for (const EntityID id : scene.GetEntities<WaterComponent>()) {
        const WaterComponent* water = scene.GetComponent<WaterComponent>(id);
        if (water && !water->simulationSurfacePublished) {
            needsInitialPublication = true;
            break;
        }
    }
    if (!needsInitialPublication) return;
    for (const EntityID id : scene.GetEntities<WaterComponent>()) {
        if (WaterComponent* water = scene.GetComponent<WaterComponent>(id))
            water->simulationReadEnabled = true;
    }
    PrepareWaterSceneSurfaces(scene);
    PrepareWaterDepthFields(scene, WaterSurfaceReadView::LIVE_QUERY);
    CaptureWaterSimulationSurfaces(scene, true);
}

WaterComponent* ResolveWaterZoneOwner(Scene& scene, EntityID entity)
{
    WaterComponent* water = scene.GetComponent<WaterComponent>(entity);
    if (!water) return nullptr;
    if (water->zoneOwner.IsValid()) {
        if (WaterComponent* owner = scene.GetComponent<WaterComponent>(water->zoneOwner))
            return owner;
    }
    return water;
}

namespace {

bool ResolveWaterWaveBases(WaterComponent& water, const WaterMaterialPreparation& material,
                           const WaterWind& wind, std::span<const GerstnerWave> bases,
                           float grouping, float spread, bool constrainContraction)
{
    if (bases.size() > fluid::MAX_BASE_GERSTNER_WAVES) return false;
    if (constrainContraction) {
        if (!std::isfinite(grouping) || grouping < 0.0f || grouping > 1.0f
            || !std::isfinite(spread) || spread < 0.0f || spread > 1.0f) return false;
        for (const GerstnerWave& wave : bases) {
            if (!std::isfinite(wave.direction.x) || !std::isfinite(wave.direction.y)
                || !std::isfinite(wave.amplitude) || wave.amplitude < 0.0f
                || !std::isfinite(wave.wavelength) || wave.wavelength <= 0.0f
                || !std::isfinite(wave.steepness) || wave.steepness < 0.0f || wave.steepness > 1.0f
                || !std::isfinite(wave.phaseOffsetRadians)) return false;
        }
    }

    const float scaleInput = constrainContraction && !std::isfinite(water.waveAmplitudeScale)
        ? 0.0f : water.waveAmplitudeScale;
    const float scale = water.enableGerstnerWaves
        ? (constrainContraction
            ? (std::min)((std::max)(scaleInput, 0.0f), 1000000.0f)
            : (std::max)(scaleInput, 0.0f))
        : 0.0f;
    float response = water.waveAuthoring.preset == WaterSeaPreset::AUTHORED ? material.windResponse : 0.0f;
    if (constrainContraction && !std::isfinite(response)) response = 0.0f;
    response = math::Clamp01(response);
    const bool validWind = std::isfinite(wind.speed) && std::isfinite(wind.direction.x)
        && std::isfinite(wind.direction.y) && std::isfinite(wind.direction.z);
    const float windGain = wind.active && validWind
        ? response * math::Clamp(wind.speed / kWaveGrowthReferenceWindSpeed, 0.0f, 2.0f)
        : 0.0f;
    const float windLen = validWind
        ? std::sqrt(wind.direction.x * wind.direction.x + wind.direction.z * wind.direction.z) : 0.0f;
    const bool hasWindDir = windLen > 1.0e-3f;
    const math::Vector2 windDir = hasWindDir
        ? math::Vector2{ wind.direction.x / windLen, wind.direction.z / windLen }
        : math::Vector2::ZERO;

    std::array<GerstnerWave, fluid::MAX_BASE_GERSTNER_WAVES> resolved{};
    for (size_t i = 0; i < bases.size(); ++i) {
        GerstnerWave wave = bases[i];
        if (windGain > 0.0f && hasWindDir) {
            const float dirLen = wave.direction.Length();
            const float align = dirLen > 1.0e-4f
                ? math::Vector2::Dot(wave.direction * (1.0f / dirLen), windDir)
                : 0.0f;
            /// @note 向かい風で潰れる量は、追い風で育つ量の半分に留める。
            const float gain = align >= 0.0f
                ? 1.0f + windGain * align
                : 1.0f + windGain * align * 0.5f;
            wave.amplitude *= (std::max)(gain, 0.2f);
            wave.steepness = math::Clamp01(wave.steepness * (1.0f + windGain * 0.25f));
        }
        wave.amplitude *= scale;
        resolved[i] = wave;
    }

    const float resolvedGrouping = scale > 0.0f ? math::Clamp01(grouping) : 0.0f;
    const float resolvedSpread = scale > 0.0f ? math::Clamp01(spread) : 0.0f;
    if (constrainContraction
        && !fluid::LimitGerstnerWaveContraction(
            std::span<GerstnerWave>(resolved.data(), bases.size()),
            resolvedGrouping, resolvedSpread, 0.95f)) {
        return false;
    }

    math::Vector2 flow = material.flow;
    float speed = material.speed;
    float flowLen = material.flowLength;
    if (constrainContraction) {
        if (!std::isfinite(flowLen) || !std::isfinite(speed)) {
            flow = math::Vector2::ZERO;
            speed = 0.0f;
            flowLen = 0.0f;
        }
        speed = (std::min)(speed, 1000000.0f);
    }
    water.waves = resolved;
    water.waveCount = static_cast<std::uint32_t>(bases.size());
    water.waveGrouping = resolvedGrouping;
    water.waveSpread = resolvedSpread;
    water.current = flowLen > 1.0e-4f ? flow * (speed / flowLen) : math::Vector2::ZERO;
    water.rippleDirectionOverride = math::Vector2::ZERO;
    if (water.waveAuthoring.rippleMode == WaterRippleDirectionMode::MANUAL) {
        const float angle = WaveHeadingRadians(water.waveAuthoring.rippleHeadingDegrees);
        water.rippleDirectionOverride = {std::cos(angle), std::sin(angle)};
    } else if (water.waveAuthoring.rippleMode == WaterRippleDirectionMode::SCENE_WIND
               && wind.active && hasWindDir) {
        water.rippleDirectionOverride = windDir;
    }
    return true;
}

} /// @note namespace

static bool ResolveWaterWavesFromValues(WaterComponent& water, const WaterMaterialPreparation& material,
                                const WaterWind& wind, const asset::WaterWavesAsset& wavesAsset)
{
    std::array<GerstnerWave, fluid::MAX_BASE_GERSTNER_WAVES> generated{};
    std::uint32_t count = 0;
    math::Vector2 display;
    if (!GenerateWaveBands(wavesAsset, water.waveAuthoring, generated, count, display)
        || !ResolveWaterWaveBases(water, material, wind, std::span(generated.data(), count),
            wavesAsset.waveGrouping, wavesAsset.waveSpread, true)) return false;
    water.waveDirectionDisplay = display;
    return true;
}

static void ResolveWaterWavesValues(WaterComponent& water, const WaterMaterialPreparation& material, const WaterWind& wind)
{
    const auto* preset = BuiltInSeaPreset(water.waveAuthoring.preset);
    if (preset && ResolveWaterWavesFromValues(water, material, wind, *preset)) return;
    water.waveDirectionDisplay = math::Vector2::ZERO;
    ResolveWaterWaveBases(water, material, wind, material.bases, material.grouping, material.spread, false);
}

void ResolveWaterWaves(WaterComponent& water, const asset::MaterialAsset* material, const WaterWind& wind)
{
    const auto snapshot = material ? material->ReadAuthoring() : asset::MaterialAsset::AuthoringSnapshotPtr{};
    ResolveWaterWavesValues(water, PrepareWaterMaterial(snapshot), wind);
}

void ResolveWaterWaves(WaterComponent& water, const asset::MaterialAssetAuthoring& material, const WaterWind& wind)
{
    ResolveWaterWavesValues(water, BuildWaterMaterialPreparation(&material), wind);
}

bool ResolveWaterWavesFromAsset(WaterComponent& water, const asset::MaterialAsset* material,
    const WaterWind& wind, const asset::WaterWavesAsset& wavesAsset)
{
    const auto snapshot = material ? material->ReadAuthoring() : asset::MaterialAsset::AuthoringSnapshotPtr{};
    return ResolveWaterWavesFromValues(water, PrepareWaterMaterial(snapshot), wind, wavesAsset);
}

math::Vector2 ResolveWaterCellSize(const WaterComponent& water, const Transform& transform)
{
    if (UsesWaterSurfaceGeometry(water)) {
        const WaterSplineSnapshot* geometry = WaterSurfaceGeometry(water);
        const float cellSize = geometry
            ? (std::max)(geometry->maximumWorldEdgeLength, 0.001f)
            : (std::max)(water.splineMeshCellSize, 0.001f);
        return {cellSize, cellSize};
    }
    math::Vector2 cell;
    cell = {
        water.extentX * std::abs(transform.worldScale.x) / static_cast<float>((std::max)(water.resolutionX, 1u)),
        water.extentZ * std::abs(transform.worldScale.z) / static_cast<float>((std::max)(water.resolutionZ, 1u)),
    };
    if (water.cameraFocusedGrid) {
        /// @note 物理の波は最密区間のスペクトルを使い、SceneView と GameView のカメラ変更では動かさない。
        const float nearCell = (std::clamp)(water.nearCellSize, 0.25f, 10.0f);
        cell.x = (std::min)(cell.x, nearCell);
        cell.y = (std::min)(cell.y, nearCell);
    }
    return cell;
}

void EmitWaterRipple(WaterComponent& water, const Transform& transform,
                     const math::Vector3& worldPos, float strength)
{
    const math::Vector2 size = WaterWorldSize(water, transform);

    /// @note 波紋テクスチャは水面全体で 1 枚。テクセルより細い輪は焼いても «輪» にならないので、 テクセルのワールド実寸を太さの下限にする。
    const float mean   = (size.x + size.y) * 0.5f;
    const float texel  = mean / static_cast<float>(kWaterRippleTextureSize);
    const float s      = math::Clamp01(strength);
    const float width  = (std::max)(0.25f + 0.35f * s, texel * 1.5f);
    const float speed  = (std::max)(1.2f + 1.2f * s, texel * 3.0f);

    /// @note テクセルで止めた «実際に出る輪の太さ» [m]。海サイズの水面では 1 テクセルが数十 m あり、 小石 1 個の着水が直径 100 m の輪として出てしまう。太くなったぶんだけ薄くし、 «輪として読めない» ところで置くのをやめる。しぶきのパーティクルはそのまま残る。
    const float t = math::Clamp01((width - 3.0f) / 5.0f);
    const float scaleFade = 1.0f - t * t * (3.0f - 2.0f * t);
    if (scaleFade <= 0.01f) return;

    WaterRipple ripple;
    ripple.center    = { worldPos.x, worldPos.z };
    ripple.radius    = 0.0f;
    ripple.width     = width;
    ripple.amplitude = s * scaleFade;
    ripple.speed     = speed;
    ripple.decayRate = 1.3f;
    /// @note 断面 sin(ring/w·PI) の周期は 2·width。«波長 / セル» で測る WaveMeshFade へは その周期を渡す (帯の原則は Gerstner 波と同じ尺で決める)。
    ripple.meshFade  = WaterComponent::WaveMeshFade(width * 2.0f, water.cellSize);
    /// @note 輪は «水面を横切り切ったら» 用済み。UV 時代の 2.8 (= 幅の 2.8 倍) を実寸へ写した値。
    ripple.maxRadius = mean * 2.8f;

    /// @note 波紋テクスチャは «テクセル数 × 波紋数» を毎フレーム CPU で焼き直す。物体を大量に 水へ落とすと波紋の数だけフレームが重くなるので、上限を置いて古いものから捨てる。
    if (water.ripples.size() >= kMaxWaterRipples)
        water.ripples.erase(water.ripples.begin());
    water.ripples.push_back(ripple);
}

void UpdateWaterRipples(WaterComponent& water, const Transform& transform, float dt)
{
    if (water.ripples.empty()) return;
    const math::Vector2 size = WaterWorldSize(water, transform);
    const float mean = (size.x + size.y) * 0.5f;

    for (WaterRipple& ripple : water.ripples) {
        ripple.radius += ripple.speed * dt;
        ripple.amplitude *= std::exp(-ripple.decayRate * dt);
        /// @note セルの実寸は resolution / extent / 親スケールで動く。寝かせ方も毎フレーム測り直す。
        ripple.meshFade = WaterComponent::WaveMeshFade(ripple.width * 2.0f, water.cellSize);
        ripple.maxRadius = mean * 2.8f;
    }
    std::erase_if(water.ripples, [](const WaterRipple& ripple) {
        return ripple.amplitude < 0.001f || ripple.radius > ripple.maxRadius;
    });
}

void ResolveWaterSurfaceFlows(WaterComponent& water, const Transform& transform,
                              const std::vector<ActiveFlowField>& fields)
{
    water.surfaceFlowCount = 0;
    water.surfaceFlows = {};

    const math::Vector2 size = WaterWorldSize(water, transform);
    const float halfX = size.x * 0.5f;
    const float halfZ = size.y * 0.5f;

    for (const ActiveFlowField& field : fields) {
        if (!FlowReceiver{}.Accepts(field)) continue;
        /// @note 流れではない。読み込み時に flowCoupling へ写るので、ここまで届くのは移行漏れ。
        if (field.type == FlowFieldType::LegacyDrag) continue;

        WaterSurfaceFlow flow;
        flow.kind         = field.type;
        flow.center       = { field.position.x, field.position.z };
        flow.radius       = field.radius;
        flow.falloffPower = field.falloffPower;
        flow.speed        = std::abs(field.strength);
        float reach = field.radius;

        if (field.type == FlowFieldType::Baked) {
            if (field.vectorField == nullptr || field.vectorField->Empty()) continue;
            flow.vectorField     = field.vectorField;
            flow.inverseRotation = field.inverseRotation;
            flow.extents         = field.extents;
            /// @note Baked の strength は «焼いた値 [m/s] に掛ける無次元の倍率» で符号を持つ。
            flow.speed           = field.strength;
            flow.planeOffsetY    = transform.worldPosition.y - field.position.y;
            /// @note 回した箱を軸並行で包む代わりに対角長で見る。枠に入るかの粗いふるいなので、 安全側に大きく取って構わない。
            reach = field.extents.Length();
            if (std::abs(flow.planeOffsetY) > reach) continue;
            /// @note 特徴の大きさは «狭い辺の半分»。extents は箱の半径で、場の中の起伏は箱より 細かい。全幅で測ると、刻めない場が縞として残る。
            const float feature = (std::min)(field.extents.x, field.extents.z);
            flow.height = -kMaxWaterSurfaceDisplacement
                        * WaterComponent::WaveMeshFade(feature, water.cellSize);
        } else {
            /// @note 半径の無い («シーン全体») 要素は中心からの距離で減衰しないので、水面のどこまで 効くのか決まらない。流速としては効いたままで、形と質感にだけ出さない。
            if (field.radius <= 0.0001f) continue;
            /// @note 波による変位を含めると形が自身の高さへ依存するため、水面の基準面と球の断面で解決する。
            /// @see Docs/design/water-waves.md 「水面と場の高低差」
            const float distanceY = std::abs(transform.worldPosition.y - field.position.y);
            if (distanceY >= field.radius) continue;
            const float heightRatio = distanceY / field.radius;
            const float planeInfluence = std::pow(1.0f - heightRatio, field.falloffPower);
            flow.radius = field.radius * std::sqrt((1.0f - heightRatio) * (1.0f + heightRatio));
            reach = flow.radius;
            /// @note Bernoulli。strength は流速 [m/s] なので v^2/2g がそのまま長さになる。
            const float bernoulli =
                (std::min)(flow.speed * flow.speed / 19.6f, kMaxWaterSurfaceDisplacement)
                /// @note 上限を掛けた後で減衰させ、強い場も Y 移動で直ちに浅くなるようにする。
                * planeInfluence * planeInfluence
                /// @note 頂点で刻めない形は消す。波長にあたるのは形の差し渡し = 2·radius。
                * WaterComponent::WaveMeshFade(flow.radius * 2.0f, water.cellSize);
            flow.speed *= planeInfluence;
            const bool wantsSwell = field.waterSwellAmplitude > 0.0f
                && std::isfinite(field.waterSwellAmplitude) && std::isfinite(field.waterSwellWavelength)
                && field.waterSwellWavelength >= 2.0f && field.waterSwellWavelength <= 1000.0f;
            const auto setSwellBand = [&] {
                auto nearCell = water.cellSize;
                if ((water.adaptivePatchLod || WaterSurfaceUsesHorizon(water)) && !UsesWaterSurfaceGeometry(water)) {
                    nearCell.x = (std::min)(nearCell.x, water.lodNearCellSize);
                    nearCell.y = (std::min)(nearCell.y, water.lodNearCellSize);
                }
                flow.swellWavelength = field.waterSwellWavelength;
                flow.swellNearFade = WaterComponent::WaveMeshFade(
                    (std::min)(flow.swellWavelength, 2.0f * flow.radius), nearCell);
            };
            switch (field.type) {
            case FlowFieldType::Uniform: {
                /// @note 真上・真下を向いた流れは水面を撫でない。水平成分だけを «風» として読む。
                const float dirLen = std::sqrt(field.direction.x * field.direction.x
                                             + field.direction.z * field.direction.z);
                if (dirLen > 1.0e-4f)
                    flow.direction = { field.direction.x / dirLen, field.direction.z / dirLen };
                flow.speed *= dirLen;
                flow.chop = math::Clamp01(flow.speed / kWaterSurfaceFlowReference);
                if (dirLen > 1.0e-4f && wantsSwell) {
                    setSwellBand();
                    /// @note 波長に対する振幅を制限し、短波を急峻にしすぎない。
                    flow.height = (std::min)({field.waterSwellAmplitude, kMaxWaterSurfaceDisplacement,
                                              0.07f * flow.swellWavelength})
                                * planeInfluence * flow.swellNearFade;
                }
                break;
            }
            case FlowFieldType::Curl:
                flow.chop = math::Clamp01(flow.speed / kWaterSurfaceFlowReference);
                break;
            case FlowFieldType::Sink:
                flow.height = -bernoulli;
                break;
            case FlowFieldType::Source:
                flow.height = bernoulli;
                break;
            case FlowFieldType::Vortex:
                /// @note 横倒しの渦は水面を «掘る» のではなく撫でる。解析項として書けない。
                if (std::abs(field.direction.y) < kWaterVortexUprightDot) continue;
                flow.height = -bernoulli;
                flow.speed  = field.direction.y >= 0.0f ? flow.speed : -flow.speed;
                if (wantsSwell) {
                    setSwellBand();
                    /// @note Explicit vortex depth is an art control, limited relative to its horizontal support.
                    flow.height = -(std::min)({field.waterSwellAmplitude, 24.0f, 0.45f * flow.radius})
                                * planeInfluence * flow.swellNearFade;
                }
                break;
            default:
                continue;
            }
        }

        /// @note 形を持つ型が «刻めない» ときは枠を使わない。占めたままだと、頂点に出ない形の ために Uniform / Curl の質感が押し出される。
        const bool shapeless = flow.kind == FlowFieldType::Uniform
                            || flow.kind == FlowFieldType::Curl;
        if (!shapeless && std::abs(flow.height) <= 0.0001f) continue;

        const float dx = field.position.x - transform.worldPosition.x;
        const float dz = field.position.z - transform.worldPosition.z;
        if ((std::abs(dx) > halfX + reach || std::abs(dz) > halfZ + reach)
            && !(flow.swellWavelength > 0.0f && flow.height != 0.0f && WaterSurfaceUsesHorizon(water))) continue;

        /// @note 枠が空いていれば足し、埋まっていれば «一番弱い枠» と競らせる。
        if (water.surfaceFlowCount < kWaterSurfaceFlowCount) {
            water.surfaceFlows[static_cast<size_t>(water.surfaceFlowCount)] = flow;
            ++water.surfaceFlowCount;
            continue;
        }
        int weakest = 0;
        for (int i = 1; i < kWaterSurfaceFlowCount; ++i) {
            if (IsStrongerSurfaceFlow(water.surfaceFlows[static_cast<size_t>(weakest)],
                                      water.surfaceFlows[static_cast<size_t>(i)]))
                weakest = i;
        }
        if (IsStrongerSurfaceFlow(flow, water.surfaceFlows[static_cast<size_t>(weakest)]))
            water.surfaceFlows[static_cast<size_t>(weakest)] = flow;
    }
}

/// @brief Return the local base current plus explicit field contributions at a water query point.
/// @note Spline vertex velocity replaces material current so CPU queries match the renderer's vertex payload.
math::Vector3 WaterFlowVelocityAt(const WaterComponent& water,
                                  const std::vector<ActiveFlowField>& fields,
                                  float surfaceBaseY,
                                  const math::Vector3& worldPos,
                                  float time,
                                  WaterSurfaceReadView view)
{
    const bool geometrySurface = UsesWaterSurfaceGeometry(water, view);
    const WaterSplineSnapshot* geometry = WaterSurfaceGeometry(water, view);
    math::Vector3 splineFlow = math::Vector3::ZERO;
    if (geometrySurface && geometry) {
        const math::Vector2 origin = water.SolveUndisplacedXZ(worldPos.x, worldPos.z, time, view);
        WaterSplineSample splineSample;
        if (TrySampleWaterSpline(*geometry, origin.x, origin.y, splineSample))
            splineFlow = splineSample.worldVelocity;
    }
    const math::Vector2 baseFlow = geometrySurface
        ? math::Vector2{splineFlow.x, splineFlow.z} : water.current;
    if (fields.empty())
        return {baseFlow.x, 0.0f, baseFlow.y};
    const math::Vector3 surfacePoint = {
        worldPos.x,
        surfaceBaseY + water.GetSurfaceHeightAt(worldPos.x, worldPos.z, time, view),
        worldPos.z,
    };
    /// @note 全ビットで引く。剛体にはチャンネルの申告が無いので «どの場も受ける»。
    math::Vector3 flow = math::Vector3::ZERO;
    for (const auto& field : fields) {
        /// @note 水域の所属と抵抗は Water が持つため、流速 0 の場の未使用形状・noise を評価する必要はない。
        if (field.strength == 0.0f) continue;
        flow += FlowReceiver{}.Sample(surfacePoint, std::span<const ActiveFlowField>{&field, 1}, time);
    }
    /// @note 鉛直成分は捨てる。水面の «表面の流速» は水平にしか意味がなく、y を渡すと 浮いた体が浮力と綱引きして水面の上で震える。
    return {baseFlow.x + flow.x, 0.0f, baseFlow.y + flow.z};
}

ComponentAccess WaterSystem::GetAccess() const
{
    return ComponentAccess{}
        .Reads<RigidBodyComponent, FlowField, WaterZoneComponent, WaterExclusionComponent,
               TargetPointComponent, Transform>()
        .Writes<WaterComponent, WaterZoneComponent, WaterExclusionComponent>();
}

void WaterSystem::Update(SystemContext& ctx)
{
    Scene& scene = ctx.scene;
    for (const EntityID id : scene.GetEntities<WaterComponent>()) {
        if (WaterComponent* water = scene.GetComponent<WaterComponent>(id))
            water->simulationReadEnabled = ctx.playing || ctx.simulating;
    }
    PrepareWaterSplineSnapshots(scene);
    const std::uint64_t sceneGeneration = scene.GetRenderSceneGeneration();
    if (m_waveAssetSceneGeneration != sceneGeneration) {
        m_waveAssets.clear();
        m_bodies.clear();
        m_waveAssetSceneGeneration = sceneGeneration;
    }
    /// @note 環境流は Scene のフレームキャッシュが正本。GameObject の並び順で勝者が決まる «最初に見つかった radius 0 の Wind» の探索はもう無い。
    const FlowFieldFrame& flowFrame = scene.FlowFrame();
    const AmbientWind& ambient = flowFrame.ambient;
    const WaterWind wind{ ambient.active, ambient.direction, ambient.speed };

    for (auto& entry : m_waveAssets) entry.second.seen = false;

    std::vector<SplashTarget> targets;
    for (EntityID id : scene.GetEntities<WaterComponent>()) {
        auto* water = scene.GetComponent<WaterComponent>(id);
        GameObject* go = scene.GetGameObject(id);
        /// @note 親ごと無効化された水面は波・流れ・波紋も止める。描画・浮力・スクリプトの問い合わせはどれも無効な水面を見ないので、準備だけ進めても使われない。
        if (!water || !go || !go->activeInHierarchy()) continue;
        const auto materialSnapshot = LoadWaterMaterialSnapshot(*water);
        const auto material = PrepareWaterMaterial(materialSnapshot);
        WaveAssetCache& cache = m_waveAssets[BodyKey(id)];
        cache.seen = true;
        const std::string& referencePath = water->wavesAssetRef.path;
        const auto* preset = BuiltInSeaPreset(water->waveAuthoring.preset);
        if (referencePath.empty() && !preset) {
            cache = WaveAssetCache{};
            cache.seen = true;
            ResolveWaterWavesValues(*water, material, wind);
        } else {
            if (cache.referencePath != referencePath || cache.preset != water->waveAuthoring.preset) {
                cache = WaveAssetCache{};
                cache.referencePath = referencePath;
                cache.preset = water->waveAuthoring.preset;
                cache.seen = true;
            }

            const asset::DataAsset* resolved = preset ? preset : asset::DataAssetRegistry::Resolve(referencePath);
            const asset::WaterWavesAsset* source = nullptr;
            fluid::GerstnerWaveGenerationSettings settings{};
            float grouping = 0.0f;
            float spread = 0.0f;
            bool hasSettings = false;
            std::string reason;
            if (!resolved) {
                reason = "asset could not be resolved";
            } else {
                const char* typeName = resolved->GetTypeName();
                if (!typeName || std::strcmp(typeName, asset::WaterWavesAsset::TYPE_NAME) != 0) {
                    reason = "asset type is not WaterWavesAsset";
                } else {
                    source = static_cast<const asset::WaterWavesAsset*>(resolved);
                    settings = source->GenerationSettings();
                    grouping = source->waveGrouping;
                    spread = source->waveSpread;
                    hasSettings = true;
                    const std::uint64_t signature = WaveAuthoringSignature(
                        WaveSettingsSignature(referencePath, settings, grouping, spread), water->waveAuthoring);
                    const bool cacheMatches = cache.hasAttempt && cache.attemptedAsset == resolved
                        && cache.attemptedSignature == signature;
                    if (!cacheMatches) {
                        std::array<fluid::GerstnerWave, fluid::MAX_BASE_GERSTNER_WAVES> generated{};
                        std::uint32_t count = 0;
                        math::Vector2 display;
                        const bool generatedValid = GenerateWaveBands(*source, water->waveAuthoring, generated, count, display);
                        cache.attemptedAsset = resolved;
                        cache.attemptedSignature = signature;
                        cache.hasAttempt = true;
                        cache.lastAttemptValid = generatedValid;
                        if (generatedValid) {
                            cache.generatedWaves = generated;
                            cache.generatedWaveCount = count;
                            cache.generatedGrouping = grouping;
                            cache.generatedSpread = spread;
                            cache.generatedDirectionDisplay = display;
                            cache.hasLastGood = true;
                        }
                    }
                    if (!cache.lastAttemptValid)
                        reason = "asset settings are invalid or use an unsupported algorithm version";
                }
            }

            if (reason.empty()) {
                if (!ResolveWaterWaveBases(*water, material, wind,
                        std::span<const GerstnerWave>(cache.generatedWaves.data(), cache.generatedWaveCount),
                        cache.generatedGrouping, cache.generatedSpread, true)) {
                    reason = "resolved values could not be applied safely";
                } else {
                    cache.diagnosticActive = false;
                }
            }

            if (!reason.empty()) {
                const std::uint64_t diagnosticSignature = WaveDiagnosticSignature(
                    referencePath, reason, settings, grouping, spread, hasSettings);
                const bool newDiagnostic = !cache.diagnosticActive
                    || cache.diagnosticPath != referencePath
                    || cache.diagnosticReason != reason
                    || cache.diagnosticAsset != resolved
                    || cache.diagnosticSignature != diagnosticSignature;
                if (newDiagnostic) {
                    FBZZ_LOG_ERROR("WaterSystem: waves asset '%s' failed: %s; using same-reference last-good waves or calm water.",
                        referencePath.c_str(), reason.c_str());
                }
                cache.diagnosticActive = true;
                cache.diagnosticPath = referencePath;
                cache.diagnosticReason = reason;
                cache.diagnosticAsset = resolved;
                cache.diagnosticSignature = diagnosticSignature;

                std::array<fluid::GerstnerWave, fluid::MAX_BASE_GERSTNER_WAVES> fallback{};
                std::uint32_t fallbackCount = 0;
                float fallbackGrouping = 0.0f;
                float fallbackSpread = 0.0f;
                if (cache.hasLastGood && cache.referencePath == referencePath) {
                    fallback = cache.generatedWaves;
                    fallbackCount = cache.generatedWaveCount;
                    fallbackGrouping = cache.generatedGrouping;
                    fallbackSpread = cache.generatedSpread;
                }
                static_cast<void>(ResolveWaterWaveBases(*water, material, wind,
                    std::span<const GerstnerWave>(fallback.data(), fallbackCount),
                    fallbackGrouping, fallbackSpread, true));
            }
            water->waveDirectionDisplay = cache.generatedDirectionDisplay;
        }
    }

    PrepareWaterZoneSnapshots(scene);
    PrepareWaterExclusionSnapshots(scene);
    for (EntityID id : scene.GetEntities<WaterComponent>()) {
        WaterComponent* water = scene.GetComponent<WaterComponent>(id);
        GameObject* go = scene.GetGameObject(id);
        if (!water || !go || !go->activeInHierarchy() || water->zoneOwner.IsValid()) continue;
        water->cellSize = ResolveWaterCellSize(*water, go->transform);
        water->PrepareWaveCache();
        /// @note 流れと輪は cellSize を決めた後で解決する。どちらも «頂点で刻めるか» を WaveMeshFade で測るので、順序が逆だと 1 フレーム古いセル実寸で判断する。
        ResolveWaterSurfaceFlows(*water, go->transform, *flowFrame.fields);
        UpdateWaterRipples(*water, go->transform, ctx.dt);
        if (water->enabled && (water->splashEnabled || water->interactionSettings.enabled)
            && IsWaterSurfaceSupported(*water, go->transform)) targets.push_back({water, &go->transform, id});
    }

    PrepareWaterDepthFields(scene, WaterSurfaceReadView::LIVE_QUERY);
    if (ctx.simulating) {
        CaptureWaterSimulationSurfaces(scene);
    } else if (ctx.playing) {
        /// @note Pause publishes new surfaces once while preserving every prior fixed-step snapshot.
        CaptureWaterSimulationSurfaces(scene, true);
    }
    std::erase_if(m_waveAssets, [](const auto& entry) { return !entry.second.seen; });

    /// @note 編集中は «前回どこに居たか» を持ち越さない。持ち越すと、水中に置いた物体が Play 開始の瞬間に一斉に «着水» したことになる。
    if (!ctx.playing) { m_bodies.clear(); return; }
    if (!ctx.simulating) return;
    if (targets.empty()) { m_bodies.clear(); return; }

    const float time = Time::time;
    for (auto& entry : m_bodies) entry.second.seen = false;

    for (EntityID id : scene.GetEntities<RigidBodyComponent>()) {
        auto* rb = scene.GetComponent<RigidBodyComponent>(id);
        GameObject* go = scene.GetGameObject(id);
        if (!rb || !go || !rb->enabled || !rb->rigidBody || rb->rigidBody->IsStatic()
            || !go->activeInHierarchy())
            continue;

        const math::Vector3 pos = go->transform.worldPosition;
        WaterSurfaceHeightSample sample;
        const SplashTarget* target = FindSplashTarget(targets, pos, time,
            WaterSurfaceReadView::SIMULATION, sample);
        if (!target) continue;

        const float baseY = target->transform->worldPosition.y;
        const float bound = target->water->SurfaceHeightBound(WaterSurfaceReadView::SIMULATION);
        auto [it, inserted] = m_bodies.try_emplace(BodyKey(id));
        BodyState& state = it->second;
        state.seen = true;
        const bool outsideBand = std::abs(pos.y - baseY) > bound + kWakeBand;
        const bool rectangle = !UsesWaterSurfaceGeometry(*target->water,
            WaterSurfaceReadView::SIMULATION)
            && !WaterSurfaceExclusions(*target->water, WaterSurfaceReadView::SIMULATION);
        const bool definitelySubmerged = pos.y < baseY
            && pos.y >= baseY - target->water->buoyancyDepth;
        /// @note 泳ぎの演出は深い水中でも必要なので、波のない遠方の着水判定省略より先に数値だけ記録する。
        if (target->water->interactionSettings.enabled) {
            const auto velocity = rb->rigidBody->GetVelocity();
            SupplyWaterMotionImpulse(scene, target->owner, id, pos, velocity, ctx.dt,
                std::abs(sample.depth) <= target->water->interactionSettings.surfaceBand,
                !target->water->splashEnabled);
        }
        /// @note 帯を飛び越えた着水・離水は正確な高さを求める。状態が変わらない遠方だけ省略する。
        if (rectangle && outsideBand && (inserted || state.submerged == definitelySubmerged)) {
            state.submerged = definitelySubmerged;
            continue;
        }
        const float surfaceY = sample.position.y;
        const bool submerged = sample.underwater;
        const math::Vector3 vel = rb->rigidBody->GetVelocity();
        const math::Vector3 surfacePoint = { pos.x, surfaceY, pos.z };

        if (inserted) {
            /// @note 初めて見た剛体は «元から水中に置いてあった» かもしれない。鳴らさずに状態だけ覚える。
            state.submerged = submerged;
            continue;
        }

        if (target->water->splashEnabled && submerged && !state.submerged && -vel.y > kMinEntrySpeed) {
            const float impact = math::Clamp01(-vel.y / kFullSplashSpeed);
            EmitWaterRipple(*target->water, *target->transform, surfacePoint,
                            0.3f + 0.7f * impact);
            QueueWaterSplash(scene, target->owner, surfacePoint, impact);
        } else if (target->water->splashEnabled && !submerged && state.submerged && vel.y > kMinExitSpeed) {
            EmitWaterRipple(*target->water, *target->transform, surfacePoint,
                            0.15f + 0.35f * math::Clamp01(vel.y / kFullSplashSpeed));
        }

        const float horizontalSpeed = std::sqrt(vel.x * vel.x + vel.z * vel.z);
        if (target->water->splashEnabled && std::abs(pos.y - surfaceY) < kWakeBand && horizontalSpeed > kWakeSpeed) {
            state.wakeTimer -= ctx.dt;
            if (state.wakeTimer <= 0.0f) {
                EmitWaterRipple(*target->water, *target->transform, surfacePoint,
                                math::Clamp(horizontalSpeed / 12.0f, 0.1f, 0.4f));
                state.wakeTimer = kWakeInterval;
            }
        }
        state.submerged = submerged;
    }

    std::erase_if(m_bodies, [](const auto& entry) { return !entry.second.seen; });
}

}
