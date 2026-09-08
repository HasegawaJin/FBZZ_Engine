/// @file    BeamGeometry.hpp
/// @brief   ビームの当たりを測る純関数群
/// @author  Hasegawa Jin
/// @date    2026-08-23
///
/// WHY 純関数に切り出すか:
///   企画書 6.4 は「痛くあるべきなのはエイムミスではなく判断ミス」と書き、その代替として
///   ビーム自体に太さ (0.6m) を持たせている。つまりこの太さの扱いが体験の質に直結し、
///   必ず何度も触ることになる。副作用の無い関数にしておけば、数値だけを見て調整でき、
///   周りのスクリプトを読み直さずに済む。
///
/// WHY 物理の SphereCast を使わないか:
///   6.2 の「貫通する。線上の敵すべてに判定が乗る」を満たすには、最初のヒットで
///   止まらない掃引が要る。ScriptPhysicsProxy が公開しているのは最初の 1 件を返す
///   SphereCast だけで、掃引の全件版が無い。極性を帯びられる対象はシーン全体でも
///   高々 8 体 (10.3 の同時出現上限) なので、線分との距離を直接測る方が単純で速い。
#pragma once

#include <Math/MathUtils.hpp>
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
    const Vector3 segment  = to - from;
    const float   lengthSq = segment.LengthSq();
    if (lengthSq <= fbzz::math::EPSILON) {
        outAlong = 0.0f;
        return (point - from).Length();
    }

    const float t = fbzz::math::Clamp01(Vector3::Dot(point - from, segment) / lengthSq);
    outAlong = t * std::sqrt(lengthSq);
    return (point - (from + segment * t)).Length();
}

/// ビームが体に触れているか。
///
/// WHY 体の半径を足すか: ビームの太さだけで判定すると、大きい敵ほど「見た目は
///     ど真ん中を貫いているのに塗れない」が起きる。当たり判定は常に
///     「線の太さ ＋ 相手の太さ」で決まる。
[[nodiscard]] inline bool Touches(float distanceToAxis, float beamRadius, float bodyRadius)
{
    return distanceToAxis <= beamRadius + fbzz::math::Max(bodyRadius, 0.0f);
}

} // namespace sandbox::beamgeom
