/// @file    RenderPipeline.cpp
/// @brief   IRenderPass / raw pass の収集と RenderGraph への組み込み・実行。
/// @author  Hasegawa Jin
/// @date    2026-06-18
#include "Engine/Scene/Systems/RenderPasses/RenderPipeline.hpp"
#include "Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp"
#include <Engine/Scene/Systems/RenderPasses/RenderPassCapture.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Profiler/ProfileScope.hpp>
#include <Engine/Renderer/RenderBindingGuard.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <algorithm>
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
    // WHY: Plan 結果は別の永続状態として残すことで、構成不変フレームの依存解析を省略できる。
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
    std::vector<RA> accesses;
    accesses.reserve(reads.size() + writes.size());
    for (std::string_view r : reads)
        accesses.push_back(RA{ std::string(r), U::Read });
    for (std::string_view w : writes)
        accesses.push_back(RA{ std::string(w), U::Write });
    m_entries.push_back(
        std::make_unique<LambdaPass>(name, std::move(accesses), std::move(fn), allowCulling));
}

void RenderPipeline::AddRawPass(
    std::string_view name,
    std::initializer_list<renderer::RenderGraph::ResourceAccess> accesses,
    renderer::RenderGraph::ExecuteFn fn,
    bool allowCulling)
{
    m_entries.push_back(std::make_unique<LambdaPass>(
        name,
        std::vector<renderer::RenderGraph::ResourceAccess>(accesses.begin(), accesses.end()),
        std::move(fn), allowCulling));
}

void RenderPipeline::AddRawPass(
    std::string_view name,
    std::vector<renderer::RenderGraph::ResourceAccess> accesses,
    renderer::RenderGraph::ExecuteFn fn,
    bool allowCulling)
{
    m_entries.push_back(
        std::make_unique<LambdaPass>(name, std::move(accesses), std::move(fn), allowCulling));
}

void RenderPipeline::AddRawPass(
    std::string_view name,
    std::initializer_list<std::string_view> reads,
    std::initializer_list<std::string_view> writes,
    LambdaPass::PassFn fn,
    bool allowCulling)
{
    using U  = renderer::RenderGraph::ResourceUsage;
    using RA = renderer::RenderGraph::ResourceAccess;
    std::vector<RA> accesses;
    accesses.reserve(reads.size() + writes.size());
    for (std::string_view r : reads)
        accesses.push_back(RA{ std::string(r), U::Read });
    for (std::string_view w : writes)
        accesses.push_back(RA{ std::string(w), U::Write });
    m_entries.push_back(
        std::make_unique<LambdaPass>(name, std::move(accesses), std::move(fn), allowCulling));
}

void RenderPipeline::AddRawPass(
    std::string_view name,
    std::initializer_list<renderer::RenderGraph::ResourceAccess> accesses,
    LambdaPass::PassFn fn,
    bool allowCulling)
{
    m_entries.push_back(std::make_unique<LambdaPass>(
        name,
        std::vector<renderer::RenderGraph::ResourceAccess>(accesses.begin(), accesses.end()),
        std::move(fn), allowCulling));
}

