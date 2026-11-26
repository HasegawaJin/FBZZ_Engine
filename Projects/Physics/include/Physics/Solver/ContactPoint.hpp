/// @file    ContactPoint.hpp
/// @brief   NarrowPhase が生成する衝突接触点データ。
/// @author  Hasegawa Jin
/// @date    2026-05-21
#pragma once
#include <Math/LinearAlgebra/Vector3.hpp>

namespace fbzz::physics 
{

    class RigidBody;
    class Collider;
    struct PhysicsMaterial;

    /// @note 剛体・コライダー・材質への参照は非所有。World の現在の登録内容に対して使う。
    struct ContactPoint {
        math::Vector3 point;   ///< @note 接触点のワールド座標。
        math::Vector3 normal;  ///< @note B から A へ押し戻す向きのワールド空間の法線。
        float         depth;   ///< @note 貫通の深さ。正の値が貫通を表す。
        RigidBody*    bodyA = nullptr; ///< ペアの 1 番目の形状に紐づく剛体。非所有。
        RigidBody*    bodyB = nullptr; ///< ペアの 2 番目の形状に紐づく剛体。非所有。
        const Collider* colliderA = nullptr; ///< ペアの 1 番目のコライダー。非所有。
        const Collider* colliderB = nullptr; ///< ペアの 2 番目のコライダー。非所有。
        const PhysicsMaterial* materialA = nullptr; ///< 1 番目のコライダーの接触材質。非所有。
        const PhysicsMaterial* materialB = nullptr; ///< 2 番目のコライダーの接触材質。非所有。
        bool isTrigger = false; ///< 重なりを通知するだけで、接触解決の対象外かどうか。
        bool cacheImpulse = true; ///< 解決した累積力積を次フレームへ引き継ぐかどうか。

        math::Vector3 tangent[2]; ///< 接触面の摩擦基底。WarmStart と Resolve の前に接触法線から組み立てる。
        float cachedNormalImpulse       = 0.0f;      ///< @note 蓄積法線インパルスは 0 以上に制限する。
        float cachedTangentImpulse[2]   = {0.0f, 0.0f}; ///< @note 現在の摩擦基底での蓄積インパルス。
        float positionCorrectionWeight  = 1.0f; ///< 同じ形状ペアの接触点間で押し戻し量を分配する重み。
    };

}
