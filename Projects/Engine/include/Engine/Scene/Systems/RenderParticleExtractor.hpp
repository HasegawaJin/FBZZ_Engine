/// @file    RenderParticleExtractor.hpp
/// @brief   粒子の物理更新・アセット解決と GPU 更新要求の抽出。
/// @author  Hasegawa Jin
/// @date    2026-09-21
#pragma once
#include <Engine/Api.hpp>
namespace fbzz::renderer { struct RenderScene; }
namespace fbzz::scene {
class Scene;
struct RenderPassContext;
/// @note Append once after the snapshot's particle simulation. GPU alive counts are unavailable on CPU, so positive draw capacity is conservatively unsupported.
/// @note 実験 RT の許可なしでは Scene を走査せず、output を変更しない。
FBZZ_ENGINE_API void ExtractRayParticleSources(const Scene& scene, renderer::RenderScene& output,
    bool experimentalRayTracingEnabled = false);
FBZZ_ENGINE_API void ExtractRenderParticles(RenderPassContext& ctx, renderer::RenderScene& output);
/// @note Scene の終了時に全作成元の材質資源を返す。
FBZZ_ENGINE_API void ReleaseParticleRenderResources(Scene& scene);
}
