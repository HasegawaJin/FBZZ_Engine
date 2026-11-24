/// @file    RenderDecalExtractor.hpp
/// @brief   デカールの寿命更新と描画入力の抽出。
/// @author  Hasegawa Jin
/// @date    2026-09-21
#pragma once
#include <Engine/Api.hpp>
namespace fbzz::renderer { struct RenderScene; }
namespace fbzz::scene { struct RenderPassContext; FBZZ_ENGINE_API void ExtractRenderDecals(RenderPassContext& ctx, renderer::RenderScene& output); }
