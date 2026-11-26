/// @file    RenderPipeline.hpp
/// @brief   IRenderPass の登録と renderer::RenderGraph への変換を担う実行パイプライン。
/// @author  Hasegawa Jin
/// @date    2026-06-18

/// @note AddPass<T>() で型付きパスを、AddRawPass() でラムダ式パスをフレームごとに登録し、
/// @note Execute(ctx) が呼ばれた時点ですべてのパスを RenderGraph に組み込んで実行する。
/// @note 追加順が RenderGraph 上の優先度になる (依存関係が同一の場合のタイブレーク)。

/// @note GPU 実体の所有権は呼び出し側に残る。Declare 系 API が記述と型付きハンドルを同時に受け取る。
/// @note Execute() の冒頭で登録簿へ流し、パスは PassResources から名前で引く。
#pragma once
#include <Graphics/Api.hpp>
#include "Graphics/Pipeline/Passes/IRenderPass.hpp"
#include <Graphics/Pipeline/View/ResolvedRenderPlan.hpp>
#include <Graphics/Renderer/Diagnostics/GpuProfiler.hpp>
#include <Graphics/RenderGraph/RenderGraph.hpp>
#include <Graphics/Renderer/View/RenderSettings.hpp>
#include <Graphics/Renderer/Resources/ResourceHandle.hpp>
#include <Graphics/Renderer/Resources/ResourceManager.hpp>
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

namespace fbzz::renderer {

struct RenderPassContext;
class RenderPassCapture;

/// @note ラムダ式で書かれたパスを IRenderPass 契約へ載せるアダプタ。
/// @note 恒久的な «第 2 の登録形式»。スクリプト由来のパス (std::function) や名前が実行時
/// @note に決まるパス (CustomHDR の連番など) はクラスに書けないため無くならない。
/// @note それでもエンジン自身のパスを型付きにするのは、ラムダ登録だと申告が登録側
/// @note (RenderSystem) に残り本体から離れ、実際に Terrain / DeferredLighting で
/// @note «読んでいるのに申告していない» が起きたため。申告は本体の隣へ置く。
class LambdaPass final : public IRenderPass {
public:
    /// @note 申告した名前から実体を引く本体。移行の済んだパスはこちらを使う。
    using PassFn = std::function<void(PassResources&)>;

    LambdaPass(std::string_view name,
               std::vector<renderer::RenderGraph::ResourceAccess> accesses,
               PassFn fn,
               bool allowCulling)
        : m_name(name), m_accesses(std::move(accesses)),
          m_fn(std::move(fn)), m_allowCulling(allowCulling) {}

    /// @note 旧来の «引数なし» 本体。ctx.handles を直接触るパスがまだ多いので受け皿を残す。
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
            builder.Declare(access);
        }
    }

    bool IsEnabled(const RenderPassContext&) const override { return static_cast<bool>(m_fn); }
    bool AllowCulling() const override { return m_allowCulling; }

    void Execute(PassResources& resources, RenderPassContext&) override { m_fn(resources); }

private:
    std::string                                        m_name; ///< グラフ登録とパス上書きに使う名前の所有文字列。
    std::vector<renderer::RenderGraph::ResourceAccess> m_accesses; ///< 外部から登録されたパスが読み書きする論理資源の一覧。
    PassFn                                             m_fn; ///< パス実行時に呼び出す処理本体。
    bool                                               m_allowCulling = true; ///< 出力から到達しない場合にパスを省略できるかどうか。
};

class RenderPipeline {
public:
    /// @note Plan キャッシュの鍵を組み立てる FNV-1a 累積器。

