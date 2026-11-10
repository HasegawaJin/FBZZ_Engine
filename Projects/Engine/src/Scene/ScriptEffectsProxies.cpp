/// @file    ScriptEffectsProxies.cpp
/// @brief   Effects 領域の Script 転送と関連する内部ヘルパー。
/// @author  Hasegawa Jin
/// @date    2026-10-31
#include "ScriptProxyCommon.hpp"
#include <Engine/Scene/Script.hpp>
#include <Engine/Input/Input.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Components/MeshTrailComponent.hpp>
#include <Engine/Scene/Components/ParticleEmitter.hpp>
#include <Engine/Scene/Components/VFXComponent.hpp>
#include <Engine/Scene/Fields/FlowField.hpp>
#include <Engine/Scene/Components/TrailComponent.hpp>
#include <Engine/Scene/Components/DecalComponent.hpp>
#include <Engine/Scene/Components/ReflectionProbeComponent.hpp>
#include <Engine/Scene/Components/VolumeComponent.hpp>
#include <Engine/Scene/EntityRef.hpp>
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
#include <Engine/Scene/Components/ParticleGpuSimulation.hpp>
namespace fbzz::scene {
using namespace script_proxy_detail;
namespace {

DecalComponent* SelfDecal(const Script* script)
{
    return SelfComponent<DecalComponent>(script);
}


/// @brief AssignUIMaterialOverride と同じ理由で assign を使う (毎フレーム呼ばれる Setter)。
void AssignDecalMaterialOverride(DecalComponent& decal, std::string_view param,
                                 const float* values, std::size_t count)
{
    auto& target = decal.materialParamOverrides[std::string(param)];
    target.assign(values, values + count);
}


VolumeComponent* SelfVolume(const Script* script)
{
    return SelfComponent<VolumeComponent>(script);
}


ReflectionProbeComponent* SelfReflectionProbe(const Script* script)
{
    return SelfComponent<ReflectionProbeComponent>(script);
}

} /// @note namespace
void ScriptParticleProxy::SetEmitRate(float rate) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) p->settings.emitRate = rate;
}


void ScriptParticleProxy::SetEmitPosition(const math::Vector3& position) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) p->settings.emitPosition = position;
}


void ScriptParticleProxy::SetEmitVelocity(const math::Vector3& velocity) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) p->settings.emitVelocity = velocity;
}


void ScriptParticleProxy::SetVelocitySpread(float spread) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) p->settings.velocitySpread = (std::max)(spread, 0.0f);
}


void ScriptParticleProxy::SetEnabled(bool enabled) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) p->settings.enabled = enabled;
}


void ScriptParticleProxy::Play(bool restart) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) p->Play(restart);
}


void ScriptParticleProxy::Stop(bool clear) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) p->Stop(clear);
}


void ScriptParticleProxy::Burst(int count) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) p->Burst(count);
}


void ScriptParticleProxy::Clear() const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) p->ClearParticles();
}


void ScriptParticleProxy::SetGravity(const math::Vector3& gravity) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) p->settings.SetGravityAcceleration(gravity);
}


void ScriptParticleProxy::SetColor(const math::Vector4& start, const math::Vector4& end) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) {
        p->settings.colorStart = start;
        p->settings.colorEnd = end;
    }
}


void ScriptParticleProxy::SetSize(float start, float end) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) {
        p->settings.sizeStart = (std::max)(start, 0.0f);
        p->settings.sizeEnd = (std::max)(end, 0.0f);
    }
}


void ScriptParticleProxy::SetLifetime(float seconds) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) p->settings.lifetime = (std::max)(seconds, 0.01f);
}


void ScriptParticleProxy::SetMaxParticles(int maxParticles) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) {
        const int clamped = (std::max)(maxParticles, 1);
        if (p->settings.maxParticles != clamped) {
            p->settings.maxParticles = clamped;
            p->runtime.gpuInitialized = false;
            p->runtime.gpuCapacity = 0;
        }
    }
}


void ScriptParticleProxy::SetPlayback(bool loop, float duration, bool clearOnStop) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) {
        p->settings.loop = loop;
        p->settings.duration = (std::max)(duration, 0.0f);
        p->settings.clearOnStop = clearOnStop;
    }
}


void ScriptParticleProxy::SetShape(ParticleEmitterShape shape) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) p->settings.shape = shape;
}


void ScriptParticleProxy::SetSphereShape(float radius) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) {
        p->settings.shape = ParticleEmitterShape::Sphere;
        p->settings.sphereRadius = (std::max)(radius, 0.0f);
    }
}


