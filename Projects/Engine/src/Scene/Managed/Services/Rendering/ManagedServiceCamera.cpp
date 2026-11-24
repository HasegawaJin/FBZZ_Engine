/// @file    ManagedServiceCamera.cpp
/// @brief   カメラの設定と座標投影のサービス経路。
/// @author  Hasegawa Jin
/// @date    2026-11-21
#include "../ManagedServiceRoutes.hpp"

#include <Engine/Scene/Components/Camera/CameraComponent.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Scripting/Script.hpp>
#include <Engine/Scene/Scripting/ScriptRuntime.hpp>
#include <Graphics/Renderer/View/Camera.hpp>
#include <Math/Scalar/MathUtils.hpp>
#include <algorithm>
#include <cmath>

namespace fbzz::scene::managed_detail {

namespace {

bool FiniteVector(const math::Vector3& value)
{ return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z); }
bool NormalizeVector(const math::Vector3& value, math::Vector3& output)
{
    const float length = value.Length();
    if (!FiniteVector(value) || !std::isfinite(length) || math::NearlyZero(length)) return false;
    output = value / length; return FiniteVector(output);
}
/// @note Perspective の DirectX depth を解析的に戻し、極端な行列で Inverse の MathContract を発火させない。
ManagedBridgeStatus CameraProjection(GameObject& target, const ManagedBridgeRequest& request, const ManagedServiceArguments& arguments, ManagedServiceResult& output)
{
    const auto& p = arguments.values;
    const uint32_t method = request.index;
    const size_t expected = method == 18 ? 2 : 3;
    if (request.flags != 1 || arguments.integer || arguments.relatedId || !arguments.text.empty() || p.size() != expected)
        return ManagedBridgeStatus::INVALID_ARGUMENT;
    const auto* component = target.GetComponent<CameraComponent>();
    if (!component) return ManagedBridgeStatus::NOT_FOUND;
    const bool tryResult = method == 17 || method == 19 || method == 20;
    output.integer = 0;
    if (!tryResult) output.values.assign(method == 18 ? 6 : 3, 0.0f);
    if (!component->enabled || !target.activeInHierarchy() || !std::isfinite(component->fovY) || component->fovY <= 0 || component->fovY >= 180
        || !std::isfinite(component->aspectRatio) || component->aspectRatio <= math::EPSILON || !std::isfinite(component->nearZ)
        || component->nearZ <= 0 || !std::isfinite(component->farZ) || component->farZ <= component->nearZ) return ManagedBridgeStatus::OK;
    const auto& pose = target.transform;
    const math::Vector3 rawForward = pose.worldRotation * math::Vector3::FORWARD;
    const math::Vector3 rawRight = pose.worldRotation * math::Vector3::RIGHT;
    const math::Vector3 rawUp = pose.worldRotation * math::Vector3::UP;
    math::Vector3 forward, right;
    if (!FiniteVector(pose.worldPosition) || !NormalizeVector((pose.worldPosition + rawForward) - pose.worldPosition, forward)
        || !NormalizeVector(math::Vector3::Cross(rawUp, forward), right)) return ManagedBridgeStatus::OK;
    const math::Vector3 up = math::Vector3::Cross(forward, right);
    const float yScale = 1.0f / std::tan(math::ToRad(component->fovY) * 0.5f);
    const float xScale = yScale / component->aspectRatio;
    const float q = component->farZ / (component->farZ - component->nearZ);
    const float zBias = -component->nearZ * component->farZ / (component->farZ - component->nearZ);
    if (!std::isfinite(xScale) || !std::isfinite(yScale) || xScale == 0 || yScale == 0 || !std::isfinite(q) || !std::isfinite(zBias)) return ManagedBridgeStatus::OK;
    const auto runtime = ScriptRuntime::GetCurrent();
    const float width = static_cast<float>(std::max(runtime.viewportWidth, 1u));
    const float height = static_cast<float>(std::max(runtime.viewportHeight, 1u));
    const auto unproject = [&](float x, float y, float z, math::Vector3& result) {
        const float nx = (x / width) * 2.0f - 1.0f;
        const float ny = 1.0f - (y / height) * 2.0f;
        const float denominator = q - std::clamp(z, 0.0f, 1.0f);
        if (!std::isfinite(nx) || !std::isfinite(ny) || denominator == 0 || !std::isfinite(denominator)) return false;
        const float depth = -zBias / denominator;
        const math::Vector3 candidate = pose.worldPosition + forward * depth + right * (nx * depth / xScale) + up * (ny * depth / yScale);
        if (!FiniteVector(candidate)) return false;
        result = candidate; return true;
    };
    math::Vector3 value;
    if (method == 8) {
        if (unproject(p[0], p[1], p[2], value)) output.values = {value.x, value.y, value.z};
    } else if (method == 18) {
        math::Vector3 near = math::Vector3::ZERO, far = math::Vector3::ZERO, direction = math::Vector3::ZERO;
        if (unproject(p[0], p[1], 0, near) && unproject(p[0], p[1], 1, far)) (void)NormalizeVector(far - near, direction);
        output.values = {near.x, near.y, near.z, direction.x, direction.y, direction.z};
    } else if (method == 20) {
        if (p[2] <= 0) return ManagedBridgeStatus::OK;
        const float halfHeight = std::tan(math::ToRad(component->fovY) * 0.5f) * p[2];
        value = pose.worldPosition + rawForward * p[2] + rawRight * ((p[0] * 2.0f - 1.0f) * halfHeight * component->aspectRatio)
            + rawUp * ((1.0f - p[1] * 2.0f) * halfHeight);
        if (FiniteVector(value)) { output.integer = 1; output.values = {value.x, value.y, value.z}; }
    } else {
        /// @note 元の world projection と同じ行列の演算順を維持する。LookAt の正規化前提は上で検証済み。
        renderer::Camera camera; camera.m_position = pose.worldPosition; camera.m_rotation = pose.worldRotation;
        camera.m_fovY = component->fovY; camera.m_aspect = component->aspectRatio; camera.m_near = component->nearZ; camera.m_far = component->farZ;
        const math::Vector4 clip = camera.GetViewProjection() * math::Vector4(p[0], p[1], p[2], 1);
        if (!std::isfinite(clip.w) || clip.w <= 0.000001f) return ManagedBridgeStatus::OK;
        value = {0.5f + 0.5f * clip.x / clip.w, 0.5f - 0.5f * clip.y / clip.w, clip.z / clip.w};
        if (!FiniteVector(value)) return ManagedBridgeStatus::OK;
        if (method == 17) output.integer = value.x >= 0 && value.x <= 1 && value.y >= 0 && value.y <= 1 && value.z >= 0 && value.z <= 1 ? 1 : 0;
        else if (method == 19) { output.integer = 1; output.values = {value.x, value.y, value.z}; }
        else {
            value.x *= width; value.y *= height;
            if (FiniteVector(value)) output.values = {value.x, value.y, value.z};
        }
    }
    return ManagedBridgeStatus::OK;
}

}

