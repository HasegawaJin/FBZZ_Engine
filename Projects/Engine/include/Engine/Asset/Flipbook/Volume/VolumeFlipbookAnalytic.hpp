/// @file    VolumeFlipbookAnalytic.hpp
/// @brief   Volume Flipbook Baker が焼く、速度が厳密に分かる解析ボリューム。
/// @author  Hasegawa Jin
/// @date    2026-09-11

/// @note puff は時刻 a の 3x3 行列 A(a) = s(a)·E(a)·R(ω·a) (膨張・進行方向への伸び・定軸回転) と中心
/// @note c(a) の和で表す。中心は閉じた式で求まるため、flow map x' = c1 + A1·A0^-1·(x - c0) も閉じた式に
/// @note なり MV の正解として使える。
/// @note 流体ソルバーより先にこれを書く理由: 速度場の正誤は外から検証できないため、流れの向きと MV の
/// @note 符号・単位をここで先に確定させてから VolumeFill.cs を差し替える。
/// @note 座標は bake 空間の立方体 [-1,1]^3 (y 上向き)、時間は秒。GPU 側 (VolumeFill.cs.hlsl) と式を
/// @note 1:1 で一致させること。
#pragma once
#include <Engine/Api.hpp>

#include <Math/LinearAlgebra/Vector3.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace fbzz::asset {

/// @note 1 コマに詰められる puff の上限 (GPU の StructuredBuffer の要素数)。
inline constexpr std::uint32_t kVolumeFillMaxPuffs = 1024;
/// @note 物体座標でこの半径より外は密度が必ず 0 (noise による縁の揺らぎを含めた上限)。
inline constexpr float kVolumePuffBodyCullRadius = 1.4f;

struct VolumeNoiseSettings {
    float frequency = 2.2f; ///< パフの外縁を変調するノイズの空間周波数。
    float amplitude = 0.7f; ///< 縁の揺らぎと内部の濃淡の強さ。[0,1]。1 を超えると cull 半径の外に密度が出る。
};

struct VolumePuff {
    float birthTime = 0.0f; ///< ベイクの先頭を基準とするパフの発生時刻 [s]。
    float lifetime = 1.0e9f; ///< 発生からパフが消えるまでの時間 [s]。
    float fadeIn = 0.0f; ///< 発生してから密度を立ち上げる時間 [s]。
    float fadeOut = 0.0f; ///< 寿命の末尾で密度を下げる時間 [s]。
    math::Vector3 startCenter; ///< パフの発生時点のシミュレーション空間上の中心。
    math::Vector3 velocity; ///< 生まれたときの中心の速度。
    math::Vector3 acceleration; ///< 中心にかかる一定の加速度 (重力・浮力)。
    float drag = 0.0f; ///< 速度に比例する減速の係数 [1/s]。0 で減速しない。
    math::Vector3 angularVelocity; ///< 回転軸 × 角速度 [rad/s]。
    float expansionRate = 0.0f; ///< 半径が e^{rate·age} で膨らむ。
    float radius = 0.3f; ///< パフの伸長を適用する前の基準半径。
    /// @note 中心の進行方向 (止まっているときは上) への伸び σ = max(1, stretch + stretchPerSpeed·速さ)。
    /// @note 体積を保つよう、横は 1/√σ に縮む。液滴の筋や炎の舌に使う。
    float stretch = 1.0f; ///< 速度方向の軸へ掛けるパフの伸長倍率。
    float stretchPerSpeed = 0.0f; ///< 速度の大きさから追加の伸長倍率を求める係数。
    float density = 1.0f; ///< パフがボリュームに加える媒質密度の強さ。
    float temperature = 0.0f; ///< 生まれたときの芯の温度 [0,1]。0 なら発光しない。
    float coolingTime = 1.0f; ///< 発生後に温度を減衰させる時間尺度 [s]。
    float noiseSeedOffset = 0.0f; ///< 個々のパフでノイズの模様を変える参照位置のずれ。
    float noiseScale = 1.0f; ///< 縁の揺らぎ (VolumeNoiseSettings::amplitude) へ掛ける倍率。液体は小さくして表面を滑らかにする。
    float colorKey = 0.0f; ///< Look の Albedo Ramp 上の位置 [0,1]。
    float liquid = 0.0f; ///< 0 = 煙 (散乱する媒質) / 1 = 液体 (表面を持つ)。中間は混ざる。
};

