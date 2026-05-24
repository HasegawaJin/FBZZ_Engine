// FBZZ Engine
// ParticleEmitter.hpp | fbzz::scene
// CPU パーティクルシミュレーション設定コンポーネント
// 発生率・寿命・速度などを保持し、System が毎フレーム粒子状態を進める。
// 描画リソースの所有は Renderer 側に分ける。
#pragma once
#include <Engine/Scene/Script.hpp>
#include <vector>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>

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

    const char* GetTypeName() const { return "Particle Emitter"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.Field("emitPosition", emitPosition);
        r.Field("emitVelocity", emitVelocity);
        r.Field("velocitySpread", velocitySpread);
        r.Field("colorStart", colorStart);
        r.Field("colorEnd", colorEnd);
        r.Field("sizeStart", sizeStart);
        r.Field("sizeEnd", sizeEnd);
        r.Field("lifetime", lifetime);
        r.Field("emitRate", emitRate);
        r.Field("maxParticles", maxParticles);
    }
};

} // namespace fbzz::scene
