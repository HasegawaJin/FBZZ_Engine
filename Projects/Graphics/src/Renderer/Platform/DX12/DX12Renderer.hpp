/// @file    DX12Renderer.hpp
/// @brief   コマンド記録と GPU 資源の生成を担う IRenderer の DirectX 12 実装。
/// @author  Hasegawa Jin
/// @date    2026-07-15
#pragma once
#include <Graphics/Api.hpp>

#include <Graphics/Renderer/IRenderer.hpp>
#include <Graphics/Renderer/Diagnostics/GpuProfilerLedger.hpp>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <string>
#include "DX12Context.hpp"
#include "Commands/DX12UploadArena.hpp"
#include "Pipeline/DX12PsoCache.hpp"
#include "Commands/DX12StateTracker.hpp"
#include "Pipeline/DX12PreparedSkinning.hpp"

namespace fbzz::renderer {

struct BindlessIndicesConstants;

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
    struct PreparedDraw;

    /// @brief 環境変数からディスクリプタ束縛の診断設定を読む。
    void ConfigureBindingDiagnostics();
    /// @brief 描画・Compute の記録件数と束縛の計測値をフレーム開始時に初期化する。
    void ResetFrameDiagnostics();
    /// @brief 定数未指定の CBV と資源未指定の b14 で共有する既定値を、当該フレームの転送領域に用意する。
    void PrepareFrameDefaultBindings();
    /// @brief 記録した GPU タイムスタンプを、フレームの最終提出で読み戻せるようにする。
    /// @param[in] frameIndex クエリヒープ内のフレームスロット番号。
    void ResolveFrameTimestamps(uint32_t frameIndex);
    /// @brief フレーム内で準備できなかった命令と、転送領域の使用量を診断ログへ出す。
    /// @param[in] frameIndex 使用量を参照する転送領域のフレームスロット番号。
    void ReportFrameDiagnostics(uint32_t frameIndex);
    /// @brief 指定されたリソースの準備を含め、描画命令を実際に記録できたかを返す。
    [[nodiscard]] bool TrySubmitDraw(const DrawCall& call, ResourceManager& resources);
    /// @brief チケットの出力を照合し、Draw が使用する変形済み頂点を解決する。
    /// @param[in] call チケットと出力行を含む描画要求。
    /// @param[out] skinning 描画中に借用する出力。チケットの指定がなければ初期値を保つ。
    /// @return 出力を使用できるか、チケットの指定がないとき true。
    [[nodiscard]] bool ResolvePreparedDraw(const DrawCall& call, PreparedDraw& skinning) const;

    /// @brief 準備済みの描画命令を選択し、ネイティブリストへ記録する。
    [[nodiscard]] bool RecordDraw(ID3D12GraphicsCommandList& commands, ID3D12GraphicsCommandList6* meshCommands,
                    ID3D12CommandSignature* indirectSignature, const DrawCall& call,
                    ResourceManager& resources, const PreparedDraw& skinning, bool usesBackBuffer);
    /// @brief PSO・定数・参照資源を束縛し、通常描画では入力アセンブラーも準備する。
    /// @param[in,out] commands 束縛と遷移を記録する描画コマンドリスト。
    /// @param[in] pipeline 描画先の形式と要求されたトポロジーに対応する PSO。
    /// @param[in] call 束縛する資源と描画方式を指定した要求。
    /// @param[in,out] resources ハンドルを解決し、必要に応じて資源のディスクリプタを取得する管理元。
    /// @param[in] skinning チケットで指定された場合に使う変形済み頂点。
    /// @param[in] shader 入力ストライドの診断に使うシェーダー。
    /// @param[in] meshDispatch メッシュシェーダーを起動する場合は true。
    /// @return 描画に必要な束縛がそろえば true。失敗時も記録済みの束縛と遷移はリストに残る。
    [[nodiscard]] bool PrepareDrawBindings(ID3D12GraphicsCommandList& commands, ID3D12PipelineState& pipeline,
        const DrawCall& call, ResourceManager& resources, const PreparedDraw& skinning,
        const DX12Shader& shader, bool meshDispatch);

