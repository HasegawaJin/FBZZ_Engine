// FBZZ Engine
// LightSystem.cpp | fbzz::renderer
// DirectionalLight の管理と定数バッファ転送
#include "engine/Renderer/LightSystem.hpp"
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

void LightSystem::Upload(IConstantBuffer& cb) const
{
    cb.Update(&m_directional, sizeof(DirectionalLight));
}

} // namespace fbzz::renderer
