/// @file    RenderFiberExtractor.hpp
/// @brief   繊維表面・骨・接触・場の描画入力の抽出。
/// @author  Hasegawa Jin
/// @date    2026-09-21
#pragma once
namespace fbzz::renderer { struct RenderScene; }
namespace fbzz::scene { struct RenderPassContext; void ExtractRenderFibers(RenderPassContext& ctx, renderer::RenderScene& output); }
