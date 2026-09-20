/// @file    DX12Context.cpp
/// @brief   DirectX 12 の初期化、フレーム記録、フェンス同期、リサイズ処理。
/// @author  Hasegawa Jin
/// @date    2026-07-15
#include "DX12Context.hpp"

#include "../GpuValidation.hpp"

#include <Engine/Core/Logger.hpp>
#include <cstring>
#include <cwchar>
#include <algorithm>
#include <iterator>
#include <vector>
#if defined(FBZZ_GPU_VALIDATION)
#include <d3d12sdklayers.h>
#endif

namespace fbzz::renderer {

namespace {

/// @note 遮蔽検知の Present-test は実 Present を行わないため、常に同期無しで投げる。
constexpr UINT kPresentSyncIntervalNoVsync = 0;

bool CheckResult(HRESULT result, const char* operation)
{
    if (SUCCEEDED(result))
        return true;
    FBZZ_LOG_ERROR("DX12Context: %s に失敗しました (HRESULT=0x%08X)", operation,
                   static_cast<unsigned int>(result));
    return false;
}

} // namespace

DX12Context::~DX12Context()
{
    Shutdown();
}

bool DX12Context::Initialize(HWND hwnd, uint32_t width, uint32_t height)
{
    m_width = width;
    m_height = height;
    FBZZ_LOG_INFO("DX12Context::Initialize: 開始 (%ux%u)", width, height);

#if defined(FBZZ_GPU_VALIDATION)
    Microsoft::WRL::ComPtr<ID3D12Debug> debug;
    /// @note `FBZZ_GPU_VALIDATION=0` を環境変数に入れると、ビルドし直さずに切れる。
    if (gpuvalidation::IsEnabled() && SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
        debug->EnableDebugLayer();
        FBZZ_LOG_INFO("DX12Context: デバッグレイヤー有効化");
        /// @note GPU-Based Validation は全 Draw/Dispatch へ検証処理を挿入し、Scene/Game の
        ///       2 View を描く Editor では数十 FPS まで低下する。通常の Debug Layer は維持し、
        ///       GPU-Based Validation は PIX 等で問題を局所調査するときだけ一時的に有効化する。
        FBZZ_LOG_INFO("DX12Context: GPU-Based Validation 無効 (通常Debug実行)");
    } else if (gpuvalidation::IsEnabled()) {
        FBZZ_LOG_WARN("DX12Context: D3D12GetDebugInterface 取得不可 (デバッグレイヤーなしで続行)");
    }
#endif

    /// @note 各ステップの成否をログに残す。起動時サイレントクラッシュの切り分け用に、
    ///       「直前に出た INFO の次の段階で落ちている」と特定できるようにする。
    if (!CreateFactoryAndDevice(hwnd)) { FBZZ_LOG_ERROR("DX12Context: CreateFactoryAndDevice 失敗"); return false; }
    FBZZ_LOG_INFO("DX12Context: [1/6] Factory/Device/Queue OK");
    if (!CreateSwapChain(hwnd))        { FBZZ_LOG_ERROR("DX12Context: CreateSwapChain 失敗"); return false; }
    FBZZ_LOG_INFO("DX12Context: [2/6] SwapChain OK");
    if (!CreateDescriptorHeaps())      { FBZZ_LOG_ERROR("DX12Context: CreateDescriptorHeaps 失敗"); return false; }
    FBZZ_LOG_INFO("DX12Context: [3/6] Descriptor Heaps OK");
    if (!CreateBackBuffers())          { FBZZ_LOG_ERROR("DX12Context: CreateBackBuffers 失敗"); return false; }
    FBZZ_LOG_INFO("DX12Context: [4/6] Back Buffers OK");
    if (!CreateDepthBuffer())          { FBZZ_LOG_ERROR("DX12Context: CreateDepthBuffer 失敗"); return false; }
    FBZZ_LOG_INFO("DX12Context: [5/6] Depth Buffer OK");
    if (!CreateCommandsAndFence())     { FBZZ_LOG_ERROR("DX12Context: CreateCommandsAndFence 失敗"); return false; }
    FBZZ_LOG_INFO("DX12Context: [6/6] Commands/Fence OK — Initialize 完了");
    return true;
}

bool DX12Context::CreateFactoryAndDevice(HWND hwnd)
{
    (void)hwnd;
    UINT flags = 0;
#if defined(FBZZ_GPU_VALIDATION)
    if (gpuvalidation::IsEnabled())
        flags |= DXGI_CREATE_FACTORY_DEBUG;
#endif
    HRESULT factoryResult = CreateDXGIFactory2(flags, IID_PPV_ARGS(&m_factory));
#if defined(FBZZ_GPU_VALIDATION)
    if (FAILED(factoryResult) && (flags & DXGI_CREATE_FACTORY_DEBUG) != 0) {
        /// @note 検証つきの Factory は «グラフィックス ツール» が入っていない機械では作れない。
        ///       検証が無いだけで動く構成を、起動できない構成にしない。
        FBZZ_LOG_WARN("DX12Context: 検証つき DXGI Factory を作れません "
                      "(オプション機能「グラフィックス ツール」未導入?)。検証なしで続行します");
        flags &= ~static_cast<UINT>(DXGI_CREATE_FACTORY_DEBUG);
        factoryResult = CreateDXGIFactory2(flags, IID_PPV_ARGS(&m_factory));
    }
#endif
    if (!CheckResult(factoryResult, "DXGI Factory の生成"))
        return false;
    FBZZ_LOG_INFO("DX12Context: DXGI Factory 生成 OK (flags=0x%X)", flags);

    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    /// @note --warp はソフトウェアラスタライザ。GPU の無い CI で Playtest を回すための口 (Docs/design/ai-verification-loop.md)。
    if (wcsstr(GetCommandLineW(), L"--warp") != nullptr) {
        Microsoft::WRL::ComPtr<IDXGIAdapter> warp;
        if (SUCCEEDED(m_factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)))
            && SUCCEEDED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_device)))) {
            FBZZ_LOG_INFO("DX12Context: アダプター選択 [WARP] (--warp)");
        } else {
            FBZZ_LOG_ERROR("DX12Context: --warp が指定されたが WARP デバイスを作れません");
            return false;
        }
    }
    for (UINT index = 0; !m_device && m_factory->EnumAdapterByGpuPreference(
             index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)) != DXGI_ERROR_NOT_FOUND; ++index) {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0
            && SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                                           IID_PPV_ARGS(&m_device)))) {
            FBZZ_LOG_INFO("DX12Context: アダプター選択 [%ls] (VRAM=%lluMB)",
                          desc.Description,
                          static_cast<unsigned long long>(desc.DedicatedVideoMemory / (1024ull * 1024ull)));
            break;
        }
        adapter.Reset();
    }
    if (!m_device && FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_device)))) {
        FBZZ_LOG_ERROR("DX12Context: DirectX 12 対応デバイスが見つかりません");
        return false;
    }
    FBZZ_LOG_INFO("DX12Context: D3D12 Device 生成 OK");

    /// @note OS/runtime は未知の Shader Model を `E_INVALIDARG` で拒否するため新しい順に照会する。
    ///       コンパイル可能な SM と実機で実行可能な SM は別物で、DXR パスの安全な縮退に使う。
    constexpr D3D_SHADER_MODEL shaderModels[] = {
        D3D_SHADER_MODEL_6_8, D3D_SHADER_MODEL_6_7, D3D_SHADER_MODEL_6_6,
        D3D_SHADER_MODEL_6_5, D3D_SHADER_MODEL_6_4, D3D_SHADER_MODEL_6_3,
        D3D_SHADER_MODEL_6_2, D3D_SHADER_MODEL_6_1, D3D_SHADER_MODEL_6_0,
        D3D_SHADER_MODEL_5_1
    };
    for (const D3D_SHADER_MODEL candidate : shaderModels) {
        D3D12_FEATURE_DATA_SHADER_MODEL shaderModel{candidate};
        if (SUCCEEDED(m_device->CheckFeatureSupport(
                D3D12_FEATURE_SHADER_MODEL, &shaderModel, sizeof(shaderModel)))) {
            m_highestShaderModel = shaderModel.HighestShaderModel;
            break;
        }
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 raytracingOptions{};
    if (SUCCEEDED(m_device->CheckFeatureSupport(
            D3D12_FEATURE_D3D12_OPTIONS5, &raytracingOptions, sizeof(raytracingOptions))))
        m_raytracingTier = raytracingOptions.RaytracingTier;

    /// @note bindless (`ResourceDescriptorHeap`) は «SM 6.6 以上» と «Resource Binding Tier 3»
    ///       の両方が要る。片方でも欠けるとルートシグネチャの生成自体が `E_INVALIDARG` で
    ///       落ちるため、フラグを立てる前に実機能力で確定させる。
    D3D12_FEATURE_DATA_D3D12_OPTIONS bindingOptions{};
    if (SUCCEEDED(m_device->CheckFeatureSupport(
            D3D12_FEATURE_D3D12_OPTIONS, &bindingOptions, sizeof(bindingOptions))))
        m_resourceBindingTier = bindingOptions.ResourceBindingTier;

    const unsigned int shaderModelMajor = (static_cast<unsigned int>(m_highestShaderModel) >> 4u) & 0xFu;
    const unsigned int shaderModelMinor = static_cast<unsigned int>(m_highestShaderModel) & 0xFu;
    FBZZ_LOG_INFO("DX12Context: Shader Model %u.%u / DXR Tier %u.%u / Inline RayQuery=%s",
                  shaderModelMajor, shaderModelMinor,
                  static_cast<unsigned int>(m_raytracingTier) / 10u,
                  static_cast<unsigned int>(m_raytracingTier) % 10u,
                  SupportsInlineRaytracing() ? "対応" : "非対応");
    FBZZ_LOG_INFO("DX12Context: Resource Binding Tier %u / Bindless=%s",
                  static_cast<unsigned int>(m_resourceBindingTier),
                  SupportsBindless() ? "対応" : "非対応 (ディスクリプタテーブル経路のみ)");
    if (m_highestShaderModel < D3D_SHADER_MODEL_6_8) {
        FBZZ_LOG_ERROR("DX12Context: このエンジンは DXC / SM 6.8 を要求します。"
                       "この GPU / ドライバーでは起動できません (DirectX 11 サポートは v1.0 で終了)");
        return false;
    }
    /// @note マテリアルシェーダーは `ResourceDescriptorHeap` を `FBZZ_TEX2D` 経由で無条件に引く。
    ///       非対応のまま起動を許すと、原因が見えない «何も貼られていない» 描画になるため、
    ///       起動時に不足を名指しして止める。
    if (!SupportsBindless()) {
        FBZZ_LOG_ERROR("DX12Context: このエンジンは bindless (Resource Binding Tier 3) を要求します。"
                       "この GPU / ドライバーは Tier %u までです (Docs/design/bindless.md)",
                       static_cast<unsigned int>(m_resourceBindingTier));
        return false;
    }

#if defined(FBZZ_GPU_VALIDATION)
    Microsoft::WRL::ComPtr<ID3D12InfoQueue> infoQueue;
    if (gpuvalidation::IsEnabled() && SUCCEEDED(m_device.As(&infoQueue))) {
        /// @note DX11 と同じ方針: 検証は維持し読み出しは終了時の 1 回だけにする。D3D12 は
        ///       リソース遷移・ディスクリプタ操作の通知が多く、毎フレーム読み出すと
        ///       Development/Debug の CPU コストが Release と大きく離れる。
        const BOOL breakOnError = gpuvalidation::ShouldBreakOnError() ? TRUE : FALSE;
        /// @note メッセージ 1 件ごとの `OutputDebugString` はデバッガー接続時ミリ秒級のため、
        ///       溜めて `Shutdown()` で一度に読む (DX11 側と同じ理由)。
        infoQueue->SetMuteDebugOutput(TRUE);
        infoQueue->SetMessageCountLimit(
            static_cast<UINT64>(gpuvalidation::kMaxStoredMessages));
        infoQueue->ClearStoredMessages();
        infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, breakOnError);
        infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, breakOnError);
        infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, FALSE);
        infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_INFO, FALSE);
        infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_MESSAGE, FALSE);

        /// @note WARNING 以上は残す (解放漏れ・状態違反はここに出る)。実況になる 2 つだけ止める。
        D3D12_MESSAGE_SEVERITY denySeverities[] = {
            D3D12_MESSAGE_SEVERITY_INFO,
            D3D12_MESSAGE_SEVERITY_MESSAGE,
        };
        D3D12_INFO_QUEUE_FILTER filter{};
        filter.DenyList.NumSeverities = static_cast<UINT>(std::size(denySeverities));
        filter.DenyList.pSeverityList = denySeverities;
        infoQueue->AddStorageFilterEntries(&filter);
    }
