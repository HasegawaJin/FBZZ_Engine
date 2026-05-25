// FBZZ Engine
// DX11RenderTarget.hpp | fbzz::renderer
// DX11 オフスクリーン描画ターゲット実装
// IRenderTarget を継承し、RTV / DSV / SRV の組を管理する。
// バックバッファ以外の描画先を Renderer 抽象から扱えるようにする。
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
#include <Engine/Renderer/IRenderTarget.hpp>
#include "DX11Texture.hpp"

namespace fbzz::renderer
{

class DX11RenderTarget : public IRenderTarget
{
public:
    // colorCount: 同時出力カラーバッファ数 (0 = 深度専用, 最大 MAX_COLOR)
    // 全スロット RGBA16_FLOAT で生成する (符号付き法線ベクトルも収容できる精度)
    bool Init(ID3D11Device* device, uint32_t width, uint32_t height, uint32_t colorCount = 1);

    uint32_t GetWidth()      const override { return m_width; }
    uint32_t GetHeight()     const override { return m_height; }
    uint32_t GetColorCount() const override { return m_colorCount; }

    // index 枚目のカラーバッファを DX11Texture として返す (次パスで DrawCall::textures[] にセット)
    std::shared_ptr<ITexture> GetColorTexture(uint32_t index = 0) const override;

    // 深度バッファを SRV として返す (シャドウマップ等、次パスで t8 にセット)
    std::shared_ptr<ITexture> GetDepthTexture() const override;

    // DX11Renderer::SetRenderTarget() が OMSetRenderTargets に渡す RTV 配列を取得する
    void GetRTVs(ID3D11RenderTargetView** out, uint32_t& count) const;

    // DX11Renderer が OMSetRenderTargets に渡す DSV を取得する (DX11 内部用)
    ID3D11DepthStencilView* GetDSV() const { return m_dsv.Get(); }

    // ImGui Viewport 用 SRV ポインタ (IRenderTarget 経由で IRenderer が取得する)
    void* GetNativeSRV(int slot = 0) const override { return m_srv[slot].Get(); }

    static constexpr uint32_t MAX_COLOR = 8;  // DX11 の MRT 上限

private:

    Microsoft::WRL::ComPtr<ID3D11Texture2D>          m_colorBuffer[MAX_COLOR];
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView>   m_rtv[MAX_COLOR];
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_srv[MAX_COLOR];
    std::shared_ptr<ITexture>                        m_colorTexture[MAX_COLOR];

    // 深度バッファ (全 RT で生成。colorCount=0 の場合はシャドウマップ専用)
    Microsoft::WRL::ComPtr<ID3D11Texture2D>          m_depthBuffer;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView>   m_dsv;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_depthSRV;
    std::shared_ptr<ITexture>                        m_depthTexture;

    uint32_t m_colorCount = 0;
    uint32_t m_width      = 0;
    uint32_t m_height     = 0;
};

} // namespace fbzz::renderer
