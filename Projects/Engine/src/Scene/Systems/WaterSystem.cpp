/// @file    WaterSystem.cpp
/// @brief   水面の実効波の解決と、剛体の着水 (波紋・しぶき・航跡) の検出。
/// @author  Hasegawa Jin
/// @date    2026-09-11
#include <Engine/Scene/Systems/WaterSystem.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Core/Scheduler/SystemContext.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Transform.hpp>
#include <Engine/Scene/Fields/FlowField.hpp>
#include <Engine/Scene/Fields/FlowFieldFrame.hpp>
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

/// @brief 着水の判定。速度は鉛直成分 [m/s]。
constexpr float kMinEntrySpeed   = 1.0f;  ///< @brief これより遅い沈み込みは «浸かった» だけで波紋を出さない
constexpr float kFullSplashSpeed = 8.0f;  ///< @brief この速さで最大のしぶき
constexpr float kMinExitSpeed    = 2.0f;
/// @brief 航跡: 水面をまたいで横へ進む物体が一定間隔で小さな波紋を落とす。
constexpr float kWakeBand     = 0.6f;  ///< @brief [m] 水面からこの範囲に居る物体だけ
constexpr float kWakeSpeed    = 1.5f;  ///< @brief [m/s]
constexpr float kWakeInterval = 0.25f; ///< @brief [s]

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
    WaterComponent*  water;
    const Transform* transform;
    float            halfX;
    float            halfZ;
};

/// @brief 水面のワールド実寸 [m] (X, Z)。
math::Vector2 WaterWorldSize(const WaterComponent& water, const Transform& transform)
{
    return {
        (std::max)(water.extentX * std::abs(transform.worldScale.x), 0.1f),
        (std::max)(water.extentZ * std::abs(transform.worldScale.z), 0.1f),
    };
}

/// @brief 枠を争う 2 本のうち a が残るか。
/// @note 形を持つものが常に勝ち、形の無いものどうしは速い方が残る。形は «その場に無いと
///       シルエットが変わる» のに対し、質感は 1 本落ちても画として崩れないため。
bool IsStrongerSurfaceFlow(const WaterSurfaceFlow& a, const WaterSurfaceFlow& b)
{
    const float heightA = std::abs(a.height);
    const float heightB = std::abs(b.height);
    if (heightA != heightB) return heightA > heightB;
    return std::abs(a.speed) > std::abs(b.speed);
}

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

    /// @note 基準風速で、風と同じ向きの波が windResponse 倍ぶん上乗せされる。
    const float response = math::Clamp01(GetF(material, kWindResponse, 0.0f));
    const float windGain = wind.active
        ? response * math::Clamp(wind.speed / kWaveGrowthReferenceWindSpeed, 0.0f, 2.0f)
        : 0.0f;
    /// @note 真上・真下を向いた風は水面を押さない。
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
            /// @note 向かい風で潰れる量は、追い風で育つ量の半分に留める。
            ///       全部の波が消えると、風上側を向いた水面だけが鏡のように止まって見える。
            const float gain = align >= 0.0f
                ? 1.0f + windGain * align
                : 1.0f + windGain * align * 0.5f;
            wave.amplitude *= (std::max)(gain, 0.2f);
            wave.steepness  = math::Clamp01(wave.steepness * (1.0f + windGain * 0.25f));
        }
        wave.amplitude *= scale;
        water.waves[static_cast<size_t>(i)] = wave;
    }

    /// @note 群と方向広がりは «波があるとき» だけの話。enableGerstnerWaves を切った水面では 0 に倒す。
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