void ScriptParticleProxy::SetConeShape(float radius, float angleDegrees) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) {
        p->settings.shape = ParticleEmitterShape::Cone;
        p->settings.coneRadius = (std::max)(radius, 0.0f);
        p->settings.coneAngleDegrees = (std::max)(angleDegrees, 0.0f);
    }
}


void ScriptParticleProxy::SetBoxShape(const math::Vector3& extents) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) {
        p->settings.shape = ParticleEmitterShape::Box;
        p->settings.boxExtents = {
            (std::max)(extents.x, 0.0f),
            (std::max)(extents.y, 0.0f),
            (std::max)(extents.z, 0.0f)
        };
    }
}


void ScriptParticleProxy::SetMeshShape(std::string_view modelPath, int meshIndex, float scale,
                                       bool followSkinnedAnimation, float normalVelocity) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) {
        p->settings.shape = ParticleEmitterShape::MeshSurface;
        p->settings.meshShapePath = std::string(modelPath);
        p->settings.meshShapeIndex = meshIndex;
        p->settings.meshShapeScale = (std::max)(scale, 0.0001f);
        p->settings.meshShapeFollowSkinnedAnimation = followSkinnedAnimation;
        p->settings.meshShapeNormalVelocity = normalVelocity;
        /// @note パス・サブメッシュ変更時は次のスポーンでFBX頂点を再構築する。
        p->runtime.loadedMeshShapePath.clear();
        p->runtime.loadedMeshShapeIndex = -2;
        p->runtime.meshShapeVertices.clear();
        p->runtime.meshShapeTriangles.clear();
    }
}


void ScriptParticleProxy::SetMeshShapeNormalVelocity(float normalVelocity) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script))
        p->settings.meshShapeNormalVelocity = normalVelocity;
}


void ScriptParticleProxy::SetSortMode(ParticleSortMode sortMode) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) p->settings.sortMode = sortMode;
}


void ScriptParticleProxy::SetSimulationMode(ParticleSimulationMode simulationMode) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) {
        if (p->settings.simulationMode != simulationMode) p->runtime.gpuClearPending = true;
        p->settings.simulationMode = simulationMode;
    }
}


void ScriptParticleProxy::SetSimulationSpace(ParticleSimulationSpace space) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) {
        p->settings.simulationSpace = space;
        if (space == ParticleSimulationSpace::Local)
            p->settings.simulationMode = ParticleSimulationMode::Cpu;
    }
}


void ScriptParticleProxy::SetRenderMode(ParticleRenderMode mode, float stretchScale) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) {
        p->settings.renderMode = mode;
        p->settings.stretchedVelocityScale = (std::max)(stretchScale, 0.0f);
    }
}


void ScriptParticleProxy::SetCollision(ParticleCollisionMode mode,
                                       ParticleCollisionResponse response,
                                       float radius, float bounciness) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) {
        p->settings.collisionMode = mode;
        p->settings.collisionResponse = response;
        p->settings.collisionRadius = (std::max)(radius, 0.0f);
        p->settings.collisionBounciness = (std::max)(0.0f, (std::min)(bounciness, 1.0f));
        if (mode == ParticleCollisionMode::Physics)
            p->settings.simulationMode = ParticleSimulationMode::Cpu;
    }
}


int ScriptParticleProxy::GetCollisionCount() const
{
    if (const auto* p = SelfComponent<ParticleEmitter>(script))
        return p->runtime.collisionCountThisFrame;
    return 0;
}


void ScriptParticleProxy::SetRateOverDistance(float particlesPerMeter) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script))
        p->settings.rateOverDistance = (std::max)(particlesPerMeter, 0.0f);
}


void ScriptParticleProxy::SetPrewarm(bool enabled) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) {
        p->settings.prewarm = enabled;
        p->runtime.prewarmed = false;
        p->runtime.prewarmSpawnPending = 0;
    }
}


void ScriptParticleProxy::SetSubEmitters(std::string_view birthEmitter,
                                         std::string_view deathEmitter,
                                         std::string_view collisionEmitter,
                                         int burstCount) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) {
        p->settings.birthSubEmitter = std::string(birthEmitter);
        p->settings.deathSubEmitter = std::string(deathEmitter);
        p->settings.collisionSubEmitter = std::string(collisionEmitter);
        p->settings.subEmitterBurstCount = (std::max)(burstCount, 1);
    }
}