void RenderPipeline::DeclareResource(std::string_view name,
                                      renderer::RenderGraph::ResourceDesc desc)
{
    // 申告は依存解析と寿命解析のためのもの。実体の確保は伴わない。
    // 記述の変更検出は Execute() のグラフ指紋で一括して行う。
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

void RenderPipeline::ReleaseViewResources(renderer::ResourceManager& resources)
{
    // 毒用の RT は呼び出し元 (ReleaseViewRenderTargets) がこの直後に RenderPipeline ごと
    // 作り直すので、ここで返さないとハンドルを握ったまま消える。
    m_bindingPoisonRT.Release(resources);
}

bool RenderPipeline::Execute(RenderPassContext& ctx, RenderPassCapture* capture)
{
    FBZZ_PROFILE_SCOPE("RenderPipeline::Execute");

    // 有効パスのインデックス列を先に確定し、前フレームと比較してトポロジ変化を検出する。
    std::vector<size_t> enabledNow;
    enabledNow.reserve(m_entries.size());
    for (size_t i = 0; i < m_entries.size(); ++i) {
        if (m_entries[i] && m_entries[i]->IsEnabled(ctx))
            enabledNow.push_back(i);
    }

    // 申告はこのフレームで 1 回だけ引く。Setup は Execute からも参照するので、
    // グラフへ渡した後も生き続ける場所へ置く (ラムダは参照で掴む)。
    m_setups.assign(m_entries.size(), {});
    for (const size_t i : enabledNow) {
        PassBuilder builder;
        m_entries[i]->Setup(builder, ctx);
        m_setups[i].accesses   = builder.Accesses();
        m_setups[i].autoTarget = builder.AutoTarget();
    }

    renderer::RenderGraph graph;

    // 束縛毒 (RenderBindingGuard)。立っているときだけ 1x1 の RT を用意し、
    // 各パスの実行直前に束縛して «前のパスが残した RT» を当てにできなくする。
    const bool poisonBindings = renderer::bindingguard::IsEnabled();
    if (poisonBindings)
        m_bindingPoisonRT.Ensure(ctx.resources, 1, 1, 1);

    {
        FBZZ_PROFILE_SCOPE("RenderPipeline::BuildGraph");
        for (auto& [name, desc] : m_resources)
            graph.DeclareResource(name, desc);

        for (const auto& o : m_outputs)
            graph.AddOutput(o);

        const auto poison = m_bindingPoisonRT.Handle();
        for (const size_t i : enabledNow) {
            IRenderPass* pass  = m_entries[i].get();
            const PassSetup& s = m_setups[i];
            graph.AddPass(
                pass->Name(),
                s.accesses,
                [this, pass, &s, &ctx, poisonBindings, poison] {
                    // 束縛毒。前のパスが残した RT を当てにできなくする。
                    if (poisonBindings)
                        ctx.renderer.SetRenderTarget(poison, ctx.resources);
                    // 申告した書き先を自動束縛する (SetAutoTarget を呼んだパスだけ)。
                    if (!s.autoTarget.empty()) {
                        const auto target = ctx.resourceRegistry.Target(s.autoTarget);
                        if (target.IsValid())
                            ctx.renderer.SetRenderTarget(target, ctx.resources);
                    }
                    PassResources resources(ctx.resourceRegistry, s.accesses, pass->Name());
                    // 自由関数へ散ったパス本体からも ctx.Res() で引けるようにする。
                    // パスは順に実行されるので、実行中の 1 本だけが差さっている。
                    ctx.passResources = &resources;
                    pass->Execute(resources, ctx);
                    ctx.passResources = nullptr;
                },
                pass->AllowCulling());
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
                    const std::string_view name = m_entries[i]->Name();
                    FBZZ_LOG_ERROR("  enabled pass[%zu]: %.*s", i,
                                   static_cast<int>(name.size()), name.data());
                }
                return false;
            }
            m_planValid = true;
            // 構成が変わったときだけ作る。毎フレーム作ると文字列連結が乗るうえ、
            // 差分を見たいのは «変わった瞬間» だけ。
            m_lastPlanDescription = graph.DescribeLastPlan();
        } else {
            graph.InjectPlan(m_lastReport);
        }
    }

    if (capture) {
        capture->Begin(graph.GetPasses(), graph.GetLastReport());
        graph.SetPassCompletedHook([capture, &ctx](size_t index) { capture->Capture(index, ctx); });
    }

    bool ok = false;
    {
        FBZZ_PROFILE_SCOPE("RenderPipeline::GraphExecute");
        ok = graph.Execute();
    }
    m_lastReport = graph.GetLastReport();

    return ok;
}

} // namespace fbzz::scene
