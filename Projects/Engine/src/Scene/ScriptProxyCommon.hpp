/// @file    ScriptProxyCommon.hpp
/// @brief   複数領域が共有する非所有の Script・Component 解決。
/// @author  Hasegawa Jin
/// @date    2026-10-31
#pragma once
#include <Engine/Scene/Script.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
namespace fbzz::scene::script_proxy_detail {

inline constexpr float DEG_TO_RAD = 3.14159265358979323846f / 180.0f;

template<typename T>
T* SelfComponent(const Script* script)
{
    return script ? script->GetComponent<T>() : nullptr;
}

template<typename T>
T* ObjectComponent(GameObject* go)
{
    return go && go->IsValid() ? go->GetComponent<T>() : nullptr;
}

} /// @note namespace fbzz::scene::script_proxy_detail
