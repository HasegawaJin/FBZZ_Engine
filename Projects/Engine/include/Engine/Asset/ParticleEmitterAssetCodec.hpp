// FBZZ Engine
// ParticleEmitterAssetCodec.hpp | fbzz::asset
// ParticleEmitterのauthoring設定をTOMLへ変換する共通codec
#pragma once

#include <toml++/toml.hpp>

namespace fbzz::scene {
struct ParticleEmitter;
struct ParticleCurve;
struct ParticleGradient;
}

namespace fbzz::asset {

// ParticleCurve / ParticleGradient の TOML 表現。
// WHY: Emitter だけでなく VFX ノード (Light の減衰カーブ、Decal のフェードカーブ) も
//      同じ型を保存するようになったため、変換をここ 1 か所に集約する。
//      形式が二系統に分かれると、キー数やクランプの扱いが片方だけずれる。
//      書き出しは { interp = <int>, keys = [...] } のテーブル形式。読み込みは旧来の
//      配列形式 (補間モードが無かった頃のアセット) も受け付ける。
[[nodiscard]] toml::table SerializeParticleCurve(const scene::ParticleCurve& curve);
// キーが無い/壊れている場合は outCurve を変更しない (既定値を保つ後方互換)。
void DeserializeParticleCurve(const toml::table& table, const char* key,
                              scene::ParticleCurve& outCurve);

[[nodiscard]] toml::table SerializeParticleGradient(const scene::ParticleGradient& gradient);
void DeserializeParticleGradient(const toml::table& table, const char* key,
                                 scene::ParticleGradient& outGradient);

// Sceneと.vfxの双方が同じ設定schemaを使えるよう、ランタイム状態を除いて書き出す。
[[nodiscard]] toml::table SerializeParticleEmitterSettings(const scene::ParticleEmitter& emitter);

// 未知フィールドを無視し、欠落フィールドはoutEmitterの初期値を維持して後方互換を保つ。
void DeserializeParticleEmitterSettings(const toml::table& table,
                                        scene::ParticleEmitter& outEmitter);

} // namespace fbzz::asset
