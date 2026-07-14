// FBZZ Engine
// DX12PipelineState.hpp | fbzz::renderer
// 遅延 PSO 生成に使うバックエンド非依存状態の記述子
#pragma once

#include <Engine/Renderer/IPipelineState.hpp>

namespace fbzz::renderer {

class DX12PipelineState final : public IPipelineState {
public:
    explicit DX12PipelineState(const PipelineStateDesc& desc) : m_desc(desc) {}
    const PipelineStateDesc& GetDesc() const override { return m_desc; }

private:
    PipelineStateDesc m_desc;
};

} // namespace fbzz::renderer
