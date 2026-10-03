/// @file    RayGameReconstructionPass.hpp
/// @brief   Game 輸送成分の境界検証付き時間・空間再構成。
/// @author  Hasegawa Jin
/// @date    2026-10-01
#pragma once
#include <Graphics/Pipeline/IRenderPass.hpp>
#include <Graphics/RayTracing/RayPathTraceResources.hpp>

namespace fbzz::renderer {

/// @note 空間処理後の表示を RAW/reference 履歴へ書き戻さない。履歴表は SRV/UAV を別実体にする。
class RayGameReconstructionPass final : public IRenderPass {
public:
    RayGameReconstructionPass(RayPathViewResources& state, ResourceHandle<ShaderTag> shader, bool spatial)
        : m_state(state), m_shader(shader), m_spatial(spatial) {}
    std::string_view Name() const override { return m_spatial ? "RayGameSpatial" : "RayGameTemporal"; }
    void Setup(PassBuilder& builder, const RenderPassContext& context) const override;
    void Execute(PassResources& resources, RenderPassContext& context) override;
private:
    RayPathViewResources& m_state;
    ResourceHandle<ShaderTag> m_shader;
    bool m_spatial = false;
};

} /// @note namespace fbzz::renderer
