/// @file    ScriptControlPointProxyHelpers.hpp
/// @brief   Scene と Navigation が共有する制御点の読取りモード。
/// @author  Hasegawa Jin
/// @date    2026-10-31
#pragma once
#include <Engine/Scene/Script.hpp>
#include <Engine/Scene/ControlPointModes.hpp>
namespace fbzz::scene::script_proxy_detail {
inline ControlPointReadContext ScriptControlPointReadContext()
{
    return Script::IsInPlayMode() ? ControlPointReadContext::RUNTIME
                                  : ControlPointReadContext::AUTHORING;
}
} /// @note namespace fbzz::scene::script_proxy_detail
