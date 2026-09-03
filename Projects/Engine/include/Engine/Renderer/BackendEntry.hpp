/// @file    BackendEntry.hpp
/// @brief   各バックエンドライブラリが外へ公開する唯一の生成関数。
/// @author  Hasegawa Jin
/// @date    2026-09-03
#pragma once
#include <cstdint>

#include "RendererFactory.hpp"

namespace fbzz::renderer {

/// DX11 バックエンドと、同じデバイスに紐づく ImGui バックエンドを生成する。
///
/// 具象型は各バックエンドの PRIVATE include に閉じており RendererFactory からも
/// 見えないため、生成と配線はバックエンド側の翻訳単位で完結させ、外へはこの
/// シグネチャだけを出す。
/// @param hwnd Win32 ウィンドウハンドル。境界に Windows.h を持ち込まないため void*。
/// @return 初期化に失敗した場合は両ポインタが nullptr。
RendererBundle CreateDX11Backend(void* hwnd, uint32_t width, uint32_t height);

#if FBZZ_ENABLE_DX12
/// DX12 バックエンドと ImGui を同一 DX12Context へ配線して生成する。
/// FBZZ_ENABLE_DX12=OFF の構成では定義ごと存在しない。
RendererBundle CreateDX12Backend(void* hwnd, uint32_t width, uint32_t height);
#endif

} // namespace fbzz::renderer
