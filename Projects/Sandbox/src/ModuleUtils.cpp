// FBZZ Engine
// ModuleUtils.cpp | fbzz::sandbox
// Sandbox Module 間で共有するシーン・設定ヘルパー
#include "ModuleUtils.hpp"

#include <Engine/Scene/Components/CameraComponent.hpp>
#include <Engine/Scene/Systems/UISystem.hpp>

namespace fbzz::sandbox {

void ApplyPhysicsSettings(physics::World& world, const ProjectSettings& settings)
{
    world.SetGravity(settings.physics.gravity);
    world.SetSubsteps(settings.physics.substeps);
}

void ApplyUISettings(const ProjectSettings& settings)
{
    scene::UISystemSetDefaultFontPath(settings.ui.defaultFontPath);
}

renderer::Camera ResolveGameCamera(scene::Scene& scene, float aspectRatio)
{
    for (auto& go : scene.GameObjects()) {
        auto* cam = go.GetComponent<scene::CameraComponent>();
        if (!go.activeSelf() || !cam || !cam->enabled || !cam->isMain) continue;

        renderer::Camera result;
        result.m_position = go.transform.position;
        result.m_rotation = go.transform.rotation;
        result.m_fovY     = cam->fovY;
        result.m_near     = cam->nearZ;
        result.m_far      = cam->farZ;
        result.m_aspect   = aspectRatio;
        return result;
    }

    // WHY: Main Camera がないシーンでも Standalone を落とさず、既定カメラで描画を継続する。
    renderer::Camera fallback;
    fallback.m_aspect = aspectRatio;
    return fallback;
}

} // namespace fbzz::sandbox