void ScriptParticleProxy::SetVelocityDamping(float damping) const
{
    /// @note 旧 API 名のまま。減衰は «流れへ寄る速さ» になったので flowCoupling を触る。
    if (auto* p = SelfComponent<ParticleEmitter>(script))
        p->settings.flowCoupling = (std::max)(damping, 0.0f);
}


void ScriptParticleProxy::SetAngularVelocity(float minValue, float maxValue) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) {
        p->settings.angularVelocityMin = minValue;
        p->settings.angularVelocityMax = maxValue;
    }
}


void ScriptParticleProxy::SetNoise(float strength, float frequency, float speed) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) {
        FlowFieldSettings& curl = p->settings.EnsureLocalForce(FlowFieldType::Curl);
        curl.strength       = (std::max)(strength, 0.0f);
        curl.noiseFrequency = (std::max)(frequency, 0.0001f);
        curl.noiseSpeed     = speed;
    }
}


void ScriptParticleProxy::SetReceiveFlowFields(bool receive) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) p->settings.receiveFlowFields = receive;
}


void ScriptParticleProxy::SetFlowFieldChannels(uint32_t channels) const
{
    if (auto* p = SelfComponent<ParticleEmitter>(script)) p->settings.flowFieldChannels = channels;
}


bool ScriptParticleProxy::IsEnabled() const
{
    const auto* p = SelfComponent<ParticleEmitter>(script);
    return p && p->settings.enabled;
}


bool ScriptParticleProxy::IsPlaying() const
{
    const auto* p = SelfComponent<ParticleEmitter>(script);
    return p && p->settings.playing;
}


int ScriptParticleProxy::GetParticleCount() const
{
    const auto* p = SelfComponent<ParticleEmitter>(script);
    return p ? static_cast<int>(p->runtime.particles.size()) : 0;
}


float ScriptParticleProxy::GetEmitRate() const
{
    const auto* p = SelfComponent<ParticleEmitter>(script);
    return p ? p->settings.emitRate : 0.0f;
}


int ScriptParticleProxy::GetMaxParticles() const
{
    const auto* p = SelfComponent<ParticleEmitter>(script);
    return p ? p->settings.maxParticles : 0;
}


float ScriptParticleProxy::GetLifetime() const
{
    const auto* p = SelfComponent<ParticleEmitter>(script);
    return p ? p->settings.lifetime : 0.0f;
}


uint32_t ScriptParticleProxy::GetFlowFieldChannels() const
{
    const auto* p = SelfComponent<ParticleEmitter>(script);
    return p ? p->settings.flowFieldChannels : 0u;
}


float ScriptParticleProxy::GetPlayTime() const
{
    const auto* p = SelfComponent<ParticleEmitter>(script);
    return p ? p->runtime.playTime : 0.0f;
}


ParticleSimulationMode ScriptParticleProxy::GetSimulationMode() const
{
    const auto* p = SelfComponent<ParticleEmitter>(script);
    return p ? p->settings.simulationMode : ParticleSimulationMode::Cpu;
}


ParticleGpuFallbackReason ScriptParticleProxy::GetGpuFallbackReason() const
{
    const auto* p = SelfComponent<ParticleEmitter>(script);
    if (!p) return ParticleGpuFallbackReason::NotRequested;
    /// @note ParticlePass と同じ引数で呼ぶ。runtime.material を渡さないと .mat 由来の 3 条件
    /// @note (フリップブック補間・モーションベクター・自己影) が判定から抜け、
    /// @note 実際は CPU で回っているのに「GPU で回る」と答えてしまう。
    return GetParticleGpuFallbackReason(p->settings, &p->runtime.material);
}


bool ScriptParticleProxy::IsGpuSimulated() const
{
    return GetGpuFallbackReason() == ParticleGpuFallbackReason::None;
}


const char* ScriptParticleProxy::GetGpuFallbackField() const
{
    return ParticleGpuFallbackFieldName(GetGpuFallbackReason());
}


const char* ScriptParticleProxy::GetGpuFallbackDescription() const
{
    return ParticleGpuFallbackDescription(GetGpuFallbackReason());
}


void ScriptVFXProxy::Play(bool restart) const
{
    if (auto* vfx = SelfComponent<VFXComponent>(script)) {
        if (restart) vfx->Restart();
        else vfx->Resume();
    }
}


void ScriptVFXProxy::Pause() const
{
    if (auto* vfx = SelfComponent<VFXComponent>(script)) vfx->Pause();
}


