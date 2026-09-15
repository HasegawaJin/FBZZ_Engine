/// @file    DX11Renderer.cpp
/// @brief   IRenderer の DX11 実装。
/// @author  Hasegawa Jin
/// @date    2026-05-21
///
/// デバイス・スワップチェーン・バックバッファ・フレーム送信を管理する。
/// 上位レイヤーには IRenderer と ResourceManager の境界だけを見せる。
///
/// d3d11.lib / dxgi.lib はプラグマリンクで解決する。
/// CMakeLists で target_link_libraries に追加してもよいが、
/// DX11 依存を実装ファイルに閉じ込めるためここで宣言している。
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

#include "DX11Renderer.hpp"
#include "DX11Buffer.hpp"
#include "DX11ConstantBuffer.hpp"
#include "DX11Shader.hpp"
#include "DX11PipelineState.hpp"
#include "DX11StructuredBuffer.hpp"
#include "DX11Texture.hpp"
#include "DX11RenderTarget.hpp"
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Renderer/AssetPathService.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Core/HResult.hpp>
#include "../RenderTargetCapture.hpp" // AI 連携: RT → PNG エンコード共通処理
#include "../GpuValidation.hpp"
#include <DirectXTex.h>
#include <Engine/Profiler/ProfileScope.hpp>
#include <dxgi1_5.h>
#include <iterator>
#include <string>
#ifdef FBZZ_GPU_VALIDATION
#include <d3d11sdklayers.h>  // ID3D11InfoQueue
#endif

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
    // DXGI_PRESENT_ALLOW_TEARING を使える環境では、DWM の表示周期と Present を切り離す。
    // WHY: SyncInterval=0 だけでは flip-model のキューが満杯になった際に Present が待機し、
    //      60 Hz 環境で Time::targetFps=144 を指定しても約 60 FPS に制限されるため。
    Microsoft::WRL::ComPtr<IDXGIFactory5> factory5;
    BOOL allowTearing = FALSE;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(factory5.GetAddressOf()))) &&
        SUCCEEDED(factory5->CheckFeatureSupport(
            DXGI_FEATURE_PRESENT_ALLOW_TEARING,
            &allowTearing,
            sizeof(allowTearing)))) {
        m_allowTearing = allowTearing == TRUE;
    }

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
    scDesc.Flags                                       = m_allowTearing
                                                       ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING
                                                       : 0u;

    // Debug / Development では検証レイヤーを立てる (Release は素通し)。
    // FBZZ_GPU_VALIDATION=0 を環境変数に入れると、ビルドし直さずに切れる。
    UINT flags = 0;
#ifdef FBZZ_GPU_VALIDATION
    if (gpuvalidation::IsEnabled())
        flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    // D3D_FEATURE_LEVEL_11_0 を明示して、それ未満の GPU でエラーを即座に返す
    D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
    const auto createDevice = [&](UINT createFlags) {
        return D3D11CreateDeviceAndSwapChain(
            nullptr,                        // 既定アダプター
            D3D_DRIVER_TYPE_HARDWARE,       // GPU ドライバーを使用
            nullptr,
            createFlags,
            &featureLevel, 1,
            D3D11_SDK_VERSION,
            &scDesc,
            m_swapChain.GetAddressOf(),
            m_device.GetAddressOf(),
            nullptr,
            m_context.GetAddressOf());
    };
    HRESULT hr = createDevice(flags);
#ifdef FBZZ_GPU_VALIDATION
    if (FAILED(hr) && (flags & D3D11_CREATE_DEVICE_DEBUG) != 0) {
        // WHY 落とさず作り直すか: 検証レイヤーは Windows の «グラフィックス ツール» が
        //     入っていない機械では生成そのものが失敗する。検証が無いだけで動く構成を、
        //     起動できない構成にはしない。
        FBZZ_LOG_WARN("DX11Renderer: 検証レイヤーを有効化できません "
                      "(オプション機能「グラフィックス ツール」未導入?)。検証なしで続行します");
        flags &= ~static_cast<UINT>(D3D11_CREATE_DEVICE_DEBUG);
        hr = createDevice(flags);
    }
#endif
    FBZZ_HR_CHECK(hr);

    if (!CreateRenderTargetView())  return false;
    if (!CreateDepthStencilView())  return false;

