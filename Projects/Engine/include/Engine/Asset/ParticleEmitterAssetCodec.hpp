// FBZZ Engine
// ParticleEmitterAssetCodec.hpp | fbzz::asset
// ParticleEmitterのauthoring設定をTOMLへ変換する共通codec
#pragma once

#include <toml++/toml.hpp>

namespace fbzz::scene { struct ParticleEmitter; }

namespace fbzz::asset {

// Sceneと.vfxの双方が同じ設定schemaを使えるよう、ランタイム状態を除いて書き出す。
[[nodiscard]] toml::table SerializeParticleEmitterSettings(const scene::ParticleEmitter& emitter);

// 未知フィールドを無視し、欠落フィールドはoutEmitterの初期値を維持して後方互換を保つ。
void DeserializeParticleEmitterSettings(const toml::table& table,
                                        scene::ParticleEmitter& outEmitter);

} // namespace fbzz::asset
