/// @file    RenderPipeline.hpp
/// @brief   IRenderPass の登録と renderer::RenderGraph への変換を担う実行パイプライン。
/// @author  Hasegawa Jin
/// @date    2026-06-18
///
/// AddPass<T>() で型付きパスを、AddRawPass() でラムダ式パスをフレームごとに登録し、
/// Execute(ctx) が呼ばれた時点ですべてのパスを RenderGraph に組み込んで実行する。
/// 追加順が RenderGraph 上の優先度になる (依存関係が同一の場合のタイブレーク)。
///
/// リソースの実体はまだ ViewRenderTargets が持つ。パスは名前で引き
/// (PassResources::Target / Texture)、名前 → 実体の対応は RenderPassContext の
/// 登録簿が持つ。DeclareResource は依存解析と寿命解析のための申告で、
/// 実体の確保は伴わない。
#pragma once
#include "IRenderPass.hpp"
#include <Engine/Renderer/RenderGraph.hpp>
#include <Engine/Renderer/ResourceHandle.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <functional>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace fbzz::renderer { class ResourceManager; }

namespace fbzz::scene {

struct RenderPassContext;

/// 既存のラムダ式パスを新しい IRenderPass 契約へ載せるアダプタ。
///
/// WHY 生ラムダを残すか: 契約を 1 つに保ったまま切り替えるため。56 本のパス本体を
///     同時に書き換えると、途中でビルドが通らずゴールデンも撮れない。名前と申告は
///     登録時に確定しているので、それをそのまま Setup から流せば «新契約の 1 実装»
///     として成立する。本体は順次 res.Target(...) へ移していけばよい。
class LambdaPass final : public IRenderPass {
public:
    /// 申告した名前から実体を引く本体。移行の済んだパスはこちらを使う。
    using PassFn = std::function<void(PassResources&)>;

    LambdaPass(std::string_view name,
               std::vector<renderer::RenderGraph::ResourceAccess> accesses,
               PassFn fn,
               bool allowCulling)
        : m_name(name), m_accesses(std::move(accesses)),
          m_fn(std::move(fn)), m_allowCulling(allowCulling) {}

    /// 旧来の «引数なし» 本体。ctx.handles を直接触るパスがまだ多いので受け皿を残す。
    LambdaPass(std::string_view name,
               std::vector<renderer::RenderGraph::ResourceAccess> accesses,
               renderer::RenderGraph::ExecuteFn fn,
               bool allowCulling)
        : LambdaPass(name, std::move(accesses),
                     PassFn{ [fn = std::move(fn)](PassResources&) { fn(); } }, allowCulling) {}

    std::string_view Name() const override { return m_name; }

    void Setup(PassBuilder& builder, const RenderPassContext&) const override
    {
        for (const auto& access : m_accesses) {
            switch (access.usage) {
            case renderer::RenderGraph::ResourceUsage::Read:      builder.Read(access.name); break;
            case renderer::RenderGraph::ResourceUsage::Write:     builder.Write(access.name); break;
            case renderer::RenderGraph::ResourceUsage::ReadWrite: builder.ReadWrite(access.name); break;
            }
        }
    }

    bool IsEnabled(const RenderPassContext&) const override { return static_cast<bool>(m_fn); }
    bool AllowCulling() const override { return m_allowCulling; }

    void Execute(PassResources& resources, RenderPassContext&) override { m_fn(resources); }

private:
    std::string                                        m_name;
    std::vector<renderer::RenderGraph::ResourceAccess> m_accesses;
    PassFn                                             m_fn;
    bool                                               m_allowCulling = true;
};

class RenderPipeline {
public:
    // 前フレームの登録内容だけを破棄し、Plan とトランジェント RT のキャッシュは維持する。
    // WHY: Scene/Game View ごとに RenderPipeline を永続化しつつ、フレーム固有のラムダが
    //      前フレームの RenderPassContext を参照し続けないよう毎フレーム登録し直す。
    void BeginBuild();

    // 型付きパスを末尾に追加する。コンストラクタ引数を渡せる。
    template<typename T, typename... Args>
    void AddPass(Args&&... args)
    {
        m_entries.push_back(std::make_unique<T>(std::forward<Args>(args)...));
    }

    // ラムダ式パス (reads + writes を initializer_list<string_view> で指定)
    void AddRawPass(std::string_view name,
                    std::initializer_list<std::string_view> reads,
                    std::initializer_list<std::string_view> writes,
                    renderer::RenderGraph::ExecuteFn fn,
                    bool allowCulling = true);

    // ラムダ式パス (accesses を initializer_list<ResourceAccess> で指定 — ReadWrite 混在時)
    void AddRawPass(std::string_view name,
                    std::initializer_list<renderer::RenderGraph::ResourceAccess> accesses,
                    renderer::RenderGraph::ExecuteFn fn,
                    bool allowCulling = true);

