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

/// @note DX12Renderer と各 DX12 サブシステムが共有する GPU 実行コンテキスト。リソースごとに Device や
/// @note Fence の所有権を持たせず、破棄順序と同期点を一箇所へ集約する。
class DX12Context final {
public:
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
    /// @note 転送する 1 段ぶん。rowPitch は rgba の 1 行のバイト数。
    struct TextureMip {
        const uint8_t* rgba = nullptr;
        uint32_t width = 0;
        uint32_t height = 0;
        std::size_t rowPitch = 0;
    };
    /// @note 0 段目から並べたミップの列を RGBA8 の 2D テクスチャとして同期転送する。
    bool UploadTexture2DMips(std::span<const TextureMip> mips, Microsoft::WRL::ComPtr<ID3D12Resource>& texture);
    bool UploadTexture2D(const uint8_t* rgba, uint32_t width, uint32_t height,
                         Microsoft::WRL::ComPtr<ID3D12Resource>& texture);
    /// @brief UploadTexture2DMips の待たない版。転送を投入して戻る。
    /// @param outFenceValue 転送完了で通過するフェンス値。0 なら投入時に完了済み。
    /// @note upload バッファとアロケーターはフェンス通過まで内部で保持し、CollectDeferredReleases で返す。
    /// @see https://learn.microsoft.com/en-us/windows/win32/direct3d12/fence-based-resource-management Fence-Based Resource Management
    bool UploadTexture2DMipsAsync(std::span<const TextureMip> mips,
                                  Microsoft::WRL::ComPtr<ID3D12Resource>& texture,
                                  uint64_t& outFenceValue);
    /// @brief フェンス値を GPU が通過したか。0 は常に true。CPU は待たない。
    /// @note kCopyQueueTokenBit の立った値はコピーキューのフェンスとして見る。
    [[nodiscard]] bool IsFenceComplete(uint64_t fenceValue) const;
    /// @note UploadTexture2DMipsAsync が返す値のうち、コピーキューで転送したことを表す印。
    static constexpr uint64_t kCopyQueueTokenBit = 1ull << 63;
    /// @brief 非同期転送に専用コピーキューを使うか。無効ならこれまでどおり描画キューで転送する。
    void SetUseCopyQueue(bool enabled) { m_useCopyQueue = enabled; }
    [[nodiscard]] bool UsesCopyQueue() const { return m_copyQueue && m_useCopyQueue; }

    /// @name 非同期コンピュート
    /// @see Docs/design/async-compute.md
    /// @{
    /// @brief コンピュートキューが使えるか。作れない機械や無効化した構成では false。
    [[nodiscard]] bool SupportsAsyncCompute() const { return m_computeQueue && m_useAsyncCompute; }
    void SetUseAsyncCompute(bool enabled) { m_useAsyncCompute = enabled; }

    /// @brief 記録中の描画リストをここで閉じて投入し、同じアロケーターで開き直す。
    /// @return 投入した仕事が終わると通過する描画フェンスの値。割れなければ 0。
    /// @pre フレームが開いていること。
    /// @warning Reset でパイプライン状態が全部落ちる。呼び出し側は描画先・ビューポート・
    /// @note          ルート引数の記憶を捨て直すこと (DX12Renderer::InvalidateRootCbvCache)。
    /// @note アロケーターはフレーム末まで Reset しない。投入済みリストが参照する記録メモリを
    /// @note       生かしたまま、同じアロケーターへ追記していく。
    uint64_t SplitGraphicsList();

    /// @brief コンピュートリストを開く。waitGraphicsFenceValue まで描画キューを待ってから走る。
    /// @return 記録先。使えないときは nullptr (呼び出し側は描画キューのまま続けてよい)。
    ID3D12GraphicsCommandList* BeginComputeList(uint64_t waitGraphicsFenceValue);

