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
    MeshSurface = 4,
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

enum class ParticleSimulationSpace : uint8_t {
    World = 0,
    Local = 1,
};

enum class ParticleRenderMode : uint8_t {
    Billboard = 0,
    StretchedBillboard,
    HorizontalBillboard,
    VerticalBillboard,
};

enum class ParticleCollisionMode : uint8_t {
    None = 0,
    Physics,
    Plane,
    Depth,
};

enum class ParticleCollisionResponse : uint8_t {
    Bounce = 0,
    Kill,
    Stop,
};

enum class ParticleFlipbookMode : uint8_t {
    Lifetime = 0,
    FramesPerSecond,
    RandomFrame,
    PingPong,
};

struct ScriptParticleProxy {
    Script* script = nullptr;

    void SetEmitRate(float rate) const;
    void SetEmitPosition(const math::Vector3& position) const;
    void SetEmitVelocity(const math::Vector3& velocity) const;
    void SetVelocitySpread(float spread) const;
    void SetEnabled(bool enabled) const;
    void Play(bool restart = true) const;
    void Stop(bool clear = false) const;
    void Burst(int count) const;
    void Clear() const;
    void SetGravity(const math::Vector3& gravity) const;
    void SetColor(const math::Vector4& start, const math::Vector4& end) const;
    void SetSize(float start, float end) const;
    void SetLifetime(float seconds) const;
    void SetMaxParticles(int maxParticles) const;
    void SetPlayback(bool loop, float duration, bool clearOnStop = false) const;
    void SetTexture(std::string_view texturePath, int columns = 1, int rows = 1) const;
    void SetShape(ParticleEmitterShape shape) const;
    void SetSphereShape(float radius) const;
    void SetConeShape(float radius, float angleDegrees) const;
    void SetBoxShape(const math::Vector3& extents) const;
    // FBX / Modelの頂点群からParticle形状を生成する。followSkinnedAnimation=trueなら同じGOのAnimator姿勢に追従する。
    void SetMeshShape(std::string_view modelPath, int meshIndex = -1, float scale = 1.0f,
                      bool followSkinnedAnimation = false) const;
    void SetBlendMode(ParticleBlendMode blendMode) const;
    void SetSortMode(ParticleSortMode sortMode) const;
    void SetSimulationMode(ParticleSimulationMode simulationMode) const;
    void SetSimulationSpace(ParticleSimulationSpace space) const;
    void SetRenderMode(ParticleRenderMode mode, float stretchScale = 1.0f) const;
    void SetCollision(ParticleCollisionMode mode, ParticleCollisionResponse response,
                      float radius = 0.05f, float bounciness = 0.5f) const;
    [[nodiscard]] int GetCollisionCount() const;
    void SetRateOverDistance(float particlesPerMeter) const;
    void SetPrewarm(bool enabled) const;
    void SetSoftParticles(bool enabled, float fadeDistance = 0.5f) const;
    void SetFlipbookMode(ParticleFlipbookMode mode, float framesPerSecond = 24.0f) const;
    void SetSubEmitters(std::string_view birthEmitter, std::string_view deathEmitter,
                        std::string_view collisionEmitter, int burstCount = 1) const;
    void SetVelocityDamping(float damping) const;
    void SetAngularVelocity(float minValue, float maxValue) const;
    // ノイズモジュール (カールノイズ乱流)。strength 0 で無効。
    void SetNoise(float strength, float frequency = 0.5f, float speed = 1.0f) const;
    // シーン内の ParticleForceField から力を受けるか
    void SetReceiveForceFields(bool receive) const;
};

} // namespace fbzz::scene
