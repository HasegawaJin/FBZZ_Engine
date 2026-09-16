/// @file    DX11ImGuiRenderer.hpp
/// @brief   ImGui バックエンドの DX11 実装。
/// @author  Hasegawa Jin
/// @date    2026-06-06
///
/// WHY: DX11Renderer から ImGui の初期化・フレーム制御を分離し、通常描画 API とエディター UI API を別責務にする。
#pragma once

#include <Engine/Renderer/IImGuiRenderer.hpp>
#include <d3d11.h>

namespace fbzz::renderer {

class DX11ImGuiRenderer final : public IImGuiRenderer {
public:
    // 呼び出し側の終了経路に依存せず、ImGui が所有する DX11 リソースを必ず解放する。
    ~DX11ImGuiRenderer() override;

    // DX11Renderer が所有する Device / Context を非所有で参照する。
    // WHY: ImGui バックエンドは GPU リソースを作るため DX11 デバイスが必要だが、
    //      デバイスの寿命は通常レンダラーが管理するため、ここでは所有しない。
    bool Init(ID3D11Device* device, ID3D11DeviceContext* context);

    // ImGui の Win32 / DX11 バックエンドを初期化する。
    void ImGuiInit(void* hwnd) override;

    // ImGui バックエンドを終了する。
    void ImGuiShutdown() override;

    // ImGui フレーム開始前に各バックエンドを更新する。
    void ImGuiNewFrame() override;

    // ImGui::Render() 後の DrawData を DX11 へ送信する。
    void ImGuiRenderDrawData() override;

    // Docking Window をメイン HWND 外へ出した時の追加 SwapChain を描画する。
    void ImGuiRenderPlatformWindows() override;

    // DX11RenderTarget が保持する SRV を ImGui 用の TextureID として返す。
    void* GetImTextureID(ResourceHandle<RenderTargetTag> rt, ResourceManager& resources, int slot = 0) override;

    // DX11Texture が保持する SRV を ImGui 用の TextureID として返す。
    void* GetImTextureID(ResourceHandle<TextureTag> texture, ResourceManager& resources) override;

private:
    ID3D11Device*        m_device  = nullptr;
    ID3D11DeviceContext* m_context = nullptr;
    bool                 m_imguiInitialized = false;
};

} // namespace fbzz::renderer
