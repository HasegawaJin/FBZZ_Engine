/// @file    RenderPipeline.hpp
/// @brief   Engine 拡張パスを Graphics パイプラインへ登録するアダプター。
/// @author  Hasegawa Jin
/// @date    2026-06-18
#pragma once
#include <Graphics/Pipeline/RenderPipeline.hpp>
#include <Engine/Scene/Systems/RenderPasses/IRenderPass.hpp>
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <type_traits>
namespace fbzz::scene {
using renderer::LambdaPass;
class RenderPipeline : public renderer::RenderPipeline {
    template<typename T> class HostPass final : public renderer::IRenderPass {
    public:
        template<typename... Args> HostPass(RenderPassContext*& context, Args&&... args)
            : m_context(context), m_pass(std::forward<Args>(args)...) {}
        std::string_view Name() const override { return m_pass.Name(); }
        void Setup(renderer::PassBuilder& builder, const renderer::RenderPassContext&) const override { m_pass.Setup(builder, *m_context); }
        bool IsEnabled(const renderer::RenderPassContext&) const override { return m_pass.IsEnabled(*m_context); }
        bool AllowCulling() const override { return m_pass.AllowCulling(); }
        void Execute(renderer::PassResources& resources, renderer::RenderPassContext&) override { m_pass.Execute(resources, *m_context); }
    private:
        RenderPassContext*& m_context;
        T m_pass;
    };
public:
    RenderPipeline() = default;
    /// @note Graphics のビューが持つ Plan/RT キャッシュを当該フレームだけ借りる。
    explicit RenderPipeline(renderer::RenderPipeline&& state)
        : renderer::RenderPipeline(std::move(state)) {}
    template<typename T, typename... Args> void AddPass(Args&&... args) {
        if constexpr (std::is_base_of_v<renderer::IRenderPass, T>)
            renderer::RenderPipeline::AddPass<T>(std::forward<Args>(args)...);
        else renderer::RenderPipeline::AddPass<HostPass<T>>(m_context, std::forward<Args>(args)...);
    }
    bool Execute(RenderPassContext& context, renderer::RenderPassCapture* capture = nullptr,
                 const renderer::GpuProfilerViewMetadata* gpuView = nullptr,
                 const renderer::ResolvedRenderPlan* gpuPlan = nullptr) {
        m_context = &context;
        const bool result = renderer::RenderPipeline::Execute(context, capture, gpuView, gpuPlan);
        m_context = nullptr;
        return result;
    }
private:
    RenderPassContext* m_context = nullptr;
};
}
