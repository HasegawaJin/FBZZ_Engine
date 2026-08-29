/// @file    DX11RenderTarget.hpp
/// @brief   DX11 オフスクリーン描画ターゲット実装。
/// @author  Hasegawa Jin
/// @date    2026-05-21
///
/// IRenderTarget を継承し、RTV / DSV / SRV の組を管理する。
/// バックバッファ以外の描画先を Renderer 抽象から扱えるようにする。
///
/// 設計方針:
/// IRenderTarget を継承し、ポストプロセス・シャドウマップ等の RTT パターンを抽象化する。
/// カラーバッファは D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE の両フラグで
/// 生成し、描画先としても SRV としても使用できる "Read-Back テクスチャ" にする。
///
/// ResourceManager は DX11Renderer 経由で SRV を ITexture ラッパー化し、
/// 上位レイヤーへは ResourceHandle<TextureTag> として公開する。
#pragma once

#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>
#include <vector>
#include <Engine/Renderer/IRenderTarget.hpp>

namespace fbzz::renderer
{

class DX11RenderTarget : public IRenderTarget
{
public:
    // colorCount: 同時出力カラーバッファ数 (0 = 深度専用, 最大 MAX_COLOR)
    // 全スロット RGBA16_FLOAT で生成する (符号付き法線ベクトルも収容できる精度)
    bool Init(ID3D11Device* device, uint32_t width, uint32_t height, uint32_t colorCount = 1);

    // InitCubemap — 6 面キューブマップを描画先として生成する (空連動 IBL の SkyCapture 用)。
    // WHY: 空を実行時にキューブマップへ焼くには「面ごとに RTV を持つ描画先」が要る。
    //      面×mip ごとの RTV と、サンプリング用 TextureCube SRV を生成する。深度は持たない
    //      (空ドームは深度不要のため。OM には face RTV + null DSV をバインドする)。
    // size: 1 辺のピクセル数 / mipCount: 1 で mip0 のみ。>1 で GenerateMips 可能にする。
    // format: 既定は HDR の R16G16B16A16_FLOAT (IBL ベイク経路と一致)。
    bool InitCubemap(ID3D11Device* device, uint32_t size, uint32_t mipCount = 1,
                     DXGI_FORMAT format = DXGI_FORMAT_R16G16B16A16_FLOAT);

    uint32_t GetWidth()      const override { return m_width; }
    uint32_t GetHeight()     const override { return m_height; }
    uint32_t GetColorCount() const override { return m_colorCount; }

    // DX11Renderer::SetRenderTarget() が OMSetRenderTargets に渡す RTV 配列を取得する
    void GetRTVs(ID3D11RenderTargetView** out, uint32_t& count) const;

    // DX11Renderer が OMSetRenderTargets に渡す DSV を取得する (DX11 内部用)
    ID3D11DepthStencilView* GetDSV() const { return m_dsv.Get(); }

    // ResourceManager が TextureTag の実体を作るために SRV を借りる。
    // WHY: RenderTarget 自身に ITexture の shared_ptr を持たせず、所有を ResourcePool に集約するため。
    ID3D11ShaderResourceView* GetColorSRV(uint32_t index) const;
    ID3D11ShaderResourceView* GetDepthSRV() const { return m_depthSRV.Get(); }

    // ── キューブマップ (InitCubemap で生成した場合のみ有効) ───────────────────
    bool                      IsCubemap()    const { return m_isCubemap; }
    uint32_t                  GetMipCount()  const { return m_cubeMipCount; }
    // 指定 face(0..5)・mip の描画先 RTV。SetRenderTargetFace が OM にバインドする。
    ID3D11RenderTargetView*   GetFaceRTV(uint32_t face, uint32_t mip) const;
    // 全 mip を含む TextureCube SRV (各 Lit パスが束縛する)。
    ID3D11ShaderResourceView* GetCubeSRV() const { return m_cubeSRV.Get(); }
    // mip>0 を含む場合に mip0 から残り mip を生成するための SRV (GenerateMips 用)。
    void GenerateCubeMips(ID3D11DeviceContext* ctx) const { if (m_cubeSRV) ctx->GenerateMips(m_cubeSRV.Get()); }

    // ImGui Viewport 用 SRV ポインタ (IRenderTarget 経由で IRenderer が取得する)
    void* GetNativeSRV(int slot = 0) const override {
        return m_isCubemap ? static_cast<void*>(m_cubeSRV.Get()) : static_cast<void*>(m_srv[slot].Get());
    }

    static constexpr uint32_t MAX_COLOR = 8;  // DX11 の MRT 上限

private:

    Microsoft::WRL::ComPtr<ID3D11Texture2D>          m_colorBuffer[MAX_COLOR];
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView>   m_rtv[MAX_COLOR];
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_srv[MAX_COLOR];

    // 深度バッファ (全 RT で生成。colorCount=0 の場合はシャドウマップ専用)
    Microsoft::WRL::ComPtr<ID3D11Texture2D>          m_depthBuffer;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView>   m_dsv;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_depthSRV;

    // キューブマップ描画先 (InitCubemap)
    Microsoft::WRL::ComPtr<ID3D11Texture2D>          m_cubeTex;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_cubeSRV;        // TextureCube (全 mip)
    // face×mip の RTV。インデックス = face * m_cubeMipCount + mip。
    std::vector<Microsoft::WRL::ComPtr<ID3D11RenderTargetView>> m_cubeFaceRTV;
    bool     m_isCubemap   = false;
    uint32_t m_cubeMipCount = 0;

    uint32_t m_colorCount = 0;
    uint32_t m_width      = 0;
    uint32_t m_height     = 0;
};

} // namespace fbzz::renderer
