/// @file    SupportShapes.hpp
/// @brief   GJK / EPA を単体で回すための、解析解が分かっている支持関数つき形状。
/// @author  Hasegawa Jin
/// @date    2026-09-02
///
/// GJK / EPA は形状を知らず `const void*` と支持関数だけで動く。テストでも Collider を
/// 経由せず、期待値を手で計算できる素の形状を渡す。
/// こうしておくと «GJK が落ちたのか、Collider の Update が落ちたのか» が切り分けられる。
#pragma once

#include <Math/Vector3.hpp>
#include <Physics/GJK.hpp>

namespace fbzz::testkit {

/// 中心と半径だけの球。任意方向の支持点は解析的に求まる。
struct SupportSphere {
    math::Vector3 center;
    float         radius = 1.0f;

    static math::Vector3 Support(const void* shape, const math::Vector3& dir);
};

/// 軸整合の箱。支持点は各軸の符号だけで決まる。
struct SupportBox {
    math::Vector3 center;
    math::Vector3 halfExtents{0.5f, 0.5f, 0.5f};

    static math::Vector3 Support(const void* shape, const math::Vector3& dir);
};

/// 大きさを持たない 1 点。単体が縮退する経路を踏ませるために使う。
struct SupportPoint {
    math::Vector3 position;

    static math::Vector3 Support(const void* shape, const math::Vector3& dir);
};

// SupportFn へ代入できることをここで固定する。署名がずれたらテスト本体ではなく
// この行がコンパイルエラーになるので、原因が一目で分かる。
inline constexpr physics::SupportFn kSphereSupport = &SupportSphere::Support;
inline constexpr physics::SupportFn kBoxSupport    = &SupportBox::Support;
inline constexpr physics::SupportFn kPointSupport  = &SupportPoint::Support;

} // namespace fbzz::testkit
