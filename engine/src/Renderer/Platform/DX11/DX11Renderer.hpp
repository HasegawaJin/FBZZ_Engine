// FBZZ Engine
// DX11Renderer.hpp | fbzz::renderer
// IRenderer の DX11 実装 — デバイス・スワップチェーン・フレーム管理
//
// 設計方針:
//   上位レイヤー (Application / Sandbox) は IRenderer& のみを参照し、
//   このクラスに直接アクセスしない。依存方向: sandbox → engine (IRenderer) → DX11Renderer。
//   Application.cpp のみが DX11Renderer を make_unique して IRenderer に格納する
//   ファクトリー役を担い、それ以外の場所では DX11Renderer を知る必要がない。
//
//   サンプラー:
//     Init() 時に 3 種のサンプラー (WRAP_LINEAR / WRAP_POINT / CLAMP_LINEAR) を
//     事前生成し、SetSampler() で要求に応じてバインドする。
//     毎フレーム生成/破棄するのではなくキャッシュ方式を採用する。
#pragma once

#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>
#include <memory>
#include <engine/Renderer/IRenderer.hpp>
#include "DX11Buffer.hpp"

namespace fbzz::renderer
{

class DX11Renderer : public IRenderer
{
public:
    // Win32 ウィンドウハンドルと初期解像度を受け取ってデバイス・スワップチェーンを構築する
    bool Init(HWND hwnd, std::uint32_t width, std::uint32_t height);

    // ClearState() でパイプラインをリセットしてから ComPtr が自動解放する
    void Shutdown();

    // OM に RTV + DSV をバインドし直す (SetRenderTarget() 後のフレーム先頭で呼ぶ)
    void BeginFrame() override;

    // スワップチェーンを Present して画面に反映する (VSyncあり: interval=1)
    void EndFrame() override;

    // RTV と DSV を指定色でクリアする
    void Clear(const math::Vector4& color) override;

    // リソース生成 (GPU バッファ / シェーダー / テクスチャ / PSO)
    std::shared_ptr<IBuffer>         CreateVertexBuffer(const void* data, size_t sizeBytes, uint32_t stride) override;
    std::shared_ptr<IBuffer>         CreateIndexBuffer(const void* data, uint32_t count) override;
    std::shared_ptr<IConstantBuffer> CreateConstantBuffer(size_t sizeBytes) override;
    std::shared_ptr<IShader>         CreateShader(const std::string& path) override;
    std::shared_ptr<ITexture>        CreateTexture(const std::string& path) override;
    std::shared_ptr<IPipelineState>  CreatePipelineState(const PipelineStateDesc& desc) override;
    std::shared_ptr<IRenderTarget>   CreateRenderTarget(uint32_t width, uint32_t height) override;

    // DrawCall を受け取り、パイプラインステート → シェーダー → リソース → Draw の順で実行する
    void Submit(const DrawCall& call) override;

    // ウィンドウリサイズ時にスワップチェーン・RTV・DSV を再構築する
    void Resize(uint32_t width, uint32_t height) override;

    // オフスクリーン RT に切り替える (nullptr でバックバッファに戻す)
    void SetRenderTarget(std::shared_ptr<IRenderTarget> rt) override;

    // スロット番号に対応するサンプラープリセットをバインドする
    void SetSampler(uint32_t slot, SamplerMode mode) override;

    // DX11Buffer 等の DX11 サブシステムが Init 時にデバイスを必要とする場合に使用
    ID3D11Device*        GetDevice()       const { return m_device.Get(); }
    ID3D11DeviceContext* GetDeviceContext() const { return m_context.Get(); }

private:
    Microsoft::WRL::ComPtr<ID3D11Device>           m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext>    m_context;
    Microsoft::WRL::ComPtr<IDXGISwapChain>         m_swapChain;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_renderTargetView;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> m_depthStencilView;
    Microsoft::WRL::ComPtr<ID3D11Texture2D>        m_depthStencilBuffer;

    // SamplerMode (WRAP_LINEAR=0, WRAP_POINT=1, CLAMP_LINEAR=2) に対応するプリセット
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_samplers[3];

    uint32_t m_width  = 0;
    uint32_t m_height = 0;

    // --- Init 内部ヘルパー ---
    bool CreateRenderTargetView();
    bool CreateDepthStencilView();
    void InitSamplers();
};

} // namespace fbzz::renderer
