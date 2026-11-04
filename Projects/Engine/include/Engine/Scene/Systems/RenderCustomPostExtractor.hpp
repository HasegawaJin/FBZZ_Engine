/// @file    RenderCustomPostExtractor.hpp
/// @brief   カスタムポスト処理の解決済み入力。
/// @author  Hasegawa Jin
/// @date    2026-09-21
#pragma once
#include <Engine/Api.hpp>
namespace fbzz::renderer { struct RenderScene; }
namespace fbzz::scene { struct RenderPassContext; FBZZ_ENGINE_API void ExtractRenderCustomPost(RenderPassContext&, renderer::RenderScene&); }
