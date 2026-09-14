/// @file    DX12Context.hpp
/// @brief   DirectX 12 のデバイス・キュー・フレーム同期を共有管理する基盤。
/// @author  Hasegawa Jin
/// @date    2026-07-15
#pragma once

#include <span>

#include <array>
#include <cstdint>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <vector>

namespace fbzz::renderer {

// DX12Renderer と各 DX12 サブシステムが共有する GPU 実行コンテキスト。
// WHY: リソースごとに Device や Fence の所有権を持たせず、破棄順序と同期点を一箇所へ集約する。
class DX12Context final {
public:
    struct DescriptorTableAllocation {
        D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
        D3D12_GPU_DESCRIPTOR_HANDLE gpu{};
        explicit operator bool() const { return cpu.ptr != 0; }
    };
    static constexpr uint32_t FRAME_COUNT = 2;
    static constexpr uint32_t BACK_BUFFER_COUNT = 3;
    static constexpr DXGI_FORMAT BACK_BUFFER_FORMAT = DXGI_FORMAT_R8G8B8A8_UNORM;
    static constexpr DXGI_FORMAT DEPTH_FORMAT = DXGI_FORMAT_D32_FLOAT;

    ~DX12Context();

    bool Initialize(HWND hwnd, uint32_t width, uint32_t height);
    void Shutdown();
    bool BeginFrame();
    void EndFrame();
    void Resize(uint32_t width, uint32_t height);
    void Flush();

    ID3D12Device* GetDevice() const { return m_device.Get(); }
    ID3D12CommandQueue* GetCommandQueue() const { return m_commandQueue.Get(); }
    ID3D12GraphicsCommandList* GetCommandList() const { return m_commandList.Get(); }
    ID3D12DescriptorHeap* GetImGuiSrvHeap() const { return m_imguiSrvHeap.Get(); }
    ID3D12DescriptorHeap* GetResourceSrvHeap() const { return m_resourceSrvHeap.Get(); }
    D3D12_CPU_DESCRIPTOR_HANDLE GetCurrentRtv() const;
    D3D12_CPU_DESCRIPTOR_HANDLE GetDsv() const;
    D3D12_CPU_DESCRIPTOR_HANDLE GetImGuiSrvCpu() const;
    D3D12_GPU_DESCRIPTOR_HANDLE GetImGuiSrvGpu() const;
    D3D12_CPU_DESCRIPTOR_HANDLE GetImGuiSrvCpu(uint32_t index) const;
    D3D12_GPU_DESCRIPTOR_HANDLE GetImGuiSrvGpu(uint32_t index) const;
    uint32_t GetImGuiDescriptorCapacity() const { return IMGUI_DESCRIPTOR_CAPACITY; }
    D3D12_GPU_DESCRIPTOR_HANDLE GetNullPixelSrvTable() const;
    D3D12_GPU_DESCRIPTOR_HANDLE GetNullVertexSrvTable() const;
    D3D12_CPU_DESCRIPTOR_HANDLE GetNullPixelSrv(uint32_t slot) const;
    DescriptorTableAllocation AllocatePixelSrvTable();
    DescriptorTableAllocation AllocateVertexSrvTable();
    DescriptorTableAllocation AllocateUavTable();
    D3D12_CPU_DESCRIPTOR_HANDLE GetNullBufferSrv(uint32_t slot) const;
    D3D12_CPU_DESCRIPTOR_HANDLE GetNullUav(uint32_t slot) const;
    /// 転送する 1 段ぶん。rowPitch は rgba の 1 行のバイト数。
    struct TextureMip {
        const uint8_t* rgba = nullptr;
        uint32_t width = 0;
        uint32_t height = 0;
        std::size_t rowPitch = 0;
    };
    /// 0 段目から並べたミップの列を RGBA8 の 2D テクスチャとして同期転送する。
    bool UploadTexture2DMips(std::span<const TextureMip> mips, Microsoft::WRL::ComPtr<ID3D12Resource>& texture);
    bool UploadTexture2D(const uint8_t* rgba, uint32_t width, uint32_t height,
                         Microsoft::WRL::ComPtr<ID3D12Resource>& texture);
    bool CreateDefaultBuffer(const void* data, size_t sizeBytes,
                             Microsoft::WRL::ComPtr<ID3D12Resource>& buffer);
    bool StageBufferCopy(ID3D12Resource* destination, const void* data, size_t sizeBytes);
    void DeferRelease(Microsoft::WRL::ComPtr<ID3D12Resource>& resource);
    uint32_t GetWidth() const { return m_width; }
    uint32_t GetHeight() const { return m_height; }
    uint32_t GetFrameIndex() const { return m_frameIndex; }
    uint32_t GetSrvDescriptorIncrement() const { return m_srvIncrement; }
    uint32_t GetDynamicSrvUsage() const { return m_dynamicSrvOffsets[m_frameIndex]; }
    uint32_t GetDynamicSrvCapacity() const { return DYNAMIC_DESCRIPTORS_PER_FRAME; }
    bool IsFrameOpen() const { return m_frameOpen; }

