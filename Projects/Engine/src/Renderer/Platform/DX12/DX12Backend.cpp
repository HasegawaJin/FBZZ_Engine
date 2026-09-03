/// @file    DX12Backend.cpp
/// @brief   DX12 バックエンドの生成と、ImGui バックエンドとのコンテキスト配線。
/// @author  Hasegawa Jin
/// @date    2026-09-03
#include <Engine/Renderer/BackendEntry.hpp>

#include <Engine/Core/Logger.hpp>

#include "DX12ImGuiRenderer.hpp"
#include "DX12Renderer.hpp"

namespace fbzz::renderer {

// CommandList への UI 描画記録は通常描画と同じフレーム内で行うため、ImGui へは
// Device 単体ではなく実行コンテキスト全体を渡す。
RendererBundle CreateDX12Backend(void* hwnd, uint32_t width, uint32_t height)
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
    bundle.renderer      = std::move(renderer);
    bundle.imguiRenderer = std::move(imgui);
    return bundle;
}

} // namespace fbzz::renderer
