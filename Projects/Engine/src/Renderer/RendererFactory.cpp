/// @file    RendererFactory.cpp
/// @brief   RendererBackend → 具象バックエンドの生成・配線を担う合成ポイント。
/// @author  Hasegawa Jin
/// @date    2026-07-02
///
/// WHY: 具象ヘッダー (DX11Renderer / DX11ImGuiRenderer) を include するのは本ファイルだけに限定し、
/// 上位レイヤー (Application / GameHub) が DX11 を知らずにバックエンドを選べるようにする。
/// DX12 追加時は switch に case を足すだけで済む。
#include "Engine/Renderer/RendererFactory.hpp"

#include "Engine/Core/Logger.hpp"
#include "Platform/DX11/DX11ImGuiRenderer.hpp"
#include "Platform/DX11/DX11Renderer.hpp"
#include "Platform/DX12/DX12ImGuiRenderer.hpp"
#include "Platform/DX12/DX12Renderer.hpp"

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

// DX12 バックエンドと ImGui を同一 DX12Context へ配線する。
// WHY: CommandList への UI 描画記録は通常描画と同じフレーム内で行う必要があるため、
//      Device だけでなく実行コンテキスト全体を共有する。
RendererBundle CreateDX12(void* hwnd, uint32_t width, uint32_t height)
{
    FBZZ_LOG_INFO("RendererFactory: DX12 バックエンド生成開始 (hwnd=%p %ux%u)", hwnd, width, height);
    auto renderer = std::make_unique<DX12Renderer>();
    if (!renderer->Init(static_cast<HWND>(hwnd), width, height)) {
        FBZZ_LOG_ERROR("RendererFactory: DX12Renderer の初期化に失敗しました");
        return {};
    }
    FBZZ_LOG_INFO("RendererFactory: DX12Renderer OK — ImGui バックエンド初期化へ");

    auto imgui = std::make_unique<DX12ImGuiRenderer>();
    if (!imgui->Init(&renderer->GetContext())) {
        FBZZ_LOG_ERROR("RendererFactory: DX12ImGuiRenderer の初期化に失敗しました");
        return {};
    }
    FBZZ_LOG_INFO("RendererFactory: DX12 バックエンド生成完了");

    RendererBundle bundle;
    bundle.renderer = std::move(renderer);
    bundle.imguiRenderer = std::move(imgui);
    return bundle;
}

} // namespace

RendererBundle CreateRenderer(RendererBackend backend, void* hwnd, uint32_t width, uint32_t height)
{
    FBZZ_LOG_INFO("RendererFactory: CreateRenderer 要求 backend=%s", ToString(backend));
    switch (backend) {
    case RendererBackend::DX11:
        return CreateDX11(hwnd, width, height);
    case RendererBackend::DX12:
        return CreateDX12(hwnd, width, height);
    }

    FBZZ_LOG_ERROR("RendererFactory: 未対応のバックエンドが指定されました (%d)",
                   static_cast<int>(backend));
    return {};
}

} // namespace fbzz::renderer
