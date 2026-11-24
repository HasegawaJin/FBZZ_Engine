/// @file    RenderMeshTrailExtractor.hpp
/// @brief   メッシュ残像の過去姿勢と材質資源の抽出。
/// @author  Hasegawa Jin
/// @date    2026-09-21
#pragma once
#include <Engine/Api.hpp>
namespace fbzz::renderer { struct RenderScene; }
namespace fbzz::scene {
class Scene;
struct RenderPassContext;
FBZZ_ENGINE_API void ExtractRenderMeshTrails(RenderPassContext& ctx, renderer::RenderScene& output);
/// @note Scene の終了時に全作成元の材質バッファを返す。共有シェーダーは Manager が所有する。
FBZZ_ENGINE_API void ReleaseMeshTrailRenderResources(Scene& scene);
}
