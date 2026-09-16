/// @file    RenderPipeline.hpp
/// @brief   IRenderPass の登録と renderer::RenderGraph への変換を担う実行パイプライン。
/// @author  Hasegawa Jin
/// @date    2026-06-18
///
/// AddPass<T>() で型付きパスを、AddRawPass() でラムダ式パスをフレームごとに登録し、
/// Execute(ctx) が呼ばれた時点ですべてのパスを RenderGraph に組み込んで実行する。
/// 追加順が RenderGraph 上の優先度になる (依存関係が同一の場合のタイブレーク)。
///
/// リソースの実体はまだ ViewRenderTargets が確保する。ただし «その名前がどれを指すか»
/// は DeclareTarget / DeclareTexture で申告と一緒に受け取り、Execute() の冒頭で
/// RenderPassContext の登録簿へ流す。パスは名前で引く (PassResources::Target / Texture)。
/// 申告と実体を別々に書く経路は残っていない。
#pragma once
#include "IRenderPass.hpp"
#include <Engine/Renderer/RenderGraph.hpp>
#include <Engine/Renderer/RenderSettings.hpp>
#include <Engine/Renderer/ResourceHandle.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fbzz::renderer { class ResourceManager; }

namespace fbzz::scene {

struct RenderPassContext;
class RenderPassCapture;

/// ラムダ式で書かれたパスを IRenderPass 契約へ載せるアダプタ。
///
/// @note 恒久的な «第 2 の登録形式» であって、移行中の足場ではない。
///       スクリプトから来るパス (UserRenderPassDesc::execute は
///       std::function) と、名前が実行時に決まるパス (CustomHDR の連番など) は
///       クラスに書けないので、この経路が無くなることはない。
///
/// WHY それでもエンジン自身のパスは型付きにするか: ラムダで登録すると申告が
///     登録側 (RenderSystem) に残り、本体のあるファイルから離れる。本体が新しい
///     テクスチャを読み始めても申告を直す場所が視界に入らず、実際 Terrain と
///     DeferredLighting で «読んでいるのに申告していない» が起きた。申告を
///     本体の隣へ置けるものは置く。
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
        for (const auto& access : m_accesses)
            builder.Declare(access);
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
    /// Plan キャッシュの鍵を組み立てる FNV-1a 累積器。
    ///
    /// @note 混ぜ忘れた項目は «変わっても同じ鍵» になり、古い実行順を使い回す。
    ///       実際に colorCount / withDepth が抜けていた。項目を足したら
    ///       RenderPipelineTests に «その項目だけ変えた 2 つが異なる» を足すこと。
    /// @note 確保を伴わない。毎フレーム全パスぶん回るため。
    class GraphFingerprint {
    public:
        /// @param desc 寸法・形式だけでなく実体の «形» を決める項目もすべて混ぜる。
        void MixResource(std::string_view name, const renderer::RenderGraph::ResourceDesc& desc);
        void MixOutput(std::string_view name);
        void MixPass(std::string_view name, bool allowCulling,
                     const std::vector<renderer::RenderGraph::ResourceAccess>& accesses);

        [[nodiscard]] uint64_t Value() const { return m_value; }

    private:
        void MixBytes(const void* data, size_t size);
        /// @note 末尾に区切りを足す。"ab"+"c" と "a"+"bc" を別の鍵にするため。
        void MixString(std::string_view text);

        uint64_t m_value = 1469598103934665603ull; ///< FNV-1a offset basis
    };

    /// 上書きを «このフレームの確定した申告» へ畳み込む。
    ///
    /// @param allowCulling 入出力。false 方向にしか変わらない。
    /// @note extraReads のうち既に申告済みの名前は足さない (重複した辺は無意味)。
    /// @note ctx を要らないので単体で試せる。判定の本体はここに閉じる。
    static void ApplyOverride(const renderer::RenderPassOverride& over,
                              std::vector<renderer::RenderGraph::ResourceAccess>& accesses,
                              bool& allowCulling);

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

