/// @file    RenderViewSettings.hpp
/// @brief   ビューのカメラ・描画設定を解決する内部インターフェース。
/// @author  Hasegawa Jin
/// @date    2026-11-21
#pragma once
#include <Graphics/Renderer/View/Camera.hpp>
#include <Graphics/Renderer/View/RenderSettings.hpp>
#include <Engine/Scene/Components/Environment/EnvironmentLightComponent.hpp>

namespace fbzz::scene {
class Scene;
namespace render_view_detail {
struct ResolvedViewSettings {
    renderer::Camera camera;
    renderer::RenderSettings settings;
    IblSource iblSource;
};

[[nodiscard]] ResolvedViewSettings ResolveViewSettings(
    Scene& scene, const renderer::Camera& inputCamera, const renderer::RenderSettings* settings);
}
}
