/// @file    InspectorBoneBake.hpp
/// @brief   Baked animation settings and fallback diagnostics in the Animator inspector.
/// @author  Hasegawa Jin
/// @date    2026-10-12
#pragma once
namespace fbzz::scene { struct AnimatorComponent; }
namespace fbzz::editor { void DrawBoneBakeControls(scene::AnimatorComponent& animator); }
