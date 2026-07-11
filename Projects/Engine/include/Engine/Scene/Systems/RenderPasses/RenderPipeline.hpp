// FBZZ Engine
// RenderPipeline.hpp | fbzz::scene
// IRenderPass の登録と renderer::RenderGraph への変換を担う実行パイプライン
// AddPass<T>() で型付きパスを、AddRawPass() でラムダ式パスをフレームごとに登録し、
// Execute(ctx) が呼ばれた時点ですべてのパスを RenderGraph に組み込んで実行する。
// 追加順が RenderGraph 上の優先度になる (依存関係が同一の場合のタイブレーク)。
//
// [TransientRTPool]
// DeclareResource() で transient=true のリソースを宣言すると、Execute() 内で
// Plan() → RebuildTransientPool() が走り、同一 aliasGroup のリソースが同一の
// 物理 RT ハンドルを共有する。パスのコールバックからは ctx.getTransientRT(name) で
// 確保済みハンドルを取得できる。
#pragma once
#include "IRenderPass.hpp"
#include <Engine/Renderer/RenderGraph.hpp>
#include <Engine/Renderer/ResourceHandle.hpp>
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

class RenderPipeline {
public:
    // 型付きパスを末尾に追加する。コンストラクタ引数を渡せる。
    template<typename T, typename... Args>
    void AddPass(Args&&... args)
    {
        Entry e;
        e.pass = std::make_unique<T>(std::forward<Args>(args)...);
        m_entries.push_back(std::move(e));
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

    void DeclareResource(std::string_view name, renderer::RenderGraph::ResourceDesc desc);
    void SetOutputs(std::initializer_list<std::string_view> outputs);
    void SetGpuProfilerHooks(std::function<void(std::string_view)> begin,
                              std::function<void(std::string_view)> end);

    // 登録されたすべてのパスを RenderGraph に組み込んで実行する。
    // IsEnabled が false のパスはスキップされる。
    bool Execute(RenderPassContext& ctx);

    const renderer::RenderGraph::ExecutionReport& LastReport() const { return m_lastReport; }

    // トランジェントリソース名からプールされた RT ハンドルを返す。
    // WHY: パスのコールバック内で ctx.getTransientRT(name) を通じて呼ばれる。
    //      DeclareResource で transient=true と宣言されたリソースのみ有効。
    renderer::ResourceHandle<renderer::RenderTargetTag> GetTransientRT(std::string_view name) const;

private:
    struct Entry {
        std::unique_ptr<IRenderPass> pass;  // 型付きパス (null = raw pass)
        // raw pass 専用フィールド
        std::string name;
        std::vector<renderer::RenderGraph::ResourceAccess> accesses;
        renderer::RenderGraph::ExecuteFn fn;
        bool allowCulling = true;
    };

    // TransientRTPool: aliasGroup ごとに物理 RT を 1 つ確保し、同グループのリソースで共有する。
    // WHY: RenderGraph の AnalyzeLifetimes() が算出したエイリアスグループを実際の RT 割り当てに
    //      反映することで、ライフタイムが重ならない中間 RT のメモリを節約できる。
    //      例: bloomHalf と ssaoRaw がエイリアスグループ 0 なら 1 つの RT バッファを使い回す。
    struct PooledRT {
        renderer::ResourceHandle<renderer::RenderTargetTag> handle;
        renderer::RenderGraph::ResourceDesc                 desc;
    };
    // aliasGroup (-1 = 非エイリアス) → 物理 RT
    std::unordered_map<int, PooledRT>        m_aliasGroupPool;
    // リソース名 → aliasGroup (GetTransientRT の高速引き当て用)
    std::unordered_map<std::string, int>     m_nameToAliasGroup;
    bool                                     m_poolDirty = true;

    // パイプライン構成変更時にプールを破棄して次フレームの再構築を促す。
    void MarkPoolDirty() { m_poolDirty = true; }

    // 直前の Plan() 結果を使ってトランジェント RT プールを再構築する。
    // WHY: 毎フレーム呼ぶとアロケーションが走るため、m_poolDirty フラグで
    //      パイプライン構成変更時のみ再構築するように制御する。
    void RebuildTransientPool(const renderer::RenderGraph::ExecutionReport& report,
                              renderer::ResourceManager& resources);

    std::vector<Entry> m_entries;
    std::vector<std::pair<std::string, renderer::RenderGraph::ResourceDesc>> m_resources;
    std::vector<std::string> m_outputs;
    renderer::RenderGraph::ExecutionReport m_lastReport;
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
