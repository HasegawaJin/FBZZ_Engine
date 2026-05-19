// FBZZ Engine
// LightSystem.hpp | fbzz::renderer
// DirectionalLight / PointLight / SpotLight 管理と定数バッファへの転送
#pragma once
#include "IConstantBuffer.hpp"
#include <math/Vector3.hpp>
#include <vector>

namespace fbzz::renderer {

struct DirectionalLight {
    math::Vector3 direction = {  0.0f, -1.0f,  0.5f };
    float         _pad0     = 0.0f;
    math::Vector3 color     = {  1.0f,  1.0f,  1.0f };
    float         intensity = 1.0f;
};

struct PointLight {
    math::Vector3 position  = {};
    float         range     = 10.0f;
    math::Vector3 color     = { 1.0f, 1.0f, 1.0f };
    float         intensity = 1.0f;
};

struct SpotLight {
    math::Vector3 position  = {};
    float         range     = 10.0f;
    math::Vector3 direction = { 0.0f, -1.0f, 0.0f };
    float         innerCos  = 0.966f;   // ~15°
    math::Vector3 color     = { 1.0f, 1.0f, 1.0f };
    float         outerCos  = 0.866f;   // ~30°
    float         intensity = 1.0f;
    float         _pad[3]   = {};
};

// Constants.hlsli の LightConstants cbuffer と完全一致 (560 bytes)
struct LightConstantsCB {
    math::Vector3 lightDir;
    float         _lightPad0 = 0.0f;
    math::Vector3 lightColor;
    float         lightIntensity = 0.0f;
    PointLight    pointLights[8];
    SpotLight     spotLights[4];
    int           pointLightCount = 0;
    int           spotLightCount  = 0;
    float         _lightPad2[2]   = {};
};

class LightSystem {
public:
    void SetDirectional(const DirectionalLight& light);
    const DirectionalLight& GetDirectional() const;

    void AddPoint(const PointLight& light);
    void AddSpot(const SpotLight& light);
    void Clear();

    void Upload(IConstantBuffer& cb) const;

private:
    DirectionalLight    m_directional;
    std::vector<PointLight> m_pointLights;
    std::vector<SpotLight>  m_spotLights;
};

} // namespace fbzz::renderer
