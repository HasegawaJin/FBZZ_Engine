/// @file    RenderCustomPostExtractor.hpp
/// @brief   カスタムポスト処理の解決済み入力。
/// @author  Hasegawa Jin
/// @date    2026-09-21
#pragma once
namespace fbzz::renderer { struct RenderScene; }
namespace fbzz::scene { struct RenderPassContext; void ExtractRenderCustomPost(RenderPassContext&, renderer::RenderScene&); }