void ScriptVFXProxy::Stop() const
{
    if (auto* vfx = SelfComponent<VFXComponent>(script)) vfx->Stop();
}


void ScriptVFXProxy::SetSpeed(float speed) const
{
    if (auto* vfx = SelfComponent<VFXComponent>(script))
        vfx->speed = (std::max)(speed, 0.0f);
}


bool ScriptVFXProxy::Trigger(std::string_view name) const
{
    if (name.empty()) return false;
    auto* vfx = SelfComponent<VFXComponent>(script);
    if (vfx == nullptr) return false;
    vfx->Trigger(name);
    return true;
}


bool ScriptVFXProxy::IsPlaying() const
{
    const auto* vfx = SelfComponent<VFXComponent>(script);
    return vfx != nullptr && vfx->playing;
}


float ScriptVFXProxy::GetTime() const
{
    const auto* vfx = SelfComponent<VFXComponent>(script);
    return vfx != nullptr ? vfx->time : 0.0f;
}


float ScriptVFXProxy::GetDuration() const
{
    const auto* vfx = SelfComponent<VFXComponent>(script);
    if (vfx == nullptr) return 0.0f;
    /// @note duration が 0 のときは配下から算出した実効尺が正。
    return vfx->duration > 0.0f ? vfx->duration : vfx->resolvedDuration;
}


float ScriptVFXProxy::GetSpeed() const
{
    const auto* vfx = SelfComponent<VFXComponent>(script);
    return vfx != nullptr ? vfx->speed : 1.0f;
}


void ScriptTrailProxy::SetEnabled(bool enabled, bool clearWhenDisabled) const
{
    if (auto* t = SelfComponent<TrailComponent>(script)) {
        t->enabled = enabled;
        t->clearOnDisable = clearWhenDisabled;
        if (!enabled && clearWhenDisabled)
            Clear();
    }
}


void ScriptTrailProxy::Clear() const
{
    if (auto* t = SelfComponent<TrailComponent>(script)) {
        t->ringHead = 0;
        t->ringTail = 0;
        t->ringCount = 0;
        t->lastSampleTime = -1.0f;
    }
}


void ScriptTrailProxy::SetDuration(float seconds) const
{
    if (auto* t = SelfComponent<TrailComponent>(script))
        t->duration = (std::max)(seconds, 0.01f);
}


void ScriptTrailProxy::SetMaxPoints(int maxPoints) const
{
    if (auto* t = SelfComponent<TrailComponent>(script))
        t->maxPoints = (std::max)(maxPoints, 2);
}


void ScriptTrailProxy::SetSampling(float sampleInterval, float minVertexDist) const
{
    if (auto* t = SelfComponent<TrailComponent>(script)) {
        t->sampleInterval = (std::max)(sampleInterval, 0.0f);
        t->minVertexDist = (std::max)(minVertexDist, 0.0f);
    }
}


void ScriptTrailProxy::SetWidth(float start, float end) const
{
    if (auto* t = SelfComponent<TrailComponent>(script)) {
        t->widthStart = (std::max)(start, 0.0f);
        t->widthEnd = (std::max)(end, 0.0f);
    }
}


void ScriptTrailProxy::SetWidthEasing(TrailWidthEasing easing) const
{
    if (auto* t = SelfComponent<TrailComponent>(script))
        t->widthEasing = easing;
}


void ScriptTrailProxy::SetColor(const math::Vector4& start, const math::Vector4& end) const
{
    if (auto* t = SelfComponent<TrailComponent>(script)) {
        t->colorStart = start;
        t->colorEnd = end;
    }
}


void ScriptTrailProxy::SetMaterial(std::string_view materialPath) const
{
    if (auto* t = SelfComponent<TrailComponent>(script)) {
        t->materialPath = std::string(materialPath);
        t->loadedMaterialPath.clear();
        t->texture = {};
    }
}


void ScriptTrailProxy::SetUVMode(TrailUVMode mode) const
{
    if (auto* t = SelfComponent<TrailComponent>(script))
        t->uvMode = mode;
}


void ScriptTrailProxy::SetAlignment(TrailAlignment alignment) const
{
    if (auto* t = SelfComponent<TrailComponent>(script))
        t->alignment = alignment;
}


void ScriptTrailProxy::SetCameraFacing() const
{
    SetAlignment(TrailAlignment::CameraFacing);
}


void ScriptTrailProxy::SetWorldUp() const
{
    SetAlignment(TrailAlignment::WorldUp);
}