#endif

    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (!CheckResult(m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_commandQueue)),
                     "Direct Queue の生成"))
        return false;

    BOOL tearing = FALSE;
    m_allowTearing = SUCCEEDED(m_factory->CheckFeatureSupport(
        DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing))) && tearing;
    FBZZ_LOG_INFO("DX12Context: Present VSync=%s / tearing=%s",
                  m_vsync ? "ON" : "OFF", m_allowTearing ? "ON" : "OFF");
    return true;
}

bool DX12Context::CreateSwapChain(HWND hwnd)
{
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = m_width;
    desc.Height = m_height;
    desc.Format = BACK_BUFFER_FORMAT;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = BACK_BUFFER_COUNT;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.Flags = m_allowTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;

    Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain;
    if (!CheckResult(m_factory->CreateSwapChainForHwnd(
            m_commandQueue.Get(), hwnd, &desc, nullptr, nullptr, &swapChain), "SwapChain の生成"))
        return false;
    m_factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    return CheckResult(swapChain.As(&m_swapChain), "IDXGISwapChain3 の取得");
}

bool DX12Context::CreateDescriptorHeaps()
{
    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
    rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvDesc.NumDescriptors = BACK_BUFFER_COUNT;
    if (!CheckResult(m_device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&m_rtvHeap)), "RTV Heap の生成"))
        return false;

    D3D12_DESCRIPTOR_HEAP_DESC dsvDesc{};
    dsvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsvDesc.NumDescriptors = 1;
    if (!CheckResult(m_device->CreateDescriptorHeap(&dsvDesc, IID_PPV_ARGS(&m_dsvHeap)), "DSV Heap の生成"))
        return false;

    D3D12_DESCRIPTOR_HEAP_DESC srvDesc{};
    srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvDesc.NumDescriptors = IMGUI_DESCRIPTOR_CAPACITY;
    srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (!CheckResult(m_device->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&m_imguiSrvHeap)), "ImGui SRV Heap の生成"))
        return false;

    /// @note bindless 一本になったので、このヒープは永続レンジそのもの。
    ///       添字はヒープ先頭からの位置 (`BINDLESS_HEAP_BASE` = 0)。
    srvDesc.NumDescriptors = BINDLESS_HEAP_BASE + BINDLESS_DESCRIPTOR_CAPACITY;
    if (!CheckResult(m_device->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&m_resourceSrvHeap)), "Resource SRV Heap の生成"))
        return false;

    /// @note bindless では «束縛されていない» を添字 `INVALID_BINDLESS_INDEX` で表すため、
    ///       null ビューは要らない。

    m_srvIncrement = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    m_rtvIncrement = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    return true;
}

