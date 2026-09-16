/// @file    MathConvert.hpp
/// @brief   engine math 型 ↔ ImGui 型 のインライン変換。
/// @author  Hasegawa Jin
/// @date    2026-05-21
#pragma once
#include <imgui.h>
#include <Math/Vector2.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>

namespace fbzz::editor {

inline ImVec2 ToImGui(const math::Vector2& v)  { return { v.x, v.y }; }
inline ImVec4 ToImGui(const math::Vector4& v)  { return { v.x, v.y, v.z, v.w }; }
inline ImVec4 ToImGui4(const math::Vector3& v) { return { v.x, v.y, v.z, 1.0f }; }

inline math::Vector2 FromImGui(const ImVec2& v) { return { v.x, v.y }; }
inline math::Vector4 FromImGui(const ImVec4& v) { return { v.x, v.y, v.z, v.w }; }

} // namespace fbzz::editor
