/// @file    RenderTrailExtractor.hpp
/// @brief   トレイルの更新と描画用スナップショットの抽出。
/// @author  Hasegawa Jin
/// @date    2026-09-21
#pragma once
namespace fbzz::renderer { struct RenderScene; }
namespace fbzz::scene { struct RenderPassContext; void ExtractRenderTrails(RenderPassContext& ctx, renderer::RenderScene& output); }
