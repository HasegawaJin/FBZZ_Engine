// FBZZ Engine
// DX12Context.cpp | fbzz::renderer
// DirectX 12 の初期化、フレーム記録、フェンス同期、リサイズ処理
#include "DX12Context.hpp"

#include <Engine/Core/Logger.hpp>
#include <cstring>
#include <algorithm>
#if defined(_DEBUG)
#include <d3d12sdklayers.h>
#include <dxgidebug.h>
#endif

namespace fbzz::renderer {

namespace {

// VSync は常時無効。Present の第 1 引数を 0 に固定し、対応環境では tearing も許可する。
// WHY: フレームレート制御は Application 側の targetFps に任せ、DXGI の表示周期待ちを描画同期に混ぜない。
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

#if defined(_DEBUG)
    Microsoft::WRL::ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
        debug->EnableDebugLayer();
        FBZZ_LOG_INFO("DX12Context: デバッグレイヤー有効化");
        // WHY: GPU-Based Validationは全Draw/Dispatchへ検証処理を挿入し、Scene/Gameの
        //      2 Viewを描くEditorでは数十FPSまで低下する。通常のDebug Layerは維持し、
        //      GPU-Based ValidationはPIX等で問題を局所調査するときだけ一時的に有効化する。
        FBZZ_LOG_INFO("DX12Context: GPU-Based Validation 無効 (通常Debug実行)");
    } else {
        FBZZ_LOG_WARN("DX12Context: D3D12GetDebugInterface 取得不可 (デバッグレイヤーなしで続行)");
    }
#endif