/// @note StructuredBuffer の 1 要素 (float4 x 8)。時間に依存する計算は CPU で済ませる。
struct alignas(16) VolumeFillPuffGpu {
    /// @note 行 i = (B の行 i, beta_i)。物体座標 y = B x + beta。
    float body[3][4];
    /// @note 行 i = (M の行 i, m_i)。1 コマぶりの割線速度 v = M x + m [bake 単位/秒]。
    float velocity[3][4];
    /// @note x = density·envelope, y = 芯の温度, z = noise seed, w = 物体座標の cull 半径
    float params[4];
    /// @note x = colorKey, y = liquid, z = noise の倍率, w = 予備
    float look[4];
};
static_assert(sizeof(VolumeFillPuffGpu) == 128, "VolumeFill.cs.hlsl の VolumePuffGpu と一致させること");

/// @note VolumeFill.cs.hlsl の cbuffer。
struct alignas(16) VolumeFillHeader {
    std::uint32_t resolution = 64;
    std::uint32_t puffCount = 0;
    float noiseFrequency = 2.2f;
    float noiseAmplitude = 0.7f;
};
static_assert(sizeof(VolumeFillHeader) == 16, "VolumeFill.cs.hlsl の cbuffer と一致させること");

/// @note 1 コマぶんの GPU 入力。puffs.size() == header.puffCount。
struct VolumeFillFrame {
    VolumeFillHeader header; ///< グリッドを充填する GPU パスへ渡す、このコマ共通のパラメーター。
    std::vector<VolumeFillPuffGpu> puffs; ///< このコマの充填パスへ渡す有効パフの GPU 形式の列。
};

struct VolumeSample {
    float density = 0.0f; ///< 問い合わせ位置で合成した媒質密度。
    float temperature = 0.0f; ///< 問い合わせ位置で合成した媒質の温度チャンネル。
    float colorKey = 0.0f; ///< 問い合わせ位置の色をランプから得るための補間キー。
    float liquid = 0.0f; ///< 問い合わせ位置で合成した液体の充填量。
    math::Vector3 velocity; ///< 問い合わせ位置で合成したシミュレーション空間の速度。
};

/// @note 生まれてから age 秒後の中心。
[[nodiscard]] FBZZ_ENGINE_API math::Vector3 PuffCenterAt(const VolumePuff& puff, float age);
/// @note 生まれてから age 秒後の中心の速度。
[[nodiscard]] FBZZ_ENGINE_API math::Vector3 PuffCenterVelocityAt(const VolumePuff& puff, float age);

/// @note 時刻 t0 に x にあった物質点の、時刻 t1 での位置。
[[nodiscard]] FBZZ_ENGINE_API math::Vector3 PuffFlowMap(const VolumePuff& puff, const math::Vector3& x,
                                        float t0, float t1);

/// @note 物体座標での密度 (envelope を掛ける前)。|y| > kVolumePuffBodyCullRadius では必ず 0。
[[nodiscard]] FBZZ_ENGINE_API float EvaluatePuffBodyDensity(const math::Vector3& bodyPosition, float noiseSeedOffset,
                                            const VolumeNoiseSettings& noise, float noiseScale = 1.0f);

/// @note 時刻 time に envelope が 0 でない puff の数。
[[nodiscard]] FBZZ_ENGINE_API std::uint32_t CountLiveVolumePuffs(std::span<const VolumePuff> puffs, float time);

/// @note 時刻 t に生きている puff を詰める。速度は [t, t + frameDt] の割線速度。
/// @return 詰めた puff 数 (kVolumeFillMaxPuffs を超えた分は捨てる)。
FBZZ_ENGINE_API std::uint32_t PackVolumeFill(std::span<const VolumePuff> puffs, const VolumeNoiseSettings& noise,
                             std::uint32_t resolution, float time, float frameDt, VolumeFillFrame& out);

/// @note VolumeFill.cs.hlsl の 1 ボクセルぶんの計算の写し。
[[nodiscard]] FBZZ_ENGINE_API VolumeSample SampleVolumeFill(const VolumeFillFrame& frame, const math::Vector3& x);

/// @note 1 つの puff が «目に見える» 範囲を包む球。
struct VolumeBound {
    math::Vector3 center; ///< ボリューム境界球のシミュレーション空間上の中心。
    float radius = 0.0f; ///< 境界球の中心からボリュームを覆う距離。
};

/// @note 時刻 time に見えている puff の範囲。構図の検査 (箱やタイルの縁で切れないか) に使う。
/// @note 密度が厳密に 0 になる半径 (kVolumePuffBodyCullRadius) ではなく、ほぼ透明になる半径で見積もる。
/// @note 厳密な半径だと «実際には見えない縁» まで警告になり、既定のプリセットでも鳴り止まない。
[[nodiscard]] FBZZ_ENGINE_API std::vector<VolumeBound> CollectVisibleVolumeBounds(std::span<const VolumePuff> puffs,
                                                                  const VolumeNoiseSettings& noise,
                                                                  float time);

} // namespace fbzz::asset
