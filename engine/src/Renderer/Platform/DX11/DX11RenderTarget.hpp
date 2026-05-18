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
#include "DX11Texture.hpp"

namespace fbzz::renderer
{

class DX11RenderTarget : public IRenderTarget
{
public:
    // colorCount: 同時出力カラーバッファ数 (最大 MAX_COLOR)
    // 全スロット RGBA16_FLOAT で生成する (符号付き法線ベクトルも収容できる精度)
    bool Init(ID3D11Device* device, uint32_t width, uint32_t height, uint32_t colorCount = 1);

    uint32_t GetWidth()      const override { return m_width; }
    uint32_t GetHeight()     const override { return m_height; }
    uint32_t GetColorCount() const override { return m_colorCount; }

    // index 枚目のカラーバッファを DX11Texture として返す (次パスで DrawCall::textures[] にセット)
    std::shared_ptr<ITexture> GetColorTexture(uint32_t index = 0) const override;

    // DX11Renderer::SetRenderTarget() が OMSetRenderTargets に渡す RTV 配列を取得する
    void GetRTVs(ID3D11RenderTargetView** out, uint32_t& count) const;

    static constexpr uint32_t MAX_COLOR = 8;  // DX11 の MRT 上限

private:

    Microsoft::WRL::ComPtr<ID3D11Texture2D>          m_colorBuffer[MAX_COLOR];
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView>   m_rtv[MAX_COLOR];
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_srv[MAX_COLOR];
    std::shared_ptr<ITexture>                        m_colorTexture[MAX_COLOR];

    uint32_t m_colorCount = 0;
    uint32_t m_width      = 0;
    uint32_t m_height     = 0;
};

} // namespace fbzz::renderer
