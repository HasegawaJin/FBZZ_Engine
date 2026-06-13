// FBZZ Engine
// DX11Renderer.cpp | fbzz::renderer
// IRenderer の DX11 実装
// デバイス・スワップチェーン・バックバッファ・フレーム送信を管理する。
// 上位レイヤーには IRenderer と ResourceManager の境界だけを見せる。
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
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Core/HResult.hpp>
#include <string>

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
    //   DXGI の blt-model(DISCARD/SEQUENTIAL) は現在ではレガシー扱い。
    //   WHY: flip-model は DWM との合成経路が現代的で、デバッグレイヤーの #294 警告も避けられる。
    //        FLIP_DISCARD は BufferCount >= 2 かつ MSAA 無効が前提なので、バックバッファを 2 枚にする。
    // -------------------------------------------------------------------------
    DXGI_SWAP_CHAIN_DESC scDesc                        = {};
    scDesc.BufferCount                                 = 2;
    scDesc.BufferDesc.Width                            = width;
    scDesc.BufferDesc.Height                           = height;
    scDesc.BufferDesc.Format                           = DXGI_FORMAT_R8G8B8A8_UNORM;
    scDesc.BufferDesc.RefreshRate.Numerator            = 0;
    scDesc.BufferDesc.RefreshRate.Denominator          = 1;
    scDesc.BufferUsage                                 = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scDesc.OutputWindow                                = hwnd;
    scDesc.SampleDesc.Count                            = 1;  // MSAA は無効
    scDesc.Windowed                                    = TRUE;
    scDesc.SwapEffect                                  = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    // DEBUG ビルドではデバッグレイヤーを有効化し、DX11 の検証エラーを OutputDebugString に出力する
    UINT flags = 0;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    // D3D_FEATURE_LEVEL_11_0 を明示して、それ未満の GPU でエラーを即座に返す
    D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
    FBZZ_HR_CHECK(D3D11CreateDeviceAndSwapChain(
        nullptr,                        // 既定アダプター
        D3D_DRIVER_TYPE_HARDWARE,       // GPU ドライバーを使用
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

    FBZZ_LOG_INFO("DX11Renderer initialized: %ux%u", width, height);
    return true;
}

void DX11Renderer::Shutdown()
{
    // ClearState() でパイプラインの全バインドを解除してから ComPtr に解放させる。
    // 解放順序は依存関係の逆順: Context → SwapChain → Device。
    // ComPtr のデストラクタが自動でこの順序を保証するため、明示的な Release() は不要。
    m_context->ClearState();
    FBZZ_LOG_INFO("DX11Renderer shutdown");
}

// =============================================================================
// フレーム制御
// =============================================================================

void DX11Renderer::BeginFrame()
{
    // 前フレームで残ったSRVバインドを解除してからRTをセットする
    static ID3D11ShaderResourceView* const kNullSRVs[16] = {};
    m_context->PSSetShaderResources(0, 16, kNullSRVs);
    m_context->CSSetShaderResources(0, 16, kNullSRVs);

    m_currentRT = nullptr;
    m_context->OMSetRenderTargets(1, m_renderTargetView.GetAddressOf(), m_depthStencilView.Get());
}

void DX11Renderer::EndFrame()
{
    // FPS limiting is handled by Time::targetFps.
    m_swapChain->Present(0, 0);
}

void DX11Renderer::Clear(const math::Vector4& color)
{
    float c[4] = { color.x, color.y, color.z, color.w };
    if (!m_currentRT)
    {
        m_context->ClearRenderTargetView(m_renderTargetView.Get(), c);
        m_context->ClearDepthStencilView(m_depthStencilView.Get(),
            D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
        return;
    }
    ID3D11RenderTargetView* rtvs[DX11RenderTarget::MAX_COLOR] = {};
    uint32_t count = 0;
    m_currentRT->GetRTVs(rtvs, count);
    for (uint32_t i = 0; i < count; ++i)
        m_context->ClearRenderTargetView(rtvs[i], c);
    if (auto* dsv = m_currentRT->GetDSV())
        m_context->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
}

void DX11Renderer::ClearDepth(float depth)
{
    auto* dsv = m_currentRT ? m_currentRT->GetDSV() : m_depthStencilView.Get();
    if (dsv)
        m_context->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH, depth, 0);
}

// =============================================================================
// リソース生成
// =============================================================================

std::unique_ptr<IBuffer> DX11Renderer::CreateNativeVertexBuffer(const void* data, size_t sizeBytes, uint32_t stride)
{
    auto buf = std::make_unique<DX11Buffer>();
    if (!buf->Init(m_device.Get(), m_context.Get(), data, sizeBytes, stride, D3D11_BIND_VERTEX_BUFFER))
        return nullptr;
    return buf;
}

std::unique_ptr<IBuffer> DX11Renderer::CreateNativeIndexBuffer(const void* data, uint32_t count)
{
    // インデックスは uint32_t 固定 (DXGI_FORMAT_R32_UINT)。
    // uint16_t (65536 頂点未満) のほうがメモリ効率は良いが、
    // 複雑なメッシュに備えて 32bit を標準とする。
    size_t sizeBytes = count * sizeof(uint32_t);
    auto buf = std::make_unique<DX11Buffer>();
    if (!buf->Init(m_device.Get(), m_context.Get(), data, sizeBytes, 0, D3D11_BIND_INDEX_BUFFER))
        return nullptr;
    return buf;
}

std::unique_ptr<IConstantBuffer> DX11Renderer::CreateNativeConstantBuffer(size_t sizeBytes)
{
    auto cb = std::make_unique<DX11ConstantBuffer>();
    if (!cb->Init(m_device.Get(), m_context.Get(), sizeBytes))
        return nullptr;
    return cb;
}

std::unique_ptr<IShader> DX11Renderer::CreateNativeShader(const std::string& path)
{
    auto shader = std::make_unique<DX11Shader>();
    if (!shader->Init(m_device.Get(), path))
        return nullptr;
    return shader;
}

std::unique_ptr<ITexture> DX11Renderer::CreateNativeTexture(const std::string& path)
{
    auto tex = std::make_unique<DX11Texture>();
    if (!tex->Init(m_device.Get(), m_context.Get(), path))
        return nullptr;
    return tex;
}

std::unique_ptr<ITexture> DX11Renderer::CreateNativeTextureFromData(const uint8_t* rgba, uint32_t width, uint32_t height)
{
    auto tex = std::make_unique<DX11Texture>();
    if (!tex->InitFromData(m_device.Get(), rgba, width, height))
        return nullptr;
    return tex;
}

std::unique_ptr<IPipelineState> DX11Renderer::CreateNativePipelineState(const PipelineStateDesc& desc)
{
    auto pso = std::make_unique<DX11PipelineState>();
    if (!pso->Init(m_device.Get(), desc))
        return nullptr;
    return pso;
}

std::unique_ptr<IRenderTarget> DX11Renderer::CreateNativeRenderTarget(uint32_t width, uint32_t height, uint32_t colorCount)
{
    auto rt = std::make_unique<DX11RenderTarget>();
    if (!rt->Init(m_device.Get(), width, height, colorCount))
        return nullptr;
    return rt;
}

std::unique_ptr<ITexture> DX11Renderer::CreateNativeComputeTexture(uint32_t width, uint32_t height)
{
    auto tex = std::make_unique<DX11Texture>();
    if (!tex->InitForCompute(m_device.Get(), width, height))
        return nullptr;
    return tex;
}

// =============================================================================
// Dispatch — ComputeCall に従って CS を実行する
// =============================================================================

void DX11Renderer::Dispatch(const ComputeCall& call, ResourceManager& resources)
{
    auto* shader = resources.Get(call.shader);
    if (!shader) return;

    // Compute を実行する前に OM の RTV/DSV をアンバインドする。
    // HDR RT などが RTV と CS-SRV に同時バインドされると HAZARD 警告が出るため。
    m_context->OMSetRenderTargets(0, nullptr, nullptr);
    m_currentRT = nullptr;

    // CS バインド
    auto* cs = static_cast<DX11Shader*>(shader);
    m_context->CSSetShader(cs->GetComputeShader(), nullptr, 0);

    // WHY: HLSL 側の Compute Shader は SAMPLER_DEFAULT(s0) を使うパスがある。
    //      DX11 は NULL Sampler でも既定動作にフォールバックするが、デバッグレイヤー警告を避けるため
    //      ポストプロセスで最も一般的な clamp + linear を Dispatch ごとに明示する。
    ID3D11SamplerState* defaultSampler = m_samplers[static_cast<uint32_t>(SamplerMode::CLAMP_LINEAR)].Get();
    m_context->CSSetSamplers(0, 1, &defaultSampler);

    // 定数バッファ (CS ステージ)
    for (uint32_t i = 0; i < static_cast<uint32_t>(call.constantBuffers.size()); ++i)
    {
        auto* cb = resources.Get(call.constantBuffers[i]);
        if (!cb) continue;
        ID3D11Buffer* buf = static_cast<DX11ConstantBuffer*>(cb)->GetBuffer();
        m_context->CSSetConstantBuffers(i, 1, &buf);
    }

    // SRV 入力 (CS ステージ)
    for (uint32_t i = 0; i < static_cast<uint32_t>(call.srvInputs.size()); ++i)
    {
        auto* texture = resources.Get(call.srvInputs[i]);
        if (!texture) continue;
        ID3D11ShaderResourceView* srv = static_cast<DX11Texture*>(texture)->GetSRV();
        m_context->CSSetShaderResources(i, 1, &srv);
    }

    // UAV 出力 (CS ステージ)
    ID3D11UnorderedAccessView* uavs[2] = { nullptr, nullptr };
    for (uint32_t i = 0; i < 2; ++i)
    {
        if (auto* texture = resources.Get(call.uavOutputs[i]))
            uavs[i] = static_cast<DX11Texture*>(texture)->GetUAV();
    }
    m_context->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);

    // Dispatch
    m_context->Dispatch(call.dispatchX, call.dispatchY, call.dispatchZ);

    // UAV / SRV / CS をアンバインドする (次パスでの SRV 競合を防ぐ)
    ID3D11UnorderedAccessView* nullUAVs[2] = { nullptr, nullptr };
    m_context->CSSetUnorderedAccessViews(0, 2, nullUAVs, nullptr);
    ID3D11ShaderResourceView* nullSRVs[16] = {};
    m_context->CSSetShaderResources(0, 16, nullSRVs);
    m_context->CSSetShader(nullptr, nullptr, 0);
}