bool DX12Context::CreateBackBuffers()
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (uint32_t index = 0; index < BACK_BUFFER_COUNT; ++index) {
        if (!CheckResult(m_swapChain->GetBuffer(index, IID_PPV_ARGS(&m_backBuffers[index])),
                         "BackBuffer の取得"))
            return false;
        m_device->CreateRenderTargetView(m_backBuffers[index].Get(), nullptr, handle);
        handle.ptr += m_rtvIncrement;
    }
    m_backBufferIndex = m_swapChain->GetCurrentBackBufferIndex();
    return true;
}

bool DX12Context::CreateDepthBuffer()
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = m_width;
    desc.Height = m_height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DEPTH_FORMAT;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE clear{};
    clear.Format = DEPTH_FORMAT;
    clear.DepthStencil.Depth = 1.0f;
    if (!CheckResult(m_device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear,
            IID_PPV_ARGS(&m_depthBuffer)), "DepthBuffer の生成"))
        return false;
    m_device->CreateDepthStencilView(m_depthBuffer.Get(), nullptr, GetDsv());
    return true;
}

bool DX12Context::CreateCommandsAndFence()
{
    for (FrameResource& frame : m_frames) {
        if (!CheckResult(m_device->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&frame.allocator)), "CommandAllocator の生成"))
            return false;
    }
    if (!CheckResult(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            m_frames[0].allocator.Get(), nullptr, IID_PPV_ARGS(&m_commandList)), "CommandList の生成"))
        return false;
    m_commandList->Close();
    if (!CheckResult(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)), "Fence の生成"))
        return false;
    m_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (m_fenceEvent == nullptr) return false;

    /// @note 非同期テクスチャ転送の専用コピーキュー。作れなくても起動は止めず、描画キューでの転送へ倒す。
    ///       `FBZZ_COPY_QUEUE=0` で使わない構成を試せる (検証レイヤーでの切り分け用)。
    D3D12_COMMAND_QUEUE_DESC copyDesc{};
    copyDesc.Type = D3D12_COMMAND_LIST_TYPE_COPY;
    if (SUCCEEDED(m_device->CreateCommandQueue(&copyDesc, IID_PPV_ARGS(&m_copyQueue)))
        && SUCCEEDED(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_copyFence)))) {
        wchar_t value[8]{};
        if (GetEnvironmentVariableW(L"FBZZ_COPY_QUEUE", value, 8) > 0 && value[0] == L'0') m_useCopyQueue = false;
        FBZZ_LOG_INFO("DX12Context: コピーキュー %s", m_useCopyQueue ? "有効" : "無効 (FBZZ_COPY_QUEUE=0)");
    } else {
        m_copyQueue.Reset();
        m_copyFence.Reset();
        FBZZ_LOG_WARN("DX12Context: コピーキューを作れません。テクスチャ転送は描画キューで行います");
    }

    /// @note 画面空間 Compute をジオメトリ描画と重ねるためのキュー。使うかどうかを決めるのは
    ///       RenderSettings::asyncCompute (既定 false) で、ここは «機械が持っているか» だけを見る。
    ///       `FBZZ_ASYNC_COMPUTE=0` は機械側の強制停止 (検証レイヤーでの切り分け用)。
    /// @see Docs/design/async-compute.md
    D3D12_COMMAND_QUEUE_DESC computeDesc{};
    computeDesc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    bool computeReady = SUCCEEDED(m_device->CreateCommandQueue(&computeDesc, IID_PPV_ARGS(&m_computeQueue)))
                     && SUCCEEDED(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_computeFence)));
    if (computeReady) {
        for (auto& allocator : m_computeAllocators) {
            if (FAILED(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE,
                                                        IID_PPV_ARGS(&allocator)))) {
                computeReady = false;
                break;
            }
        }
    }
    if (computeReady) {
        computeReady = SUCCEEDED(m_device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_COMPUTE, m_computeAllocators[0].Get(), nullptr,
            IID_PPV_ARGS(&m_computeList)));
        if (computeReady) m_computeList->Close();
    }
    if (computeReady) {
        wchar_t value[8]{};
        if (GetEnvironmentVariableW(L"FBZZ_ASYNC_COMPUTE", value, 8) > 0 && value[0] == L'0')
            m_useAsyncCompute = false;
        FBZZ_LOG_INFO("DX12Context: コンピュートキュー %s",
                      m_useAsyncCompute ? "利用可" : "無効 (FBZZ_ASYNC_COMPUTE=0)");
    } else {
        m_computeQueue.Reset();
        m_computeFence.Reset();
        m_computeList.Reset();
        for (auto& allocator : m_computeAllocators) allocator.Reset();
        FBZZ_LOG_WARN("DX12Context: コンピュートキューを作れません。Compute は描画キューで行います");
    }
    return true;
}

