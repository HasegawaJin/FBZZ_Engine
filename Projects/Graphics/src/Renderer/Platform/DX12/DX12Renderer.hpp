/// @file    DX12Renderer.hpp
/// @brief   IRenderer の DirectX 12 実装と Phase 1 フレーム制御。
/// @author  Hasegawa Jin
/// @date    2026-07-15
#pragma once
#include <Graphics/Api.hpp>

#include <Graphics/Renderer/IRenderer.hpp>
#include <Graphics/Renderer/GpuProfilerLedger.hpp>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <string>
#include "DX12Context.hpp"
#include "DX12UploadArena.hpp"
#include "DX12PsoCache.hpp"
#include "DX12StateTracker.hpp"
#include "DX12PreparedSkinning.hpp"

namespace fbzz::renderer {

/// @brief DirectX 12 バックエンドの公開境界。
/// @note Phase 1 ではバックバッファ描画と同期を担当し、リソース描画は Phase 2 で追加する。
class DX12Renderer final : public IRenderer {
public:
    /// @note m_iblBaker は unique_ptr<DX12IblBaker> (前方宣言のみ) を持つため、デストラクターを
    /// @note       out-of-line 化する。省くと `make_unique<DX12Renderer>()` が不完全型のデリーターを
    /// @note       インスタンス化し "can't delete an incomplete type" で失敗する。定義は DX12Renderer.cpp。
    FBZZ_GRAPHICS_API DX12Renderer();
    FBZZ_GRAPHICS_API ~DX12Renderer() override;

