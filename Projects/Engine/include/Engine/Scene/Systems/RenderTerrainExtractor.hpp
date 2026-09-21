/// @file    RenderTerrainExtractor.hpp
/// @brief   地形パッチとマテリアル資源の抽出。
/// @author  Hasegawa Jin
/// @date    2026-09-21
#pragma once
namespace fbzz::renderer { struct RenderScene; }
namespace fbzz::scene { struct RenderPassContext; void ExtractRenderTerrains(RenderPassContext& ctx, renderer::RenderScene& output); }