void ScriptTrailProxy::SetSmoothSubdivisions(int subdivisions) const
{
    if (auto* t = SelfComponent<TrailComponent>(script))
        t->smoothSubdivisions = (std::max)(subdivisions, 0);
}


void ScriptTrailProxy::SetAttachBone(std::string_view boneName, const math::Vector3& offset) const
{
    if (auto* t = SelfComponent<TrailComponent>(script)) {
        t->attachBone = std::string(boneName);
        t->attachOffset = offset;
    }
}


void ScriptTrailProxy::ClearAttachBone() const
{
    if (auto* t = SelfComponent<TrailComponent>(script)) {
        t->attachBone.clear();
        t->attachOffset = math::Vector3::ZERO;
    }
}


bool ScriptTrailProxy::IsEnabled() const
{
    const auto* t = SelfComponent<TrailComponent>(script);
    return t && t->enabled;
}


float ScriptTrailProxy::GetDuration() const
{
    const auto* t = SelfComponent<TrailComponent>(script);
    return t ? t->duration : 0.0f;
}


int ScriptTrailProxy::GetPointCount() const
{
    const auto* t = SelfComponent<TrailComponent>(script);
    return t ? t->ringCount : 0;
}


void ScriptMeshTrailProxy::SetEnabled(bool enabled, bool clearWhenDisabled) const
{
    if (auto* t = SelfComponent<MeshTrailComponent>(script)) {
        t->enabled = enabled;
        t->clearOnDisable = clearWhenDisabled;
        if (!enabled && clearWhenDisabled)
            Clear();
    }
}


void ScriptMeshTrailProxy::Clear() const
{
    if (auto* t = SelfComponent<MeshTrailComponent>(script))
        t->clearRequested = true;
}


void ScriptMeshTrailProxy::SetDuration(float seconds) const
{
    if (auto* t = SelfComponent<MeshTrailComponent>(script))
        t->duration = (std::max)(seconds, 0.01f);
}


void ScriptMeshTrailProxy::SetSampling(float sampleInterval, float minVertexDist) const
{
    if (auto* t = SelfComponent<MeshTrailComponent>(script)) {
        t->sampleInterval = (std::max)(sampleInterval, 0.0f);
        t->minVertexDist = (std::max)(minVertexDist, 0.0f);
    }
}


void ScriptMeshTrailProxy::SetMaxSamples(int maxSamples) const
{
    if (auto* t = SelfComponent<MeshTrailComponent>(script))
        t->maxSamples = (std::max)(maxSamples, 1);
}


void ScriptMeshTrailProxy::SetColor(const math::Vector4& start, const math::Vector4& end) const
{
    if (auto* t = SelfComponent<MeshTrailComponent>(script)) {
        t->colorStart = start;
        t->colorEnd = end;
    }
}


void ScriptMeshTrailProxy::SetDoubleSided(bool doubleSided) const
{
    if (auto* t = SelfComponent<MeshTrailComponent>(script))
        t->doubleSided = doubleSided;
}


void ScriptMeshTrailProxy::AddExcludedMeshIndex(int meshIndex) const
{
    if (auto* t = SelfComponent<MeshTrailComponent>(script)) {
        if (meshIndex < 0)
            return;
        if (std::find(t->excludedMeshIndices.begin(), t->excludedMeshIndices.end(), meshIndex) == t->excludedMeshIndices.end())
            t->excludedMeshIndices.push_back(meshIndex);
    }
}


void ScriptMeshTrailProxy::ClearExcludedMeshIndices() const
{
    if (auto* t = SelfComponent<MeshTrailComponent>(script))
        t->excludedMeshIndices.clear();
}


bool ScriptMeshTrailProxy::IsEnabled() const
{
    const auto* t = SelfComponent<MeshTrailComponent>(script);
    return t && t->enabled;
}


float ScriptMeshTrailProxy::GetDuration() const
{
    const auto* t = SelfComponent<MeshTrailComponent>(script);
    return t ? t->duration : 0.0f;
}


int ScriptMeshTrailProxy::GetSampleCount() const
{
    const auto* t = SelfComponent<MeshTrailComponent>(script);
    return t ? t->sampleCount : 0;
}

/// @name UIButton 入力の取得
/// @brief UISystem が毎フレーム立て直す onClick / onEnter / onExit / state をそのまま読む。
/// @brief 取得系はボタンが無い場合に「押されていない」を返す方が呼び出し側の分岐が減るため、
/// @brief nullptr は一律 false / Disabled に潰す。

