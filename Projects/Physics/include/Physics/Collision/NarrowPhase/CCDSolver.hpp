/// @file    CCDSolver.hpp
/// @brief   Continuous Collision Detection (高速移動物体のトンネリング防止)。
/// @author  Hasegawa Jin
/// @date    2026-05-24

/// @note 適用対象: SphereCollider を持つ物体のみ (m_useCCD == true)
#pragma once
#include <Physics/Api.hpp>
#include <Math/LinearAlgebra/Vector3.hpp>

namespace fbzz::physics
{

    class RigidBody;

    struct CCDResult
    {
        bool          hit          = false; ///< 指定時間内に移動する形状が相手へ達したかどうか。
        float         toi          = 1.0f;   ///< @note 衝突までの時間を入力の dt に対する割合 [0, 1] で表す。
        math::Vector3 normal;                ///< @note 相手から移動球へ向くワールド空間の法線 (hit 時に有効)。
        math::Vector3 contactPoint;          ///< @note 相手の表面上のワールド座標 (hit 時に有効)。
    };

    class CCDSolver
    {
    public:
        /// @note 速度が radius * CCD_THRESHOLD / dt を超える場合に CCD を適用する。
        /// @note 小さい値ほど安全だが、CCDPhase の対象が増えて負荷が上がる。
        static constexpr float CCD_THRESHOLD = 0.5f; ///< 連続判定を有効にする相対移動量のしきい値の倍率。

        /// @brief 移動球 A と静止球 B の最初の球面交差を求める。
        /// @param[in] velA ワールド空間の速度。B も移動する場合は B に対する相対速度。
        /// @param[in] dt 判定する時間幅 (秒)。
        /// @note 開始時に重なっている場合、区間内にある離脱の根を返す。
        FBZZ_PHYSICS_API static CCDResult SweptSphereSphere(
            const math::Vector3& centerA, float radiusA,
            const math::Vector3& velA,
            const math::Vector3& centerB, float radiusB,
            float dt);

        /// @brief 移動球が法線側から静止平面に触れる時刻を求める。
        /// @param[in] planeNormal 正規化したワールド空間の法線。
        /// @param[in] planeD 平面の式 dot(planeNormal, p) = planeD の右辺。
        FBZZ_PHYSICS_API static CCDResult SweptSpherePlane(
            const math::Vector3& center, float radius,
            const math::Vector3& vel,
            const math::Vector3& planeNormal, float planeD,
            float dt);

        /// @note body の速度が CCD を適用するほど速いか判定する
        FBZZ_PHYSICS_API static bool NeedsCCD(const RigidBody& body, float radius, float dt);
    };

}
