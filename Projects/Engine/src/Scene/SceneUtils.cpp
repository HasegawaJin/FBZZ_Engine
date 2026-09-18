/// @file    SceneUtils.cpp
/// @brief   起動・実行時にシーン / 物理 / UI サブシステムへ ProjectSettings を適用するユーティリティ。
/// @author  Hasegawa Jin
/// @date    2026-06-03
#include <Engine/Scene/SceneUtils.hpp>

#include <Engine/Scene/Components/CameraComponent.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Systems/UISystem.hpp>
#include <Engine/Util/StringUtils.hpp>
#include <Physics/Layer.hpp>
#include <algorithm>

namespace fbzz::scene {

void ApplyPhysicsSettings(physics::World& world, const ProjectSettings& settings)
{
    world.SetGravity(settings.physics.gravity);
    world.SetSubsteps(settings.physics.substeps);
    world.SetCollisionMatrix(settings.physics.collisionMatrix);
}

void ApplyUISettings(const ProjectSettings& settings, UISystemContext* ctx)
{
    if (ctx && !settings.ui.defaultFontPath.empty())
        UISystemSetDefaultFontPath(*ctx, settings.ui.defaultFontPath);
}

core::Window::Config MakeWindowConfig(const ProjectSettings& settings)
{
    core::Window::Config cfg;
    cfg.title      = util::StringUtils::ToWide(settings.window.title);
    cfg.width      = static_cast<uint32_t>(settings.window.width);
    cfg.height     = static_cast<uint32_t>(settings.window.height);
    cfg.fullscreen = settings.window.fullscreen;
    return cfg;
}

renderer::Camera ResolveGameCamera(Scene& scene, float aspectRatio)
{
    for (auto& go : scene.GameObjects()) {
        auto* cam = go.GetComponent<CameraComponent>();
        if (!go.activeInHierarchy() || !cam || !cam->enabled || !cam->isMain) continue;

        renderer::Camera result;
        result.m_position = go.transform.worldPosition;
        result.m_rotation = go.transform.worldRotation;
        result.m_fovY     = cam->fovY;
        result.m_near     = cam->nearZ;
        result.m_far      = cam->farZ;
        result.m_backgroundColor = cam->backgroundColor;
        result.m_clearMode       = cam->clearMode;
        /// @note viewport から決まる実効 aspect を Component に戻し、Script の座標変換と描画カメラを一致させる。
        cam->aspectRatio  = aspectRatio;
        result.m_aspect   = cam->aspectRatio;
        return result;
    }

    /// @note Main Camera がないシーンでも Standalone を落とさず、既定カメラで描画を継続する。
    renderer::Camera fallback;
    fallback.m_aspect = aspectRatio;
    return fallback;
}

renderer::Camera ResolveEditorGameCamera(Scene& scene,
                                         const renderer::Camera& editorFallback,
                                         float aspectRatio)
{
    renderer::Camera camera = editorFallback;
    camera.m_aspect = aspectRatio;

    for (auto& go : scene.GameObjects()) {
        auto* cam = go.GetComponent<CameraComponent>();
        if (!go.activeInHierarchy() || !cam || !cam->enabled || !cam->isMain) continue;

        camera.m_position = go.transform.worldPosition;
        camera.m_rotation = go.transform.worldRotation;
        camera.m_fovY     = cam->fovY;
        camera.m_near     = cam->nearZ;
        camera.m_far      = cam->farZ;
        camera.m_backgroundColor = cam->backgroundColor;
        camera.m_clearMode       = cam->clearMode;
        cam->aspectRatio  = aspectRatio;
        camera.m_aspect   = cam->aspectRatio;
        break;
    }

    return camera;
}

math::Vector4 ResolveGameBackgroundColor(Scene& scene)
{
    for (auto& go : scene.GameObjects()) {
        auto* cam = go.GetComponent<CameraComponent>();
        if (!go.activeInHierarchy() || !cam || !cam->enabled || !cam->isMain) continue;
        return cam->backgroundColor;
    }
    return renderer::kDefaultBackgroundColor;
}

fbzz::LayerMask ResolveGameCullingMask(Scene& scene)
{
    for (auto& go : scene.GameObjects()) {
        auto* cam = go.GetComponent<CameraComponent>();
        if (!go.activeInHierarchy() || !cam || !cam->enabled || !cam->isMain) continue;
        return cam->cullingMask;
    }
    return fbzz::Layer::Everything;
}

/// レイヤー数が Physics 側とずれると層ごとの距離が 1 つずつずれる。
/// CameraCullingSettings.hpp は Layer.hpp を引かない方針なので、突き合わせはここで行う。
static_assert(kCullLayerCount == 32, "CameraCullingSettings must cover fbzz::Layer 0-31");

CameraCullingSettings ResolveGameCullingSettings(Scene& scene)
{
    for (auto& go : scene.GameObjects()) {
        auto* cam = go.GetComponent<CameraComponent>();
        if (!go.activeInHierarchy() || !cam || !cam->enabled || !cam->isMain) continue;
        CameraCullingSettings settings;
        settings.frustumCulling       = cam->frustumCulling;
        settings.occlusionCulling     = cam->occlusionCulling;
        /// @note 負値は球を縮める / 距離を反転させるだけで意味を持たないので入口で 0 に潰し、
        ///       各パスが毎オブジェクト符号を気にする必要をなくす。
        settings.cullingBoundsPadding = (std::max)(cam->cullingBoundsPadding, 0.0f);
        settings.maxDrawDistance      = (std::max)(cam->maxDrawDistance, 0.0f);
        settings.cullDistanceSpherical = cam->cullDistanceSpherical;
        settings.smallObjectScreenHeight = (std::max)(cam->smallObjectScreenHeight, 0.0f);
        for (int i = 0; i < kCullLayerCount; ++i)
            settings.layerCullDistances[i] = (std::max)(cam->layerCullDistances[i], 0.0f);
        return settings;
    }
    return CameraCullingSettings{};
}

} // namespace fbzz::scene