#ifdef FBZZ_GPU_VALIDATION
    // 検証は効かせたまま、読み出しの重さだけを消す。
    // WHY 溜めて終了時に読むか: InfoQueue の取り出しはメッセージ 1 件につき COM 呼び出し 2 回で、
    //     毎フレーム触ると検証レイヤー本体より重い。上限付きで溜め、Shutdown() で一度に吐く。
    //     WARNING を捨てないのは、リソースの取り違えや解放漏れがそこに出るため。
    if (gpuvalidation::IsEnabled())
    {
        Microsoft::WRL::ComPtr<ID3D11InfoQueue> infoQueue;
        if (SUCCEEDED(m_device.As(&infoQueue)))
        {
            // WHY デバッガー接続時だけ止めるか: ブレークポイント例外は、デバッガーが
            //     居ない実行では «原因不明のクラッシュ» にしかならない。
            const BOOL breakOnError = gpuvalidation::ShouldBreakOnError() ? TRUE : FALSE;
            // WHY 実況を黙らせるか: メッセージ 1 件ごとの OutputDebugString は、デバッガーが
            //     付いていると 1 回あたりミリ秒級のラウンドトリップになる。検証レイヤー本体より
            //     この «出力» の方が重い。溜めるのは続け、Shutdown() で一度に読む。
            infoQueue->SetMuteDebugOutput(TRUE);
            infoQueue->SetMessageCountLimit(
                static_cast<UINT64>(gpuvalidation::kMaxStoredMessages));
            infoQueue->ClearStoredMessages();

            infoQueue->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_CORRUPTION, breakOnError);
            infoQueue->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_ERROR,      breakOnError);
            infoQueue->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_WARNING,    FALSE);
            infoQueue->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_INFO,       FALSE);
            infoQueue->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_MESSAGE,    FALSE);

            // INFO / MESSAGE は «状態が変わった» の実況で、量が桁違いに多い。蓄積を止める。
            D3D11_MESSAGE_SEVERITY denySeverities[] = {
                D3D11_MESSAGE_SEVERITY_INFO,
                D3D11_MESSAGE_SEVERITY_MESSAGE,
            };
            D3D11_INFO_QUEUE_FILTER filter = {};
            filter.DenyList.NumSeverities  = static_cast<UINT>(std::size(denySeverities));
            filter.DenyList.pSeverityList  = denySeverities;
            infoQueue->AddStorageFilterEntries(&filter);
        }
    }
#endif

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
    // WHAT: パイプライン参照を解除した後、デバイス子オブジェクトから依存順に明示解放する。
    // WHY: GPU Query 配列など一部のメンバーは C++ の逆順破棄だけでは Device より後に解放される。
    //      Flush() と明示 Reset() により Shader を含む全 DX11 Live Object の終了時残留を防ぐ。
    if (m_context)
    {
        m_context->ClearState();
        m_context->Flush();
    }

    for (GpuQueryFrame& frame : m_gpuFrames)
    {
        frame.disjoint.Reset();
        for (int i = 0; i < GPU_MAX_PASSES; ++i)
        {
            frame.beginTs[i].Reset();
            frame.endTs[i].Reset();
        }
        frame.count = 0;
        frame.begun = false;
        frame.ended = false;
        frame.collected = true;
    }
    m_gpuResults.clear();

    for (auto& sampler : m_samplers)
        sampler.Reset();
    m_depthStencilBuffer.Reset();
    m_depthStencilView.Reset();
    m_renderTargetView.Reset();
    m_swapChain.Reset();

#ifdef FBZZ_GPU_VALIDATION
    // デバイスを手放す前に、溜まった検証メッセージを回収する。
    if (m_device) {
        Microsoft::WRL::ComPtr<ID3D11InfoQueue> infoQueue;
        if (SUCCEEDED(m_device.As(&infoQueue)))
            gpuvalidation::DrainStoredMessages<D3D11_MESSAGE>(*infoQueue.Get(), "DX11Renderer");
    }
#endif

    m_context.Reset();
    m_device.Reset();
    m_currentRT = nullptr;

#ifdef FBZZ_GPU_VALIDATION
    // 参照を全部落とした «後» に数える。ここで残っているものが本当の解放漏れ。
    gpuvalidation::ReportLiveObjects(DXGI_DEBUG_D3D11, "D3D11");
#endif

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

    BindStaticSamplers();

    m_currentRT = nullptr;
    m_context->OMSetRenderTargets(1, m_renderTargetView.GetAddressOf(), m_depthStencilView.Get());
}

