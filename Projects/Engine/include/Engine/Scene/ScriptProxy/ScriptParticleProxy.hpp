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
    // 事前乗算アルファ。発光する芯と背景を隠す煙を 1 枚のテクスチャで両立できる。
    Premultiplied = 2,
};

enum class ParticleSortMode : uint8_t {
    None        = 0,
    BackToFront = 1,
};

// グラデーションのキー間をどの色空間で混ぜるか。
// WHY: どの空間で混ぜるかは「正解が 1 つに決まらない」種類の選択で、用途で使い分かれる。
//      Gamma はカラーピッカー上の見た目どおりに繋がるが、白熱 → 橙 → 暗赤のような
//      彩度の高いランプでは中間が濁る。Linear は光として正しく足し合わさる代わりに
//      中間が明るく寄る。Oklab は明度と色相が知覚的に等間隔で動くため、
//      魔法エフェクトのような色相を大きく回すランプで破綻しない。
// 値は Assets/Shaders/Rendering/ParticleCommon.hlsli の FBZZ_PGRAD_* と一致させること。
enum class ParticleColorSpace : uint8_t {
    Gamma  = 0, // オーサリング値をそのまま線形補間 (従来の挙動)
    Linear = 1, // リニア空間で補間
    Oklab  = 2, // 知覚的に等間隔な OkLab で補間
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

// テクスチャからアルファをどう取り出すか。
// WHY: パーティクル素材は RGBA でアルファを持つものばかりではない。加算合成前提の
//      素材は黒背景の RGB だけでアルファが無い (または全面 1) ことが多く、
//      そのままアルファブレンドすると黒い矩形が出る。逆に印刷用途由来の素材は
//      白背景のことがある。素材を加工させずエミッター側で吸収する。
// 値は Rendering/Mask.hlsli の FBZZ_MASK_* と一致させること。
// パーティクル専用の番号体系を作らず、全マテリアル共通のチャンネル語彙を使う。
enum class ParticleAlphaSource : uint8_t {
    TextureAlpha      = 0, // 通常 (アルファ付き素材)
    Luminance         = 1, // 黒 = 透明 (加算用の黒背景素材)
    LuminanceInverted = 2, // 白 = 透明 (白背景素材)
    Red               = 3, // R チャンネル (パック済みマスクの1枚目)
    Green             = 4, // G チャンネル
    Blue              = 5, // B チャンネル
    AlphaInverted     = 6, // アルファ反転
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
