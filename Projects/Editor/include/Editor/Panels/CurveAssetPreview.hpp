/// @file    CurveAssetPreview.hpp
/// @brief   曲線と色グラデーションの読み取り専用プレビュー。
/// @author  Hasegawa Jin
/// @date    2026-09-26
#pragma once
#include <Engine/Scene/ParticleCurve.hpp>
#include <imgui.h>

namespace fbzz::editor::curvepreview {

/// @brief Evaluate の結果を描き、補間モードと保存値に一致させる。
void DrawCurve(ImDrawList* draw, const scene::ParticleCurve& curve, ImVec2 origin, ImVec2 size);

/// @brief オーサリング色を透明度が見える市松模様の上に描く。
void DrawGradient(ImDrawList* draw, const scene::ParticleGradient& gradient, ImVec2 origin, ImVec2 size);

} /// @note namespace fbzz::editor::curvepreview
