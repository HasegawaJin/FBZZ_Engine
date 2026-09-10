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
    // Editor の HDRI → DDS ベイクは DX12HdriBaker (IIblBaker 実装) を返す。
    // WHY: BakeSkyLight (実行時畳み込み) とは別に、フレーム非依存で 4 DDS を焼く同期経路。
    std::unique_ptr<IIblBaker> CreateIblBaker() override;
    void GpuProfBeginFrame() override;
    void GpuProfEndFrame() override;
    void GpuProfBeginPass(const char* name) override;
    void GpuProfEndPass(const char* name) override;
    void GpuProfCollect() override;
    const std::vector<GpuPassProfile>& GpuProfGetResults() const override { return m_gpuResults; }
    // AI 連携 (viewport.capture): Scene View RT を PNG バイト列へ読み戻す。
    bool CaptureRenderTargetToPng(ResourceHandle<RenderTargetTag> rt, ResourceManager& resources,
                                  std::vector<uint8_t>& outPng,
                                  uint32_t& outWidth, uint32_t& outHeight) override;
    // AI 連携 (vfx.previewMetrics): 同じ RT を HDR 線形値のまま数値評価用に読み戻す。
    bool CaptureRenderTargetToLinearRGBA(ResourceHandle<RenderTargetTag> rt, ResourceManager& resources,
                                         std::vector<float>& outRgba,
                                         uint32_t& outWidth, uint32_t& outHeight) override;
    uint32_t GetWidth() const override { return m_context.GetWidth(); }
    uint32_t GetHeight() const override { return m_context.GetHeight(); }
    DX12Context& GetContext() { return m_context; }

private:
    std::unique_ptr<IBuffer> CreateNativeVertexBuffer(const void*, size_t, uint32_t) override;
    std::unique_ptr<IBuffer> CreateNativeGpuWritableVertexBuffer(size_t sizeBytes, uint32_t stride) override;
    std::unique_ptr<IBuffer> CreateNativeIndexBuffer(const void*, uint32_t) override;
    std::unique_ptr<IConstantBuffer> CreateNativeConstantBuffer(size_t) override;
    std::unique_ptr<IShader> CreateNativeShader(const std::string&) override;
    std::unique_ptr<ITexture> CreateNativeTexture(const std::string&) override;
    std::unique_ptr<ITexture> CreateNativeTextureFromData(const uint8_t*, uint32_t, uint32_t) override;
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
    // 独立 Dispatch 群が書いた UAV を保持し、パス末尾の 1 回の ResourceBarrier へ集約する。
    std::vector<ID3D12Resource*> m_computeBatchWrittenResources;
    bool m_computeBatchActive = false;
    class DX12RenderTarget* m_currentRenderTarget = nullptr;
    // 束縛中の RT のハンドル。生ポインタと二重に持つのは «消えたかどうか» を問えるようにするため。
    // WHY: RT は描画の途中でも解放される (ビューポートのリサイズ、ViewRenderTargets の作り直し)。
    //      解放されたものを次の SetRenderTarget が «前の RT» として触ると、破棄済みの
    //      オブジェクトから GetColorResource を引いて、無関係な番地へバリアを積む。
    ResourceHandle<RenderTargetTag> m_currentRenderTargetHandle;
    std::unique_ptr<class DX12IblBaker> m_iblBaker;
    D3D12_CPU_DESCRIPTOR_HANDLE m_currentCubeRtv{};
    D3D12_GPU_VIRTUAL_ADDRESS m_nullConstantAddress = 0;

    // WHY: 連続する Draw が同じテクスチャ/バッファ集合を束縛する場合 (同一マテリアルのバッチ等)、
    //      shader-visible リングへの CopyDescriptors を毎 Draw 発行するのは無駄。
    //      束縛シグネチャをキーに GPU テーブルをキャッシュし、一致すればコピーを丸ごと省略する。
    //      リングはフレームごとに巻き戻る (BeginFrame でオフセットリセット) ため、フレームを跨いだ
    //      再利用は不可 — BeginFrame で必ず無効化する。
    //
    // WHY 直前 1 件ではなくフレーム内マップか: GBuffer のようにマテリアルが交互に来るパスでは
    //      「直前と同じか」だけの判定はほぼ毎 Draw で外れ、32 回のディスクリプタコピーが
    //      そのまま記録コストになる。フレーム内で同じ束縛が再登場したら必ず当たるようにする。
    // テクスチャ 32 枠 + PS-readable StructuredBuffer 2 枠を (id,gen) へ畳んだ束縛シグネチャ。
    // WHY タグ違いのハンドルを uint64 へ潰すか: textures は TextureTag、psBuffers は
    //     StructuredBufferTag と型が違うため 1 本の配列に並べられない。
    //     比較とハッシュにしか使わないので、identity をそのまま数値化する。
    static constexpr size_t kPixelTableKeySize = 34;
    using PixelTableKey = std::array<uint64_t, kPixelTableKeySize>;
    static PixelTableKey MakePixelTableKey(const DrawCall& call);
    struct PixelTableKeyHash {
        size_t operator()(const PixelTableKey& key) const noexcept;
    };
    std::unordered_map<PixelTableKey, D3D12_GPU_DESCRIPTOR_HANDLE, PixelTableKeyHash> m_pixelTableCache;
    // 直前 Draw の結果だけは別に持ち、マップ探索すら省く高速路にする。
    PixelTableKey m_lastPixelTextures{};
    D3D12_GPU_DESCRIPTOR_HANDLE m_lastPixelTableGpu{};
    bool m_lastPixelTableValid = false;
    std::array<ResourceHandle<StructuredBufferTag>, 3> m_lastVertexBuffers{};
    D3D12_GPU_DESCRIPTOR_HANDLE m_lastVertexTableGpu{};
    bool m_lastVertexTableValid = false;

    // 直前に root スロットへ束縛した CBV の GPU VA。変化したスロットだけ再設定するために持つ。
    // WHY: 従来は毎 Draw「14 スロットを null で埋めてから実 CB で上書き」していて最大 28 回の
    //      SetGraphicsRootConstantBufferView が出ていた。GBuffer では実際に変わるのは
    //      Object CB と Material CB だけなので、差分だけ出せば 2〜3 回で済む。
    // NOTE: グラフィクスとコンピュートで root signature が別物のため、Dispatch を挟んだら
    //       必ず無効化する (InvalidateRootCbvCache)。
    std::array<D3D12_GPU_VIRTUAL_ADDRESS, 14> m_lastRootCbv{};
    bool m_rootCbvCacheValid = false;

    // 冗長なパイプライン状態設定を弾くための直前値。
    // NOTE: m_lastGraphicsRootSignature は「正しさ」のために必要 (ルートシグネチャの再設定は
    //       全ルート引数を無効化するため、m_lastRootCbv の前提が崩れる)。
    //       他の 2 つは記録コスト削減のみが目的。
    ID3D12RootSignature* m_lastGraphicsRootSignature = nullptr;
    ID3D12PipelineState* m_lastPipelineState = nullptr;
    ID3D12DescriptorHeap* m_lastDescriptorHeap = nullptr;
    // 共有コマンドリストへ他所が記録したことを検知するための世代 (DX12Context 側が上げる)。
    uint64_t m_seenPipelineStateGeneration = 0;

    // Compute へ切り替えるとグラフィクス側のルート束縛は当てにできなくなる。
    // Dispatch / BeginFrame / コマンドリスト再取得のたびに呼ぶこと。
    void InvalidateRootCbvCache();

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
