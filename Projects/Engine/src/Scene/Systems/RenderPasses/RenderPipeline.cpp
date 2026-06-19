// FBZZ Engine
// RenderPipeline.cpp | fbzz::scene
// IRenderPass / raw pass の収集と RenderGraph への組み込み・実行
#include "Engine/Scene/Systems/RenderPasses/RenderPipeline.hpp"
#include "Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp"
#include <Engine/Profiler/ProfileScope.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <unordered_set>

namespace fbzz::scene {

namespace {

// 動的な RenderPass 名を ProfilerRecord の寿命より長く保持する。
// WHY: ProfilerMarker は const char* を保持するため、フレーム末尾で破棄される
//      RenderGraph 内部文字列を直接渡すと AnalysisPanel がダングリングポインターを読む。
const char* InternRenderPassProfileName(std::string_view name)
{
    static std::unordered_set<std::string> names;
    const auto [it, inserted] = names.emplace(name);
    (void)inserted;
    return it->c_str();
}

} // namespace

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
    // transient リソースが追加・変更されたらプールを再構築するフラグを立てる。
    if (desc.transient) MarkPoolDirty();
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

void RenderPipeline::RebuildTransientPool(
    const renderer::RenderGraph::ExecutionReport& report,
    renderer::ResourceManager& resources)
{
    // WHAT: ライフタイム解析で同一 aliasGroup に割り当てられたトランジェントリソースは
    //       1 つの物理 RT を共有できる。グループごとに RT を 1 つ確保し、
    //       m_nameToAliasGroup でリソース名から高速にハンドルを引けるようにする。
    for (auto& [group, pr] : m_aliasGroupPool)
        resources.Release(pr.handle);
    m_aliasGroupPool.clear();
    m_nameToAliasGroup.clear();

    for (const auto& lt : report.lifetimes) {
        if (lt.desc.external || !lt.desc.transient) continue;
        if (lt.aliasGroup < 0) continue;

        m_nameToAliasGroup[lt.name] = lt.aliasGroup;

        if (m_aliasGroupPool.contains(lt.aliasGroup)) continue;

        // このグループ用に物理 RT を 1 つ確保する。
        // WHY: aliasGroup が同じリソースはライフタイムが重ならないため、
        //      同一の物理 RT バッファを順番に使い回してもアクセス競合が起きない。
        const auto& desc = lt.desc;
        const uint32_t w = desc.width  > 0 ? desc.width  : 1;
        const uint32_t h = desc.height > 0 ? desc.height : 1;
        PooledRT pr;
        pr.desc   = desc;
        pr.handle = resources.CreateRenderTarget(w, h, 1);
        m_aliasGroupPool.emplace(lt.aliasGroup, std::move(pr));
    }

    m_poolDirty = false;
}

renderer::ResourceHandle<renderer::RenderTargetTag>
RenderPipeline::GetTransientRT(std::string_view name) const
{
    auto it = m_nameToAliasGroup.find(std::string(name));
    if (it == m_nameToAliasGroup.end())
        return {};
    auto poolIt = m_aliasGroupPool.find(it->second);
    if (poolIt == m_aliasGroupPool.end())
        return {};
    return poolIt->second.handle;
}

bool RenderPipeline::Execute(RenderPassContext& ctx)
{
    FBZZ_PROFILE_SCOPE("RenderPipeline::Execute");

    // 有効パスのインデックス列を先に確定し、前フレームと比較してトポロジ変化を検出する。
    std::vector<size_t> enabledNow;
    enabledNow.reserve(m_entries.size());
    for (size_t i = 0; i < m_entries.size(); ++i) {
        const auto& e = m_entries[i];
        if (e.pass ? e.pass->IsEnabled(ctx) : bool(e.fn))
            enabledNow.push_back(i);
    }
    const bool topologyChanged = !m_planValid || (enabledNow != m_lastEnabledEntryIndices);

    renderer::RenderGraph graph;

    {
        FBZZ_PROFILE_SCOPE("RenderPipeline::BuildGraph");
        for (auto& [name, desc] : m_resources)
            graph.DeclareResource(name, desc);

        for (const auto& o : m_outputs)
            graph.AddOutput(o);

        for (size_t i : enabledNow) {
            auto& e = m_entries[i];
            if (e.pass) {
                IRenderPass* p = e.pass.get();
                graph.AddPass(
                    p->Name(),
                    p->DeclareAccesses(ctx),
                    [p, &ctx] { p->Execute(ctx); },
                    p->AllowCulling());
            } else {
                graph.AddPass(std::string_view(e.name), e.accesses, e.fn, e.allowCulling);
            }
        }
    }

    // 各 RenderGraph パスをCPU Profilerにも流し、ドライバー待機が発生したパスを特定する。
    graph.SetProfilerHooks(
        [](std::string_view name) {
            profiler::Profiler::BeginSample(
                profiler::ProfilerMarker(InternRenderPassProfileName(name), "Rendering"));
        },
        [](std::string_view) { profiler::Profiler::EndSample(); });
    graph.SetGpuProfilerHooks(m_gpuBegin, m_gpuEnd);

    // Phase 1: Plan — トポロジが変わった場合のみ依存解決・カリング・ライフタイム解析を実行する。
    // トポロジ不変フレームでは前フレームの結果を注入して Plan() をスキップする。
    {
        FBZZ_PROFILE_SCOPE("RenderPipeline::Plan");
        if (topologyChanged) {
            if (!graph.Plan()) return false;
            m_lastEnabledEntryIndices = std::move(enabledNow);
            m_planValid = true;
            m_poolDirty = true; // トポロジ変化時はプールも必ず再構築する
        } else {
            graph.InjectPlan(m_lastReport);
        }
    }

    // Phase 2: プールが古い場合 (パイプライン構成変更後の初回フレーム) に再構築する。
    if (m_poolDirty) {
        FBZZ_PROFILE_SCOPE("RenderPipeline::RebuildTransientPool");
        RebuildTransientPool(graph.GetLastReport(), ctx.resources);
    }

    // Phase 3: ctx.getTransientRT をパイプラインのプールに接続してからコールバックを実行する。
    // WHY: 各パスコールバックが ctx.getTransientRT(name) でハンドルを取得できるように、
    //      Execute() より前にラムダを設定しておく必要がある。
    ctx.getTransientRT = [this](std::string_view name) {
        return GetTransientRT(name);
    };

    bool ok = false;
    {
        FBZZ_PROFILE_SCOPE("RenderPipeline::GraphExecute");
        ok = graph.Execute();
    }
    m_lastReport = graph.GetLastReport();

    // トランジェント RT を解放する。
    // WHY: RenderPipeline はフレームごとにスタック上で生成・破棄される。
    //      ResourceHandle は整数 ID に過ぎず、デストラクタは ResourceManager に
    //      通知しないため、Execute 完了後に明示的に解放しないと D3D11 リソースが漏れる。
    {
        FBZZ_PROFILE_SCOPE("RenderPipeline::ReleaseTransientPool");
        for (auto& [group, pr] : m_aliasGroupPool)
            ctx.resources.Release(pr.handle);
        m_aliasGroupPool.clear();
        m_nameToAliasGroup.clear();
    }

    return ok;
}

} // namespace fbzz::scene
