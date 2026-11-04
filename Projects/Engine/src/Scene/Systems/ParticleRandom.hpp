/// @file    ParticleRandom.hpp
/// @brief   CPU・GPUスポーンと再生処理が共有する決定論的な粒子乱数。
/// @author  Hasegawa Jin
/// @date    2026-10-31
#pragma once
#include <Engine/Scene/Components/ParticleEmitter.hpp>
#include <algorithm>
namespace fbzz::scene::particle_runtime_detail {
inline uint32_t NextParticleRandom(ParticleEmitter& emitter)
{
    /// @note Numerical Recipes 系 LCG。軽量で、エミッターごとの seed から決定的な乱数列を作る。
    /// @note `std::rand()` はグローバル状態のため、複数エミッターや再生順序で結果が変わりやすい。
    if (emitter.runtime.randomState == 0) {
        emitter.runtime.randomState = emitter.settings.randomSeed != 0 ? emitter.settings.randomSeed : 1;
    }
    emitter.runtime.randomState = emitter.runtime.randomState * 1664525u + 1013904223u;
    return emitter.runtime.randomState;
}

inline float RandomSigned01(ParticleEmitter& emitter)
{
    constexpr float INV_MAX_UINT = 1.0f / 4294967295.0f;
    return static_cast<float>(NextParticleRandom(emitter)) * INV_MAX_UINT * 2.0f - 1.0f;
}

inline float Random01(ParticleEmitter& emitter)
{
    return (std::max)(0.0f, (std::min)(RandomSigned01(emitter) * 0.5f + 0.5f, 1.0f));
}

} /// @note namespace fbzz::scene::particle_runtime_detail
