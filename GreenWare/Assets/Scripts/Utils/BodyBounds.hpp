/// @file BodyBounds.hpp
/// @brief コライダーから体の上端・下端・当たり半径をワールド単位で測る
/// @author Hasegawa Jin
/// @date 2026-08-22
///
/// WHY 1 箇所へ集めるか:
///   頭上の体力バー・ロックオン枠・接触判定の 3 者が「この敵はどこからどこまでか」を
///   知る必要がある。各自がコライダーを読むと、スケールの掛け方が 1 箇所ずれただけで
///   「バーだけ頭にめり込む」「枠だけ小さい」という、絵を見ても原因が分からない差になる。
///
/// WHY スケールを自分で掛けるか:
///   Inspector の radius / size は「スケールを掛ける前の寸法」で、実際の当たり判定は
///   ColliderSync が worldScale を掛けたもの。ここは ColliderSync と同じ規則
///   (球は最大軸 / カプセルは半径 = max(x,z)・半長 = y) を写している。
///   規則を変えるときは両方を同時に直すこと。
#pragma once

#include <Engine/Scene/Components/ColliderComponent.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Math/Vector3.hpp>
#include <algorithm>
#include <cmath>

namespace sandbox::bodybounds {

/// コライダーから測った体の寸法。すべてオブジェクト原点からのワールド距離。
struct Extents {
    float top    = 0.0f; ///< 原点から上端まで
    float bottom = 0.0f; ///< 原点から下端まで (足元が原点なら概ね 0)
    float radius = 0.0f; ///< 水平方向の当たり半径
    /// コライダーが見つかって実測できたか。false のとき他のメンバーは 0。
    bool  measured = false;
};

/// 体の寸法を測る。プリミティブコライダーが無ければ measured = false。
[[nodiscard]] inline Extents Of(fbzz::scene::GameObject& object)
{
    using namespace fbzz::scene;
    const fbzz::math::Vector3& s = object.transform.worldScale;
    const float sx = std::abs(s.x), sy = std::abs(s.y), sz = std::abs(s.z);

    Extents out{};
    float centerY = 0.0f;
    float halfY   = 0.0f;

    if (const auto* sphere = object.GetComponent<SphereColliderComponent>()) {
        out.radius = sphere->radius * std::max({ sx, sy, sz });
        halfY      = out.radius;
        centerY    = sphere->center.y * sy;
    } else if (const auto* capsule = object.GetComponent<CapsuleColliderComponent>()) {
        out.radius = capsule->radius * std::max(sx, sz);
        halfY      = capsule->halfHeight * sy + out.radius;
        centerY    = capsule->center.y * sy;
    } else if (const auto* box = object.GetComponent<BoxColliderComponent>()) {
        out.radius = std::max(box->size.x * sx, box->size.z * sz) * 0.5f;
        halfY      = box->size.y * 0.5f * sy;
        centerY    = box->center.y * sy;
    } else {
        return out;
    }

    out.top      = centerY + halfY;
    out.bottom   = centerY - halfY;
    out.measured = true;
    return out;
}

/// 体の上端。測れなければ fallbackTop をそのまま返す (スケールは掛けない)。
[[nodiscard]] inline float TopWorld(fbzz::scene::GameObject& object, float fallbackTop)
{
    const Extents extents = Of(object);
    return extents.measured ? extents.top : fallbackTop;
}

/// 水平方向の当たり半径。測れなければ 0。
[[nodiscard]] inline float RadiusWorld(fbzz::scene::GameObject& object)
{
    return Of(object).radius;
}

/// 胴体の中心。UI をぶら下げる基準点として使う。
[[nodiscard]] inline fbzz::math::Vector3 CenterWorld(fbzz::scene::GameObject& object,
                                                     float fallbackHeight)
{
    const Extents extents = Of(object);
    fbzz::math::Vector3 center = object.transform.worldPosition;
    center.y += extents.measured ? (extents.top + extents.bottom) * 0.5f
                                 : fallbackHeight * 0.5f;
    return center;
}

} // namespace sandbox::bodybounds
