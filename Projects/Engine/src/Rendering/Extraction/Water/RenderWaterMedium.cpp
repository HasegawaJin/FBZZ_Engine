/// @file    RenderWaterMedium.cpp
/// @brief   水中の境界・光学設定・除外領域・濁りを描画入力にまとめる。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#include "RenderWaterAppearance.hpp"
#include "RenderWaterResources.hpp"
#include <Engine/Asset/Water/WaterOpticalSettings.hpp>
#include <Engine/Scene/Components/Water/WaterComponent.hpp>
#include <Engine/Scene/Hierarchy/Transform.hpp>
#include <Engine/Scene/Water/Surface/WaterExclusion.hpp>
#include <Engine/Scene/Water/WaterInteraction.hpp>
#include <Engine/Scene/Water/Surface/WaterSurfaceQuery.hpp>
#include <Math/Scalar/MathUtils.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace fbzz::scene::water_render_detail {

namespace {

std::array<math::Vector4, 3> WorldToLocalRows(const math::Vector3& position,
    const math::Quaternion& inverseRotation, const math::Vector3& inverseScale)
{
    const auto basisX = inverseRotation * math::Vector3::RIGHT;
    const auto basisY = inverseRotation * math::Vector3::UP;
    const auto basisZ = inverseRotation * math::Vector3::FORWARD;
    const math::Vector3 rows[3] = {
        {basisX.x * inverseScale.x, basisY.x * inverseScale.x, basisZ.x * inverseScale.x},
        {basisX.y * inverseScale.y, basisY.y * inverseScale.y, basisZ.y * inverseScale.y},
        {basisX.z * inverseScale.z, basisY.z * inverseScale.z, basisZ.z * inverseScale.z}
    };
    return {{{rows[0].x, rows[0].y, rows[0].z, -math::Vector3::Dot(rows[0], position)},
             {rows[1].x, rows[1].y, rows[1].z, -math::Vector3::Dot(rows[1], position)},
             {rows[2].x, rows[2].y, rows[2].z, -math::Vector3::Dot(rows[2], position)}}};
}

float MediumSetting(const asset::MaterialAssetAuthoring* material, const char* name,
    float fallback, float minimum, float maximum)
{
    const float value = ReadWaterMaterialFloat(material, name, fallback);
    return std::isfinite(value) ? std::clamp(value, minimum, maximum) : fallback;
}

} // namespace