// =============================================================================
// Submit — DrawCall の内容に従ってパイプラインを構築して Draw を発行する
// =============================================================================

void DX11Renderer::Submit(const DrawCall& call, ResourceManager& resources)
{
    // ---- 1. Pipeline State (RS / OM ステート) --------------------------------
    // PipelineState は毎フレーム Apply するが、DX11 ドライバが重複バインドを検出して
    // 実際の状態変更がない場合はステートチェンジコストをスキップする。
    if (auto* pipelineState = resources.Get(call.pipelineState))
        static_cast<DX11PipelineState*>(pipelineState)->Apply(m_context.Get());

    // ---- 2. Shader + InputLayout (VS / PS / IA) --------------------------------
    auto* boundShader = resources.Get(call.shader);
    if (boundShader)
        static_cast<DX11Shader*>(boundShader)->Bind(m_context.Get());

    // WHY: ShadowMap は VS が出した SV_POSITION の深度だけを書き込むパスで、PS は空実装。
    //      PS を残したまま colorCount=0 の RT に Draw すると RTV 未設定警告が出るため、
    //      DepthCopy のように PS 側で SV_Depth を生成するパスは除外し、ShadowMap だけ PS を外す。
    const bool isShadowMapShader = boundShader &&
        static_cast<DX11Shader*>(boundShader)->GetPath().find("ShadowMap") != std::string::npos;
    if (m_currentRT && m_currentRT->GetColorCount() == 0 && isShadowMapShader)
        m_context->PSSetShader(nullptr, nullptr, 0);

    // ---- 3. Constant Buffers (VS・PS 両方の同スロットへバインド) ----------------
    // 同じ定数バッファを VS と PS の両方にバインドすることで、
    // シェーダーの種類ごとにスロットを分けずに済む。
    for (uint32_t i = 0; i < static_cast<uint32_t>(call.constantBuffers.size()); ++i)
    {
        auto* cb = resources.Get(call.constantBuffers[i]);
        if (!cb) continue;
        ID3D11Buffer* buf = static_cast<DX11ConstantBuffer*>(cb)->GetBuffer();
        m_context->VSSetConstantBuffers(i, 1, &buf);
        m_context->PSSetConstantBuffers(i, 1, &buf);
    }

    // ---- 4. Textures (SRV → PS ステージ) ----------------------------------------
    // WHY: DrawCall ごとに未使用スロットを NULL に戻す。
    //      前の DrawCall の SRV が残ると、次のパスで同じリソースを RTV/DSV として使った際に
    //      DX11 デバッグレイヤーの HAZARD 警告や意図しないサンプリングが起きる。
    static ID3D11ShaderResourceView* const kNullSRVs[16] = {};
    m_context->PSSetShaderResources(0, 16, kNullSRVs);
    for (uint32_t i = 0; i < static_cast<uint32_t>(call.textures.size()); ++i)
    {
        auto* texture = resources.Get(call.textures[i]);
        if (!texture) continue;
        ID3D11ShaderResourceView* srv = static_cast<DX11Texture*>(texture)->GetSRV();
        m_context->PSSetShaderResources(i, 1, &srv);
    }

    // ---- 5. Primitive Topology (IA ステージ) --------------------------------------
    m_context->IASetPrimitiveTopology(
        call.topology == PrimitiveTopology::LINE_LIST
            ? D3D11_PRIMITIVE_TOPOLOGY_LINELIST
            : D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // ---- 6. Vertex Buffer (IA ステージ) ------------------------------------------
    if (auto* vertexBuffer = resources.Get(call.vertexBuffer))
    {
        auto*         dx11vb = static_cast<DX11Buffer*>(vertexBuffer);
        ID3D11Buffer* vb     = dx11vb->GetBuffer();
        UINT          stride = dx11vb->GetStride();
        UINT          offset = 0;
        m_context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    }

    // ---- 7. Draw (インデックスあり / なしで分岐) -----------------------------------
    if (auto* indexBuffer = resources.Get(call.indexBuffer))
    {
        // DXGI_FORMAT_R32_UINT: インデックスは uint32_t 固定
        ID3D11Buffer* ib = static_cast<DX11Buffer*>(indexBuffer)->GetBuffer();
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
    if (FAILED(hr)) { FBZZ_LOG_ERROR("ResizeBuffers failed: 0x%08X", (unsigned)hr); return; }

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

void DX11Renderer::BindRenderTarget(IRenderTarget* rt)
{
    // RTV/DSV を新たにバインドする前に PS・CS の SRV を全スロット解除する。
    // 同一サブリソースが SRV と RTV/DSV に同時バインドされると DX11 デバッグ層が
    // DEVICE_OMSETRENDERTARGETS_HAZARD を報告するため、事前に競合を取り除く。
    static ID3D11ShaderResourceView* const kNullSRVs[16] = {};
    m_context->PSSetShaderResources(0, 16, kNullSRVs);
    m_context->CSSetShaderResources(0, 16, kNullSRVs);

    D3D11_VIEWPORT vp = {};
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;

    if (!rt)
    {
        m_currentRT = nullptr;
        m_context->OMSetRenderTargets(1, m_renderTargetView.GetAddressOf(), m_depthStencilView.Get());
        vp.Width  = static_cast<float>(m_width);
        vp.Height = static_cast<float>(m_height);
        m_context->RSSetViewports(1, &vp);
        return;
    }
    m_currentRT = static_cast<DX11RenderTarget*>(rt);
    ID3D11RenderTargetView* rtvs[DX11RenderTarget::MAX_COLOR] = {};
    uint32_t count = 0;
    m_currentRT->GetRTVs(rtvs, count);
    // colorCount=0 (深度専用 RT) の場合 count=0, rtvs=nullptr → デプスのみバインド
    m_context->OMSetRenderTargets(count, count > 0 ? rtvs : nullptr, m_currentRT->GetDSV());
    // RT サイズに合わせてビューポートを更新する。
    // ビューポートが RT と異なると NDC → ピクセル変換がずれ、シャドウマップの
    // 深度が誤った UV 位置に書き込まれる (Pass 1 → Pass 2 でサンプル位置不一致)。
    vp.Width  = static_cast<float>(m_currentRT->GetWidth());
    vp.Height = static_cast<float>(m_currentRT->GetHeight());
    m_context->RSSetViewports(1, &vp);
}

void DX11Renderer::SetRenderTarget(ResourceHandle<RenderTargetTag> rt, ResourceManager& resources)
{
    BindRenderTarget(resources.Get(rt));
}

void DX11Renderer::SetSampler(uint32_t slot, SamplerMode mode)
{
    // m_samplers のインデックスは SamplerMode の列挙値と一致させている (InitSamplers 参照)
    uint32_t idx = static_cast<uint32_t>(mode);
    ID3D11SamplerState* sampler = m_samplers[idx].Get();
    m_context->PSSetSamplers(slot, 1, &sampler);
    m_context->CSSetSamplers(slot, 1, &sampler);
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
    // SamplerMode の列挙値と配列インデックスを一致させる。
    // SamplerMode::COUNT = 8 個を Init 時に一括生成してキャッシュする。
    auto make = [&](D3D11_FILTER filter,
                    D3D11_TEXTURE_ADDRESS_MODE addr,
                    uint32_t maxAniso,
                    const FLOAT* borderColor,
                    Microsoft::WRL::ComPtr<ID3D11SamplerState>& out)
    {
        D3D11_SAMPLER_DESC desc  = {};
        desc.Filter              = filter;
        desc.AddressU            = addr;
        desc.AddressV            = addr;
        desc.AddressW            = addr;
        desc.MaxAnisotropy       = maxAniso;
        desc.ComparisonFunc      = D3D11_COMPARISON_NEVER;
        desc.MinLOD              = 0.0f;
        desc.MaxLOD              = D3D11_FLOAT32_MAX;
        if (borderColor)
            memcpy(desc.BorderColor, borderColor, sizeof(desc.BorderColor));
        m_device->CreateSamplerState(&desc, out.GetAddressOf());
    };

    const FLOAT zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

    // [0] WRAP_ANISOTROPIC
    make(D3D11_FILTER_ANISOTROPIC,          D3D11_TEXTURE_ADDRESS_WRAP,   16, nullptr, m_samplers[0]);
    // [1] WRAP_TRILINEAR
    make(D3D11_FILTER_MIN_MAG_MIP_LINEAR,   D3D11_TEXTURE_ADDRESS_WRAP,    1, nullptr, m_samplers[1]);
    // [2] WRAP_BILINEAR
    make(D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT, D3D11_TEXTURE_ADDRESS_WRAP, 1, nullptr, m_samplers[2]);
    // [3] WRAP_POINT
    make(D3D11_FILTER_MIN_MAG_MIP_POINT,    D3D11_TEXTURE_ADDRESS_WRAP,    1, nullptr, m_samplers[3]);
    // [4] CLAMP_ANISOTROPIC
    make(D3D11_FILTER_ANISOTROPIC,          D3D11_TEXTURE_ADDRESS_CLAMP,  16, nullptr, m_samplers[4]);
    // [5] CLAMP_LINEAR
    make(D3D11_FILTER_MIN_MAG_MIP_LINEAR,   D3D11_TEXTURE_ADDRESS_CLAMP,   1, nullptr, m_samplers[5]);
    // [6] CLAMP_POINT
    make(D3D11_FILTER_MIN_MAG_MIP_POINT,    D3D11_TEXTURE_ADDRESS_CLAMP,   1, nullptr, m_samplers[6]);
    // [7] BORDER_ZERO → PCF 比較サンプラー (SamplerComparisonState / SAMPLER_SHADOW s1)
    //   LESS_EQUAL: depth <= stored → 1.0 (照らされている)
    //   境界色 1.0: ライト錐台外は常に照らされている (影なし) にする
    {
        const FLOAT ones[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        D3D11_SAMPLER_DESC desc  = {};
        desc.Filter              = D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
        desc.AddressU = desc.AddressV = desc.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
        desc.ComparisonFunc      = D3D11_COMPARISON_LESS_EQUAL;
        desc.MaxLOD              = D3D11_FLOAT32_MAX;
        memcpy(desc.BorderColor, ones, sizeof(desc.BorderColor));
        m_device->CreateSamplerState(&desc, m_samplers[7].GetAddressOf());
    }
    // [8] WRAP_ANISOTROPIC_4X — 地形ディフューズ用。x16 の約 1/3 コストで
    //   斜め方向の縦縞ノイズを抑えつつ、全画素に 16x を掛ける過剰品質を回避する。
    make(D3D11_FILTER_ANISOTROPIC, D3D11_TEXTURE_ADDRESS_WRAP, 4, nullptr, m_samplers[8]);
}

// =============================================================================
// =============================================================================

// =============================================================================
// GPU Timestamp Query プロファイリング
// =============================================================================

void DX11Renderer::InitGpuQueryFrame(GpuQueryFrame& frame)
{
    // WHY: クエリオブジェクトは生成コストがあるため、フレームごとではなく初回のみ生成する。
    //      GPU_QUERY_LATENCY フレーム分を事前に生成し、リングバッファで循環させる。
    D3D11_QUERY_DESC disjDesc = {};
    disjDesc.Query            = D3D11_QUERY_TIMESTAMP_DISJOINT;
    m_device->CreateQuery(&disjDesc, frame.disjoint.GetAddressOf());

    D3D11_QUERY_DESC tsDesc = {};
    tsDesc.Query            = D3D11_QUERY_TIMESTAMP;
    for (int i = 0; i < GPU_MAX_PASSES; ++i) {
        m_device->CreateQuery(&tsDesc, frame.beginTs[i].GetAddressOf());
        m_device->CreateQuery(&tsDesc, frame.endTs[i].GetAddressOf());
    }
}

void DX11Renderer::GpuProfBeginFrame()
{
    GpuQueryFrame& frame = m_gpuFrames[m_gpuWriteIdx];
    if (!frame.disjoint)
        InitGpuQueryFrame(frame);

    frame.count = 0;
    frame.begun = true;
    frame.ended = false;

    // TIMESTAMP_DISJOINT クエリで GPU クロック周波数の一貫性を保証する。
    // Begin 〜 End の間に発行した TIMESTAMP クエリが有効かどうかも disjoint 結果で判断する。
    m_context->Begin(frame.disjoint.Get());
}

void DX11Renderer::GpuProfEndFrame()
{
    GpuQueryFrame& frame = m_gpuFrames[m_gpuWriteIdx];
    if (!frame.begun) return;

    m_context->End(frame.disjoint.Get());
    frame.ended = true;

    // 書き込みインデックスを次のフレームへ進める
    m_gpuWriteIdx = (m_gpuWriteIdx + 1) % GPU_QUERY_LATENCY;
    if (m_gpuFilled < GPU_QUERY_LATENCY)
        ++m_gpuFilled;
}

void DX11Renderer::GpuProfBeginPass(const char* name)
{
    GpuQueryFrame& frame = m_gpuFrames[m_gpuWriteIdx];
    if (!frame.begun || frame.count >= GPU_MAX_PASSES) return;

    const int idx = frame.count;
    // strncpy_s: バッファオーバーランを防ぐ。名前が長い場合は末尾を切り捨てる。
    strncpy_s(frame.names[idx], sizeof(frame.names[idx]),
              name ? name : "Unknown", _TRUNCATE);

    // パス開始直前のタイムスタンプを GPU コマンドキューに積む。
    // WHY: End() を Begin() のように使うのが D3D11 Timestamp クエリの慣例。
    m_context->End(frame.beginTs[idx].Get());
}

void DX11Renderer::GpuProfEndPass(const char* /*name*/)
{
    GpuQueryFrame& frame = m_gpuFrames[m_gpuWriteIdx];
    if (!frame.begun || frame.count >= GPU_MAX_PASSES) return;

    m_context->End(frame.endTs[frame.count].Get());
    ++frame.count;
}

void DX11Renderer::GpuProfCollect()
{
    m_gpuResults.clear();

    // QUERY_LATENCY フレーム分溜まるまで収集しない。
    // WHY: GPU が処理しきれていないフレームの結果を読もうとすると GetData がビジー待ちになりパフォーマンス劣化する。
    if (m_gpuFilled < GPU_QUERY_LATENCY) return;

    GpuQueryFrame& frame = m_gpuFrames[m_gpuCollectIdx];
    if (!frame.ended) return;

    // D3D11_ASYNC_GETDATA_DONOTFLUSH: フラッシュを避けてノンブロッキングで読む。
    // まだ GPU が終わっていない場合は S_FALSE が返り、その周のフレームをスキップする。
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjData = {};
    HRESULT hr = m_context->GetData(frame.disjoint.Get(), &disjData,
                                    sizeof(disjData), D3D11_ASYNC_GETDATA_DONOTFLUSH);
    if (hr != S_OK || disjData.Disjoint) {
        // GPU クロックが不安定 (リモートデスクトップ切替等) な場合は Disjoint = true になる。
        // スキップしてインデックスは進めない (次フレームで再試行)。
        return;
    }

    const double freqMs = static_cast<double>(disjData.Frequency) / 1000.0;

    for (int i = 0; i < frame.count; ++i) {
        UINT64 tsBegin = 0, tsEnd = 0;
        hr = m_context->GetData(frame.beginTs[i].Get(), &tsBegin,
                                sizeof(UINT64), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (hr != S_OK) continue;

        hr = m_context->GetData(frame.endTs[i].Get(), &tsEnd,
                                sizeof(UINT64), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (hr != S_OK) continue;

        const double gpuMs = static_cast<double>(tsEnd - tsBegin) / freqMs;
        m_gpuResults.push_back({ frame.names[i], gpuMs });
    }

    m_gpuCollectIdx = (m_gpuCollectIdx + 1) % GPU_QUERY_LATENCY;
}

} // namespace fbzz::renderer
