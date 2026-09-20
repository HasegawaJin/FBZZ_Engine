/// @file    RenderWaterExtractor.hpp
/// @brief   水面の地形交差・流れ・描画資源の抽出。
/// @author  Hasegawa Jin
/// @date    2026-09-21
#pragma once
namespace fbzz::renderer { struct RenderScene; }
namespace fbzz::scene { struct RenderPassContext; void ExtractRenderWater(RenderPassContext& ctx, renderer::RenderScene& output); }
