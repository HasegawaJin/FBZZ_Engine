/// @file    ParticleDrawTypes.hpp
/// @brief   パーティクルの描画・スポーン用値型。
/// @author  Hasegawa Jin
/// @date    2026-09-21
#pragma once
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
#include <array>
#include <cstdint>
namespace fbzz::renderer {
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
enum class ParticleSimulationSpace : uint8_t {
    World = 0,
    Local = 1,
};
/// @brief CS/VS 共通の GPU パーティクル 1 粒子レイアウト (96 bytes, 16-byte aligned)
/// @brief `StructuredBuffer<GpuParticle>` に格納し、CS が lifetime/age を更新、VS が位置を読む。
struct GpuParticle {
    math::Vector3 position;        ///< @brief 12B
    float         size;            ///< @brief 4B
    math::Vector3 velocity;        ///< @brief 12B
    float         age;             ///< @brief 4B
    math::Vector4 color;           ///< @brief 16B
    float         lifetime;        ///< @brief 4B
    float         rotation;        ///< @brief 4B
    float         angularVelocity; ///< @brief 4B
    float         spriteSeed;      ///< @brief 4B
    math::Vector4 uvRect;          ///< @brief 16B
    /// @brief 粒子ごとの色倍率 (colorVariation の結果)。
    /// @note CS は毎フレーム色を CB の colorStart/End (または gradient) から作り直すため、
    /// @note スポーン時のゆらぎは倍率として保持し毎フレーム掛け直す (でなければ翌フレームに消える)。
    math::Vector3 colorScale;      ///< @brief 12B
    /// @brief 次のコマへの補間率 (Frame Blending)。0 なら nextUvRect は読まれない。
    float         spriteBlend;     ///< @brief 4B
    /// @brief 次のコマの UV 矩形。CS が EvaluateFlipbookFrame と同じ規則で書く。
    math::Vector4 nextUvRect;      ///< @brief 16B
};

/// @brief CPU → CS へのスポーンリクエスト 1 件 (96 bytes, 16-byte aligned)
/// @brief DYNAMIC StructuredBuffer に毎フレーム書き込み、CS がリングバッファで配置する。
struct GpuSpawnEntry {
    math::Vector3 position;        ///< @brief 12B
    float         lifetime;        ///< @brief 4B
    math::Vector3 velocity;        ///< @brief 12B
    float         size;            ///< @brief 4B
    math::Vector4 colorStart;      ///< @brief 16B
    /// @brief 粒子ごとの色ゆらぎ倍率 (xyz)。w は未使用。
    /// @brief CPU が求めた倍率をそのまま渡す。基準色との「比」から復元しようとすると、
    /// @brief 黒に近いチャンネルで暴れ、グラデーション使用時はそもそも成立しない。
    math::Vector4 colorScale;      ///< @brief 16B
    math::Vector4 uvRect;          ///< @brief 16B
    float         rotation;        ///< @brief 4B
    float         angularVelocity; ///< @brief 4B
    float         spriteSeed;      ///< @brief 4B
    float         pad1;            ///< @brief 4B
};

static_assert(sizeof(GpuParticle) == 112, "GpuParticle must match ParticleGpuSim.cs.hlsl (112 bytes)");
static_assert(sizeof(GpuSpawnEntry) == 96, "GpuSpawnEntry must match ParticleGpuSim.cs.hlsl (96 bytes)");

/// @brief 1 粒子が保持するトレイル履歴の最大点数。
/// @note 固定長にして Particle を POD のまま保つ。粒子ごとに vector を持たせると
/// @note スポーン/消滅のたびにヒープ確保が走り、数千粒子では確保コストが支配的になる。
inline constexpr int kMaxParticleTrailPoints = 8;

struct Particle {
    math::Vector3 position;
    math::Vector3 velocity;
    math::Vector4 color;
    float         size;
    float         age;
    float         rotation = 0.0f;
    float         angularVelocity = 0.0f;
    float         lifetime = 1.0f;
    float         startSize = 1.0f;
    float         endSize = 0.0f;
    float         spriteSeed = 0.0f;
    math::Vector4 startColor = { 1, 1, 1, 1 };
    math::Vector4 endColor = { 1, 1, 1, 0 };
    /// @brief 発生時に配る色ゆらぎ倍率。毎フレーム作り直す色へ掛け直すため保持する。
    /// @note グラデーション使用時は毎フレーム color が上書きされるため、色そのものへ焼き込むと
    /// @note ゆらぎが翌フレームに消える。
    math::Vector3 colorScale = { 1.0f, 1.0f, 1.0f };
    math::Vector4 uvRect = { 0.0f, 0.0f, 1.0f, 1.0f };
    math::Vector4 nextUvRect = { 0.0f, 0.0f, 1.0f, 1.0f };
    float spriteBlend = 0.0f;
    /// @brief トレイル履歴。[0] が最新で、後ろほど古い (＝尾の先端側)。
    /// @brief 位置はシミュレーション空間で持ち、描画時に粒子本体と同じ変換を通す。
    std::array<math::Vector3, kMaxParticleTrailPoints> trailPoints{};
    uint8_t trailCount = 0;
    float   trailSampleTimer = 0.0f;
};

struct ParticleTrailSettings {
    bool  trailEnabled        = false;
    int   trailPointCount     = 6;      ///< @brief 使用する履歴点数 [1, kMaxParticleTrailPoints]
    float trailSampleInterval = 0.03f;  ///< @brief 履歴を刻む間隔 [秒]。短いほど滑らか
    float trailWidthScale     = 0.6f;   ///< @brief 尾の先端 (最古) 側のサイズ倍率
    float trailAlphaScale     = 0.5f;   ///< @brief 尾の先端側の不透明度倍率
    math::Vector4 trailColorTint = { 1.0f, 1.0f, 1.0f, 1.0f };
    bool  trailRibbon         = false;
    /// @brief 帯の幅 [m]。0 以下なら粒子サイズをそのまま使う。
    /// @note 帯は粒子サイズと独立に太さを決めたいことが多い (小さな火の粉が太い軌跡を引く等)。
    float trailRibbonWidth    = 0.0f;
};
}
