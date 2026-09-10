/// @file    IRenderPass.hpp
/// @brief   レンダーパスの基底インターフェース。
/// @author  Hasegawa Jin
/// @date    2026-06-18
///
/// RenderPipeline::AddPass<T>() で登録し、Execute 時に RenderGraph へ自動組み込みされる。
///
/// 契約は 4 つ。
///   Name      … グラフ上の名前。プロファイラーと構成テキストにも出る
///   Setup     … 読む / 書くものを申告する «唯一の» 場所
///   IsEnabled … このフレームに載せるか。Execute 冒頭の早期 return は書かない
///   Execute   … 申告した名前からだけ実体を引いて描く
#pragma once
#include "PassResources.hpp"

#include <string_view>

namespace fbzz::scene {

struct RenderPassContext;

class IRenderPass {
public:
    virtual ~IRenderPass() = default;

    virtual std::string_view Name() const = 0;

    // 読み書きするリソースを申告する。RenderGraph の依存解析はここだけを見る。
    virtual void Setup(PassBuilder& builder, const RenderPassContext& ctx) const = 0;

    // false を返すとこのフレームはグラフに載らない。
    // WHY Execute 冒頭の早期 return と分けるか: 早期 return だとパスはグラフに載ったまま
    //     依存の鎖を伸ばし続け、デッドパスカリングも効かない。
    virtual bool IsEnabled(const RenderPassContext& /*ctx*/) const { return true; }

    // false のとき、出力から到達できなくてもカリングされない (外部副作用が目的のパス)。
    virtual bool AllowCulling() const { return true; }

    virtual void Execute(PassResources& resources, RenderPassContext& ctx) = 0;
};

} // namespace fbzz::scene
