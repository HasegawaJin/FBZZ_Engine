// FBZZ Engine
// DX11Renderer.cpp | fbzz::renderer
// IRenderer の DX11 実装 — デバイス・スワップチェーン・フレーム管理
//
// d3d11.lib / dxgi.lib はプラグマリンクで解決する。
// CMakeLists で target_link_libraries に追加してもよいが、
// DX11 依存を実装ファイルに閉じ込めるためここで宣言している。
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

#include "DX11Renderer.hpp"
#include "DX11Buffer.hpp"
#include "DX11ConstantBuffer.hpp"
#include "DX11Shader.hpp"
#include "DX11PipelineState.hpp"
#include "DX11Texture.hpp"
#include "DX11RenderTarget.hpp"
#include <engine/Core/Logger.hpp>
#include <engine/Core/HResult.hpp>

namespace fbzz::renderer
{

// =============================================================================
// Init / Shutdown
// =============================================================================

bool DX11Renderer::Init(HWND hwnd, uint32_t width, uint32_t height)
{
    m_width  = width;
    m_height = height;

    // -------------------------------------------------------------------------
    // スワップチェーン設定
    //   BufferCount=1 は DX11 の FLIP_DISCARD 非対応環境向けの古典的な単バッファ構成。
    //   DXGI_SWAP_EFFECT_DISCARD との組み合わせが必須。
    //   DX12 移行時は BufferCount=2 + FLIP_DISCARD に切り替える。
    // -------------------------------------------------------------------------
    DXGI_SWAP_CHAIN_DESC scDesc                        = {};
    scDesc.BufferCount                                 = 1;
    scDesc.BufferDesc.Width                            = width;
    scDesc.BufferDesc.Height                           = height;
    scDesc.BufferDesc.Format                           = DXGI_FORMAT_R8G8B8A8_UNORM;
    scDesc.BufferDesc.RefreshRate.Numerator            = 60;
    scDesc.BufferDesc.RefreshRate.Denominator          = 1;
    scDesc.BufferUsage                                 = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scDesc.OutputWindow                                = hwnd;
    scDesc.SampleDesc.Count                            = 1;  // MSAA なし
    scDesc.Windowed                                    = TRUE;
    scDesc.SwapEffect                                  = DXGI_SWAP_EFFECT_DISCARD;

    // DEBUG ビルドではデバッグレイヤーを有効化し、DX11 の検証エラーを OutputDebugString に出力する
    UINT flags = 0;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    // D3D_FEATURE_LEVEL_11_0 を明示して、それ未満の GPU でエラーを即座に返す
    D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
    FBZZ_HR_CHECK(D3D11CreateDeviceAndSwapChain(
        nullptr,                        // デフォルトアダプタ
        D3D_DRIVER_TYPE_HARDWARE,       // GPU レンダリング (WARP は使わない)
        nullptr,
        flags,
        &featureLevel, 1,
        D3D11_SDK_VERSION,
        &scDesc,
        m_swapChain.GetAddressOf(),
        m_device.GetAddressOf(),
        nullptr,
        m_context.GetAddressOf()));

    if (!CreateRenderTargetView())  return false;
    if (!CreateDepthStencilView())  return false;

    // OM ステージに RTV と DSV を一括バインド
    m_context->OMSetRenderTargets(1, m_renderTargetView.GetAddressOf(), m_depthStencilView.Get());

    // ビューポートをウィンドウ全体に設定 (左上原点、Z: 0.0〜1.0)
    D3D11_VIEWPORT vp = {};
    vp.Width          = static_cast<float>(width);
    vp.Height         = static_cast<float>(height);
    vp.MinDepth       = 0.0f;
    vp.MaxDepth       = 1.0f;
    m_context->RSSetViewports(1, &vp);

    InitSamplers();

    FBZZ_LOG_INFO("DX11Renderer 初期化完了: %ux%u", width, height);
    return true;
}

void DX11Renderer::Shutdown()
{
    // ClearState() でパイプラインの全バインドを解除してから ComPtr に解放させる。
    // 解放順序は依存関係の逆順: Context → SwapChain → Device。
    // ComPtr のデストラクタが自動でこの順序を保証するため、明示的な Release() は不要。
    m_context->ClearState();
    FBZZ_LOG_INFO("DX11Renderer シャットダウン");
}

// =============================================================================
// フレーム制御
// =============================================================================

void DX11Renderer::BeginFrame()
{
    // SetRenderTarget() でオフスクリーン RT に切り替えた後、
    // バックバッファに戻すためにフレーム先頭で必ず呼ぶ。
    m_context->OMSetRenderTargets(1, m_renderTargetView.GetAddressOf(), m_depthStencilView.Get());
}

void DX11Renderer::EndFrame()
{
    // interval=1: リフレッシュレートに同期して Present する (VSync ON)
    m_swapChain->Present(1, 0);
}

void DX11Renderer::Clear(const math::Vector4& color)
{
    float c[4] = { color.x, color.y, color.z, color.w };
    m_context->ClearRenderTargetView(m_renderTargetView.Get(), c);
    // D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL を同時クリア
    m_context->ClearDepthStencilView(m_depthStencilView.Get(),
        D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
}

// =============================================================================
// リソース生成
// =============================================================================

std::shared_ptr<IBuffer> DX11Renderer::CreateVertexBuffer(const void* data, size_t sizeBytes, uint32_t stride)
{
    auto buf = std::make_shared<DX11Buffer>();
    if (!buf->Init(m_device.Get(), m_context.Get(), data, sizeBytes, stride, D3D11_BIND_VERTEX_BUFFER))
        return nullptr;
    return buf;
}

std::shared_ptr<IBuffer> DX11Renderer::CreateIndexBuffer(const void* data, uint32_t count)
{
    // インデックスは uint32_t 固定 (DXGI_FORMAT_R32_UINT)。
    // uint16_t (65536 頂点未満) のほうがメモリ効率は良いが、
    // 複雑なメッシュに備えて 32bit を標準とする。
    size_t sizeBytes = count * sizeof(uint32_t);
    auto buf = std::make_shared<DX11Buffer>();
    if (!buf->Init(m_device.Get(), m_context.Get(), data, sizeBytes, 0, D3D11_BIND_INDEX_BUFFER))
        return nullptr;
    return buf;
}

std::shared_ptr<IConstantBuffer> DX11Renderer::CreateConstantBuffer(size_t sizeBytes)
{
    auto cb = std::make_shared<DX11ConstantBuffer>();
    if (!cb->Init(m_device.Get(), m_context.Get(), sizeBytes))
        return nullptr;
    return cb;
}

std::shared_ptr<IShader> DX11Renderer::CreateShader(const std::string& path)
{
    auto shader = std::make_shared<DX11Shader>();
    if (!shader->Init(m_device.Get(), path))
        return nullptr;
    return shader;
}

std::shared_ptr<ITexture> DX11Renderer::CreateTexture(const std::string& path)
{
    auto tex = std::make_shared<DX11Texture>();
    if (!tex->Init(m_device.Get(), m_context.Get(), path))
        return nullptr;
    return tex;
}

std::shared_ptr<IPipelineState> DX11Renderer::CreatePipelineState(const PipelineStateDesc& desc)
{
    auto pso = std::make_shared<DX11PipelineState>();
    if (!pso->Init(m_device.Get(), desc))
        return nullptr;
    return pso;
}

std::shared_ptr<IRenderTarget> DX11Renderer::CreateRenderTarget(uint32_t width, uint32_t height)
{
    auto rt = std::make_shared<DX11RenderTarget>();
    if (!rt->Init(m_device.Get(), width, height))
        return nullptr;
    return rt;
}

// =============================================================================
// Submit — DrawCall の内容に従ってパイプラインを構築して Draw を発行する
// =============================================================================

void DX11Renderer::Submit(const DrawCall& call)
{
    // ---- 1. Pipeline State (RS / OM ステート) --------------------------------
    // PipelineState は毎フレーム Apply するが、DX11 ドライバが重複バインドを検出して
    // 実際の状態変更がない場合はステートチェンジコストをスキップする。
    if (call.pipelineState)
        static_cast<DX11PipelineState*>(call.pipelineState.get())->Apply(m_context.Get());

    // ---- 2. Shader + InputLayout (VS / PS / IA) --------------------------------
    if (call.shader)
        static_cast<DX11Shader*>(call.shader.get())->Bind(m_context.Get());

    // ---- 3. Constant Buffers (VS・PS 両方の同スロットへバインド) ----------------
    // 同じ定数バッファを VS と PS の両方にバインドすることで、
    // シェーダーの種類ごとにスロットを分けずに済む。
    for (uint32_t i = 0; i < static_cast<uint32_t>(call.constantBuffers.size()); ++i)
    {
        if (!call.constantBuffers[i]) continue;
        ID3D11Buffer* buf = static_cast<DX11ConstantBuffer*>(call.constantBuffers[i].get())->GetBuffer();
        m_context->VSSetConstantBuffers(i, 1, &buf);
        m_context->PSSetConstantBuffers(i, 1, &buf);
    }

    // ---- 4. Textures (SRV → PS ステージ) ----------------------------------------
    for (uint32_t i = 0; i < static_cast<uint32_t>(call.textures.size()); ++i)
    {
        if (!call.textures[i]) continue;
        ID3D11ShaderResourceView* srv = static_cast<DX11Texture*>(call.textures[i].get())->GetSRV();
        m_context->PSSetShaderResources(i, 1, &srv);
    }

    // ---- 5. Primitive Topology (IA ステージ) --------------------------------------
    m_context->IASetPrimitiveTopology(
        call.topology == PrimitiveTopology::LINE_LIST
            ? D3D11_PRIMITIVE_TOPOLOGY_LINELIST
            : D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // ---- 6. Vertex Buffer (IA ステージ) ------------------------------------------
    if (call.vertexBuffer)
    {
        auto*         dx11vb = static_cast<DX11Buffer*>(call.vertexBuffer.get());
        ID3D11Buffer* vb     = dx11vb->GetBuffer();
        UINT          stride = dx11vb->GetStride();
        UINT          offset = 0;
        m_context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    }

    // ---- 7. Draw (インデックスあり / なしで分岐) -----------------------------------
    if (call.indexBuffer)
    {
        // DXGI_FORMAT_R32_UINT: インデックスは uint32_t 固定
        ID3D11Buffer* ib = static_cast<DX11Buffer*>(call.indexBuffer.get())->GetBuffer();
        m_context->IASetIndexBuffer(ib, DXGI_FORMAT_R32_UINT, 0);
        m_context->DrawIndexed(call.indexCount, call.startIndex, static_cast<INT>(call.baseVertex));
    }
    else
    {
        m_context->Draw(call.vertexCount, 0);
    }
}

// =============================================================================
// Resize — ウィンドウリサイズ時にスワップチェーン・RTV・DSV を再構築する
// =============================================================================

void DX11Renderer::Resize(uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0) return;

    m_width  = width;
    m_height = height;

    // ResizeBuffers を呼ぶ前に RTV・DSV を OM から外し、COM 参照を全て解放する必要がある。
    // 参照が残ったまま ResizeBuffers を呼ぶと DXGI_ERROR_INVALID_CALL が返る。
    m_context->OMSetRenderTargets(0, nullptr, nullptr);
    m_renderTargetView.Reset();
    m_depthStencilView.Reset();
    m_depthStencilBuffer.Reset();

    HRESULT hr = m_swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) { FBZZ_LOG_ERROR("ResizeBuffers 失敗: 0x%08X", (unsigned)hr); return; }

    // 新しいサイズで RTV・DSV を再生成して OM に再バインドする
    if (!CreateRenderTargetView()) return;
    if (!CreateDepthStencilView()) return;

    m_context->OMSetRenderTargets(1, m_renderTargetView.GetAddressOf(), m_depthStencilView.Get());

    D3D11_VIEWPORT vp = {};
    vp.Width          = static_cast<float>(width);
    vp.Height         = static_cast<float>(height);
    vp.MinDepth       = 0.0f;
    vp.MaxDepth       = 1.0f;
    m_context->RSSetViewports(1, &vp);
}

// =============================================================================
// SetRenderTarget / SetSampler
// =============================================================================

void DX11Renderer::SetRenderTarget(std::shared_ptr<IRenderTarget> rt)
{
    if (!rt)
    {
        // nullptr を渡すとバックバッファ (デフォルト RTV) に戻す
        m_context->OMSetRenderTargets(1, m_renderTargetView.GetAddressOf(), m_depthStencilView.Get());
        return;
    }
    // DX11RenderTarget にダウンキャストして RTV を取り出す。
    // IRenderer 経由で渡されるのは必ず CreateRenderTarget() で生成した DX11RenderTarget なので
    // static_cast は安全。
    ID3D11RenderTargetView* rtv = static_cast<DX11RenderTarget*>(rt.get())->GetRTV();
    m_context->OMSetRenderTargets(1, &rtv, m_depthStencilView.Get());
}

void DX11Renderer::SetSampler(uint32_t slot, SamplerMode mode)
{
    // m_samplers のインデックスは SamplerMode の列挙値と一致させている (InitSamplers 参照)
    uint32_t idx = static_cast<uint32_t>(mode);
    m_context->PSSetSamplers(slot, 1, m_samplers[idx].GetAddressOf());
}

// =============================================================================
// Private helpers
// =============================================================================

bool DX11Renderer::CreateRenderTargetView()
{
    // スワップチェーンのバックバッファを取得し、そこへの RTV を生成する。
    // Resize() 後は必ずこの関数を再呼び出しして新しいバックバッファに RTV を張り直す。
    Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
    FBZZ_HR_CHECK(m_swapChain->GetBuffer(0, IID_PPV_ARGS(backBuffer.GetAddressOf())));
    FBZZ_HR_CHECK(m_device->CreateRenderTargetView(
        backBuffer.Get(), nullptr, m_renderTargetView.GetAddressOf()));
    return true;
}

bool DX11Renderer::CreateDepthStencilView()
{
    // 深度バッファフォーマット DXGI_FORMAT_D24_UNORM_S8_UINT:
    //   24bit 深度 (精度と互換性のバランスが良い) + 8bit ステンシル。
    //   DXGI_FORMAT_D32_FLOAT はより高精度だが、ステンシルを使いたい場合は D24S8 が標準。
    D3D11_TEXTURE2D_DESC depthDesc   = {};
    depthDesc.Width                  = m_width;
    depthDesc.Height                 = m_height;
    depthDesc.MipLevels              = 1;
    depthDesc.ArraySize              = 1;
    depthDesc.Format                 = DXGI_FORMAT_D24_UNORM_S8_UINT;
    depthDesc.SampleDesc.Count       = 1;
    depthDesc.Usage                  = D3D11_USAGE_DEFAULT;
    depthDesc.BindFlags              = D3D11_BIND_DEPTH_STENCIL;

    FBZZ_HR_CHECK(m_device->CreateTexture2D(
        &depthDesc, nullptr, m_depthStencilBuffer.GetAddressOf()));
    FBZZ_HR_CHECK(m_device->CreateDepthStencilView(
        m_depthStencilBuffer.Get(), nullptr, m_depthStencilView.GetAddressOf()));
    return true;
}

void DX11Renderer::InitSamplers()
{
    // 3 種のサンプラープリセットを Init 時に一括生成してキャッシュする。
    // SetSampler() で SamplerMode のインデックスを使って参照する。
    //
    //   [0] WRAP_LINEAR  : テクスチャ繰り返し + 線形補間 (地形・壁面タイル等の標準)
    //   [1] WRAP_POINT   : テクスチャ繰り返し + 最近傍 (ピクセルアート・UI 等)
    //   [2] CLAMP_LINEAR : UV を [0,1] にクランプ + 線形補間 (スカイボックス・GBuffer 等)
    auto createSampler = [&](D3D11_FILTER filter, D3D11_TEXTURE_ADDRESS_MODE address,
                             Microsoft::WRL::ComPtr<ID3D11SamplerState>& out)
    {
        D3D11_SAMPLER_DESC desc = {};
        desc.Filter             = filter;
        desc.AddressU           = address;
        desc.AddressV           = address;
        desc.AddressW           = address;
        desc.ComparisonFunc     = D3D11_COMPARISON_NEVER;
        desc.MinLOD             = 0.0f;
        desc.MaxLOD             = D3D11_FLOAT32_MAX;  // 全ミップレベルを使用
        // If using anisotropic filtering, set a high MaxAnisotropy
        if (filter == D3D11_FILTER_ANISOTROPIC) {
            desc.MaxAnisotropy = 16; // maximum quality on most hardware
        } else {
            desc.MaxAnisotropy = 1;
        }
        m_device->CreateSamplerState(&desc, out.GetAddressOf());
    };

    // Use anisotropic filtering (best quality) for linear samplers
    createSampler(D3D11_FILTER_ANISOTROPIC, D3D11_TEXTURE_ADDRESS_WRAP,  m_samplers[0]);
    createSampler(D3D11_FILTER_MIN_MAG_MIP_POINT,  D3D11_TEXTURE_ADDRESS_WRAP,  m_samplers[1]);
    createSampler(D3D11_FILTER_ANISOTROPIC, D3D11_TEXTURE_ADDRESS_CLAMP, m_samplers[2]);
}

} // namespace fbzz::renderer