uint64_t DX12Context::SplitGraphicsList()
{
    if (!m_frameOpen) return 0;
    if (!CheckResult(m_commandList->Close(), "CommandList::Close (分割)")) return 0;

    ID3D12CommandList* lists[] = { m_commandList.Get() };
    m_commandQueue->ExecuteCommandLists(1, lists);
    const uint64_t value = m_nextFenceValue++;
    m_commandQueue->Signal(m_fence.Get(), value);

    /// @note アロケーターは Reset しない。投入したリストがまだその記録メモリを読んでいる。
    if (!CheckResult(m_commandList->Reset(m_frames[m_frameIndex].allocator.Get(), nullptr),
                     "CommandList::Reset (分割)")) {
        /// @note ここで失敗するとフレームの記録先が閉じたままになる。以降の記録を捨てる。
        m_frameOpen = false;
        return 0;
    }
    /// @note Reset で全パイプライン状態が落ちた。束縛の記憶を持つ側へ知らせる。
    MarkPipelineStateDirty();
    return value;
}

ID3D12GraphicsCommandList* DX12Context::BeginComputeList(uint64_t waitGraphicsFenceValue)
{
    if (!SupportsAsyncCompute() || m_computeListOpen || !m_frameOpen) return nullptr;
    if (!CheckResult(m_computeList->Reset(m_computeAllocators[m_frameIndex].Get(), nullptr),
                     "ComputeList::Reset"))
        return nullptr;
    if (waitGraphicsFenceValue != 0)
        m_computeQueue->Wait(m_fence.Get(), waitGraphicsFenceValue);
    m_computeListOpen = true;
    return m_computeList.Get();
}

void DX12Context::EndComputeList()
{
    if (!m_computeListOpen) return;
    m_computeListOpen = false;
    if (!CheckResult(m_computeList->Close(), "ComputeList::Close")) return;

    ID3D12CommandList* lists[] = { m_computeList.Get() };
    m_computeQueue->ExecuteCommandLists(1, lists);
    const uint64_t value = m_nextComputeFenceValue++;
    m_computeQueue->Signal(m_computeFence.Get(), value);
    /// @note この後に投入される描画だけが待つ。区間の結果を読むのは区間より後の描画なので足りる。
    m_commandQueue->Wait(m_computeFence.Get(), value);
}

bool DX12Context::BeginFrame()
{
    if (m_frameOpen || !m_swapChain || m_suspended)
        return false;
    /// @note 遮蔽中は Present-test で復帰を検知するまでフレームを開かない。ロック画面や
    ///       全面被覆で Present が OCCLUDED を返す間、記録・Present を続けると GPU/CPU を
    ///       無駄に消費するため。`DXGI_PRESENT_TEST` は実 Present せず可視性だけ確認する。
    if (m_occluded) {
        if (m_swapChain->Present(kPresentSyncIntervalNoVsync, DXGI_PRESENT_TEST) != S_OK)
            return false;
        m_occluded = false;
    }
    if (m_resizePending) {
        const uint32_t width = m_pendingWidth;
        const uint32_t height = m_pendingHeight;
        m_resizePending = false;
        if (!ApplyResize(width, height)) return false;
    }
    FrameResource& frame = m_frames[m_frameIndex];
    WaitForFence(frame.fenceValue);
    CollectDeferredReleases();
    m_transientUploads[m_frameIndex].clear();
    if (!CheckResult(frame.allocator->Reset(), "CommandAllocator::Reset")
        || !CheckResult(m_commandList->Reset(frame.allocator.Get(), nullptr), "CommandList::Reset"))
        return false;
    /// @note このフレームのコンピュート用アロケーターもここで 1 回だけ巻き戻す。同じフレームの
    ///       描画フェンスを待った後なので、そのフレームのコンピュートも既に終わっている
    ///       (描画キューが区間の完了を待ってから最後の Signal を出すため)。
    if (m_computeAllocators[m_frameIndex])
        m_computeAllocators[m_frameIndex]->Reset();
    RecordPostCopyBarriers();

    m_backBufferIndex = m_swapChain->GetCurrentBackBufferIndex();
    TransitionBackBuffer(D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const auto rtv = GetCurrentRtv();
    const auto dsv = GetDsv();
    m_commandList->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
    D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(m_width), static_cast<float>(m_height), 0.0f, 1.0f};
    D3D12_RECT scissor{0, 0, static_cast<LONG>(m_width), static_cast<LONG>(m_height)};
    m_commandList->RSSetViewports(1, &viewport);
    m_commandList->RSSetScissorRects(1, &scissor);
    m_frameOpen = true;
    return true;
}

