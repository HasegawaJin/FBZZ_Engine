// FBZZ Engine
// RendererFactory.hpp | fbzz::renderer
// バックエンド具象 (DX11 / 将来の DX12) を選択して IRenderer / IImGuiRenderer を生成する唯一の窓口。
//
// 設計方針:
//   これまで Application.cpp が DX11Renderer / DX11ImGuiRenderer を直接 make_unique しており、
//   合成ルートに DX11 具象名が漏れていた。DX12 バックエンド追加時に Application を書き換えずに済むよう、
//   バックエンド選択を本ファクトリへ集約する。具象ヘッダーを知るのは RendererFactory.cpp のみとなり、
//   上位レイヤーは RendererBackend enum と IRenderer / IImGuiRenderer 抽象だけを参照する。
//
//   ImGui バックエンドは GPU デバイスを必要とするため、DX11 では DX11Renderer が保持する
//   Device / Context を DX11ImGuiRenderer へ橋渡しする必要がある。この「デバイス配線」も
//   バックエンド固有の知識なので、Application ではなくファクトリ内部に閉じ込める。
#pragma once
#include <cstdint>
#include <memory>

#include "IImGuiRenderer.hpp"
#include "IRenderer.hpp"
#include "RendererBackend.hpp" // RendererBackend enum (旧: 本ファイル内で定義。共有のため分離)

namespace fbzz::renderer {

// CreateRenderer が返す IRenderer + ImGui バックエンドのペア。
// WHY: 両者は同一デバイスを共有するため、個別生成では合成ルートが再びデバイス配線を知る必要が出る。
//      ペアで返すことで配線をファクトリ内部に閉じ込める。生成失敗時は両ポインタが nullptr。
struct RendererBundle {
    std::unique_ptr<IRenderer>      renderer;
    std::unique_ptr<IImGuiRenderer> imguiRenderer;
};

// 指定バックエンドの IRenderer を hwnd / 解像度で初期化し、対応する IImGuiRenderer を
// 同一デバイスに紐づけて生成する。
//   hwnd : Win32 ウィンドウハンドル。ヘッダーの Windows.h 依存を避けるため void* で受ける。
// いずれかの初期化に失敗した場合は空の RendererBundle (nullptr ペア) を返す。
// なお ImGui バックエンドの Win32 初期化 (ImGuiInit) は呼び出し側が別途行う。ここでは
// GPU デバイスへの紐づけ (DX11 の Device/Context 配線) のみを担う。
RendererBundle CreateRenderer(RendererBackend backend, void* hwnd, uint32_t width, uint32_t height);

} // namespace fbzz::renderer
