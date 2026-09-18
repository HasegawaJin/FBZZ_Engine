/// @file    XPBDPoseAnchor.cpp
/// @brief   剛体 1 個を目標のワールド姿勢へ引き止める拘束
/// @author  Hasegawa Jin
/// @date    2026-09-03
#include <Physics/XPBDPoseAnchor.hpp>

#include <cmath>

namespace fbzz::physics
{
    void XPBDPoseAnchor::SetStrength(float maxForce, float maxTorque, float sag, float tilt)
    {
        m_finiteStrength = false;
        m_maxForce  = maxForce;
        m_maxTorque = maxTorque;
        /// @note たわみ x でのカは x/α。α = sag / 上限 と置くと «sag だけずれたところで
        ///       上限を出し切る» という、関節のサーボと同じ意味になる。
        m_linearCompliance  = maxForce  > 0.0f ? sag  / maxForce  : 0.0f;
        m_angularCompliance = maxTorque > 0.0f ? tilt / maxTorque : 0.0f;
    }

    void XPBDPoseAnchor::SetFiniteStrength(float maxForce, float maxTorque, float sag, float tilt)
    {
        SetStrength(maxForce, maxTorque, sag, tilt);
        m_finiteStrength = true;
    }

    void XPBDPoseAnchor::ResetLambda()
    {
        m_lambdaLinear  = 0.0f;
        m_lambdaAngular = 0.0f;
    }

    void XPBDPoseAnchor::SolvePosition(float h)
    {
        m_offset    = 0.0f;
        m_saturated = false;
        if (!m_body || !m_enabled) return;

        /// @note 位置は重心で受ける。腕が 0 なので回転は起こさず、«引き寄せる» だけになる。
        const math::Vector3 delta = m_body->GetPosition() - m_targetPosition;
        m_offset = delta.Length();

        const float maxLinear = m_maxForce > 0.0f ? m_maxForce * h * h : 0.0f;
        if (!m_finiteStrength || m_maxForce > 0.0f)
            SolvePositional(m_body, nullptr, math::Vector3::ZERO, math::Vector3::ZERO,
                            delta, m_linearCompliance, h, m_lambdaLinear, maxLinear);

        const math::Quaternion deviation =
            (m_body->GetRotation() * m_targetRotation.Inverse()).Normalized();
        const float maxAngular = m_maxTorque > 0.0f ? m_maxTorque * h * h : 0.0f;
        if (!m_finiteStrength || m_maxTorque > 0.0f)
            SolveAngular(m_body, nullptr, RotationVector(deviation),
                         m_angularCompliance, h, m_lambdaAngular, maxAngular);

        m_saturated =
            (maxLinear  > 0.0f && std::abs(m_lambdaLinear)  >= maxLinear  * 0.9999f) ||
            (maxAngular > 0.0f && std::abs(m_lambdaAngular) >= maxAngular * 0.9999f);
    }
} // namespace fbzz::physics
