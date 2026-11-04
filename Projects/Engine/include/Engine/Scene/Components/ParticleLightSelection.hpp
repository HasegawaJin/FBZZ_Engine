/// @file    ParticleLightSelection.hpp
/// @brief   Lights モジュール — どの粒子を点光源にするかを決める純関数
/// @author  Hasegawa Jin
/// @date    2026-09-11
#pragma once
#include <Engine/Api.hpp>

#include <Engine/Scene/Components/ParticleEmitter.hpp>
#include <Math/Vector3.hpp>

#include <cstddef>
#include <vector>

namespace fbzz::scene {

/// @note 粒子 1 つが出す点光源。位置はシミュレーション空間 (Local ならエミッター基準)。
struct ParticleLightEmission {
    math::Vector3 position;
    math::Vector3 color;
    float range = 0.0f;
    float intensity = 0.0f;
};

/// @note 粒子が lightRatio の判定を通るか。粒子ごとの固定乱数 (spriteSeed) で決めるので、
/// @note 同じ粒子は寿命の間ずっと同じ判定になる (光ったり消えたりしない)。
[[nodiscard]] FBZZ_ENGINE_API bool ParticlePassesLightRatio(float spriteSeed, float ratio);

/// @note light に従って光らせる粒子を選び、明るい順に out へ積む (out は先に空にする)。
/// @note 選ばれるのは lightRatio を通った粒子のうち、明るい上位 min(lightMaxCount, budget) 個。
/// @note 明るさが同じなら配列の前にある粒子を先にする (決定論的)。
/// @note ライト配列の枠 (kMaxPunctualLights) は LightComponent と共有で、溢れた分は捨てる
/// @note しかない。捨てるなら画面への寄与が小さいものから。
/// @param budget ライト配列の残り枠
FBZZ_ENGINE_API void SelectParticleLights(const ParticleLightSettings& light, const std::vector<Particle>& particles,
                          std::size_t budget, std::vector<ParticleLightEmission>& out);

} /// @note namespace fbzz::scene
