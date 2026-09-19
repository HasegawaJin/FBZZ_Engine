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

/// @brief 動的な RenderPass 名を ProfilerRecord の寿命より長く保持する。
/// @note ProfilerMarker は const char* を保持するため、フレーム末尾で破棄される RenderGraph 内部文字列を直接渡すと AnalysisPanel がダングリングポインターを読む。
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
    /// @note パス本体と raw pass のラムダは RenderPassContext を参照するため毎フレーム破棄する。Plan 結果は別の永続状態として残し、構成不変フレームでは依存解析を省略できる。
    m_entries.clear();
    m_resources.clear();
    m_outputs.clear();
    m_gpuBegin = {};
    m_gpuEnd = {};
}

std::vector<renderer::RenderGraph::ResourceAccess> RenderPipeline::MakeAccesses(
    std::initializer_list<std::string_view> reads,
    std::initializer_list<std::string_view> writes)
{
    using U  = renderer::RenderGraph::ResourceUsage;
    using RA = renderer::RenderGraph::ResourceAccess;
    std::vector<RA> accesses;
    accesses.reserve(reads.size() + writes.size());
    for (std::string_view r : reads)
        accesses.push_back(RA{ std::string(r), U::Read });
    for (std::string_view w : writes)
        accesses.push_back(RA{ std::string(w), U::Write });
    return accesses;
}

void RenderPipeline::DeclareTarget(std::string_view name,
                                    renderer::ResourceHandle<renderer::RenderTargetTag> handle,
                                    renderer::RenderGraph::ResourceDesc desc)
{
    /// @note 確保はしない。実体は呼び出し側 (ViewRenderTargets) が持ち、ここは «その名前が
    ///       どれを指すか» を申告と一緒に預かるだけ。記述の変更検出は Execute() の指紋が行う。
    m_resources.push_back({ std::string(name), desc, handle, {} });
}

void RenderPipeline::DeclareTexture(std::string_view name,
                                     renderer::ResourceHandle<renderer::TextureTag> handle,
                                     renderer::RenderGraph::ResourceDesc desc)
{
    m_resources.push_back({ std::string(name), desc, {}, handle });
}

void RenderPipeline::BindDeclaredResources(RenderPassContext& ctx) const
{
    /// @note 登録簿はこのフレームの宣言から «導かれる» もの。別に持ち回らない。
    ctx.resourceRegistry.Clear();
    for (const auto& resource : m_resources) {
        if (resource.target.IsValid())
            ctx.resourceRegistry.BindTarget(resource.name, resource.target);
        if (resource.texture.IsValid())
            ctx.resourceRegistry.BindTexture(resource.name, resource.texture);
    }
}

void RenderPipeline::SetOutputs(std::initializer_list<std::string_view> outputs)
{
    m_outputs.clear();
    for (std::string_view o : outputs)
        m_outputs.emplace_back(o);
}

void RenderPipeline::SetPassOverrides(std::vector<renderer::RenderPassOverride> overrides)
{
    m_overrides = std::move(overrides);
}

void RenderPipeline::ApplyOverride(const renderer::RenderPassOverride& over,
                                   std::vector<renderer::RenderGraph::ResourceAccess>& accesses,
                                   bool& allowCulling)
{
    allowCulling = allowCulling && over.allowCulling;

    /// @note 追加の Read は «このパスはこれを待つ» という申告。順序を直に書かせない
    ///       代わりに依存を足させるので、実行順が動いても理由がグラフに残る。
    for (const std::string& read : over.extraReads) {
        const bool declared = std::any_of(accesses.begin(), accesses.end(),
            [&read](const renderer::RenderGraph::ResourceAccess& access) {
                return access.name == read;
            });
        if (!declared)
            accesses.push_back({ read, renderer::RenderGraph::ResourceUsage::Read });
    }
}

const renderer::RenderPassOverride* RenderPipeline::FindOverride(std::string_view name) const
{
    const auto it = std::find_if(m_overrides.begin(), m_overrides.end(),
        [name](const renderer::RenderPassOverride& entry) { return entry.name == name; });
    return it == m_overrides.end() ? nullptr : &*it;
}

void RenderPipeline::SetGpuProfilerHooks(std::function<void(std::string_view)> begin,
                                          std::function<void(std::string_view)> end)
{
    m_gpuBegin = std::move(begin);
    m_gpuEnd   = std::move(end);
}

void RenderPipeline::ReleaseViewResources(renderer::ResourceManager& resources)
{
    /// @note 毒用の RT は呼び出し元 (ReleaseViewRenderTargets) がこの直後に RenderPipeline ごと
    ///       作り直すので、ここで返さないとハンドルを握ったまま消える。
    m_bindingPoisonRT.Release(resources);
}

