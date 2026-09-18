/// @file    DX12Renderer.hpp
/// @brief   IRenderer の DirectX 12 実装と Phase 1 フレーム制御。
/// @author  Hasegawa Jin
/// @date    2026-07-15
#pragma once

#include <Engine/Renderer/IRenderer.hpp>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "DX12Context.hpp"
#include "DX12UploadArena.hpp"
#include "DX12PsoCache.hpp"
#include "DX12StateTracker.hpp"

namespace fbzz::renderer {

/// @brief DirectX 12 バックエンドの公開境界。
/// @note Phase 1 ではバックバッファ描画と同期を担当し、リソース描画は Phase 2 で追加する。
class DX12Renderer final : public IRenderer {
public:
    /// @note m_iblBaker は unique_ptr<DX12IblBaker> (前方宣言のみ) を持つため、デストラクターを
    ///       out-of-line 化する。省くと `make_unique<DX12Renderer>()` が不完全型のデリーターを
    ///       インスタンス化し "can't delete an incomplete type" で失敗する。定義は DX12Renderer.cpp。
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
    bool RenderDebugPreview(const DrawCall& call, ResourceHandle<RenderTargetTag> target,
                            ResourceManager& resources) override;
    void Dispatch(const ComputeCall& call, ResourceManager& resources) override;
    void BeginComputeBatch() override;
    void EndComputeBatch() override;
    void Resize(uint32_t width, uint32_t height) override;
    void SetVSync(bool enabled) override;
    [[nodiscard]] bool GetVSync() const override;
    void SetRenderTarget(ResourceHandle<RenderTargetTag> rt, ResourceManager& resources) override;
    void SetViewport(uint32_t x, uint32_t y, uint32_t width, uint32_t height) override;
    void SetRenderTargetFace(ResourceHandle<RenderTargetTag> rt, uint32_t face,
                             uint32_t mip, ResourceManager& resources) override;
    bool BakeSkyLight(ResourceHandle<RenderTargetTag>, ResourceManager&, uint32_t, uint32_t,
                      uint32_t, uint32_t, std::unique_ptr<ITexture>&,
                      std::unique_ptr<ITexture>&) override;
    /// @brief Editor の HDRI → DDS ベイクは DX12HdriBaker (IIblBaker 実装) を返す。
    /// @note BakeSkyLight (実行時畳み込み) とは別に、フレーム非依存で 4 DDS を焼く同期経路。
    std::unique_ptr<IIblBaker> CreateIblBaker() override;
    void GpuProfBeginFrame() override;
    void GpuProfEndFrame() override;
    void GpuProfBeginPass(const char* name) override;
    void GpuProfEndPass(const char* name) override;
    void GpuProfCollect() override;
    const std::vector<GpuPassProfile>& GpuProfGetResults() const override { return m_gpuResults; }
    /// AI 連携 (viewport.capture): Scene View RT を PNG バイト列へ読み戻す。
    bool CaptureRenderTargetToPng(ResourceHandle<RenderTargetTag> rt, ResourceManager& resources,
                                  std::vector<uint8_t>& outPng,
                                  uint32_t& outWidth, uint32_t& outHeight) override;
    /// AI 連携 (vfx.previewMetrics): 同じ RT を HDR 線形値のまま数値評価用に読み戻す。
    bool CaptureRenderTargetToLinearRGBA(ResourceHandle<RenderTargetTag> rt, ResourceManager& resources,
                                         std::vector<float>& outRgba,
                                         uint32_t& outWidth, uint32_t& outHeight) override;
    uint32_t GetWidth() const override { return m_context.GetWidth(); }
    uint32_t GetHeight() const override { return m_context.GetHeight(); }
    DX12Context& GetContext() { return m_context; }

private:
    bool PrepareShaderReload() override;
    std::unique_ptr<IBuffer> CreateNativeVertexBuffer(const void*, size_t, uint32_t) override;
    std::unique_ptr<IBuffer> CreateNativeGpuWritableVertexBuffer(size_t sizeBytes, uint32_t stride) override;
    std::unique_ptr<IBuffer> CreateNativeIndexBuffer(const void*, uint32_t) override;
    std::unique_ptr<IConstantBuffer> CreateNativeConstantBuffer(size_t) override;
    std::unique_ptr<IShader> CreateNativeShader(const std::string&) override;
    std::unique_ptr<ITexture> CreateNativeTexture(const std::string&) override;
    std::unique_ptr<ITexture> CreateNativeTextureFromData(const uint8_t*, uint32_t, uint32_t) override;
    std::unique_ptr<ITexture> CreateNativeTextureFromDataMips(const TextureMipData*, uint32_t) override;
    std::unique_ptr<ITexture> CreateNativeTexture3DFromData(const uint8_t*, uint32_t, uint32_t, uint32_t) override;
    std::unique_ptr<ITexture> CreateNativeTextureFromRenderTarget(IRenderTarget&, uint32_t, RenderTargetTextureKind) override;
    std::unique_ptr<IPipelineState> CreateNativePipelineState(const PipelineStateDesc&) override;
    std::unique_ptr<IRenderTarget> CreateNativeRenderTarget(uint32_t, uint32_t,
                                                            const RenderTargetDesc&) override;
    std::unique_ptr<IRenderTarget> CreateNativeCubemapRenderTarget(uint32_t, uint32_t) override;
    std::unique_ptr<ITexture> CreateNativeCubeTextureFromRenderTarget(IRenderTarget&) override;
    std::unique_ptr<ITexture> CreateNativeComputeTexture(uint32_t, uint32_t) override;
    std::unique_ptr<ITexture> CreateNativeComputeTexture3D(uint32_t, uint32_t, uint32_t) override;
    std::unique_ptr<ITexture> CreateNativeDynamicTexture(uint32_t width, uint32_t height,
                                                         DynamicTextureFormat format) override;
    std::unique_ptr<IStructuredBuffer> CreateNativeStructuredBuffer(const void*, uint32_t, uint32_t) override;
    std::unique_ptr<IStructuredBuffer> CreateNativeGpuLocalStructuredBuffer(
        const void*, uint32_t, uint32_t) override;
    std::unique_ptr<IStructuredBuffer> CreateNativeRWStructuredBuffer(const void*, uint32_t, uint32_t) override;