    /// @brief コンピュートリストを閉じて投入し、描画キューへ «通過するまで待て» を積む。
    /// @note 描画キューへの Wait はこの後に投入される描画にだけ効く。区間を抜けた後の描画が
    /// @note       結果を読むので、これで順序は足りる。
    void EndComputeList();
    [[nodiscard]] bool IsComputeListOpen() const { return m_computeListOpen; }
    [[nodiscard]] ID3D12GraphicsCommandList* GetComputeList() const { return m_computeList.Get(); }
    /// @}
    bool CreateDefaultBuffer(const void* data, size_t sizeBytes,
                             Microsoft::WRL::ComPtr<ID3D12Resource>& buffer);
    bool StageBufferCopy(ID3D12Resource* destination, const void* data, size_t sizeBytes);
    void DeferRelease(Microsoft::WRL::ComPtr<ID3D12Resource>& resource);
    uint32_t GetWidth() const { return m_width; }
    uint32_t GetHeight() const { return m_height; }
    uint32_t GetFrameIndex() const { return m_frameIndex; }
    uint32_t GetSrvDescriptorIncrement() const { return m_srvIncrement; }
    bool IsFrameOpen() const { return m_frameOpen; }

    /// @note Present の垂直同期。既定は無効 (フレームレート制御は Time::targetFps)。
    /// @note 有効時は SyncInterval=1 にし、tearing フラグは落とす (併用は DXGI が拒否する)。
    void SetVSync(bool enabled) { m_vsync = enabled; }
    bool GetVSync() const { return m_vsync; }

    /// @name 共有コマンドリストのパイプライン状態の世代
    /// @{
    /// @note フレーム用コマンドリストは DX12Renderer 以外 (ImGui/IblBaker) も記録に使い、そこで
    /// @note       ルートシグネチャ・PSO・ディスクリプタヒープを勝手に差し替える。DX12Renderer::Submit
    /// @note       は「直前に何を束縛したか」を覚えて冗長設定を弾くため、外部が状態を触ったらこの世代を
    /// @note       上げて知らせる契約にする (ルートシグネチャの再設定は全ルート引数を無効化するため、
    /// @note       黙って踏むと描画が壊れる)。
    void MarkPipelineStateDirty() { ++m_pipelineStateGeneration; }
    uint64_t GetPipelineStateGeneration() const { return m_pipelineStateGeneration; }
    uint64_t GetCompletedFenceValue() const { return m_fence ? m_fence->GetCompletedValue() : 0; }
    uint64_t GetFrameFenceValue(uint32_t index) const { return m_frames[index % FRAME_COUNT].fenceValue; }
    /// @note DXIL / DXR パス選択に使う実機 capability。シェーダーマクロだけで対応可否を決めない。
    D3D_SHADER_MODEL GetHighestShaderModel() const { return m_highestShaderModel; }
    D3D12_RAYTRACING_TIER GetRaytracingTier() const { return m_raytracingTier; }
    bool SupportsRaytracingPipeline() const {
        return m_raytracingTier >= D3D12_RAYTRACING_TIER_1_0;
    }
    bool SupportsInlineRaytracing() const {
        return m_highestShaderModel >= D3D_SHADER_MODEL_6_5
            && m_raytracingTier >= D3D12_RAYTRACING_TIER_1_1;
    }
    /// @brief シェーダーから ResourceDescriptorHeap を直接引けるか (bindless)。
    /// @note SM 6.6 と Resource Binding Tier 3 の両方が要る。どちらかが欠ける機械では
    /// @note       ルートシグネチャへ CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED を立てられないため、
    /// @note       ディスクリプタテーブル経路へ縮退する。
    /// @see  Docs/design/bindless.md
    bool SupportsBindless() const {
        return m_highestShaderModel >= D3D_SHADER_MODEL_6_6
            && m_resourceBindingTier >= D3D12_RESOURCE_BINDING_TIER_3;
    }
    /// @}