std::vector<size_t> RenderPipeline::CollectEnabledSetups(RenderPassContext& ctx)
{
    std::vector<size_t> enabled;
    enabled.reserve(m_entries.size());
    for (size_t i = 0; i < m_entries.size(); ++i) {
        if (!m_entries[i] || !m_entries[i]->IsEnabled(ctx))
            continue;
        const auto* over = FindOverride(m_entries[i]->Name());
        if (over && !over->enabled)
            continue;
        enabled.push_back(i);
    }

    /// @note 申告はこのフレームで 1 回だけ引く。Setup は Execute からも参照するので、グラフへ渡した後も生き続ける場所へ置く (ラムダは参照で掴む)。
    /// @note 要素ごと作り直さない。assign すると全パスぶんの vector / string を毎フレーム確保し直すことになるため、中身だけ空にして容量は残す。
    m_setups.resize(m_entries.size());
    for (auto& setup : m_setups) {
        setup.accesses.clear();
        setup.autoTarget.clear();
        setup.allowCulling = true;
    }
    for (const size_t i : enabled) {
        PassBuilder builder;
        m_entries[i]->Setup(builder, ctx);
        PassSetup& setup = m_setups[i];
        builder.MoveOut(setup.accesses, setup.autoTarget);
        setup.allowCulling = m_entries[i]->AllowCulling();

        if (const auto* over = FindOverride(m_entries[i]->Name()))
            ApplyOverride(*over, setup.accesses, setup.allowCulling);
    }
    return enabled;
}

void RenderPipeline::GraphFingerprint::MixBytes(const void* data, size_t size)
{
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < size; ++i) {
        m_value ^= bytes[i];
        /// @note FNV-1a prime
        m_value *= 1099511628211ull;
    }
}

void RenderPipeline::GraphFingerprint::MixString(std::string_view text)
{
    MixBytes(text.data(), text.size());
    const char separator = '\0';
    MixBytes(&separator, 1);
}

void RenderPipeline::GraphFingerprint::MixResource(
    std::string_view name, const renderer::RenderGraph::ResourceDesc& desc)
{
    MixString(name);
    MixBytes(&desc.kind, sizeof(desc.kind));
    MixBytes(&desc.width, sizeof(desc.width));
    MixBytes(&desc.height, sizeof(desc.height));
    MixBytes(&desc.format, sizeof(desc.format));
    MixBytes(&desc.colorCount, sizeof(desc.colorCount));
    MixBytes(&desc.withDepth, sizeof(desc.withDepth));
    MixBytes(&desc.external, sizeof(desc.external));
    MixBytes(&desc.transient, sizeof(desc.transient));
}

void RenderPipeline::GraphFingerprint::MixOutput(std::string_view name)
{
    MixString(name);
}

void RenderPipeline::GraphFingerprint::MixPass(
    std::string_view name, bool allowCulling,
    const std::vector<renderer::RenderGraph::ResourceAccess>& accesses)
{
    MixString(name);
    MixBytes(&allowCulling, sizeof(allowCulling));
    for (const auto& access : accesses) {
        MixString(access.name);
        MixBytes(&access.usage, sizeof(access.usage));
    }
}

uint64_t RenderPipeline::ComputeGraphFingerprint(const std::vector<size_t>& enabled) const
{
    GraphFingerprint fingerprint;
    /// @note 実行順の決め方そのものも鍵に含める。方針が変われば同じ申告でも別の順序になる。
    fingerprint.MixOutput(m_policy == renderer::RenderGraphSchedulePolicy::MinimizeLifetimes
                          ? "policy:MinimizeLifetimes" : "policy:RegistrationOrder");
    /// @note 寸法も混ぜる。パス依存が同じでも Viewport リサイズ後は旧寸法の
    ///       物理 RT を再利用できないため。
    for (const auto& resource : m_resources)
        fingerprint.MixResource(resource.name, resource.desc);
    for (const auto& output : m_outputs)
        fingerprint.MixOutput(output);
    for (const size_t i : enabled)
        fingerprint.MixPass(m_entries[i]->Name(), m_setups[i].allowCulling, m_setups[i].accesses);
    return fingerprint.Value();
}