void DX12Context::EndFrame()
{
    if (!m_frameOpen)
        return;
    TransitionBackBuffer(D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    if (!CheckResult(m_commandList->Close(), "CommandList::Close")) {
        m_frameOpen = false;
        return;
    }
    ID3D12CommandList* lists[] = {m_commandList.Get()};
    m_commandQueue->ExecuteCommandLists(1, lists);
    const uint64_t fenceValue = m_nextFenceValue++;
    m_commandQueue->Signal(m_fence.Get(), fenceValue);
    m_frames[m_frameIndex].fenceValue = fenceValue;
    const UINT syncInterval = m_vsync ? 1u : 0u;
    const UINT presentFlags = (!m_vsync && m_allowTearing) ? DXGI_PRESENT_ALLOW_TEARING : 0u;
    const HRESULT presentResult = m_swapChain->Present(syncInterval, presentFlags);
    if (presentResult == DXGI_STATUS_OCCLUDED) {
        /// @note 遮蔽開始。次フレームは `BeginFrame` の Present-test 復帰待ちへ回す (エラーではない)。
        m_occluded = true;
    } else if (FAILED(presentResult)) {
        const HRESULT removedReason = m_device ? m_device->GetDeviceRemovedReason() : presentResult;
        FBZZ_LOG_ERROR("DX12Context: Present失敗 (HRESULT=0x%08X, RemovedReason=0x%08X)",
                       static_cast<unsigned int>(presentResult),
                       static_cast<unsigned int>(removedReason));
    } else {
        m_occluded = false;
    }
    m_frameIndex = (m_frameIndex + 1) % FRAME_COUNT;
    m_frameOpen = false;
}

void DX12Context::Resize(uint32_t width, uint32_t height)
{
    if (!m_swapChain) return;
    if (width == 0 || height == 0) {
        m_suspended = true;
        m_pendingWidth = width;
        m_pendingHeight = height;
        return;
    }
    m_suspended = false;
    if (width == m_width && height == m_height) {
        /// @note リサイズドラッグ中は 1 フレームに何度も `WM_SIZE` が届く。途中サイズで
        ///       `m_resizePending` を立てた後に元の寸法へ戻された場合、ここで素通しすると
        ///       古い中間サイズが次の `BeginFrame` で適用されてしまう。
        m_resizePending = false;
        return;
    }
    if (m_frameOpen) {
        m_resizePending = true;
        m_pendingWidth = width;
        m_pendingHeight = height;
        return;
    }
    m_resizePending = false;
    ApplyResize(width, height);
}

bool DX12Context::ApplyResize(uint32_t width, uint32_t height)
{
    if (!m_swapChain || width == 0 || height == 0) return false;
    Flush();
    m_depthBuffer.Reset();
    for (auto& buffer : m_backBuffers)
        buffer.Reset();
    const UINT flags = m_allowTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    if (!CheckResult(m_swapChain->ResizeBuffers(BACK_BUFFER_COUNT, width, height, BACK_BUFFER_FORMAT, flags),
                     "SwapChain::ResizeBuffers"))
        return false;
    m_width = width;
    m_height = height;
    return CreateBackBuffers() && CreateDepthBuffer();
}

void DX12Context::Flush()
{
    if (!m_commandQueue || !m_fence)
        return;
    const uint64_t value = m_nextFenceValue++;
    if (SUCCEEDED(m_commandQueue->Signal(m_fence.Get(), value)))
        WaitForFence(value);
}

void DX12Context::Shutdown()
{
    if (m_commandQueue)
        Flush();
    FlushCopyQueue();
    m_pendingPostCopyBarriers.clear();
    m_copyFence.Reset();
    m_copyQueue.Reset();
    if (m_fenceEvent) {
        CloseHandle(m_fenceEvent);
        m_fenceEvent = nullptr;
    }
    m_frameOpen = false;
    m_depthBuffer.Reset();
    for (auto& buffer : m_backBuffers) buffer.Reset();
    m_imguiSrvHeap.Reset();
    m_resourceSrvHeap.Reset();
    m_dsvHeap.Reset();
    m_rtvHeap.Reset();
    m_fence.Reset();
    m_commandList.Reset();
    for (auto& frame : m_frames) frame.allocator.Reset();
    for (auto& uploads : m_transientUploads) uploads.clear();
    m_deferredResources.clear();
    m_pendingUploads.clear();
    /// @note 永続 bindless の台帳もヒープと一緒に捨てる。残すと再初期化後に «存在しないヒープの
    ///       添字» を配ってしまい、デバイスロストからの復帰で真っ黒に描画される形で出る。
    m_bindlessNextSlot = 0;
    m_bindlessExhaustionReported = false;
    m_bindlessFreeList.clear();
    m_bindlessPendingFrees.clear();
    m_swapChain.Reset();
    m_commandQueue.Reset();

#if defined(FBZZ_GPU_VALIDATION)
    /// @note デバイスを手放す前に、溜まった検証メッセージを回収する。
    if (m_device) {
        Microsoft::WRL::ComPtr<ID3D12InfoQueue> infoQueue;
        if (SUCCEEDED(m_device.As(&infoQueue)))
            gpuvalidation::DrainStoredMessages<D3D12_MESSAGE>(*infoQueue.Get(), "DX12Context");
    }
#endif

    m_device.Reset();
    m_factory.Reset();

#if defined(FBZZ_GPU_VALIDATION)
    gpuvalidation::ReportLiveObjects(DXGI_DEBUG_D3D12, "D3D12");
#endif
}

void DX12Context::WaitForFence(uint64_t value)
{
    if (value == 0 || m_fence->GetCompletedValue() >= value)
        return;
    if (SUCCEEDED(m_fence->SetEventOnCompletion(value, m_fenceEvent)))
        WaitForSingleObject(m_fenceEvent, INFINITE);
}

void DX12Context::TransitionBackBuffer(D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = m_backBuffers[m_backBufferIndex].Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    m_commandList->ResourceBarrier(1, &barrier);
}

D3D12_CPU_DESCRIPTOR_HANDLE DX12Context::GetCurrentRtv() const
{
    auto handle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(m_backBufferIndex) * m_rtvIncrement;
    return handle;
}

D3D12_CPU_DESCRIPTOR_HANDLE DX12Context::GetDsv() const
{
    return m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
}

D3D12_CPU_DESCRIPTOR_HANDLE DX12Context::GetImGuiSrvCpu() const
{
    return m_imguiSrvHeap->GetCPUDescriptorHandleForHeapStart();
}

D3D12_GPU_DESCRIPTOR_HANDLE DX12Context::GetImGuiSrvGpu() const
{
    return m_imguiSrvHeap->GetGPUDescriptorHandleForHeapStart();
}

D3D12_CPU_DESCRIPTOR_HANDLE DX12Context::GetImGuiSrvCpu(uint32_t index) const
{
    auto handle = GetImGuiSrvCpu();
    handle.ptr += static_cast<SIZE_T>(index) * m_srvIncrement;
    return handle;
}

D3D12_GPU_DESCRIPTOR_HANDLE DX12Context::GetImGuiSrvGpu(uint32_t index) const
{
    auto handle = GetImGuiSrvGpu();
    handle.ptr += static_cast<UINT64>(index) * m_srvIncrement;
    return handle;
}

uint32_t DX12Context::AllocateBindlessSlot()
{
    if (!m_bindlessFreeList.empty()) {
        const uint32_t reused = m_bindlessFreeList.back();
        m_bindlessFreeList.pop_back();
        return reused;
    }
    if (m_bindlessNextSlot >= BINDLESS_DESCRIPTOR_CAPACITY) {
        /// @note 枯渇は縮退であって異常終了ではない。ログの洪水を避けて一度だけ報告する。
        if (!m_bindlessExhaustionReported) {
            FBZZ_LOG_ERROR("DX12Context: bindless ディスクリプタ枯渇 (capacity=%u)。"
                           "以降のテクスチャはディスクリプタテーブル経路で描画します",
                           BINDLESS_DESCRIPTOR_CAPACITY);
            m_bindlessExhaustionReported = true;
        }
        return INVALID_BINDLESS_INDEX;
    }
    return m_bindlessNextSlot++;
}

void DX12Context::FreeBindlessSlot(uint32_t index)
{
    if (index == INVALID_BINDLESS_INDEX || index >= BINDLESS_DESCRIPTOR_CAPACITY)
        return;
    /// @note 直前まで記録したコマンドリストがこの添字を読む可能性があるため、フェンスを
    ///       通過するまで再利用させない。寿命規則は `DeferRelease` と完全に揃える
    ///       (フレーム記録中は次に Signal される値、フレーム外は最後に Signal 済みの値)。
    const uint64_t fenceValue = m_frameOpen ? m_nextFenceValue
        : (m_nextFenceValue > 0 ? m_nextFenceValue - 1 : 0);
    m_bindlessPendingFrees.push_back({index, fenceValue});
}

uint32_t DX12Context::PublishBindlessDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE source)
{
    if (source.ptr == 0) return INVALID_BINDLESS_INDEX;
    const uint32_t slot = AllocateBindlessSlot();
    if (slot == INVALID_BINDLESS_INDEX) return INVALID_BINDLESS_INDEX;
    m_device->CopyDescriptorsSimple(1, GetBindlessCpu(slot), source,
                                    D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    return slot;
}

D3D12_CPU_DESCRIPTOR_HANDLE DX12Context::GetBindlessCpu(uint32_t index) const
{
    auto handle = m_resourceSrvHeap->GetCPUDescriptorHandleForHeapStart();
    if (index == INVALID_BINDLESS_INDEX || index >= BINDLESS_DESCRIPTOR_CAPACITY)
        return handle;
    handle.ptr += static_cast<SIZE_T>(BINDLESS_HEAP_BASE + index) * m_srvIncrement;
    return handle;
}

bool DX12Context::UploadTexture2D(const uint8_t* rgba, uint32_t width, uint32_t height,
                                  Microsoft::WRL::ComPtr<ID3D12Resource>& texture)
{
    const TextureMip mip{ rgba, width, height, static_cast<std::size_t>(width) * 4 };
    return UploadTexture2DMips(std::span<const TextureMip>(&mip, 1), texture);
}

bool DX12Context::UploadTexture2DMips(std::span<const TextureMip> mips,
                                      Microsoft::WRL::ComPtr<ID3D12Resource>& texture)
{
    PendingUpload upload;
    if (!SubmitTexture2DUpload(mips, texture, upload, false))
        return false;
    Flush();
    return true;
}

void DX12Context::FlushCopyQueue()
{
    if (!m_copyQueue || !m_copyFence) return;
    const uint64_t value = m_nextCopyFenceValue++;
    if (FAILED(m_copyQueue->Signal(m_copyFence.Get(), value))) return;
    if (m_copyFence->GetCompletedValue() >= value) return;
    if (SUCCEEDED(m_copyFence->SetEventOnCompletion(value, m_fenceEvent)))
        WaitForSingleObject(m_fenceEvent, INFINITE);
}

void DX12Context::RecordPostCopyBarriers()
{
    /// @note コピーキューで書き終えた転送先を PIXEL_SHADER_RESOURCE へ移す。フレームの頭に置くので、
    ///       その後の描画は遷移済みの状態で読む。公開 (Pump) はフェンス通過を見てから行うので、
    ///       公開済みのテクスチャは必ずここで遷移している。
    const uint64_t completed = m_copyFence ? m_copyFence->GetCompletedValue() : 0;
    std::vector<D3D12_RESOURCE_BARRIER> barriers;
    const auto firstPending = std::remove_if(
        m_pendingPostCopyBarriers.begin(), m_pendingPostCopyBarriers.end(),
        [&](const PostCopyBarrier& entry) {
            if (entry.copyFenceValue > completed) return false;
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = entry.resource.Get();
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            barriers.push_back(barrier);
            return true;
        });
    if (!barriers.empty())
        m_commandList->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
    /// @note 遷移を記録したフレームが終わるまで転送先を生かす (記録したコマンドがまだ指している)。
    for (auto it = firstPending; it != m_pendingPostCopyBarriers.end(); ++it) DeferRelease(it->resource);
    m_pendingPostCopyBarriers.erase(firstPending, m_pendingPostCopyBarriers.end());
}

bool DX12Context::UploadTexture2DMipsAsync(std::span<const TextureMip> mips,
                                           Microsoft::WRL::ComPtr<ID3D12Resource>& texture,
                                           uint64_t& outFenceValue)
{
    outFenceValue = 0;
    PendingUpload upload;
    /// @note 専用コピーキューがあればそちらへ流す。描画キューのフレームと並んで走り、描画を待たせない。
    if (m_copyQueue && m_useCopyQueue && SubmitTexture2DUpload(mips, texture, upload, true)) {
        upload.fenceValue = m_nextCopyFenceValue++;
        if (FAILED(m_copyQueue->Signal(m_copyFence.Get(), upload.fenceValue))) {
            FlushCopyQueue();
            m_pendingPostCopyBarriers.push_back({ texture, 0 });
            return true;
        }
        m_pendingPostCopyBarriers.push_back({ texture, upload.fenceValue });
        outFenceValue = upload.fenceValue | kCopyQueueTokenBit;
        m_pendingUploads.push_back(std::move(upload));
        return true;
    }
    texture.Reset();
    if (!SubmitTexture2DUpload(mips, texture, upload, false))
        return false;
    /// @note フレーム記録中は新しい値を Signal しない。DeferRelease / FreeBindlessSlot は記録中の
    ///       m_nextFenceValue を «このフレームの終わり» とみなしているため、途中で値を消費すると
    ///       まだ記録中のコマンドが読む資源を早く返してしまう。転送は同じキューで先に実行されるので、
    ///       フレーム末尾の値を待てば転送完了も保証される。
    if (m_frameOpen) {
        upload.fenceValue = m_nextFenceValue;
    } else {
        upload.fenceValue = m_nextFenceValue++;
        if (FAILED(m_commandQueue->Signal(m_fence.Get(), upload.fenceValue))) {
            /// @note Signal できなければ完了を知る手段が無い。待って同期転送へ倒す。
            Flush();
            outFenceValue = 0;
            return true;
        }
    }
    outFenceValue = upload.fenceValue;
    m_pendingUploads.push_back(std::move(upload));
    return true;
}

bool DX12Context::IsFenceComplete(uint64_t fenceValue) const
{
    /// @note デバイス削除後の GetCompletedValue は UINT64_MAX を返すので、ロスト中も待ち続けない。
    /// @see https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12fence-getcompletedvalue ID3D12Fence::GetCompletedValue
    if (fenceValue == 0) return true;
    if ((fenceValue & kCopyQueueTokenBit) != 0) {
        /// @note コピーキューのフェンスで判定する。描画側の遷移は、公開 (フレーム外の Pump) の後に来る
        ///       BeginFrame の頭で RecordPostCopyBarriers が記録するので、公開済みの描画は遷移の後に並ぶ。
        const uint64_t copyValue = fenceValue & ~kCopyQueueTokenBit;
        return m_copyFence && m_copyFence->GetCompletedValue() >= copyValue;
    }
    return GetCompletedFenceValue() >= fenceValue;
}

bool DX12Context::SubmitTexture2DUpload(std::span<const TextureMip> mips,
                                        Microsoft::WRL::ComPtr<ID3D12Resource>& texture,
                                        PendingUpload& outUpload, bool onCopyQueue)
{
    if (mips.empty() || !mips[0].rgba || mips[0].width == 0 || mips[0].height == 0 || !m_device || !m_commandQueue)
        return false;
    if (onCopyQueue && !m_copyQueue) return false;
    /// @note コピーキューで使う資源は COMMON から暗黙に COPY_DEST へ昇格させ、実行後は COMMON へ戻る (decay)。
    ///       遷移の命令はコピーキューに記録できないので、PIXEL_SHADER_RESOURCE への遷移は描画側で行う。
    /// @see https://learn.microsoft.com/en-us/windows/win32/direct3d12/using-resource-barriers-to-synchronize-resource-states-in-direct3d-12 «Implicit state transitions»
    const D3D12_COMMAND_LIST_TYPE listType = onCopyQueue ? D3D12_COMMAND_LIST_TYPE_COPY : D3D12_COMMAND_LIST_TYPE_DIRECT;
    const D3D12_RESOURCE_STATES initialState = onCopyQueue ? D3D12_RESOURCE_STATE_COMMON : D3D12_RESOURCE_STATE_COPY_DEST;
    const UINT mipCount = static_cast<UINT>(mips.size());
    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC textureDesc{};
    textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    textureDesc.Width = mips[0].width;
    textureDesc.Height = mips[0].height;
    textureDesc.DepthOrArraySize = 1;
    textureDesc.MipLevels = static_cast<UINT16>(mipCount);
    textureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    if (FAILED(m_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &textureDesc,
            initialState, nullptr, IID_PPV_ARGS(&texture))))
        return false;

    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(mipCount);
    std::vector<UINT> rowCounts(mipCount);
    std::vector<UINT64> rowSizes(mipCount);
    UINT64 uploadSize = 0;
    m_device->GetCopyableFootprints(&textureDesc, 0, mipCount, 0, footprints.data(), rowCounts.data(),
                                    rowSizes.data(), &uploadSize);
    D3D12_HEAP_PROPERTIES uploadHeap{};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC uploadDesc{};
    uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    uploadDesc.Width = uploadSize;
    uploadDesc.Height = 1;
    uploadDesc.DepthOrArraySize = 1;
    uploadDesc.MipLevels = 1;
    uploadDesc.SampleDesc.Count = 1;
    uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    Microsoft::WRL::ComPtr<ID3D12Resource> upload;
    if (FAILED(m_device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &uploadDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload))))
        return false;
    void* mapped = nullptr;
    if (FAILED(upload->Map(0, nullptr, &mapped)))
        return false;
    for (UINT level = 0; level < mipCount; ++level) {
        const TextureMip& mip = mips[level];
        auto* destination = static_cast<uint8_t*>(mapped) + footprints[level].Offset;
        const std::size_t rowBytes = (std::min)(static_cast<std::size_t>(rowSizes[level]),
                                                static_cast<std::size_t>(mip.width) * 4);
        for (UINT row = 0; row < rowCounts[level]; ++row)
            std::memcpy(destination + static_cast<std::size_t>(row) * footprints[level].Footprint.RowPitch,
                        mip.rgba + static_cast<std::size_t>(row) * mip.rowPitch, rowBytes);
    }
    upload->Unmap(0, nullptr);

    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(m_device->CreateCommandAllocator(listType, IID_PPV_ARGS(&allocator)))
        || FAILED(m_device->CreateCommandList(0, listType, allocator.Get(), nullptr, IID_PPV_ARGS(&list))))
        return false;
    for (UINT level = 0; level < mipCount; ++level) {
        D3D12_TEXTURE_COPY_LOCATION destinationLocation{};
        destinationLocation.pResource = texture.Get();
        destinationLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        destinationLocation.SubresourceIndex = level;
        D3D12_TEXTURE_COPY_LOCATION sourceLocation{};
        sourceLocation.pResource = upload.Get();
        sourceLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        sourceLocation.PlacedFootprint = footprints[level];
        list->CopyTextureRegion(&destinationLocation, 0, 0, 0, &sourceLocation, nullptr);
    }
    if (!onCopyQueue) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = texture.Get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        list->ResourceBarrier(1, &barrier);
    }
    if (FAILED(list->Close()))
        return false;
    ID3D12CommandList* lists[] = {list.Get()};
    (onCopyQueue ? m_copyQueue : m_commandQueue)->ExecuteCommandLists(1, lists);
    /// @note アロケーター・upload バッファ・転送先は GPU が読み書きし終えるまで生かす。呼び出し側がフェンスを決める。
    ///       転送先まで持つのは、転送中に候補が捨てられても (DeferRelease は描画キューのフェンスしか見ない)
    ///       コピーキューが書き終えるまで実体を消さないため。
    /// @see https://learn.microsoft.com/en-us/windows/win32/direct3d12/fence-based-resource-management Fence-Based Resource Management
    outUpload.allocator = std::move(allocator);
    outUpload.list = std::move(list);
    outUpload.upload = std::move(upload);
    outUpload.destination = texture;
    outUpload.onCopyQueue = onCopyQueue;
    return true;
}

