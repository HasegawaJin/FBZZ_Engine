/// @file    RenderWaterExtractor.hpp
/// @brief   水面の地形交差・流れ・描画資源の抽出。
/// @author  Hasegawa Jin
/// @date    2026-09-21
#pragma once
#include <Engine/Api.hpp>
namespace fbzz::renderer { struct RenderScene; class ResourceManager; }
namespace fbzz::scene {
class Scene;
struct EntityID;
struct RenderPassContext;
FBZZ_ENGINE_API void ExtractRenderWater(RenderPassContext& ctx, renderer::RenderScene& output);
/// @note Scene の破棄・Clear は、その Scene 世代が所有する水面資源だけを返す。
FBZZ_ENGINE_API void ReleaseWaterRenderResources(Scene& scene);
FBZZ_ENGINE_API void ReleaseWaterRenderResources(Scene& scene, EntityID entity);
}
