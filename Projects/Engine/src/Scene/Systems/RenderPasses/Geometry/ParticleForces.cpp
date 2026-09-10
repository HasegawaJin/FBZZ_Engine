/// @file    ParticleForces.cpp
/// @brief   力場の解決と適用。式は ParticleGpuSim.cs.hlsl と一致させること。
/// @author  Hasegawa Jin
/// @date    2026-09-11
#include "ParticleForces.hpp"

#include "GeometryPasses.hpp"
#include "ParticleEmitterSpace.hpp"
#include "Engine/Asset/AssetManager.hpp"
#include "Engine/Asset/VectorFieldAsset.hpp"
#include "Engine/Core/CurlNoise.hpp"
#include "Engine/Scene/Components/ParticleEmitter.hpp"
#include "Engine/Scene/Scene.hpp"
#include "Engine/Scene/Transform.hpp"
#include <algorithm>
#include <cmath>
#include <string>

namespace fbzz::scene {
namespace {

// 乱流の式は Engine/Core/CurlNoise.hpp が正本。ParticleForces・VectorFieldAsset・
// ParticleNoise.hlsli の 3 者で同じ値が出ることが前提の機能なので、実体は 1 つに保つ。
using core::CurlNoise;
using core::TurbulenceSamplePoint;

// tightness を «場へ寄る速さ» へ直す係数 [1/s]。
// ParticleGpuSim.cs.hlsl の VELOCITY_FIELD_TIGHTNESS_RATE と一致させること。
// 1.0 で半減期およそ 35ms — 「流れに乗り換えた」と見える最短のあたり。
constexpr float kVelocityFieldTightnessRate = 20.0f;

// .vfield を解決する。パス→ハンドルは AssetManager が持つので毎フレーム呼んでよい
// (中身はハッシュ 1 回。materialPath の解決と同じ流儀)。
const asset::VectorFieldAsset* ResolveVectorField(const std::string& path)
{
    if (path.empty()) return nullptr;
    const auto handle = asset::AssetManager::Load<asset::VectorFieldAsset>(path);
    return handle.IsValid() ? asset::AssetManager::Get(handle) : nullptr;
}

} // namespace

// 力場がこのエミッターに作用するか。収集はパス先頭で 1 回だけ行い全エミッターで
// 共有するので、絞り込みは適用時に行う。
bool AffectsEmitter(const ActiveForceField& field, uint32_t emitterChannels)
{
    return (field.channels & emitterChannels) != 0u;
}

// WHY 共通化するか: シーンに置いた力場とエミッター内蔵の力で、falloff の下限や
//      方向の正規化がずれていると «同じ設定なのに置き方で効き方が違う» になる。
ActiveForceField ResolveForceField(const ParticleForceFieldSettings& settings,
                                   const math::Vector3&    origin,
                                   const math::Quaternion& rotation,
                                   bool                    isLocal)
{
    ActiveForceField f;
    f.position = origin;
    f.radius   = settings.radius;
    const math::Vector3 worldDir = rotation * settings.direction;
    const float dirLen = worldDir.Length();
    f.direction      = dirLen > 1.0e-4f ? worldDir * (1.0f / dirLen)
                                        : math::Vector3{ 0.0f, 1.0f, 0.0f };
    f.strength       = settings.strength;
    f.type           = settings.fieldType;
    f.falloffPower   = (std::max)(settings.falloffPower, 0.001f);
    f.noiseFrequency = (std::max)(settings.noiseFrequency, 0.0001f);
    f.noiseSpeed     = settings.noiseSpeed;
    f.channels       = settings.channels;
    f.local          = isLocal;
    if (settings.fieldType == ParticleForceFieldType::VectorField) {
        // 速度場の効く範囲は extents (焼いた AABB の貼り先) が決める。radius まで見ると
        // 球で二重にクリップされる。Inspector は VectorField のとき Radius を出さないので、
        // 型を切り替えて残った値が «見えないのに効く» ことのないよう無効化する。
        f.radius          = 0.0f;
        f.vectorField     = ResolveVectorField(settings.vectorFieldPath);
        f.inverseRotation = rotation.Conjugate();
        f.extents         = {
            (std::max)(std::abs(settings.vectorFieldExtents.x), 1.0e-3f),
            (std::max)(std::abs(settings.vectorFieldExtents.y), 1.0e-3f),
            (std::max)(std::abs(settings.vectorFieldExtents.z), 1.0e-3f) };
        f.tightness       = std::clamp(settings.vectorFieldTightness, 0.0f, 1.0f);
    }
    return f;
}

std::vector<ActiveForceField> GatherForceFields(Scene& scene, uint32_t cullingMask)
{
    std::vector<ActiveForceField> fields;
    for (auto& go : scene.GameObjects()) {
        if (!ShouldRenderGameObject(go, cullingMask)) continue;
        auto* ff = go.GetComponent<ParticleForceField>();
        if (!ff || !ff->enabled) continue;
        // direction はローカル指定。GameObject を回せば風向き・渦軸も回る。
        fields.push_back(ResolveForceField(*ff, go.transform.worldPosition,
                                           go.transform.worldRotation, /*isLocal=*/false));
    }

    // WindZone をシーングローバルの風 (+乱流) として力場リストへ追加する。
    // 草・雲と同じ WindZone 1 つでパーティクルもなびく。個別に強い風が要るなら
    // ParticleForceField(Wind) を置く。
    const ActiveWindZone windZone = FindActiveWindZone(scene);
    if (windZone.active && windZone.strength > 0.0f) {
        ActiveForceField wind{};
        wind.position       = math::Vector3::ZERO;
        wind.radius         = 0.0f; // 無限 (減衰なし)
        wind.direction      = windZone.direction;
        wind.strength       = windZone.strength;
        wind.type           = ParticleForceFieldType::Wind;
        wind.falloffPower   = 1.0f;
        wind.noiseFrequency = 0.5f;
        wind.noiseSpeed     = 1.0f;
        wind.channels       = 0xFFFFFFFFu; // 環境風はチャンネルで除外させない
        fields.push_back(wind);
    }
    if (windZone.active && windZone.turbulence > 0.0f) {
        ActiveForceField turb{};
        turb.position       = math::Vector3::ZERO;
        turb.radius         = 0.0f;
        turb.direction      = windZone.direction;
        turb.strength       = windZone.turbulence;
        turb.type           = ParticleForceFieldType::Turbulence;
        turb.falloffPower   = 1.0f;
        turb.noiseFrequency = 0.5f;
        turb.noiseSpeed     = windZone.pulseFrequency;
        turb.channels       = 0xFFFFFFFFu;
        fields.push_back(turb);
    }
    return fields;
}

std::vector<ActiveForceField> GatherForceFields(RenderPassContext& ctx)
{
    return GatherForceFields(ctx.scene, ctx.cullingMask);
}

// 契約 (どの空間で効くか・dragScale が掛かる相手) は ParticleForces.hpp を参照。
void ApplyForceFields(const std::vector<ActiveForceField>& fields,
                      uint32_t             emitterChannels,
                      const math::Vector3& position,
                      math::Vector3&       velocity,
                      float dt, float time, float dragScale)
{
    for (const auto& f : fields) {
        if (!AffectsEmitter(f, emitterChannels)) continue;
        const math::Vector3 toParticle = position - f.position;
        float influence = 1.0f;
        if (f.radius > 0.0f) {
            const float dist = toParticle.Length();
            if (dist >= f.radius) continue;
            influence = std::pow(1.0f - dist / f.radius, f.falloffPower);
        }
        float impulse = f.strength * influence * dt;
        // 寿命による減衰の作り分けは «この粒子の設定» なので、内蔵の Drag にだけ掛ける。
        if (f.local && f.type == ParticleForceFieldType::Drag) impulse *= dragScale;
        switch (f.type) {
        case ParticleForceFieldType::Wind:
            velocity = velocity + f.direction * impulse;
            break;
        case ParticleForceFieldType::Attract:
        case ParticleForceFieldType::Repulse: {
            const float dist = (std::max)(toParticle.Length(), 1.0e-4f);
            const math::Vector3 dir = toParticle * (1.0f / dist);
            velocity = velocity + dir * (f.type == ParticleForceFieldType::Repulse
                                             ? impulse : -impulse);
            break;
        }
        case ParticleForceFieldType::Vortex: {
            // 軸×粒子方向の外積 = 接線方向。軸周りに回す
            const math::Vector3 tangent = math::Vector3::Cross(f.direction, toParticle);
            const float len = tangent.Length();
            if (len > 1.0e-4f)
                velocity = velocity + tangent * (impulse / len);
            break;
        }
        case ParticleForceFieldType::Turbulence:
            velocity = velocity + CurlNoise(TurbulenceSamplePoint(
                position, f.noiseFrequency, f.noiseSpeed, time)) * impulse;
            break;
        case ParticleForceFieldType::Drag:
            // strength を減衰係数 [1/s] として扱う
            velocity = velocity * (std::max)(0.0f, 1.0f - impulse);
            break;
        case ParticleForceFieldType::VectorField: {
            if (f.vectorField == nullptr || f.vectorField->Empty()) break;
            // 場のローカルへ: 平行移動 → 逆回転 → 指定した寸法で正規化。
            // 式は ParticleGpuSim.cs.hlsl の FF_VECTOR_FIELD 分岐と一致させること。
            //
            // アセット側の bounds をそのまま使わないのは、同じ 1 枚を «部屋いっぱいの渦» と
            // «手のひらの渦» に貼り分けられるようにするため。
            const math::Vector3 localOffset = f.inverseRotation * toParticle;
            const math::Vector3 uvw = {
                localOffset.x / f.extents.x * 0.5f + 0.5f,
                localOffset.y / f.extents.y * 0.5f + 0.5f,
                localOffset.z / f.extents.z * 0.5f + 0.5f };
            if (uvw.x < 0.0f || uvw.x > 1.0f || uvw.y < 0.0f || uvw.y > 1.0f
                || uvw.z < 0.0f || uvw.z > 1.0f) break;

            // uvw [0,1] をアセットの bounds へ写して引く。
            const math::Vector3& assetMin = f.vectorField->boundsMin;
            const math::Vector3& assetMax = f.vectorField->boundsMax;
            const math::Vector3 samplePoint = {
                assetMin.x + (assetMax.x - assetMin.x) * uvw.x,
                assetMin.y + (assetMax.y - assetMin.y) * uvw.y,
                assetMin.z + (assetMax.z - assetMin.z) * uvw.z };
            const math::Vector3 fieldValue = f.vectorField->SampleLocal(samplePoint);
            // 場は «場のローカル» で焼かれているのでワールドへ戻す。
            const math::Vector3 worldValue = f.inverseRotation.Conjugate() * fieldValue;

            velocity = velocity + worldValue * (f.strength * influence * dt);

            // tightness は «どれだけ強く場へ従わせるか»。指数接近にしてあるのは、
            // 線形に混ぜるとフレームレートで収束速度が変わるため (30fps と 120fps で
            // 別の軌跡になる)。半減期で書けば dt に依らず同じ絵になる。
            if (f.tightness > 0.0f) {
                const float blend = 1.0f - std::exp2(
                    -dt * f.tightness * influence * kVelocityFieldTightnessRate);
                velocity = velocity + (worldValue - velocity) * blend;
            }
            break;
        }
        }
    }
}


// このエミッターに効く力を 1 本のリストへまとめる。すべて **ワールド空間** で解決する。
//
// WHY ワールドに揃えるか: 力場・乱流はもともとワールドで解決され、内蔵の重力と周回だけが
//   «シミュレーション空間» で適用されていた。混在していると simulationSpace = Local の
//   エミッターを傾けたとき、重力だけが一緒に傾いて横へ落ちる。空間を 1 つに決めれば
//   評価器も 1 本で済み、傾けたエミッターも素直に «下» へ落ちる。
//
// 並び順は «内蔵の力が先»。Drag が速度に掛け算で効く以外はすべて加算なので、
// 順序が絵を変えるのは Drag が絡むときだけ。それでも順を固定しておくのは、
// シーンにオブジェクトを 1 つ足しただけで既存の煙の減衰が変わらないようにするため。
//
// receiveForceFields が効くのはシーン側だけ。内蔵の力は «このエミッターの運動» なので、
// 環境の風を切っても重力まで消えたりはしない。
void ResolveEmitterForces(const ParticleEmitter& emitter, const Transform& tf,
                          const std::vector<ActiveForceField>& sceneFields,
                          std::vector<ActiveForceField>&       outFields)
{
    outFields.clear();

    const math::Vector3 emitterOrigin = TransformEmitterPoint(tf, emitter.settings.emitPosition);

    for (const ParticleForceFieldSettings& force : emitter.settings.localForces) {
        if (!force.enabled) continue;
        // Emitter 空間の力 (周回・放射) だけがエミッターの位置と回転に追従する。
        // 重力や乱流を追従させると、エミッターを傾けただけで下が変わってしまう。
        const bool emitterSpace = force.space == ParticleForceFieldSpace::Emitter;
        ActiveForceField resolved = ResolveForceField(
            force,
            emitterSpace ? emitterOrigin : math::Vector3::ZERO,
            emitterSpace ? tf.worldRotation : math::Quaternion{},
            /*isLocal=*/true);
        // 内蔵の力は相手が既にこのエミッター 1 体に決まっている。channels を持ち込むと
        // «自分の重力が自分のマスクに弾かれる» という説明のつかない挙動になる。
        resolved.channels = 0xFFFFFFFFu;
        outFields.push_back(resolved);
    }

    if (!emitter.settings.receiveForceFields) return;
    for (const ActiveForceField& field : sceneFields) {
        if (!AffectsEmitter(field, emitter.settings.forceFieldChannels)) continue;
        outFields.push_back(field);
    }
}

} // namespace fbzz::scene
