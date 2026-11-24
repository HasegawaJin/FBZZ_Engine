/// @file    XPBDJointAngles.hpp
/// @brief   関節制限の測定と速度射影で共有する角度。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#pragma once
#include <Math/LinearAlgebra/Quaternion.hpp>

namespace fbzz::physics::joint_detail
{
    struct JointAngles
    {
        math::Quaternion swingRotation;
        math::Vector3 swingRotationVector;
        float twistAngle;
    };

    /// @pre relativeRotation は正規化済みの親フレームから見た子フレームの回転。
    [[nodiscard]] JointAngles MeasureAngles(const math::Quaternion& relativeRotation);
}
