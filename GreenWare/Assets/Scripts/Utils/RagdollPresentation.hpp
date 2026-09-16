/// @file    RagdollPresentation.hpp
/// @brief   GreenWareの立位リアクション設定と並進・回転の演出配分。
/// @author  Hasegawa Jin
/// @date    2026-09-13
#pragma once
#include <Engine/Scene/ScriptProxy/ScriptRagdollProxy.hpp>
#include <Math/MathUtils.hpp>

namespace sandbox {

inline void ConfigureStandingReaction(const fbzz::scene::ScriptRagdollProxy& ragdoll,
                                       fbzz::scene::ScriptRagdollProfile profile,
                                       float distance, float degrees,
                                       float support, float supportTiltRadians)
{
    ragdoll.SetProfile(profile);
    ragdoll.SetStandingGuard(true, distance, degrees);
    ragdoll.SetRootAnchor(support, 0.04f, supportTiltRadians * 180.0f / fbzz::math::PI);
    ragdoll.SetGravity(9.8f);
    ragdoll.SetContacts(true, true, false, false);
    ragdoll.SetGroundPlane(false);
    ragdoll.SetCollapse(0.0f);
}

/// 回転の軸・倍率・上限はこのゲームの演出として決める。
inline void PushRagdollReaction(const fbzz::scene::ScriptRagdollProxy& ragdoll,
                                const fbzz::math::Vector3& velocity, float angularScale,
                                const fbzz::math::Vector3& origin = fbzz::math::Vector3::ZERO,
                                float radius = 0.0f)
{
    auto angular = fbzz::math::Vector3::Cross(fbzz::math::Vector3::UP, velocity) * angularScale;
    const float speed = angular.Length();
    if (speed > 8.0f) angular = angular * (8.0f / speed);
    ragdoll.PushAt(origin, velocity, radius);
    ragdoll.PushAngularAt(origin, angular, radius);
}

} // namespace sandbox
