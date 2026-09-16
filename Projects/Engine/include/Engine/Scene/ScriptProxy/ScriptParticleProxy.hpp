/// @file    ScriptParticleProxy.hpp
/// @brief   Script から ParticleEmitter を操作するショートハンド。
/// @author  Hasegawa Jin
/// @date    2026-06-01
#pragma once
#include <Engine/Scene/Components/ParticleColorSpace.hpp> // ParticleColorSpace
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

// ParticleColorSpace は ParticleColorSpace.hpp が持つ。
// WHY 移したか: ParticleGradient (Engine/Scene/ParticleCurve.hpp) がメンバーに持つため、
//     Script.hpp のプロキシ群より先に読まれる必要がある。変換関数と同じ場所に置けば、
//     enum と「その空間で混ぜる処理」が離れない。

enum class ParticleSimulationMode : uint8_t {
    Cpu = 0,
    Gpu = 1,
};

// simulationMode = Gpu を要求しても GPU で回らない理由。None なら GPU で回る。
// 判定の実体と rationale は Engine/Scene/Components/ParticleGpuSimulation.hpp。
// 宣言だけここに置くのは、Script が Engine 実装を include せずに理由を受け取れるように
// するため (ParticleSimulationMode 等と同じ扱い)。
enum class ParticleGpuFallbackReason {
    None = 0,
    NotRequested,          // simulationMode が Cpu。そもそも要求していない (縮退ではない)
    LocalSpace,            // simulationSpace = Local。GPU 側は World 座標でしか積分しない
    Collision,             // collisionMode が Depth 以外。シーン形状との判定は CPU 側にしかない
    Prewarm,               // prewarm。過去へ遡ったスポーンは GPU の逐次積分では作れない
    FlipbookFrameBlending, // フレーム間補間
    MotionVectorFlipbook,  // モーションベクター付きフリップブック
    Trail,                 // per-particle Trail。GpuParticle は位置履歴を持たない
    SubEmitter,            // birth / death / collision SubEmitter。発火は CPU 側の処理
    SelfShadow,            // selfShadowStrength > 0。密度パスが CPU 頂点バッファを要求する
    // 速さで見た目を変えるモジュール (useSpeedSizeCurve / useSpeedColorGradient)。
    // WHY GPU へ載せないか: 速度グラデーションは 8 キー = float4 が 12 本必要で、
    //     シミュレーション定数バッファを 1 モジュールのために倍近く太らせる。
    //     しかも CPU と «必ず同じ式» を保つ約束があるため、GPU 側だけ 2 色補間で
    //     近似する逃げ方も取れない。載せるなら CB の作り直しから始める話になる。
    SpeedModule,
    // 予約: 速度場 (.vfield) は 2026-09-11 に GPU 対応した (VelocityFieldAtlas)。
    // WHY 枠を残すか: この enum は Script DLL が数値で受け取る。詰めると、
    //     再ビルドしていないスクリプトが SpeedModule を «その次の理由» と読む。
    ReservedVectorField,
    // Lights モジュール (light.lightEnabled)。粒子を点光源にするには CPU 側に位置が
    // 要る (RenderSystem が runtime.particles を読んでライト配列へ積む)。
    // GPU の粒子は位置が GPU にしか無いため、光らせるなら CPU へ縮退する。
    // NOTE: 追加は必ず末尾へ。途中へ挿すと、再ビルドしていない Script DLL が
    //       以降の理由を 1 つずれた値として読む (上の予約枠と同じ理由)。
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
    // normalVelocity は表面法線方向へ与える初速 [m/s]。負値は表面へ向かう収束になる。
    void SetMeshShape(std::string_view modelPath, int meshIndex = -1, float scale = 1.0f,
                      bool followSkinnedAnimation = false, float normalVelocity = 0.0f) const;
    /// 法線初速だけを差し替える。崩壊の進行に合わせて «にじむ» から «噴き出す» へ
    /// 変えるなど、形状を組み直さずに勢いだけ動かしたいときに使う。
    void SetMeshShapeNormalVelocity(float normalVelocity) const;
    // NOTE: ブレンド・フリップブック・ソフトパーティクルなど «見た目» は .mat の
    //       [particle] が正本になったため、エミッター側からは触れない。
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
    // ノイズモジュール (カールノイズ乱流)。strength 0 で無効。
    void SetNoise(float strength, float frequency = 0.5f, float speed = 1.0f) const;
    // シーン内の ForceField から力を受けるか
    void SetReceiveForceFields(bool receive) const;
    /// 受け取る力場をビットマスクで絞る。ForceField::channels と 1 ビットでも
    /// 重なった力場だけが効く。SetReceiveForceFields(false) が優先される。
    void SetForceFieldChannels(uint32_t channels) const;

    // enabled は Component の有効/無効、IsPlaying はエフェクトの再生状態で別物。
    // Stop() 後も粒子が残っている間は IsPlaying が false で GetParticleCount > 0 になるため、
    // 「完全に消えてから GameObject を片付ける」判定はこの 2 つを併せて見る。
    [[nodiscard]] bool  IsEnabled() const;
    [[nodiscard]] bool  IsPlaying() const;
    // 生存中の粒子数。Gpu シミュレーションでは CPU 側に粒子列を持たないため常に 0。
    [[nodiscard]] int   GetParticleCount() const;
    [[nodiscard]] float GetEmitRate() const;
    [[nodiscard]] int   GetMaxParticles() const;
    [[nodiscard]] float GetLifetime() const;
    [[nodiscard]] uint32_t GetForceFieldChannels() const;
    // Play() からの経過秒。loop = false なら duration に達した時点で発生が止まる。
    [[nodiscard]] float GetPlayTime() const;

    // ── CPU / GPU シミュレーション ──
    // WHY «要求» と «実際» を別に読ませるか: SetSimulationMode(Gpu) は要求でしかなく、
    //     縮退条件に 1 つでも触れると黙って CPU で回る。要求値だけを見て
    //     「10 万粒子を GPU で捌けている」と思い込む事故がここでしか防げない。
    //
    //   if (!particle.IsGpuSimulated())
    //       debug.LogWarning(std::string("particles fell back to CPU: ")
    //                        + particle.GetGpuFallbackField());

    /// エミッターに設定されている経路 (要求値)。エミッターが無ければ Cpu。
    [[nodiscard]] ParticleSimulationMode GetSimulationMode() const;

    /// 実際に GPU シミュレーションで回るか。ParticlePass と同じ判定を通す。
    [[nodiscard]] bool IsGpuSimulated() const;

    /// GPU に載らない理由。None なら GPU で回る。Cpu を指定しているだけなら
    /// NotRequested (縮退ではない)。エミッターが無い場合も NotRequested。
    ///
    /// NOTE: .mat 由来の 3 条件 (flipbookFrameBlending / motionVectorFlipbook /
    ///       selfShadowStrength) は素材が解決されるまで判定に入らない。解決は最初の
    ///       描画時なので、OnStart と 1 フレーム目の OnUpdate では「GPU で回る」と
    ///       答えることがある。常時監視するなら OnUpdate で読むこと。
    [[nodiscard]] ParticleGpuFallbackReason GetGpuFallbackReason() const;

    /// 縮退の原因になっている設定名 ("trailEnabled" 等)。None のときは空文字列。
    /// Editor / vfx.lint と同じ語なので、ログへ出せばそのまま突き合わせられる。
    [[nodiscard]] const char* GetGpuFallbackField() const;

    /// 「なぜ落ちたか」と「どうすれば GPU に載るか」まで含む説明。
    [[nodiscard]] const char* GetGpuFallbackDescription() const;
};

} // namespace fbzz::scene
