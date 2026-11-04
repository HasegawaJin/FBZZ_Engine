/// @file    ScriptPhysicsProxyHelpers.hpp
/// @brief   Physics と Joint が共有する非所有の剛体解決。
/// @author  Hasegawa Jin
/// @date    2026-10-31
#pragma once
#include "ScriptProxyCommon.hpp"
#include <Engine/Scene/Components/RigidBodyComponent.hpp>
#include <Physics/RigidBody.hpp>
namespace fbzz::scene::script_proxy_detail {
inline physics::RigidBody* SelfRigidBody(const Script* script)
{
    auto* component = SelfComponent<RigidBodyComponent>(script);
    return component && component->enabled ? component->rigidBody.get() : nullptr;
}
} /// @note namespace fbzz::scene::script_proxy_detail
