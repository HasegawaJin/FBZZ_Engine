// FBZZ Engine
// DX11ImGuiRenderer.cpp | fbzz::renderer
// ImGui バックエンドの DX11 実装
// WHAT: imgui_impl_win32 / imgui_impl_dx11 を IImGuiRenderer 経由で呼び出す。
// WHY: DX11Renderer に ImGui 依存を混ぜると、通常描画インターフェースの責務が膨らむため別クラスへ分離する。
#include "DX11ImGuiRenderer.hpp"

#include <Engine/Renderer/IRenderTarget.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

namespace fbzz::renderer {

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
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(m_device, m_context);
}

void DX11ImGuiRenderer::ImGuiShutdown()
{
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
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

} // namespace fbzz::renderer