    // 各ステップの成否をログに残す。起動時サイレントクラッシュの切り分け用に、
    // 「直前に出た INFO の次の段階で落ちている」と特定できるようにする。
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
#if defined(_DEBUG)
    flags |= DXGI_CREATE_FACTORY_DEBUG;
#endif
    if (!CheckResult(CreateDXGIFactory2(flags, IID_PPV_ARGS(&m_factory)), "DXGI Factory の生成"))
        return false;
    FBZZ_LOG_INFO("DX12Context: DXGI Factory 生成 OK (flags=0x%X)", flags);

    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    for (UINT index = 0; m_factory->EnumAdapterByGpuPreference(
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

    // WHAT: OS/runtime が未知の Shader Model を E_INVALIDARG で拒否するため、新しい順に照会する。
    // WHY: コンパイル可能な SM と実機で実行可能な SM は別物であり、DXR パスを安全に縮退させるため。
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
    const unsigned int shaderModelMajor = (static_cast<unsigned int>(m_highestShaderModel) >> 4u) & 0xFu;
    const unsigned int shaderModelMinor = static_cast<unsigned int>(m_highestShaderModel) & 0xFu;
    FBZZ_LOG_INFO("DX12Context: Shader Model %u.%u / DXR Tier %u.%u / Inline RayQuery=%s",
                  shaderModelMajor, shaderModelMinor,
                  static_cast<unsigned int>(m_raytracingTier) / 10u,
                  static_cast<unsigned int>(m_raytracingTier) % 10u,
                  SupportsInlineRaytracing() ? "対応" : "非対応");
    if (m_highestShaderModel < D3D_SHADER_MODEL_6_8) {
        FBZZ_LOG_ERROR("DX12Context: DXC / SM 6.8 へ移行済みのため、この GPU/driver では DX12 を起動できません。--renderer=dx11 を使用してください");
        return false;
    }

#if defined(_DEBUG)
    Microsoft::WRL::ComPtr<ID3D12InfoQueue> infoQueue;
    if (SUCCEEDED(m_device.As(&infoQueue))) {
        // DX11 と同じく、検証は維持しつつ Warning/Info の蓄積と定期フラッシュを止める。
        // WHY: D3D12 はリソース遷移・ディスクリプタ操作の通知が多く、毎フレーム蓄積すると
        //      Development/Debug 実行の CPU コストとメモリ使用量が Release と大きく離れる。
        infoQueue->SetMuteDebugOutput(FALSE);
        infoQueue->SetMessageCountLimit(-1);
        infoQueue->ClearStoredMessages();
        infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE);
        infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, TRUE);
        infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, FALSE);
        infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_INFO, FALSE);
        infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_MESSAGE, FALSE);

        // ERROR 以上は残し、Warning 以下は蓄積自体を止める。
        D3D12_MESSAGE_SEVERITY denySeverities[] = {
            D3D12_MESSAGE_SEVERITY_INFO,
            D3D12_MESSAGE_SEVERITY_MESSAGE,
            D3D12_MESSAGE_SEVERITY_WARNING,
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
    FBZZ_LOG_INFO("DX12Context: Present VSync=OFF / tearing=%s", m_allowTearing ? "ON" : "OFF");
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

    srvDesc.NumDescriptors = NULL_DESCRIPTOR_COUNT + FRAME_COUNT * DYNAMIC_DESCRIPTORS_PER_FRAME;
    if (!CheckResult(m_device->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&m_resourceSrvHeap)), "Resource SRV Heap の生成"))
        return false;

    D3D12_DESCRIPTOR_HEAP_DESC nullHeapDesc{};
    nullHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    nullHeapDesc.NumDescriptors = 56;
    nullHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (!CheckResult(m_device->CreateDescriptorHeap(&nullHeapDesc, IID_PPV_ARGS(&m_nullSrvHeap)),
                     "Null SRV staging Heap の生成"))
        return false;

    m_srvIncrement = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto cpu = m_nullSrvHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC textureNull{};
    textureNull.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    textureNull.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    textureNull.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    textureNull.Texture2D.MipLevels = 1;
    for (uint32_t index = 0; index < 32; ++index) {
        m_device->CreateShaderResourceView(nullptr, &textureNull, cpu);
        cpu.ptr += m_srvIncrement;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC bufferNull{};
    bufferNull.Format = DXGI_FORMAT_UNKNOWN;
    bufferNull.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    bufferNull.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    bufferNull.Buffer.NumElements = 1;
    bufferNull.Buffer.StructureByteStride = 16;
    for (uint32_t index = 0; index < 16; ++index) {
        m_device->CreateShaderResourceView(nullptr, &bufferNull, cpu);
        cpu.ptr += m_srvIncrement;
    }
    D3D12_UNORDERED_ACCESS_VIEW_DESC nullUav{};
    nullUav.Format = DXGI_FORMAT_UNKNOWN;
    nullUav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    nullUav.Buffer.NumElements = 1;
    nullUav.Buffer.StructureByteStride = 16;
    for (uint32_t index = 0; index < 8; ++index) {
        m_device->CreateUnorderedAccessView(nullptr, nullptr, &nullUav, cpu);
        cpu.ptr += m_srvIncrement;
    }

    // Root tableのフォールバック用に、CPU stagingで作ったNull descriptorをGPU可視領域へ複製する。
    m_device->CopyDescriptorsSimple(
        56,
        m_resourceSrvHeap->GetCPUDescriptorHandleForHeapStart(),
        m_nullSrvHeap->GetCPUDescriptorHandleForHeapStart(),
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

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
    return m_fenceEvent != nullptr;
}

bool DX12Context::BeginFrame()
{
    if (m_frameOpen || !m_swapChain || m_suspended)
        return false;
    // 遮蔽中は Present-test で復帰を検知するまでフレームを開かない。
    // WHY: ロック画面や全面被覆で Present が OCCLUDED を返す間、記録・Present を続けると
    //      GPU/CPU を無駄に消費する。DXGI_PRESENT_TEST は実 Present せず可視性だけ確認する。
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

    m_backBufferIndex = m_swapChain->GetCurrentBackBufferIndex();
    m_dynamicSrvOffsets[m_frameIndex] = 0;
    m_srvHeapExhaustionReported[m_frameIndex] = false;
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
    const HRESULT presentResult = m_swapChain->Present(
        kPresentSyncIntervalNoVsync, m_allowTearing ? DXGI_PRESENT_ALLOW_TEARING : 0);
    if (presentResult == DXGI_STATUS_OCCLUDED) {
        // 遮蔽開始。次フレームは BeginFrame の Present-test 復帰待ちへ回す (エラーではない)。
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
        // WHY 保留を取り消すか: リサイズドラッグ中は 1 フレームに何度も WM_SIZE が届く。
        //     途中サイズで m_resizePending を立てた後、最後に元の寸法へ戻された場合、
        //     ここで素通しすると古い中間サイズが次の BeginFrame で適用され、
        //     バックバッファだけがウィンドウより小さいまま引き伸ばされる。
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
    if (m_fenceEvent) {
        CloseHandle(m_fenceEvent);
        m_fenceEvent = nullptr;
    }
    m_frameOpen = false;
    m_depthBuffer.Reset();
    for (auto& buffer : m_backBuffers) buffer.Reset();
    m_imguiSrvHeap.Reset();
    m_resourceSrvHeap.Reset();
    m_nullSrvHeap.Reset();
    m_dsvHeap.Reset();
    m_rtvHeap.Reset();
    m_fence.Reset();
    m_commandList.Reset();
    for (auto& frame : m_frames) frame.allocator.Reset();
    for (auto& uploads : m_transientUploads) uploads.clear();
    m_deferredResources.clear();
    m_swapChain.Reset();
    m_commandQueue.Reset();
    m_device.Reset();
    m_factory.Reset();
#if defined(_DEBUG)
    Microsoft::WRL::ComPtr<IDXGIDebug1> dxgiDebug;
    if (SUCCEEDED(DXGIGetDebugInterface1(0, IID_PPV_ARGS(&dxgiDebug))))
        dxgiDebug->ReportLiveObjects(
            DXGI_DEBUG_D3D12, static_cast<DXGI_DEBUG_RLO_FLAGS>(
                DXGI_DEBUG_RLO_SUMMARY | DXGI_DEBUG_RLO_IGNORE_INTERNAL));
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

D3D12_GPU_DESCRIPTOR_HANDLE DX12Context::GetNullPixelSrvTable() const
{
    return m_resourceSrvHeap->GetGPUDescriptorHandleForHeapStart();
}

D3D12_GPU_DESCRIPTOR_HANDLE DX12Context::GetNullVertexSrvTable() const
{
    auto handle = m_resourceSrvHeap->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<UINT64>(32) * m_srvIncrement;
    return handle;
}

D3D12_CPU_DESCRIPTOR_HANDLE DX12Context::GetNullPixelSrv(uint32_t slot) const
{
    auto handle = m_nullSrvHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(slot % 32) * m_srvIncrement;
    return handle;
}

DX12Context::DescriptorTableAllocation DX12Context::AllocatePixelSrvTable()
{
    return AllocateSrvTable(32);
}

DX12Context::DescriptorTableAllocation DX12Context::AllocateVertexSrvTable()
{
    return AllocateSrvTable(16);
}

DX12Context::DescriptorTableAllocation DX12Context::AllocateSrvTable(uint32_t descriptorCount)
{
    uint32_t& offset = m_dynamicSrvOffsets[m_frameIndex];
    if (offset > DYNAMIC_DESCRIPTORS_PER_FRAME
        || descriptorCount > DYNAMIC_DESCRIPTORS_PER_FRAME - offset) {
        // 同一フレームの後続Drawも失敗するため、一度だけ報告してログの洪水を防ぐ。
        if (!m_srvHeapExhaustionReported[m_frameIndex]) {
            FBZZ_LOG_ERROR("DX12Context: shader-visible SRV ヒープ不足 "
                           "(frame=%u, used=%u, request=%u, capacity=%u)",
                           m_frameIndex, offset, descriptorCount,
                           DYNAMIC_DESCRIPTORS_PER_FRAME);
            m_srvHeapExhaustionReported[m_frameIndex] = true;
        }
        return {};
    }
    const uint32_t descriptorIndex = NULL_DESCRIPTOR_COUNT
        + m_frameIndex * DYNAMIC_DESCRIPTORS_PER_FRAME + offset;
    offset += descriptorCount;
    auto cpu = m_resourceSrvHeap->GetCPUDescriptorHandleForHeapStart();
    auto gpu = m_resourceSrvHeap->GetGPUDescriptorHandleForHeapStart();
    cpu.ptr += static_cast<SIZE_T>(descriptorIndex) * m_srvIncrement;
    gpu.ptr += static_cast<UINT64>(descriptorIndex) * m_srvIncrement;
    return {cpu, gpu};
}

DX12Context::DescriptorTableAllocation DX12Context::AllocateUavTable()
{
    return AllocateSrvTable(8);
}

D3D12_CPU_DESCRIPTOR_HANDLE DX12Context::GetNullBufferSrv(uint32_t slot) const
{
    auto handle = m_nullSrvHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(32 + slot % 16) * m_srvIncrement;
    return handle;
}

D3D12_CPU_DESCRIPTOR_HANDLE DX12Context::GetNullUav(uint32_t slot) const
{
    auto handle = m_nullSrvHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(48 + slot % 8) * m_srvIncrement;
    return handle;
}

bool DX12Context::UploadTexture2D(const uint8_t* rgba, uint32_t width, uint32_t height,
                                  Microsoft::WRL::ComPtr<ID3D12Resource>& texture)
{
    if (!rgba || width == 0 || height == 0 || !m_device || !m_commandQueue)
        return false;
    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC textureDesc{};
    textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    textureDesc.Width = width;
    textureDesc.Height = height;
    textureDesc.DepthOrArraySize = 1;
    textureDesc.MipLevels = 1;
    textureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    if (FAILED(m_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &textureDesc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture))))
        return false;

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rowCount = 0;
    UINT64 rowSize = 0;
    UINT64 uploadSize = 0;
    m_device->GetCopyableFootprints(&textureDesc, 0, 1, 0, &footprint, &rowCount, &rowSize, &uploadSize);
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
    auto* destination = static_cast<uint8_t*>(mapped) + footprint.Offset;
    const size_t sourcePitch = static_cast<size_t>(width) * 4;
    for (UINT row = 0; row < rowCount; ++row)
        std::memcpy(destination + static_cast<size_t>(row) * footprint.Footprint.RowPitch,
                    rgba + static_cast<size_t>(row) * sourcePitch, sourcePitch);
    upload->Unmap(0, nullptr);

    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)))
        || FAILED(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                              IID_PPV_ARGS(&list))))
        return false;
    D3D12_TEXTURE_COPY_LOCATION destinationLocation{};
    destinationLocation.pResource = texture.Get();
    destinationLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION sourceLocation{};
    sourceLocation.pResource = upload.Get();
    sourceLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    sourceLocation.PlacedFootprint = footprint;
    list->CopyTextureRegion(&destinationLocation, 0, 0, 0, &sourceLocation, nullptr);
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = texture.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    list->ResourceBarrier(1, &barrier);
    if (FAILED(list->Close()))
        return false;
    ID3D12CommandList* lists[] = {list.Get()};
    m_commandQueue->ExecuteCommandLists(1, lists);
    Flush();
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
    // WHY: GPU実行完了までUploadリソースをフレームスロットに保持する。
    m_transientUploads[m_frameIndex].push_back(std::move(upload));
    return true;
}

void DX12Context::DeferRelease(Microsoft::WRL::ComPtr<ID3D12Resource>& resource)
{
    if (!resource) return;
    // WHY: フレーム記録中は次にSignalされるFenceまでGPU参照が残る。
    //      フレーム外では最後にSignal済みのFenceを待てば安全に解放できる。
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
}

} // namespace fbzz::renderer
