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

        /// @brief B から A を押し戻す単位法線。
        /// @note Minkowski 差 A-B の外向き法線を反転する。軸補正も ContactPoint と同じ B→A 契約を守る。
        math::Vector3 normal;

        float         depth    = 0.0f;
        math::Vector3 contactA;   ///< @brief shape A 上の接触点。
        math::Vector3 contactB;   ///< @brief shape B 上の接触点。
    };

    /// @brief GJK で交差確認済みの Simplex から EPA 接触情報を求める。
    /// @param maxIter 反復上限。
    /// @param tolerance 新しいサポート点とポリトープ面の距離差が、この値未満なら収束する。
    /// @return 単体の頂点不足や実体のある面の消失は valid=false。接触法線は B→A の向き。
    EPAResult EPA_GetContactInfo(
        const void* shapeA, SupportFn supportA,
        const void* shapeB, SupportFn supportB,
        const Simplex& gjkSimplex,
        int   maxIter   = 64,
        float tolerance = 1e-4f);

} /// @note namespace fbzz::physics
