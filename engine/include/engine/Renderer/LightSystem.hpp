// FBZZ Engine
// LightSystem.hpp | fbzz::renderer
// DirectionalLight 管理と定数バッファへの転送
#pragma once
#include "IConstantBuffer.hpp"
#include <math/Vector3.hpp>

namespace fbzz::renderer {

struct DirectionalLight {
    math::Vector3 direction = {  0.0f, -1.0f,  0.5f };
    float         _pad0     = 0.0f;
    math::Vector3 color     = {  1.0f,  1.0f,  1.0f };
    float         intensity = 1.0f;
};

class LightSystem {
public:
    void SetDirectional(const DirectionalLight& light);
    const DirectionalLight& GetDirectional() const;
    void Upload(IConstantBuffer& cb) const;

private:
    DirectionalLight m_directional;
};

} // namespace fbzz::renderer
