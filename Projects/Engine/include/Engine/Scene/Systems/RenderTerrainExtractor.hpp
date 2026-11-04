/// @file    RenderTerrainExtractor.hpp
/// @brief   地形パッチとマテリアル資源の抽出。
/// @author  Hasegawa Jin
/// @date    2026-09-21
#pragma once
#include <Engine/Api.hpp>
namespace fbzz::renderer { struct RenderScene; }
namespace fbzz::scene {
class Scene;
struct EntityID;
struct RenderPassContext;
FBZZ_ENGINE_API void ExtractRenderTerrains(RenderPassContext& ctx, renderer::RenderScene& output);
/// @note Scene 終了時は全作成元のキャッシュを返し、Entity 終了時はその個体だけを返す。
FBZZ_ENGINE_API void ReleaseTerrainRenderResources(Scene& scene);
FBZZ_ENGINE_API void ReleaseTerrainRenderResources(Scene& scene, EntityID entity);
}
