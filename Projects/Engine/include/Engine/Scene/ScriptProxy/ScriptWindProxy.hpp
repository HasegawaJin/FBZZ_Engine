// FBZZ Engine
// ScriptWindProxy.hpp | fbzz::scene
// Script から WindZoneComponent を操作するプロキシ
#pragma once

#include <Math/Vector3.hpp>

namespace fbzz::scene {

class Script;

struct ScriptWindProxy {
    Script* script = nullptr;

    void SetEnabled(bool enabled) const;
    void SetDirection(const math::Vector3& direction) const;
    void SetStrength(float strength) const;
    void SetTurbulence(float turbulence) const;
    void SetPulseFrequency(float frequency) const;
};

} // namespace fbzz::scene
