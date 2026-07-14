// FBZZ Engine
// DX12Renderer.hpp | fbzz::renderer
// IRenderer の DirectX 12 実装と Phase 1 フレーム制御
#pragma once

#include <Engine/Renderer/IRenderer.hpp>
#include <unordered_set>
#include "DX12Context.hpp"
#include "DX12UploadArena.hpp"
#include "DX12PsoCache.hpp"
#include "DX12StateTracker.hpp"

namespace fbzz::renderer {

// DirectX 12 バックエンドの公開境界。
// WHAT: Phase 1 ではバックバッファ描画と同期を担当し、リソース描画は Phase 2 で追加する。
class DX12Renderer final : public IRenderer {
public:
    // WHY: m_iblBaker が unique_ptr<DX12IblBaker>（本ヘッダーでは前方宣言のみ）を持つため、
    //      デストラクターを明示的に out-of-line 化する。これがないと RendererFactory.cpp の
    //      make_unique<DX12Renderer>() が不完全型 DX12IblBaker のデリーターをインスタンス化し、
    //      "can't delete an incomplete type" (<memory> の static_assert) で失敗する。
    //      定義は DX12IblBaker.hpp を include 済みの DX12Renderer.cpp 側に置く。
    DX12Renderer();
    ~DX12Renderer() override;

    const char* GetBackendName() const override { return "DirectX 12"; }
    bool Init(HWND hwnd, uint32_t width, uint32_t height);
    void Shutdown() override;
    void BeginFrame() override;
    void EndFrame() override;
    void Clear(const math::Vector4& color) override;
    void ClearDepth(float depth = 1.0f) override;
    void Submit(const DrawCall& call, ResourceManager& resources) override;
    void Dispatch(const ComputeCall& call, ResourceManager& resources) override;
    void Resize(uint32_t width, uint32_t height) override;
    void SetRenderTarget(ResourceHandle<RenderTargetTag> rt, ResourceManager& resources) override;
    void SetRenderTargetFace(ResourceHandle<RenderTargetTag> rt, uint32_t face,
                             uint32_t mip, ResourceManager& resources) override;
    void SetSampler(uint32_t slot, SamplerMode mode) override;
    bool BakeSkyLight(ResourceHandle<RenderTargetTag>, ResourceManager&, uint32_t, uint32_t,
                      uint32_t, uint32_t, std::unique_ptr<ITexture>&,
                      std::unique_ptr<ITexture>&) override;
    // Editor の HDRI → DDS ベイクは DX12HdriBaker (IIblBaker 実装) を返す。
    // WHY: BakeSkyLight (実行時畳み込み) とは別に、フレーム非依存で 4 DDS を焼く同期経路。
    std::unique_ptr<IIblBaker> CreateIblBaker() override;
    void GpuProfBeginFrame() override;
    void GpuProfEndFrame() override;
    void GpuProfBeginPass(const char* name) override;
    void GpuProfEndPass(const char* name) override;
    void GpuProfCollect() override;
    const std::vector<GpuPassProfile>& GpuProfGetResults() const override { return m_gpuResults; }
    uint32_t GetWidth() const override { return m_context.GetWidth(); }
    uint32_t GetHeight() const override { return m_context.GetHeight(); }
    DX12Context& GetContext() { return m_context; }

private:
    std::unique_ptr<IBuffer> CreateNativeVertexBuffer(const void*, size_t, uint32_t) override;
    std::unique_ptr<IBuffer> CreateNativeIndexBuffer(const void*, uint32_t) override;
    std::unique_ptr<IConstantBuffer> CreateNativeConstantBuffer(size_t) override;
    std::unique_ptr<IShader> CreateNativeShader(const std::string&) override;
    std::unique_ptr<ITexture> CreateNativeTexture(const std::string&) override;
    std::unique_ptr<ITexture> CreateNativeTextureFromData(const uint8_t*, uint32_t, uint32_t) override;
    std::unique_ptr<ITexture> CreateNativeTexture3DFromData(const uint8_t*, uint32_t, uint32_t, uint32_t) override;
    std::unique_ptr<ITexture> CreateNativeTextureFromRenderTarget(IRenderTarget&, uint32_t, RenderTargetTextureKind) override;
    std::unique_ptr<IPipelineState> CreateNativePipelineState(const PipelineStateDesc&) override;
    std::unique_ptr<IRenderTarget> CreateNativeRenderTarget(uint32_t, uint32_t, uint32_t) override;
    std::unique_ptr<IRenderTarget> CreateNativeCubemapRenderTarget(uint32_t, uint32_t) override;
    std::unique_ptr<ITexture> CreateNativeCubeTextureFromRenderTarget(IRenderTarget&) override;
    std::unique_ptr<ITexture> CreateNativeComputeTexture(uint32_t, uint32_t) override;
    std::unique_ptr<IStructuredBuffer> CreateNativeStructuredBuffer(const void*, uint32_t, uint32_t) override;
    std::unique_ptr<IStructuredBuffer> CreateNativeRWStructuredBuffer(const void*, uint32_t, uint32_t) override;

    DX12Context m_context;
    DX12UploadArena m_uploadArena;
    DX12PsoCache m_psoCache;
    DX12StateTracker m_stateTracker;
    class DX12RenderTarget* m_currentRenderTarget = nullptr;
    std::unique_ptr<class DX12IblBaker> m_iblBaker;
    D3D12_CPU_DESCRIPTOR_HANDLE m_currentCubeRtv{};
    D3D12_GPU_VIRTUAL_ADDRESS m_nullConstantAddress = 0;

    // WHY: 連続する Draw が同じテクスチャ/バッファ集合を束縛する場合 (同一マテリアルのバッチ等)、
    //      shader-visible リングへの CopyDescriptorsSimple を毎 Draw 発行するのは無駄。
    //      直前 Draw の束縛シグネチャと GPU テーブルをキャッシュし、一致すればコピーを丸ごと省略する。
    //      リングはフレームごとに巻き戻る (BeginFrame でオフセットリセット) ため、フレームを跨いだ
    //      再利用は不可 — BeginFrame で必ず無効化する。
    std::array<ResourceHandle<TextureTag>, 32> m_lastPixelTextures{};
    D3D12_GPU_DESCRIPTOR_HANDLE m_lastPixelTableGpu{};
    bool m_lastPixelTableValid = false;
    std::array<ResourceHandle<StructuredBufferTag>, 3> m_lastVertexBuffers{};
    D3D12_GPU_DESCRIPTOR_HANDLE m_lastVertexTableGpu{};
    bool m_lastVertexTableValid = false;

    // 頂点バッファ実ストライドとリフレクション推定ストライドの不一致を
    // シェーダーごとに一度だけ警告するための記録。
    std::unordered_set<const void*> m_strideWarned;
    bool m_reportedMissingDrawResource = false;

    static constexpr uint32_t GPU_MAX_PASSES = 32;
    struct GpuQueryFrame {
        std::array<std::array<char, 64>, GPU_MAX_PASSES> names{};
        uint32_t count = 0;
        bool recording = false;
        bool pending = false;
    };
    bool InitializeGpuProfiler();
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> m_gpuQueryHeap;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_gpuReadback;
    uint64_t* m_gpuMappedTimestamps = nullptr;
    uint64_t m_gpuTimestampFrequency = 0;
    std::array<GpuQueryFrame, DX12Context::FRAME_COUNT> m_gpuQueryFrames;
    uint32_t m_gpuProfilerFrame = 0;
    std::vector<GpuPassProfile> m_gpuResults;
};

} // namespace fbzz::renderer
