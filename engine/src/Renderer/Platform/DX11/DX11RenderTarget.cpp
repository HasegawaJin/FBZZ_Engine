// FBZZ Engine
// DX11RenderTarget.cpp | fbzz::renderer
// DX11 オフスクリーン描画ターゲット (Render-To-Texture)
#include "DX11RenderTarget.hpp"
#include "DX11Texture.hpp"
#include <engine/Core/Logger.hpp>
#include <engine/Core/HResult.hpp>

namespace fbzz::renderer
{

bool DX11RenderTarget::Init(ID3D11Device* device, uint32_t width, uint32_t height)
{
    m_width  = width;
    m_height = height;

    // -------------------------------------------------------------------------
    // カラーバッファ用テクスチャ
    //   D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE を同時指定することで
    //   「描画先として使いながら、別パスでサンプリングもできる」テクスチャになる。
    //   フォーマット R8G8B8A8_UNORM はスワップチェーンと合わせて一般的な選択。
    // -------------------------------------------------------------------------
    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width            = width;
    texDesc.Height           = height;
    texDesc.MipLevels        = 1;      // オフスクリーン RT はミップ不要
    texDesc.ArraySize        = 1;
    texDesc.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;      // MSAA なし (将来対応の余地あり)
    texDesc.Usage            = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags        = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    FBZZ_HR_CHECK(device->CreateTexture2D(&texDesc, nullptr, m_colorBuffer.GetAddressOf()));

    // RTV: このテクスチャへ描画するために OM ステージにバインドする
    FBZZ_HR_CHECK(device->CreateRenderTargetView(m_colorBuffer.Get(), nullptr, m_rtv.GetAddressOf()));

    // SRV: 後段パスでシェーダーからサンプリングするために PS ステージにバインドする
    FBZZ_HR_CHECK(device->CreateShaderResourceView(m_colorBuffer.Get(), nullptr, m_srv.GetAddressOf()));

    // -------------------------------------------------------------------------
    // ITexture ラッパー (RTTexture)
    //   カラーバッファの SRV を ITexture として外部に渡すための最小実装クラス。
    //   DX11Texture を継承せず ITexture を直接実装しているのは、
    //   DX11Texture の Init() を経由せずに SRV を直接差し込みたいため。
    //   このクラスは Init() のスコープ内に定義するが、make_shared で寿命を延ばすため
    //   Init() 終了後も m_colorTexture 経由で生存する。
    // -------------------------------------------------------------------------
    class RTTexture : public ITexture
    {
    public:
        RTTexture(ID3D11ShaderResourceView* srv, uint32_t w, uint32_t h)
            : m_srv(srv), m_width(w), m_height(h) {}
        uint32_t GetWidth()  const override { return m_width; }
        uint32_t GetHeight() const override { return m_height; }
        ID3D11ShaderResourceView* GetSRV() const { return m_srv; }
    private:
        ID3D11ShaderResourceView* m_srv;  // 非所有の参照 (寿命は m_srv ComPtr が管理)
        uint32_t m_width, m_height;
    };

    m_colorTexture = std::make_shared<RTTexture>(m_srv.Get(), width, height);

    return true;
}

std::shared_ptr<ITexture> DX11RenderTarget::GetColorTexture() const
{
    return m_colorTexture;
}

} // namespace fbzz::renderer
