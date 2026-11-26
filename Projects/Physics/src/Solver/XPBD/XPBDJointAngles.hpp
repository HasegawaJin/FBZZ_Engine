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
        math::Quaternion swingRotation; ///< 相対回転のうち、関節フレームの X 軸ねじりを除いた回転。
        math::Vector3 swingRotationVector; ///< 曲げの回転軸へ曲げ角 [rad] を掛けたベクトル。
        float twistAngle; ///< 関節フレームの X 軸まわりの符号付きねじり角 [rad]。
    };

    /// @pre relativeRotation は正規化済みの親フレームから見た子フレームの回転。
    [[nodiscard]] JointAngles MeasureAngles(const math::Quaternion& relativeRotation);
}
