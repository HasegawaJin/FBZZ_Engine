/// @file    XPBDConstraint.cpp
/// @brief   XPBD の位置・角度補正の共通形
/// @author  Hasegawa Jin
/// @date    2026-09-02
#include <Physics/XPBDConstraint.hpp>

#include <cmath>

namespace fbzz::physics
{
    namespace
    {
        /// 一般化逆質量の和がこれを下回る = 双方が動けない。ここで打ち切らないと
        /// 0 除算で NaN が姿勢へ入り、以降のフレームが全部壊れる。
        constexpr float kMinInverseMass = 1e-9f;
        constexpr float kMinViolation   = 1e-9f;

        void ApplyPositionalImpulse(RigidBody* body,
                                    const math::Vector3& r,
                                    const math::Vector3& p)
        {
            if (!body) return;

            const float invMass = body->GetInvMass();
            if (invMass != 0.0f)
                body->SetPosition(body->GetPosition() + p * invMass);

            const math::Vector3 angular = body->ApplyInvInertia(math::Vector3::Cross(r, p));
            if (angular.LengthSq() > 0.0f)
                body->SetRotation(IntegrateRotation(body->GetRotation(), angular, 1.0f));
        }

        void ApplyAngularImpulse(RigidBody* body, const math::Vector3& p)
        {
            if (!body) return;
            const math::Vector3 angular = body->ApplyInvInertia(p);
            if (angular.LengthSq() > 0.0f)
                body->SetRotation(IntegrateRotation(body->GetRotation(), angular, 1.0f));
        }
    } // namespace

    math::Quaternion IntegrateRotation(const math::Quaternion& q,
                                       const math::Vector3&    angular,
                                       float                   h)
    {
        /// @note q_dot = 0.5 * [ω, 0] * q。RigidBody::Integrate と同じ形にしてある。
        const math::Quaternion spin =
            math::Quaternion{ angular.x, angular.y, angular.z, 0.0f } * q;
        const float s = 0.5f * h;
        return math::Quaternion{
            q.x + spin.x * s,
            q.y + spin.y * s,
            q.z + spin.z * s,
            q.w + spin.w * s
        }.Normalized();
    }

    math::Vector3 AngularVelocityFromDelta(const math::Quaternion& previous,
                                           const math::Quaternion& current,
                                           float                   h)
    {
        if (h <= 0.0f) return math::Vector3::ZERO;

        const math::Quaternion delta = current * previous.Inverse();
        /// @note 同じ姿勢を表す符号違いの 2 つのうち短い方を採る。採らないと «ほぼ 1 回転ぶん»
        ///       の角速度が出て、次の substep で体が弾け飛ぶ。
        const float scale = (delta.w < 0.0f ? -2.0f : 2.0f) / h;
        return math::Vector3{ delta.x, delta.y, delta.z } * scale;
    }

    float GeneralizedInverseMass(const RigidBody*     body,
                                 const math::Vector3& r,
                                 const math::Vector3& n)
    {
        if (!body) return 0.0f;
        const math::Vector3 rn = math::Vector3::Cross(r, n);
        return body->GetInvMass() + math::Vector3::Dot(rn, body->ApplyInvInertia(rn));
    }

    float SolvePositional(RigidBody* a, RigidBody* b,
                          const math::Vector3& rA, const math::Vector3& rB,
                          const math::Vector3& correction,
                          float compliance, float h, float& lambda,
                          float maxLambda)
    {
        const float violation = correction.Length();
        if (violation <= kMinViolation || h <= 0.0f) return 0.0f;

        const math::Vector3 n = correction * (1.0f / violation);
        const float wSum = GeneralizedInverseMass(a, rA, n) + GeneralizedInverseMass(b, rB, n);
        if (wSum <= kMinInverseMass) return 0.0f;

        const float alphaTilde = compliance / (h * h);
        float deltaLambda = (-violation - alphaTilde * lambda) / (wSum + alphaTilde);

        /// @note SolveAngular と同じく «蓄積した λ» に掛ける。摩擦は λ が上限に当たった時点で
        ///       滑り出す ─ 差分に掛けると substep が細かいほど強い摩擦になってしまう。
        if (maxLambda > 0.0f) {
            float clamped = lambda + deltaLambda;
            clamped = clamped >  maxLambda ?  maxLambda : clamped;
            clamped = clamped < -maxLambda ? -maxLambda : clamped;
            deltaLambda = clamped - lambda;
        }
        lambda += deltaLambda;
        if (deltaLambda == 0.0f) return 0.0f;

        const math::Vector3 p = n * deltaLambda;
        ApplyPositionalImpulse(a, rA, p);
        ApplyPositionalImpulse(b, rB, -p);
        return deltaLambda;
    }