    DX12Context m_context;
    DX12UploadArena m_uploadArena;
    DX12PsoCache m_psoCache;
    DX12StateTracker m_stateTracker;
    /// 独立 Dispatch 群が書いた UAV を保持し、パス末尾の 1 回の ResourceBarrier へ集約する。
    std::vector<ID3D12Resource*> m_computeBatchWrittenResources;
    bool m_computeBatchActive = false;
    /// @brief 束縛中の RT をハンドルから引き直す。
    /// @return バックバッファ束縛中、または束縛した RT が解放済みなら nullptr。
    /// @note 生ポインタを持たないのは、RT が描画の途中でも解放されるため
    ///       (ビューポートのリサイズ、ViewRenderTargets の作り直し)。
    [[nodiscard]] class DX12RenderTarget* ResolveCurrentRenderTarget(ResourceManager* resources) const;
    /// @brief RT を束縛したのに、その RT が既に解放されているか。
    /// @note 描き先の書式も RTV も失われているので、Clear / Submit は何もしない。
    [[nodiscard]] bool IsCurrentRenderTargetLost(ResourceManager* resources) const;
    void ReportLostRenderTarget(const char* where);
    /// 束縛中の RT。無効ハンドルはバックバッファを表す。
    ResourceHandle<RenderTargetTag> m_currentRenderTargetHandle;
    bool m_reportedLostRenderTarget = false;
    std::unique_ptr<class DX12IblBaker> m_iblBaker;
    D3D12_CPU_DESCRIPTOR_HANDLE m_currentCubeRtv{};
    uint32_t m_currentCubeFace = 0;
    uint32_t m_currentCubeMip = 0;
    D3D12_VIEWPORT m_currentViewport{};
    D3D12_RECT m_currentScissor{};
    D3D12_GPU_VIRTUAL_ADDRESS m_nullConstantAddress = 0;
    /// 全スロットを INVALID_BINDLESS_INDEX で埋めた添字ブロック。フレーム頭に 1 個だけ確保する。
    /// @note アリーナ枯渇時に m_nullConstantAddress (ゼロ埋め) を差すと、添字 0 がヒープ先頭の
    ///       有効なディスクリプタと解釈されてしまうため、専用に持つ。
    D3D12_GPU_VIRTUAL_ADDRESS m_invalidBindlessAddress = 0;

    /// @note かつてここに «ピクセル/頂点 SRV テーブルのフレーム内キャッシュ» があった。
    ///       bindless 移行で 1 ドローあたりのディスクリプタコピーが無くなり、
    ///       «同じ束縛なら再利用する» という最適化そのものが不要になったため撤去した。
    ///       @see Docs/design/bindless.md

    /// 直前に root スロットへ束縛した CBV の GPU VA。変化したスロットだけ再設定するために持つ。
    /// @note 毎 Draw 全 14 スロットを null 埋め→実 CB で上書きすると最大 28 回の
    ///       SetGraphicsRootConstantBufferView が出るため、差分だけ設定してコストを落とす。
    /// @note グラフィクスとコンピュートは root signature が別物のため、Dispatch を挟んだら
    ///       必ず無効化する (InvalidateRootCbvCache)。
    std::array<D3D12_GPU_VIRTUAL_ADDRESS, 14> m_lastRootCbv{};
    bool m_rootCbvCacheValid = false;

    /// 冗長なパイプライン状態設定を弾くための直前値。
    /// @note m_lastGraphicsRootSignature は正しさのために必要 (再設定は全ルート引数を無効化し、
    ///       m_lastRootCbv の前提が崩れるため)。他の 2 つは記録コスト削減のみが目的。
    ID3D12RootSignature* m_lastGraphicsRootSignature = nullptr;
    ID3D12PipelineState* m_lastPipelineState = nullptr;
    ID3D12DescriptorHeap* m_lastDescriptorHeap = nullptr;
    /// 共有コマンドリストへ他所が記録したことを検知するための世代 (DX12Context 側が上げる)。
    uint64_t m_seenPipelineStateGeneration = 0;

    /// Compute へ切り替えるとグラフィクス側のルート束縛は当てにできなくなる。
    /// Dispatch / BeginFrame / コマンドリスト再取得のたびに呼ぶこと。
    void InvalidateRootCbvCache();

    /// 頂点バッファ実ストライドとリフレクション推定ストライドの不一致を
    /// シェーダーごとに一度だけ警告するための記録。
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
