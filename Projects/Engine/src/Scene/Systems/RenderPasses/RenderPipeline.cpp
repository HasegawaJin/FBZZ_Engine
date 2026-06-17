// FBZZ Engine
// RenderPipeline.cpp | fbzz::scene
// IRenderPass / raw pass の収集と RenderGraph への組み込み・実行
#include "Engine/Scene/Systems/RenderPasses/RenderPipeline.hpp"
#include "Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp"
#include <Engine/Profiler/ProfileScope.hpp>

namespace fbzz::scene {

void RenderPipeline::AddRawPass(
    std::string_view name,
    std::initializer_list<std::string_view> reads,
    std::initializer_list<std::string_view> writes,
    renderer::RenderGraph::ExecuteFn fn,
    bool allowCulling)
{
    using U  = renderer::RenderGraph::ResourceUsage;
    using RA = renderer::RenderGraph::ResourceAccess;
    Entry e;
    e.name         = std::string(name);
    e.allowCulling = allowCulling;
    e.fn           = std::move(fn);
    e.accesses.reserve(reads.size() + writes.size());
    for (std::string_view r : reads)
        e.accesses.push_back(RA{ std::string(r), U::Read });
    for (std::string_view w : writes)
        e.accesses.push_back(RA{ std::string(w), U::Write });
    m_entries.push_back(std::move(e));
}

void RenderPipeline::AddRawPass(
    std::string_view name,
    std::initializer_list<renderer::RenderGraph::ResourceAccess> accesses,
    renderer::RenderGraph::ExecuteFn fn,
    bool allowCulling)
{
    Entry e;
    e.name         = std::string(name);
    e.allowCulling = allowCulling;
    e.fn           = std::move(fn);
    e.accesses.assign(accesses.begin(), accesses.end());
    m_entries.push_back(std::move(e));
}

void RenderPipeline::AddRawPass(
    std::string_view name,
    std::vector<renderer::RenderGraph::ResourceAccess> accesses,
    renderer::RenderGraph::ExecuteFn fn,
    bool allowCulling)
{
    Entry e;
    e.name         = std::string(name);
    e.allowCulling = allowCulling;
    e.fn           = std::move(fn);
    e.accesses     = std::move(accesses);
    m_entries.push_back(std::move(e));
}

void RenderPipeline::DeclareResource(std::string_view name,
                                      renderer::RenderGraph::ResourceDesc desc)
{
    m_resources.emplace_back(std::string(name), desc);
}

void RenderPipeline::SetOutputs(std::initializer_list<std::string_view> outputs)
{
    m_outputs.clear();
    for (std::string_view o : outputs)
        m_outputs.emplace_back(o);
}

void RenderPipeline::SetGpuProfilerHooks(std::function<void(std::string_view)> begin,
                                          std::function<void(std::string_view)> end)
{
    m_gpuBegin = std::move(begin);
    m_gpuEnd   = std::move(end);
}

bool RenderPipeline::Execute(RenderPassContext& ctx)
{
    FBZZ_PROFILE_SCOPE("RenderPipeline::Execute");

    renderer::RenderGraph graph;

    for (auto& [name, desc] : m_resources)
        graph.DeclareResource(name, desc);

    for (const auto& o : m_outputs)
        graph.AddOutput(o);

    for (auto& e : m_entries) {
        if (e.pass) {
            if (!e.pass->IsEnabled(ctx))
                continue;
            IRenderPass* p = e.pass.get();
            graph.AddPass(
                p->Name(),
                p->DeclareAccesses(ctx),
                [p, &ctx] { p->Execute(ctx); },
                p->AllowCulling());
        } else if (e.fn) {
            graph.AddPass(std::string_view(e.name), e.accesses, e.fn, e.allowCulling);
        }
    }

    graph.SetGpuProfilerHooks(m_gpuBegin, m_gpuEnd);

    const bool ok = graph.Execute();
    m_lastReport = graph.GetLastReport();
    return ok;
}

} // namespace fbzz::scene