void EmitWaterRipple(WaterComponent& water, const Transform& transform,
                     const math::Vector3& worldPos, float strength)
{
    const math::Vector2 size = WaterWorldSize(water, transform);

    /// @note 波紋テクスチャは水面全体で 1 枚。テクセルより細い輪は焼いても «輪» にならないので、
    ///       テクセルのワールド実寸を太さの下限にする。
    const float mean   = (size.x + size.y) * 0.5f;
    const float texel  = mean / static_cast<float>(kWaterRippleTextureSize);
    const float s      = math::Clamp01(strength);
    const float width  = (std::max)(0.25f + 0.35f * s, texel * 1.5f);
    const float speed  = (std::max)(1.2f + 1.2f * s, texel * 3.0f);

    /// @note テクセルで止めた «実際に出る輪の太さ» [m]。海サイズの水面では 1 テクセルが数十 m あり、
    ///       小石 1 個の着水が直径 100 m の輪として出てしまう。太くなったぶんだけ薄くし、
    ///       «輪として読めない» ところで置くのをやめる。しぶきのパーティクルはそのまま残る。
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
    /// @note 断面 sin(ring/w·PI) の周期は 2·width。«波長 / セル» で測る WaveMeshFade へは
    ///       その周期を渡す (帯の原則は Gerstner 波と同じ尺で決める)。
    ripple.meshFade  = WaterComponent::WaveMeshFade(width * 2.0f, water.cellSize);
    /// @note 輪は «水面を横切り切ったら» 用済み。UV 時代の 2.8 (= 幅の 2.8 倍) を実寸へ写した値。
    ripple.maxRadius = mean * 2.8f;

    /// @note 波紋テクスチャは «テクセル数 × 波紋数» を毎フレーム CPU で焼き直す。物体を大量に
    ///       水へ落とすと波紋の数だけフレームが重くなるので、上限を置いて古いものから捨てる。
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
            /// @note 回した箱を軸並行で包む代わりに対角長で見る。枠に入るかの粗いふるいなので、
            ///       安全側に大きく取って構わない。
            reach = field.extents.Length();
            if (std::abs(flow.planeOffsetY) > reach) continue;
            /// @note 特徴の大きさは «狭い辺の半分»。extents は箱の半径で、場の中の起伏は箱より
            ///       細かい。全幅で測ると、刻めない場が縞として残る。
            const float feature = (std::min)(field.extents.x, field.extents.z);
            flow.height = -kMaxWaterSurfaceDisplacement
                        * WaterComponent::WaveMeshFade(feature, water.cellSize);
        } else {
            /// @note 半径の無い («シーン全体») 要素は中心からの距離で減衰しないので、水面のどこまで
            ///       効くのか決まらない。流速としては効いたままで、形と質感にだけ出さない。
            if (field.radius <= 0.0001f) continue;
            /// @note Bernoulli。strength は流速 [m/s] なので v^2/2g がそのまま長さになる。
            const float bernoulli =
                (std::min)(flow.speed * flow.speed / 19.6f, kMaxWaterSurfaceDisplacement)
                /// @note 頂点で刻めない形は消す。波長にあたるのは形の差し渡し = 2·radius。
                * WaterComponent::WaveMeshFade(field.radius * 2.0f, water.cellSize);
            switch (field.type) {
            case FlowFieldType::Uniform: {
                /// @note 真上・真下を向いた流れは水面を撫でない。水平成分だけを «風» として読む。
                const float dirLen = std::sqrt(field.direction.x * field.direction.x
                                             + field.direction.z * field.direction.z);
                if (dirLen > 1.0e-4f)
                    flow.direction = { field.direction.x / dirLen, field.direction.z / dirLen };
                flow.speed *= dirLen;
                flow.chop = math::Clamp01(flow.speed / kWaterSurfaceFlowReference);
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
                break;
            default:
                continue;
            }
        }

        /// @note 形を持つ型が «刻めない» ときは枠を使わない。占めたままだと、頂点に出ない形の
        ///       ために Uniform / Curl の質感が押し出される。
        const bool shapeless = flow.kind == FlowFieldType::Uniform
                            || flow.kind == FlowFieldType::Curl;
        if (!shapeless && std::abs(flow.height) <= 0.0001f) continue;

        const float dx = field.position.x - transform.worldPosition.x;
        const float dz = field.position.z - transform.worldPosition.z;
        if (std::abs(dx) > halfX + reach || std::abs(dz) > halfZ + reach) continue;

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

math::Vector3 WaterFlowVelocityAt(const WaterComponent& water,
                                  const std::vector<ActiveFlowField>& fields,
                                  float surfaceBaseY,
                                  const math::Vector3& worldPos,
                                  float time)
{
    if (fields.empty()) return { water.current.x, 0.0f, water.current.y };
    const math::Vector3 surfacePoint = {
        worldPos.x,
        surfaceBaseY + water.GetSurfaceHeightAt(worldPos.x, worldPos.z, time),
        worldPos.z,
    };
    /// @note 全ビットで引く。剛体にはチャンネルの申告が無いので «どの場も受ける»。
    const math::Vector3 flow = SampleFlow(surfacePoint, fields, 0xFFFFFFFFu, time);
    /// @note 鉛直成分は捨てる。水面の «表面の流速» は水平にしか意味がなく、y を渡すと
    ///       浮いた体が浮力と綱引きして水面の上で震える。
    return { water.current.x + flow.x, 0.0f, water.current.y + flow.z };
}

ComponentAccess WaterSystem::GetAccess() const
{
    return ComponentAccess{}
        .Reads<RigidBodyComponent, FlowField>()
        .Writes<WaterComponent>();
}

void WaterSystem::Update(SystemContext& ctx)
{
    Scene& scene = ctx.scene;
    /// @note 前フレームまでに積まれたしぶきをここで生成する。描画パスの中で作ると
    ///       同じフレームの後続パスが握るエミッターを詰め替えてしまう。
    UpdateWaterSplashes(scene);
    /// @note 環境流は Scene のフレームキャッシュが正本。GameObject の並び順で勝者が決まる
    ///       «最初に見つかった radius 0 の Wind» の探索はもう無い。
    const FlowFieldFrame& flowFrame = scene.FlowFrame();
    const AmbientWind& ambient = flowFrame.ambient;
    const WaterWind wind{ ambient.active, ambient.direction, ambient.speed };

    std::vector<SplashTarget> targets;
    for (EntityID id : scene.GetEntities<WaterComponent>()) {
        auto* water = scene.GetComponent<WaterComponent>(id);
        GameObject* go = scene.GetGameObject(id);
        if (!water || !go) continue;
        ResolveWaterWaves(*water, LoadWaterMaterial(*water), wind);
        water->cellSize = ResolveWaterCellSize(*water, go->transform);
        water->PrepareWaveCache();
        /// @note 流れと輪は cellSize を決めた後で解決する。どちらも «頂点で刻めるか» を
        ///       WaveMeshFade で測るので、順序が逆だと 1 フレーム古いセル実寸で判断する。
        ResolveWaterSurfaceFlows(*water, go->transform, *flowFrame.fields);
        UpdateWaterRipples(*water, go->transform, ctx.dt);
        if (water->enabled && water->splashEnabled && go->activeInHierarchy()) {
            targets.push_back({ water, &go->transform,
                                water->extentX * 0.5f * std::abs(go->transform.worldScale.x),
                                water->extentZ * 0.5f * std::abs(go->transform.worldScale.z) });
        }
    }

    /// @note 編集中は «前回どこに居たか» を持ち越さない。持ち越すと、水中に置いた物体が
    ///       Play 開始の瞬間に一斉に «着水» したことになる。
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

        const float baseY = target->transform->worldPosition.y;
        const float bound = target->water->SurfaceHeightBound();
        auto [it, inserted] = m_bodies.try_emplace(BodyKey(id));
        BodyState& state = it->second;
        state.seen = true;
        const bool outsideBand = std::abs(pos.y - baseY) > bound + kWakeBand;
        const bool definitelySubmerged = pos.y < baseY;
        /// @note 帯を飛び越えた着水・離水は正確な高さを求める。状態が変わらない遠方だけ省略する。
        if (outsideBand && (inserted || state.submerged == definitelySubmerged)) {
            state.submerged = definitelySubmerged;
            continue;
        }
        const float surfaceY = baseY + target->water->GetSurfaceHeightAt(pos.x, pos.z, time);
        const bool submerged = pos.y < surfaceY;
        const math::Vector3 vel = rb->rigidBody->GetVelocity();
        const math::Vector3 surfacePoint = { pos.x, surfaceY, pos.z };

        if (inserted) {
            /// @note 初めて見た剛体は «元から水中に置いてあった» かもしれない。鳴らさずに状態だけ覚える。
            state.submerged = submerged;
            continue;
        }

        if (submerged && !state.submerged && -vel.y > kMinEntrySpeed) {
            const float impact = math::Clamp01(-vel.y / kFullSplashSpeed);
            EmitWaterRipple(*target->water, *target->transform, surfacePoint,
                            0.3f + 0.7f * impact);
            QueueWaterSplash(surfacePoint, impact);
        } else if (!submerged && state.submerged && vel.y > kMinExitSpeed) {
            EmitWaterRipple(*target->water, *target->transform, surfacePoint,
                            0.15f + 0.35f * math::Clamp01(vel.y / kFullSplashSpeed));
        }

        const float horizontalSpeed = std::sqrt(vel.x * vel.x + vel.z * vel.z);
        if (std::abs(pos.y - surfaceY) < kWakeBand && horizontalSpeed > kWakeSpeed) {
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

} // namespace fbzz::scene