    /// @note 混ぜ忘れた項目は «変わっても同じ鍵» になり、古い実行順を使い回す。
    /// @note 実際に colorCount / withDepth が抜けていた。項目を足したら
    /// @note RenderPipelineTests に «その項目だけ変えた 2 つが異なる» を足すこと。
    /// @note 確保を伴わない。毎フレーム全パスぶん回るため。
    class GraphFingerprint {
    public:
        /// @param[in] desc 寸法・形式だけでなく実体の «形» を決める項目もすべて混ぜる。
        FBZZ_GRAPHICS_API void MixResource(std::string_view name, const renderer::RenderGraph::ResourceDesc& desc);
        FBZZ_GRAPHICS_API void MixOutput(std::string_view name);
        FBZZ_GRAPHICS_API void MixPass(std::string_view name, bool allowCulling,
                     const std::vector<renderer::RenderGraph::ResourceAccess>& accesses);

        [[nodiscard]] uint64_t Value() const { return m_value; }

    private:
        FBZZ_GRAPHICS_API void MixBytes(const void* data, size_t size);
        /// @note 末尾に区切りを足す。"ab"+"c" と "a"+"bc" を別の鍵にするため。
        FBZZ_GRAPHICS_API void MixString(std::string_view text);

        uint64_t m_value = 1469598103934665603ull; ///< 資源・出力・パス宣言を混ぜた FNV-1a の累積値。
    };

    /// @note 上書きを «このフレームの確定した申告» へ畳み込む。

    /// @param[in,out] allowCulling 入出力。false 方向にしか変わらない。
    /// @note extraReads のうち既に申告済みの名前は足さない (重複した辺は無意味)。
    /// @note ctx を要らないので単体で試せる。判定の本体はここに閉じる。
    FBZZ_GRAPHICS_API static void ApplyOverride(const renderer::RenderPassOverride& over,
                              std::vector<renderer::RenderGraph::ResourceAccess>& accesses,
                              bool& allowCulling);

    /// @note 前フレームの登録内容だけを破棄し、Plan とトランジェント RT のキャッシュは維持する。
    /// @note Scene/Game View ごとに RenderPipeline を永続化しつつ、フレーム固有のラムダが
    /// @note 前フレームの RenderPassContext を参照し続けないよう毎フレーム登録し直す。
    FBZZ_GRAPHICS_API void BeginBuild();

    /// @brief 無効判定・カリング前の登録列を診断用に取得する。
    /// @note 実行順は LastReport で確認する。返した文字列は BeginBuild 後も有効。
    [[nodiscard]] std::vector<std::string> RegisteredPassNames() const
    {
        std::vector<std::string> names;
        names.reserve(m_entries.size());
        for (const auto& entry : m_entries) {
            names.emplace_back(entry->Name());
        }
        return names;
    }

    /// @note 型付きパスを末尾に追加する。コンストラクタ引数を渡せる。
    template<typename T, typename... Args>
    void AddPass(Args&&... args)
    {
        m_entries.push_back(std::make_unique<T>(std::forward<Args>(args)...));
    }

    /// @note ラムダ式パス。申告の書き方 (reads/writes ・ initializer_list ・ vector) だけが違い、
    /// @note 本体は «引数なし» と «PassResources を受け取る» の両方を受け付ける。
    /// @note オーバーロードを本体の型で分けないのは、std::function の変換制約で引数なしの
    /// @note ラムダは PassFn、PassResources を取るラムダは ExecuteFn へしか «変換できない»
    /// @note ため。契約は LambdaPass のコンストラクタ選択で決まる。

    /// @note reads + writes を `initializer_list<string_view>` で指定
    template<typename Fn>
    void AddRawPass(std::string_view name,
                    std::initializer_list<std::string_view> reads,
                    std::initializer_list<std::string_view> writes,
                    Fn&& fn,
                    bool allowCulling = true)
    {
        Emplace(name, MakeAccesses(reads, writes), std::forward<Fn>(fn), allowCulling);
    }

    /// @note accesses を `initializer_list<ResourceAccess>` で指定 — ReadWrite 混在時
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

