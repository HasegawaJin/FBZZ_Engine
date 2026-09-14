/// @file    SixWayLighting.hpp
/// @brief   6 方向ライトマップ (Six-way lighting) の重み — ParticleCommon.hlsli の SixWayResponse の写し
/// @author  Hasegawa Jin
/// @date    2026-09-11
///
/// 規約 (VFX Graph と同じ並び):
///   Positive map = (右, 上, 奥, α) / Negative map = (左, 下, 手前, 発光マスク)
///   値は «その向きから光が来たときの明るさ» (モノクロ・リニア・ストレート)。
/// 光の向きは «テクスチャの軸» で表す (x = 右, y = 上, z = 奥 = 画面の向こう)。
///
/// WHY C++ に写しを置くか: 焼く側 (VolumeRaymarch.hlsl) も読む側 (ParticleCommon.hlsli) も HLSL で、
///     並びや符号がずれてもエラーにならない。式をここで固定してテストで守る
///     (FlipbookMotionVectorEncoding と同じ流儀)。
#pragma once

#include <Math/Vector3.hpp>

namespace fbzz::asset {

struct SixWayWeights {
    math::Vector3 positive; ///< (右, 上, 奥) の重み
    math::Vector3 negative; ///< (左, 下, 手前) の重み
};

/// 光源への向きの各成分を 2 乗して正負へ振り分ける。単位ベクトルなら 6 つの和は 1。
[[nodiscard]] inline SixWayWeights ComputeSixWayWeights(const math::Vector3& toLight)
{
    const auto positive = [](float v) { return v > 0.0f ? v * v : 0.0f; };
    const auto negative = [](float v) { return v < 0.0f ? v * v : 0.0f; };
    return { { positive(toLight.x), positive(toLight.y), positive(toLight.z) },
             { negative(toLight.x), negative(toLight.y), negative(toLight.z) } };
}

/// 6 方向マップの応答 (HLSL の SixWayResponse)。
[[nodiscard]] inline float EvaluateSixWay(const math::Vector3& positiveMap, const math::Vector3& negativeMap,
                                          const math::Vector3& toLight)
{
    const SixWayWeights weights = ComputeSixWayWeights(toLight);
    return math::Vector3::Dot(weights.positive, positiveMap) + math::Vector3::Dot(weights.negative, negativeMap);
}

/// 全方向から一様に来る光 (環境光) への応答。6 方向の平均 (HLSL の SixWayAmbient)。
[[nodiscard]] inline float EvaluateSixWayAmbient(const math::Vector3& positiveMap, const math::Vector3& negativeMap)
{
    return (positiveMap.x + positiveMap.y + positiveMap.z + negativeMap.x + negativeMap.y + negativeMap.z) / 6.0f;
}

} // namespace fbzz::asset
