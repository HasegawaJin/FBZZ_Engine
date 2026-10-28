/// @file    RenderWaterExtractor.hpp
/// @brief   水面の地形交差・流れ・描画資源の抽出。
/// @author  Hasegawa Jin
/// @date    2026-09-21
#pragma once
namespace fbzz::renderer { struct RenderScene; class ResourceManager; }
namespace fbzz::scene {
class Scene;
struct EntityID;
struct RenderPassContext;
void ExtractRenderWater(RenderPassContext& ctx, renderer::RenderScene& output);
/// @note Scene の破棄・Clear は、その Scene 世代が所有する水面資源だけを返す。
void ReleaseWaterRenderResources(Scene& scene, renderer::ResourceManager& resources);
void ReleaseWaterRenderResources(Scene& scene, EntityID entity, renderer::ResourceManager& resources);
}