EntityRef ScriptDecalProxy::Spawn(const math::Vector3& point,
                                  const math::Vector3& normal,
                                  std::string_view materialPath,
                                  float size,
                                  float lifetime,
                                  float depth,
                                  float rollDegrees) const
{
    if (!script || !script->m_scene) return EntityRef{};
    Scene& scene = *script->m_scene;

    /// @note 投影軸はデカールのローカル +Y。法線をそのまま +Y へ向けるので、床への着弾は
    /// @note 無回転になる (BossShockwave / DecalScorch が置いている床デカールと同じ規約)。
    const math::Vector3 axis = normal.NormalizedOr(math::Vector3::UP);

    /// @note ローカル +Z は投影 UV の V 軸。法線に直交していれば向きは何でもよいので、
    /// @note 軸に平行になりにくい 2 本から選ぶ (真上 / 真横の面で基底が潰れるのを避ける)。
    math::Vector3 forward = math::Vector3::Cross(axis, math::Vector3::RIGHT);
    if (forward.LengthSq() < 1.0e-6f)
        forward = math::Vector3::Cross(axis, math::Vector3::FORWARD);
    forward = forward.NormalizedOr(math::Vector3::FORWARD);

    math::Quaternion rotation =
        math::Quaternion::LookRotation(forward, axis).Normalized();
    /// @note roll はローカル +Y (= 投影軸) まわり。右から掛けてローカル回転として足す。
    if (rollDegrees != 0.0f) {
        rotation = (rotation * math::Quaternion::FromAxisAngle(
                        math::Vector3::UP, rollDegrees * math::DEG2RAD)).Normalized();
    }

    GameObject* created = scene.TryCreateGameObject("Decal");
    if (!created) return EntityRef{};
    created->SetRuntimeGenerated(true);
    const EntityID id = created->GetID();
    scene.AddComponent<DecalComponent>(id, DecalComponent{});

    GameObject* spawned = scene.GetGameObject(id);
    if (!spawned) return EntityRef{};
    DecalComponent* decal = spawned->GetComponent<DecalComponent>();
    if (!decal) return EntityRef{};

    const float extent = (std::max)(size, 0.001f);
    /// @note worldPosition だけでは動かない。PrePhysics が local から組み直すため position も書く。
    spawned->transform.position      = point;
    spawned->transform.worldPosition = point;
    spawned->transform.rotation      = rotation;
    spawned->transform.worldRotation = rotation;
    spawned->transform.scale         = { extent, (std::max)(depth, 0.001f), extent };

    decal->materialPath = std::string(materialPath);
    decal->lifetime     = lifetime;

    return EntityRef{ id };
}


void ScriptDecalProxy::SetEnabled(bool enabled) const
{
    if (auto* d = SelfDecal(script)) d->enabled = enabled;
}

void ScriptDecalProxy::SetLifetime(float seconds) const
{
    if (auto* d = SelfDecal(script)) d->lifetime = seconds;
}

void ScriptDecalProxy::SetFadeTime(float fadeTime) const
{
    if (auto* d = SelfDecal(script)) d->fadeTime = fadeTime;
}

void ScriptDecalProxy::SetFadeInTime(float seconds) const
{
    if (auto* d = SelfDecal(script)) d->fadeInTime = (std::max)(seconds, 0.0f);
}

void ScriptDecalProxy::SetFlipbook(int frameCount, int framesPerRow,
                                   float frameRate, bool loop) const
{
    auto* d = SelfDecal(script);
    if (!d) return;
    d->frameCount   = (std::max)(frameCount, 0);
    d->framesPerRow = std::clamp(framesPerRow, 1, (std::max)(d->frameCount, 1));
    d->frameRate    = (std::max)(frameRate, 0.0f);
    d->frameLoop    = loop;
}

void ScriptDecalProxy::SetSortOrder(int order) const
{
    if (auto* d = SelfDecal(script)) d->sortOrder = order;
}

void ScriptDecalProxy::ResetAge() const
{
    if (auto* d = SelfDecal(script)) d->age = 0.0f;
}

void ScriptDecalProxy::SetAlbedoColor(float r, float g, float b, float a) const
{
    auto* d = SelfDecal(script);
    if (!d) return;
    d->albedoColor[0] = r; d->albedoColor[1] = g;
    d->albedoColor[2] = b; d->albedoColor[3] = a;
}

void ScriptDecalProxy::SetNormalStrength(float strength) const
{
    if (auto* d = SelfDecal(script)) d->normalStrength = strength;
}

