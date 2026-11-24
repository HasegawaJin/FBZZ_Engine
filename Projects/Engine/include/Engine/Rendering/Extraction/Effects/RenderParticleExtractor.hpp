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
/// @note 粒子シミュレーション後に一度だけ追加する。CPU から GPU の生存数は取得できないため、描画可能数が正ならレイ描画の未対応要素に数える。
/// @note 実験 RT の許可なしでは Scene を走査せず、output を変更しない。
FBZZ_ENGINE_API void ExtractRayParticleSources(const Scene& scene, renderer::RenderScene& output,
    bool experimentalRayTracingEnabled = false);
FBZZ_ENGINE_API void ExtractRenderParticles(RenderPassContext& ctx, renderer::RenderScene& output);
/// @note Scene の終了時に全作成元の材質資源を返す。
FBZZ_ENGINE_API void ReleaseParticleRenderResources(Scene& scene);
}