    // ラムダ式パス (accesses を vector<ResourceAccess> で指定 — UserRenderPassDesc 経由)
    void AddRawPass(std::string_view name,
                    std::vector<renderer::RenderGraph::ResourceAccess> accesses,
                    renderer::RenderGraph::ExecuteFn fn,
                    bool allowCulling = true);

    // --- PassResources を受け取る版 (移行の済んだパス) ---
    // 引数なしのラムダはこちらへは解決しない (invocable でないため) ので曖昧にならない。
    void AddRawPass(std::string_view name,
                    std::initializer_list<std::string_view> reads,
                    std::initializer_list<std::string_view> writes,
                    LambdaPass::PassFn fn,
                    bool allowCulling = true);

    void AddRawPass(std::string_view name,
                    std::initializer_list<renderer::RenderGraph::ResourceAccess> accesses,
                    LambdaPass::PassFn fn,
                    bool allowCulling = true);

    void DeclareResource(std::string_view name, renderer::RenderGraph::ResourceDesc desc);
    void SetOutputs(std::initializer_list<std::string_view> outputs);
    void SetGpuProfilerHooks(std::function<void(std::string_view)> begin,
                              std::function<void(std::string_view)> end);

    // 登録されたすべてのパスを RenderGraph に組み込んで実行する。
    // IsEnabled が false のパスはスキップされる。
    bool Execute(RenderPassContext& ctx);

    const renderer::RenderGraph::ExecutionReport& LastReport() const { return m_lastReport; }

    // 直近に Plan をやり直したときの構成テキスト (RenderGraph::DescribeLastPlan)。
    // 毎フレームは作らない — トポロジが変わったフレームだけ更新する。
    // 差分を取れば «絵は同じだが実行順が変わった» を捕まえられる。
    const std::string& LastPlanDescription() const { return m_lastPlanDescription; }

    // 保持している GPU リソースを ResourceManager へ返す。
    // WHAT: Viewport のリサイズなど、実体を直ちに破棄すべき境界で呼び出す。
    void ReleaseViewResources(renderer::ResourceManager& resources);

private:
    // 契約は 1 つ。ラムダ式パスも LambdaPass として同じ列に並ぶ。
    using Entry = std::unique_ptr<IRenderPass>;

    // Setup が返した申告。グラフへ渡すラムダが参照で掴むので、Execute の間ずっと
    // 生きている場所へ置く。添字は m_entries と揃える。
    struct PassSetup {
        std::vector<renderer::RenderGraph::ResourceAccess> accesses;
        std::string                                        autoTarget;
    };
    std::vector<PassSetup> m_setups;

    // NOTE: ここには «aliasGroup ごとに物理リソースを確保して貸し回す» プールがあった。
    //
    // WHY 消したか: 貸出先が 1 つも無いまま、alias グループの数だけ実体を確保していた。
    //     実測 (Artifacts/RenderGraph の構成テキスト) では、トランジェント 7 個が
    //     7 グループに分かれて «1 枚も共有できていない» 状態で、それでも 7 枚ぶんの
    //     VRAM (1 ビュー約 34MB) を握っていた。節約ゼロで消費だけがある状態だったので、
    //     使う当てができるまで確保しない。
    //
    //     寿命とエイリアスグループの解析そのものは RenderGraph::AnalyzeLifetimes に
    //     残っている (構成テキストの alias= がそれ)。パスが増えて寿命が分かれたら、
    //     まず構成テキストで «何枚浮くか» を測ってから作り直すこと。

    std::vector<Entry> m_entries;
    std::vector<std::pair<std::string, renderer::RenderGraph::ResourceDesc>> m_resources;
    std::vector<std::string> m_outputs;
    renderer::RenderGraph::ExecutionReport m_lastReport;
    std::string m_lastPlanDescription;

    // 束縛毒用の 1x1 RT (RenderBindingGuard)。診断を立てたときだけ確保する。
    renderer::SizedRenderTarget m_bindingPoisonRT;
    std::function<void(std::string_view)> m_gpuBegin;
    std::function<void(std::string_view)> m_gpuEnd;

    // Plan キャッシュ: 有効パスのインデックス列と依存宣言の指紋が変わらなければ
    // 毎フレームの Plan() をスキップし、前フレームの実行順を注入する。
    std::vector<size_t> m_lastEnabledEntryIndices;
    // パス名 + accesses + allowCulling + outputs の FNV-1a ハッシュ。
    // WHY: index 列だけでは DeclareAccesses() の返却内容の変化 (設定トグル等) を検出できない。
    uint64_t m_lastGraphFingerprint = 0;
    bool m_planValid = false;
};

} // namespace fbzz::scene