void ScriptDecalProxy::SetEmissiveColor(float r, float g, float b) const
{
    auto* d = SelfDecal(script);
    if (!d) return;
    d->emissiveColor[0] = r; d->emissiveColor[1] = g; d->emissiveColor[2] = b;
}

void ScriptDecalProxy::SetEmissiveScale(float scale) const
{
    if (auto* d = SelfDecal(script)) d->emissiveScale = scale;
}

void ScriptDecalProxy::SetAlbedoTexture(std::string_view path) const
{
    if (auto* d = SelfDecal(script)) d->albedoTexPath = std::string(path);
}

void ScriptDecalProxy::SetNormalTexture(std::string_view path) const
{
    if (auto* d = SelfDecal(script)) d->normalTexPath = std::string(path);
}

void ScriptDecalProxy::SetEmissiveTexture(std::string_view path) const
{
    if (auto* d = SelfDecal(script)) d->emissiveTexPath = std::string(path);
}

void ScriptDecalProxy::SetOpacity(float opacity) const
{
    if (auto* d = SelfDecal(script)) d->opacity = std::clamp(opacity, 0.0f, 1.0f);
}

bool ScriptDecalProxy::IsEnabled() const
{
    const auto* d = SelfDecal(script);
    return d && d->enabled;
}

float ScriptDecalProxy::GetOpacity() const
{
    const auto* d = SelfDecal(script);
    return d ? d->opacity : 0.0f;
}

float ScriptDecalProxy::GetLifetime() const
{
    const auto* d = SelfDecal(script);
    return d ? d->lifetime : -1.0f;
}

float ScriptDecalProxy::GetFadeTime() const
{
    const auto* d = SelfDecal(script);
    return d ? d->fadeTime : 0.0f;
}

float ScriptDecalProxy::GetAge() const
{
    const auto* d = SelfDecal(script);
    return d ? d->age : 0.0f;
}

float ScriptDecalProxy::GetEffectiveOpacity() const
{
    const auto* d = SelfDecal(script);
    if (!d) return 0.0f;
    /// @note DecalPass のライフタイムフェードと同じ式。片方だけ変えると
    /// @note 「見た目は消えているのに Script は 1.0 を読む」というズレになる。
    float fade = 1.0f;
    if (d->lifetime >= 0.0f && d->fadeTime > 0.0f)
        fade = (std::min)(1.0f, (d->lifetime - d->age) / d->fadeTime);
    if (d->fadeInTime > 0.0f)
        fade = (std::min)(fade, d->age / d->fadeInTime);
    return std::clamp(fade, 0.0f, 1.0f) * std::clamp(d->opacity, 0.0f, 1.0f);
}

void ScriptDecalProxy::SetAngleFade(float strength, float limitDegrees) const
{
    auto* d = SelfDecal(script);
    if (!d) return;
    d->angleFadeStrength = std::clamp(strength, 0.0f, 1.0f);
    d->angleFadeDegrees  = std::clamp(limitDegrees, 0.0f, 89.0f);
}

void ScriptDecalProxy::SetReceiverLayerMask(uint32_t mask) const
{
    if (auto* d = SelfDecal(script)) d->receiverLayerMask = mask;
}

void ScriptDecalProxy::SetMaterial(std::string_view materialPath) const
{
    auto* d = SelfDecal(script);
    if (!d) return;
    std::string next(materialPath);
    if (d->materialPath == next) return;
    d->materialPath = std::move(next);
    /// @note 別のマテリアルへ移ると、前のシェーダーに合わせた上書きは意味を失う。
    /// @note 残すと「効かない上書き」が黙って積まれ、綴り間違いと区別が付かなくなる。
    d->materialParamOverrides.clear();
    d->materialTextureOverrides.clear();
}


void ScriptDecalProxy::SetMaterialFloat(std::string_view param, float value) const
{
    if (auto* d = SelfDecal(script)) AssignDecalMaterialOverride(*d, param, &value, 1);
}

void ScriptDecalProxy::SetMaterialVector2(std::string_view param, float x, float y) const
{
    const float values[2] = { x, y };
    if (auto* d = SelfDecal(script)) AssignDecalMaterialOverride(*d, param, values, 2);
}

void ScriptDecalProxy::SetMaterialVector4(std::string_view param, const math::Vector4& value) const
{
    const float values[4] = { value.x, value.y, value.z, value.w };
    if (auto* d = SelfDecal(script)) AssignDecalMaterialOverride(*d, param, values, 4);
}