    /// @brief トポロジーと、通常描画・メッシュシェーダー・間接描画の指定を照合する。
    [[nodiscard]] bool ValidateDrawTopology(const DrawCall& call, const DX12Shader& shader) const;
    [[nodiscard]] bool ValidateMeshDispatch(const DrawCall& call, bool isListTopology) const;
    [[nodiscard]] bool ValidateDrawReads(const DrawCall& call, ResourceManager& resources,
                                         const PreparedDraw& skinning, bool meshDispatch) const;
    [[nodiscard]] ID3D12CommandSignature* ResolveIndirectSignature(const DrawCall& call, bool meshDispatch) const;
    [[nodiscard]] bool ValidateIndirectDraw(const DrawCall& call, ResourceManager& resources, bool meshDispatch,
                                            const ID3D12CommandSignature* signature) const;
    [[nodiscard]] bool ValidateDrawColorReads(const DrawCall& call, ResourceManager& resources) const;

    /// @brief 現在の描画先とトポロジーに対応する PSO を取得する。
    [[nodiscard]] ID3D12PipelineState* ResolveDrawPipeline(const DrawCall& call, const DX12Shader& shader,
                                                          const class DX12PipelineState& state,
                                                          const class DX12RenderTarget* target);
    void BindDrawPipeline(ID3D12GraphicsCommandList& commands, ID3D12PipelineState& pipeline);
    [[nodiscard]] bool BindDrawResources(ID3D12GraphicsCommandList& commands, const DrawCall& call,
                           ResourceManager& resources, const PreparedDraw& skinning, bool meshDispatch);
    /// @brief 頂点・メッシュ段で参照するディスクリプタの添字を集める。
    static void CollectDrawVertexIndices(const DrawCall& call, ResourceManager& resources,
                                         const PreparedDraw& skinning, BindlessIndicesConstants& indices);
    /// @brief 全スロットが未使用なら共有ブロックを、それ以外なら今回の添字を b14 に束縛する。
    [[nodiscard]] bool BindDrawBindingIndices(ID3D12GraphicsCommandList& commands, const BindlessIndicesConstants& indices);
    void QueueDrawVertexReads(const DrawCall& call, ResourceManager& resources, const PreparedDraw& skinning);
    [[nodiscard]] bool BindDrawInputAssembler(ID3D12GraphicsCommandList& commands, const DrawCall& call,
                                               ResourceManager& resources, const PreparedDraw& skinning);
    [[nodiscard]] bool BindDrawConstants(ID3D12GraphicsCommandList& commands, const DrawCall& call, ResourceManager& resources);
    /// @brief 未指定スロットにはゼロ定数を返し、指定された定数を準備できないときは 0 を返す。
    [[nodiscard]] D3D12_GPU_VIRTUAL_ADDRESS ResolveConstantAddress(ResourceHandle<ConstantBufferTag> handle,
                                                                   ResourceManager& resources) const;
    void WarnDrawVertexStride(const DrawCall& call, ResourceManager& resources,
                              const DX12Shader& shader, const PreparedDraw& skinning);

