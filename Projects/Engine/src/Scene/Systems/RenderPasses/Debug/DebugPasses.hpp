// FBZZ Engine
// DebugPasses.hpp | fbzz::scene
// デバッグ描画 IRenderPass の宣言
#pragma once
#include <Engine/Scene/Systems/RenderPasses/IRenderPass.hpp>

namespace fbzz::scene {

class DebugCollidersPass final : public IRenderPass {
public:
    std::string_view Name() const override;
    std::vector<renderer::RenderGraph::ResourceAccess> DeclareAccesses(const RenderPassContext&) const override;
    void Execute(RenderPassContext& ctx) override;
};

class AnimatorDebugPass final : public IRenderPass {
public:
    std::string_view Name() const override;
    std::vector<renderer::RenderGraph::ResourceAccess> DeclareAccesses(const RenderPassContext&) const override;
    void Execute(RenderPassContext& ctx) override;
};

class GridDebugPass final : public IRenderPass {
public:
    std::string_view Name() const override;
    std::vector<renderer::RenderGraph::ResourceAccess> DeclareAccesses(const RenderPassContext&) const override;
    void Execute(RenderPassContext& ctx) override;
};

class LightRangeDebugPass final : public IRenderPass {
public:
    std::string_view Name() const override;
    std::vector<renderer::RenderGraph::ResourceAccess> DeclareAccesses(const RenderPassContext&) const override;
    void Execute(RenderPassContext& ctx) override;
};

class TerrainCollisionDebugPass final : public IRenderPass {
public:
    std::string_view Name() const override;
    std::vector<renderer::RenderGraph::ResourceAccess> DeclareAccesses(const RenderPassContext&) const override;
    void Execute(RenderPassContext& ctx) override;
};

class NavMeshDebugPass final : public IRenderPass {
public:
    std::string_view Name() const override;
    std::vector<renderer::RenderGraph::ResourceAccess> DeclareAccesses(const RenderPassContext&) const override;
    void Execute(RenderPassContext& ctx) override;
};

class DecalDebugPass final : public IRenderPass {
public:
    std::string_view Name() const override;
    std::vector<renderer::RenderGraph::ResourceAccess> DeclareAccesses(const RenderPassContext&) const override;
    void Execute(RenderPassContext& ctx) override;
};

class ConstraintDebugPass final : public IRenderPass {
public:
    std::string_view Name() const override;
    std::vector<renderer::RenderGraph::ResourceAccess> DeclareAccesses(const RenderPassContext&) const override;
    void Execute(RenderPassContext& ctx) override;
};

} // namespace fbzz::scene
