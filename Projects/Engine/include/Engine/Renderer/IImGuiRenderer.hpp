// FBZZ Engine
// IImGuiRenderer.hpp | fbzz::renderer
// ImGui バックエンドの抽象インターフェース
// WHY: IRenderer は通常描画 API の境界に限定し、エディター UI 用の ImGui 依存を別契約へ分離する。
#pragma once
#include "ResourceHandle.hpp"

namespace fbzz::renderer {

class ResourceManager;

class IImGuiRenderer {
public:
    virtual ~IImGuiRenderer() = default;

    // ImGui の Win32 / GPU バックエンドを初期化する。
    // hwnd は Win32 ウィンドウハンドルだが、ヘッダーの Windows.h 依存を避けるため void* で受ける。
    virtual void ImGuiInit(void* hwnd) = 0;

    // ImGui バックエンドを終了し、内部で保持する GPU / Win32 リソースを解放する。
    virtual void ImGuiShutdown() = 0;

    // ImGui フレーム開始前に、プラットフォーム入力と GPU バックエンド状態を更新する。
    virtual void ImGuiNewFrame() = 0;

    // ImGui::Render() 後の DrawData を現在のバックバッファへ送信する。
    virtual void ImGuiRenderDrawData() = 0;

    // RenderTarget を ImGui::Image が受け取れるテクスチャ ID へ変換する。
    // WHY: ImGui 表示用の ID 解決は UI バックエンド固有の処理なので IRenderer から分離する。
    virtual void* GetImTextureID(ResourceHandle<RenderTargetTag> rt, ResourceManager& resources, int slot = 0) = 0;
};

} // namespace fbzz::renderer