    /// @brief TLAS と raw SRV のキュー・スロット・資源の契約を検証する。
    [[nodiscard]] bool ValidateComputeSpecialReads(const ComputeCall& call, ResourceManager& resources) const;
    /// @brief raw SRV が書き込みと競合せず、描画キューの Compute から読めるか検証する。
    /// @param[in] handle raw SRV として使う頂点バッファ。
    /// @param[in] vertexOutput 同じ Dispatch が直接 UAV に束縛する頂点バッファ。未指定なら無効ハンドル。
    /// @param[in,out] resources ハンドル解決と SRV 添字取得に使う管理元。
    /// @return この Dispatch から安全に参照できるとき true。
    [[nodiscard]] bool ValidateComputeRawRead(ResourceHandle<BufferTag> handle,
        ResourceHandle<BufferTag> vertexOutput, ResourceManager& resources) const;
    [[nodiscard]] bool ValidateComputeRayBindings(const ComputeCall& call, ResourceManager& resources) const;
    [[nodiscard]] bool ValidateComputeIndirectResources(const ComputeCall& call, ResourceManager& resources) const;
    [[nodiscard]] bool ValidateComputeDescriptors(const ComputeCall& call, ResourceManager& resources) const;
    [[nodiscard]] bool PrepareComputeConstants(const ComputeCall& call, ResourceManager& resources,
                                                std::span<D3D12_GPU_VIRTUAL_ADDRESS> constantAddresses) const;
    /// @brief Compute 用のディスクリプタヒープ、ルートシグネチャ、PSO、定数を順に束縛する。
    /// @param[in,out] commands Dispatch を記録するコマンドリスト。
    /// @param[in] pipeline 使用する Compute シェーダーに対応する PSO。
    /// @param[in] constantAddresses ルートスロット順の定数バッファ GPU アドレス。未指定スロットも既定値を持つ。
    void BindComputePipeline(ID3D12GraphicsCommandList& commands, ID3D12PipelineState& pipeline,
        std::span<const D3D12_GPU_VIRTUAL_ADDRESS> constantAddresses);
    void QueueComputeReads(const ComputeCall& call, ResourceManager& resources);
    [[nodiscard]] std::vector<ID3D12Resource*> QueueComputeWrites(const ComputeCall& call, ResourceManager& resources);
    [[nodiscard]] static BindlessIndicesConstants CollectComputeIndices(const ComputeCall& call, ResourceManager& resources);
    void SynchronizeComputeWrites(ID3D12GraphicsCommandList& commands, std::span<ID3D12Resource* const> writtenResources);

    FBZZ_GRAPHICS_API static std::unique_ptr<prepared_skinning_detail::NativeWork> PrepareCapturedNative(
        IRenderer& native, ResourceManager& resources, ResourceHandle<ShaderTag> shader,
        std::span<const CapturedSkinningRequest> requests, std::span<const math::Matrix4> matrices);
    std::shared_ptr<class DX12PreparedSkinningPool> m_preparedSkinningPool; ///< 発行済みのスキニングチケットと共有する GPU 資源プール。
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