ManagedBridgeStatus RunManagedCameraService(Script& view, GameObject& target, const ManagedBridgeRequest& request,
    const ManagedServiceArguments& arguments, ManagedServiceResult& output)
{
    const auto& p = arguments.values;
    const auto& t = arguments.text;
    const int64_t integer = arguments.integer;
    if (!target.GetComponent<CameraComponent>()) return ManagedBridgeStatus::NOT_FOUND;
    if (request.index == 7 || request.index == 8 || request.index >= 17) {
        if (request.index > 20) return ManagedBridgeStatus::UNSUPPORTED;
        return CameraProjection(target, request, arguments, output);
    }
    switch (request.index) {
    case 0: {
        if (!MatchesServiceArguments(request, arguments, 0u, 0, 0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        view.camera.SetAsMain();
        break;
    }
    case 1: {
        if (!MatchesServiceArguments(request, arguments, 0u, 1, 0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        if (p[0] <= 0 || p[0] >= 180) return ManagedBridgeStatus::INVALID_ARGUMENT;
        view.camera.SetFOV(p[0]);
        break;
    }
    case 2: {
        if (!MatchesServiceArguments(request, arguments, 0u, 1, 0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        if (p[0] <= math::EPSILON) return ManagedBridgeStatus::INVALID_ARGUMENT;
        view.camera.SetAspectRatio(p[0]);
        break;
    }
    case 3: {
        if (!MatchesServiceArguments(request, arguments, 0u, 2, 0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        if (p[0] <= 0 || p[1] <= p[0] || !std::isfinite(p[0] * p[1] / (p[1] - p[0]))) return ManagedBridgeStatus::INVALID_ARGUMENT;
        view.camera.SetNearFar(p[0], p[1]);
        break;
    }
    case 4: {
        if (request.flags != 0u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
        if (integer < 0 || integer > UINT32_MAX) return ManagedBridgeStatus::INVALID_ARGUMENT;
        if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
        view.camera.SetCullingMask(static_cast<uint32_t>(integer));
        break;
    }
    case 5: {
        if (!MatchesServiceArguments(request, arguments, 0u, 4, 0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        view.camera.SetBackgroundColor(math::Vector4{p[0], p[1], p[2], p[3]});
        break;
    }
    case 6: {
        if (request.flags != 0u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
        if (integer < 0 || integer > 1) return ManagedBridgeStatus::INVALID_ARGUMENT;
        if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
        view.camera.SetClearMode(static_cast<renderer::CameraClearMode>(integer));
        break;
    }
    case 7: {
        if (!MatchesServiceArguments(request, arguments, 1u, 3, 0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        const auto value = view.camera.WorldToScreenPoint(math::Vector3{p[0], p[1], p[2]}); output.values = {value.x, value.y, value.z};
        break;
    }
    case 8: {
        if (!MatchesServiceArguments(request, arguments, 1u, 3, 0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        const auto value = view.camera.ScreenToWorldPoint(math::Vector3{p[0], p[1], p[2]}); output.values = {value.x, value.y, value.z};
        break;
    }
    case 9: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        output.values = {view.camera.GetFOV()};
        break;
    }
    case 10: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        output.values = {view.camera.GetNearZ()};
        break;
    }
    case 11: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        output.values = {view.camera.GetFarZ()};
        break;
    }
    case 12: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        output.values = {view.camera.GetAspectRatio()};
        break;
    }
    case 13: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        output.integer = static_cast<int64_t>(view.camera.GetCullingMask());
        break;
    }
    case 14: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        const auto value = view.camera.GetBackgroundColor(); output.values = {value.x, value.y, value.z, value.w};
        break;
    }
    case 15: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        output.integer = static_cast<int64_t>(view.camera.GetClearMode());
        break;
    }
    case 16: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        output.integer = static_cast<int64_t>(view.camera.IsMain());
        break;
    }
    case 17: {
        if (!MatchesServiceArguments(request, arguments, 1u, 3, 0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        output.integer = static_cast<int64_t>(view.camera.IsVisible(math::Vector3{p[0], p[1], p[2]}));
        break;
    }
    case 18: {
        if (!MatchesServiceArguments(request, arguments, 1u, 2, 0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        const auto value = view.camera.ScreenPointToRay(p[0], p[1]); output.values = {value.origin.x, value.origin.y, value.origin.z, value.direction.x, value.direction.y, value.direction.z};
        break;
    }
    case 19: {
        if (!MatchesServiceArguments(request, arguments, 1u, 3, 0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        math::Vector3 value;
        output.integer = view.camera.TryWorldToViewportPoint(math::Vector3{p[0], p[1], p[2]}, value, &target) ? 1 : 0;
        if (output.integer) output.values = {value.x, value.y, value.z};
        break;
    }
    case 20: {
        if (!MatchesServiceArguments(request, arguments, 1u, 3, 0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        math::Vector3 value;
        output.integer = view.camera.TryViewportToWorldPoint(math::Vector2{p[0], p[1]}, p[2], value, &target) ? 1 : 0;
        if (output.integer) output.values = {value.x, value.y, value.z};
        break;
    }
    default: return ManagedBridgeStatus::UNSUPPORTED;
    }
    return ManagedBridgeStatus::OK;
}

}