    const char* GetBackendName() const override { return "DirectX 12"; }
    bool SupportsIndirectDraw() const override { return m_context.GetDrawIndexedCommandSignature() != nullptr; }
    bool SupportsMeshShaders() const override { return m_context.SupportsMeshShaders(); }
    void SetCpuExecutor(IRenderer* executor) override { m_context.SetCpuExecutor(executor); }
    [[nodiscard]] FBZZ_GRAPHICS_API GraphicsCapabilities GetCapabilities() const override;
    FBZZ_GRAPHICS_API bool Init(HWND hwnd, uint32_t width, uint32_t height);
    FBZZ_GRAPHICS_API void Shutdown() override;
    FBZZ_GRAPHICS_API void BeginFrame() override;
    FBZZ_GRAPHICS_API void EndFrame() override;
    void SetRenderWhenOccluded(bool enabled) override { m_context.SetRenderWhenOccluded(enabled); }
    FBZZ_GRAPHICS_API void Clear(const math::Vector4& color) override;
    FBZZ_GRAPHICS_API void ClearDepth() override;
    FBZZ_GRAPHICS_API void ClearDepthRegion(uint32_t x, uint32_t y, uint32_t width, uint32_t height) override;
    FBZZ_GRAPHICS_API void Submit(const DrawCall& call, ResourceManager& resources) override;
    FBZZ_GRAPHICS_API bool RenderDebugPreview(const DrawCall& call, ResourceHandle<RenderTargetTag> target,
                            ResourceManager& resources) override;
    FBZZ_GRAPHICS_API void Dispatch(const ComputeCall& call, ResourceManager& resources) override;
    FBZZ_GRAPHICS_API bool TryDispatch(const ComputeCall& call, ResourceManager& resources) override;
    FBZZ_GRAPHICS_API bool BuildAccelerationStructure(ResourceHandle<AccelerationStructureTag>, ResourceManager&) override;
    FBZZ_GRAPHICS_API void BeginComputeBatch() override;
    FBZZ_GRAPHICS_API void EndComputeBatch() override;
    FBZZ_GRAPHICS_API bool BeginAsyncCompute(ResourceManager& resources) override;
    FBZZ_GRAPHICS_API void EndAsyncCompute() override;
    FBZZ_GRAPHICS_API void Resize(uint32_t width, uint32_t height) override;
    FBZZ_GRAPHICS_API void SetVSync(bool enabled) override;
    [[nodiscard]] FBZZ_GRAPHICS_API bool GetVSync() const override;
    FBZZ_GRAPHICS_API void SetRenderTarget(ResourceHandle<RenderTargetTag> rt, ResourceManager& resources) override;
    FBZZ_GRAPHICS_API bool SetRenderTargetColors(std::span<const RenderTargetColorAttachment> colors, ResourceManager& resources) override;
    FBZZ_GRAPHICS_API void SetViewport(uint32_t x, uint32_t y, uint32_t width, uint32_t height) override;
    FBZZ_GRAPHICS_API void SetRenderTargetFace(ResourceHandle<RenderTargetTag> rt, uint32_t face,
                             uint32_t mip, ResourceManager& resources) override;
    FBZZ_GRAPHICS_API bool BakeSkyLight(ResourceHandle<RenderTargetTag>, ResourceManager&, uint32_t, uint32_t,
                      uint32_t, uint32_t, std::unique_ptr<ITexture>&,
                      std::unique_ptr<ITexture>&) override;
    /// @brief Editor の HDRI → DDS ベイクは DX12HdriBaker (IIblBaker 実装) を返す。
    /// @note BakeSkyLight (実行時畳み込み) とは別に、フレーム非依存で 4 DDS を焼く同期経路。
    FBZZ_GRAPHICS_API std::unique_ptr<IIblBaker> CreateIblBaker() override;
    FBZZ_GRAPHICS_API void GpuProfBeginFrame() override;
    FBZZ_GRAPHICS_API void GpuProfEndFrame() override;
    FBZZ_GRAPHICS_API void GpuProfBeginPass(const char* name) override;
    FBZZ_GRAPHICS_API void GpuProfEndPass(const char* name) override;
    FBZZ_GRAPHICS_API void GpuProfCollect() override;
    const std::vector<GpuPassProfile>& GpuProfGetResults() const override { return m_gpuProfilerLedger.GetSnapshot().passes; }
    FBZZ_GRAPHICS_API bool GpuProfBeginView(const GpuProfilerViewMetadata& metadata) override;
    FBZZ_GRAPHICS_API void GpuProfEndView() override;
    const GpuProfilerSnapshot& GpuProfGetSnapshot() const override { return m_gpuProfilerLedger.GetSnapshot(); }
    /// @note AI 連携 (viewport.capture): Scene View RT を PNG バイト列へ読み戻す。
    FBZZ_GRAPHICS_API bool CaptureRenderTargetToPng(ResourceHandle<RenderTargetTag> rt, ResourceManager& resources,
                                  std::vector<uint8_t>& outPng,
                                  uint32_t& outWidth, uint32_t& outHeight) override;
    /// @note AI 連携 (vfx.previewMetrics): 同じ RT を HDR 線形値のまま数値評価用に読み戻す。
    FBZZ_GRAPHICS_API bool CaptureRenderTargetToLinearRGBA(ResourceHandle<RenderTargetTag> rt, ResourceManager& resources,
                                         std::vector<float>& outRgba,
                                         uint32_t& outWidth, uint32_t& outHeight) override;
    uint32_t GetWidth() const override { return m_context.GetWidth(); }
    uint32_t GetHeight() const override { return m_context.GetHeight(); }
    DX12Context& GetContext() { return m_context; }

private:
    FBZZ_GRAPHICS_API static std::unique_ptr<prepared_skinning_detail::NativeWork> PrepareCapturedNative(
        IRenderer& native, ResourceManager& resources, ResourceHandle<ShaderTag> shader,
        std::span<const CapturedSkinningRequest> requests, std::span<const math::Matrix4> matrices);
    std::shared_ptr<class DX12PreparedSkinningPool> m_preparedSkinningPool;
    FBZZ_GRAPHICS_API bool PrepareShaderReload() override;
    FBZZ_GRAPHICS_API std::unique_ptr<IAccelerationStructure> CreateNativeAccelerationStructure(
        const AccelerationStructureDesc&, ResourceManager&) override;
    FBZZ_GRAPHICS_API std::unique_ptr<IBuffer> CreateNativeVertexBuffer(const void*, size_t, uint32_t) override;
    FBZZ_GRAPHICS_API std::unique_ptr<IBuffer> CreateNativeGpuWritableVertexBuffer(size_t sizeBytes, uint32_t stride) override;
    FBZZ_GRAPHICS_API std::unique_ptr<IBuffer> CreateNativeIndexBuffer(const void*, uint32_t) override;
    FBZZ_GRAPHICS_API std::unique_ptr<IConstantBuffer> CreateNativeConstantBuffer(size_t) override;
    FBZZ_GRAPHICS_API std::unique_ptr<IShader> CreateNativeShader(const std::string&) override;
    FBZZ_GRAPHICS_API std::unique_ptr<ITexture> CreateNativeTexture(const std::string&) override;
    FBZZ_GRAPHICS_API std::unique_ptr<ITexture> CreateNativeTextureFromData(const uint8_t*, uint32_t, uint32_t) override;
    FBZZ_GRAPHICS_API std::unique_ptr<ITexture> CreateNativeTextureFromDataMips(const TextureMipData*, uint32_t) override;
    FBZZ_GRAPHICS_API std::unique_ptr<ITexture> CreateNativeTextureFromDataMipsAsync(const TextureMipData*, uint32_t,
                                                                   uint64_t& outUploadToken) override;
    FBZZ_GRAPHICS_API bool IsUploadComplete(uint64_t uploadToken) const override;
    FBZZ_GRAPHICS_API std::unique_ptr<ITexture> CreateNativeTexture3DFromData(const uint8_t*, uint32_t, uint32_t, uint32_t) override;
    FBZZ_GRAPHICS_API std::unique_ptr<ITexture> CreateNativeTextureFromRenderTarget(IRenderTarget&, uint32_t, RenderTargetTextureKind) override;
    FBZZ_GRAPHICS_API std::unique_ptr<IPipelineState> CreateNativePipelineState(const PipelineStateDesc&) override;
    FBZZ_GRAPHICS_API std::unique_ptr<IRenderTarget> CreateNativeRenderTarget(uint32_t, uint32_t,
                                                            const RenderTargetDesc&) override;
    FBZZ_GRAPHICS_API std::unique_ptr<IRenderTarget> CreateNativeCubemapRenderTarget(uint32_t, uint32_t) override;
    FBZZ_GRAPHICS_API std::unique_ptr<ITexture> CreateNativeCubeTextureFromRenderTarget(IRenderTarget&) override;
    FBZZ_GRAPHICS_API std::unique_ptr<ITexture> CreateNativeComputeTexture(uint32_t, uint32_t) override;
    FBZZ_GRAPHICS_API std::unique_ptr<ITexture> CreateNativeComputeTexture3D(uint32_t, uint32_t, uint32_t) override;
    FBZZ_GRAPHICS_API std::unique_ptr<ITexture> CreateNativeDynamicTexture(uint32_t width, uint32_t height,
                                                         DynamicTextureFormat format) override;
    FBZZ_GRAPHICS_API std::unique_ptr<IStructuredBuffer> CreateNativeStructuredBuffer(const void*, uint32_t, uint32_t) override;
    FBZZ_GRAPHICS_API std::unique_ptr<IStructuredBuffer> CreateNativeGpuLocalStructuredBuffer(
        const void*, uint32_t, uint32_t) override;
    FBZZ_GRAPHICS_API std::unique_ptr<IStructuredBuffer> CreateNativeRWStructuredBuffer(const void*, uint32_t, uint32_t) override;

