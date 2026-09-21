/// @file    DX12PipelineState.hpp
/// @brief   遅延 PSO 生成に使うバックエンド非依存状態の記述子。
/// @author  Hasegawa Jin
/// @date    2026-07-15
#pragma once

#include <Graphics/Renderer/IPipelineState.hpp>

namespace fbzz::renderer {

class DX12PipelineState final : public IPipelineState {
public:
    explicit DX12PipelineState(const PipelineStateDesc& desc) : m_desc(desc) {}
    const PipelineStateDesc& GetDesc() const override { return m_desc; }

private:
    PipelineStateDesc m_desc;
};

} /// @note namespace fbzz::renderer
