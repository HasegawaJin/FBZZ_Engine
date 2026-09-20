/// @file    RenderEnvironmentExtractor.hpp
/// @brief   Graphics へ渡す環境値の抽出境界。
/// @author  Hasegawa Jin
/// @date    2026-09-21
#pragma once
#include <Graphics/Renderer/RenderEnvironment.hpp>
namespace fbzz::scene {
struct RenderPassContext;
renderer::RenderEnvironmentInput ExtractRenderEnvironment(RenderPassContext& ctx);
}