// レジスタごとに意味を固定したサンプラーをフレーム頭で 1 回だけ張る。
//
// WHY パスごとに差し替えないか: DX12 は Root Signature へ焼き込む静的サンプラーなので
//     1 レジスタに 1 つの意味しか持てない。DX11 だけ差し替えられるようにしておくと、
//     同じシェーダーがバックエンドによって違う絵を出す。意味を固定して両者を揃える。
// LAYOUT: DX12PsoCache.cpp の MakeStaticSamplers と、Assets/Shaders/Common/Binding.hlsli の
//         SAMPLER_* に一致させること。3 か所のうち 1 つだけ変えると静かに壊れる。
void DX11Renderer::BindStaticSamplers()
{
    // s6 はどのシェーダーも宣言していない予約枠。DX12 側と数を揃えるために埋めておく。
    static constexpr SamplerMode kSlotModes[] = {
        SamplerMode::WRAP_ANISOTROPIC,     // s0 SAMPLER_DEFAULT      : メッシュのタイリング
        SamplerMode::BORDER_ZERO,          // s1 SAMPLER_SHADOW       : 比較サンプラー (PCF)
        SamplerMode::CLAMP_LINEAR,         // s2 SAMPLER_LINEAR_CLAMP : 全画面フェッチ / LUT
        SamplerMode::CLAMP_POINT,          // s3 SAMPLER_POINT_CLAMP  : TAA 再投影
        SamplerMode::WRAP_BILINEAR,        // s4 SAMPLER_WRAP_LINEAR  : タイラブルな 3D ノイズ
        SamplerMode::CLAMP_LINEAR,         // s5                      : UI / ライト Cookie
        SamplerMode::CLAMP_POINT,          // s6                      : 予約
        SamplerMode::BORDER_ZERO,          // s7 SAMPLER_SHADOW_PUNCTUAL : Spot / Point の比較
        SamplerMode::WRAP_ANISOTROPIC_4X,  // s8                      : 地形ディフューズ
    };
    constexpr uint32_t kSlotCount = sizeof(kSlotModes) / sizeof(kSlotModes[0]);

    ID3D11SamplerState* samplers[kSlotCount] = {};
    for (uint32_t slot = 0; slot < kSlotCount; ++slot)
        samplers[slot] = m_samplers[static_cast<uint32_t>(kSlotModes[slot])].Get();

    m_context->PSSetSamplers(0, kSlotCount, samplers);
    m_context->CSSetSamplers(0, kSlotCount, samplers);
}

