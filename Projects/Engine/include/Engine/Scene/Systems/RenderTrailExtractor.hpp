/// @file    RenderTrailExtractor.hpp
/// @brief   トレイルの更新と描画用スナップショットの抽出。
/// @author  Hasegawa Jin
/// @date    2026-09-21
#pragma once
#include <Engine/Api.hpp>
namespace fbzz::renderer { struct RenderScene; }
namespace fbzz::scene { struct RenderPassContext; FBZZ_ENGINE_API void ExtractRenderTrails(RenderPassContext& ctx, renderer::RenderScene& output); }
