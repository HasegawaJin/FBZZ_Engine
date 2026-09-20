/// @file    BackendEntry.hpp
/// @brief   各バックエンドライブラリが外へ公開する唯一の生成関数。
/// @author  Hasegawa Jin
/// @date    2026-09-03
/// @note 具象型は各バックエンドの PRIVATE include に閉じており RendererFactory からも
/// @note       見えないため、生成と配線はバックエンド側の翻訳単位で完結させ、外へはこの
/// @note       シグネチャだけを出す。
/// @see  Docs/design/dx11-removal.md
#pragma once
#include <cstdint>

#include "RendererFactory.hpp"

namespace fbzz::renderer {

#if FBZZ_ENABLE_DX12
/// @brief DX12 バックエンドと ImGui を同一 DX12Context へ配線して生成する。
/// @param hwnd Win32 ウィンドウハンドル。境界に Windows.h を持ち込まないため void*。
/// @return 初期化に失敗した場合は両ポインタが nullptr。
/// @note FBZZ_ENABLE_DX12=OFF の構成では定義ごと存在しない。DX11 撤去後、この構成は
/// @note       描画バックエンドを 1 つも持たないため、カバレッジ計測など GPU を使わない
/// @note       ビルド専用となる。
RendererBundle CreateDX12Backend(void* hwnd, uint32_t width, uint32_t height);
#endif

} /// @note namespace fbzz::renderer
