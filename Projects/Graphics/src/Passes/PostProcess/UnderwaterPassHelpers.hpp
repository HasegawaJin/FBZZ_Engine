/// @file    UnderwaterPassHelpers.hpp
/// @brief   水中積分と最終合成で同じ解決済み媒体を定数へ写す。
/// @author  Hasegawa Jin
/// @date    2026-10-09
#pragma once
#include <Graphics/Pipeline/RenderConstants.hpp>
#include <Graphics/Renderer/RenderEnvironment.hpp>
#include <Graphics/Renderer/RenderScene.hpp>

namespace fbzz::renderer {
/// @brief Plans the shared field from immutable numeric bodies, including rays originating inside a dry exclusion.
inline bool HasOpticalWaterMedium(const RenderScene& scene, bool requireLightStrength = false)
{
    for (const auto& water : scene.water)
        if (water.medium.opticalEnabled && water.medium.depthLimit > 0.0f
            && (!requireLightStrength || water.medium.scatteringLight.w > 0.0f)) return true;
    return false;
}

inline bool UsesOpticalUnderwater(const RenderUnderwaterInput& water)
{
    return water.enabled && water.opticalEnabled && water.strength > 0.0f;
}

/// @note 水域の所属・係数・距離はScene抽出で検証済み。光学mode0の旧表示も同じ境界から渡す。
inline void FillUnderwaterConstants(PostProcCB& constants, const RenderUnderwaterInput& water)
{
    if (!water.enabled) return;
    constants.underwaterStrength = water.strength;
    constants.underwaterDepth = water.depth;
    constants.underwaterColor[0] = water.color.x;
    constants.underwaterColor[1] = water.color.y;
    constants.underwaterColor[2] = water.color.z;
    constants.underwaterFogDensity = water.fogDensity;
    if (!UsesOpticalUnderwater(water)) return;
    constants.underwaterOpticalMode = 1.0f;
    constants.underwaterAbsorption[0] = water.absorption.x;
    constants.underwaterAbsorption[1] = water.absorption.y;
    constants.underwaterAbsorption[2] = water.absorption.z;
    constants.underwaterScattering[0] = water.scattering.x;
    constants.underwaterScattering[1] = water.scattering.y;
    constants.underwaterScattering[2] = water.scattering.z;
    constants.underwaterPhaseG = water.phaseG;
    constants.underwaterSurfaceY = water.surfaceY;
    constants.underwaterBottomY = water.bottomY;
    constants.underwaterMaxDistance = water.maxDistance;
    constants.underwaterLightStrength = water.lightStrength;
    constants.underwaterWaveAmplitude = water.waveAmplitude;
    constants.underwaterWaveFrequency = water.waveFrequency;
    constants.underwaterWaveSpeed = water.waveSpeed;
    constants.underwaterDistortion = water.distortion;
    constants.underwaterCenter[0] = water.center.x;
    constants.underwaterCenter[1] = water.center.y;
    constants.underwaterHalfExtent[0] = water.halfExtent.x;
    constants.underwaterHalfExtent[1] = water.halfExtent.y;
    constants.underwaterCosYaw = water.cosYaw;
    constants.underwaterSinYaw = water.sinYaw;
    constants.underwaterLightSteps = water.lightSteps;
}
} /// @note namespace fbzz::renderer
