/// @file    ParticleTypes.hpp
/// @brief   粒子の保存値・描画入力・スクリプトで共有する列挙型。
/// @author  Hasegawa Jin
/// @date    2026-10-31
#pragma once
#include <Graphics/Effects/ParticleDrawTypes.hpp>
#include <Engine/Scene/Components/ParticleColorSpace.hpp>
#include <cstdint>
namespace fbzz::scene {
enum class ParticleEmitterShape : uint8_t {
    Point  = 0,
    Sphere = 1,
    Cone   = 2,
    Box    = 3,
    MeshSurface = 4,
};

using renderer::ParticleBlendMode;

using renderer::ParticleSortMode;

enum class ParticleSimulationMode : uint8_t {
    Cpu = 0,
    Gpu = 1,
};

/// @brief simulationMode = Gpu を要求しても GPU で回らない理由。None なら GPU で回る。
/// @brief 判定の実体と rationale は Engine/Scene/Components/ParticleGpuSimulation.hpp。
/// @brief 宣言だけここに置くのは、Script が Engine 実装を include せずに理由を受け取れるように
/// @brief するため (ParticleSimulationMode 等と同じ扱い)。
enum class ParticleGpuFallbackReason {
    None = 0,
    NotRequested,          ///< @brief simulationMode が Cpu。そもそも要求していない (縮退ではない)
    LocalSpace,            ///< @brief simulationSpace = Local。GPU 側は World 座標でしか積分しない
    Collision,             ///< @brief collisionMode が Depth 以外。シーン形状との判定は CPU 側にしかない
    Prewarm,               ///< @brief prewarm。過去へ遡ったスポーンは GPU の逐次積分では作れない
    FlipbookFrameBlending, ///< @brief フレーム間補間
    MotionVectorFlipbook,  ///< @brief モーションベクター付きフリップブック
    Trail,                 ///< @brief per-particle Trail。GpuParticle は位置履歴を持たない
    SubEmitter,            ///< @brief birth / death / collision SubEmitter。発火は CPU 側の処理
    SelfShadow,            ///< @brief selfShadowStrength > 0。密度パスが CPU 頂点バッファを要求する
    /// @brief 速さで見た目を変えるモジュール (useSpeedSizeCurve / useSpeedColorGradient)。
    /// @note 速度グラデーション (8キー=float4×12) は定数バッファを太らせ、CPU と同じ式を
    /// @note 保つ必要もあるため GPU 非対応 (載せるなら CB の再設計が要る)。
    SpeedModule,
    /// @brief 予約: 速度場 (速度場 PNG) は 2026-09-11 に GPU 対応済み (VelocityFieldAtlas)。
    /// @note managed facade の数値契約を維持するため、この予約値は詰めない。
    ReservedVectorField,
    /// @brief Lights モジュール (light.lightEnabled)。GPU 粒子は位置が GPU にしかないため、
    /// @brief 点光源化 (RenderSystem が runtime.particles を読む) には CPU へ縮退する。
    /// @note managed facade の数値契約を維持するため、新しい理由は末尾へ追加する。
    Light,
};

using renderer::ParticleSimulationSpace;

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

/// @brief テクスチャからアルファをどう取り出すか。パーティクル素材はアルファ無し (黒/白背景) の
/// @brief ものが多く、そのままブレンドすると黒い矩形が出るため吸収する。値は Rendering/Mask.hlsli
/// @brief の FBZZ_MASK_* と一致させ、専用の番号体系は作らない。
enum class ParticleAlphaSource : uint8_t {
    TextureAlpha      = 0, ///< @brief 通常 (アルファ付き素材)
    Luminance         = 1, ///< @brief 黒 = 透明 (加算用の黒背景素材)
    LuminanceInverted = 2, ///< @brief 白 = 透明 (白背景素材)
    Red               = 3, ///< @brief R チャンネル (パック済みマスクの1枚目)
    Green             = 4, ///< @brief G チャンネル
    Blue              = 5, ///< @brief B チャンネル
    AlphaInverted     = 6, ///< @brief アルファ反転
};

enum class ParticleFlipbookMode : uint8_t {
    Lifetime = 0,
    FramesPerSecond,
    RandomFrame,
    PingPong,
};


}
