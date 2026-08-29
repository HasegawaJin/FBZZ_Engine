/// @file    IRenderPass.hpp
/// @brief   レンダーパスの基底インターフェース。
/// @author  Hasegawa Jin
/// @date    2026-06-18
///
/// RenderPipeline::AddPass<T>() で登録し、Execute 時に RenderGraph へ自動組み込みされる。
/// 各パスは Name / DeclareAccesses / IsEnabled / Execute を実装する。
#pragma once
#include <Engine/Renderer/RenderGraph.hpp>
#include <string_view>
#include <vector>

namespace fbzz::scene {

struct RenderPassContext;

class IRenderPass {
public:
    virtual ~IRenderPass() = default;

    virtual std::string_view Name() const = 0;

    // このパスが読み書きするリソースを宣言する。RenderGraph の依存解析に使われる。
    virtual std::vector<renderer::RenderGraph::ResourceAccess> DeclareAccesses(
        const RenderPassContext& ctx) const = 0;

    // false を返すとこのフレームは RenderGraph に追加しない (IsEnabled で条件分岐を一元管理する)。
    virtual bool IsEnabled(const RenderPassContext& ctx) const { return true; }

    // true のとき RenderGraph のデッドパスカリング対象になる。
    virtual bool AllowCulling() const { return true; }

    virtual void Execute(RenderPassContext& ctx) = 0;
};

} // namespace fbzz::scene
