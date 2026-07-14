// FBZZ Engine
// RenderPipeline.cpp | fbzz::scene
// IRenderPass / raw pass の収集と RenderGraph への組み込み・実行
#include "Engine/Scene/Systems/RenderPasses/RenderPipeline.hpp"
#include "Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp"
#include <Engine/Core/Logger.hpp>
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

void RenderPipeline::BeginBuild()
{
    // WHAT: パス本体と raw pass のラムダは RenderPassContext を参照するため毎フレーム破棄する。
    // WHY: Plan 結果とトランジェント RT プールは別の永続状態として残すことで、
    //      構成不変フレームの依存解析と D3D リソース再生成を省略できる。
    m_entries.clear();
    m_resources.clear();
    m_outputs.clear();
    m_gpuBegin = {};
    m_gpuEnd = {};
}

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
    // リソース記述の変更検出は Execute() のグラフ指紋で一括して行う。
    // WHY: 毎フレーム同じ transient 宣言を登録するだけでプールを dirty にすると、
    //      永続化した物理 RT を再利用できず毎フレーム再生成へ戻ってしまうため。
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

void RenderPipeline::ReleaseTransientPool(renderer::ResourceManager& resources)
{
    // ResourceHandle は非所有の整数 ID なので、ResourceManager 経由で明示的に解放する。
    for (auto& [group, pooled] : m_aliasGroupPool) {
        (void)group;
        if (pooled.handle.IsValid())
            resources.Release(pooled.handle);
    }
    m_aliasGroupPool.clear();
    m_nameToAliasGroup.clear();
    m_poolDirty = true;
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

    // パス構成の指紋 (パス名 + accesses + allowCulling + outputs) を FNV-1a で計算する。
    // WHY: 有効パスの index 列だけを比較すると、パス集合は同じまま DeclareAccesses() の返す
    //      内容だけが変わったフレーム (例: 設定トグルで reads が増減する) を見逃し、
    //      古い実行順・カリング結果を注入し続けてしまう。accesses まで含めて比較することで、
    //      依存関係が変わったフレームで確実に Plan() をやり直す。
    uint64_t fingerprint = 1469598103934665603ull; // FNV-1a offset basis
    const auto mixBytes = [&fingerprint](const void* data, size_t size) {
        const auto* bytes = static_cast<const unsigned char*>(data);
        for (size_t i = 0; i < size; ++i) {
            fingerprint ^= bytes[i];
            fingerprint *= 1099511628211ull; // FNV-1a prime
        }
    };
    const auto mixString = [&mixBytes](std::string_view s) {
        mixBytes(s.data(), s.size());
        const char separator = '\0'; // "ab"+"c" と "a"+"bc" を区別するための区切り
        mixBytes(&separator, 1);
    };
    // リソース寸法・形式も指紋へ含める。
    // WHY: パス依存が同じでも Viewport リサイズ時は旧寸法の物理 RT を再利用できないため。
    for (const auto& [name, desc] : m_resources) {
        mixString(name);
        mixBytes(&desc.kind, sizeof(desc.kind));
        mixBytes(&desc.width, sizeof(desc.width));
        mixBytes(&desc.height, sizeof(desc.height));
        mixBytes(&desc.format, sizeof(desc.format));
        mixBytes(&desc.external, sizeof(desc.external));
        mixBytes(&desc.transient, sizeof(desc.transient));
    }
    for (const auto& output : m_outputs)
        mixString(output);
    for (const auto& pass : graph.GetPasses()) {
        mixString(pass.name);
        mixBytes(&pass.allowCulling, sizeof(pass.allowCulling));
        for (const auto& access : pass.accesses) {
            mixString(access.name);
            mixBytes(&access.usage, sizeof(access.usage));
        }
    }

    const bool topologyChanged = !m_planValid
        || enabledNow != m_lastEnabledEntryIndices
        || fingerprint != m_lastGraphFingerprint;

    // 各 RenderGraph パスをCPU Profilerにも流し、ドライバー待機が発生したパスを特定する。
    graph.SetProfilerHooks(
        [](std::string_view name) {
            profiler::Profiler::BeginSample(
                profiler::ProfilerMarker(InternRenderPassProfileName(name), "Rendering"));
        },
        [](std::string_view) { profiler::Profiler::EndSample(); });
    graph.SetDebugLogHook([](const char* msg) { FBZZ_LOG_ERROR("%s", msg); });
    graph.SetGpuProfilerHooks(m_gpuBegin, m_gpuEnd);

    // Phase 1: Plan — トポロジが変わった場合のみ依存解決・カリング・ライフタイム解析を実行する。
    // トポロジ不変フレームでは前フレームの結果を注入して Plan() をスキップする。
    {
        FBZZ_PROFILE_SCOPE("RenderPipeline::Plan");
        if (topologyChanged) {
            if (!graph.Plan()) {
                // どのパスが有効か・カリングされたかを出力して依存関係の問題を特定する
                FBZZ_LOG_ERROR("RenderGraph::Plan() failed — dependency cycle or missing resource writer.");
                for (size_t i : enabledNow) {
                    FBZZ_LOG_ERROR("  enabled pass[%zu]: %s", i, m_entries[i].name.c_str());
                }
                return false;
            }
            m_lastEnabledEntryIndices = std::move(enabledNow);
            m_lastGraphFingerprint    = fingerprint;
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

    return ok;
}

} // namespace fbzz::scene
