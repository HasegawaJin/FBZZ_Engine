// FBZZ Engine
// ScriptParticleProxy.hpp | fbzz::scene
// Script から ParticleEmitter を操作するショートハンド
#pragma once
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
#include <cstdint>
#include <string_view>

namespace fbzz::scene {

class Script;

enum class ParticleEmitterShape : uint8_t {
    Point  = 0,
    Sphere = 1,
    Cone   = 2,
    Box    = 3,
};

enum class ParticleBlendMode : uint8_t {
    Additive = 0,
    Alpha    = 1,
};

enum class ParticleSortMode : uint8_t {
    None        = 0,
    BackToFront = 1,
};

enum class ParticleSimulationMode : uint8_t {
    Cpu = 0,
    Gpu = 1,
};

struct ScriptParticleProxy {
    Script* script = nullptr;

    void SetEmitRate(float rate) const;
    void SetEnabled(bool enabled) const;
    void Play(bool restart = true) const;
    void Stop(bool clear = false) const;
    void Burst(int count) const;
    void Clear() const;
    void SetGravity(const math::Vector3& gravity) const;
    void SetColor(const math::Vector4& start, const math::Vector4& end) const;
    void SetSize(float start, float end) const;
    void SetTexture(std::string_view texturePath, int columns = 1, int rows = 1) const;
    void SetShape(ParticleEmitterShape shape) const;
    void SetBlendMode(ParticleBlendMode blendMode) const;
    void SetSortMode(ParticleSortMode sortMode) const;
    void SetSimulationMode(ParticleSimulationMode simulationMode) const;
};

} // namespace fbzz::scene
