// FBZZ Engine
// MathConvert.hpp | fbzz::editor
// engine math 型 ↔ ImGui 型 のインライン変換
#pragma once
#include <imgui.h>
#include <math/Vector2.hpp>
#include <math/Vector3.hpp>
#include <math/Vector4.hpp>

namespace fbzz::editor {

inline ImVec2 ToImGui(const math::Vector2& v)  { return { v.x, v.y }; }
inline ImVec4 ToImGui(const math::Vector4& v)  { return { v.x, v.y, v.z, v.w }; }
inline ImVec4 ToImGui4(const math::Vector3& v) { return { v.x, v.y, v.z, 1.0f }; }

inline math::Vector2 FromImGui(const ImVec2& v) { return { v.x, v.y }; }
inline math::Vector4 FromImGui(const ImVec4& v) { return { v.x, v.y, v.z, v.w }; }

} // namespace fbzz::editor
