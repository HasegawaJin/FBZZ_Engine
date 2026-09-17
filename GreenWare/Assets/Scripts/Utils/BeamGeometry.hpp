/// @file    BeamGeometry.hpp
/// @brief   ビームの当たりを測る純関数群
/// @author  Hasegawa Jin
/// @date    2026-08-23
///
/// @note ビームの太さ (0.6m) は企画書 6.4 の «判断ミスで痛む» を支える値で、体験の質に
///       直結し何度も調整される。副作用の無い関数にすれば数値だけ見て調整できる。
///       物理の SphereCast は最初の 1 件しか返さず、6.2 の «貫通し線上の敵すべてに
///       判定が乗る» を満たせない。対象は高々 8 体 (10.3 の同時出現上限) なので、
///       線分との距離を直接測る方が単純で速い。
#pragma once

#include <Math/MathUtils.hpp>
#include <Math/Segment.hpp>
#include <Math/Vector3.hpp>
#include <cmath>

namespace sandbox::beamgeom {

using fbzz::math::Vector3;

/// 線分 from→to と点の最短距離。outAlong には from からの距離 (m) を返す。
[[nodiscard]] inline float DistanceToSegment(const Vector3& point,
                                             const Vector3& from,
                                             const Vector3& to,
                                             float& outAlong)
{
    return fbzz::math::DistanceToSegment(point, from, to, outAlong);
}

/// ビームが体に触れているか。
///
/// @note 体の半径を足す。ビームの太さだけで判定すると、大きい敵ほど「見た目はど真ん中を
///       貫いているのに塗れない」が起きるため、常に「線の太さ ＋ 相手の太さ」で決める。
[[nodiscard]] inline bool Touches(float distanceToAxis, float beamRadius, float bodyRadius)
{
    return distanceToAxis <= beamRadius + fbzz::math::Max(bodyRadius, 0.0f);
}

} // namespace sandbox::beamgeom