    DX12Context m_context; ///< デバイス、キュー、スワップチェーンとコマンドリストの所有元。
    DX12UploadArena m_uploadArena; ///< フレームスロットごとに定数などの一時転送領域を管理する。
    DX12PsoCache m_psoCache; ///< 描画・Compute の PSO とルートシグネチャを保持する。
    DX12StateTracker m_stateTracker; ///< コマンドリストに積む資源遷移と、その前後の状態を追跡する。
    std::vector<ID3D12Resource*> m_computeBatchWrittenResources; ///< 独立 Dispatch 群が書いた UAV。バッチ末尾のバリアへまとめる。
    bool m_computeBatchActive = false; ///< UAV バリアをまとめる Compute バッチの記録中か。
    /// @see Docs/design/async-compute.md
    bool m_asyncComputeActive = false; ///< true の間は Dispatch をコンピュートキュー用のリストに記録する。
    /// @brief いま記録すべきコマンドリスト。非同期区間ではコンピュートリストを返す。
    /// @note Submit / SetRenderTarget は常に描画リストを使う。区間の中で描画は呼ばない契約。
    [[nodiscard]] FBZZ_GRAPHICS_API ID3D12GraphicsCommandList* RecordingList() const;
    /// @brief 列を割った後の描画リストへ、描画先・ビューポート・その状態を張り直す。
    /// @note Reset で束縛が全部落ちるうえ、区間へ入る前に束縛中の RT も COMMON へ落としてある。
    /// @note       呼び出し側が SetRenderTarget を呼び直すとは限らないので、ここで揃える。
    FBZZ_GRAPHICS_API void RestoreGraphicsTargetState(ResourceManager& resources);
    /// @brief 通常の RT の色・深度資源に描き込めるよう、束縛前に状態遷移を積む。
    /// @param[in] target 遷移する資源を所有する描画先。
    void QueueDrawTargetWrites(const class DX12RenderTarget& target);
    /// @brief 状態遷移済みのバックバッファ、通常の RT、またはキューブ面のビューを束縛する。
    /// @param[in,out] commands RTV と DSV の束縛を記録する描画コマンドリスト。
    /// @param[in] target 描画先の借用参照。nullptr はバックバッファを表す。
    void BindDrawTargetViews(ID3D12GraphicsCommandList& commands, const class DX12RenderTarget* target) const;
    /// @brief 描画範囲をピクセル単位で記憶し、ビューポートとシザーに同じ矩形を設定する。
    /// @param[in,out] commands 設定を記録する描画コマンドリスト。
    /// @param[in] x 描画先の左端から矩形の左端までのピクセル数。
    /// @param[in] y 描画先の上端から矩形の上端までのピクセル数。
    /// @param[in] width 矩形の幅を示すピクセル数。
    /// @param[in] height 矩形の高さを示すピクセル数。
    void SetRasterRegion(ID3D12GraphicsCommandList& commands, uint32_t x, uint32_t y, uint32_t width, uint32_t height);
    /// @brief 描画リストの分割やプレビューを挟んだあと、記憶したビューポートとシザーを設定する。
    /// @param[in,out] commands 設定を記録する描画コマンドリスト。
    void ApplyRasterRegion(ID3D12GraphicsCommandList& commands) const;
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
    ResourceHandle<RenderTargetTag> m_currentRenderTargetHandle; ///< 束縛中の RT。無効ハンドルはバックバッファを表す。
    uint64_t m_currentRenderTargetOwner = 0; ///< ハンドルを解決する資源管理元の生存台帳 ID。バックバッファの場合は 0。
    std::array<RenderTargetColorAttachment, D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT> m_currentColorAttachments{}; ///< 独立して束縛した色面を RTV スロット順に保持する。
    uint32_t m_currentColorAttachmentCount = 0; ///< 配列の有効要素数。0 なら通常の描画先かキューブ面を使う。
    bool m_reportedLostRenderTarget = false; ///< 束縛中に解放された RT の警告を出したことがあるか。
    std::unique_ptr<class DX12IblBaker> m_iblBaker; ///< 環境光ベイクに使うバックエンド実装の所有元。
    D3D12_CPU_DESCRIPTOR_HANDLE m_currentCubeRtv{}; ///< 束縛中のキューブ面の RTV。0 はキューブ面の束縛なし。
    uint32_t m_currentCubeFace = 0; ///< 選択中のキューブ面の番号。
    uint32_t m_currentCubeMip = 0; ///< 選択中のキューブ面と共有深度面のミップレベル。
    D3D12_VIEWPORT m_currentViewport{}; ///< 描画先内で使うビューポート。プレビューやリスト分割の後に再適用する。
    D3D12_RECT m_currentScissor{}; ///< 描画先内のシザー矩形。右端と下端は範囲に含めない。
    D3D12_GPU_VIRTUAL_ADDRESS m_nullConstantAddress = 0; ///< 未指定の定数スロットへ束縛するゼロ埋め領域の GPU アドレス。確保失敗時は 0。
    D3D12_GPU_VIRTUAL_ADDRESS m_invalidBindlessAddress = 0; ///< このフレーム内の全スロット未使用の Draw が共有する b14 の GPU アドレス。

    std::array<D3D12_GPU_VIRTUAL_ADDRESS, bindingCount::CONSTANT_BUFFERS> m_lastRootCbv{}; ///< 描画のルートスロットごとに直前の CBV アドレスを保持する。
    bool m_rootCbvCacheValid = false; ///< 現在のリストとルートシグネチャで、記憶した CBV の束縛を再利用できるか。
    ID3D12RootSignature* m_lastGraphicsRootSignature = nullptr; ///< 描画リストに束縛したシグネチャの借用参照。再設定時は全 CBV を束縛し直す。
    ID3D12PipelineState* m_lastPipelineState = nullptr; ///< 描画リストに最後に束縛した PSO の借用参照。
    ID3D12DescriptorHeap* m_lastDescriptorHeap = nullptr; ///< 描画リストに最後に束縛した SRV ヒープの借用参照。
    uint64_t m_seenPipelineStateGeneration = 0; ///< 外部から描画リストへの記録を検出する、最後に確認した Context の世代。

    /// @note Compute へ切り替えるとグラフィクス側のルート束縛は当てにできなくなる。
    /// @note Dispatch / BeginFrame / コマンドリスト再取得のたびに呼ぶこと。
    FBZZ_GRAPHICS_API void InvalidateRootCbvCache();

