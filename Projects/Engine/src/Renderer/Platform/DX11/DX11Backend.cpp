/// @file    DX11Backend.cpp
/// @brief   DX11 バックエンドの生成と、ImGui バックエンドとのデバイス配線。
/// @author  Hasegawa Jin
/// @date    2026-09-03
#include <Engine/Renderer/BackendEntry.hpp>

#include <Engine/Core/Logger.hpp>

#include "DX11ImGuiRenderer.hpp"
#include "DX11Renderer.hpp"

namespace fbzz::renderer {

// DX11ImGuiRenderer は GPU リソース生成に DX11 デバイスを要するため、
// DX11Renderer が保持する Device / Context をここで橋渡しする。
RendererBundle CreateDX11Backend(void* hwnd, uint32_t width, uint32_t height)
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

} // namespace fbzz::renderer
