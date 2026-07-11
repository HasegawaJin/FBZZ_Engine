// FBZZ Engine
// RendererFactory.cpp | fbzz::renderer
// RendererBackend → 具象バックエンドの生成・配線を担う合成ポイント。
// WHY: 具象ヘッダー (DX11Renderer / DX11ImGuiRenderer) を include するのは本ファイルだけに限定し、
//      上位レイヤー (Application / GameHub) が DX11 を知らずにバックエンドを選べるようにする。
//      DX12 追加時は switch に case を足すだけで済む。
#include "Engine/Renderer/RendererFactory.hpp"

#include "Engine/Core/Logger.hpp"
#include "Platform/DX11/DX11ImGuiRenderer.hpp"
#include "Platform/DX11/DX11Renderer.hpp"

namespace fbzz::renderer {

namespace {

// DX11 バックエンドを生成し、ImGui バックエンドを同一デバイスへ配線する。
// WHY: DX11ImGuiRenderer は GPU リソース生成に DX11 デバイスを要するため、
//      DX11Renderer が保持する Device / Context をここで橋渡しする。この配線知識は
//      DX11 固有なのでファクトリ内部に閉じ込め、合成ルートには漏らさない。
RendererBundle CreateDX11(void* hwnd, uint32_t width, uint32_t height)
{
    auto renderer = std::make_unique<DX11Renderer>();
    if (!renderer->Init(static_cast<HWND>(hwnd), width, height)) {
        FBZZ_LOG_ERROR("RendererFactory: DX11Renderer の初期化に失敗しました");
        return {};
    }

    auto imgui = std::make_unique<DX11ImGuiRenderer>();
    if (!imgui->Init(renderer->GetDevice(), renderer->GetDeviceContext())) {
        FBZZ_LOG_ERROR("RendererFactory: DX11ImGuiRenderer の初期化に失敗しました");
        return {};
    }

    RendererBundle bundle;
    bundle.renderer      = std::move(renderer);
    bundle.imguiRenderer = std::move(imgui);
    return bundle;
}

} // namespace

RendererBundle CreateRenderer(RendererBackend backend, void* hwnd, uint32_t width, uint32_t height)
{
    switch (backend) {
    case RendererBackend::DX11:
        return CreateDX11(hwnd, width, height);
    }

    FBZZ_LOG_ERROR("RendererFactory: 未対応のバックエンドが指定されました (%d)",
                   static_cast<int>(backend));
    return {};
}

} // namespace fbzz::renderer