    /// @note accesses を `vector<ResourceAccess>` で指定 — UserRenderPassDesc 経由
    template<typename Fn>
    void AddRawPass(std::string_view name,
                    std::vector<renderer::RenderGraph::ResourceAccess> accesses,
                    Fn&& fn,
                    bool allowCulling = true)
    {
        Emplace(name, std::move(accesses), std::forward<Fn>(fn), allowCulling);
    }

    /// @note 論理リソースを «申告» と «実体» を揃えて登録する。
    /// @note 以前は DeclareResource (依存解析用の申告) と RenderResourceRegistry::BindTarget
    /// @note (実体の登録) が別々に維持され、片方だけ書いても Plan は通るため «申告したのに
    /// @note 実体が無い» が静かに成立した。同じ引数列に並べ片方だけ書けなくする。
    /// @note 束縛は Execute() の冒頭でまとめて登録簿へ流す。実体を差し替えるパス
    /// @note (TAA の ping-pong) はその後で上書きすればよい。
    FBZZ_GRAPHICS_API void DeclareTarget(std::string_view name,
                       renderer::ResourceHandle<renderer::RenderTargetTag> handle,
                       renderer::RenderGraph::ResourceDesc desc);
    FBZZ_GRAPHICS_API void DeclareTexture(std::string_view name,
                        renderer::ResourceHandle<renderer::TextureTag> handle,
                        renderer::RenderGraph::ResourceDesc desc);

    /// @note 実体の所有権は呼び出し側に残る。stride は頂点幅 [byte]、raw / index Buffer では 0。
    FBZZ_GRAPHICS_API void DeclareBuffer(std::string_view name,
                       renderer::ResourceHandle<renderer::BufferTag> handle,
                       renderer::RenderGraph::ResourceDesc desc);
    /// @note StructuredBuffer も ResourceKind::Buffer とし、byteSize / stride で記述する。
    FBZZ_GRAPHICS_API void DeclareBuffer(std::string_view name,
                       renderer::ResourceHandle<renderer::StructuredBufferTag> handle,
                       renderer::RenderGraph::ResourceDesc desc);
    FBZZ_GRAPHICS_API void DeclareStructuredBuffer(std::string_view name,
                                 renderer::ResourceHandle<renderer::StructuredBufferTag> handle,
                                 renderer::RenderGraph::ResourceDesc desc);
    /// @note AS は transient 宣言でも alias しない。状態と build 同期は backend の契約。
    FBZZ_GRAPHICS_API void DeclareAccelerationStructure(std::string_view name,
                                      renderer::ResourceHandle<renderer::AccelerationStructureTag> handle,
                                      renderer::RenderGraph::ResourceDesc desc);

    FBZZ_GRAPHICS_API void SetOutputs(std::initializer_list<std::string_view> outputs);

    /// @note エディターからのパス単位の上書き。BeginBuild では消えない (登録ではなく設定)。

    /// @note allowCulling は «false でだけ効く»。パス自身が false を返しているものを
    /// @note true にはできない。SkinningCompute のように «刈られては困る» と自分で言っている
    /// @note パスを、UI の既定値が黙って刈れるようにしてしまうため。
    FBZZ_GRAPHICS_API void SetPassOverrides(std::vector<renderer::RenderPassOverride> overrides);

    /// @note 実行順の決め方。次の Plan から効く。
    /// @note 指紋に含める。方針が変われば実行順が変わるので、Plan を作り直す必要がある。
    void SetSchedulePolicy(renderer::RenderGraphSchedulePolicy policy) { m_policy = policy; }
    FBZZ_GRAPHICS_API void SetGpuProfilerHooks(std::function<void(std::string_view)> begin,
                              std::function<void(std::string_view)> end);

    /// @note 登録されたすべてのパスを RenderGraph に組み込んで実行する。
    /// @note IsEnabled が false のパスはスキップされる。
    /// @note GPU の出自は最終解決済み Plan と実際の Graph 計画世代を記録する。準備処理は計測範囲外。
    FBZZ_GRAPHICS_API bool Execute(RenderPassContext& ctx, RenderPassCapture* capture = nullptr,
                 const GpuProfilerViewMetadata* gpuView = nullptr,
                 const ResolvedRenderPlan* gpuPlan = nullptr);

