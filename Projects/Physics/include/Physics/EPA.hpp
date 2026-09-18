/// @file    EPA.hpp
/// @brief   Expanding Polytope Algorithm (GJK 交差後の貫通深度・法線・接触点計算)。
/// @author  Hasegawa Jin
/// @date    2026-05-24
#pragma once
#include <Physics/GJK.hpp>

namespace fbzz::physics
{

    struct EPAResult
    {
        bool valid = false;

        /// 衝突法線 (正規化済み)。向きは **B から A へ押し戻す方向**で、
        /// ContactPoint::normal と同じ規約。RefineCardinalAxis もこの向きで軸を選ぶ。
        /// 逆に取ると、床の上に乗ったカプセルを床へ押し込む向きに解決してしまう。
        math::Vector3 normal;

        float         depth    = 0.0f;
        math::Vector3 contactA;   ///< shape A 上の接触点
        math::Vector3 contactB;   ///< shape B 上の接触点
    };

    /// GJK で交差確認済みの Simplex から EPA を実行する。
    /// 返した接触点は PhysicsSolver が ContactPoint へ変換し、PGS 解決で使う。
    /// maxIter: 反復上限
    /// tolerance: 収束判定 (新しいサポート点がポリトープ面より tolerance 以内なら収束)
    EPAResult EPA_GetContactInfo(
        const void* shapeA, SupportFn supportA,
        const void* shapeB, SupportFn supportB,
        const Simplex& gjkSimplex,
        int   maxIter   = 64,
        float tolerance = 1e-4f);

} // namespace fbzz::physics
