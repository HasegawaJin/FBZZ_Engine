/// @file    ParticleEmitterAssetCodec.hpp
/// @brief   ParticleEmitterのauthoring設定をTOMLへ変換する共通codec。
/// @author  Hasegawa Jin
/// @date    2026-07-20
#pragma once

#include <toml++/toml.hpp>

namespace fbzz::scene {
struct ParticleEmitterSettings;
struct ParticleCurve;
struct ParticleGradient;
}

namespace fbzz::asset {

// ParticleCurve / ParticleGradient の TOML 表現。
// WHY: Emitter だけでなく VFX ノード (Light の減衰カーブ、Decal のフェードカーブ) も
//      同じ型を保存するようになったため、変換をここ 1 か所に集約する。
//      形式が二系統に分かれると、キー数やクランプの扱いが片方だけずれる。
//      書き出し・読み込みとも { interp = <int>, keys = [...] } のテーブル形式に統一する。
[[nodiscard]] toml::table SerializeParticleCurve(const scene::ParticleCurve& curve);
// キーが無い/壊れている場合は outCurve を変更しない。
void DeserializeParticleCurve(const toml::table& table, const char* key,
                              scene::ParticleCurve& outCurve);

[[nodiscard]] toml::table SerializeParticleGradient(const scene::ParticleGradient& gradient);
void DeserializeParticleGradient(const toml::table& table, const char* key,
                                 scene::ParticleGradient& outGradient);

// Sceneと.vfxの双方が同じ設定schemaを使えるよう、ランタイム状態を除いて書き出す。
[[nodiscard]] toml::table SerializeParticleEmitterSettings(const scene::ParticleEmitterSettings& emitter);

// 未知フィールドを無視し、欠落フィールドは outEmitter の初期値を維持する。
// NOTE: ランタイム状態には触れない (この型が持っていない)。コンポーネントへ流し込んだ場合は、
//       呼び出し側が ParticleEmitter::ResetPlayback() を呼んで再生状態を初期化すること。
void DeserializeParticleEmitterSettings(const toml::table& table,
                                        scene::ParticleEmitterSettings& outEmitter);

} // namespace fbzz::asset
