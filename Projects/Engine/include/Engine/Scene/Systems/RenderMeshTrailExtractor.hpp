/// @file    RenderMeshTrailExtractor.hpp
/// @brief   メッシュ残像の過去姿勢と材質資源の抽出。
/// @author  Hasegawa Jin
/// @date    2026-09-21
#pragma once
namespace fbzz::renderer { struct RenderScene; }
namespace fbzz::scene { struct RenderPassContext; void ExtractRenderMeshTrails(RenderPassContext& ctx, renderer::RenderScene& output); }
