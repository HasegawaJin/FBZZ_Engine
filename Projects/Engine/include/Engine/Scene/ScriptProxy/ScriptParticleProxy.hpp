// FBZZ Engine
// ScriptParticleProxy.hpp | fbzz::scene
// Script から ParticleEmitter を操作するショートハンド
#pragma once

namespace fbzz::scene {

class Script;

struct ScriptParticleProxy {
    Script* script = nullptr;

    void SetEmitRate(float rate) const;
    void SetEnabled(bool enabled) const;
    void Clear() const;
};

} // namespace fbzz::scene
