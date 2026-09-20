/// @file    ParticleEmitterAssetCodec.hpp
/// @brief   ParticleEmitterのauthoring設定をTOMLへ変換する共通codec。
/// @author  Hasegawa Jin
/// @date    2026-07-20
#pragma once

#include <toml++/toml.hpp>

namespace fbzz::renderer { struct ParticleCurve; struct ParticleGradient; }
namespace fbzz::scene {
struct ParticleEmitterSettings;
using renderer::ParticleCurve;
using renderer::ParticleGradient;
}

namespace fbzz::asset {

/// @note ParticleCurve / ParticleGradient の TOML 表現。
/// @note Emitter だけでなく VFX ノード (Light の減衰カーブ、Decal のフェードカーブ) も同じ型を
/// @note 保存するため変換をここへ集約する (二系統に分かれるとキー数・クランプの扱いがずれる)。
/// @note 書き出し・読み込みとも `{ interp = <int>, keys = [...] }` のテーブル形式に統一する。
[[nodiscard]] toml::table SerializeParticleCurve(const scene::ParticleCurve& curve);
/// @note キーが無い/壊れている場合は outCurve を変更しない。
void DeserializeParticleCurve(const toml::table& table, const char* key,
                              scene::ParticleCurve& outCurve);

[[nodiscard]] toml::table SerializeParticleGradient(const scene::ParticleGradient& gradient);
void DeserializeParticleGradient(const toml::table& table, const char* key,
                                 scene::ParticleGradient& outGradient);

/// @note Sceneと.vfxの双方が同じ設定schemaを使えるよう、ランタイム状態を除いて書き出す。
[[nodiscard]] toml::table SerializeParticleEmitterSettings(const scene::ParticleEmitterSettings& emitter);

/// @note 未知フィールドを無視し、欠落フィールドは outEmitter の初期値を維持する。
/// @note ランタイム状態には触れない (この型が持っていない)。コンポーネントへ流し込んだ場合は、
/// @note 呼び出し側が ParticleEmitter::ResetPlayback() を呼んで再生状態を初期化すること。
void DeserializeParticleEmitterSettings(const toml::table& table,
                                        scene::ParticleEmitterSettings& outEmitter);

} /// @note namespace fbzz::asset
