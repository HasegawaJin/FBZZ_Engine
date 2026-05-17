// FBZZ Engine
// DX11RenderTarget.hpp | fbzz::renderer
// DX11 オフスクリーン描画ターゲット (Render-To-Texture)
//
// 設計方針:
//   IRenderTarget を継承し、ポストプロセス・シャドウマップ等の RTT パターンを抽象化する。
//   カラーバッファは D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE の両フラグで
//   生成し、描画先としても SRV としても使用できる "Read-Back テクスチャ" にする。
//
//   GetColorTexture() は ITexture インターフェース越しに SRV を渡すため、
//   上位レイヤーは DX11 の詳細を知らずに DrawCall::textures[] にセットできる。
#pragma once

#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>
#include <memory>
#include <engine/Renderer/IRenderTarget.hpp>

namespace fbzz::renderer
{

class DX11RenderTarget : public IRenderTarget
{
public:
    bool Init(ID3D11Device* device, uint32_t width, uint32_t height);

    uint32_t GetWidth()  const override { return m_width; }
    uint32_t GetHeight() const override { return m_height; }

    // カラーバッファを ITexture として取得する (ポストプロセス等で SRV としてバインドする)
    std::shared_ptr<ITexture> GetColorTexture() const override;

    // DX11Renderer::SetRenderTarget() が RTV をバインドする際に使用する
    ID3D11RenderTargetView* GetRTV() const { return m_rtv.Get(); }

private:
    Microsoft::WRL::ComPtr<ID3D11Texture2D>          m_colorBuffer;  // RTV + SRV 兼用テクスチャ
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView>   m_rtv;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_srv;

    // ITexture ラッパー: m_srv を ITexture::GetSRV() として公開するための薄い実装
    std::shared_ptr<ITexture>                        m_colorTexture;

    uint32_t m_width  = 0;
    uint32_t m_height = 0;
};

} // namespace fbzz::renderer