bool DX12Context::CreateDefaultBuffer(
    const void* data, size_t sizeBytes, Microsoft::WRL::ComPtr<ID3D12Resource>& buffer)
{
    if (sizeBytes == 0) return false;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = sizeBytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    const D3D12_RESOURCE_STATES initialState = data
        ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_COMMON;
    if (FAILED(m_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &desc,
            initialState, nullptr, IID_PPV_ARGS(&buffer)))) return false;
    if (!data) return true;
    D3D12_HEAP_PROPERTIES uploadHeap{};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    Microsoft::WRL::ComPtr<ID3D12Resource> upload;
    if (FAILED(m_device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)))) return false;
    void* mapped = nullptr;
    if (FAILED(upload->Map(0, nullptr, &mapped))) return false;
    std::memcpy(mapped, data, sizeBytes);
    upload->Unmap(0, nullptr);
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)))
        || FAILED(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                              IID_PPV_ARGS(&list)))) return false;
    list->CopyBufferRegion(buffer.Get(), 0, upload.Get(), 0, sizeBytes);
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = buffer.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    list->ResourceBarrier(1, &barrier);
    if (FAILED(list->Close())) return false;
    ID3D12CommandList* lists[] = {list.Get()};
    m_commandQueue->ExecuteCommandLists(1, lists);
    Flush();
    return true;
}

