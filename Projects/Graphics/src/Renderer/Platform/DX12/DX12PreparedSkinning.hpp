/// @file    DX12PreparedSkinning.hpp
/// @brief   専用 DIRECT 列と世代別頂点出力を保持する内部 backend。
/// @author  Hasegawa Jin
/// @date    2026-10-16
#pragma once
#include <Graphics/Api.hpp>
#include "../../PreparedSkinningBackend.hpp"

namespace fbzz::renderer {
class DX12Context;
class DX12PsoCache;
class DX12StateTracker;
class DX12PreparedSkinningPool;
FBZZ_GRAPHICS_API std::shared_ptr<DX12PreparedSkinningPool> CreatePreparedSkinningPool(DX12Context& context);
FBZZ_GRAPHICS_API void StopPreparedSkinningPool(const std::shared_ptr<DX12PreparedSkinningPool>& pool);
FBZZ_GRAPHICS_API void BeginPreparedSkinningFrame(const std::shared_ptr<DX12PreparedSkinningPool>& pool);
FBZZ_GRAPHICS_API void EndPreparedSkinningFrame(const std::shared_ptr<DX12PreparedSkinningPool>& pool, uint64_t actualFence);
FBZZ_GRAPHICS_API std::unique_ptr<prepared_skinning_detail::NativeWork> PrepareDX12Skinning(
    const std::shared_ptr<DX12PreparedSkinningPool>& pool, DX12Context& context,
    DX12PsoCache& pipelines, DX12StateTracker& tracker, ResourceManager& resources,
    ResourceHandle<ShaderTag> shader, std::span<const CapturedSkinningRequest> requests,
    std::span<const math::Matrix4> matrices);
} /// @note namespace fbzz::renderer