    const GpuProfilerViewMetadata& LastGpuProfilerView() const { return m_lastGpuProfilerView; }

    const renderer::RenderGraph::ExecutionReport& LastReport() const { return m_lastReport; }

    /// @note 直近に Plan をやり直したときの構成テキスト (RenderGraph::DescribeLastPlan)。
    /// @note 毎フレームは作らない — トポロジが変わったフレームだけ更新する。
    /// @note 差分を取れば «絵は同じだが実行順が変わった» を捕まえられる。
    const std::string& LastPlanDescription() const { return m_lastPlanDescription; }

    /// @note 保持している GPU リソースを ResourceManager へ返す。
    /// @note Viewport のリサイズなど、実体を直ちに破棄すべき境界で呼び出す。
    FBZZ_GRAPHICS_API void ReleaseViewResources(renderer::ResourceManager& resources);

private:
    /// @note 契約は 1 つ。ラムダ式パスも LambdaPass として同じ列に並ぶ。
    using Entry = std::unique_ptr<IRenderPass>;

    FBZZ_GRAPHICS_API static std::vector<renderer::RenderGraph::ResourceAccess>
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

    /// @note 宣言された実体を登録簿へ流す。パス本体が名前から引けるのはここを通ったものだけ。
    FBZZ_GRAPHICS_API void BindDeclaredResources(RenderPassContext& ctx) const;

    /// @note このフレームに載せるパスの添字列を確定し、同時に Setup を引き直す。
    FBZZ_GRAPHICS_API std::vector<size_t> CollectEnabledSetups(RenderPassContext& ctx);

    /// @note 申告 (リソース記述 / 出力 / パス名 / accesses / allowCulling) の FNV-1a 指紋。
    /// @note 前フレームと一致すれば Plan の結果をそのまま使い回せる。
    FBZZ_GRAPHICS_API uint64_t ComputeGraphFingerprint(const std::vector<size_t>& enabled) const;

    FBZZ_GRAPHICS_API void BuildGraph(renderer::RenderGraph& graph,
                    RenderPassContext& ctx,
                    const std::vector<size_t>& enabled);

    FBZZ_GRAPHICS_API const renderer::RenderPassOverride* FindOverride(std::string_view name) const;

    /// @note Setup が返した申告に上書きを適用した «このフレームの確定値»。グラフへ渡すラムダが
    /// @note 参照で掴むので、Execute の間ずっと生きている場所へ置く。添字は m_entries と揃える。
    struct PassSetup {
        std::vector<renderer::RenderGraph::ResourceAccess> accesses; ///< Setup の宣言とビュー上書きを反映した読み書き一覧。
        std::string                                        autoTarget; ///< 実行前に自動束縛する論理ターゲット名。空なら自動束縛しない。
        bool                                               allowCulling = true; ///< このフレームの上書きまで反映したカリング可否。
    };
    std::vector<PassSetup> m_setups; ///< m_entries と同じ添字で保持する、そのフレームの確定済み申告。

    /// @note ここには «aliasGroup ごとに物理リソースを確保して貸し回す» プールがあった。
    /// @note 貸出先が 1 つも無いまま alias グループの数だけ実体を確保しており、実測では
    /// @note トランジェント 7 個が 7 グループに分かれて «1 枚も共有できていない» まま
    /// @note 7 枚ぶんの VRAM (1 ビュー約 34MB) を握っていたため削除した。使う当てが
    /// @note できるまで確保しない。寿命とエイリアスグループの解析自体は
    /// @note RenderGraph::AnalyzeLifetimes に残る (構成テキストの alias= がそれ)。パスが
    /// @note 増えて寿命が分かれたら、まず構成テキストで «何枚浮くか» を測ってから作り直す。

