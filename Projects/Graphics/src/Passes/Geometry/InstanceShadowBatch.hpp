/// @file    InstanceShadowBatch.hpp
/// @brief   Prepares InstanceSet visibility for all light views before submitting atlas draws.
/// @author  Hasegawa Jin
/// @date    2026-11-07
#pragma once
#include <Graphics/Renderer/ComputeCall.hpp>
#include <Math/Frustum.hpp>
#include <memory>
#include <span>
#include <vector>

namespace fbzz::renderer {

struct RenderPassContext;

struct InstanceShadowView {
    math::Frustum frustum;
    bool directional = false;
};

class PreparedInstanceShadowBatch {
public:
    PreparedInstanceShadowBatch();
    ~PreparedInstanceShadowBatch();
    PreparedInstanceShadowBatch(PreparedInstanceShadowBatch&&) noexcept;
    PreparedInstanceShadowBatch& operator=(PreparedInstanceShadowBatch&&) noexcept;

private:
    struct State;
    std::unique_ptr<State> m_state;

    friend PreparedInstanceShadowBatch PrepareInstanceShadowBatch(RenderPassContext&,
        std::span<const InstanceShadowView>,std::vector<ComputeCall>&);
    friend void SubmitInstanceShadowBatchView(RenderPassContext&,const PreparedInstanceShadowBatch&,
        size_t,std::span<const uint8_t>,ResourceHandle<ConstantBufferTag>);
};

/// @note Commands append to the caller's batch and share its receipt indices. Visibility outputs are leased for this resource frame.
[[nodiscard]] PreparedInstanceShadowBatch PrepareInstanceShadowBatch(RenderPassContext& context,
    std::span<const InstanceShadowView> views,std::vector<ComputeCall>& commands);

/// @note The caller has bound the light viewport and uploaded its frame constants before replaying this view.
void SubmitInstanceShadowBatchView(RenderPassContext& context,const PreparedInstanceShadowBatch& batch,
    size_t viewIndex,std::span<const uint8_t> receipts,ResourceHandle<ConstantBufferTag> lightFrame);

} /// @note namespace fbzz::renderer