bool DX12Context::StageBufferCopy(
    ID3D12Resource* destination, const void* data, size_t sizeBytes)
{
    if (!m_frameOpen || !destination || !data || sizeBytes == 0) return false;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = sizeBytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    Microsoft::WRL::ComPtr<ID3D12Resource> upload;
    if (FAILED(m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)))) return false;
    void* mapped = nullptr;
    if (FAILED(upload->Map(0, nullptr, &mapped))) return false;
    std::memcpy(mapped, data, sizeBytes);
    upload->Unmap(0, nullptr);
    m_commandList->CopyBufferRegion(destination, 0, upload.Get(), 0, sizeBytes);
    /// @note GPU 実行完了まで Upload リソースをフレームスロットに保持する。
    m_transientUploads[m_frameIndex].push_back(std::move(upload));
    return true;
}

void DX12Context::DeferRelease(Microsoft::WRL::ComPtr<ID3D12Resource>& resource)
{
    if (!resource) return;
    /// @note フレーム記録中は次に Signal される Fence まで GPU 参照が残る。
    ///       フレーム外では最後に Signal 済みの Fence を待てば安全に解放できる。
    const uint64_t fenceValue = m_frameOpen ? m_nextFenceValue
        : (m_nextFenceValue > 0 ? m_nextFenceValue - 1 : 0);
    m_deferredResources.push_back({std::move(resource), fenceValue});
}

