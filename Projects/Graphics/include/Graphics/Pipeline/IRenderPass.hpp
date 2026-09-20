/// @file    IRenderPass.hpp
/// @brief   レンダーパスの基底インターフェース。
/// @author  Hasegawa Jin
/// @date    2026-06-18

/// @note RenderPipeline::AddPass<T>() で登録し、Execute 時に RenderGraph へ自動組み込みされる。

/// @note 契約は 4 つ。
/// @note Name      … グラフ上の名前。プロファイラーと構成テキストにも出る
/// @note Setup     … 読む / 書くものを申告する «唯一の» 場所
/// @note IsEnabled … このフレームに載せるか。Execute 冒頭の早期 return は書かない
/// @note Execute   … 申告した名前からだけ実体を引いて描く
#pragma once
#include "PassResources.hpp"

#include <string_view>

namespace fbzz::renderer {

struct RenderPassContext;

class IRenderPass {
public:
    virtual ~IRenderPass() = default;

    virtual std::string_view Name() const = 0;

    /// @note 読み書きするリソースを申告する。RenderGraph の依存解析はここだけを見る。
    virtual void Setup(PassBuilder& builder, const RenderPassContext& ctx) const = 0;

    /// @note false を返すとこのフレームはグラフに載らない。
    /// @note Execute 冒頭の早期 return と分けない理由: 早期 return だとパスはグラフに載ったまま
    /// @note 依存の鎖を伸ばし続け、デッドパスカリングも効かない。
    virtual bool IsEnabled(const RenderPassContext& /*ctx*/) const { return true; }

    /// @note false のとき、出力から到達できなくてもカリングされない (外部副作用が目的のパス)。
    virtual bool AllowCulling() const { return true; }

    virtual void Execute(PassResources& resources, RenderPassContext& ctx) = 0;
};

} /// @note namespace fbzz::renderer
