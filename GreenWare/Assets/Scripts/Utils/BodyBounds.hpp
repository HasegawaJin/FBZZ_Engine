/// @file    BodyBounds.hpp
/// @brief   コライダーから体の上端・下端・当たり半径をワールド単位で測る
/// @author  Hasegawa Jin
/// @date    2026-08-22
///
/// @note 頭上の体力バー・ロックオン枠・接触判定の 3 者が「この敵はどこからどこまでか」を
///       知る必要がある。各自がコライダーを読むと、スケールの掛け方が 1 箇所ずれただけで
///       絵を見ても原因が分からない差になる。
#pragma once

#include <Engine/Scene/ScriptProxy/ScriptColliderProxy.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Math/Vector3.hpp>
#include <algorithm>

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
    Extents out{};
    fbzz::math::Vector3 minimum, maximum;
    if (!fbzz::scene::ScriptColliderProxy::TryGetPrimitiveWorldBounds(&object, minimum, maximum))
        return out;
    out.top      = maximum.y - object.transform.worldPosition.y;
    out.bottom   = minimum.y - object.transform.worldPosition.y;
    /// @note ビームの太さへ足す近似半径。厳密な衝突判定は物理クエリの責務。
    out.radius   = std::max(maximum.x - minimum.x, maximum.z - minimum.z) * 0.5f;
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
    fbzz::math::Vector3 minimum, maximum;
    if (fbzz::scene::ScriptColliderProxy::TryGetPrimitiveWorldBounds(&object, minimum, maximum))
        return (minimum + maximum) * 0.5f;
    fbzz::math::Vector3 center = object.transform.worldPosition;
    center.y += fallbackHeight * 0.5f;
    return center;
}

} // namespace sandbox::bodybounds
