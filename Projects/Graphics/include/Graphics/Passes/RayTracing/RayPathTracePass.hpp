/// @file    RayPathTracePass.hpp
/// @brief   Reference Path の光輸送と Progressive HDR 蓄積を記録する。
/// @author  Hasegawa Jin
/// @date    2026-10-01
#pragma once
#include <Graphics/Pipeline/IRenderPass.hpp>
#include <Graphics/RayTracing/RayPathTraceResources.hpp>

namespace fbzz::renderer {

/// @pre state はパイプライン実行終了まで移動・破棄しない。Prepare が履歴 key と reset flag を確定済み。
class RayPathTracePass final : public IRenderPass {
public:
    RayPathTracePass(RayPathViewResources& state, ResourceHandle<ShaderTag> shader)
        : m_state(state), m_shader(shader) {}
    std::string_view Name() const override { return "RayPathTrace"; }
    void Setup(PassBuilder& builder, const RenderPassContext& context) const override;
    void Execute(PassResources& resources, RenderPassContext& context) override;
private:
    RayPathViewResources& m_state;
    ResourceHandle<ShaderTag> m_shader;
};

} /// @note namespace fbzz::renderer
