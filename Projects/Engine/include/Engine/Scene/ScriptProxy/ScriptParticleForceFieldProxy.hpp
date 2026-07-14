// FBZZ Engine
// ScriptParticleForceFieldProxy.hpp | fbzz::scene
// Script から ParticleForceField を安全に操作するプロキシ
#pragma once

#include <Math/Vector3.hpp>
#include <cstdint>

namespace fbzz::scene {

class Script;

enum class ScriptParticleForceFieldType : uint8_t {
    WIND = 0,
    ATTRACT,
    REPULSE,
    VORTEX,
    TURBULENCE,
    DRAG
};

struct ScriptParticleForceFieldProxy {
    Script* script = nullptr;

    void SetEnabled(bool enabled) const;
    void SetType(ScriptParticleForceFieldType type) const;
    void SetStrength(float strength) const;
    void SetRadius(float radius, float falloffPower = 2.0f) const;
    void SetDirection(const math::Vector3& direction) const;
    void SetTurbulence(float frequency, float speed) const;
};

} // namespace fbzz::scene
