/// @file    DebugPasses.hpp
/// @brief   デバッグ描画 IRenderPass の宣言。
/// @author  Hasegawa Jin
/// @date    2026-06-18
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

// パーティクル力場の影響体積とエミッターの発生形状をワイヤーで可視化する。
// WHY: どちらも「粒子の動きからしか推測できない見えない体積」で、VFX の調整で
//      最も当て推量になりやすい部分だった (力場の半径・向き、Sphere/Cone/Box の発生範囲)。
class VFXGizmoDebugPass final : public IRenderPass {
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