    float SolveAngular(RigidBody* a, RigidBody* b,
                       const math::Vector3& correction,
                       float compliance, float h, float& lambda,
                       float maxLambda)
    {
        const float violation = correction.Length();
        if (violation <= kMinViolation || h <= 0.0f) return 0.0f;

        const math::Vector3 n = correction * (1.0f / violation);
        const float wA = a ? math::Vector3::Dot(n, a->ApplyInvInertia(n)) : 0.0f;
        const float wB = b ? math::Vector3::Dot(n, b->ApplyInvInertia(n)) : 0.0f;
        const float wSum = wA + wB;
        if (wSum <= kMinInverseMass) return 0.0f;

        const float alphaTilde = compliance / (h * h);
        float deltaLambda = (-violation - alphaTilde * lambda) / (wSum + alphaTilde);

        /// @note 上限は «加えた分» ではなく «蓄積した λ» に掛ける。λ/h² がその substep で
        ///       関節が出しているトルクそのものなので、ここを切ると出力が頭打ちになる。
        if (maxLambda > 0.0f) {
            float clamped = lambda + deltaLambda;
            clamped = clamped >  maxLambda ?  maxLambda : clamped;
            clamped = clamped < -maxLambda ? -maxLambda : clamped;
            deltaLambda = clamped - lambda;
        }
        lambda += deltaLambda;
        if (deltaLambda == 0.0f) return 0.0f;

        const math::Vector3 p = n * deltaLambda;
        ApplyAngularImpulse(a, p);
        ApplyAngularImpulse(b, -p);
        return deltaLambda;
    }

    void ApplyAngularVelocityChange(RigidBody* a, RigidBody* b,
                                    const math::Vector3& deltaOmega,
                                    float maxImpulse)
    {
        const float magnitude = deltaOmega.Length();
        if (magnitude <= kMinViolation) return;

        const math::Vector3 n = deltaOmega * (1.0f / magnitude);
        const float wA = a ? math::Vector3::Dot(n, a->ApplyInvInertia(n)) : 0.0f;
        const float wB = b ? math::Vector3::Dot(n, b->ApplyInvInertia(n)) : 0.0f;
        const float wSum = wA + wB;
        if (wSum <= kMinInverseMass) return;

        float impulse = magnitude / wSum;
        if (maxImpulse > 0.0f && impulse > maxImpulse) impulse = maxImpulse;

        const math::Vector3 p = n * impulse;
        if (a) a->SetAngularVelocity(a->GetAngularVelocity() - a->ApplyInvInertia(p));
        if (b) b->SetAngularVelocity(b->GetAngularVelocity() + b->ApplyInvInertia(p));
    }

    void ApplyVelocityChangeAtPoint(RigidBody* a, RigidBody* b,
                                    const math::Vector3& rA, const math::Vector3& rB,
                                    const math::Vector3& deltaV,
                                    float maxImpulse)
    {
        const float magnitude = deltaV.Length();
        if (magnitude <= kMinViolation) return;

        const math::Vector3 n = deltaV * (1.0f / magnitude);
        const float wSum = GeneralizedInverseMass(a, rA, n) + GeneralizedInverseMass(b, rB, n);
        if (wSum <= kMinInverseMass) return;

        float impulse = magnitude / wSum;
        if (maxImpulse > 0.0f && impulse > maxImpulse) impulse = maxImpulse;

        const math::Vector3 p = n * impulse;
        if (a) {
            a->SetVelocity(a->GetVelocity() - p * a->GetInvMass());
            a->SetAngularVelocity(a->GetAngularVelocity() -
                                  a->ApplyInvInertia(math::Vector3::Cross(rA, p)));
        }
        if (b) {
            b->SetVelocity(b->GetVelocity() + p * b->GetInvMass());
            b->SetAngularVelocity(b->GetAngularVelocity() +
                                  b->ApplyInvInertia(math::Vector3::Cross(rB, p)));
        }
    }

    math::Vector3 RotationVector(const math::Quaternion& q)
    {
        math::Vector3 v{ q.x, q.y, q.z };
        float         w = q.w;
        /// @note 同じ姿勢を表す符号違いのうち短い方。採らないと «ほぼ 1 回転» を返す。
        if (w < 0.0f) { v = -v; w = -w; }

        const float length = v.Length();
        /// @note 小角では 2v が回転ベクトル
        if (length <= kMinViolation) return v * 2.0f;
        return v * (2.0f * std::atan2(length, w) / length);
    }

    void DecomposeSwingTwist(const math::Quaternion& q,
                             math::Quaternion&       swing,
                             math::Quaternion&       twist)
    {
        /// @note ツイスト軸は関節フレームの X。q の X 成分と実部だけを残せばツイストになる。
        const float length = std::sqrt(q.x * q.x + q.w * q.w);
        /// @note 特異点: スイングが 180° に近いとツイスト成分が消え、向きが決められない。
        ///       ここを 0 除算で通すと NaN が姿勢へ入って以降のフレームが全部壊れる。
        twist = length <= kMinViolation
            ? math::Quaternion::Identity()
            : math::Quaternion{ q.x / length, 0.0f, 0.0f, q.w / length };
        swing = q * twist.Conjugate();
    }
} // namespace fbzz::physics
