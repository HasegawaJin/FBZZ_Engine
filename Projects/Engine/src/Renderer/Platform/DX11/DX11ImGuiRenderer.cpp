// FBZZ Engine
// DX11ImGuiRenderer.cpp | fbzz::renderer
// ImGui バックエンドの DX11 実装
// WHAT: imgui_impl_win32 / imgui_impl_dx11 を IImGuiRenderer 経由で呼び出す。
// WHY: DX11Renderer に ImGui 依存を混ぜると、通常描画インターフェースの責務が膨らむため別クラスへ分離する。
#include "DX11ImGuiRenderer.hpp"

#include <Engine/Renderer/IRenderTarget.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include "DX11Texture.hpp"
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

namespace fbzz::renderer {

DX11ImGuiRenderer::~DX11ImGuiRenderer()
{
    ImGuiShutdown();
}

bool DX11ImGuiRenderer::Init(ID3D11Device* device, ID3D11DeviceContext* context)
{
    if (!device || !context) {
        return false;
    }

    m_device  = device;
    m_context = context;
    return true;
}

void DX11ImGuiRenderer::ImGuiInit(void* hwnd)
{
    if (m_imguiInitialized) {
        return;
    }

    const bool win32Initialized = ImGui_ImplWin32_Init(hwnd);
    const bool dx11Initialized  = ImGui_ImplDX11_Init(m_device, m_context);
    m_imguiInitialized = win32Initialized && dx11Initialized;

    // WHY: 片方だけ初期化できた場合も、そのバックエンドが確保したリソースを残さない。
    if (!m_imguiInitialized) {
        if (dx11Initialized)
            ImGui_ImplDX11_Shutdown();
        if (win32Initialized)
            ImGui_ImplWin32_Shutdown();
    }
}

void DX11ImGuiRenderer::ImGuiShutdown()
{
    if (!m_imguiInitialized) {
        return;
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    m_imguiInitialized = false;
}

void DX11ImGuiRenderer::ImGuiNewFrame()
{
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
}

void DX11ImGuiRenderer::ImGuiRenderDrawData()
{
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}

void* DX11ImGuiRenderer::GetImTextureID(ResourceHandle<RenderTargetTag> rt, ResourceManager& resources, int slot)
{
    auto* target = resources.Get(rt);
    if (!target) {
        return nullptr;
    }
    return target->GetNativeSRV(slot);
}

void* DX11ImGuiRenderer::GetImTextureID(ResourceHandle<TextureTag> texture, ResourceManager& resources)
{
    auto* tex = resources.Get(texture);
    if (!tex) {
        return nullptr;
    }

    auto* dxTexture = static_cast<DX11Texture*>(tex);
    return dxTexture->GetSRV();
}

} // namespace fbzz::renderer
