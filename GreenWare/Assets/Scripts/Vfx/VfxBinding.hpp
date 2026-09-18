/// @file    VfxBinding.hpp
/// @brief   .vfx プレハブの子へ値を差し込むための小さなヘルパー群
/// @author  Hasegawa Jin
/// @date    2026-08-26
///
/// @note 旧 VFXParamBinding は schemaPath 文字列でフィールドを指し、型は実行時にしか合わず綴り
///       ミスも検証を通っていた。ここでは値の行き先を C++ 関数として書き、型不一致はコンパイルで止める。
/// @note 子は名前で引く。.vfx の子は層そのもの (name が層名) で、id は並べ替えで壊れ参照は
///       インスタンス化ごとに張り直しが要る。変換器が残す空の group GameObject があるため再帰で探す。
#pragma once

#include <Engine/Scene/Components/DecalComponent.hpp>
#include <Engine/Scene/Components/LightComponent.hpp>
#include <Engine/Scene/Components/ParticleEmitter.hpp>
#include <Engine/Scene/Components/VFXElement.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>

#include <string>
#include <string_view>

using namespace fbzz::scene;
using namespace fbzz::math;

namespace sandbox::vfxbind {

/// 配下から名前で GameObject を引く (自身は含まない)。見つからなければ nullptr。
[[nodiscard]] inline GameObject* Find(GameObject& root, std::string_view name)
{
    const int count = root.GetChildCount();
    for (int index = 0; index < count; ++index) {
        GameObject* child = root.GetChild(index);
        if (child == nullptr) continue;
        if (child->name == name) return child;
        if (GameObject* found = Find(*child, name)) return found;
    }
    return nullptr;
}

/// 粒の «出はじめの色»。旧 schemaPath "particle.colorStart"。
inline void ParticleColor(GameObject& root, std::string_view node, const Vector4& color)
{
    if (GameObject* target = Find(root, node))
        if (auto* emitter = target->GetComponent<ParticleEmitter>())
            emitter->settings.colorStart = color;
}

inline void ParticleMaterial(GameObject& root, std::string_view node, const std::string& path)
{
    if (GameObject* target = Find(root, node))
        if (auto* emitter = target->GetComponent<ParticleEmitter>())
            emitter->settings.materialPath = path;
}

inline void ParticleSizeStart(GameObject& root, std::string_view node, float value)
{
    if (GameObject* target = Find(root, node))
        if (auto* emitter = target->GetComponent<ParticleEmitter>())
            emitter->settings.sizeStart = value;
}

inline void ParticleSizeEnd(GameObject& root, std::string_view node, float value)
{
    if (GameObject* target = Find(root, node))
        if (auto* emitter = target->GetComponent<ParticleEmitter>())
            emitter->settings.sizeEnd = value;
}

inline void ParticleSphereRadius(GameObject& root, std::string_view node, float value)
{
    if (GameObject* target = Find(root, node))
        if (auto* emitter = target->GetComponent<ParticleEmitter>())
            emitter->settings.sphereRadius = value;
}

inline void ParticleVelocitySpread(GameObject& root, std::string_view node, float value)
{
    if (GameObject* target = Find(root, node))
        if (auto* emitter = target->GetComponent<ParticleEmitter>())
            emitter->settings.velocitySpread = value;
}

inline void ParticleEmitRate(GameObject& root, std::string_view node, float value)
{
    if (GameObject* target = Find(root, node))
        if (auto* emitter = target->GetComponent<ParticleEmitter>())
            emitter->settings.emitRate = value;
}

inline void ParticleEmitVelocity(GameObject& root, std::string_view node, const Vector3& value)
{
    if (GameObject* target = Find(root, node))
        if (auto* emitter = target->GetComponent<ParticleEmitter>())
            emitter->settings.emitVelocity = value;
}

/// 原点から外向きの加速度 [m/s²]。負で «吸い込み»。
/// @note emitVelocity は層のローカル軸へ固定の初速を与えるため «全方位へ同じだけ広がる» は書けず、
///       粒ごとに向きが違う放射には発生位置 (球・円錐) しか使えない。
inline void ParticleRadialVelocity(GameObject& root, std::string_view node, float value)
{
    if (GameObject* target = Find(root, node))
        if (auto* emitter = target->GetComponent<ParticleEmitter>())
            emitter->settings.EnsureLocalForce(FlowFieldType::Source).strength = value;
}

/// 層そのものの位置。旧 schemaPath "localPosition"。
inline void NodePosition(GameObject& root, std::string_view node, const Vector3& value)
{
    if (GameObject* target = Find(root, node)) target->transform.position = value;
}

/// 光の色と強さ。
/// @note VFXLightEnvelope は Inspector 値をピークとして掴み毎フレームカーブを掛け直すため
///       (`VFXCaptured<Base>`)、LightComponent だけ書き換えても次フレームで基準値から作り直される。
///       1 発ごとに強さを変えるには基準値も一緒に書く必要がある。
inline void Light(GameObject& root, std::string_view node,
                  const Vector3* color, const float* intensity)
{
    GameObject* target = Find(root, node);
    if (target == nullptr) return;
    auto* light = target->GetComponent<LightComponent>();
    if (light == nullptr) return;

    auto* envelope = target->GetComponent<VFXLightEnvelope>();
    if (color != nullptr) {
        light->color = *color;
        if (envelope != nullptr) envelope->base.value.color = *color;
    }
    if (intensity != nullptr) {
        light->intensity = *intensity;
        if (envelope != nullptr) envelope->base.value.intensity = *intensity;
    }
    /// @note 以降 «掴み直し» をさせない。掴み直されると、カーブを掛けた後の暗い値を
    ///       新しいピークとして拾ってしまい、撃つたびに暗くなる。
    if (envelope != nullptr) envelope->base.captured = true;
}

inline void LightColor(GameObject& root, std::string_view node, const Vector3& color)
{
    Light(root, node, &color, nullptr);
}

inline void LightIntensity(GameObject& root, std::string_view node, float intensity)
{
    Light(root, node, nullptr, &intensity);
}

/// デカールの絵柄。旧 schemaPath "decal.albedoPath"。
inline void DecalAlbedo(GameObject& root, std::string_view node, const std::string& path)
{
    if (GameObject* target = Find(root, node))
        if (auto* decal = target->GetComponent<DecalComponent>())
            decal->albedoTexPath = path;
}

} // namespace sandbox::vfxbind