    /// @name 永続 bindless ディスクリプタ
    /// @{
    /// @note 動的リングと分ける理由: 動的リングはフレームごとに巻き戻る前提で、同じ添字が翌フレーム
    /// @note       には別のリソースを指す。bindless の添字はテクスチャの «識別子» としてシーンやマテリアル
    /// @note       側に載るため、リソースが生きている限り不変でなければならない。
    static constexpr uint32_t INVALID_BINDLESS_INDEX = 0xFFFFFFFFu;

    /// @brief 永続レンジから 1 枠確保する。返るのはヒープ先頭からの添字 (シェーダーが使う値)。
    /// @return 枯渇した場合は INVALID_BINDLESS_INDEX。呼び出し側はテーブル経路へ縮退すること。
    uint32_t AllocateBindlessSlot();

    /// @brief 枠を解放する。GPU が参照し終えるまで再利用しない。
    /// @note 現在記録中のコマンドリストがまだその添字を読む可能性があるため、フェンス通過まで
    /// @note       フリーリストへ戻さない。即時に戻すと、直後の確保が生きている描画のテクスチャを奪う。
    void FreeBindlessSlot(uint32_t index);

    /// @brief 永続レンジ内の CPU ハンドル。ここへ SRV を書き込むと添字で引けるようになる。
    D3D12_CPU_DESCRIPTOR_HANDLE GetBindlessCpu(uint32_t index) const;

    /// @brief 任意の CPU ディスクリプタを新しい枠へ複製し、その添字を返す。
    /// @return 枯渇していれば INVALID_BINDLESS_INDEX。
    /// @note リソース自身が添字を持てない一時ビュー (IBL ベイクの面ごと UAV など) 用。
    /// @note       呼び出し側が使い終わったら FreeBindlessSlot で返すこと。
    uint32_t PublishBindlessDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE source);

    uint32_t GetBindlessCapacity() const { return BINDLESS_DESCRIPTOR_CAPACITY; }
    uint32_t GetBindlessUsage() const { return m_bindlessNextSlot - static_cast<uint32_t>(m_bindlessFreeList.size()); }
    /// @}

