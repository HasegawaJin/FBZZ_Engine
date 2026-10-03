/// @file    RayGameResources.cpp
/// @brief   Game 再構成のカメラ cut と表面履歴受理契約。
/// @author  Hasegawa Jin
/// @date    2026-10-01
#include <Graphics/RayTracing/RayGameResources.hpp>
#include <algorithm>
#include <bit>
#include <cmath>

namespace fbzz::renderer {
namespace {
bool Exact(float first, float second)
{
    return std::bit_cast<uint32_t>(first) == std::bit_cast<uint32_t>(second);
}
float Dot3(const math::Vector4& first, const math::Vector4& second)
{
    return first.x * second.x + first.y * second.y + first.z * second.z;
}
} /// @note namespace

bool IsRayGameCameraCut(const Camera& previous, const Camera& current)
{
    const auto movement = current.m_position - previous.m_position;
    return movement.LengthSq() > 1.0f
        || math::Vector3::Dot(previous.GetForward(), current.GetForward()) < 0.70710678118f
        || previous.m_projection != current.m_projection
        || !Exact(previous.m_fovY, current.m_fovY) || !Exact(previous.m_aspect, current.m_aspect)
        || !Exact(previous.m_near, current.m_near) || !Exact(previous.m_far, current.m_far)
        || !Exact(previous.m_orthoHeight, current.m_orthoHeight);
}

bool IsRayReconstructionHistoryCompatible(const RayReconstructionSurface& current,
    const RayReconstructionSurface& previous, const Camera& previousCamera)
{
    if (current.objectMaterialValid[3] != 1 || previous.objectMaterialValid[3] != 1
        || current.previousPositionValid.w == 0 || current.sceneFlags != previous.sceneFlags
        || current.objectMaterialValid != previous.objectMaterialValid
        || Dot3(current.normalRoughness, previous.normalRoughness) < 0.95f
        || std::abs(current.normalRoughness.w - previous.normalRoughness.w) > 0.03f) return false;
    const auto& expected = current.previousPositionValid;
    const auto& actual = previous.positionDepth;
    const float distanceSquared = (expected.x - actual.x) * (expected.x - actual.x)
        + (expected.y - actual.y) * (expected.y - actual.y) + (expected.z - actual.z) * (expected.z - actual.z);
    const float previousDepth = math::Vector3::Dot(math::Vector3{expected.x, expected.y, expected.z}
        - previousCamera.m_position, previousCamera.GetForward());
    const float tolerance = (std::max)(0.001f, std::abs(previousDepth) * 0.002f);
    return std::isfinite(distanceSquared) && distanceSquared <= tolerance * tolerance
        && std::abs(current.albedoMetallic.x - previous.albedoMetallic.x) <= 0.02f
        && std::abs(current.albedoMetallic.y - previous.albedoMetallic.y) <= 0.02f
        && std::abs(current.albedoMetallic.z - previous.albedoMetallic.z) <= 0.02f
        && std::abs(current.albedoMetallic.w - previous.albedoMetallic.w) <= 0.02f;
}

} /// @note namespace fbzz::renderer
