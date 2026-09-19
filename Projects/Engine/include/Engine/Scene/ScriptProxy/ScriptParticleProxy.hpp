/// @file    ScriptParticleProxy.hpp
/// @brief   Script から ParticleEmitter を操作するショートハンド。
/// @author  Hasegawa Jin
/// @date    2026-06-01
#pragma once
/// @note ParticleColorSpace は別ヘッダーに置く。ParticleGradient (Engine/Scene/ParticleCurve.hpp)
///       がメンバーに持つため、Script.hpp のプロキシ群より先に読まれる必要がある。
#include <Engine/Scene/Components/ParticleColorSpace.hpp>
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
    /// @brief 事前乗算アルファ。発光する芯と背景を隠す煙を 1 枚のテクスチャで両立できる。
    Premultiplied = 2,
};

enum class ParticleSortMode : uint8_t {
    None        = 0,
    BackToFront = 1,
};

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
    ///       保つ必要もあるため GPU 非対応 (載せるなら CB の再設計が要る)。
    SpeedModule,
    /// @brief 予約: 速度場 (速度場 PNG) は 2026-09-11 に GPU 対応済み (VelocityFieldAtlas)。
    /// @note 枠を残す理由: Script DLL は数値で受け取るため、詰めると未再ビルドの DLL が
    ///       SpeedModule を別の理由として誤読する。
    ReservedVectorField,
    /// @brief Lights モジュール (light.lightEnabled)。GPU 粒子は位置が GPU にしかないため、
    /// @brief 点光源化 (RenderSystem が runtime.particles を読む) には CPU へ縮退する。
    /// @note 追加は必ず末尾へ。途中へ挿すと未再ビルドの DLL が以降の理由を 1 つずれて読む。
    Light,
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
    /// @brief FBX / Modelの頂点群からParticle形状を生成する。followSkinnedAnimation=trueなら同じGOのAnimator姿勢に追従する。
    /// @brief normalVelocity は表面法線方向へ与える初速 [m/s]。負値は表面へ向かう収束になる。
    void SetMeshShape(std::string_view modelPath, int meshIndex = -1, float scale = 1.0f,
                      bool followSkinnedAnimation = false, float normalVelocity = 0.0f) const;
    /// @brief 法線初速だけを差し替える。崩壊の進行に合わせて «にじむ» から «噴き出す» へ
    /// @brief 変えるなど、形状を組み直さずに勢いだけ動かしたいときに使う。
    void SetMeshShapeNormalVelocity(float normalVelocity) const;
    /// @note ブレンド・フリップブック・ソフトパーティクルなど «見た目» は .mat の
    ///       [particle] が正本になったため、エミッター側からは触れない。
    void SetSortMode(ParticleSortMode sortMode) const;
    void SetSimulationMode(ParticleSimulationMode simulationMode) const;
    void SetSimulationSpace(ParticleSimulationSpace space) const;
    void SetRenderMode(ParticleRenderMode mode, float stretchScale = 1.0f) const;
    void SetCollision(ParticleCollisionMode mode, ParticleCollisionResponse response,
                      float radius = 0.05f, float bounciness = 0.5f) const;
    [[nodiscard]] int GetCollisionCount() const;
    void SetRateOverDistance(float particlesPerMeter) const;
    void SetPrewarm(bool enabled) const;
    void SetSubEmitters(std::string_view birthEmitter, std::string_view deathEmitter,
                        std::string_view collisionEmitter, int burstCount = 1) const;
    void SetVelocityDamping(float damping) const;
    void SetAngularVelocity(float minValue, float maxValue) const;
    /// @brief ノイズモジュール (カールノイズ乱流)。strength 0 で無効。
    void SetNoise(float strength, float frequency = 0.5f, float speed = 1.0f) const;
    /// @brief シーン内の FlowField から流れを受けるか
    void SetReceiveFlowFields(bool receive) const;
    /// @brief 受け取る場をビットマスクで絞る。FlowField::channels と 1 ビットでも
    /// @brief 重なった場だけが効く。SetReceiveFlowFields(false) が優先される。
    void SetFlowFieldChannels(uint32_t channels) const;

    /// @brief enabled は Component の有効/無効、IsPlaying はエフェクトの再生状態で別物。
    /// @brief Stop() 後も粒子が残っている間は IsPlaying が false で GetParticleCount > 0 になるため、
    /// @brief 「完全に消えてから GameObject を片付ける」判定はこの 2 つを併せて見る。
    [[nodiscard]] bool  IsEnabled() const;
    [[nodiscard]] bool  IsPlaying() const;
    /// @brief 生存中の粒子数。Gpu シミュレーションでは CPU 側に粒子列を持たないため常に 0。
    [[nodiscard]] int   GetParticleCount() const;
    [[nodiscard]] float GetEmitRate() const;
    [[nodiscard]] int   GetMaxParticles() const;
    [[nodiscard]] float GetLifetime() const;
    [[nodiscard]] uint32_t GetFlowFieldChannels() const;
    /// @brief Play() からの経過秒。loop = false なら duration に達した時点で発生が止まる。
    [[nodiscard]] float GetPlayTime() const;

    /// @name CPU / GPU シミュレーション
    /// @note «要求» (SetSimulationMode) と «実際» (IsGpuSimulated) は別物。縮退条件に触れると
    ///       要求と無関係に CPU で回るため、両方を見ないと「GPU で捌けている」と誤認する。
    ///@{
    /// @brief エミッターに設定されている経路 (要求値)。エミッターが無ければ Cpu。
    [[nodiscard]] ParticleSimulationMode GetSimulationMode() const;

    /// @brief 実際に GPU シミュレーションで回るか。ParticlePass と同じ判定を通す。
    [[nodiscard]] bool IsGpuSimulated() const;

    /// @brief GPU に載らない理由。None なら GPU で回る。Cpu を指定しているだけなら
    ///        NotRequested (縮退ではない)。エミッターが無い場合も NotRequested。
    /// @note .mat 由来の 3 条件 (flipbookFrameBlending / motionVectorFlipbook /
    ///       selfShadowStrength) は素材解決 (最初の描画時) まで判定に入らないため、
    ///       OnStart や 1 フレーム目の OnUpdate では「GPU で回る」と誤答することがある。
    [[nodiscard]] ParticleGpuFallbackReason GetGpuFallbackReason() const;

    /// @brief 縮退の原因になっている設定名 ("trailEnabled" 等)。None のときは空文字列。
    /// @brief Editor / vfx.lint と同じ語なので、ログへ出せばそのまま突き合わせられる。
    [[nodiscard]] const char* GetGpuFallbackField() const;

    /// @brief 「なぜ落ちたか」と「どうすれば GPU に載るか」まで含む説明。
    [[nodiscard]] const char* GetGpuFallbackDescription() const;
    /// @}
};

} // namespace fbzz::scene
