/// @file    ScriptParticleProxy.hpp
/// @brief   Script から ParticleEmitter を操作するショートハンド。
/// @author  Hasegawa Jin
/// @date    2026-06-01
#pragma once
#include <Engine/Api.hpp>
#include <Engine/Scene/Components/ParticleTypes.hpp>
/// @note ParticleColorSpace は別ヘッダーに置く。ParticleGradient (Engine/Scene/ParticleCurve.hpp)
/// @note がメンバーに持つため、Script.hpp のプロキシ群より先に読まれる必要がある。
#include <Engine/Scene/Components/ParticleColorSpace.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
#include <cstdint>
#include <string_view>

namespace fbzz::scene {

class Script;

struct ScriptParticleProxy {
    Script* script = nullptr;

    FBZZ_ENGINE_API void SetEmitRate(float rate) const;
    FBZZ_ENGINE_API void SetEmitPosition(const math::Vector3& position) const;
    FBZZ_ENGINE_API void SetEmitVelocity(const math::Vector3& velocity) const;
    FBZZ_ENGINE_API void SetVelocitySpread(float spread) const;
    FBZZ_ENGINE_API void SetEnabled(bool enabled) const;
    FBZZ_ENGINE_API void Play(bool restart = true) const;
    FBZZ_ENGINE_API void Stop(bool clear = false) const;
    FBZZ_ENGINE_API void Burst(int count) const;
    FBZZ_ENGINE_API void Clear() const;
    FBZZ_ENGINE_API void SetGravity(const math::Vector3& gravity) const;
    FBZZ_ENGINE_API void SetColor(const math::Vector4& start, const math::Vector4& end) const;
    FBZZ_ENGINE_API void SetSize(float start, float end) const;
    FBZZ_ENGINE_API void SetLifetime(float seconds) const;
    FBZZ_ENGINE_API void SetMaxParticles(int maxParticles) const;
    FBZZ_ENGINE_API void SetPlayback(bool loop, float duration, bool clearOnStop = false) const;
    FBZZ_ENGINE_API void SetShape(ParticleEmitterShape shape) const;
    FBZZ_ENGINE_API void SetSphereShape(float radius) const;
    FBZZ_ENGINE_API void SetConeShape(float radius, float angleDegrees) const;
    FBZZ_ENGINE_API void SetBoxShape(const math::Vector3& extents) const;
    /// @brief FBX / Modelの頂点群からParticle形状を生成する。followSkinnedAnimation=trueなら同じGOのAnimator姿勢に追従する。
    /// @brief normalVelocity は表面法線方向へ与える初速 [m/s]。負値は表面へ向かう収束になる。
    FBZZ_ENGINE_API void SetMeshShape(std::string_view modelPath, int meshIndex = -1, float scale = 1.0f,
                      bool followSkinnedAnimation = false, float normalVelocity = 0.0f) const;
    /// @brief 法線初速だけを差し替える。崩壊の進行に合わせて «にじむ» から «噴き出す» へ
    /// @brief 変えるなど、形状を組み直さずに勢いだけ動かしたいときに使う。
    FBZZ_ENGINE_API void SetMeshShapeNormalVelocity(float normalVelocity) const;
    /// @note ブレンド・フリップブック・ソフトパーティクルなど «見た目» は .mat の
    /// @note [particle] が正本になったため、エミッター側からは触れない。
    FBZZ_ENGINE_API void SetSortMode(ParticleSortMode sortMode) const;
    FBZZ_ENGINE_API void SetSimulationMode(ParticleSimulationMode simulationMode) const;
    FBZZ_ENGINE_API void SetSimulationSpace(ParticleSimulationSpace space) const;
    FBZZ_ENGINE_API void SetRenderMode(ParticleRenderMode mode, float stretchScale = 1.0f) const;
    FBZZ_ENGINE_API void SetCollision(ParticleCollisionMode mode, ParticleCollisionResponse response,
                      float radius = 0.05f, float bounciness = 0.5f) const;
    [[nodiscard]] FBZZ_ENGINE_API int GetCollisionCount() const;
    FBZZ_ENGINE_API void SetRateOverDistance(float particlesPerMeter) const;
    FBZZ_ENGINE_API void SetPrewarm(bool enabled) const;
    FBZZ_ENGINE_API void SetSubEmitters(std::string_view birthEmitter, std::string_view deathEmitter,
                        std::string_view collisionEmitter, int burstCount = 1) const;
    FBZZ_ENGINE_API void SetVelocityDamping(float damping) const;
    FBZZ_ENGINE_API void SetAngularVelocity(float minValue, float maxValue) const;
    /// @brief ノイズモジュール (カールノイズ乱流)。strength 0 で無効。
    FBZZ_ENGINE_API void SetNoise(float strength, float frequency = 0.5f, float speed = 1.0f) const;
    /// @brief シーン内の FlowField から流れを受けるか
    FBZZ_ENGINE_API void SetReceiveFlowFields(bool receive) const;
    /// @brief 受け取る場をビットマスクで絞る。FlowField::channels と 1 ビットでも
    /// @brief 重なった場だけが効く。SetReceiveFlowFields(false) が優先される。
    FBZZ_ENGINE_API void SetFlowFieldChannels(uint32_t channels) const;

