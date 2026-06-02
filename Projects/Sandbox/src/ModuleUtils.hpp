// FBZZ Engine
// ModuleUtils.hpp | fbzz::sandbox
// Sandbox Module 間で共有するシーン・設定ヘルパー
#pragma once

#include <Engine/ProjectSettings.hpp>
#include <Engine/Renderer/Camera.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Physics/World.hpp>

namespace fbzz::sandbox {

/// ProjectSettings の物理設定を physics::World へ反映する。
void ApplyPhysicsSettings(physics::World& world, const ProjectSettings& settings);

/// ProjectSettings の UI 設定を UI システムへ反映する。
void ApplyUISettings(const ProjectSettings& settings);

/// シーン内の Main Camera を renderer::Camera として解決する。
[[nodiscard]] renderer::Camera ResolveGameCamera(scene::Scene& scene, float aspectRatio);

} // namespace fbzz::sandbox
