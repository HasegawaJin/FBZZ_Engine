/// @file    IPipelineState.hpp
/// @brief   パイプライン状態の抽象インターフェース。
/// @author  Hasegawa Jin
/// @date    2026-05-21
/// @note ブレンド・深度・カリングなど描画状態をバックエンド非依存でまとめる。
/// @note DrawCall はハンドルでこの状態を参照する。
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
