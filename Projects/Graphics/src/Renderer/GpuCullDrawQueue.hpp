/// @file    GpuCullDrawQueue.hpp
/// @brief   Ordered value-owned GPU cull/draw requests with a direct fallback.
/// @author  Hasegawa Jin
/// @date    2026-10-19
#pragma once
#include <Graphics/Api.hpp>
#include <Graphics/Renderer/ComputeCall.hpp>
#include <Graphics/Renderer/DrawCall.hpp>
#include <span>

namespace fbzz::renderer {
class IRenderer;
class ResourceManager;
namespace gpu_cull_detail {
/// @note Requests own their handles; Manager has the same frame lifetime as ordinary Submit. No Scene is borrowed.
FBZZ_GRAPHICS_API void SubmitCullDraw(IRenderer& renderer, ResourceManager& resources,
    const ComputeCall& compute, const DrawCall& indirect, const DrawCall& fallback);

/// @note 記録順と各 Dispatch のバリアを保つ。呼び出し元が所有する両 span は完了まで有効であること。
/// @param receipts 各 Dispatch の記録結果 (0 または 1)。寸法不一致ではすべて 0。
/// @param stopOnFailure 依存する後続 Dispatch を失敗時に止め、未記録の結果を 0 にする。
void TryDispatchSequence(IRenderer& renderer, ResourceManager& resources,
    std::span<const ComputeCall> calls, std::span<uint8_t> receipts, bool stopOnFailure = false);
} /// @note namespace gpu_cull_detail
} /// @note namespace fbzz::renderer