void ScriptDecalProxy::SetMaterialColor(std::string_view param, const math::Vector4& color) const
{
    SetMaterialVector4(param, color);
}

void ScriptDecalProxy::SetMaterialTexture(std::string_view slot, std::string_view texturePath) const
{
    if (auto* d = SelfDecal(script))
        d->materialTextureOverrides[std::string(slot)].assign(texturePath.data(), texturePath.size());
}

void ScriptDecalProxy::ClearMaterialOverride(std::string_view param) const
{
    auto* d = SelfDecal(script);
    if (!d) return;
    const std::string key(param);
    d->materialParamOverrides.erase(key);
    d->materialTextureOverrides.erase(key);
}

void ScriptDecalProxy::ClearMaterialOverrides() const
{
    auto* d = SelfDecal(script);
    if (!d) return;
    d->materialParamOverrides.clear();
    d->materialTextureOverrides.clear();
}

/// @brief ScriptVolumeProxy

void ScriptVolumeProxy::SetEnabled(bool enabled) const
{
    if (auto* v = SelfVolume(script)) v->enabled = enabled;
}

void ScriptVolumeProxy::SetGravity(const math::Vector3& gravity) const
{
    if (auto* v = SelfVolume(script)) v->gravity = gravity;
}

void ScriptVolumeProxy::SetSwirlStrength(float strength) const
{
    if (auto* v = SelfVolume(script)) v->swirlStrength = strength;
}

void ScriptVolumeProxy::SetExplosionImpulse(float impulse) const
{
    if (auto* v = SelfVolume(script)) v->explosionImpulse = impulse;
}

void ScriptVolumeProxy::SetTimeScale(float scale) const
{
    if (auto* v = SelfVolume(script)) v->timeScale = scale;
}

void ScriptVolumeProxy::SetDuration(float seconds) const
{
    if (auto* v = SelfVolume(script)) v->duration = seconds;
}

void ScriptVolumeProxy::ResetElapsed() const
{
    if (auto* v = SelfVolume(script)) v->elapsed = 0.0f;
}

bool ScriptVolumeProxy::IsEnabled() const
{
    const auto* v = SelfVolume(script);
    return v && v->enabled;
}

float ScriptVolumeProxy::GetTimeScale() const
{
    const auto* v = SelfVolume(script);
    return v ? v->timeScale : 1.0f;
}

float ScriptVolumeProxy::GetDuration() const
{
    const auto* v = SelfVolume(script);
    return v ? v->duration : -1.0f;
}

float ScriptVolumeProxy::GetElapsed() const
{
    const auto* v = SelfVolume(script);
    return v ? v->elapsed : 0.0f;
}

/// @brief ScriptReflectionProbeProxy

void ScriptReflectionProbeProxy::SetEnabled(bool enabled) const
{
    if (auto* rp = SelfReflectionProbe(script)) rp->enabled = enabled;
}

void ScriptReflectionProbeProxy::SetIntensity(float intensity) const
{
    if (auto* rp = SelfReflectionProbe(script)) rp->intensity = intensity;
}

void ScriptReflectionProbeProxy::SetInfluenceRadius(float radius) const
{
    if (auto* rp = SelfReflectionProbe(script)) rp->influenceRadius = radius;
}

void ScriptReflectionProbeProxy::SetBoxInfluence(bool useBox) const
{
    if (auto* rp = SelfReflectionProbe(script)) rp->boxInfluence = useBox;
}

void ScriptReflectionProbeProxy::SetBoxExtents(const math::Vector3& halfExtents) const
{
    if (auto* rp = SelfReflectionProbe(script)) rp->boxExtents = halfExtents;
}

void ScriptReflectionProbeProxy::SetCubemap(std::string_view path) const
{
    if (auto* rp = SelfReflectionProbe(script)) rp->cubemapPath = std::string(path);
}

bool ScriptReflectionProbeProxy::IsEnabled() const
{
    const auto* rp = SelfReflectionProbe(script);
    return rp && rp->enabled;
}

float ScriptReflectionProbeProxy::GetIntensity() const
{
    const auto* rp = SelfReflectionProbe(script);
    return rp ? rp->intensity : 0.0f;
}

float ScriptReflectionProbeProxy::GetInfluenceRadius() const
{
    const auto* rp = SelfReflectionProbe(script);
    return rp ? rp->influenceRadius : 0.0f;
}


} /// @note namespace fbzz::scene