    DX12Context m_context;
    DX12UploadArena m_uploadArena;
    DX12PsoCache m_psoCache;
    DX12StateTracker m_stateTracker;
    /// @note 独立 Dispatch 群が書いた UAV を保持し、パス末尾の 1 回の ResourceBarrier へ集約する。
    std::vector<ID3D12Resource*> m_computeBatchWrittenResources;
    bool m_computeBatchActive = false;
    /// @note 非同期コンピュート区間の記録中か。true の間、Dispatch はコンピュートリストへ記録する。
    /// @see Docs/design/async-compute.md
    bool m_asyncComputeActive = false;
    /// @brief いま記録すべきコマンドリスト。非同期区間ではコンピュートリストを返す。
    /// @note Submit / SetRenderTarget は常に描画リストを使う。区間の中で描画は呼ばない契約。
    [[nodiscard]] FBZZ_GRAPHICS_API ID3D12GraphicsCommandList* RecordingList() const;
    /// @brief 列を割った後の描画リストへ、描画先・ビューポート・その状態を張り直す。
    /// @note Reset で束縛が全部落ちるうえ、区間へ入る前に束縛中の RT も COMMON へ落としてある。
    /// @note       呼び出し側が SetRenderTarget を呼び直すとは限らないので、ここで揃える。
    FBZZ_GRAPHICS_API void RestoreGraphicsTargetState(ResourceManager& resources);
    /// @brief 束縛中の RT をハンドルから引き直す。
    /// @return バックバッファ束縛中、または束縛した RT が解放済みなら nullptr。
    /// @note 生ポインタを持たないのは、RT が描画の途中でも解放されるため
    /// @note       (ビューポートのリサイズ、ViewRenderTargets の作り直し)。
    [[nodiscard]] FBZZ_GRAPHICS_API class DX12RenderTarget* ResolveCurrentRenderTarget(ResourceManager* resources = nullptr) const;
    [[nodiscard]] FBZZ_GRAPHICS_API class DX12RenderTarget* ResolveCurrentColorAttachment(uint32_t index, ResourceManager* resources = nullptr) const;
    FBZZ_GRAPHICS_API void TransitionCurrentTargetForRead();
    /// @brief RT を束縛したのに、その RT が既に解放されているか。
    /// @note 描き先の書式も RTV も失われているので、Clear / Submit は何もしない。
    [[nodiscard]] FBZZ_GRAPHICS_API bool IsCurrentRenderTargetLost(ResourceManager* resources = nullptr) const;
    FBZZ_GRAPHICS_API void ReportLostRenderTarget(const char* where);
    /// @note 束縛中の RT。無効ハンドルはバックバッファを表す。
    ResourceHandle<RenderTargetTag> m_currentRenderTargetHandle;
    /// @note ハンドルの数値は Manager 間で重複する。生ポインタを保存せず、生存台帳へ所有者 ID を照会する。
    uint64_t m_currentRenderTargetOwner = 0;
    std::array<RenderTargetColorAttachment, 8> m_currentColorAttachments{};
    uint32_t m_currentColorAttachmentCount = 0;
    bool m_reportedLostRenderTarget = false;
    std::unique_ptr<class DX12IblBaker> m_iblBaker;
    D3D12_CPU_DESCRIPTOR_HANDLE m_currentCubeRtv{};
    uint32_t m_currentCubeFace = 0;
    uint32_t m_currentCubeMip = 0;
    D3D12_VIEWPORT m_currentViewport{};
    D3D12_RECT m_currentScissor{};
    D3D12_GPU_VIRTUAL_ADDRESS m_nullConstantAddress = 0;
    /// @note 全スロットを INVALID_BINDLESS_INDEX で埋めた添字ブロック。フレーム頭に 1 個だけ確保する。
    /// @note アリーナ枯渇時に m_nullConstantAddress (ゼロ埋め) を差すと、添字 0 がヒープ先頭の
    /// @note       有効なディスクリプタと解釈されてしまうため、専用に持つ。
    D3D12_GPU_VIRTUAL_ADDRESS m_invalidBindlessAddress = 0;