    // ラムダ式パス。申告の書き方 (reads/writes ・ initializer_list ・ vector) だけが違い、
    // 本体は «引数なし» と «PassResources を受け取る» の両方を受け付ける。
    //
    // WHY 本体の型でオーバーロードを分けないか: std::function の変換制約により、
    //     引数なしのラムダは PassFn へ、PassResources を取るラムダは ExecuteFn へ
    //     «変換できない»。どちらの契約かは LambdaPass のコンストラクタ選択で決まるので、
    //     ここで場合分けすると同じ本体を 2 度書くだけになる。

    // reads + writes を initializer_list<string_view> で指定
    template<typename Fn>
    void AddRawPass(std::string_view name,
                    std::initializer_list<std::string_view> reads,
                    std::initializer_list<std::string_view> writes,
                    Fn&& fn,
                    bool allowCulling = true)
    {
        Emplace(name, MakeAccesses(reads, writes), std::forward<Fn>(fn), allowCulling);
    }

    // accesses を initializer_list<ResourceAccess> で指定 — ReadWrite 混在時
    template<typename Fn>
    void AddRawPass(std::string_view name,
                    std::initializer_list<renderer::RenderGraph::ResourceAccess> accesses,
                    Fn&& fn,
                    bool allowCulling = true)
    {
        Emplace(name,
                std::vector<renderer::RenderGraph::ResourceAccess>(accesses.begin(), accesses.end()),
                std::forward<Fn>(fn), allowCulling);
    }

    // accesses を vector<ResourceAccess> で指定 — UserRenderPassDesc 経由
    template<typename Fn>
    void AddRawPass(std::string_view name,
                    std::vector<renderer::RenderGraph::ResourceAccess> accesses,
                    Fn&& fn,
                    bool allowCulling = true)
    {
        Emplace(name, std::move(accesses), std::forward<Fn>(fn), allowCulling);
    }

    // 論理リソースを «申告» と «実体» を揃えて登録する。
    //
    // WHY 2 つを 1 つの呼び出しにするか: 以前は DeclareResource (依存解析用の申告) と
    //     RenderResourceRegistry::BindTarget (実体の登録) が別々の場所で手で維持されて
    //     いた。片方だけ書いても Plan は通るので、«申告したのに実体が無い» も
    //     «実体はあるが誰も申告していない» も静かに成立した。同じ引数列に並べれば
    //     片方だけ書くこと自体ができなくなる。
    //
    // 束縛は Execute() の冒頭でまとめて登録簿へ流す。実体を差し替えるパス
    // (TAA の ping-pong) はその後で上書きすればよい。
    void DeclareTarget(std::string_view name,
                       renderer::ResourceHandle<renderer::RenderTargetTag> handle,
                       renderer::RenderGraph::ResourceDesc desc);
    void DeclareTexture(std::string_view name,
                        renderer::ResourceHandle<renderer::TextureTag> handle,
                        renderer::RenderGraph::ResourceDesc desc);

    void SetOutputs(std::initializer_list<std::string_view> outputs);

    // エディターからのパス単位の上書き。BeginBuild では消えない (登録ではなく設定)。
    //
    // allowCulling は «false でだけ効く»。パス自身が false を返しているものを
    // true にはできない。SkinningCompute のように «刈られては困る» と自分で言っている
    // パスを、UI の既定値が黙って刈れるようにしてしまうため。
    void SetPassOverrides(std::vector<renderer::RenderPassOverride> overrides);

    /// 実行順の決め方。次の Plan から効く。
    /// @note 指紋に含める。方針が変われば実行順が変わるので、Plan を作り直す必要がある。
    void SetSchedulePolicy(renderer::RenderGraphSchedulePolicy policy) { m_policy = policy; }
    void SetGpuProfilerHooks(std::function<void(std::string_view)> begin,
                              std::function<void(std::string_view)> end);

