/// @file    GpuHiZ.hpp
/// @brief   現在の描画深度から作る GPU 遮蔽階層。
/// @author  Hasegawa Jin
/// @date    2026-11-07
#pragma once
#include <Graphics/Api.hpp>
#include <Graphics/Renderer/ResourceHandle.hpp>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace fbzz::renderer {

class IRenderer;
class ResourceManager;
struct DrawCall;
struct RenderScene;
struct RenderPassContext;

inline constexpr std::string_view OCCLUDER_DEPTH = "OccluderDepth";
inline constexpr std::string_view OCCLUSION_HIZ = "OcclusionHiZ";
inline constexpr std::string_view OCCLUSION_DRAW_ARGUMENTS = "OcclusionDrawArguments";
inline constexpr std::string_view DEPTH_PREPASS = "DepthPrePass";
inline constexpr std::string_view BUILD_OCCLUSION_HIZ = "BuildOcclusionHiZ";
inline constexpr std::string_view CULL_OCCLUDED_MESHES = "CullOccludedMeshes";
inline constexpr uint32_t OCCLUSION_DRAW_ARGUMENT_STRIDE = 20;

/// @note float の各段を row-major で連結する。先頭段は 4×4 ピクセルを覆い、各軸の段寸法は切り上げる。
/// @note 寸法はビュー内のピクセル数。描画を続ける同じビューと深度書き込みだけに対応する。
struct GpuHiZ {
    ResourceHandle<StructuredBufferTag> depths;
    uint32_t width = 0;
    uint32_t height = 0;
    bool reversedZ = false;
};

/// @param source 現在の描画先。深度を書いた後、後続の描画が始まる前に呼ぶ。
/// @param viewportWidth 0 は viewportHeight も 0 のとき描画先全体を示す。
/// @return 構築できなければ depths は無効。構築後は source と指定ビューポートを復元する。
/// @pre DIRECT queue の独立 dispatch。相互依存のない compute batch 内で呼ばないこと。
[[nodiscard]] FBZZ_GRAPHICS_API GpuHiZ BuildGpuHiZ(IRenderer& renderer, ResourceManager& resources,
    ResourceHandle<RenderTargetTag> source, uint32_t viewportX = 0, uint32_t viewportY = 0,
    uint32_t viewportWidth = 0, uint32_t viewportHeight = 0);

[[nodiscard]] bool SupportsViewGpuOcclusion(const RenderPassContext& ctx);
[[nodiscard]] bool DrawOpaqueOccluderDepth(RenderPassContext& ctx);
void CullOccludedMeshes(RenderPassContext& ctx, const GpuHiZ& hierarchy);
[[nodiscard]] GpuHiZ ReadViewGpuHiZ(const RenderPassContext& ctx);
[[nodiscard]] bool BindMeshOcclusionArguments(RenderPassContext& ctx, const RenderScene& scene, std::size_t itemIndex,
    ResourceHandle<StructuredBufferTag> arguments, DrawCall& draw);

} /// @note namespace fbzz::renderer