void RenderPipeline::BuildGraph(renderer::RenderGraph& graph,
                                RenderPassContext& ctx,
                                const std::vector<size_t>& enabled)
{
    FBZZ_PROFILE_SCOPE("RenderPipeline::BuildGraph");

    /// @note 束縛毒 (RenderBindingGuard)。立っているときだけ 1x1 の RT を用意し、
    ///       各パスの実行直前に束縛して «前のパスが残した RT» を当てにできなくする。
    const bool poisonBindings = renderer::bindingguard::IsEnabled();
    if (poisonBindings)
        m_bindingPoisonRT.Ensure(ctx.resources, 1, 1, 1);
    const auto poison = m_bindingPoisonRT.Handle();

    graph.SetSchedulePolicy(m_policy == renderer::RenderGraphSchedulePolicy::MinimizeLifetimes
        ? renderer::RenderGraph::SchedulePolicy::MinimizeLifetimes
        : renderer::RenderGraph::SchedulePolicy::RegistrationOrder);

    for (const auto& resource : m_resources)
        graph.DeclareResource(resource.name, resource.desc);

    for (const auto& o : m_outputs)
        graph.AddOutput(o);

    for (const size_t i : enabled) {
        IRenderPass* pass  = m_entries[i].get();
        const PassSetup& s = m_setups[i];
        graph.AddPass(
            pass->Name(),
            s.accesses,
            [pass, &s, &ctx, poisonBindings, poison] {
                if (poisonBindings)
                    ctx.renderer.SetRenderTarget(poison, ctx.resources);
                /// @note 申告した書き先を自動束縛する (SetAutoTarget を呼んだパスだけ)。
                if (!s.autoTarget.empty()) {
                    const auto target = ctx.resourceRegistry.Target(s.autoTarget);
                    if (target.IsValid())
                        ctx.renderer.SetRenderTarget(target, ctx.resources);
                }
                PassResources resources(ctx.resourceRegistry, s.accesses, pass->Name());
                /// @note 自由関数へ散ったパス本体からも ctx.Res() で引けるようにする。
                ///       パスは順に実行されるので、実行中の 1 本だけが差さっている。
                ctx.passResources = &resources;
                pass->Execute(resources, ctx);
                ctx.passResources = nullptr;
            },
            s.allowCulling);
    }
}

bool RenderPipeline::Execute(RenderPassContext& ctx, RenderPassCapture* capture)
{
    FBZZ_PROFILE_SCOPE("RenderPipeline::Execute");

    BindDeclaredResources(ctx);

    const std::vector<size_t> enabledNow = CollectEnabledSetups(ctx);
    const uint64_t fingerprint = ComputeGraphFingerprint(enabledNow);
    const bool topologyChanged = !m_planValid
        || enabledNow != m_lastEnabledEntryIndices
        || fingerprint != m_lastGraphFingerprint;

    renderer::RenderGraph graph;
    BuildGraph(graph, ctx, enabledNow);

    /// @note 各 RenderGraph パスをCPU Profilerにも流し、ドライバー待機が発生したパスを特定する。
    graph.SetProfilerHooks(
        [](std::string_view name) {
            profiler::Profiler::BeginSample(
                profiler::ProfilerMarker(InternRenderPassProfileName(name), "Rendering"));
        },
        [](std::string_view) { profiler::Profiler::EndSample(); });
    graph.SetDebugLogHook([](const char* msg) { FBZZ_LOG_ERROR("%s", msg); });
    graph.SetGpuProfilerHooks(m_gpuBegin, m_gpuEnd);

    /// @note Phase 1: Plan — トポロジが変わった場合のみ依存解決・カリング・ライフタイム解析を実行する。
    ///       トポロジ不変フレームでは前フレームの結果を注入して Plan() をスキップする。
    {
        FBZZ_PROFILE_SCOPE("RenderPipeline::Plan");
        if (topologyChanged) {
            if (!graph.Plan()) {
                m_planValid = false;
                /// @note どのパスが有効か・カリングされたかを出力して依存関係の問題を特定する
                FBZZ_LOG_ERROR("RenderGraph::Plan() failed — dependency cycle or missing resource writer.");
                for (size_t i : enabledNow) {
                    const std::string_view name = m_entries[i]->Name();
                    FBZZ_LOG_ERROR("  enabled pass[%zu]: %.*s", i,
                                   static_cast<int>(name.size()), name.data());
                }
                return false;
            }
            m_planValid = true;
            /// @note 構成が変わったときだけ作る。毎フレーム作ると文字列連結が乗るうえ、
            ///       差分を見たいのは «変わった瞬間» だけ。
            m_lastPlanDescription = graph.DescribeLastPlan();

            /// @note 鍵はここで控える。控え忘れると «前フレームと同じ» が永久に成立せず、構成不変フレームでも Plan と構成テキスト生成が毎フレーム走る (InjectPlan が一度も通らない)。
            m_lastEnabledEntryIndices = enabledNow;
            m_lastGraphFingerprint    = fingerprint;
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