    /// @note かつてここに «ピクセル/頂点 SRV テーブルのフレーム内キャッシュ» があった。
    /// @note       bindless 移行で 1 ドローあたりのディスクリプタコピーが無くなり、
    /// @note       «同じ束縛なら再利用する» という最適化そのものが不要になったため撤去した。
    /// @note       @see Docs/design/bindless.md

    /// @note 直前に root スロットへ束縛した CBV の GPU VA。変化したスロットだけ再設定するために持つ。
    /// @note 毎 Draw 全 14 スロットを null 埋め→実 CB で上書きすると最大 28 回の
    /// @note       SetGraphicsRootConstantBufferView が出るため、差分だけ設定してコストを落とす。
    /// @note グラフィクスとコンピュートは root signature が別物のため、Dispatch を挟んだら
    /// @note       必ず無効化する (InvalidateRootCbvCache)。
    std::array<D3D12_GPU_VIRTUAL_ADDRESS, 14> m_lastRootCbv{};
    bool m_rootCbvCacheValid = false;

    /// @note 冗長なパイプライン状態設定を弾くための直前値。
    /// @note m_lastGraphicsRootSignature は正しさのために必要 (再設定は全ルート引数を無効化し、
    /// @note       m_lastRootCbv の前提が崩れるため)。他の 2 つは記録コスト削減のみが目的。
    ID3D12RootSignature* m_lastGraphicsRootSignature = nullptr;
    ID3D12PipelineState* m_lastPipelineState = nullptr;
    ID3D12DescriptorHeap* m_lastDescriptorHeap = nullptr;
    /// @note 共有コマンドリストへ他所が記録したことを検知するための世代 (DX12Context 側が上げる)。
    uint64_t m_seenPipelineStateGeneration = 0;

