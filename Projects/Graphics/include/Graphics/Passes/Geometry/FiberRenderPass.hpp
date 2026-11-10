/// @file    FiberRenderPass.hpp
/// @brief   抽出済み繊維の色・影・速度・選択描画。
/// @author  Hasegawa Jin
/// @date    2026-09-21
#pragma once
#include <Graphics/Api.hpp>
#include <Graphics/Pipeline/RenderPassContext.hpp>
#include <Graphics/Pipeline/IRenderPass.hpp>
#include <Graphics/Effects/RenderFiberInput.hpp>
namespace fbzz::renderer {
/// @note Forward は不透明と半透明が同じパスなので、その間から同じ描画関数を呼ぶ。
/// @see Docs/design/fiber-rendering.md
FBZZ_GRAPHICS_API void ExecuteFiberPass(RenderPassContext& ctx);
FBZZ_GRAPHICS_API void ExecuteFiberGBufferPass(RenderPassContext& ctx);
FBZZ_GRAPHICS_API void ExecuteFiberVelocityPass(RenderPassContext& ctx);
FBZZ_GRAPHICS_API void ExecuteFiberSelectionMask(RenderPassContext& ctx);
/// @note 呼び出し元の深度 RT とアトラス viewport を維持する。光源のビューで Fin の輪郭を評価する。
FBZZ_GRAPHICS_API void SubmitFiberShadowCasters(RenderPassContext& ctx, const PerFrameCB& lightFrame,
                             const math::Frustum& lightFrustum);

class FiberRenderPass final : public IRenderPass {
public:
    std::string_view Name() const override { return "FiberForward"; }
    FBZZ_GRAPHICS_API void Setup(PassBuilder& builder, const RenderPassContext& ctx) const override;
    FBZZ_GRAPHICS_API bool IsEnabled(const RenderPassContext& ctx) const override;
    void Execute(PassResources&, RenderPassContext& ctx) override { ExecuteFiberPass(ctx); }
};

} /// @note namespace fbzz::renderer

static_assert(sizeof(fbzz::renderer::FiberFrameCB) == 80);
static_assert(sizeof(fbzz::renderer::FiberContactCB) == 2064);