    /// @brief enabled は Component の有効/無効、IsPlaying はエフェクトの再生状態で別物。
    /// @brief Stop() 後も粒子が残っている間は IsPlaying が false で GetParticleCount > 0 になるため、
    /// @brief 「完全に消えてから GameObject を片付ける」判定はこの 2 つを併せて見る。
    [[nodiscard]] FBZZ_ENGINE_API bool  IsEnabled() const;
    [[nodiscard]] FBZZ_ENGINE_API bool  IsPlaying() const;
    /// @brief 生存中の粒子数。Gpu シミュレーションでは CPU 側に粒子列を持たないため常に 0。
    [[nodiscard]] FBZZ_ENGINE_API int   GetParticleCount() const;
    [[nodiscard]] FBZZ_ENGINE_API float GetEmitRate() const;
    [[nodiscard]] FBZZ_ENGINE_API int   GetMaxParticles() const;
    [[nodiscard]] FBZZ_ENGINE_API float GetLifetime() const;
    [[nodiscard]] FBZZ_ENGINE_API uint32_t GetFlowFieldChannels() const;
    /// @brief Play() からの経過秒。loop = false なら duration に達した時点で発生が止まる。
    [[nodiscard]] FBZZ_ENGINE_API float GetPlayTime() const;

    /// @name CPU / GPU シミュレーション
    /// @note «要求» (SetSimulationMode) と «実際» (IsGpuSimulated) は別物。縮退条件に触れると
    /// @note 要求と無関係に CPU で回るため、両方を見ないと「GPU で捌けている」と誤認する。
    /// @{
    /// @brief エミッターに設定されている経路 (要求値)。エミッターが無ければ Cpu。
    [[nodiscard]] FBZZ_ENGINE_API ParticleSimulationMode GetSimulationMode() const;

    /// @brief 実際に GPU シミュレーションで回るか。ParticlePass と同じ判定を通す。
    [[nodiscard]] FBZZ_ENGINE_API bool IsGpuSimulated() const;

    /// @brief GPU に載らない理由。None なら GPU で回る。Cpu を指定しているだけなら
    /// @note NotRequested (縮退ではない)。エミッターが無い場合も NotRequested。
    /// @note .mat 由来の 3 条件 (flipbookFrameBlending / motionVectorFlipbook /
    /// @note selfShadowStrength) は素材解決 (最初の描画時) まで判定に入らないため、
    /// @note OnStart や 1 フレーム目の OnUpdate では「GPU で回る」と誤答することがある。
    [[nodiscard]] FBZZ_ENGINE_API ParticleGpuFallbackReason GetGpuFallbackReason() const;

    /// @brief 縮退の原因になっている設定名 ("trailEnabled" 等)。None のときは空文字列。
    /// @brief Editor / vfx.lint と同じ語なので、ログへ出せばそのまま突き合わせられる。
    [[nodiscard]] FBZZ_ENGINE_API const char* GetGpuFallbackField() const;

    /// @brief 「なぜ落ちたか」と「どうすれば GPU に載るか」まで含む説明。
    [[nodiscard]] FBZZ_ENGINE_API const char* GetGpuFallbackDescription() const;
    /// @}
};

} /// @note namespace fbzz::scene