    /// @note Compute へ切り替えるとグラフィクス側のルート束縛は当てにできなくなる。
    /// @note Dispatch / BeginFrame / コマンドリスト再取得のたびに呼ぶこと。
    FBZZ_GRAPHICS_API void InvalidateRootCbvCache();

    /// @note 頂点バッファ実ストライドとリフレクション推定ストライドの不一致を
    /// @note シェーダーごとに一度だけ警告するための記録。
    std::unordered_set<const void*> m_strideWarned;
    bool m_reportedMissingDrawResource = false;

    /// @note Recording-thread options and diagnostics; fusion retains no resource pointer or binding across Submit calls.
    bool m_bindingFusionEnabled = false, m_bindingDiagnostics = false;
    uint64_t m_bindingDraws = 0, m_bindingUploads = 0, m_bindingFailures = 0;
    uint64_t m_bindingResourceOwner = 0, m_bindingResourceFrame = 0;
    bool m_bindingResourceCoherent = true;
    double m_bindingSectionMs = 0;

    static constexpr uint32_t GPU_MAX_PASSES = GpuProfilerLedger::MAX_PASSES;
    FBZZ_GRAPHICS_API bool InitializeGpuProfiler();
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> m_gpuQueryHeap;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_gpuReadback;
    uint64_t* m_gpuMappedTimestamps = nullptr;
    uint64_t m_gpuTimestampFrequency = 0;
    GpuProfilerLedger m_gpuProfilerLedger{DX12Context::FRAME_COUNT};
    uint32_t m_gpuProfilerFrame = 0;
    uint64_t m_gpuPhysicalFrameSerial = 0;
    uint64_t m_gpuDeviceEpoch = 0;
    uint64_t m_gpuPassListSerial = 0;
    bool m_gpuProfilerRecording = false;
    uint64_t m_pixFrameToken = 0;
    uint64_t m_pixViewToken = 0;
    uint64_t m_pixPassToken = 0;
    std::string m_pixViewName;
};

} /// @note namespace fbzz::renderer
