/// @file    DX12Capabilities.cpp
/// @brief   DX12 の能力照会を公開 GraphicsCapabilities へ変換する。
/// @author  Hasegawa Jin
/// @date    2026-09-30
#include "DX12Renderer.hpp"

namespace fbzz::renderer {

GraphicsCapabilities DX12Renderer::GetCapabilities() const
{
    if (!m_context.GetDevice())
        return {};

    /// @see https://devblogs.microsoft.com/directx/dxr-1-1/ Inline Raytracing と DXR Tier 1.1。
    return { m_context.SupportsBindless(), m_context.SupportsInlineRaytracing(),
             m_context.SupportsRaytracingPipeline() };
}

}
