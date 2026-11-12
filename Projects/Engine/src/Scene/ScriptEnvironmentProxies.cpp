/// @file    ScriptEnvironmentProxies.cpp
/// @brief   Environment 領域の Script 転送と関連する内部ヘルパー。
/// @author  Hasegawa Jin
/// @date    2026-10-31
#include "ScriptProxyCommon.hpp"
#include <Engine/Scene/WaterInteraction.hpp>
#include <Engine/Scene/Script.hpp>
#include <Engine/Scene/TerrainSurfaceQuery.hpp>
#include <Engine/Scene/WaterSurfaceQuery.hpp>
#include <Engine/Input/Input.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Fields/FlowField.hpp>
#include <Engine/Scene/Components/VolumetricCloudComponent.hpp>
#include <Engine/Scene/Components/SunMoonRenderer.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Engine/Scene/Components/WaterComponent.hpp>
#include <Engine/Scene/Components/EnvironmentLightComponent.hpp>
#include <Engine/Scene/Components/SkyRenderer.hpp>
#include <limits>
#include <type_traits>
#include <string_view>
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cmath>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <Engine/Scene/Systems/WaterSystem.hpp>
#include <Engine/Scene/WaterInteraction.hpp>
namespace fbzz::scene {
using namespace script_proxy_detail;
namespace {

WaterComponent* SelfWater(const Script* script)
{
    return SelfComponent<WaterComponent>(script);
}

/// @brief Resolve the active shared surface while retaining the requesting component as the fallback.
/// @param script Script whose GameObject owns the WaterComponent.
/// @param sceneContext 非所有のScript実行Scene。nullptrなら自水面を使う。
/// @param water Receives the standalone surface or Zone owner component.
/// @param object Receives the corresponding surface GameObject.
/// @return False when the requested component or resolved surface no longer exists.

bool ResolveScriptWaterSurface(Script* script, Scene* sceneContext, WaterComponent*& water, GameObject*& object)
{
    water = SelfWater(script);
    object = script ? script->scene.Self() : nullptr;
    if (!water || !object) return false;
    if (!sceneContext) return true;

    Scene& scene = *sceneContext;
    const EntityID requestedId = object->GetID();
    PrepareWaterSceneSurfaces(scene);
    const WaterComponent* requestedWater = scene.GetComponent<WaterComponent>(requestedId);
    water = ResolveWaterZoneOwner(scene, requestedId);
    if (!water) return false;
    const EntityID surfaceId = requestedWater && requestedWater->zoneOwner.IsValid()
        ? requestedWater->zoneOwner : requestedId;
    object = scene.GetGameObject(surfaceId);
    return object != nullptr;
}


TerrainComponent* SelfTerrain(const Script* script)
{
    return SelfComponent<TerrainComponent>(script);
}


math::Vector3 WorldToTerrainLocal(const GameObject* gameObject, const math::Vector3& worldPos)
{
    if (!gameObject) return worldPos;
    const auto& t = gameObject->transform;
    const math::Vector3 delta = worldPos - t.worldPosition;
    const math::Vector3 local = t.worldRotation.Inverse() * delta;
    return {
        t.worldScale.x != 0.0f ? local.x / t.worldScale.x : local.x,
        t.worldScale.y != 0.0f ? local.y / t.worldScale.y : local.y,
        t.worldScale.z != 0.0f ? local.z / t.worldScale.z : local.z
    };
}


/// @brief シーン全体から最初のコンポーネントを検索する汎用ヘルパー。
/// @note EnvironmentLight/SkyRenderer は通常シーンに 1 つで、どの Script
/// @note からでもアクセスできるよう全探索する。enabled は見ない (SetEnabled(false) を呼ぶために
/// @note コンポーネント自体を見つける必要があるため)。
/// @note Script* でなく Scene* を受け取る: Script::m_scene は protected で free function から
/// @note 直接アクセスできないため、呼び出し側 (friend の proxy メソッド) で取り出して渡す。
template<typename T>
T* FindFirstInScene(Scene* scene)
{
    if (!scene) return nullptr;
    for (EntityID eid : scene->GetEntities<T>()) {
        if (auto* c = scene->GetComponent<T>(eid)) return c;
    }
    return nullptr;
}



FlowFieldSettings* SelfPrimaryForce(const Script* script)
{
    auto* field = SelfComponent<FlowField>(script);
    if (field == nullptr || field->forces.empty()) return nullptr;
    return &field->forces.front();
}


} /// @note namespace
float ScriptWaterProxy::GetSurfaceHeightWorld(float worldX, float worldZ, float time) const
{
    if (!script || !script->m_scene) return 0.0f;
    WaterComponent* surfaceWater = nullptr;
    GameObject* surfaceObject = nullptr;
    if (!ResolveScriptWaterSurface(script, script->m_scene, surfaceWater, surfaceObject)) return 0.0f;
    const auto& transform = surfaceObject->transform;
    WaterSurfaceHeightSample sample;
    if (!TrySampleWaterSurfaceHeightWorld(*surfaceWater, transform,
            {worldX, transform.worldPosition.y, worldZ}, time, sample)) return 0.0f;
    return sample.position.y;
}


float ScriptWaterProxy::GetSurfaceHeightLocal(float localX, float localZ, float time) const
{
    if (!script || !script->m_scene) return 0.0f;
    /// @note ローカル XZ は呼び出し元 GameObject でワールド化し、高さの基準も呼び出し元のワールド Y とする。波の位相はワールド XZ で評価する。
    WaterComponent* surfaceWater = nullptr;
    GameObject* surfaceObject = nullptr;
    if (!ResolveScriptWaterSurface(script, script->m_scene, surfaceWater, surfaceObject)) return 0.0f;
    const auto& requestedTransform = script->m_gameObject->transform;
    const auto& surfaceTransform = surfaceObject->transform;
    const math::Vector3 world = requestedTransform.worldPosition
        + requestedTransform.worldRotation * math::Vector3{
            localX * requestedTransform.worldScale.x, 0.0f,
            localZ * requestedTransform.worldScale.z };
    WaterSurfaceHeightSample sample;
    if (!TrySampleWaterSurfaceHeightWorld(*surfaceWater, surfaceTransform,
            {world.x, surfaceTransform.worldPosition.y, world.z}, time, sample)) return 0.0f;
    return static_cast<float>(static_cast<double>(surfaceTransform.worldPosition.y)
        - static_cast<double>(requestedTransform.worldPosition.y) + sample.displacement);
}


void ScriptWaterProxy::SetWaveAmplitudeScale(float scale) const
{
    if (auto* wc = SelfWater(script)) wc->waveAmplitudeScale = scale < 0.0f ? 0.0f : scale;
}


float ScriptWaterProxy::GetWaveAmplitudeScale() const
{
    const auto* wc = SelfWater(script);
    return wc ? wc->waveAmplitudeScale : 0.0f;
}


void ScriptWaterProxy::SetGerstnerEnabled(bool enabled) const
{
    if (auto* wc = SelfWater(script)) wc->enableGerstnerWaves = enabled;
}


math::Vector2 ScriptWaterProxy::GetCurrent() const
{
    const auto* wc = SelfWater(script);
    return wc ? wc->current : math::Vector2::ZERO;
}


void ScriptWaterProxy::AddRipple(const math::Vector3& worldPos, float strength) const
{
    WaterComponent* surfaceWater = nullptr;
    GameObject* surfaceObject = nullptr;
    if (!ResolveScriptWaterSurface(script, script ? script->m_scene : nullptr, surfaceWater, surfaceObject)) return;
    EmitWaterRipple(*surfaceWater, surfaceObject->transform, worldPos, strength);
}


void ScriptWaterProxy::Splash(const math::Vector3& worldPos, float intensity) const
{
    const float clamped = intensity < 0.0f ? 0.0f : (intensity > 1.0f ? 1.0f : intensity);
    WaterComponent* surfaceWater = nullptr;
    GameObject* surfaceObject = nullptr;
    if (!ResolveScriptWaterSurface(script, script ? script->m_scene : nullptr, surfaceWater, surfaceObject)) return;
    EmitWaterRipple(*surfaceWater, surfaceObject->transform, worldPos, 0.3f + 0.7f * clamped);
    QueueWaterSplash(*script->m_scene, surfaceObject->GetID(), worldPos, clamped);
}


void ScriptWaterProxy::SetBuoyancyEnabled(bool enabled) const
{
    if (auto* wc = SelfWater(script)) wc->buoyancyEnabled = enabled;
}


void ScriptWaterProxy::SetEnabled(bool enabled) const
{
    if (auto* wc = SelfWater(script)) wc->enabled = enabled;
}


bool ScriptWaterProxy::IsEnabled() const
{
    auto* wc = SelfWater(script);
    return wc && wc->enabled;
}


float ScriptTerrainProxy::GetHeightLocal(float localX, float localZ) const
{
    auto* terrain = SelfTerrain(script);
    return terrain ? terrain->GetHeightAt(localX, localZ) : 0.0f;
}


math::Vector3 ScriptTerrainProxy::GetNormalLocal(float localX, float localZ) const
{
    auto* terrain = SelfTerrain(script);
    return terrain ? terrain->GetNormalAt(localX, localZ) : math::Vector3::UP;
}


float ScriptTerrainProxy::GetHeightWorld(const math::Vector3& worldPos) const
{
    math::Vector3 position, normal;
    return TryGetSurfaceWorld(worldPos.x, worldPos.z, position, normal) ? position.y : worldPos.y;
}


bool ScriptTerrainProxy::TryGetSurfaceWorld(float worldX, float worldZ, math::Vector3& position, math::Vector3& normal) const
{
    const auto* terrain = SelfTerrain(script);
    if (!terrain || !script || !script->m_gameObject) return false;
    TerrainSurfaceSample sample;
    if (!TrySampleTerrainWorld(*terrain, script->m_gameObject->transform, worldX, worldZ, sample)) return false;
    position = sample.position;
    normal = sample.normal;
    return true;
}



bool ScriptTerrainProxy::TryGetFloorWorld(const math::Vector3& reference, float maxDistance,
    math::Vector3& position, math::Vector3& normal) const
{
    const auto* terrain = SelfTerrain(script);
    if (!terrain || !script || !script->m_gameObject) return false;
    TerrainSurfaceSample sample;
    if (!TrySampleTerrainFloorWorld(*terrain, script->m_gameObject->transform, reference, maxDistance, sample)) return false;
    position = sample.position; normal = sample.normal;
    return true;
}

bool ScriptTerrainProxy::TryGetCeilingWorld(const math::Vector3& reference, float maxDistance,
    math::Vector3& position, math::Vector3& normal) const
{
    const auto* terrain = SelfTerrain(script);
    if (!terrain || !script || !script->m_gameObject) return false;
    TerrainSurfaceSample sample;
    if (!TrySampleTerrainCeilingWorld(*terrain, script->m_gameObject->transform, reference, maxDistance, sample)) return false;
    position = sample.position; normal = sample.normal;
    return true;
}


math::Vector3 ScriptTerrainProxy::GetNormalWorld(const math::Vector3& worldPos) const
{
    auto* terrain = SelfTerrain(script);
    if (!terrain || !script || !script->m_gameObject) return math::Vector3::UP;
    const auto finite = [](const math::Vector3& value) {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
    };
    const auto& transform = script->m_gameObject->transform;
    const auto scale = transform.worldScale;
    const double rotationLength = transform.worldRotation.Length();
    if (!finite(worldPos) || !finite(scale) || scale.x == 0.0f || scale.y == 0.0f || scale.z == 0.0f
        || !std::isfinite(rotationLength) || std::abs(rotationLength - 1) > 0.001)
        return math::Vector3::UP;
    if (terrain->columns < 2 || terrain->rows < 2 || !std::isfinite(terrain->cellSize) || terrain->cellSize <= 0
        || !std::isfinite(terrain->maxHeight) || (!terrain->heightData.empty() && terrain->heightData.size() != terrain->VertexCount()))
        return math::Vector3::UP;
    const math::Vector3 local = WorldToTerrainLocal(script->m_gameObject, worldPos);
    if (!finite(local)) return math::Vector3::UP;
    const auto normal = terrain->GetNormalAt((std::clamp)(local.x, 0.0f, (terrain->columns - 1) * terrain->cellSize),
        (std::clamp)(local.z, 0.0f, (terrain->rows - 1) * terrain->cellSize));
    math::Vector3 worldNormal;
    return TryTerrainNormalToWorld(transform, normal, worldNormal) ? worldNormal : math::Vector3::UP;
}


bool ScriptTerrainProxy::SetHeightAtGrid(int x, int z, float worldHeight) const
{
    auto* terrain = SelfTerrain(script);
    return terrain && terrain->SetHeightAtGrid(x, z, worldHeight);
}


bool ScriptTerrainProxy::PaintLayerAtGrid(int x, int z, int layer, float weight) const
{
    auto* terrain = SelfTerrain(script);
    return terrain && terrain->PaintLayerAtGrid(x, z, layer, weight);
}


bool ScriptTerrainProxy::SetLayerMaterial(int layer, std::string_view materialPath) const
{
    auto* terrain = SelfTerrain(script);
    return terrain && terrain->SetLayerMaterial(layer, std::string(materialPath));
}


int ScriptTerrainProxy::GetLayerCount() const
{
    auto* terrain = SelfTerrain(script);
    return terrain ? terrain->LayerCount() : 0;
}


float ScriptTerrainProxy::GetLayerWeightAtGrid(int x, int z, int layer) const
{
    auto* terrain = SelfTerrain(script);
    return terrain ? terrain->GetLayerWeightAtGrid(x, z, layer) : 0.0f;
}


bool ScriptTerrainProxy::SetHoleAtGrid(int cellX, int cellZ, bool hole) const
{
    auto* terrain = SelfTerrain(script);
    if (!terrain) return false;
    const bool wasHole = terrain->IsHoleCell(cellX, cellZ);
    if (!terrain->SetHoleCell(cellX, cellZ, hole)) return false;
    /// @note 変化の無い書き込みでメッシュとコライダーを作り直さない (毎フレーム呼ぶスクリプトを想定)。
    if (wasHole != hole) terrain->RequestHoleRebuild();
    return true;
}


void ScriptTerrainProxy::RequestRebuild() const
{
    if (auto* terrain = SelfTerrain(script)) {
        terrain->RequestHeightRebuild();
        terrain->RequestSplatRebuild();
        terrain->RequestMaterialRebuild();
    }
}

/// @brief ScriptEnvironmentProxy

void ScriptEnvironmentProxy::SetIBLEnabled(bool enabled) const
{
    if (auto* c = FindFirstInScene<EnvironmentLightComponent>(script->m_scene)) c->enabled = enabled;
}

void ScriptEnvironmentProxy::SetIBLIntensity(float intensity) const
{
    if (auto* c = FindFirstInScene<EnvironmentLightComponent>(script->m_scene)) c->intensity = intensity;
}

void ScriptEnvironmentProxy::SetIBLDiffuseScale(float scale) const
{
    if (auto* c = FindFirstInScene<EnvironmentLightComponent>(script->m_scene)) c->diffuseScale = scale;
}

void ScriptEnvironmentProxy::SetIBLSpecularScale(float scale) const
{
    if (auto* c = FindFirstInScene<EnvironmentLightComponent>(script->m_scene)) c->specularScale = scale;
}


void ScriptEnvironmentProxy::SetSunIntensity(float intensity) const
{
    /// @note 対象は大気散乱の明るさ。太陽ディスクは ScriptSunMoonProxy::SetSun が持つ。
    if (auto* c = FindFirstInScene<SkyRenderer>(script->m_scene)) c->skyScatterIntensity = intensity;
}

void ScriptEnvironmentProxy::SetMieScattering(float mie) const
{
    if (auto* c = FindFirstInScene<SkyRenderer>(script->m_scene)) c->mieScattering = mie;
}

void ScriptEnvironmentProxy::SetMieG(float g) const
{
    if (auto* c = FindFirstInScene<SkyRenderer>(script->m_scene)) c->mieG = g;
}

float ScriptEnvironmentProxy::GetSunIntensity() const
{
    auto* c = FindFirstInScene<SkyRenderer>(script->m_scene);
    return c ? c->skyScatterIntensity : 0.0f;
}

float ScriptEnvironmentProxy::GetMieScattering() const
{
    auto* c = FindFirstInScene<SkyRenderer>(script->m_scene);
    return c ? c->mieScattering : 0.0f;
}

float ScriptEnvironmentProxy::GetMieG() const
{
    auto* c = FindFirstInScene<SkyRenderer>(script->m_scene);
    return c ? c->mieG : 0.0f;
}


bool ScriptEnvironmentProxy::IsIBLEnabled() const
{
    auto* c = FindFirstInScene<EnvironmentLightComponent>(script->m_scene);
    return c && c->enabled;
}

float ScriptEnvironmentProxy::GetIBLIntensity() const
{
    auto* c = FindFirstInScene<EnvironmentLightComponent>(script->m_scene);
    return c ? c->intensity : 0.0f;
}

float ScriptEnvironmentProxy::GetIBLDiffuseScale() const
{
    auto* c = FindFirstInScene<EnvironmentLightComponent>(script->m_scene);
    return c ? c->diffuseScale : 0.0f;
}

float ScriptEnvironmentProxy::GetIBLSpecularScale() const
{
    auto* c = FindFirstInScene<EnvironmentLightComponent>(script->m_scene);
    return c ? c->specularScale : 0.0f;
}


/// @brief ScriptDecalProxy

void ScriptFlowFieldProxy::SetEnabled(bool enabled) const
{
    if (auto* force = SelfPrimaryForce(script)) force->enabled = enabled;
}

void ScriptFlowFieldProxy::SetType(ScriptFlowFieldType type) const
{
    if (auto* force = SelfPrimaryForce(script)) {
        /// @note LEGACY_DRAG (5) と Baked (6) はスクリプトからは選べない。前者は読み込み専用、
        /// @note 後者は 速度場 PNG のパスが要るのでコンポーネント側で組む。
        const int value = std::clamp(static_cast<int>(type), 0,
                                     static_cast<int>(FlowFieldType::Curl));
        force->fieldType = static_cast<FlowFieldType>(value);
    }
}

void ScriptFlowFieldProxy::SetSpeed(float speed) const
{
    if (auto* force = SelfPrimaryForce(script)) force->strength = speed;
}

void ScriptFlowFieldProxy::SetRadius(float radius, float falloffPower) const
{
    if (auto* force = SelfPrimaryForce(script)) {
        force->radius = radius;
        force->falloffPower = (std::max)(falloffPower, 0.01f);
    }
}

void ScriptFlowFieldProxy::SetDirection(const math::Vector3& direction) const
{
    if (auto* force = SelfPrimaryForce(script)) force->direction = direction;
}

void ScriptFlowFieldProxy::SetTurbulence(float frequency, float speed) const
{
    if (auto* force = SelfPrimaryForce(script)) {
        force->noiseFrequency = (std::max)(frequency, 0.0f);
        force->noiseSpeed = speed;
    }
}

void ScriptFlowFieldProxy::SetChannels(uint32_t channels) const
{
    /// @note チャンネルは «この場が誰に効くか» で、束ねた流れで分ける意味が無いので全部へ配る。
    if (auto* field = SelfComponent<FlowField>(script))
        for (auto& force : field->forces) force.channels = channels;
}

bool ScriptFlowFieldProxy::IsEnabled() const
{
    const auto* force = SelfPrimaryForce(script);
    return force && force->enabled;
}

float ScriptFlowFieldProxy::GetSpeed() const
{
    const auto* force = SelfPrimaryForce(script);
    return force ? force->strength : 0.0f;
}

float ScriptFlowFieldProxy::GetRadius() const
{
    const auto* force = SelfPrimaryForce(script);
    return force ? force->radius : 0.0f;
}

ScriptFlowFieldType ScriptFlowFieldProxy::GetType() const
{
    const auto* force = SelfPrimaryForce(script);
    if (force == nullptr || force->fieldType > FlowFieldType::Curl)
        return ScriptFlowFieldType::UNIFORM;
    return static_cast<ScriptFlowFieldType>(static_cast<int>(force->fieldType));
}

uint32_t ScriptFlowFieldProxy::GetChannels() const
{
    const auto* force = SelfPrimaryForce(script);
    return force ? force->channels : 0u;
}

/// @brief ScriptCloudProxy

void ScriptCloudProxy::SetEnabled(bool enabled) const
{
    if (auto* cloud = SelfComponent<VolumetricCloudComponent>(script)) cloud->enabled = enabled;
}

void ScriptCloudProxy::SetLayer(float bottomHeight, float thickness) const
{
    if (auto* cloud = SelfComponent<VolumetricCloudComponent>(script)) {
        cloud->bottomHeight = bottomHeight;
        cloud->thickness = (std::max)(thickness, 1.0f);
    }
}

void ScriptCloudProxy::SetCoverage(float coverage, float density) const
{
    if (auto* cloud = SelfComponent<VolumetricCloudComponent>(script)) {
        cloud->coverage = (std::max)(0.0f, (std::min)(coverage, 1.0f));
        cloud->density = (std::max)(density, 0.0f);
    }
}

void ScriptCloudProxy::SetWind(const math::Vector2& direction, float speed) const
{
    if (auto* cloud = SelfComponent<VolumetricCloudComponent>(script)) {
        cloud->windDirection = direction;
        cloud->windSpeed = speed;
    }
}

void ScriptCloudProxy::SetLighting(float absorption, float ambientStrength,
                                   float silverLining, const math::Vector3& albedo) const
{
    if (auto* cloud = SelfComponent<VolumetricCloudComponent>(script)) {
        cloud->lightAbsorption = (std::max)(absorption, 0.0f);
        cloud->ambientStrength = (std::max)(0.0f, (std::min)(ambientStrength, 1.0f));
        cloud->silverLining = (std::max)(silverLining, 0.0f);
        cloud->albedo = albedo;
    }
}

void ScriptCloudProxy::SetQuality(int stepCount, float maxDistance) const
{
    if (auto* cloud = SelfComponent<VolumetricCloudComponent>(script)) {
        cloud->stepCount = (std::max)(8, (std::min)(stepCount, 96));
        cloud->maxDistance = (std::max)(maxDistance, 100.0f);
    }
}

/// @brief ScriptSunMoonProxy

void ScriptSunMoonProxy::SetEnabled(bool enabled) const
{
    if (auto* sunMoon = SelfComponent<SunMoonRenderer>(script)) sunMoon->enabled = enabled;
}

void ScriptSunMoonProxy::SetSun(bool enabled, float intensity) const
{
    if (auto* sunMoon = SelfComponent<SunMoonRenderer>(script)) {
        sunMoon->sunEnabled = enabled;
        sunMoon->sunDiskIntensity = (std::max)(intensity, 0.0f);
    }
}

void ScriptSunMoonProxy::SetMoon(bool enabled, float size, float brightness,
                                 const math::Vector3& color) const
{
    if (auto* sunMoon = SelfComponent<SunMoonRenderer>(script)) {
        sunMoon->moonEnabled = enabled;
        sunMoon->moonSize = (std::max)(size, 0.0f);
        sunMoon->moonBrightness = (std::max)(brightness, 0.0f);
        sunMoon->moonColor = color;
    }
}

/// @brief ScriptPatrolProxy

void ScriptWindProxy::SetEnabled(bool enabled) const
{
    /// @note 風という概念全体の on/off。乱れも一緒に切る。
    if (script && script->m_scene) script->m_scene->Environment().enabled = enabled;
}

void ScriptWindProxy::SetDirection(const math::Vector3& direction) const
{
    if (script && script->m_scene) script->m_scene->Environment().direction = direction;
}

void ScriptWindProxy::SetStrength(float strength) const
{
    /// @note 旧 API 名のまま。中身は流速 [m/s]。
    if (script && script->m_scene) script->m_scene->Environment().speed = strength;
}

void ScriptWindProxy::SetTurbulence(float turbulence) const
{
    if (script && script->m_scene)
        script->m_scene->Environment().turbulence = (std::max)(turbulence, 0.0f);
}

void ScriptWindProxy::SetPulseFrequency(float frequency) const
{
    /// @note 脈動の速さ = 乱流のノイズ時間スクロール速度 (旧 WindZone と同じ対応)。
    if (script && script->m_scene)
        script->m_scene->Environment().pulseFrequency = (std::max)(frequency, 0.0f);
}


} /// @note namespace fbzz::scene
