// FBZZ Engine
// IPipelineState.hpp | fbzz::renderer
// パイプライン状態の抽象インターフェース
// ブレンド・深度・カリングなど描画状態をバックエンド非依存でまとめる。
// DrawCall はハンドルでこの状態を参照する。
#pragma once
#include "RenderState.hpp"

namespace fbzz::renderer
{
    class IPipelineState 
    {
    public:
        virtual ~IPipelineState() = default;

        virtual const PipelineStateDesc& GetDesc() const = 0;
    };
}