void DX12Context::CollectDeferredReleases()
{
    const uint64_t completed = GetCompletedFenceValue();
    const auto firstPending = std::remove_if(
        m_deferredResources.begin(), m_deferredResources.end(),
        [completed](const DeferredResource& entry) { return entry.fenceValue <= completed; });
    m_deferredResources.erase(firstPending, m_deferredResources.end());

    const uint64_t copyCompleted = m_copyFence ? m_copyFence->GetCompletedValue() : 0;
    const auto firstUploading = std::remove_if(
        m_pendingUploads.begin(), m_pendingUploads.end(),
        [completed, copyCompleted](const PendingUpload& entry) {
            return entry.fenceValue <= (entry.onCopyQueue ? copyCompleted : completed);
        });
    m_pendingUploads.erase(firstUploading, m_pendingUploads.end());

    /// @note 通過済みの bindless 枠をフリーリストへ戻す。リソース本体と同じフェンスで守る。
    const auto firstLive = std::remove_if(
        m_bindlessPendingFrees.begin(), m_bindlessPendingFrees.end(),
        [this, completed](const PendingBindlessFree& entry) {
            if (entry.fenceValue > completed) return false;
            m_bindlessFreeList.push_back(entry.index);
            return true;
        });
    m_bindlessPendingFrees.erase(firstLive, m_bindlessPendingFrees.end());
}

} // namespace fbzz::renderer
