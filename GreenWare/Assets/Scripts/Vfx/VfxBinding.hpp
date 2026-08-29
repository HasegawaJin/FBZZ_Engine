/// @file    VfxBinding.hpp
/// @brief   .vfx プレハブの子へ値を差し込むための小さなヘルパー群
/// @author  Hasegawa Jin
/// @date    2026-08-26
///
/// WHY 旧 VFXParamBinding を置き換えるか:
///   旧実装は `schemaPath = "particle.colorStart"` という文字列でノードのフィールドを
///   指していた。型は実行時にしか合わず、綴りを間違えても保存も検証も通るので、
///   «光の強さを変えたつもりでパーティクルの色へ書いていた» が黙って成立していた。
///   ここでは値の行き先を C++ の関数として書く。渡す型が合わなければコンパイルが止まる。
///
/// WHY 名前で子を引くか:
///   .vfx はプレハブなので、子は «そのエフェクトの層» そのもので、名前が層の名前になる
///   ("Blast Light" / "Sparks")。id で引くとエディタで層を並べ替えたときに壊れ、
///   参照で持つとプレハブのインスタンス化ごとに張り直しが要る。名前が一番壊れにくい。
///
/// WHY 再帰で探すか:
///   変換器は旧 Canvas の group を «層» の空 GameObject として残すため、
///   目的の子は必ずしも直下に居ない (Sparks は "Debris (無彩色)" の中)。
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

/// 層そのものの位置。旧 schemaPath "localPosition"。
inline void NodePosition(GameObject& root, std::string_view node, const Vector3& value)
{
    if (GameObject* target = Find(root, node)) target->transform.position = value;
}

/// 光の色と強さ。
///
/// WHY エンベロープの基準値も一緒に書くか:
///   VFXLightEnvelope は «Inspector に置かれた値をピークとして掴み、毎フレーム
///   カーブを掛け直す» 作りになっている (baseIntensity)。掴んだ後に
///   LightComponent 側だけ書き換えても、次のフレームで基準値から作り直されて消える。
///   1 発ごとに強さを変えるには基準値の方を書く必要がある。
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
        if (envelope != nullptr) envelope->baseColor = *color;
    }
    if (intensity != nullptr) {
        light->intensity = *intensity;
        if (envelope != nullptr) envelope->baseIntensity = *intensity;
    }
    // 以降 «掴み直し» をさせない。掴み直されると、カーブを掛けた後の暗い値を
    // 新しいピークとして拾ってしまい、撃つたびに暗くなる。
    if (envelope != nullptr) envelope->captured = true;
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