renderer::WaterMediumInput BuildMediumInput(const WaterComponent& water, const Transform& transform,
    EntityID owner, const asset::MaterialAssetAuthoring* material, const WaterMesh& mesh,
    const renderer::WaterWaveMargin& margin, renderer::WaterCB& constants)
{
    renderer::WaterMediumInput medium;
    medium.stableOwnerKey = (static_cast<uint64_t>(owner.generation) << 32u) | owner.index;
    const auto inverseRotation = transform.worldRotation.Inverse();
    medium.worldToLocalRows = WorldToLocalRows(transform.worldPosition, inverseRotation,
        {1.0f / transform.worldScale.x, 1.0f / transform.worldScale.y, 1.0f / transform.worldScale.z});
    float minimumX = std::numeric_limits<float>::max(), minimumY = minimumX, minimumZ = minimumX;
    float maximumX = -minimumX, maximumY = -minimumX, maximumZ = -minimumX;
    for (uint32_t corner = 0; corner < 8u; ++corner) {
        const math::Vector3 localCorner = {
            (corner & 1u) ? mesh.aabbMax.x : mesh.aabbMin.x,
            (corner & 2u) ? mesh.aabbMax.y : mesh.aabbMin.y,
            (corner & 4u) ? mesh.aabbMax.z : mesh.aabbMin.z
        };
        const auto worldCorner = transform.worldPosition + transform.worldRotation * math::Vector3{
            localCorner.x * transform.worldScale.x, localCorner.y * transform.worldScale.y, localCorner.z * transform.worldScale.z};
        minimumX = (std::min)(minimumX, worldCorner.x); minimumZ = (std::min)(minimumZ, worldCorner.z);
        maximumX = (std::max)(maximumX, worldCorner.x); maximumZ = (std::max)(maximumZ, worldCorner.z);
        minimumY = (std::min)(minimumY, worldCorner.y); maximumY = (std::max)(maximumY, worldCorner.y);
    }
    medium.worldBoundsXZ = {minimumX - margin.horizontal, minimumZ - margin.horizontal,
                           maximumX + margin.horizontal, maximumZ + margin.horizontal};
    const auto optics = (material ? asset::ResolveWaterOpticalSettings(*material) : asset::WaterOpticalSettings{});
    medium.opticalEnabled = optics.enabled;
    medium.absorptionPhaseG = {optics.absorption.x, optics.absorption.y, optics.absorption.z, optics.phaseG};
    medium.scatteringLight = {optics.scattering.x, optics.scattering.y, optics.scattering.z,
        MediumSetting(material, "underwaterLightStrength", 1.0f, 0.0f, 8.0f)};
    medium.surfaceY = transform.worldPosition.y;
    medium.depthLimit = water.buoyancyDepth;
    medium.analyticRectangle = !UsesWaterSurfaceGeometry(water, WaterSurfaceReadView::SIMULATION);
    const auto localExtent = WaterSurfaceLocalExtent(water, WaterSurfaceReadView::SIMULATION);
    medium.localHalfExtent = localExtent * 0.5f;
    medium.bottomY = medium.surfaceY - medium.depthLimit;
    medium.maxDistance = MediumSetting(material, "underwaterMaxDistance", 120.0f, 1.0f, 10000.0f);
    medium.transitionDepth = MediumSetting(material, "underwaterTransitionDepth", 0.15f, 0.001f, 2.0f);
    medium.distortion = MediumSetting(material, "underwaterDistortion", 0.001f, 0.0f, 0.01f);
    medium.lightSteps = static_cast<int>(MediumSetting(material, "underwaterLightSteps", 48.0f, 16.0f, 64.0f));
    medium.causticsEnabled = MediumSetting(material, "enableCaustics", 0.0f, 0.0f, 1.0f) >= 0.5f;
    medium.causticsIntensity = MediumSetting(material, "causticsIntensity", 1.0f, 0.0f, 100.0f);
    if (const auto* exclusions = WaterSurfaceExclusions(water, WaterSurfaceReadView::SIMULATION)) {
        medium.exclusions.reserve(exclusions->boxes.size());
        uint32_t waveRockCount = 0;
        for (const auto& box : exclusions->boxes) {
            medium.exclusions.push_back({WorldToLocalRows(box.worldPosition, box.inverseWorldRotation,
                box.inverseWorldScale), box.halfExtent});
            if (constants.riverWaveParams.x <= 0.0f || waveRockCount >= renderer::WATER_RIVER_WAVE_ROCK_COUNT) {
                continue;
            }
            const auto rotatedUp = box.inverseWorldRotation * math::Vector3::UP;
            if (rotatedUp.x * rotatedUp.x + rotatedUp.z * rotatedUp.z > 1.0e-6f) {
                continue;
            }
            const float halfX = box.halfExtent.x / std::abs(box.inverseWorldScale.x);
            const float halfY = box.halfExtent.y / std::abs(box.inverseWorldScale.y);
            const float halfZ = box.halfExtent.z / std::abs(box.inverseWorldScale.z);
            const float minimumWidth = (std::min)(halfX, halfZ);
            const float maximumWidth = (std::max)(halfX, halfZ);
            if (minimumWidth < 0.12f || maximumWidth > 3.0f || maximumWidth > 2.0f * minimumWidth
                || !std::isfinite(halfY)) {
                continue;
            }
            const auto& center = box.worldPosition;
            const float radius = std::sqrt((4.0f / math::PI) * halfX * halfZ);
            const float reach = 3.7f * radius;
            if (center.y + halfY < minimumY || center.y - halfY > maximumY
                || center.x + reach < minimumX || center.x - reach > maximumX
                || center.z + reach < minimumZ || center.z - reach > maximumZ) {
                continue;
            }
            constants.riverWaveRocks[2u * waveRockCount] = {center.x, center.y, center.z, radius};
            constants.riverWaveRocks[2u * waveRockCount + 1u].x = halfY;
            ++waveRockCount;
        }
        constants.riverWaveParams.y = static_cast<float>(waveRockCount);
    }
    if (const auto interactionSnapshot = WaterInteractionSnapshot(water)) {
        medium.plumes.reserve(interactionSnapshot->plumes.size());
        for (const auto& plume : interactionSnapshot->plumes) {
            if (plume.density <= 0.0f || plume.radius <= 0.0f
                || (plume.scattering.x <= 0.0f && plume.scattering.y <= 0.0f
                    && plume.scattering.z <= 0.0f)) {
                continue;
            }
            medium.plumes.push_back({{plume.worldPosition.x, plume.worldPosition.y, plume.worldPosition.z, plume.radius},
                                    {plume.scattering.x, plume.scattering.y, plume.scattering.z, plume.density}});
        }
    }
    return medium;
}

} // namespace fbzz::scene::water_render_detail