    // Present の垂直同期。既定は無効 (フレームレート制御は Time::targetFps)。
    // 有効時は SyncInterval=1 にし、tearing フラグは落とす (併用は DXGI が拒否する)。
    void SetVSync(bool enabled) { m_vsync = enabled; }
    bool GetVSync() const { return m_vsync; }

    // ---- 共有コマンドリストのパイプライン状態の世代 ----
    // WHY: フレーム用コマンドリストは DX12Renderer 以外 (ImGui / IblBaker) も記録に使い、
    //      そこでルートシグネチャ・PSO・ディスクリプタヒープを勝手に差し替える。
    //      DX12Renderer::Submit は冗長設定を弾くために「直前に何を束縛したか」を覚えているので、
    //      外部が状態を触ったらこの世代を上げて知らせる契約にする。
    //      (ルートシグネチャの再設定は全ルート引数を無効化するため、黙って踏むと描画が壊れる)
    void MarkPipelineStateDirty() { ++m_pipelineStateGeneration; }
    uint64_t GetPipelineStateGeneration() const { return m_pipelineStateGeneration; }
    uint64_t GetCompletedFenceValue() const { return m_fence ? m_fence->GetCompletedValue() : 0; }
    uint64_t GetFrameFenceValue(uint32_t index) const { return m_frames[index % FRAME_COUNT].fenceValue; }
    // DXIL / DXR パス選択に使う実機 capability。シェーダーマクロだけで対応可否を決めない。
    D3D_SHADER_MODEL GetHighestShaderModel() const { return m_highestShaderModel; }
    D3D12_RAYTRACING_TIER GetRaytracingTier() const { return m_raytracingTier; }
    bool SupportsRaytracingPipeline() const {
        return m_raytracingTier >= D3D12_RAYTRACING_TIER_1_0;
    }
    bool SupportsInlineRaytracing() const {
        return m_highestShaderModel >= D3D_SHADER_MODEL_6_5
            && m_raytracingTier >= D3D12_RAYTRACING_TIER_1_1;
    }

private:
    static constexpr uint32_t IMGUI_DESCRIPTOR_CAPACITY = 4096;
    static constexpr uint32_t NULL_DESCRIPTOR_COUNT = 56;
    // Editor描画は多数のDrawを発行するため、1フレームで約1,000 Drawを収容できる容量を確保する。
    static constexpr uint32_t DYNAMIC_DESCRIPTORS_PER_FRAME = 65536;
    struct FrameResource {
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
        uint64_t fenceValue = 0;
    };

    bool CreateFactoryAndDevice(HWND hwnd);
    bool CreateSwapChain(HWND hwnd);
    bool CreateDescriptorHeaps();
    bool CreateBackBuffers();
    bool CreateDepthBuffer();
    bool CreateCommandsAndFence();
    bool ApplyResize(uint32_t width, uint32_t height);
    void WaitForFence(uint64_t value);
    void TransitionBackBuffer(D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after);
    void CollectDeferredReleases();
    DescriptorTableAllocation AllocateSrvTable(uint32_t descriptorCount);

    struct DeferredResource {
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        uint64_t fenceValue = 0;
    };

    Microsoft::WRL::ComPtr<IDXGIFactory6> m_factory;
    Microsoft::WRL::ComPtr<ID3D12Device> m_device;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_commandQueue;
    Microsoft::WRL::ComPtr<IDXGISwapChain3> m_swapChain;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_commandList;
    Microsoft::WRL::ComPtr<ID3D12Fence> m_fence;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_dsvHeap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_imguiSrvHeap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_resourceSrvHeap;
    // Null SRV/UAVのコピー元。shader-visibleヒープはコピー元にできないためCPU stagingを分離する。
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_nullSrvHeap;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, BACK_BUFFER_COUNT> m_backBuffers;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_depthBuffer;
    std::array<FrameResource, FRAME_COUNT> m_frames;
    HANDLE m_fenceEvent = nullptr;
    uint64_t m_nextFenceValue = 1;
    uint32_t m_frameIndex = 0;
    uint32_t m_backBufferIndex = 0;
    uint32_t m_rtvIncrement = 0;
    uint32_t m_srvIncrement = 0;
    std::array<uint32_t, FRAME_COUNT> m_dynamicSrvOffsets{};
    std::array<bool, FRAME_COUNT> m_srvHeapExhaustionReported{};
    std::array<std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>, FRAME_COUNT> m_transientUploads;
    std::vector<DeferredResource> m_deferredResources;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    bool m_allowTearing = false;
    bool m_vsync = false;
    bool m_frameOpen = false;
    uint64_t m_pipelineStateGeneration = 0;
    bool m_suspended = false;
    bool m_occluded = false;   // 直前の Present が DXGI_STATUS_OCCLUDED (ウィンドウ遮蔽) を返した
    bool m_resizePending = false;
    uint32_t m_pendingWidth = 0;
    uint32_t m_pendingHeight = 0;
    D3D_SHADER_MODEL m_highestShaderModel = D3D_SHADER_MODEL_5_1;
    D3D12_RAYTRACING_TIER m_raytracingTier = D3D12_RAYTRACING_TIER_NOT_SUPPORTED;
};

} // namespace fbzz::renderer