    /// @note 1 つの論理リソースについて «申告» と «実体» を同じ行に持つ。
    /// @note 各ハンドルのうち宣言した型だけが有効になる。StructuredBuffer は Buffer 種別。
    struct ResourceEntry {
        std::string                                         name; ///< パスが資源の読み書きを宣言する際の論理名。
        renderer::RenderGraph::ResourceDesc                 desc; ///< グラフへ渡す資源の形式、寸法、所有・寿命の属性。
        renderer::ResourceHandle<renderer::RenderTargetTag> target; ///< RenderTarget 宣言が参照する非所有ハンドル。
        renderer::ResourceHandle<renderer::TextureTag>      texture; ///< Texture 宣言が参照する非所有ハンドル。
        renderer::ResourceHandle<renderer::BufferTag> buffer; ///< Buffer 宣言が参照する非所有ハンドル。
        renderer::ResourceHandle<renderer::StructuredBufferTag> structuredBuffer; ///< 構造化 Buffer 宣言が参照する非所有ハンドル。
        renderer::ResourceHandle<renderer::AccelerationStructureTag> accelerationStructure; ///< レイ用加速構造の宣言が参照する非所有ハンドル。
    };

    std::vector<Entry> m_entries; ///< 登録順に保持したフレームの描画パス本体。
    std::vector<ResourceEntry> m_resources; ///< 論理資源ごとの宣言と実体の参照を保持する登録列。
    std::vector<std::string> m_outputs; ///< 依存解決時にグラフの最終出力として残す論理資源名。
    std::vector<renderer::RenderPassOverride> m_overrides; ///< フレームをまたいで適用するパス単位の上書き設定。
    renderer::RenderGraphSchedulePolicy m_policy = renderer::RenderGraphSchedulePolicy::RegistrationOrder; ///< 複数の実行順が有効な場合に選択するグラフの方針。
    renderer::RenderGraph::ExecutionReport m_lastReport; ///< 直近のグラフ実行順と計測の結果。条件が同じフレームで実行順を再利用する。
    std::string m_lastPlanDescription; ///< パス構成が変わった時点で生成するグラフの診断テキスト。

    /// @note 束縛毒用の 1x1 RT (RenderBindingGuard)。診断を立てたときだけ確保する。
    renderer::SizedRenderTarget m_bindingPoisonRT; ///< 診断有効時に各パス直前へ束縛する 1×1 ターゲット。
    std::function<void(std::string_view)> m_gpuBegin; ///< グラフのパス開始時に GPU 計測を始めるフック。
    std::function<void(std::string_view)> m_gpuEnd; ///< グラフのパス終了時に GPU 計測を閉じるフック。

    /// @note Plan キャッシュ: 有効パスのインデックス列と依存宣言の指紋が変わらなければ
    /// @note 毎フレームの Plan() をスキップし、前フレームの実行順を注入する。
    std::vector<size_t> m_lastEnabledEntryIndices; ///< 前回の実行計画を作った時点の m_entries 内の有効パス添字。
    /// @note リソース記述 + outputs + パス名 + accesses + allowCulling の FNV-1a ハッシュ。
    /// @note index 列だけでは Setup() の申告内容の変化 (設定トグル等) を検出できない。
    uint64_t m_lastGraphFingerprint = 0; ///< 前回の実行計画を作った資源とパス宣言のハッシュ。
    bool m_planValid = false; ///< 前回の実行計画を後続フレームで再利用できるかどうか。
    uint64_t m_gpuProfilePlanGeneration = 0; ///< GPU 計測で別の描画構成を区別する計画世代。
    ResolvedRenderPlan m_gpuProfileRenderPlan; ///< GPU 計測の世代番号を決めた解決済みのビュー構成。
    bool m_gpuProfileRenderPlanValid = false; ///< GPU 計測用の比較元となるビュー構成を保持しているかどうか。
    GpuProfilerViewMetadata m_lastGpuProfilerView; ///< 直近の実行で GPU プロファイラーに記録したビュー情報。
};

} // namespace fbzz::renderer
