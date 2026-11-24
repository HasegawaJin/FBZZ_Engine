/// @file    ScriptControlPointProxyHelpers.hpp
/// @brief   Scene と Navigation が共有する制御点の読取りモード。
/// @author  Hasegawa Jin
/// @date    2026-10-31
#pragma once
#include <Engine/Scene/Scripting/Script.hpp>
#include <Engine/Scene/ControlPoints/ControlPointModes.hpp>
namespace fbzz::scene::script_proxy_detail {
inline ControlPointReadContext ScriptControlPointReadContext()
{
    return Script::IsInPlayMode() ? ControlPointReadContext::RUNTIME
                                  : ControlPointReadContext::AUTHORING;
}
} /// @note namespace fbzz::scene::script_proxy_detail
