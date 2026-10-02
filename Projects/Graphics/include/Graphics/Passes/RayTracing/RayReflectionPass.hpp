/// @file    RayReflectionPass.hpp
/// @brief   GBuffer を起点に静的定数 PBR 表面の一段反射を生成する。
/// @author  Hasegawa Jin
/// @date    2026-10-01
#pragma once
#include <Graphics/Pipeline/IRenderPass.hpp>
#include <Graphics/RayTracing/RayReflectionResources.hpp>

namespace fbzz::renderer {

/// @note 能力・表面・照明の被覆はパイプライン側で検証する。miss と未対応ヒットの alpha は 0。
class RayReflectionPass final : public IRenderPass {
public:
    RayReflectionPass(const RaySceneGpu& scene, ResourceHandle<ConstantBufferTag> constants,
        ResourceHandle<ShaderTag> shader, bool incomplete = false, RayReflectionLightingResources lighting = {},
        RayReflectionReconstructionViewResources* reconstruction = nullptr)
        : m_scene(scene), m_constants(constants), m_shader(shader), m_incomplete(incomplete), m_lighting(lighting),
          m_reconstruction(reconstruction) {}
    std::string_view Name() const override { return "RayReflection"; }
    void Setup(PassBuilder& builder, const RenderPassContext& context) const override;
    void Execute(PassResources& resources, RenderPassContext& context) override;
private:
    RaySceneGpu m_scene;
    ResourceHandle<ConstantBufferTag> m_constants;
    ResourceHandle<ShaderTag> m_shader;
    bool m_incomplete = false;
    RayReflectionLightingResources m_lighting;
    RayReflectionReconstructionViewResources* m_reconstruction = nullptr;
};

} /// @note namespace fbzz::renderer
