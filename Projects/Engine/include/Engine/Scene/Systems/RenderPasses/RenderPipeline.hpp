// FBZZ Engine
// RenderPipeline.hpp | fbzz::scene
// IRenderPass の登録と renderer::RenderGraph への変換を担う実行パイプライン
// AddPass<T>() で型付きパスを、AddRawPass() でラムダ式パスをフレームごとに登録し、
// Execute(ctx) が呼ばれた時点ですべてのパスを RenderGraph に組み込んで実行する。
// 追加順が RenderGraph 上の優先度になる (依存関係が同一の場合のタイブレーク)。
#pragma once
#include "IRenderPass.hpp"
#include <Engine/Renderer/RenderGraph.hpp>
#include <functional>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

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

private:
    struct Entry {
        std::unique_ptr<IRenderPass> pass;  // 型付きパス (null = raw pass)
        // raw pass 専用フィールド
        std::string name;
        std::vector<renderer::RenderGraph::ResourceAccess> accesses;
        renderer::RenderGraph::ExecuteFn fn;
        bool allowCulling = true;
    };

    std::vector<Entry> m_entries;
    std::vector<std::pair<std::string, renderer::RenderGraph::ResourceDesc>> m_resources;
    std::vector<std::string> m_outputs;
    renderer::RenderGraph::ExecutionReport m_lastReport;
    std::function<void(std::string_view)> m_gpuBegin;
    std::function<void(std::string_view)> m_gpuEnd;
};

} // namespace fbzz::scene
