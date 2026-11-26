/// @file    SkyAtmosphereConstants.hpp
/// @brief   ビューから解決した空の入力をシェーダーの大気定数へ変換する。
/// @author  Hasegawa Jin
/// @date    2026-11-25
#pragma once
#include <Graphics/Pipeline/Resources/RenderConstants.hpp>
#include <Graphics/Renderer/View/Lighting/RenderEnvironment.hpp>

namespace fbzz::renderer {

/// @brief 空の描画と大気フォグで共通の散乱係数を設定する。
/// @param[in] sky 当該ビューで解決した空の入力。空がない場合は null。
/// @return 空の大気定数。空がなければ既定半径 [km] と散乱量 0 を返す。
[[nodiscard]] inline AtmosphereCB MakeSkyAtmosphereConstants(const RenderSkyInput* sky)
{
    AtmosphereCB data{};
    if (!sky) {
        constexpr float DEFAULT_PLANET_RADIUS_KM = 6371.0f;
        constexpr float DEFAULT_ATMOSPHERE_HEIGHT_KM = 100.0f;
        data.planetRadius = DEFAULT_PLANET_RADIUS_KM;
        data.atmosphereRadius = DEFAULT_PLANET_RADIUS_KM + DEFAULT_ATMOSPHERE_HEIGHT_KM;
        return data;
    }

    data.rayleighScattering[0] = sky->rayleighScattering.x;
    data.rayleighScattering[1] = sky->rayleighScattering.y;
    data.rayleighScattering[2] = sky->rayleighScattering.z;
    data.mieScattering = sky->mieScattering;
    data.planetRadius = sky->planetRadius;
    data.atmosphereRadius = sky->atmosphereRadius;
    data.sunIntensity = sky->skyScatterIntensity;
    data.mieG = sky->mieG;
    return data;
}

} // namespace fbzz::renderer
