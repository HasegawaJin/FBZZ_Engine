/// @file    RayReflectionReconstructionPass.hpp
/// @brief   HDR 反射の静止時履歴と限定空間フィルター。
/// @author  Hasegawa Jin
/// @date    2026-10-01
#pragma once
#include <Graphics/Pipeline/IRenderPass.hpp>
#include <Graphics/RayTracing/RayReflectionResources.hpp>

namespace fbzz::renderer {
class RayReflectionReconstructionPass final : public IRenderPass {
public:
    RayReflectionReconstructionPass(RayReflectionViewResources& state, ResourceHandle<ShaderTag> shader, bool spatial)
        : m_state(state), m_shader(shader), m_spatial(spatial) {}
    std::string_view Name() const override { return m_spatial ? "RayReflectionSpatial" : "RayReflectionTemporal"; }
    void Setup(PassBuilder& builder, const RenderPassContext& context) const override;
    void Execute(PassResources& resources, RenderPassContext& context) override;
private:
    RayReflectionViewResources& m_state;
    ResourceHandle<ShaderTag> m_shader;
    bool m_spatial;
};
} /// @note namespace fbzz::renderer