private:
    static constexpr uint32_t IMGUI_DESCRIPTOR_CAPACITY = 4096;
    /// @note 永続 bindless レンジの枠数。1 テクスチャ = 1 枠で、シーン中の全テクスチャが同時に載る想定。
    /// @note 16384 の根拠: Stage_02 (最大シーン) の実測でテクスチャは 3 桁に収まる。Editor の
    /// @note       プレビューとフォントアトラスを足しても 1 桁余る規模を取り、枯渇時はテーブル経路へ縮退する。
    static constexpr uint32_t BINDLESS_DESCRIPTOR_CAPACITY = 16384;
    /// @note m_resourceSrvHeap は永続 bindless レンジだけを持つ。
    /// @note かつて先頭 56 枠の null ディスクリプタと、その後ろにフレームごとの動的リング
    /// @note (65536 x 2) が居たが、ディスクリプタテーブルの撤去でどちらも不要になった。
    /// @note 添字がそのままヒープ先頭からの位置になるので、base は 0。
    static constexpr uint32_t BINDLESS_HEAP_BASE = 0;
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

    struct DeferredResource {
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        uint64_t fenceValue = 0;
    };
    /// @note 投入済みで GPU がまだ読んでいるかもしれないテクスチャ転送 1 件。
    struct PendingUpload {
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
        Microsoft::WRL::ComPtr<ID3D12Resource> upload;
        /// @note 転送先。コピーキューが書き終えるまで実体を消させない。
        Microsoft::WRL::ComPtr<ID3D12Resource> destination;
        uint64_t fenceValue = 0;
        /// @note fenceValue がコピーキューのフェンスの値か。
        bool onCopyQueue = false;
    };
    /// @note コピーキューで書き終えた転送先に、描画キューで記録する COMMON → PIXEL_SHADER_RESOURCE の遷移。
    struct PostCopyBarrier {
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        uint64_t copyFenceValue = 0;
    };
    /// @brief 転送を記録してキューへ投入する。待機もフェンスの Signal もしない。
    /// @param onCopyQueue true ならコピーキューへ (遷移は記録しない)。コピーキューが無ければ失敗する。
    bool SubmitTexture2DUpload(std::span<const TextureMip> mips,
                               Microsoft::WRL::ComPtr<ID3D12Resource>& texture,
                               PendingUpload& outUpload, bool onCopyQueue);
    void FlushCopyQueue();
    /// @brief フレーム用コマンドリストの頭で、コピーを終えた転送先を PIXEL_SHADER_RESOURCE へ遷移させる。
    void RecordPostCopyBarriers();

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
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, BACK_BUFFER_COUNT> m_backBuffers;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_depthBuffer;
    std::array<FrameResource, FRAME_COUNT> m_frames;
    HANDLE m_fenceEvent = nullptr;
    uint64_t m_nextFenceValue = 1;
    uint32_t m_frameIndex = 0;
    uint32_t m_backBufferIndex = 0;
    uint32_t m_rtvIncrement = 0;
    uint32_t m_srvIncrement = 0;
    /// @note 永続 bindless レンジ。未使用の先頭は bump、返却は fence 通過後にフリーリストへ戻す。
    uint32_t m_bindlessNextSlot = 0;
    bool     m_bindlessExhaustionReported = false;
    std::vector<uint32_t> m_bindlessFreeList;
    struct PendingBindlessFree {
        uint32_t index = 0;
        uint64_t fenceValue = 0;
    };
    std::vector<PendingBindlessFree> m_bindlessPendingFrees;
    std::array<std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>, FRAME_COUNT> m_transientUploads;
    std::vector<DeferredResource> m_deferredResources;
    std::vector<PendingUpload> m_pendingUploads;
    /// @note 非同期テクスチャ転送専用のコピーキュー。作れない機械では null で、描画キューへ倒す。
    /// @see https://learn.microsoft.com/en-us/windows/win32/direct3d12/user-mode-heap-synchronization «Asynchronous compute and graphics example» (キュー間の同期)
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_copyQueue;
    Microsoft::WRL::ComPtr<ID3D12Fence> m_copyFence;
    uint64_t m_nextCopyFenceValue = 1;
    bool m_useCopyQueue = true;
    /// @note 画面空間 Compute をジオメトリ描画と重ねるためのキュー。作れない機械では null。
    /// @see Docs/design/async-compute.md
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_computeQueue;
    Microsoft::WRL::ComPtr<ID3D12Fence> m_computeFence;
    /// @note フレームごとのコンピュート用アロケーター。Reset は BeginFrame で 1 回だけ行う。
    /// @note 区間ごとに Reset すると、同じフレームで先に投入した区間の記録メモリを踏む。
    std::array<Microsoft::WRL::ComPtr<ID3D12CommandAllocator>, FRAME_COUNT> m_computeAllocators;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_computeList;
    uint64_t m_nextComputeFenceValue = 1;
    /// @note 機械が持っているか (作れたか + 強制停止していないか)。«使うか» は RenderSettings。
    bool m_useAsyncCompute = true;
    bool m_computeListOpen = false;
    std::vector<PostCopyBarrier> m_pendingPostCopyBarriers;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    bool m_allowTearing = false;
    bool m_vsync = false;
    bool m_frameOpen = false;
    uint64_t m_pipelineStateGeneration = 0;
    bool m_suspended = false;
    bool m_occluded = false;   ///< @note 直前の Present が DXGI_STATUS_OCCLUDED (ウィンドウ遮蔽) を返した
    bool m_resizePending = false;
    uint32_t m_pendingWidth = 0;
    uint32_t m_pendingHeight = 0;
    D3D_SHADER_MODEL m_highestShaderModel = D3D_SHADER_MODEL_5_1;
    D3D12_RAYTRACING_TIER m_raytracingTier = D3D12_RAYTRACING_TIER_NOT_SUPPORTED;
    D3D12_RESOURCE_BINDING_TIER m_resourceBindingTier = D3D12_RESOURCE_BINDING_TIER_1;
};

} /// @note namespace fbzz::renderer
