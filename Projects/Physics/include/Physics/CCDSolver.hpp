/// @file    CCDSolver.hpp
/// @brief   Continuous Collision Detection (高速移動物体のトンネリング防止)。
/// @author  Hasegawa Jin
/// @date    2026-05-24

/// @note 適用対象: SphereCollider を持つ物体のみ (m_useCCD == true)
#pragma once
#include <Physics/Api.hpp>
#include <Math/Vector3.hpp>

namespace fbzz::physics
{

    class RigidBody;

    struct CCDResult
    {
        bool          hit          = false;
        float         toi          = 1.0f;   ///< @note Time of Impact [0, 1]: 0=現在, 1=次フレーム
        math::Vector3 normal;                ///< @note 衝突法線 (hit 時のみ有効)
        math::Vector3 contactPoint;
    };

    class CCDSolver
    {
    public:
        /// @note 速度が radius * CCD_THRESHOLD / dt を超える場合に CCD を適用する。
        /// @note 小さい値ほど安全だが、CCDPhase の対象が増えて負荷が上がる。
        static constexpr float CCD_THRESHOLD = 0.5f;

        /// @note 移動球 A (中心 centerA, 速度 velA) と静止球 B の線形 TOI を計算する
        FBZZ_PHYSICS_API static CCDResult SweptSphereSphere(
            const math::Vector3& centerA, float radiusA,
            const math::Vector3& velA,
            const math::Vector3& centerB, float radiusB,
            float dt);

        /// @note 移動球 A と静止平面 (法線 planeNormal, 原点からの距離 planeD) の TOI を計算する
        FBZZ_PHYSICS_API static CCDResult SweptSpherePlane(
            const math::Vector3& center, float radius,
            const math::Vector3& vel,
            const math::Vector3& planeNormal, float planeD,
            float dt);

        /// @note body の速度が CCD を適用するほど速いか判定する
        FBZZ_PHYSICS_API static bool NeedsCCD(const RigidBody& body, float radius, float dt);
    };

} /// @note namespace fbzz::physics
