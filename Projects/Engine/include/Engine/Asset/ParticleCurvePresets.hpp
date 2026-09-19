/// @file    ParticleCurvePresets.hpp
/// @brief   ParticleCurve の名前付きプリセット。Editor の UI と AI のオーサリング面で共有する。
/// @author  Hasegawa Jin
/// @date    2026-08-12
///
/// @note 「AAA に見えるか」を決めるのは色や粒子数より時間曲線の形。AI に生の (time, value) を
///       書かせても意図した形になる保証が無く失敗の切り分けもできないため、名前付き語彙にして
///       AI は "Spike" 等を選ぶだけで正しい形から始め、微調整だけを画像評価に任せる。Editor の
///       匿名 namespace ではなく Engine に置くのは、AI 側からも参照できる 1 定義にするため。
#pragma once

#include <Engine/Scene/Components/ParticleEmitter.hpp>
#include <cstdint>
#include <span>
#include <string_view>

namespace fbzz::asset {

/// 値は 0..1 の正規化で持ち、適用時に対象フィールドの最大値へ伸ばす。
/// @note 同じ「立ち上がって落ちる」形を size(0..1) と velocity(0..10) の双方へ使えるようにする。
struct ParticleCurvePreset {
    std::string_view name;
    std::string_view description; ///< AI がプリセットを選ぶための意味説明
    scene::ParticleCurveInterpolation interpolation;
    std::uint32_t keyCount;
    float keys[scene::kMaxParticleCurveKeys][2]; ///< {time, normalizedValue}
};

[[nodiscard]] inline std::span<const ParticleCurvePreset> ParticleCurvePresets()
{
    static constexpr ParticleCurvePreset kPresets[] = {
        { "Ramp Up", "0 から最大へ直線的に増える", scene::ParticleCurveInterpolation::Linear,
          2, {{0,0},{1,1}} },
        { "Ramp Down", "最大から 0 へ直線的に減る", scene::ParticleCurveInterpolation::Linear,
          2, {{0,1},{1,0}} },
        { "Constant 1", "常に最大。カーブを一時的に無効化したいとき",
          scene::ParticleCurveInterpolation::Linear, 2, {{0,1},{1,1}} },
        { "Ease In", "ゆっくり始まって加速する。溜めのある動き",
          scene::ParticleCurveInterpolation::Linear, 4, {{0,0},{0.4f,0.08f},{0.75f,0.42f},{1,1}} },
        { "Ease Out", "速く始まって減速する。着地・減衰の基本形",
          scene::ParticleCurveInterpolation::Linear, 4, {{0,0},{0.25f,0.58f},{0.6f,0.92f},{1,1}} },
        { "Ease In-Out", "両端で速度 0。機械的な折れ線に見えない",
          scene::ParticleCurveInterpolation::Smooth, 2, {{0,0},{1,1}} },
        { "Spike", "一瞬で立ち上がりすぐ落ちて余韻を引く。爆発の閃光・ヒットフラッシュ",
          scene::ParticleCurveInterpolation::Linear, 4, {{0,0},{0.06f,1},{0.35f,0.22f},{1,0}} },
        { "Breathe", "ゆっくりした呼吸。ループ端を先頭と同値にして継ぎ目が飛ばない。炎の光量",
          scene::ParticleCurveInterpolation::Smooth, 4,
          {{0,0.75f},{0.35f,1},{0.7f,0.62f},{1,0.75f}} },
        { "Blink", "中間値を取らない点滅。Step 補間。警告灯・明滅する魔法陣",
          scene::ParticleCurveInterpolation::Step, 4, {{0,1},{0.25f,0},{0.5f,1},{0.75f,0}} },
        { "Grow And Settle", "大きく出て少し戻り落ち着く。衝撃波・膨張シェル",
          scene::ParticleCurveInterpolation::Smooth, 4, {{0,0},{0.3f,1},{0.55f,0.82f},{1,0.9f}} },
        { "Late Fade", "終盤まで保ってから一気に消える。煙の消え際",
          scene::ParticleCurveInterpolation::Smooth, 4, {{0,1},{0.6f,0.92f},{0.85f,0.45f},{1,0}} },
    };
    return kPresets;
}

[[nodiscard]] inline const ParticleCurvePreset* FindParticleCurvePreset(std::string_view name)
{
    for (const ParticleCurvePreset& preset : ParticleCurvePresets())
        if (preset.name == name) return &preset;
    return nullptr;
}

/// maxValue: 対象フィールドの実用上限 (size カーブなら 1.0、velocity なら 10.0 など)。
inline void ApplyParticleCurvePreset(scene::ParticleCurve& curve,
                                     const ParticleCurvePreset& preset,
                                     float maxValue)
{
    const std::uint32_t count = preset.keyCount < 2 ? 2u
        : (preset.keyCount > scene::kMaxParticleCurveKeys ? scene::kMaxParticleCurveKeys
                                                          : preset.keyCount);
    curve.keyCount = count;
    for (std::uint32_t index = 0; index < count; ++index) {
        curve.keys[index].time  = preset.keys[index][0];
        curve.keys[index].value = preset.keys[index][1] * maxValue;
    }
    curve.interpolation = preset.interpolation;
}

} // namespace fbzz::asset
