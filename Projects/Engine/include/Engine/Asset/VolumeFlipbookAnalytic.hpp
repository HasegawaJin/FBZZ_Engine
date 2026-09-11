/// @file    VolumeFlipbookAnalytic.hpp
/// @brief   Volume Flipbook Baker が焼く、速度が厳密に分かる解析ボリューム。
/// @author  Hasegawa Jin
/// @date    2026-09-11
///
/// ボリュームは noise を纏った «puff» の和。各 puff は並進 + 定軸回転 + 等方膨張の
/// アフィン変換で運ばれるため、コマ N の物質点がコマ N+1 でどこにあるか (flow map) が
/// 閉じた式で出る。MV の正解がそのまま手に入るので、ベイク処理全体の検証台になる。
///
/// WHY 流体ソルバーを先に書かないか: ソルバーの速度場は «正しいかどうか» を外から判定できない。
///   流れの向きと MV の符号・単位を先に確定させてから、VolumeFill.cs を差し替える。
///
/// 座標は bake 空間の立方体 [-1,1]^3 (y 上向き)。時間は秒。
/// GPU 側は Assets/Shaders/Bake/VolumeFlipbook/VolumeFill.cs.hlsl。式は 1:1 で一致させること。
#pragma once

#include <Math/Vector3.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace fbzz::asset {

enum class VolumeFlipbookPreset : std::uint8_t {
    Puff = 0,     ///< 単体の煙が昇りながら回って膨らむ
    RisingPlume,  ///< 下から puff が湧き続け、熱い芯が冷えて煙になる
    Fireball,     ///< 中心から放射状に弾けて急冷する
};

/// GPU 定数へ詰められる puff の上限。
inline constexpr std::uint32_t kVolumeFillMaxPuffs = 32;
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
    math::Vector3 velocity;
    /// 回転軸 × 角速度 [rad/s]。
    math::Vector3 angularVelocity;
    /// 半径が e^{rate·age} で膨らむ。
    float expansionRate = 0.0f;
    float radius = 0.3f;
    float density = 1.0f;
    /// 生まれたときの芯の温度 [0,1]。0 なら発光しない煙。
    float temperature = 0.0f;
    float coolingTime = 1.0f;
    float noiseSeedOffset = 0.0f;
};

struct VolumeSourceSettings {
    VolumeFlipbookPreset preset = VolumeFlipbookPreset::RisingPlume;
    std::uint32_t seed = 1;
    /// RisingPlume だけが持つ。frameCount·frameDt 後に場が完全に元へ戻る。
    bool loop = false;
    /// 負ならプリセットの既定 (DefaultVolumeStartTime)。
    float startTime = -1.0f;
    float frameDt = 1.0f / 24.0f;
    int frameCount = 64;
};

/// GPU の cbuffer にそのまま載る 1 puff (float4 x 7)。時間に依存する計算は CPU で済ませる。
struct alignas(16) VolumeFillPuffGpu {
    /// 行 i = (B の行 i, beta_i)。物体座標 y = B x + beta。
    float body[3][4];
    /// 行 i = (M の行 i, m_i)。1 コマぶりの割線速度 v = M x + m [bake 単位/秒]。
    float velocity[3][4];
    /// x = density·envelope, y = 芯の温度, z = noise seed, w = 物体座標の cull 半径
    float params[4];
};

struct alignas(16) VolumeFillConstants {
    std::uint32_t resolution = 64;
    std::uint32_t puffCount = 0;
    float noiseFrequency = 2.2f;
    float noiseAmplitude = 0.7f;
    VolumeFillPuffGpu puffs[kVolumeFillMaxPuffs];
};
static_assert(sizeof(VolumeFillPuffGpu) == 112, "VolumeFill.cs.hlsl の VolumePuffGpu と一致させること");
static_assert(sizeof(VolumeFillConstants) == 16 + kVolumeFillMaxPuffs * 112,
              "VolumeFill.cs.hlsl の cbuffer と一致させること");

struct VolumeSample {
    float density = 0.0f;
    float temperature = 0.0f;
    math::Vector3 velocity;
};

[[nodiscard]] float DefaultVolumeStartTime(VolumeFlipbookPreset preset);
[[nodiscard]] float ResolveVolumeStartTime(const VolumeSourceSettings& settings);

/// ベイクする時間範囲 [start, start + frameCount·frameDt] に関わる puff を列挙する。
[[nodiscard]] std::vector<VolumePuff> BuildVolumePuffs(const VolumeSourceSettings& settings);

/// 時刻 t0 に x にあった物質点の、時刻 t1 での位置。
[[nodiscard]] math::Vector3 PuffFlowMap(const VolumePuff& puff, const math::Vector3& x,
                                        float t0, float t1);

/// 物体座標での密度 (envelope を掛ける前)。|y| > kVolumePuffBodyCullRadius では必ず 0。
[[nodiscard]] float EvaluatePuffBodyDensity(const math::Vector3& bodyPosition, float noiseSeedOffset,
                                            const VolumeNoiseSettings& noise);

/// 時刻 t に生きている puff を詰める。速度は [t, t + frameDt] の割線速度。
/// @ret 詰めた puff 数 (kVolumeFillMaxPuffs を超えた分は捨てる)。
std::uint32_t PackVolumeFillConstants(std::span<const VolumePuff> puffs,
                                      const VolumeNoiseSettings& noise, std::uint32_t resolution,
                                      float time, float frameDt, VolumeFillConstants& out);

/// VolumeFill.cs.hlsl の 1 ボクセルぶんの計算の写し。
[[nodiscard]] VolumeSample SampleVolumeFill(const VolumeFillConstants& constants,
                                            const math::Vector3& x);

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