    // 登録されたすべてのパスを RenderGraph に組み込んで実行する。
    // IsEnabled が false のパスはスキップされる。
    bool Execute(RenderPassContext& ctx, RenderPassCapture* capture = nullptr);

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

    static std::vector<renderer::RenderGraph::ResourceAccess>
    MakeAccesses(std::initializer_list<std::string_view> reads,
                 std::initializer_list<std::string_view> writes);

    template<typename Fn>
    void Emplace(std::string_view name,
                 std::vector<renderer::RenderGraph::ResourceAccess> accesses,
                 Fn&& fn,
                 bool allowCulling)
    {
        m_entries.push_back(std::make_unique<LambdaPass>(
            name, std::move(accesses), std::forward<Fn>(fn), allowCulling));
    }

    // 宣言された実体を登録簿へ流す。パス本体が名前から引けるのはここを通ったものだけ。
    void BindDeclaredResources(RenderPassContext& ctx) const;

    // このフレームに載せるパスの添字列を確定し、同時に Setup を引き直す。
    std::vector<size_t> CollectEnabledSetups(RenderPassContext& ctx);

    // 申告 (リソース記述 / 出力 / パス名 / accesses / allowCulling) の FNV-1a 指紋。
    // 前フレームと一致すれば Plan の結果をそのまま使い回せる。
    uint64_t ComputeGraphFingerprint(const std::vector<size_t>& enabled) const;

    void BuildGraph(renderer::RenderGraph& graph,
                    RenderPassContext& ctx,
                    const std::vector<size_t>& enabled);

    const renderer::RenderPassOverride* FindOverride(std::string_view name) const;

    // Setup が返した申告に上書きを適用した «このフレームの確定値»。グラフへ渡すラムダが
    // 参照で掴むので、Execute の間ずっと生きている場所へ置く。添字は m_entries と揃える。
    struct PassSetup {
        std::vector<renderer::RenderGraph::ResourceAccess> accesses;
        std::string                                        autoTarget;
        bool                                               allowCulling = true;
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

    // 1 つの論理リソースについて «申告» と «実体» を同じ行に持つ。
    // target / texture はどちらか片方だけが有効になる (ResourceDesc::kind に対応)。
    struct ResourceEntry {
        std::string                                         name;
        renderer::RenderGraph::ResourceDesc                 desc;
        renderer::ResourceHandle<renderer::RenderTargetTag> target;
        renderer::ResourceHandle<renderer::TextureTag>      texture;
    };

    std::vector<Entry> m_entries;
    std::vector<ResourceEntry> m_resources;
    std::vector<std::string> m_outputs;
    std::vector<renderer::RenderPassOverride> m_overrides;
    renderer::RenderGraphSchedulePolicy m_policy = renderer::RenderGraphSchedulePolicy::RegistrationOrder;
    renderer::RenderGraph::ExecutionReport m_lastReport;
    std::string m_lastPlanDescription;

    // 束縛毒用の 1x1 RT (RenderBindingGuard)。診断を立てたときだけ確保する。
    renderer::SizedRenderTarget m_bindingPoisonRT;
    std::function<void(std::string_view)> m_gpuBegin;
    std::function<void(std::string_view)> m_gpuEnd;

    // Plan キャッシュ: 有効パスのインデックス列と依存宣言の指紋が変わらなければ
    // 毎フレームの Plan() をスキップし、前フレームの実行順を注入する。
    std::vector<size_t> m_lastEnabledEntryIndices;
    // リソース記述 + outputs + パス名 + accesses + allowCulling の FNV-1a ハッシュ。
    // WHY: index 列だけでは Setup() の申告内容の変化 (設定トグル等) を検出できない。
    uint64_t m_lastGraphFingerprint = 0;
    bool m_planValid = false;
};

} // namespace fbzz::scene