void DX11Renderer::EndFrame()
{
    // FPS limiting is handled by Time::targetFps.
    FBZZ_PROFILE_SCOPE("DX11Renderer::Present");
    // VSync 無効時は対応環境で tearing を許可し、Present 内の DWM 同期待ちを発生させない。
    // 有効時は SyncInterval=1 にし、tearing フラグは落とす (併用は DXGI が拒否する)。
    const UINT syncInterval = m_vsync ? 1u : 0u;
    const UINT presentFlags = (!m_vsync && m_allowTearing) ? DXGI_PRESENT_ALLOW_TEARING : 0u;
    m_swapChain->Present(syncInterval, presentFlags);
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

std::unique_ptr<IBuffer> DX11Renderer::CreateNativeGpuWritableVertexBuffer(size_t sizeBytes, uint32_t stride)
{
    auto buf = std::make_unique<DX11Buffer>();
    if (!buf->InitGpuWritableVertex(m_device.Get(), m_context.Get(), sizeBytes, stride))
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

std::unique_ptr<ITexture> DX11Renderer::CreateNativeTexture3DFromData(
    const uint8_t* rgba, uint32_t width, uint32_t height, uint32_t depth)
{
    auto tex = std::make_unique<DX11Texture>();
    if (!tex->Init3DFromData(m_device.Get(), rgba, width, height, depth))
        return nullptr;
    return tex;
}

std::unique_ptr<ITexture> DX11Renderer::CreateNativeTextureFromRenderTarget(
    IRenderTarget& rt,
    uint32_t index,
    RenderTargetTextureKind kind)
{
    auto* dxRT = static_cast<DX11RenderTarget*>(&rt);
    ID3D11ShaderResourceView* srv =
        (kind == RenderTargetTextureKind::Depth) ? dxRT->GetDepthSRV() : dxRT->GetColorSRV(index);
    if (!srv)
        return nullptr;

    auto tex = std::make_unique<DX11Texture>();
    tex->InitFromSRV(srv, dxRT->GetWidth(), dxRT->GetHeight());
    return tex;
}

std::unique_ptr<IPipelineState> DX11Renderer::CreateNativePipelineState(const PipelineStateDesc& desc)
{
    auto pso = std::make_unique<DX11PipelineState>();
    if (!pso->Init(m_device.Get(), desc))
        return nullptr;
    return pso;
}

std::unique_ptr<IRenderTarget> DX11Renderer::CreateNativeRenderTarget(uint32_t width, uint32_t height,
                                                                     const RenderTargetDesc& desc)
{
    auto rt = std::make_unique<DX11RenderTarget>();
    if (!rt->Init(m_device.Get(), width, height, desc))
        return nullptr;
    return rt;
}

std::unique_ptr<IRenderTarget> DX11Renderer::CreateNativeCubemapRenderTarget(uint32_t size, uint32_t mipCount)
{
    auto rt = std::make_unique<DX11RenderTarget>();
    if (!rt->InitCubemap(m_device.Get(), size, mipCount))
        return nullptr;
    return rt;
}

std::unique_ptr<ITexture> DX11Renderer::CreateNativeCubeTextureFromRenderTarget(IRenderTarget& rt)
{
    auto* dxRT = static_cast<DX11RenderTarget*>(&rt);
    ID3D11ShaderResourceView* srv = dxRT->GetCubeSRV();
    if (!srv)
        return nullptr;

    // TextureCube SRV を ITexture 化し、TextureTag として各 Lit パスへ束縛可能にする。
    auto tex = std::make_unique<DX11Texture>();
    tex->InitFromSRV(srv, dxRT->GetWidth(), dxRT->GetHeight());
    return tex;
}

bool DX11Renderer::BakeSkyLight(ResourceHandle<RenderTargetTag> envCubeRT, ResourceManager& resources,
                                uint32_t irradianceSize, uint32_t prefilterSize,
                                uint32_t prefilterMips, uint32_t sampleCount,
                                std::unique_ptr<ITexture>& outIrradiance,
                                std::unique_ptr<ITexture>& outPrefilter)
{
    // 入力キューブ RT (SkyCapture の描画先) の TextureCube SRV を取り出す。
    auto* envRT = static_cast<DX11RenderTarget*>(resources.Get(envCubeRT));
    if (!envRT || !envRT->IsCubemap()) return false;
    ID3D11ShaderResourceView* envSRV = envRT->GetCubeSRV();
    if (!envSRV) return false;

    // 畳み込み Compute を持つベイカーを遅延生成し、実証済みの editor 経路を再利用する。
    if (!m_runtimeIblBaker)
        m_runtimeIblBaker = std::make_unique<DX11IblBaker>(m_device.Get(), m_context.Get());

    // 入力キューブは mip0 のみ (SkyCapture)。prefilter の env LOD は mip0 を参照する (envMipCount=1)。
    // ConvolveCubeToTextures は結果を戻り値で返す (out 引数ではない)。
    // WHY: プロジェクトへEngine shaderを複製せず、GameHubが選択したSDKの共有assetを使う。
    const std::string compiledShaders = ResolveAssetPath("Assets/Shaders/compiled/");
    IblTextureSet set = m_runtimeIblBaker->ConvolveCubeToTextures(
        envSRV, compiledShaders,
        irradianceSize, prefilterSize, prefilterMips, sampleCount, /*envMipCount=*/1);
    if (!set.IsValid()) return false;

    // SRV は内部の ID3D11Texture2D を参照保持するため、IblTextureSet の Texture2D ComPtr が
    // スコープアウトしても SRV 経由でリソースは生存する (DX11 のビュー→リソース参照)。
    auto irr = std::make_unique<DX11Texture>();
    irr->InitFromSRV(set.irradianceSrv.Get(), irradianceSize, irradianceSize);
    auto pre = std::make_unique<DX11Texture>();
    pre->InitFromSRV(set.prefilteredSrv.Get(), prefilterSize, prefilterSize);

    outIrradiance = std::move(irr);
    outPrefilter  = std::move(pre);
    return true;
}

std::unique_ptr<ITexture> DX11Renderer::CreateNativeComputeTexture(uint32_t width, uint32_t height)
{
    auto tex = std::make_unique<DX11Texture>();
    if (!tex->InitForCompute(m_device.Get(), width, height))
        return nullptr;
    return tex;
}

std::unique_ptr<ITexture> DX11Renderer::CreateNativeComputeTexture3D(
    uint32_t width, uint32_t height, uint32_t depth)
{
    auto tex = std::make_unique<DX11Texture>();
    if (!tex->InitForCompute3D(m_device.Get(), width, height, depth))
        return nullptr;
    return tex;
}

std::unique_ptr<ITexture> DX11Renderer::CreateNativeDynamicTexture(
    uint32_t width, uint32_t height, DynamicTextureFormat format)
{
    auto tex = std::make_unique<DX11Texture>();
    if (!tex->InitDynamic(m_device.Get(), m_context.Get(), width, height, format))
        return nullptr;
    return tex;
}

std::unique_ptr<IStructuredBuffer> DX11Renderer::CreateNativeStructuredBuffer(const void* data, uint32_t elementCount, uint32_t stride)
{
    auto sb = std::make_unique<DX11StructuredBuffer>();
    if (!sb->Init(m_device.Get(), m_context.Get(), data, elementCount, stride, /*readWrite=*/false))
        return nullptr;
    return sb;
}

std::unique_ptr<IStructuredBuffer> DX11Renderer::CreateNativeRWStructuredBuffer(const void* data, uint32_t elementCount, uint32_t stride)
{
    auto sb = std::make_unique<DX11StructuredBuffer>();
    if (!sb->Init(m_device.Get(), m_context.Get(), data, elementCount, stride, /*readWrite=*/true))
        return nullptr;
    return sb;
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

    // WHY BeginFrame で張ってあるのに張り直すか: DX11IblBaker のようにバックエンド内部で
    //     CSSetSamplers を直接呼ぶ経路があり、そこを通ると s0 が別物のまま残る。
    //     Dispatch ごとに戻しておけば、以降のパスがその影響を受けない。
    BindStaticSamplers();

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

    // StructuredBuffer SRV (添字 = レジスタ番号)
    for (uint32_t i = 0; i < static_cast<uint32_t>(call.srvBuffers.size()); ++i)
    {
        auto* sb = resources.Get(call.srvBuffers[i]);
        if (!sb) continue;
        ID3D11ShaderResourceView* srv = static_cast<DX11StructuredBuffer*>(sb)->GetSRV();
        m_context->CSSetShaderResources(i, 1, &srv);
    }

    // WHY: DX11 SM5.0 の CS UAV スロットは u0〜u7 の 8 本。
    //      テクスチャ UAV (uavOutputs) は登録インデックス = スロット番号 で直接バインドする。
    //      RWStructuredBuffer (uavBuffers) は u2〜 に固定配置 (Binding.hlsli の UAV_GPU_PARTICLES 等)。
    //      以前は u0/u1 しか処理しておらず、SSR/MotionBlur/GTAO 等のテクスチャ UAV が
    //      全て無視されていた。u0〜u7 を一括バインドするよう修正する。
    ID3D11UnorderedAccessView* uavs[8] = {};
    for (uint32_t i = 0; i < 8 && i < static_cast<uint32_t>(call.uavOutputs.size()); ++i)
    {
        if (auto* texture = resources.Get(call.uavOutputs[i]))
            uavs[i] = static_cast<DX11Texture*>(texture)->GetUAV();
    }
    // RWStructuredBuffer は u2 から順に配置 (UAV_GPU_PARTICLES = u2)
    for (uint32_t i = 0; i < static_cast<uint32_t>(call.uavBuffers.size()); ++i)
    {
        auto* sb = resources.Get(call.uavBuffers[i]);
        if (!sb) continue;
        uavs[2 + i] = static_cast<DX11StructuredBuffer*>(sb)->GetUAV();
    }
    // GPU 書き込み可能な頂点バッファは u4 固定 (Binding.hlsli の UAV_SKINNED_VERTICES)。
    // コンピュートスキニングの出力先。
    if (auto* vb = resources.Get(call.uavVertexBuffer))
        uavs[4] = static_cast<DX11Buffer*>(vb)->GetUAV();
    m_context->CSSetUnorderedAccessViews(0, 8, uavs, nullptr);

    // Dispatch
    m_context->Dispatch(call.dispatchX, call.dispatchY, call.dispatchZ);

    // UAV / SRV / CS をアンバインドする (次パスでの SRV 競合を防ぐ)
    ID3D11UnorderedAccessView* nullUAVs[8] = {};
    m_context->CSSetUnorderedAccessViews(0, 8, nullUAVs, nullptr);
    ID3D11ShaderResourceView* nullSRVs[32] = {};
    m_context->CSSetShaderResources(0, 32, nullSRVs);
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
    // NOTE: 判定は DX11Shader::Init で 1 度だけ済ませてある (毎 DrawCall の文字列検索を避ける)。
    const bool isDepthOnlyDraw = boundShader &&
        static_cast<DX11Shader*>(boundShader)->IsShadowMapShader() &&
        m_currentRT && m_currentRT->GetColorCount() == 0;
    if (isDepthOnlyDraw)
        m_context->PSSetShader(nullptr, nullptr, 0);

    // ---- 3. Constant Buffers (VS・PS 両方の同スロットへバインド) ----------------
    // 同じ定数バッファを VS と PS の両方にバインドすることで、
    // シェーダーの種類ごとにスロットを分けずに済む。
    // WHY: PS を外した深度専用描画では PS ステージへのバインドは全て無視される。
    //      シャドウパスは 1 フレームで最も DrawCall 数が多くなりやすいので、
    //      効かないと分かっている API 呼び出し (CB × スロット数 + SRV クリア) は丸ごと省く。
    for (uint32_t i = 0; i < static_cast<uint32_t>(call.constantBuffers.size()); ++i)
    {
        auto* cb = resources.Get(call.constantBuffers[i]);
        if (!cb) continue;
        ID3D11Buffer* buf = static_cast<DX11ConstantBuffer*>(cb)->GetBuffer();
        m_context->VSSetConstantBuffers(i, 1, &buf);
        if (!isDepthOnlyDraw)
            m_context->PSSetConstantBuffers(i, 1, &buf);
    }

    // ---- 4. Textures (SRV → PS ステージ) ----------------------------------------
    // WHY: DrawCall ごとに未使用スロットを NULL に戻す。
    //      前の DrawCall の SRV が残ると、次のパスで同じリソースを RTV/DSV として使った際に
    //      DX11 デバッグレイヤーの HAZARD 警告や意図しないサンプリングが起きる。
    if (!isDepthOnlyDraw)
    {
        static ID3D11ShaderResourceView* const kNullSRVs[32] = {};
        m_context->PSSetShaderResources(0, 32, kNullSRVs);
        for (uint32_t i = 0; i < static_cast<uint32_t>(call.textures.size()); ++i)
        {
            auto* texture = resources.Get(call.textures[i]);
            if (!texture) continue;
            ID3D11ShaderResourceView* srv = static_cast<DX11Texture*>(texture)->GetSRV();
            m_context->PSSetShaderResources(i, 1, &srv);
        }
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
    // GPU Instancing: instanceBuffer が指定されていれば、1 インスタンスでも
    // StructuredBuffer を VS の t0 にバインドして Instanced Draw を使う。
    // WHY: StructuredBuffer<T> を SV_InstanceID でインデックスする方式は、
    //      通常 Draw に切り替えると VS が未バインドのインスタンスデータを読み、
    //      1 個だけ生成された Detail や Particle が描画されなくなるため。
    // instanceCount が 2 以上なら instanceBuffer が無くてもインスタンス描画する。
    // WHY: GPU メッシュパーティクルは per-instance データを t0 ではなく t14
    //      (vsBuffers, StructuredBuffer<GpuParticle>) から SV_InstanceID で引く。
    //      同じ .hlsl に t0 の Texture2D (albedo) があるため t0 は使えない。
    const bool isInstanced = call.instanceCount > 1
        || (call.instanceCount > 0 && call.instanceBuffer.IsValid());
    if (isInstanced)
    {
        if (auto* sb = resources.Get(call.instanceBuffer))
        {
            ID3D11ShaderResourceView* srv = static_cast<DX11StructuredBuffer*>(sb)->GetSRV();
            m_context->VSSetShaderResources(0, 1, &srv);
        }
    }

    // VS-readable StructuredBuffer (t14〜t15): GPU パーティクル等の頂点データバッファ
    // WHY: Draw 発行前にバインドしないと VS がデータを読めない
    bool hasVsBuffers = false;
    for (uint32_t i = 0; i < static_cast<uint32_t>(call.vsBuffers.size()); ++i)
    {
        auto* sb = resources.Get(call.vsBuffers[i]);
        if (!sb) continue;
        ID3D11ShaderResourceView* srv = static_cast<DX11StructuredBuffer*>(sb)->GetSRV();
        m_context->VSSetShaderResources(14 + i, 1, &srv);
        hasVsBuffers = true;
    }

    // PS-readable StructuredBuffer (t29〜t30): クラスタライティングのライト配列 / インデックスリスト
    // WHY: vsBuffers は VS にしか束縛されないため、PS からバッファを読む経路がこれしかない。
    bool hasPsBuffers = false;
    for (uint32_t i = 0; i < static_cast<uint32_t>(call.psBuffers.size()); ++i)
    {
        auto* sb = resources.Get(call.psBuffers[i]);
        if (!sb) continue;
        ID3D11ShaderResourceView* srv = static_cast<DX11StructuredBuffer*>(sb)->GetSRV();
        m_context->PSSetShaderResources(kPsBufferBaseSlot + i, 1, &srv);
        hasPsBuffers = true;
    }

    if (auto* indexBuffer = resources.Get(call.indexBuffer))
    {
        // DXGI_FORMAT_R32_UINT: インデックスは uint32_t 固定
        ID3D11Buffer* ib = static_cast<DX11Buffer*>(indexBuffer)->GetBuffer();
        m_context->IASetIndexBuffer(ib, DXGI_FORMAT_R32_UINT, 0);
        if (isInstanced)
            m_context->DrawIndexedInstanced(call.indexCount, call.instanceCount, call.startIndex, static_cast<INT>(call.baseVertex), 0);
        else
            m_context->DrawIndexed(call.indexCount, call.startIndex, static_cast<INT>(call.baseVertex));
    }
    else
    {
        if (isInstanced)
            m_context->DrawInstanced(call.vertexCount, call.instanceCount, 0, 0);
        else
            m_context->Draw(call.vertexCount, 0);
    }

    // VS SRV を解除して次パスの競合を防ぐ
    if (isInstanced)
    {
        ID3D11ShaderResourceView* nullSRV = nullptr;
        m_context->VSSetShaderResources(0, 1, &nullSRV);
    }
    if (hasVsBuffers)
    {
        ID3D11ShaderResourceView* nullSRVs[2] = { nullptr, nullptr };
        m_context->VSSetShaderResources(14, 2, nullSRVs);
    }
    // PS SRV も解除する。クラスタバッファは CS が UAV として書き込むため、
    // 束縛したままだと次フレームの Dispatch で SRV/UAV が同一リソースへ同時束縛になり、
    // ドライバーが UAV 側を黙って無効化する (D3D11 の警告付き)。
    if (hasPsBuffers)
    {
        ID3D11ShaderResourceView* nullSRVs[2] = { nullptr, nullptr };
        m_context->PSSetShaderResources(kPsBufferBaseSlot, 2, nullSRVs);
    }
}

// =============================================================================
// Resize — ウィンドウリサイズ時にスワップチェーン・RTV・DSV を再構築する
// =============================================================================

void DX11Renderer::Resize(uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0) return;
    // WHY 同寸で弾くか: WM_SIZE は最大化/復元/フォーカス変化などで同じ寸法のまま何度も届く。
    //     素通しすると毎回 RTV/DSV の破棄と ResizeBuffers が走り、深度バッファの再確保で
    //     目に見えるヒッチが出る。実際に寸法が変わった時だけ再構築する。
    if (width == m_width && height == m_height) return;

    m_width  = width;
    m_height = height;

    // ResizeBuffers を呼ぶ前に RTV・DSV を OM から外し、COM 参照を全て解放する必要がある。
    // 参照が残ったまま ResizeBuffers を呼ぶと DXGI_ERROR_INVALID_CALL が返る。
    m_context->OMSetRenderTargets(0, nullptr, nullptr);
    m_renderTargetView.Reset();
    m_depthStencilView.Reset();
    m_depthStencilBuffer.Reset();

    // Init() で付けた ALLOW_TEARING は ResizeBuffers 後も明示的に維持する。
    const UINT swapChainFlags = m_allowTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u;
    HRESULT hr = m_swapChain->ResizeBuffers(
        0, width, height, DXGI_FORMAT_UNKNOWN, swapChainFlags);
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

bool DX11Renderer::RenderDebugPreview(const DrawCall& call, ResourceHandle<RenderTargetTag> target,
                                      ResourceManager& resources)
{
    if (!m_context || !resources.Get(target) || !resources.Get(call.shader)
        || !resources.Get(call.pipelineState)) return false;

    ID3D11RenderTargetView* targets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> savedTargets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> depth;
    m_context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, targets, depth.GetAddressOf());
    for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
        savedTargets[i].Attach(targets[i]);
    D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    D3D11_RECT scissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    UINT viewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    UINT scissorCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    m_context->RSGetViewports(&viewportCount, viewports);
    m_context->RSGetScissorRects(&scissorCount, scissors);
    Microsoft::WRL::ComPtr<ID3D11Buffer> vertexConstants;
    Microsoft::WRL::ComPtr<ID3D11Buffer> pixelConstants;
    m_context->VSGetConstantBuffers(5, 1, vertexConstants.GetAddressOf());
    m_context->PSGetConstantBuffers(5, 1, pixelConstants.GetAddressOf());
    auto* previous = m_currentRT;

    SetRenderTarget(target, resources);
    Submit(call, resources);

    // プレビュー元が元の RTV/DSV でも競合しないよう、SRV を外してから戻す。
    BindRenderTarget(nullptr);
    m_context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, targets, depth.Get());
    m_context->RSSetViewports(viewportCount, viewports);
    m_context->RSSetScissorRects(scissorCount, scissors);
    // DX11 の Submit は未指定 CB を残す契約なので、診断専用 b5 も元へ戻す。
    m_context->VSSetConstantBuffers(5, 1, vertexConstants.GetAddressOf());
    m_context->PSSetConstantBuffers(5, 1, pixelConstants.GetAddressOf());
    m_currentRT = previous;
    return true;
}

void DX11Renderer::SetViewport(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
    // BindRenderTarget が RT 全体のビューポートを張った後に、その一部へ絞り込む。
    // カスケードシャドウが 1 枚のアトラスをタイル分割して使う (IRenderer::SetViewport 参照)。
    if (width == 0u || height == 0u) return;

    D3D11_VIEWPORT vp = {};
    vp.TopLeftX = static_cast<float>(x);
    vp.TopLeftY = static_cast<float>(y);
    vp.Width    = static_cast<float>(width);
    vp.Height   = static_cast<float>(height);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    m_context->RSSetViewports(1, &vp);
}

// RT のカラーを CPU 側 ScratchImage として掴む。PNG 化と数値評価で同じ読み戻しを共有する。
// WHY: 2 つの入口が別々に CaptureTexture を呼ぶと、片方だけ RT 形式の変更に追従し損ねる。
static bool CaptureDX11RenderTargetImage(ID3D11Device* device, ID3D11DeviceContext* context,
                                         IRenderTarget* base, DirectX::ScratchImage& outImage)
{
    // Platform 層内なので IRenderTarget → 具象へのキャストは許容 (上位からのダウンキャスト禁止規約の対象外)。
    auto* target = static_cast<DX11RenderTarget*>(base);
    if (target == nullptr) return false;
    ID3D11ShaderResourceView* srv = target->GetColorSRV(0);
    if (srv == nullptr) return false;

    // SRV から元テクスチャ (R16G16B16A16_FLOAT) を取り出す。CaptureTexture が内部で STAGING コピーする。
    Microsoft::WRL::ComPtr<ID3D11Resource> resource;
    srv->GetResource(resource.GetAddressOf());
    if (!resource) return false;
    return SUCCEEDED(DirectX::CaptureTexture(device, context, resource.Get(), outImage));
}

bool DX11Renderer::CaptureRenderTargetToPng(ResourceHandle<RenderTargetTag> rt, ResourceManager& resources,
                                            std::vector<uint8_t>& outPng, uint32_t& outWidth, uint32_t& outHeight)
{
    DirectX::ScratchImage captured;
    if (!CaptureDX11RenderTargetImage(m_device.Get(), m_context.Get(), resources.Get(rt), captured)) return false;
    return detail::EncodeCapturedImageToPng(captured, outPng, outWidth, outHeight);
}

bool DX11Renderer::CaptureRenderTargetToLinearRGBA(ResourceHandle<RenderTargetTag> rt, ResourceManager& resources,
                                                   std::vector<float>& outRgba, uint32_t& outWidth, uint32_t& outHeight)
{
    DirectX::ScratchImage captured;
    if (!CaptureDX11RenderTargetImage(m_device.Get(), m_context.Get(), resources.Get(rt), captured)) return false;
    return detail::ReadCapturedImageAsLinearRGBA(captured, outRgba, outWidth, outHeight);
}

void DX11Renderer::SetRenderTargetFace(ResourceHandle<RenderTargetTag> rt, uint32_t face,
                                       uint32_t mip, ResourceManager& resources)
{
    auto* base = static_cast<DX11RenderTarget*>(resources.Get(rt));
    if (!base || !base->IsCubemap()) return;

    ID3D11RenderTargetView* faceRTV = base->GetFaceRTV(face, mip);
    if (!faceRTV) return;

    // RTV を張る前に SRV を解除する (同一サブリソースの SRV/RTV 同時バインド HAZARD を防ぐ)。
    static ID3D11ShaderResourceView* const kNullSRVs[16] = {};
    m_context->PSSetShaderResources(0, 16, kNullSRVs);
    m_context->CSSetShaderResources(0, 16, kNullSRVs);

    // 空ドームは深度不要のため DSV は張らない (null)。
    m_context->OMSetRenderTargets(1, &faceRTV, nullptr);
    m_currentRT = nullptr; // 通常 RT 追跡から外す (面 RTV は m_currentRT で管理しない)

    // ビューポートを当該 mip のサイズに合わせる。
    const uint32_t mipSize = base->GetWidth() >> mip;
    D3D11_VIEWPORT vp = {};
    vp.Width    = static_cast<float>(mipSize > 0 ? mipSize : 1);
    vp.Height   = static_cast<float>(mipSize > 0 ? mipSize : 1);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    m_context->RSSetViewports(1, &vp);
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
    // SamplerMode::COUNT 個を Init 時に一括生成してキャッシュする。
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
    // [7] BORDER_ONE → PCF 比較サンプラー (SamplerComparisonState / SAMPLER_SHADOW s1)
    //   LESS_EQUAL: stored(ブロッカー深度) <= receiver_depth → 1.0 (照らされている)
    //   WHY: DirectX の shadow map は depth test LESS で書かれるため、より遠い(大きい)値が
    //        ブロッカー = 手前ではなく奥側。depth - bias で自己影を防ぎつつ LESS_EQUAL で判定。
    //   境界色 1.0: ライト錐台外サンプルは stored=1.0 → 常に 1.0(lit) を返させる意図だが、
    //               PCF カーネルが境界をまたぐと白四角アーティファクトの原因になる (別途対処)。
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

    // 前回の結果がまだ GetData で読み出されていない場合は Begin を呼ばない。
    // 呼ぶと D3D11 QUERY_BEGIN_ABANDONING_PREVIOUS_RESULTS 警告が発生する。
    if (!frame.collected) {
        frame.begun = false;
        return;
    }

    frame.count     = 0;
    frame.begun     = true;
    frame.ended     = false;
    frame.collected = false;

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

    frame.collected = true;
    m_gpuCollectIdx = (m_gpuCollectIdx + 1) % GPU_QUERY_LATENCY;
}

} // namespace fbzz::renderer