    std::unordered_set<const void*> m_strideWarned; ///< 頂点ストライドの不一致を警告済みのシェーダーのアドレス。
    bool m_reportedMissingDrawResource = false; ///< 描画用シェーダーまたは PSO の不足を警告済みか。

    bool m_bindingFusionEnabled = false; ///< 一度の走査で描画用資源の遷移と添字取得を行うか。
    bool m_bindingDiagnostics = false; ///< 描画用ディスクリプタ束縛の計測とログ出力を有効にするか。
    uint64_t m_bindingDraws = 0; ///< 今フレームで描画用 b14 の準備に入った回数。
    uint64_t m_bindingUploads = 0; ///< 実データを持つ描画用 b14 を転送した回数。
    uint64_t m_emptyBindingReuses = 0; ///< 全スロットが未使用のため共通の b14 を使った回数。
    uint64_t m_bindingFailures = 0; ///< 描画用 b14 を束縛できなかった回数。
    uint64_t m_recordedDraws = 0; ///< 今フレームのグラフィクスリストへ記録した直接・間接描画命令の数。
    uint64_t m_recordedDispatches = 0; ///< 今フレームに TryDispatch から記録した Compute 命令の数。
    uint64_t m_rejectedDrawPreparations = 0; ///< 定数・添字・頂点入力の準備に失敗し記録を取りやめた描画数。
    uint64_t m_rejectedDispatchPreparations = 0; ///< 定数または添字の準備に失敗し記録を取りやめた Dispatch 数。
    uint64_t m_bindingResourceOwner = 0; ///< フレーム内の最初の描画が参照した資源管理元の ID。
    uint64_t m_bindingResourceFrame = 0; ///< フレーム内の最初の描画が参照した資源管理元のフレーム番号。
    bool m_bindingResourceCoherent = true; ///< 今までの描画が同じ管理元と資源フレームを参照しているか。
    double m_bindingSectionMs = 0; ///< 今フレームの描画で添字の収集と束縛に費やした時間 [ms]。

    static constexpr uint32_t GPU_MAX_PASSES = GpuProfilerLedger::MAX_PASSES;
    FBZZ_GRAPHICS_API bool InitializeGpuProfiler();
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> m_gpuQueryHeap; ///< フレームスロットごとの GPU タイムスタンプを記録するヒープ。
    Microsoft::WRL::ComPtr<ID3D12Resource> m_gpuReadback; ///< クエリ結果を CPU へ読み戻すバッファ。
    uint64_t* m_gpuMappedTimestamps = nullptr; ///< 読み戻しバッファを Map した先頭。Shutdown で Unmap する。
    uint64_t m_gpuTimestampFrequency = 0; ///< 描画キューのタイムスタンプ周波数 [Hz]。0 は計測不可。
    GpuProfilerLedger m_gpuProfilerLedger{DX12Context::FRAME_COUNT}; ///< タイムスタンプと描画パスの対応、回収に使う提出フェンスを保持する。
    uint32_t m_gpuProfilerFrame = 0; ///< 計測中の物理フレームに割り当てたクエリスロット。
    uint64_t m_gpuPhysicalFrameSerial = 0; ///< このデバイスで記録を始めた物理フレームの通し番号。
    uint64_t m_gpuDeviceEpoch = 0; ///< 計測結果を識別するために初期化ごとに割り当てるデバイスの世代。
    uint64_t m_gpuPassListSerial = 0; ///< パスの開始クエリを記録した描画コマンドリストの世代。
    bool m_gpuProfilerRecording = false; ///< 現在の物理フレームでタイムスタンプを記録しているか。
    uint64_t m_pixFrameToken = 0; ///< PIX フレームイベントを閉じるトークン。0 はイベントなし。
    uint64_t m_pixViewToken = 0; ///< PIX ビューイベントを閉じるトークン。0 はイベントなし。
    uint64_t m_pixPassToken = 0; ///< PIX パスイベントを閉じるトークン。0 はイベントなし。
    std::string m_pixViewName; ///< 現在のビュー名。Compute の PIX イベントにも記録する。
};

} // namespace fbzz::renderer
