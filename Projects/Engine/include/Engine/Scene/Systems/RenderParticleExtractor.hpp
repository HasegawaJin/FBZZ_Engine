/// @file    RenderParticleExtractor.hpp
/// @brief   粒子の物理更新・アセット解決と GPU 更新要求の抽出。
/// @author  Hasegawa Jin
/// @date    2026-09-21
#pragma once
namespace fbzz::renderer { struct RenderScene; }
namespace fbzz::scene { struct RenderPassContext; void ExtractRenderParticles(RenderPassContext& ctx, renderer::RenderScene& output); }
