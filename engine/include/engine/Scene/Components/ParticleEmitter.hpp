// FBZZ Engine
// ParticleEmitter.hpp | fbzz::scene
// CPU パーティクルシミュレーション用コンポーネント
#pragma once
#include <vector>
#include <math/Vector3.hpp>
#include <math/Vector4.hpp>

namespace fbzz::scene {

struct Particle {
    math::Vector3 position;
    math::Vector3 velocity;
    math::Vector4 color;
    float         size;
    float         age;
};

struct ParticleEmitter {
    math::Vector3 emitPosition   = {};
    math::Vector3 emitVelocity   = { 0.0f, 4.0f, 0.0f };
    float         velocitySpread = 1.5f;
    math::Vector4 colorStart     = { 1.0f, 0.7f, 0.2f, 1.0f };
    math::Vector4 colorEnd       = { 1.0f, 0.1f, 0.0f, 0.0f };
    float         sizeStart      = 0.4f;
    float         sizeEnd        = 0.05f;
    float         lifetime       = 2.0f;
    float         emitRate       = 30.0f;
    int           maxParticles   = 300;
    bool          enabled        = true;

    std::vector<Particle> particles;
    float                 emitAccum = 0.0f;
};

} // namespace fbzz::scene
