/// @file    ScriptCameraProxies.cpp
/// @brief   Camera 領域の Script 転送と関連する内部ヘルパー。
/// @author  Hasegawa Jin
/// @date    2026-10-31
#include "ScriptProxyCommon.hpp"
#include <Engine/Scene/Script.hpp>
#include <Engine/Scene/ScriptRuntime.hpp>
#include <Engine/Input/Input.hpp>
#include <Engine/Renderer/Camera.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Components/CameraComponent.hpp>
#include <Engine/Scene/Components/LightComponent.hpp>
#include <limits>
#include <type_traits>
#include <string_view>
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cmath>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace fbzz::scene {
using namespace script_proxy_detail;
namespace {
renderer::Camera BuildCameraFromComponent(const GameObject* gameObject, const CameraComponent* component)
{
    renderer::Camera camera;
    if (!gameObject || !component) {
        return camera;
    }

    camera.m_position = gameObject->transform.worldPosition;
    camera.m_rotation = gameObject->transform.worldRotation;
    camera.m_fovY     = component->fovY;
    camera.m_aspect   = component->aspectRatio;
    camera.m_near     = component->nearZ;
    camera.m_far      = component->farZ;
    camera.m_backgroundColor = component->backgroundColor;
    camera.m_clearMode       = component->clearMode;
    return camera;
}


bool ResolveProjectionCamera(const Script* script, GameObject* object, renderer::Camera& out)
{
    if (!object && script) {
        object = script->scene.Self();
        if (!ObjectComponent<CameraComponent>(object)) object = script->scene.GetMainCameraObject();
    }
    const auto* component = ObjectComponent<CameraComponent>(object);
    if (!component || !component->enabled || !object->activeInHierarchy()) return false;
    if (!std::isfinite(component->fovY) || component->fovY <= 0.0f || component->fovY >= 180.0f ||
        !std::isfinite(component->aspectRatio) || component->aspectRatio <= 0.0f ||
        !std::isfinite(component->nearZ) || component->nearZ <= 0.0f ||
        !std::isfinite(component->farZ) || component->farZ <= component->nearZ) return false;
    out = BuildCameraFromComponent(object, component);
    return true;
}


bool ProjectToViewport(const renderer::Camera& camera, const math::Vector3& worldPos,
                       math::Vector3& outViewport)
{
    const math::Vector4 clip = camera.GetViewProjection() * math::Vector4(worldPos, 1.0f);
    if (!std::isfinite(clip.w) || clip.w <= 0.000001f) return false;
    const math::Vector3 result{0.5f + 0.5f * clip.x / clip.w,
                              0.5f - 0.5f * clip.y / clip.w, clip.z / clip.w};
    if (!std::isfinite(result.x) || !std::isfinite(result.y) || !std::isfinite(result.z)) return false;
    outViewport = result;
    return true;
}


} /// @note namespace
void ScriptLightProxy::SetColor(const math::Vector3& color) const
{
    if (auto* l = SelfComponent<LightComponent>(script)) l->color = color;
}


void ScriptLightProxy::SetType(LightType type) const
{
    if (auto* l = SelfComponent<LightComponent>(script))
        l->type = static_cast<LightComponent::Type>(static_cast<int>(type));
}


void ScriptLightProxy::SetIntensity(float intensity) const
{
    if (auto* l = SelfComponent<LightComponent>(script)) l->intensity = intensity;
}


void ScriptLightProxy::SetRange(float range) const
{
    if (auto* l = SelfComponent<LightComponent>(script)) l->range = range;
}


void ScriptLightProxy::SetInnerCone(float degrees) const
{
    if (auto* l = SelfComponent<LightComponent>(script)) l->innerCone = degrees;
}


void ScriptLightProxy::SetOuterCone(float degrees) const
{
    if (auto* l = SelfComponent<LightComponent>(script)) l->outerCone = degrees;
}


void ScriptLightProxy::SetEnabled(bool enabled) const
{
    if (auto* l = SelfComponent<LightComponent>(script)) l->enabled = enabled;
}


math::Vector3 ScriptLightProxy::GetColor() const
{
    const auto* l = SelfComponent<LightComponent>(script);
    return l ? l->color : math::Vector3::ONE;
}


LightType ScriptLightProxy::GetType() const
{
    const auto* l = SelfComponent<LightComponent>(script);
    return l ? static_cast<LightType>(static_cast<int>(l->type)) : LightType::Directional;
}


float ScriptLightProxy::GetIntensity() const
{
    const auto* l = SelfComponent<LightComponent>(script);
    return l ? l->intensity : 0.0f;
}


float ScriptLightProxy::GetRange() const
{
    const auto* l = SelfComponent<LightComponent>(script);
    return l ? l->range : 0.0f;
}


float ScriptLightProxy::GetInnerCone() const
{
    const auto* l = SelfComponent<LightComponent>(script);
    return l ? l->innerCone : 0.0f;
}


float ScriptLightProxy::GetOuterCone() const
{
    const auto* l = SelfComponent<LightComponent>(script);
    return l ? l->outerCone : 0.0f;
}


bool ScriptLightProxy::IsEnabled() const
{
    const auto* l = SelfComponent<LightComponent>(script);
    return l && l->enabled;
}


void ScriptCameraProxy::SetAsMain() const
{
    if (!script || !script->m_scene || !script->m_gameObject) return;
    for (auto& go : script->m_scene->GameObjects()) {
        if (auto* cam = go.GetComponent<CameraComponent>())
            cam->isMain = false;
    }
    if (auto* cam = script->m_gameObject->GetComponent<CameraComponent>())
        cam->isMain = true;
}


void ScriptCameraProxy::SetFOV(float fovY) const
{
    if (auto* cam = SelfComponent<CameraComponent>(script)) cam->fovY = fovY;
}


void ScriptCameraProxy::SetAspectRatio(float aspectRatio) const
{
    if (auto* cam = SelfComponent<CameraComponent>(script)) cam->aspectRatio = (std::max)(aspectRatio, 0.0001f);
}


void ScriptCameraProxy::SetNearFar(float nearZ, float farZ) const
{
    if (auto* cam = SelfComponent<CameraComponent>(script)) {
        cam->nearZ = nearZ;
        cam->farZ = farZ;
    }
}


void ScriptCameraProxy::SetCullingMask(fbzz::LayerMask mask) const
{
    if (auto* cam = SelfComponent<CameraComponent>(script)) cam->cullingMask = mask;
}


void ScriptCameraProxy::SetBackgroundColor(const math::Vector4& color) const
{
    if (auto* cam = SelfComponent<CameraComponent>(script)) cam->backgroundColor = color;
}


void ScriptCameraProxy::SetClearMode(renderer::CameraClearMode mode) const
{
    if (auto* cam = SelfComponent<CameraComponent>(script)) cam->clearMode = mode;
}


math::Vector3 ScriptCameraProxy::WorldToScreenPoint(const math::Vector3& worldPos) const
{
    math::Vector3 viewport;
    if (!TryWorldToViewportPoint(worldPos, viewport)) return math::Vector3::ZERO;
    const auto rt = ScriptRuntime::GetCurrent();
    const float width  = static_cast<float>((std::max)(rt.viewportWidth,  1u));
    const float height = static_cast<float>((std::max)(rt.viewportHeight, 1u));
    return {viewport.x * width, viewport.y * height, viewport.z};
}


bool ScriptCameraProxy::TryWorldToViewportPoint(const math::Vector3& worldPos,
    math::Vector3& outViewport, GameObject* cameraObject) const
{
    renderer::Camera camera;
    return ResolveProjectionCamera(script, cameraObject, camera) &&
           ProjectToViewport(camera, worldPos, outViewport);
}


bool ScriptCameraProxy::TryViewportToWorldPoint(const math::Vector2& uv, float depth,
    math::Vector3& outWorld, GameObject* cameraObject) const
{
    if (!std::isfinite(depth) || depth <= 0.0f || !std::isfinite(uv.x) || !std::isfinite(uv.y))
        return false;
    renderer::Camera camera;
    if (!ResolveProjectionCamera(script, cameraObject, camera)) return false;
    const float halfHeight = std::tan(camera.m_fovY * DEG_TO_RAD * 0.5f) * depth;
    const math::Vector3 result = camera.m_position + camera.GetForward() * depth
        + camera.GetRight() * ((uv.x * 2.0f - 1.0f) * halfHeight * camera.m_aspect)
        + camera.GetUp() * ((1.0f - uv.y * 2.0f) * halfHeight);
    if (!std::isfinite(result.x) || !std::isfinite(result.y) || !std::isfinite(result.z)) return false;
    outWorld = result;
    return true;
}


math::Vector3 ScriptCameraProxy::ScreenToWorldPoint(const math::Vector3& screenPos) const
{
    renderer::Camera camera;
    if (!ResolveProjectionCamera(script, nullptr, camera)) return math::Vector3::ZERO;
    const auto rt = ScriptRuntime::GetCurrent();
    const float width  = static_cast<float>((std::max)(rt.viewportWidth,  1u));
    const float height = static_cast<float>((std::max)(rt.viewportHeight, 1u));

    const float ndcX = (screenPos.x / width) * 2.0f - 1.0f;
    const float ndcY = 1.0f - (screenPos.y / height) * 2.0f;
    const float ndcZ = std::clamp(screenPos.z, 0.0f, 1.0f);

    const math::Matrix4 invVP = math::Matrix4::Inverse(camera.GetViewProjection());
    const math::Vector4 world = invVP * math::Vector4(ndcX, ndcY, ndcZ, 1.0f);
    if (std::abs(world.w) <= 0.000001f) {
        return math::Vector3::ZERO;
    }
    return world.XYZ() * (1.0f / world.w);
}


float ScriptCameraProxy::GetFOV() const
{
    const auto* cam = SelfComponent<CameraComponent>(script);
    return cam ? cam->fovY : 60.0f;
}


float ScriptCameraProxy::GetNearZ() const
{
    const auto* cam = SelfComponent<CameraComponent>(script);
    return cam ? cam->nearZ : 0.1f;
}


float ScriptCameraProxy::GetFarZ() const
{
    const auto* cam = SelfComponent<CameraComponent>(script);
    return cam ? cam->farZ : 1000.0f;
}


float ScriptCameraProxy::GetAspectRatio() const
{
    const auto* cam = SelfComponent<CameraComponent>(script);
    return cam ? cam->aspectRatio : 16.0f / 9.0f;
}


fbzz::LayerMask ScriptCameraProxy::GetCullingMask() const
{
    const auto* cam = SelfComponent<CameraComponent>(script);
    return cam ? cam->cullingMask : fbzz::Layer::Everything;
}


math::Vector4 ScriptCameraProxy::GetBackgroundColor() const
{
    const auto* cam = SelfComponent<CameraComponent>(script);
    return cam ? cam->backgroundColor : renderer::kDefaultBackgroundColor;
}


renderer::CameraClearMode ScriptCameraProxy::GetClearMode() const
{
    const auto* cam = SelfComponent<CameraComponent>(script);
    return cam ? cam->clearMode : renderer::CameraClearMode::SolidColor;
}


bool ScriptCameraProxy::IsMain() const
{
    const auto* cam = SelfComponent<CameraComponent>(script);
    return cam && cam->isMain;
}


bool ScriptCameraProxy::IsVisible(const math::Vector3& worldPos) const
{
    math::Vector3 viewport;
    return TryWorldToViewportPoint(worldPos, viewport) && viewport.z >= 0.0f && viewport.z <= 1.0f &&
           viewport.x >= 0.0f && viewport.x <= 1.0f && viewport.y >= 0.0f && viewport.y <= 1.0f;
}


Ray ScriptCameraProxy::ScreenPointToRay(float screenX, float screenY) const
{
    const math::Vector3 nearPt = ScreenToWorldPoint({ screenX, screenY, 0.0f });
    const math::Vector3 farPt  = ScreenToWorldPoint({ screenX, screenY, 1.0f });
    const math::Vector3 dir    = (farPt - nearPt).NormalizedOr(math::Vector3::ZERO);
    return { nearPt, dir };
}

/// @brief スロットを所有する MaterialComponent を解決する。
/// @brief SetEnabled のようなコンポーネント全体の操作はこちらを使う。

} /// @note namespace fbzz::scene
