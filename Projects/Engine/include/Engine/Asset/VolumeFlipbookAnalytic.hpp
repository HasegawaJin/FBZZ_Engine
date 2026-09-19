/// @file    VolumeFlipbookAnalytic.hpp
/// @brief   Volume Flipbook Baker が焼く、速度が厳密に分かる解析ボリューム。
/// @author  Hasegawa Jin
/// @date    2026-09-11
///
/// @note puff は時刻 a の 3x3 行列 A(a) = s(a)·E(a)·R(ω·a) (膨張・進行方向への伸び・定軸回転) と中心
///       c(a) の和で表す。中心は閉じた式で求まるため、flow map x' = c1 + A1·A0^-1·(x - c0) も閉じた式に
///       なり MV の正解として使える。
/// @note 流体ソルバーより先にこれを書く理由: 速度場の正誤は外から検証できないため、流れの向きと MV の
///       符号・単位をここで先に確定させてから VolumeFill.cs を差し替える。
/// @note 座標は bake 空間の立方体 [-1,1]^3 (y 上向き)、時間は秒。GPU 側 (VolumeFill.cs.hlsl) と式を
///       1:1 で一致させること。
#pragma once

#include <Math/Vector3.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace fbzz::asset {

/// 1 コマに詰められる puff の上限 (GPU の StructuredBuffer の要素数)。
inline constexpr std::uint32_t kVolumeFillMaxPuffs = 1024;
/// 物体座標でこの半径より外は密度が必ず 0 (noise による縁の揺らぎを含めた上限)。
inline constexpr float kVolumePuffBodyCullRadius = 1.4f;

struct VolumeNoiseSettings {
    float frequency = 2.2f;
    /// 縁の揺らぎと内部の濃淡の強さ。[0,1]。1 を超えると cull 半径の外に密度が出る。
    float amplitude = 0.7f;
};

struct VolumePuff {
    float birthTime = 0.0f;
    float lifetime = 1.0e9f;
    float fadeIn = 0.0f;
    float fadeOut = 0.0f;
    math::Vector3 startCenter;
    /// 生まれたときの中心の速度。
    math::Vector3 velocity;
    /// 中心にかかる一定の加速度 (重力・浮力)。
    math::Vector3 acceleration;
    /// 速度に比例する減速の係数 [1/s]。0 で減速しない。
    float drag = 0.0f;
    /// 回転軸 × 角速度 [rad/s]。
    math::Vector3 angularVelocity;
    /// 半径が e^{rate·age} で膨らむ。
    float expansionRate = 0.0f;
    float radius = 0.3f;
    /// 中心の進行方向 (止まっているときは上) への伸び σ = max(1, stretch + stretchPerSpeed·速さ)。
    /// 体積を保つよう、横は 1/√σ に縮む。液滴の筋や炎の舌に使う。
    float stretch = 1.0f;
    float stretchPerSpeed = 0.0f;
    float density = 1.0f;
    /// 生まれたときの芯の温度 [0,1]。0 なら発光しない。
    float temperature = 0.0f;
    float coolingTime = 1.0f;
    float noiseSeedOffset = 0.0f;
    /// 縁の揺らぎ (VolumeNoiseSettings::amplitude) へ掛ける倍率。液体は小さくして表面を滑らかにする。
    float noiseScale = 1.0f;
    /// Look の Albedo Ramp 上の位置 [0,1]。
    float colorKey = 0.0f;
    /// 0 = 煙 (散乱する媒質) / 1 = 液体 (表面を持つ)。中間は混ざる。
    float liquid = 0.0f;
};

/// StructuredBuffer の 1 要素 (float4 x 8)。時間に依存する計算は CPU で済ませる。
struct alignas(16) VolumeFillPuffGpu {
    /// 行 i = (B の行 i, beta_i)。物体座標 y = B x + beta。
    float body[3][4];
    /// 行 i = (M の行 i, m_i)。1 コマぶりの割線速度 v = M x + m [bake 単位/秒]。
    float velocity[3][4];
    /// x = density·envelope, y = 芯の温度, z = noise seed, w = 物体座標の cull 半径
    float params[4];
    /// x = colorKey, y = liquid, z = noise の倍率, w = 予備
    float look[4];
};
static_assert(sizeof(VolumeFillPuffGpu) == 128, "VolumeFill.cs.hlsl の VolumePuffGpu と一致させること");

/// VolumeFill.cs.hlsl の cbuffer。
struct alignas(16) VolumeFillHeader {
    std::uint32_t resolution = 64;
    std::uint32_t puffCount = 0;
    float noiseFrequency = 2.2f;
    float noiseAmplitude = 0.7f;
};
static_assert(sizeof(VolumeFillHeader) == 16, "VolumeFill.cs.hlsl の cbuffer と一致させること");

/// 1 コマぶんの GPU 入力。puffs.size() == header.puffCount。
struct VolumeFillFrame {
    VolumeFillHeader header;
    std::vector<VolumeFillPuffGpu> puffs;
};

struct VolumeSample {
    float density = 0.0f;
    float temperature = 0.0f;
    float colorKey = 0.0f;
    float liquid = 0.0f;
    math::Vector3 velocity;
};

/// 生まれてから age 秒後の中心。
[[nodiscard]] math::Vector3 PuffCenterAt(const VolumePuff& puff, float age);
/// 生まれてから age 秒後の中心の速度。
[[nodiscard]] math::Vector3 PuffCenterVelocityAt(const VolumePuff& puff, float age);

/// 時刻 t0 に x にあった物質点の、時刻 t1 での位置。
[[nodiscard]] math::Vector3 PuffFlowMap(const VolumePuff& puff, const math::Vector3& x,
                                        float t0, float t1);

/// 物体座標での密度 (envelope を掛ける前)。|y| > kVolumePuffBodyCullRadius では必ず 0。
[[nodiscard]] float EvaluatePuffBodyDensity(const math::Vector3& bodyPosition, float noiseSeedOffset,
                                            const VolumeNoiseSettings& noise, float noiseScale = 1.0f);

/// 時刻 time に envelope が 0 でない puff の数。
[[nodiscard]] std::uint32_t CountLiveVolumePuffs(std::span<const VolumePuff> puffs, float time);

/// 時刻 t に生きている puff を詰める。速度は [t, t + frameDt] の割線速度。
/// @return 詰めた puff 数 (kVolumeFillMaxPuffs を超えた分は捨てる)。
std::uint32_t PackVolumeFill(std::span<const VolumePuff> puffs, const VolumeNoiseSettings& noise,
                             std::uint32_t resolution, float time, float frameDt, VolumeFillFrame& out);

/// VolumeFill.cs.hlsl の 1 ボクセルぶんの計算の写し。
[[nodiscard]] VolumeSample SampleVolumeFill(const VolumeFillFrame& frame, const math::Vector3& x);

/// 1 つの puff が «目に見える» 範囲を包む球。
struct VolumeBound {
    math::Vector3 center;
    float radius = 0.0f;
};

/// 時刻 time に見えている puff の範囲。構図の検査 (箱やタイルの縁で切れないか) に使う。
/// @note 密度が厳密に 0 になる半径 (kVolumePuffBodyCullRadius) ではなく、ほぼ透明になる半径で見積もる。
///       厳密な半径だと «実際には見えない縁» まで警告になり、既定のプリセットでも鳴り止まない。
[[nodiscard]] std::vector<VolumeBound> CollectVisibleVolumeBounds(std::span<const VolumePuff> puffs,
                                                                  const VolumeNoiseSettings& noise,
                                                                  float time);

} // namespace fbzz::asset
