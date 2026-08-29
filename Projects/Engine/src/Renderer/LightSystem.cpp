/// @file    LightSystem.cpp
/// @brief   ライト配列の管理と定数バッファ転送。
/// @author  Hasegawa Jin
/// @date    2026-05-21
///
/// Directional / Point / Spot を HLSL の cbuffer レイアウトへ詰める。
/// 個数上限はシェーダー側の配列サイズと一致させる。
#include "Engine/Renderer/LightSystem.hpp"
#include <algorithm>
#include <cassert>

namespace fbzz::renderer {

void LightSystem::SetDirectional(const DirectionalLight& light)
{
    m_directional = light;
}

const DirectionalLight& LightSystem::GetDirectional() const
{
    return m_directional;
}

void LightSystem::AddPoint(const PointLight& light)
{
    assert(m_pointLights.size() < 8 && "MAX_POINT_LIGHTS exceeded");
    m_pointLights.push_back(light);
}

void LightSystem::AddSpot(const SpotLight& light)
{
    assert(m_spotLights.size() < 4 && "MAX_SPOT_LIGHTS exceeded");
    m_spotLights.push_back(light);
}

void LightSystem::Clear()
{
    m_pointLights.clear();
    m_spotLights.clear();
}

void LightSystem::Upload(IConstantBuffer& cb) const
{
    LightConstantsCB data{};

    data.lightDir       = m_directional.direction;
    data.lightColor     = m_directional.color;
    data.lightIntensity = m_directional.intensity;

    int pCount = static_cast<int>(
        std::min(m_pointLights.size(), static_cast<size_t>(8)));
    for (int i = 0; i < pCount; ++i)
        data.pointLights[i] = m_pointLights[i];
    data.pointLightCount = pCount;

    int sCount = static_cast<int>(
        std::min(m_spotLights.size(), static_cast<size_t>(4)));
    for (int i = 0; i < sCount; ++i)
        data.spotLights[i] = m_spotLights[i];
    data.spotLightCount = sCount;

    cb.Update(&data, sizeof(LightConstantsCB));
}

} // namespace fbzz::renderer
