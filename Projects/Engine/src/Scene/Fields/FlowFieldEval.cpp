/// @file    FlowFieldEval.cpp
/// @brief   流れの解決とサンプル、粒子の緩和。式は ParticleGpuSim.cs.hlsl と一致させること。
/// @author  Hasegawa Jin
/// @date    2026-09-11
#include <Engine/Scene/Fields/FlowFieldEval.hpp>

#include "Engine/Asset/AssetManager.hpp"
#include "Fluid/VectorFieldAsset.hpp"
#include <Math/CurlNoise.hpp>
#include "Engine/Scene/Components/ParticleEmitter.hpp"
#include "Engine/Scene/Components/ParticleEmitterSpace.hpp"
#include "Engine/Scene/GameObject.hpp"
#include "Engine/Scene/Scene.hpp"
#include "Engine/Scene/Transform.hpp"
#include <algorithm>
#include <cmath>
#include <string>

namespace fbzz::scene {
namespace {

/// @note 乱流の式は Math/CurlNoise.hpp が正本。FlowFieldEval・VectorFieldAsset・
///       ParticleNoise.hlsli の 3 者で同じ値が出ることが前提なので、実体は 1 つに保つ。
using math::CurlNoise;
using math::TurbulenceSamplePoint;

/// @brief 速度場 PNG を解決する。
/// @note パス→ハンドルは AssetManager が持つので毎フレーム呼んでよい (中身はハッシュ 1 回)。
const fluid::VectorFieldAsset* ResolveVectorField(const std::string& path)
{
    if (path.empty()) return nullptr;
    const auto handle = asset::AssetManager::Load<fluid::VectorFieldAsset>(path);
    return handle.IsValid() ? asset::AssetManager::Get(handle) : nullptr;
}

/// @brief 距離減衰。radius <= 0 は «減衰なしで全体へ»。範囲外なら false。
bool ResolveInfluence(const ActiveFlowField& f, const math::Vector3& toPoint, float& influence)
{
    influence = 1.0f;
    if (f.radius <= 0.0f) return true;
    const float dist = toPoint.Length();
    if (dist >= f.radius) return false;
    influence = std::pow(1.0f - dist / f.radius, f.falloffPower);
    return true;
}

} // namespace

bool AffectsEmitter(const ActiveFlowField& field, uint32_t emitterChannels)
{
    return (field.channels & emitterChannels) != 0u;
}

bool FlowIntersectsSphere(const ActiveFlowField& field, const math::Vector3& center, float radius)
{
    if (!std::isfinite(radius) || radius < 0.0f) return true;
    const auto delta = center - field.position;
    if (field.type == FlowFieldType::Baked) {
        const auto local = field.inverseRotation * delta;
        const math::Vector3 outside = {
            (std::max)(std::abs(local.x) - field.extents.x, 0.0f),
            (std::max)(std::abs(local.y) - field.extents.y, 0.0f),
            (std::max)(std::abs(local.z) - field.extents.z, 0.0f) };
        const float paddedRadius = radius + 1.0e-4f;
        return outside.LengthSq() <= paddedRadius * paddedRadius;
    }
    if (field.radius <= 0.0f) return true;
    const float reach = field.radius + radius + 1.0e-4f;
    return delta.LengthSq() <= reach * reach;
}

float FlowSpeedBound(const ActiveFlowField& field)
{
    if (field.type == FlowFieldType::LegacyDrag) return 0.0f;
    /// @note Curl の各差分は [-4,4]、差は [-8,8]。8*sqrt(3) を外側へ丸める。
    if (field.type == FlowFieldType::Curl) return std::abs(field.strength) * 14.0f;
    if (field.type == FlowFieldType::Baked)
        return field.vectorField ? std::abs(field.strength) * field.vectorField->maxMagnitude * 1.733f : 0.0f;
    return std::abs(field.strength);
}

/// @note 共通化するのは、シーンに置いた場とエミッター内蔵の流れで falloff の下限や
///       方向の正規化がずれると «同じ設定なのに置き方で効き方が違う» になるため。
ActiveFlowField ResolveFlowField(const FlowFieldSettings& settings,
                                 const math::Vector3&    origin,
                                 const math::Quaternion& rotation)
{
    ActiveFlowField f;
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
    if (settings.fieldType == FlowFieldType::Baked) {
        /// @note 焼いた場の効く範囲は extents (AABB の貼り先) が決める。radius まで見ると
        ///       球で二重にクリップされる。Inspector は Baked のとき Radius を出さないので、
        ///       型を切り替えて残った値が «見えないのに効く» ことのないよう無効化する。
        f.radius          = 0.0f;
        f.vectorField     = ResolveVectorField(settings.vectorFieldPath);
        f.inverseRotation = rotation.Conjugate();
        f.extents         = {
            (std::max)(std::abs(settings.vectorFieldExtents.x), 1.0e-3f),
            (std::max)(std::abs(settings.vectorFieldExtents.y), 1.0e-3f),
            (std::max)(std::abs(settings.vectorFieldExtents.z), 1.0e-3f) };
    }
    return f;
}

void GatherFlowFields(Scene& scene, std::vector<ActiveFlowField>& out)
{
    out.clear();
    for (auto& go : scene.GameObjects()) {
        if (!go.activeInHierarchy()) continue;
        auto* ff = go.GetComponent<FlowField>();
        if (ff == nullptr) continue;
        for (const FlowFieldSettings& settings : ff->forces) {
            if (!settings.enabled) continue;
            /// @note direction はローカル指定。GameObject を回せば流向・渦軸も回る。
            out.push_back(ResolveFlowField(settings, go.transform.worldPosition,
                                           go.transform.worldRotation));
        }
    }
}

/// @brief 契約 (何を返すか・GPU との式の一致) は FlowFieldEval.hpp を参照。
math::Vector3 SampleFlow(const math::Vector3& position,
                         const std::vector<ActiveFlowField>& fields,
                         uint32_t channels, float time, bool* covered)
{
    math::Vector3 flow = math::Vector3::ZERO;
    if (covered != nullptr) *covered = false;
    for (const auto& f : fields) {
        if (!AffectsEmitter(f, channels)) continue;
        const math::Vector3 toPoint = position - f.position;
        float influence = 1.0f;
        if (!ResolveInfluence(f, toPoint, influence)) continue;
        /// @note 半径の内側に居れば «媒質の話がある» 点。流速が 0 でも覆いは立てる。
        if (covered != nullptr && f.type != FlowFieldType::LegacyDrag) *covered = true;
        const float scale = f.strength * influence;
        switch (f.type) {
        case FlowFieldType::Uniform:
            flow = flow + f.direction * scale;
            break;
        case FlowFieldType::Sink:
        case FlowFieldType::Source: {
            const float dist = (std::max)(toPoint.Length(), 1.0e-4f);
            const math::Vector3 radial = toPoint * (1.0f / dist);
            flow = flow + radial * (f.type == FlowFieldType::Source ? scale : -scale);
            break;
        }
        case FlowFieldType::Vortex: {
            /// @note 軸×点方向の外積 = 接線方向。軸周りに回す。
            const math::Vector3 tangent = math::Vector3::Cross(f.direction, toPoint);
            const float len = tangent.Length();
            if (len > 1.0e-4f) flow = flow + tangent * (scale / len);
            break;
        }
        case FlowFieldType::Curl:
            flow = flow + CurlNoise(TurbulenceSamplePoint(
                position, f.noiseFrequency, f.noiseSpeed, time)) * scale;
            break;
        case FlowFieldType::LegacyDrag:
            /// @note 流れではない。読み込み時に ParticleEmitter::flowCoupling へ写るので、
            ///       ここまで届くのは «旧ファイルを移行し損ねた» ときだけ。何も足さない。
            break;
        case FlowFieldType::Baked: {
            if (f.vectorField == nullptr || f.vectorField->Empty()) break;
            /// @note 場のローカルへ: 平行移動 → 逆回転 → 指定した寸法で正規化。
            ///       式は ParticleGpuSim.cs.hlsl の FF_BAKED 分岐と一致させること。
            ///
            ///       @note アセット側の bounds をそのまま使わないのは、同じ 1 枚を «部屋いっぱいの渦» と
            ///       «手のひらの渦» に貼り分けられるようにするため。
            const math::Vector3 localOffset = f.inverseRotation * toPoint;
            const math::Vector3 uvw = {
                localOffset.x / f.extents.x * 0.5f + 0.5f,
                localOffset.y / f.extents.y * 0.5f + 0.5f,
                localOffset.z / f.extents.z * 0.5f + 0.5f };
            if (uvw.x < 0.0f || uvw.x > 1.0f || uvw.y < 0.0f || uvw.y > 1.0f
                || uvw.z < 0.0f || uvw.z > 1.0f) break;

            /// @note uvw [0,1] をアセットの bounds へ写して引く。
            const math::Vector3& assetMin = f.vectorField->boundsMin;
            const math::Vector3& assetMax = f.vectorField->boundsMax;
            const math::Vector3 samplePoint = {
                assetMin.x + (assetMax.x - assetMin.x) * uvw.x,
                assetMin.y + (assetMax.y - assetMin.y) * uvw.y,
                assetMin.z + (assetMax.z - assetMin.z) * uvw.z };
            const math::Vector3 fieldValue = f.vectorField->SampleLocal(samplePoint);
            /// @note 場は «場のローカル» で焼かれているのでワールドへ戻す。
            ///       @note strength は Baked だけ無次元の倍率。焼いた値が既に [m/s] のため。
            flow = flow + (f.inverseRotation.Conjugate() * fieldValue) * scale;
            break;
        }
        }
    }
    return flow;
}

/// @brief 契約 (緩和の式・coupling が何か) は FlowFieldEval.hpp を参照。
void ApplyFlowFields(const std::vector<ActiveFlowField>& fields,
                     uint32_t             emitterChannels,
                     const math::Vector3& position,
                     math::Vector3&       velocity,
                     float dt, float time, float coupling)
{
    if (fields.empty() || coupling <= 0.0f) return;
    bool covered = false;
    const math::Vector3 flow = SampleFlow(position, fields, emitterChannels, time, &covered);
    if (!covered) return;
    /// @note 1 を超えると «行き過ぎて逆に振れる» 発散になる。陽解法の 1 ステップぶんで
    ///       追い越さないところで止める (HLSL 側は saturate)。
    const float blend = std::clamp(coupling * dt, 0.0f, 1.0f);
    velocity = velocity + (flow - velocity) * blend;
}

/// @brief このエミッターに効く流れを 1 本のリストへまとめる。すべて **ワールド空間** で解決する。
///
/// @note ワールドに揃えるのは、力場・乱流がもともとワールドで解決され、内蔵の重力と周回だけが
///       «シミュレーション空間» で適用されていたため。混在していると simulationSpace = Local の
///       エミッターを傾けたとき、重力だけが一緒に傾いて横へ落ちる。
void ResolveEmitterForces(const ParticleEmitter& emitter, const Transform& tf,
                          const std::vector<ActiveFlowField>& sceneFields,
                          std::vector<ActiveFlowField>& outFields, bool useGpuBounds)
{
    outFields.clear();

    const math::Vector3 emitterOrigin = TransformEmitterPoint(tf, emitter.settings.emitPosition);

    for (const FlowFieldSettings& force : emitter.settings.localForces) {
        if (!force.enabled) continue;
        /// @note Emitter 空間の流れ (周回・放射) だけがエミッターの位置と回転に追従する。
        ///       乱流まで追従させると、エミッターを傾けただけで «どちらへ流れるか» が変わる。
        const bool emitterSpace = force.space == FlowFieldSpace::Emitter;
        ActiveFlowField resolved = ResolveFlowField(
            force,
            emitterSpace ? emitterOrigin : math::Vector3::ZERO,
            emitterSpace ? tf.worldRotation : math::Quaternion{});
        /// @note 内蔵の流れは相手が既にこのエミッター 1 体に決まっている。channels を持ち込むと
        ///       «自分の流れが自分のマスクに弾かれる» という説明のつかない挙動になる。
        resolved.channels = 0xFFFFFFFFu;
        outFields.push_back(resolved);
    }

    if (!emitter.settings.receiveFlowFields) return;
    math::Vector3 boundsCenter = emitter.runtime.flowBoundsCenter;
    float boundsRadius = emitter.runtime.flowBoundsRadius;
    if (!useGpuBounds) {
        /// @note スポーン後・移動前の全粒子を含める。Flow は移動前の位置で一度だけ標本化する。
        boundsCenter = emitterOrigin;
        boundsRadius = 0.0f;
        for (const auto& particle : emitter.runtime.particles) {
            const auto position = emitter.settings.simulationSpace == ParticleSimulationSpace::Local
                ? TransformEmitterPoint(tf, particle.position) : particle.position;
            boundsRadius = (std::max)(boundsRadius, (position - boundsCenter).Length());
        }
    }
    bool needsZeroFlow = false;
    for (const ActiveFlowField& field : sceneFields) {
        if (!AffectsEmitter(field, emitter.settings.flowFieldChannels)) continue;
        if (!FlowIntersectsSphere(field, boundsCenter, boundsRadius)) {
            needsZeroFlow |= field.type == FlowFieldType::Baked;
            continue;
        }
        outFields.push_back(field);
    }
    if (needsZeroFlow) {
        /// @note Baked は箱の外でも covered を立てる既存契約。省略した場の抵抗をゼロ流速1本で保つ。
        ActiveFlowField zero{};
        zero.type = FlowFieldType::Uniform;
        zero.channels = emitter.settings.flowFieldChannels;
        outFields.push_back(zero);
    }
}

} // namespace fbzz::scene
