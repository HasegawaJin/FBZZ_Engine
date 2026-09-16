/// @file    WaterSystem.cpp
/// @brief   水面の実効波の解決と、剛体の着水 (波紋・しぶき・航跡) の検出。
/// @author  Hasegawa Jin
/// @date    2026-09-11
#include <Engine/Scene/Systems/WaterSystem.hpp>
#include "RenderPasses/Geometry/ParticleForces.hpp"
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Core/Scheduler/SystemContext.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Transform.hpp>
#include <Engine/Scene/Components/ForceField.hpp>
#include <Engine/Scene/Components/RigidBodyComponent.hpp>
#include <Engine/Scene/Systems/RenderPasses/Geometry/WaterRenderPass.hpp>
#include <Math/MathUtils.hpp>
#include <Physics/RigidBody.hpp>
#include <algorithm>
#include <cmath>
#include <vector>

namespace fbzz::scene {

namespace {

constexpr GerstnerWave kDefaultWaves[4] = {
    { {  1.00f,  0.20f }, 0.35f, 14.0f, 0.40f },
    { { -0.30f,  0.95f }, 0.20f, 22.0f, 0.30f },
    { {  0.70f, -0.70f }, 0.15f,  9.0f, 0.25f },
    { { -0.90f,  0.40f }, 0.10f, 18.0f, 0.20f },
};

// 着水の判定。速度は鉛直成分 [m/s]。
constexpr float kMinEntrySpeed   = 1.0f;  // これより遅い沈み込みは «浸かった» だけで波紋を出さない
constexpr float kFullSplashSpeed = 8.0f;  // この速さで最大のしぶき
constexpr float kMinExitSpeed    = 2.0f;
// 航跡: 水面をまたいで横へ進む物体が一定間隔で小さな波紋を落とす。
constexpr float kWakeBand     = 0.6f;  // [m] 水面からこの範囲に居る物体だけ
constexpr float kWakeSpeed    = 1.5f;  // [m/s]
constexpr float kWakeInterval = 0.25f; // [s]

float GetF(const asset::MaterialAsset* m, const char* key, float def)
{
    if (!m) return def;
    const auto it = m->params.find(key);
    if (it != m->params.end() && !it->second.empty()) return it->second[0];
    return def;
}

math::Vector2 GetF2(const asset::MaterialAsset* m, const char* key, math::Vector2 def)
{
    if (!m) return def;
    const auto it = m->params.find(key);
    if (it != m->params.end() && it->second.size() >= 2)
        return { it->second[0], it->second[1] };
    return def;
}

uint64_t BodyKey(EntityID id)
{
    return (static_cast<uint64_t>(id.generation) << 32) | id.index;
}

struct SplashTarget {
    EntityID              id;
    const WaterComponent* water;
    const Transform*      transform;
    float                 halfX;
    float                 halfZ;
};

const SplashTarget* FindSplashTarget(const std::vector<SplashTarget>& targets, const math::Vector3& p)
{
    for (const SplashTarget& target : targets) {
        if (std::abs(p.x - target.transform->worldPosition.x) <= target.halfX &&
            std::abs(p.z - target.transform->worldPosition.z) <= target.halfZ)
            return &target;
    }
    return nullptr;
}

} // namespace

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

void ResolveWaterWaves(WaterComponent& water, const asset::MaterialAsset* material, const WaterWind& wind)
{
    using namespace water_keys;
    const float scale = water.enableGerstnerWaves ? (std::max)(water.waveAmplitudeScale, 0.0f) : 0.0f;

    // 強風 (strength 10) で、風と同じ向きの波が windResponse 倍ぶん上乗せされる。
    const float response = math::Clamp01(GetF(material, kWindResponse, 0.0f));
    const float windGain = wind.active
        ? response * math::Clamp(wind.strength / 10.0f, 0.0f, 2.0f)
        : 0.0f;
    // 真上・真下を向いた風は水面を押さない。
    const float windLen = std::sqrt(wind.direction.x * wind.direction.x + wind.direction.z * wind.direction.z);
    const bool hasWindDir = windLen > 1.0e-3f;
    const math::Vector2 windDir = hasWindDir
        ? math::Vector2{ wind.direction.x / windLen, wind.direction.z / windLen }
        : math::Vector2::ZERO;

    for (int i = 0; i < 4; ++i) {
        const GerstnerWave& def = kDefaultWaves[i];
        GerstnerWave wave;
        wave.direction  = GetF2(material, kWaveDirection[i], def.direction);
        wave.amplitude  = (std::max)(GetF(material, kWaveAmplitude[i], def.amplitude), 0.0f);
        wave.wavelength = (std::max)(GetF(material, kWaveWavelength[i], def.wavelength), 0.01f);
        wave.steepness  = math::Clamp01(GetF(material, kWaveSteepness[i], def.steepness));

        if (windGain > 0.0f && hasWindDir) {
            const float dirLen = wave.direction.Length();
            const float align = dirLen > 1.0e-4f
                ? math::Vector2::Dot(wave.direction * (1.0f / dirLen), windDir)
                : 0.0f;
            // 向かい風で潰れる量は、追い風で育つ量の半分に留める。
            // 全部の波が消えると、風上側を向いた水面だけが鏡のように止まって見える。
            const float gain = align >= 0.0f
                ? 1.0f + windGain * align
                : 1.0f + windGain * align * 0.5f;
            wave.amplitude *= (std::max)(gain, 0.2f);
            wave.steepness  = math::Clamp01(wave.steepness * (1.0f + windGain * 0.25f));
        }
        wave.amplitude *= scale;
        water.waves[static_cast<size_t>(i)] = wave;
    }

    // 群と方向広がりは «波があるとき» だけの話。enableGerstnerWaves を切った水面では 0 に倒す。
    water.waveGrouping = scale > 0.0f
        ? math::Clamp01(GetF(material, kWaveGrouping, 0.45f))
        : 0.0f;
    water.waveSpread = scale > 0.0f
        ? math::Clamp01(GetF(material, kWaveSpread, 0.35f))
        : 0.0f;

    const math::Vector2 flow = GetF2(material, kFlowDirection, { 1.0f, 0.0f });
    const float speed = (std::max)(GetF(material, kCurrentSpeed, 0.0f), 0.0f);
    const float flowLen = flow.Length();
    water.current = flowLen > 1.0e-4f ? flow * (speed / flowLen) : math::Vector2::ZERO;
}

math::Vector2 ResolveWaterCellSize(const WaterComponent& water, const Transform& transform)
{
    return {
        water.extentX * std::abs(transform.worldScale.x) / static_cast<float>((std::max)(water.resolutionX, 1u)),
        water.extentZ * std::abs(transform.worldScale.z) / static_cast<float>((std::max)(water.resolutionZ, 1u)),
    };
}

void EmitWaterRipple(EntityID waterEntity, const WaterComponent& water, const Transform& transform,
                     const math::Vector3& worldPos, float strength)
{
    const float sizeX = (std::max)(water.extentX * std::abs(transform.worldScale.x), 0.1f);
    const float sizeZ = (std::max)(water.extentZ * std::abs(transform.worldScale.z), 0.1f);
    const math::Vector2 uv = {
        (worldPos.x - transform.worldPosition.x) / sizeX + 0.5f,
        (worldPos.z - transform.worldPosition.z) / sizeZ + 0.5f,
    };

    // 波紋テクスチャは水面全体で 1 枚。UV で大きさを決めると、広い水面ほど輪が巨大になる。
    // メートルで決めて UV へ直し、テクセルより細くならないところで止める。
    const float size   = (sizeX + sizeZ) * 0.5f;
    const float texel  = 1.0f / static_cast<float>(kWaterRippleTextureSize);
    const float s      = math::Clamp01(strength);
    const float width  = (std::max)((0.25f + 0.35f * s) / size, texel * 1.5f);
    const float speed  = (std::max)((1.2f + 1.2f * s) / size, texel * 3.0f);

    // テクセルで止めた «実際に出る輪の太さ» [m]。海サイズの水面では 1 テクセルが数十 m あり、
    // 小石 1 個の着水が直径 100 m の輪として出てしまう。太くなったぶんだけ薄くし、
    // «輪として読めない» ところで置くのをやめる。しぶきのパーティクルはそのまま残る。
    const float worldWidth = width * size;
    const float t = math::Clamp01((worldWidth - 3.0f) / 5.0f);
    const float scaleFade = 1.0f - t * t * (3.0f - 2.0f * t);
    if (scaleFade <= 0.01f) return;

    AddWaterRipple(waterEntity, uv, s * scaleFade, speed, 1.3f, width);
}

ComponentAccess WaterSystem::GetAccess() const
{
    return ComponentAccess{}
        .Reads<RigidBodyComponent, ForceField>()
        .Writes<WaterComponent>();
}

void WaterSystem::Update(SystemContext& ctx)
{
    Scene& scene = ctx.scene;
    const AmbientWind ambient = FindAmbientWind(scene);
    const WaterWind wind{ ambient.active, ambient.direction, ambient.strength };

    std::vector<SplashTarget> targets;
    for (EntityID id : scene.GetEntities<WaterComponent>()) {
        auto* water = scene.GetComponent<WaterComponent>(id);
        GameObject* go = scene.GetGameObject(id);
        if (!water || !go) continue;
        ResolveWaterWaves(*water, LoadWaterMaterial(*water), wind);
        water->cellSize = ResolveWaterCellSize(*water, go->transform);
        if (water->enabled && water->splashEnabled && go->activeInHierarchy()) {
            targets.push_back({ id, water, &go->transform,
                                water->extentX * 0.5f * std::abs(go->transform.worldScale.x),
                                water->extentZ * 0.5f * std::abs(go->transform.worldScale.z) });
        }
    }

    // 編集中は «前回どこに居たか» を持ち越さない。持ち越すと、水中に置いた物体が
    // Play 開始の瞬間に一斉に «着水» したことになる。
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
        const SplashTarget* target = FindSplashTarget(targets, pos);
        if (!target) continue;

        const float surfaceY = target->transform->worldPosition.y
                             + target->water->GetSurfaceHeightAt(pos.x, pos.z, time);
        const bool submerged = pos.y < surfaceY;
        const math::Vector3 vel = rb->rigidBody->GetVelocity();
        const math::Vector3 surfacePoint = { pos.x, surfaceY, pos.z };

        auto [it, inserted] = m_bodies.try_emplace(BodyKey(id));
        BodyState& state = it->second;
        state.seen = true;
        if (inserted) {
            // 初めて見た剛体は «元から水中に置いてあった» かもしれない。鳴らさずに状態だけ覚える。
            state.submerged = submerged;
            continue;
        }

        if (submerged && !state.submerged && -vel.y > kMinEntrySpeed) {
            const float impact = math::Clamp01(-vel.y / kFullSplashSpeed);
            EmitWaterRipple(target->id, *target->water, *target->transform, surfacePoint,
                            0.3f + 0.7f * impact);
            QueueWaterSplash(surfacePoint, impact);
        } else if (!submerged && state.submerged && vel.y > kMinExitSpeed) {
            EmitWaterRipple(target->id, *target->water, *target->transform, surfacePoint,
                            0.15f + 0.35f * math::Clamp01(vel.y / kFullSplashSpeed));
        }

        const float horizontalSpeed = std::sqrt(vel.x * vel.x + vel.z * vel.z);
        if (std::abs(pos.y - surfaceY) < kWakeBand && horizontalSpeed > kWakeSpeed) {
            state.wakeTimer -= ctx.dt;
            if (state.wakeTimer <= 0.0f) {
                EmitWaterRipple(target->id, *target->water, *target->transform, surfacePoint,
                                math::Clamp(horizontalSpeed / 12.0f, 0.1f, 0.4f));
                state.wakeTimer = kWakeInterval;
            }
        }
        state.submerged = submerged;
    }

    std::erase_if(m_bodies, [](const auto& entry) { return !entry.second.seen; });
}

} // namespace fbzz::scene
