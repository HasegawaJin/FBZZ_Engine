/// @file    RendererFactory.hpp
/// @brief   バックエンド具象を選択して IRenderer / IImGuiRenderer を生成する唯一の窓口。
/// @author  Hasegawa Jin
/// @date    2026-07-02
/// @note 合成ルート (Application) に具象名を漏らさないための境界。具象ヘッダーを知るのは
/// @note       RendererFactory.cpp だけで、上位レイヤーは RendererBackend enum と
/// @note       IRenderer / IImGuiRenderer 抽象しか参照しない。
/// @note ImGui バックエンドは GPU デバイスを必要とするため、具象デバイスの橋渡しが要る。
/// @note       この「デバイス配線」もバックエンド固有の知識なので、Application ではなく
/// @note       ファクトリ内部 (実際には各バックエンドの翻訳単位) に閉じ込める。
/// @note DirectX 11 サポートは v1.0 で終了した。バックエンドが 1 つになった今もこの窓口を
/// @note       残しているのは、次のバックエンドを足すときに Application を書き換えずに済ませるため。
/// @see  Docs/design/dx11-removal.md
#pragma once
#include <cstdint>
#include <memory>

#include "IImGuiRenderer.hpp"
#include "IRenderer.hpp"
#include "RendererBackend.hpp"

namespace fbzz::renderer {

/// @brief CreateRenderer が返す IRenderer + ImGui バックエンドのペア。
/// @note 両者は同一デバイスを共有するため、個別生成では合成ルートが再びデバイス配線を
/// @note       知る必要が出る。ペアで返すことで配線をファクトリ内部に閉じ込める。
/// @note       生成失敗時は両ポインタが nullptr。
struct RendererBundle {
    std::unique_ptr<IRenderer>      renderer;
    std::unique_ptr<IImGuiRenderer> imguiRenderer;
};

/// @brief 指定バックエンドの IRenderer を初期化し、同一デバイスへ紐づけた IImGuiRenderer を生成する。
/// @param hwnd Win32 ウィンドウハンドル。ヘッダーの Windows.h 依存を避けるため void* で受ける。
/// @return いずれかの初期化に失敗した場合は空の RendererBundle (nullptr ペア)。
/// @note ImGui バックエンドの Win32 初期化 (ImGuiInit) は呼び出し側が別途行う。
/// @note       ここで担うのは GPU デバイスへの紐づけのみ。
RendererBundle CreateRenderer(RendererBackend backend, void* hwnd, uint32_t width, uint32_t height);

} /// @note namespace fbzz::renderer
