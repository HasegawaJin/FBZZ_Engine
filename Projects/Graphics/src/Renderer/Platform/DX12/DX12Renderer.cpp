/// @file    DX12Renderer.cpp
/// @brief   DirectX 12 の描画・Compute コマンド記録と GPU 資源生成。
/// @author  Hasegawa Jin
/// @date    2026-07-15
#include <Graphics/Renderer/Commands/ShaderBindingSlots.hpp>
#include "DX12Renderer.hpp"

#include <Core/Logger/Logger.hpp>
#include <Core/Util/HResult.hpp>
#include "Resources/DX12AccelerationStructure.hpp"
#include "Resources/DX12Buffer.hpp"
#include "Resources/DX12ConstantBuffer.hpp"
#include "Commands/DX12UploadArenaSizing.hpp"
#include "Pipeline/DX12PipelineState.hpp"
#include "Pipeline/DX12Shader.hpp"
#include "Resources/DX12Texture.hpp"
#include "Baking/DX12IblBaker.hpp"
#include "Baking/DX12HdriBaker.hpp"
#include "Resources/DX12RenderTarget.hpp"
#include "Resources/DX12StructuredBuffer.hpp"
#include <Graphics/Renderer/Resources/BindlessIndices.hpp>
#include <Graphics/Renderer/Resources/ResourceManager.hpp>
#include <Graphics/Renderer/Geometry/Mesh.hpp>
#include "../../Resources/ResourceManagerLifetime.hpp"
/// @note RT → PNG エンコードは AI 連携用の共通処理。
#include "../RenderTargetCapture.hpp"
#include <DirectXTex.h>
#include <cstring>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <chrono>

namespace fbzz::renderer {

namespace {
std::atomic<uint64_t> gpuProfilerDeviceEpoch{1}; ///< 初期化ごとに割り当てる GPU 計測デバイスの世代。
constexpr uint32_t CONSTANT_BUFFER_REGISTER_BYTES = 4 * sizeof(uint32_t);
constexpr uint32_t TIMESTAMPS_PER_GPU_PASS = 2; ///< パスの開始と終了で使うクエリ数。
constexpr uint32_t PIX_FRAME_COLOR = 0xff4878a8u;
constexpr uint32_t PIX_CUBE_CAPTURE_COLOR = 0xff65b891u;
constexpr uint32_t PIX_COMPUTE_COLOR = 0xff9d78c9u;
constexpr uint32_t PIX_VIEW_COLOR = 0xff589cb4u;

/// @brief 同じコマンドリスト内で、先行 Dispatch の UAV 書き込みを後続のアクセスより前に確定する。
void RecordUavBarriers(ID3D12GraphicsCommandList& commands, std::span<ID3D12Resource* const> writtenResources)
{
    std::vector<D3D12_RESOURCE_BARRIER> barriers(writtenResources.size());
    for (size_t index = 0; index < writtenResources.size(); ++index) {
        barriers[index].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barriers[index].UAV.pResource = writtenResources[index];
    }
    commands.ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
}
/// @brief スロット順に SRV の読み取り遷移を積む。走査を統合する経路では同時に添字も記録する。
/// @tparam NativeResource ハンドルに対応する DirectX 12 の資源型。
/// @tparam GATHER_INDICES 遷移と同じ走査で b14 の添字を集める場合は true。
/// @param[in] handles 連続したスロットに束縛する資源のハンドル。
/// @param[in,out] resources ハンドルを解決し、必要に応じて SRV の添字を取得する管理元。
/// @param[in,out] stateTracker 読み取りへの状態遷移を積む追跡表。
/// @param[in] shaderReadState シェーダーステージに応じた SRV 用の状態。
/// @param[in] firstSlot handles の先頭が対応する b14 のピクセル用添字スロット。
/// @param[in,out] indices 描画用の b14。対象資源から取得した添字で対応スロットを更新する。
template<class NativeResource, bool GATHER_INDICES>
void PrepareDrawPixelBindings(const auto& handles, ResourceManager& resources, DX12StateTracker& stateTracker,
    D3D12_RESOURCE_STATES shaderReadState, uint32_t firstSlot, BindlessIndicesConstants& indices)
{
    for (uint32_t index = 0; index < handles.size(); ++index) {
        const auto handle = handles[index];
        if (GATHER_INDICES && !handle.IsValid()) {
            continue;
        }
        auto* resource = resources.Get(handle);
        if (!resource) {
            continue;
        }

        stateTracker.QueueTransition(static_cast<NativeResource*>(resource)->GetResource(), shaderReadState);
        if constexpr (GATHER_INDICES) {
            indices.pixel[firstSlot + index] = resource->GetBindlessIndex();
        }
    }
}

template<bool GATHER_INDICES>
void PrepareDrawPixelReads(const DrawCall& call, ResourceManager& resources, DX12StateTracker& stateTracker,
                           D3D12_RESOURCE_STATES shaderReadState, BindlessIndicesConstants& indices)
{
    PrepareDrawPixelBindings<DX12Texture, GATHER_INDICES>(call.textures, resources, stateTracker,
        shaderReadState, /*firstSlot=*/0, indices);

    // シェーダーが添字を別途受け取るテクスチャは、読み取りに必要な状態だけを準備する。
    for (const auto handle : call.indirectReadTextures) {
        if (auto* texture = resources.Get(handle)) {
            stateTracker.QueueTransition(static_cast<DX12Texture*>(texture)->GetResource(), shaderReadState);
        }
    }

    // ライト一覧などの構造化バッファは、直前に Compute の UAV として使われたことがある。
    PrepareDrawPixelBindings<DX12StructuredBuffer, GATHER_INDICES>(call.psBuffers, resources, stateTracker,
        shaderReadState, kPsBufferBaseSlot, indices);
    PrepareDrawPixelBindings<DX12StructuredBuffer, GATHER_INDICES>(call.auxiliaryPsBuffers, resources, stateTracker,
        shaderReadState, kAuxiliaryPsBufferBaseSlot, indices);
}

void CollectDrawPixelIndices(const DrawCall& call, ResourceManager& resources, BindlessIndicesConstants& indices)
{
    for (uint32_t slot = 0; slot < call.textures.size(); ++slot) {
        if (auto* texture = resources.Get(call.textures[slot])) {
            indices.pixel[slot] = texture->GetBindlessIndex();
        }
    }
    for (uint32_t index = 0; index < call.psBuffers.size(); ++index) {
        if (auto* buffer = resources.Get(call.psBuffers[index])) {
            indices.pixel[kPsBufferBaseSlot + index] = buffer->GetBindlessIndex();
        }
    }
    for (uint32_t index = 0; index < call.auxiliaryPsBuffers.size(); ++index) {
        if (auto* buffer = resources.Get(call.auxiliaryPsBuffers[index])) {
            indices.pixel[kAuxiliaryPsBufferBaseSlot + index] = buffer->GetBindlessIndex();
        }
    }
}

bool IsStructuredUavSlot(const ComputeCall& call, uint32_t slot)
{
    return slot >= uav::STRUCTURED_FIRST && slot < uav::STRUCTURED_FIRST + call.uavBuffers.size();
}

bool ValidateOptionalComputeDescriptors(const auto& handles, ResourceManager& resources, auto getIndex)
{
    for (const auto handle : handles) {
        if (!handle.IsValid()) {
            continue;
        }
        const auto* resource = resources.Get(handle);
        if (!resource || getIndex(*resource) == INVALID_BINDLESS_INDEX) {
            return false;
        }
    }
    return true;
}

auto ReadUploadArenaSizing()
{
    char* configuredMiB = nullptr;
    size_t configuredLength = 0;
    const auto configReadResult = _dupenv_s(&configuredMiB, &configuredLength, "FBZZ_DX12_UPLOAD_ARENA_MIB");
    const auto uploadSizing = upload_arena_detail::ParseUploadArenaMiB(
        configuredMiB ? std::string_view{configuredMiB} : std::string_view{},
        configuredMiB != nullptr || configReadResult != 0);
    std::free(configuredMiB);
    if (!uploadSizing.valid) {
        FBZZ_LOG_WARN("DX12Renderer: invalid FBZZ_DX12_UPLOAD_ARENA_MIB; expected 16/32/64/128; using 16 MiB");
    }
    return uploadSizing;
}

bool IsValidColorOnlyAttachment(const DX12RenderTarget* target, uint32_t colorIndex,
                                uint32_t width, uint32_t height, DXGI_FORMAT format)
{
    if (!target || target->IsCubemap() || target->HasDepth() || colorIndex >= target->GetColorCount()) {
        return false;
    }
    return target->GetWidth() == width && target->GetHeight() == height && target->GetColorFormat() == format;
}

/// @brief 間接描画の補助引数が、間接引数バッファと命令種別に対応しているか確認する。
/// @param[in] call 間接引数のハンドル、バイトオフセット、命令数、インスタンス補正を含む描画要求。
/// @param[in] meshDispatch メッシュシェーダーを起動する場合は true。インスタンス補正はインデックス付き描画専用。
/// @return 指定された補助引数を使えるか、既定値のままであれば true。
bool ValidateDrawIndirectParameters(const DrawCall& call, bool meshDispatch)
{
    const bool hasIndirectArguments = call.indirectArguments.IsValid();
    const bool hasIndirectRange = call.indirectArgumentOffset != 0 || call.indirectCommandCount != 1;
    if (!hasIndirectArguments && hasIndirectRange) {
        FBZZ_LOG_ERROR("DX12Renderer::Submit: indirect argument offset/count set without indirectArguments");
        return false;
    }

    const bool usesIndexedIndirectCommand = hasIndirectArguments && !meshDispatch;
    if (call.indirectInstanceOffset && !usesIndexedIndirectCommand) {
        FBZZ_LOG_ERROR("DX12Renderer::Submit: indirectInstanceOffset requires indexed indirect arguments");
        return false;
    }
    return true;
}

/// @brief 間接命令のレイアウト、書き込み用ディスクリプタ、実行範囲を検証する。
/// @param[in] call 間接命令列の開始バイトと実行する命令数を含む描画要求。
/// @param[in] argumentBuffer GPU が書き込み、ExecuteIndirect が読み取る構造化バッファ。
/// @param[in] expectedStride コマンドシグネチャが要求する一命令のバイト数。0 より大きいこと。
/// @return バッファの指定と実行範囲が有効な場合は true。
bool ValidateIndirectArgumentLayout(const DrawCall& call, const DX12StructuredBuffer& argumentBuffer,
    uint32_t expectedStride)
{
    const uint64_t argumentBufferSize = argumentBuffer.GetSize();
    const bool matchesCommandLayout = argumentBuffer.GetStride() == expectedStride
        && argumentBuffer.GetBindlessUavIndex() != INVALID_BINDLESS_INDEX;

    // 開始位置を先に検証する。残りのバイト数を割ってから命令数と比較し、減算や乗算の桁あふれを防ぐ。
    const bool startsWithinBuffer = call.indirectArgumentOffset <= argumentBufferSize;
    const bool fitsCommandCount = startsWithinBuffer
        && call.indirectCommandCount <= (argumentBufferSize - call.indirectArgumentOffset) / expectedStride;
    if (matchesCommandLayout && fitsCommandCount) {
        return true;
    }

    FBZZ_LOG_ERROR("DX12Renderer::Submit: indirect argument buffer stride/UAV/range invalid "
                   "(stride=%u expected=%u offset=%llu count=%u size=%llu)",
                   argumentBuffer.GetStride(), expectedStride,
                   static_cast<unsigned long long>(call.indirectArgumentOffset), call.indirectCommandCount,
                   static_cast<unsigned long long>(argumentBufferSize));
    return false;
}

} // namespace

/// @note out-of-line 定義。ここは DX12IblBaker.hpp を include 済みなので、m_iblBaker
/// @note (unique_ptr<DX12IblBaker>) のデリーターを完全型として実体化できる。
DX12Renderer::DX12Renderer() = default;
DX12Renderer::~DX12Renderer()
{
    prepared_skinning_detail::UnregisterBackend(*this);
    StopPreparedSkinningPool(m_preparedSkinningPool);
    if (m_preparedSkinningPool) {
        m_context.Flush();
    }
}

void DX12Renderer::ConfigureBindingDiagnostics()
{
    m_bindingFusionEnabled = true;
    m_bindingDiagnostics = false;
    if (const char* mode = std::getenv("FBZZ_DX12_BINDING_SCAN_FUSION")) {
        m_bindingFusionEnabled = std::strcmp(mode, "1") == 0;
    }
    if (const char* diagnostic = std::getenv("FBZZ_DX12_BINDLESS_INDICES_DIAGNOSTICS")) {
        m_bindingDiagnostics = std::strcmp(diagnostic, "1") == 0;
    }
    if (std::getenv("FBZZ_DX12_BINDING_SCAN_FUSION") || m_bindingDiagnostics) {
        std::fprintf(stderr, "DX12_BINDING_SCAN_CONFIG fused=%u diagnostic=%u\n",
            static_cast<unsigned>(m_bindingFusionEnabled), static_cast<unsigned>(m_bindingDiagnostics));
    }
}

bool DX12Renderer::Init(HWND hwnd, uint32_t width, uint32_t height)
{
    ConfigureBindingDiagnostics();
    const auto uploadSizing = ReadUploadArenaSizing();

    if (!m_context.Initialize(hwnd, width, height)) {
        FBZZ_LOG_ERROR("DX12Renderer::Init: DX12Context 初期化失敗");
        return false;
    }
    if (!m_uploadArena.Initialize(m_context.GetDevice(), uploadSizing.bytesPerFrame)) {
        FBZZ_LOG_ERROR("DX12Renderer::Init: UploadArena 初期化失敗");
        m_context.Shutdown();
        return false;
    }
    // 確保した Upload Heap の容量を記録する。明示設定時は INFO ログ無効でも確認できるようにする。
    const size_t totalUploadBytes = uploadSizing.bytesPerFrame * DX12Context::FRAME_COUNT;
    FBZZ_LOG_INFO("DX12Renderer: UploadArena initialized (%zu bytes/frame, %u frames, %zu bytes total)",
        uploadSizing.bytesPerFrame, DX12Context::FRAME_COUNT, totalUploadBytes);
    if (uploadSizing.specified) {
        std::fprintf(stderr, "UPLOAD_ARENA_CONFIG bytesPerFrame=%zu frameCount=%u totalBytes=%zu\n",
            uploadSizing.bytesPerFrame, DX12Context::FRAME_COUNT, totalUploadBytes);
    }
    if (!m_psoCache.Initialize(m_context.GetDevice(), m_context.SupportsBindless())) {
        FBZZ_LOG_ERROR("DX12Renderer::Init: PsoCache/RootSignature 初期化失敗");
        m_uploadArena.Shutdown();
        m_context.Shutdown();
        return false;
    }
    m_gpuDeviceEpoch = gpuProfilerDeviceEpoch.fetch_add(1, std::memory_order_relaxed);
    m_gpuPhysicalFrameSerial = 0;
    m_gpuProfilerRecording = false;
    const bool profilerReady = InitializeGpuProfiler();
    m_gpuProfilerLedger.Reset(m_gpuDeviceEpoch, profilerReady);
    if (!profilerReady) {
        FBZZ_LOG_WARN("DX12Renderer: GPUプロファイラーを初期化できませんでした");
    }
    prepared_skinning_detail::RegisterBackend(*this, PrepareCapturedNative);
    return true;
}

std::unique_ptr<prepared_skinning_detail::NativeWork> DX12Renderer::PrepareCapturedNative(
    IRenderer& native, ResourceManager& resources, ResourceHandle<ShaderTag> shader,
    std::span<const CapturedSkinningRequest> requests, std::span<const math::Matrix4> matrices)
{
    auto& renderer = static_cast<DX12Renderer&>(native);
    if (renderer.m_asyncComputeActive) {
        return {};
    }
    if (!renderer.m_preparedSkinningPool) {
        renderer.m_preparedSkinningPool = CreatePreparedSkinningPool(renderer.m_context);
    }
    return PrepareDX12Skinning(renderer.m_preparedSkinningPool, renderer.m_context, renderer.m_psoCache,
        renderer.m_stateTracker, resources, shader, requests, matrices);
}

void DX12Renderer::Shutdown()
{
    prepared_skinning_detail::UnregisterBackend(*this);
    StopPreparedSkinningPool(m_preparedSkinningPool);
    EndComputeBatch();
    m_context.Flush();
    m_preparedSkinningPool.reset();
    m_iblBaker.reset();
    if (m_gpuReadback && m_gpuMappedTimestamps) {
        m_gpuReadback->Unmap(0, nullptr);
    }
    m_gpuMappedTimestamps = nullptr;
    m_gpuReadback.Reset();
    m_gpuQueryHeap.Reset();
    m_gpuTimestampFrequency = 0;
    m_gpuProfilerRecording = false;
    m_gpuProfilerLedger.Reset(m_gpuDeviceEpoch, false);
    m_psoCache.Shutdown();
    m_stateTracker.Clear();
    m_uploadArena.Shutdown();
    m_context.Shutdown();
}

void DX12Renderer::InvalidateRootCbvCache()
{
    m_lastRootCbv.fill(0);
    m_rootCbvCacheValid = false;
    m_lastGraphicsRootSignature = nullptr;
    m_lastPipelineState = nullptr;
    m_lastDescriptorHeap = nullptr;
}

void DX12Renderer::BeginFrame()
{
    if (!m_context.BeginFrame()) {
        return;
    }

    ResetFrameDiagnostics();
    BeginPreparedSkinningFrame(m_preparedSkinningPool);

    // スロットのフェンス待機が済んでから、前に使ったクエリを回収し、新しい計測を始める。
    GpuProfCollect();
    m_gpuProfilerFrame = m_context.GetFrameIndex();
    ++m_gpuPhysicalFrameSerial;
    m_gpuProfilerRecording = m_gpuProfilerLedger.BeginFrame(
        m_gpuProfilerFrame, m_gpuPhysicalFrameSerial, m_gpuDeviceEpoch);
    m_pixViewToken = 0;
    m_pixPassToken = 0;
    m_pixViewName.clear();
    m_pixFrameToken = m_context.BeginPixEvent(m_context.GetCommandList(),
        "Frame physical=" + std::to_string(m_gpuPhysicalFrameSerial)
            + " device=" + std::to_string(m_gpuDeviceEpoch), PIX_FRAME_COLOR);

    // Context が新しいリストに設定したバックバッファを、描画先の記憶にも反映する。
    m_computeBatchActive = false;
    m_computeBatchWrittenResources.clear();
    m_currentRenderTargetHandle = {};
    m_currentRenderTargetOwner = 0;
    m_currentColorAttachmentCount = 0;
    m_currentCubeRtv = {};
    const auto width = m_context.GetWidth();
    const auto height = m_context.GetHeight();
    m_currentViewport = {0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
    m_currentScissor = {0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};

    // リストの Reset で前フレームの束縛は消える。共通の定数も新しい転送スロットに用意する。
    InvalidateRootCbvCache();
    m_uploadArena.BeginFrame(m_gpuProfilerFrame);
    PrepareFrameDefaultBindings();
}

void DX12Renderer::ResetFrameDiagnostics()
{
    m_bindingDraws = 0;
    m_bindingUploads = 0;
    m_emptyBindingReuses = 0;
    m_bindingFailures = 0;
    m_rejectedDrawPreparations = 0;
    m_rejectedDispatchPreparations = 0;
    m_recordedDraws = 0;
    m_recordedDispatches = 0;
    m_bindingResourceOwner = 0;
    m_bindingResourceFrame = 0;
    m_bindingResourceCoherent = true;
    m_bindingSectionMs = 0;
}

void DX12Renderer::PrepareFrameDefaultBindings()
{
    // Root CBV はサイズを持たない。未指定の CBV でも読み得る最大 64 KiB をゼロで埋める。
    /// @see https://microsoft.github.io/DirectX-Specs/d3d/ResourceBinding.html#using-descriptors-directly-in-the-root-arguments Using descriptors directly in the root arguments
    m_nullConstantAddress = 0;
    const auto nullConstant = m_uploadArena.Allocate(
        D3D12_REQ_CONSTANT_BUFFER_ELEMENT_COUNT * CONSTANT_BUFFER_REGISTER_BYTES,
        D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
    if (nullConstant) {
        std::memset(nullConstant.cpu, 0, nullConstant.size);
        m_nullConstantAddress = nullConstant.gpu;
    }

    // 参照資源を持たない描画どうしで、全添字が無効な読み取り専用の b14 を共有する。
    m_invalidBindlessAddress = 0;
    const auto invalidIndices = m_uploadArena.Allocate(
        sizeof(BindlessIndicesConstants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
    if (invalidIndices) {
        BindlessIndicesConstants blank;
        blank.Reset();
        std::memcpy(invalidIndices.cpu, &blank, sizeof(blank));
        m_invalidBindlessAddress = invalidIndices.gpu;
    }
}

void DX12Renderer::EndFrame()
{
    // 呼び出し側が閉じ忘れても、UAV 書き込みを未同期のまま Submit しない。
    EndComputeBatch();
    const bool recordingTimestamps = m_gpuProfilerRecording;
    const uint32_t frameIndex = m_gpuProfilerFrame;
    if (recordingTimestamps) {
        ResolveFrameTimestamps(frameIndex);
    }
    m_gpuProfilerRecording = false;

    // コマンドリストを閉じる前に、残っている PIX イベントを内側から閉じる。
    auto* commands = m_context.GetCommandList();
    m_context.EndPixEvent(commands, m_pixPassToken);
    m_context.EndPixEvent(commands, m_pixViewToken);
    m_context.EndPixEvent(commands, m_pixFrameToken);
    m_pixPassToken = 0;
    m_pixViewToken = 0;
    m_pixFrameToken = 0;
    m_pixViewName.clear();

    // 最終提出のフェンスを、変形済み頂点の再利用とクエリの読み戻しの基準にする。
    const uint64_t submittedFence = m_context.EndFrame();
    EndPreparedSkinningFrame(m_preparedSkinningPool, submittedFence);
    if (recordingTimestamps) {
        m_gpuProfilerLedger.SubmitFrame(frameIndex, submittedFence);
    }
    ReportFrameDiagnostics(frameIndex);
}

void DX12Renderer::ResolveFrameTimestamps(uint32_t frameIndex)
{
    const uint32_t passCount = m_gpuProfilerLedger.FinishFrame();
    if (passCount == 0 || !m_context.IsFrameOpen()) {
        return;
    }

    // 各フレームスロットに固定のクエリ範囲を割り当て、対応する読み戻し先へコピーする。
    /// @see https://learn.microsoft.com/en-us/windows/win32/direct3d12/queries Queries
    const uint32_t firstQuery = frameIndex * GPU_MAX_PASSES * TIMESTAMPS_PER_GPU_PASS;
    const uint32_t queryCount = passCount * TIMESTAMPS_PER_GPU_PASS;
    const auto readbackOffset = static_cast<UINT64>(firstQuery) * sizeof(uint64_t);
    m_context.GetCommandList()->ResolveQueryData(m_gpuQueryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
        firstQuery, queryCount, m_gpuReadback.Get(), readbackOffset);
}

void DX12Renderer::ReportFrameDiagnostics(uint32_t frameIndex)
{
    const auto usage = m_uploadArena.GetFrameUsage(frameIndex);
    const bool hasPreparationFailures = usage.failedAllocations != 0
        || m_rejectedDrawPreparations != 0 || m_rejectedDispatchPreparations != 0;
    if (hasPreparationFailures) {
        FBZZ_LOG_WARN("DX12 Upload: recordedDraws=%llu recordedDispatches=%llu rejectedDraws=%llu "
                      "rejectedDispatches=%llu failedAllocations=%zu "
                      "failedRequestBytes=%zu largestFailedRequestBytes=%zu usedBytes=%zu committedBytes=%zu",
                      static_cast<unsigned long long>(m_recordedDraws), static_cast<unsigned long long>(m_recordedDispatches),
                      static_cast<unsigned long long>(m_rejectedDrawPreparations),
                      static_cast<unsigned long long>(m_rejectedDispatchPreparations), usage.failedAllocations,
                      usage.failedRequestBytes, usage.largestFailedRequestBytes, usage.usedBytes, usage.committedBytes);
    }
    if (!m_bindingDiagnostics) {
        return;
    }

    // 計測ツールが追跡するフレーム番号と、束縛の訪問・再利用・転送量を一行に出す。
    std::fprintf(stderr, "DX12_BINDING_SCAN_JSON {\"frame\":%llu,\"resourceOwner\":%llu,"
        "\"observedResourceFrame\":%llu,\"resourceCoherent\":%s,\"fused\":%s,\"visits\":%llu,"
        "\"uploads\":%llu,\"emptyReuses\":%llu,\"failures\":%llu,\"bindingSectionMs\":%.9f,"
        "\"uploadPagesUsed\":%zu,\"uploadResidentPages\":%zu,\"uploadUsedBytes\":%zu,"
        "\"uploadCommittedBytes\":%zu,\"failedAllocations\":%zu,\"failedRequestBytes\":%zu,"
        "\"recordedDraws\":%llu,\"recordedDispatches\":%llu,\"rejectedDraws\":%llu,\"rejectedDispatches\":%llu}\n",
        static_cast<unsigned long long>(m_gpuPhysicalFrameSerial), static_cast<unsigned long long>(m_bindingResourceOwner),
        static_cast<unsigned long long>(m_bindingResourceFrame), m_bindingResourceCoherent ? "true" : "false",
        m_bindingFusionEnabled ? "true" : "false", static_cast<unsigned long long>(m_bindingDraws),
        static_cast<unsigned long long>(m_bindingUploads), static_cast<unsigned long long>(m_emptyBindingReuses),
        static_cast<unsigned long long>(m_bindingFailures), m_bindingSectionMs, usage.pagesUsed, usage.residentPages,
        usage.usedBytes, usage.committedBytes, usage.failedAllocations, usage.failedRequestBytes,
        static_cast<unsigned long long>(m_recordedDraws), static_cast<unsigned long long>(m_recordedDispatches),
        static_cast<unsigned long long>(m_rejectedDrawPreparations),
        static_cast<unsigned long long>(m_rejectedDispatchPreparations));
}

void DX12Renderer::Clear(const math::Vector4& color)
{
    if (!m_context.IsFrameOpen()) {
        return;
    }
    if (IsCurrentRenderTargetLost()) {
        ReportLostRenderTarget("Clear");
        return;
    }
    const float clearColor[] = {color.x, color.y, color.z, color.w};
    if (m_currentColorAttachmentCount != 0) {
        for (uint32_t index = 0; index < m_currentColorAttachmentCount; ++index) {
            const auto* target = ResolveCurrentColorAttachment(index);
            m_context.GetCommandList()->ClearRenderTargetView(
                target->GetRtv(m_currentColorAttachments[index].colorIndex), clearColor, 0, nullptr);
        }
        return;
    }
    if (DX12RenderTarget* const target = ResolveCurrentRenderTarget()) {
        if (target->IsCubemap() && m_currentCubeRtv.ptr) {
            m_context.GetCommandList()->ClearRenderTargetView(m_currentCubeRtv, clearColor, 0, nullptr);
            m_context.GetCommandList()->ClearDepthStencilView(target->GetDsv(m_currentCubeMip),
                D3D12_CLEAR_FLAG_DEPTH, FarDepth(target->IsReversedZ()), 0, 0, nullptr);
            return;
        }
        for (uint32_t index = 0; index < target->GetColorCount(); ++index) {
            m_context.GetCommandList()->ClearRenderTargetView(
                target->GetRtv(index), clearColor, 0, nullptr);
        }
        // IRenderer::Clear は深度も最遠値へ戻す契約。RenderSystem は Clear(色) しか呼ばないので、
        // 残すと前の値のまま深度比較が全滅する。
        if (target->HasDepth()) {
            m_context.GetCommandList()->ClearDepthStencilView(
                target->GetDsv(), D3D12_CLEAR_FLAG_DEPTH, FarDepth(target->IsReversedZ()), 0, 0, nullptr);
        }
    } else {
        m_context.MarkBackBufferWritten();
        m_context.GetCommandList()->ClearRenderTargetView(m_context.GetCurrentRtv(), clearColor, 0, nullptr);
        // バックバッファも DX11 と同じく色クリア時に深度を 1.0 へ戻す。
        m_context.GetCommandList()->ClearDepthStencilView(
            m_context.GetDsv(), D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    }
}

void DX12Renderer::ClearDepth()
{
    ClearDepthRegion(0, 0, 0, 0);
}

/// @see https://learn.microsoft.com/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-cleardepthstencilview D3D12 ClearDepthStencilView の矩形指定。
void DX12Renderer::ClearDepthRegion(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
    if (m_currentColorAttachmentCount != 0) {
        return;
    }
    if (!m_context.IsFrameOpen()) {
        return;
    }
    if (IsCurrentRenderTargetLost()) {
        ReportLostRenderTarget("ClearDepth");
        return;
    }

    // ターゲット専用の深度面を選ぶ。既定の描画先なら画面が更新されたことも記録する。
    DX12RenderTarget* const target = ResolveCurrentRenderTarget();
    if (target && !target->HasDepth()) {
        return;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE depthView{};
    if (target) {
        const uint32_t depthMip = target->IsCubemap() ? m_currentCubeMip : 0;
        depthView = target->GetDsv(depthMip);
    } else {
        m_context.MarkBackBufferWritten();
        depthView = m_context.GetDsv();
    }

    // 矩形が空の指定は D3D12 へ矩形数 0 として渡し、深度面全体を遠端の値で消去する。
    const float farDepth = FarDepth(target && target->IsReversedZ());
    const D3D12_RECT region{static_cast<LONG>(x), static_cast<LONG>(y),
                            static_cast<LONG>(x + width), static_cast<LONG>(y + height)};
    const bool partial = width != 0 && height != 0;
    m_context.GetCommandList()->ClearDepthStencilView(
        depthView, D3D12_CLEAR_FLAG_DEPTH, farDepth, 0, partial ? 1u : 0u, partial ? &region : nullptr);
}

DX12RenderTarget* DX12Renderer::ResolveCurrentRenderTarget(ResourceManager* resources) const
{
    if (!m_currentRenderTargetHandle.IsValid()) {
        return nullptr;
    }
    auto* owner = resources ? resources : FindLiveResourceManager(m_currentRenderTargetOwner);
    if (!owner || owner->GetInstanceId() != m_currentRenderTargetOwner) {
        return nullptr;
    }
    auto* base = owner->Get(m_currentRenderTargetHandle);
    return base ? static_cast<DX12RenderTarget*>(base) : nullptr;
}

DX12RenderTarget* DX12Renderer::ResolveCurrentColorAttachment(uint32_t index, ResourceManager* resources) const
{
    if (index >= m_currentColorAttachmentCount) {
        return nullptr;
    }
    auto* owner = resources ? resources : FindLiveResourceManager(m_currentRenderTargetOwner);
    if (!owner || owner->GetInstanceId() != m_currentRenderTargetOwner) {
        return nullptr;
    }
    const auto& attachment = m_currentColorAttachments[index];
    auto* base = owner->Get(attachment.target);
    auto* target = base ? static_cast<DX12RenderTarget*>(base) : nullptr;
    return target && !target->IsCubemap() && attachment.colorIndex < target->GetColorCount() ? target : nullptr;
}

void DX12Renderer::TransitionCurrentTargetForRead()
{
    if (m_currentColorAttachmentCount != 0) {
        for (uint32_t index = 0; index < m_currentColorAttachmentCount; ++index) {
            if (auto* target = ResolveCurrentColorAttachment(index)) {
                m_stateTracker.QueueTransition(target->GetColorResource(m_currentColorAttachments[index].colorIndex),
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            }
        }
        return;
    }
    if (auto* target = ResolveCurrentRenderTarget()) {
        if (target->IsCubemap()) {
            m_stateTracker.QueueTransition(target->GetCubeResource(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
        else {
            for (uint32_t index = 0; index < target->GetColorCount(); ++index) {
                m_stateTracker.QueueTransition(target->GetColorResource(index), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            }
        }
        if (target->HasDepth()) {
            m_stateTracker.QueueTransition(target->GetDepthResource(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
    }
}

bool DX12Renderer::IsCurrentRenderTargetLost(ResourceManager* resources) const
{
    for (uint32_t index = 0; index < m_currentColorAttachmentCount; ++index) {
        if (!ResolveCurrentColorAttachment(index, resources)) {
            return true;
        }
    }
    return m_currentRenderTargetHandle.IsValid() && ResolveCurrentRenderTarget(resources) == nullptr;
}

void DX12Renderer::ReportLostRenderTarget(const char* where)
{
    if (m_reportedLostRenderTarget) {
        return;
    }
    m_reportedLostRenderTarget = true;
    FBZZ_LOG_WARN("DX12Renderer::%s: 束縛中の RT が解放済み (handle=%u:%u)。"
                  "SetRenderTarget し直すまで描画を捨てます",
                  where, m_currentRenderTargetHandle.id, m_currentRenderTargetHandle.gen);
}

bool DX12Renderer::PrepareShaderReload()
{
    // Flush は提出済みのコマンドだけを待つ。記録中の PSO を解放してはならない。
    if (m_context.IsFrameOpen()) {
        FBZZ_LOG_WARN("DX12Renderer: shader reload rejected while a frame is recording");
        return false;
    }
    m_context.Flush();
    m_psoCache.ClearPipelines();
    InvalidateRootCbvCache();
    return true;
}

struct DX12Renderer::PreparedDraw {
    const prepared_skinning_detail::NativeWork* work = nullptr; ///< チケットが保持する変形結果への借用参照。
    ID3D12Resource* resource = nullptr; ///< 選択した行の変形済み頂点を格納する GPU 資源。
    uint32_t row = 0; ///< チケット内の変形済み出力の行番号。

    /// @brief 頂点・メッシュ段の raw SRV に同じ変形結果を使うか調べる。
    /// @param[in] handle 描画が raw SRV として指定した頂点バッファ。
    bool UsesRawOutput(ResourceHandle<BufferTag> handle) const
    {
        return work && row < work->writes.size() && handle == work->writes[row].output;
    }
};

void DX12Renderer::Submit(const DrawCall& call, ResourceManager& resources)
{
    (void)TrySubmitDraw(call, resources);
}

bool DX12Renderer::TrySubmitDraw(const DrawCall& call, ResourceManager& resources)
{
    if (!m_context.IsFrameOpen()) {
        return false;
    }

    PreparedDraw skinning;
    if (!ResolvePreparedDraw(call, skinning)) {
        return false;
    }

    auto* shader = static_cast<DX12Shader*>(resources.Get(call.shader));
    auto* state = static_cast<DX12PipelineState*>(resources.Get(call.pipelineState));
    if (!shader || !state) {
        if (!m_reportedMissingDrawResource) {
            FBZZ_LOG_WARN("DX12Renderer::Submit: 描画リソース不足 "
                          "(shader=%u:%u valid=%d, pso=%u:%u valid=%d)",
                          call.shader.id, call.shader.gen, shader != nullptr,
                          call.pipelineState.id, call.pipelineState.gen, state != nullptr);
            m_reportedMissingDrawResource = true;
        }
        return false;
    }

    // 描画命令を変える条件と、リソースの競合を記録前に確認する。
    const bool meshDispatch = call.meshGroupCountX > 0;
    if (!ValidateDrawTopology(call, *shader)) {
        return false;
    }
    if (!ValidateDrawReads(call, resources, skinning, meshDispatch)) {
        return false;
    }
    ID3D12CommandSignature* indirectSignature = nullptr;
    if (call.indirectArguments.IsValid()) {
        indirectSignature = ResolveIndirectSignature(call, meshDispatch);
        if (!ValidateIndirectDraw(call, resources, meshDispatch, indirectSignature)) {
            return false;
        }
    }
    if (IsCurrentRenderTargetLost(&resources)) {
        ReportLostRenderTarget("Submit");
        return false;
    }
    const auto* currentTarget = ResolveCurrentRenderTarget(&resources);
    if (!ValidateDrawColorReads(call, resources)) {
        return false;
    }

    // 描画先の形式に合う PSO と、要求されたコマンドを記録できるリストを選ぶ。
    auto* pipeline = ResolveDrawPipeline(call, *shader, *state, currentTarget);
    if (!pipeline) {
        return false;
    }
    ID3D12GraphicsCommandList* commands = m_context.GetCommandList();
    if (!commands) {
        FBZZ_LOG_ERROR("DX12Renderer::Submit: graphics command list is unavailable");
        return false;
    }
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList6> meshCommands;
    const bool needsDirectMeshList = meshDispatch && !indirectSignature;
    if (needsDirectMeshList && FAILED(commands->QueryInterface(IID_PPV_ARGS(&meshCommands)))) {
        FBZZ_LOG_ERROR("DX12Renderer::Submit: Mesh Shader command list interface is unavailable");
        return false;
    }

    // メインビュー外の Probe・Sky キャプチャも、PIX から面とシェーダーを追えるようにする。
    const std::string cubeEventName = m_currentCubeRtv.ptr != 0
        ? "CubeCapture target=" + std::to_string(m_currentRenderTargetHandle.id)
            + ":" + std::to_string(m_currentRenderTargetHandle.gen)
            + " face=" + std::to_string(m_currentCubeFace)
            + " mip=" + std::to_string(m_currentCubeMip) + " shader=" + shader->GetPath()
        : std::string{};
    DX12ScopedPixEvent cubeEvent(m_context, cubeEventName.empty() ? nullptr : commands,
                                 cubeEventName, PIX_CUBE_CAPTURE_COLOR);

    // 定数・添字・頂点入力がそろった要求から、GPU が実行する描画命令を記録する。
    if (!PrepareDrawBindings(*commands, *pipeline, call, resources, skinning, *shader, meshDispatch)) {
        ++m_rejectedDrawPreparations;
        return false;
    }

    const bool recorded = RecordDraw(*commands, meshCommands.Get(), indirectSignature, call, resources, skinning,
                                     currentTarget == nullptr);
    if (recorded) {
        ++m_recordedDraws;
    }
    return recorded;
}

bool DX12Renderer::PrepareDrawBindings(ID3D12GraphicsCommandList& commands, ID3D12PipelineState& pipeline,
    const DrawCall& call, ResourceManager& resources, const PreparedDraw& skinning,
    const DX12Shader& shader, bool meshDispatch)
{
    BindDrawPipeline(commands, pipeline);
    if (!BindDrawConstants(commands, call, resources)) {
        return false;
    }
    if (!BindDrawResources(commands, call, resources, skinning, meshDispatch)) {
        // 添字を束縛できなくても、そこまでに積んだ遷移を同じコマンドリストで確定する。
        m_stateTracker.FlushBarriers(&commands);
        return false;
    }

    // 頂点・メッシュシェーダーが参照する資源の遷移を、描画命令より先に記録する。
    QueueDrawVertexReads(call, resources, skinning);
    m_stateTracker.FlushBarriers(&commands);
    if (meshDispatch) {
        return true;
    }

    // 通常描画は入力アセンブラーで頂点とインデックスを読む。
    if (!BindDrawInputAssembler(commands, call, resources, skinning)) {
        return false;
    }
    WarnDrawVertexStride(call, resources, shader, skinning);
    return true;
}

bool DX12Renderer::ResolvePreparedDraw(const DrawCall& call, PreparedDraw& skinning) const
{
    // スキニング済み出力は同じデバイスの有効な行だけを描画に使う。
    if (!call.preparedSkinning) {
        return true;
    }
    const auto& work = prepared_skinning_detail::TicketFactory::Native(*call.preparedSkinning);
    const auto row = call.preparedSkinningRow;
    const auto* resource = static_cast<const ID3D12Resource*>(work.OutputResource(row));
    if (!resource || work.DeviceIdentity() != m_context.GetDevice()) {
        return false;
    }
    skinning = {&work, const_cast<ID3D12Resource*>(resource), row};
    return true;
}

bool DX12Renderer::RecordDraw(ID3D12GraphicsCommandList& commands, ID3D12GraphicsCommandList6* meshCommands,
                              ID3D12CommandSignature* indirectSignature, const DrawCall& call,
                              ResourceManager& resources, const PreparedDraw& skinning, bool usesBackBuffer)
{
    // 間接描画と通常描画で共用する b15 の初期値をそろえる。
    /// @see https://github.com/microsoft/DirectX-Specs/blob/master/d3d/IndirectDrawing.md State leakage.
    commands.SetGraphicsRoot32BitConstant(kIndirectInstanceOffsetRootParam, 0, 0);
    if (skinning.work && !call.preparedSkinning->IsWriteCurrent(skinning.row)) {
        return false;
    }
    if (usesBackBuffer) {
        m_context.MarkBackBufferWritten();
    }

    // GPU 引数、直接 Mesh dispatch、通常の IA 描画の順に記録経路を選ぶ。
    if (indirectSignature) {
        auto* argumentBuffer = static_cast<DX12StructuredBuffer*>(resources.Get(call.indirectArguments));
        // 引数の解決を挟むため、間接命令を積む直前にも変形済み出力の版を照合する。
        if (skinning.work && !call.preparedSkinning->IsWriteCurrent(skinning.row)) {
            return false;
        }
        /// @see https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-executeindirect ExecuteIndirect argument buffer state and range.
        commands.ExecuteIndirect(indirectSignature, call.indirectCommandCount, argumentBuffer->GetResource(),
                                  call.indirectArgumentOffset, nullptr, 0);
        return true;
    }
    if (meshCommands) {
        meshCommands->DispatchMesh(call.meshGroupCountX, call.meshGroupCountY, call.meshGroupCountZ);
        return true;
    }

    const uint32_t instanceCount = call.instanceCount == 0 ? 1 : call.instanceCount;
    if (call.indexCount > 0) {
        commands.DrawIndexedInstanced(call.indexCount, instanceCount, call.startIndex,
                                       static_cast<INT>(call.baseVertex), 0);
        return true;
    }
    if (call.vertexCount > 0) {
        commands.DrawInstanced(call.vertexCount, instanceCount, 0, 0);
        return true;
    }
    return false;
}

bool DX12Renderer::ValidateDrawTopology(const DrawCall& call, const DX12Shader& shader) const
{
    const bool meshDispatch = call.meshGroupCountX > 0;
    if (meshDispatch != shader.IsMeshShader()) {
        FBZZ_LOG_ERROR("DX12Renderer::Submit: shader stage and meshGroupCountX disagree (shader=%s)",
                       shader.GetPath().c_str());
        return false;
    }

    if (!ValidateDrawIndirectParameters(call, meshDispatch)) {
        return false;
    }

    // メッシュシェーダーの経路ではリスト形式に加え、IA の束縛とワークグループの制限を確認する。
    const bool isListTopology = call.topology == PrimitiveTopology::TRIANGLE_LIST
        || call.topology == PrimitiveTopology::LINE_LIST;
    if (meshDispatch) {
        return ValidateMeshDispatch(call, isListTopology);
    }

    // IA の経路では、メッシュ用の Y・Z 指定や間接描画のストリップ指定を拒否する。
    const bool hasAdditionalMeshGroups = call.meshGroupCountY != 0 || call.meshGroupCountZ != 0;
    if (hasAdditionalMeshGroups) {
        FBZZ_LOG_ERROR("DX12Renderer::Submit: mesh group Y/Z set without a mesh dispatch");
        return false;
    }
    const bool isDirectTriangleStrip = call.topology == PrimitiveTopology::TRIANGLE_STRIP
        && !call.indirectArguments.IsValid();
    if (isListTopology || isDirectTriangleStrip) {
        return true;
    }
    FBZZ_LOG_ERROR("DX12Renderer::Submit: invalid primitive topology for draw");
    return false;
}

bool DX12Renderer::ValidateMeshDispatch(const DrawCall& call, bool isListTopology) const
{
    const bool hasInputAssemblerBuffers = call.vertexBuffer.IsValid() || call.indexBuffer.IsValid();
    if (!m_context.SupportsMeshShaders() || !isListTopology || hasInputAssemblerBuffers) {
        FBZZ_LOG_ERROR("DX12Renderer::Submit: unsupported mesh shader capability, topology or IA binding");
        return false;
    }
    if (call.indirectArguments.IsValid()) {
        return true;
    }

    const auto fitsAxis = [](uint32_t groupCount) {
        return groupCount > 0 && groupCount <= D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION;
    };
    if (!fitsAxis(call.meshGroupCountX) || !fitsAxis(call.meshGroupCountY) || !fitsAxis(call.meshGroupCountZ)) {
        FBZZ_LOG_ERROR("DX12Renderer::Submit: direct mesh group counts exceed D3D12 limits");
        return false;
    }
    constexpr uint64_t MAX_MESH_GROUPS_PER_DISPATCH = 1u << 22u;
    const uint64_t totalGroups = static_cast<uint64_t>(call.meshGroupCountX) * call.meshGroupCountY * call.meshGroupCountZ;
    if (totalGroups > MAX_MESH_GROUPS_PER_DISPATCH) {
        FBZZ_LOG_ERROR("DX12Renderer::Submit: direct mesh group count product exceeds D3D12 limits");
        return false;
    }
    return true;
}

bool DX12Renderer::ValidateDrawReads(const DrawCall& call, ResourceManager& resources,
                                     const PreparedDraw& skinning, bool meshDispatch) const
{
    // raw SRV を指定したスロットだけ照合する。準備済み出力はチケット側が記述子を保持する。
    constexpr uint32_t FIRST_RAW_VERTEX_REGISTER = 14;
    for (uint32_t slot = 0; slot < call.vsRawBuffers.size(); ++slot) {
        const auto rawHandle = call.vsRawBuffers[slot];
        if (!rawHandle.IsValid()) {
            continue;
        }
        if (call.vsBuffers[slot].IsValid()) {
            FBZZ_LOG_ERROR("DX12Renderer::Submit: raw and structured VS buffer conflict at t%u",
                           FIRST_RAW_VERTEX_REGISTER + slot);
            return false;
        }
        if (skinning.UsesRawOutput(rawHandle)) {
            continue;
        }
        const auto* rawBuffer = resources.Get(rawHandle);
        if (!rawBuffer || rawBuffer->GetBindlessSrvIndex() == INVALID_BINDLESS_INDEX) {
            FBZZ_LOG_ERROR("DX12Renderer::Submit: invalid or unreadable raw VS/MS buffer at t%u",
                           FIRST_RAW_VERTEX_REGISTER + slot);
            return false;
        }
    }
    if (call.meshletSelection.IsValid()) {
        const auto* selection = resources.Get(call.meshletSelection);
        if (!meshDispatch || !selection || selection->GetBindlessIndex() == INVALID_BINDLESS_INDEX) {
            return false;
        }
    }
    for (const auto handle : call.meshletPages) {
        if (!meshDispatch || !resources.Get(handle)) {
            return false;
        }
    }
    for (const auto handle : call.indirectReadBuffers) {
        const auto* buffer = resources.Get(handle);
        if (!m_context.SupportsBindless() || !buffer || buffer->GetBindlessSrvIndex() == INVALID_BINDLESS_INDEX) {
            FBZZ_LOG_ERROR("DX12Renderer::Submit: stale or unreadable indirect vertex buffer");
            return false;
        }
    }
    return true;
}

ID3D12CommandSignature* DX12Renderer::ResolveIndirectSignature(const DrawCall& call, bool meshDispatch) const
{
    if (meshDispatch) {
        return m_context.GetDispatchMeshCommandSignature();
    }
    if (call.indirectInstanceOffset) {
        return m_psoCache.GetIndexedInstanceOffsetCommandSignature();
    }
    return m_context.GetDrawIndexedCommandSignature();
}

bool DX12Renderer::ValidateIndirectDraw(const DrawCall& call, ResourceManager& resources, bool meshDispatch,
                                        const ID3D12CommandSignature* signature) const
{
    // GPU への間接命令に必要なキュー側の対応と、引数バッファの基本指定を確認する。
    const bool hasRequiredCapability = meshDispatch ? m_context.SupportsMeshShaders() : call.indexBuffer.IsValid();
    const bool hasAlignedOffset = call.indirectArgumentOffset % alignof(uint32_t) == 0;
    const bool hasExecutableRange = call.indirectCommandCount != 0 && hasAlignedOffset;
    const bool canExecuteSignature = signature && hasRequiredCapability;
    if (!hasExecutableRange || !canExecuteSignature) {
        FBZZ_LOG_ERROR("DX12Renderer::Submit: invalid indirect command count/alignment/capability/binding");
        return false;
    }
    const auto* argumentBuffer = static_cast<const DX12StructuredBuffer*>(resources.Get(call.indirectArguments));
    if (!argumentBuffer) {
        FBZZ_LOG_ERROR("DX12Renderer::Submit: stale indirect argument buffer handle");
        return false;
    }

    // インスタンス補正付きのインデックス描画は、標準の引数にルート定数一つを含む。
    uint32_t argumentStride = sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
    if (meshDispatch) {
        argumentStride = sizeof(D3D12_DISPATCH_MESH_ARGUMENTS);
    } else if (call.indirectInstanceOffset) {
        argumentStride += sizeof(uint32_t);
    }
    if (!ValidateIndirectArgumentLayout(call, *argumentBuffer, argumentStride)) {
        return false;
    }

    // 引数バッファを同じ描画の SRV にも束縛すると、間接引数用の状態と競合する。
    const auto readsArgumentBuffer = [&](const auto& handles) {
        return std::find(handles.begin(), handles.end(), call.indirectArguments) != handles.end();
    };
    const bool alsoReadsArgumentsAsSrv = call.instanceBuffer == call.indirectArguments || readsArgumentBuffer(call.vsBuffers)
        || readsArgumentBuffer(call.psBuffers) || readsArgumentBuffer(call.auxiliaryPsBuffers);
    if (alsoReadsArgumentsAsSrv) {
        FBZZ_LOG_ERROR("DX12Renderer::Submit: indirect argument buffer cannot be shader-readable in the same draw");
        return false;
    }
    if (meshDispatch) {
        return true;
    }

    const auto* indexBuffer = static_cast<const DX12Buffer*>(resources.Get(call.indexBuffer));
    if (indexBuffer && indexBuffer->GetKind() == DX12Buffer::Kind::Index) {
        return true;
    }
    FBZZ_LOG_ERROR("DX12Renderer::Submit: DRAW_INDEXED indirect draw requires a valid index buffer");
    return false;
}

bool DX12Renderer::ValidateDrawColorReads(const DrawCall& call, ResourceManager& resources) const
{
    if (m_currentColorAttachmentCount == 0) {
        return true;
    }

    const auto overlapsOutput = [&](ResourceHandle<TextureTag> handle) {
        const auto* textureBase = resources.Get(handle);
        if (!textureBase) {
            return false;
        }
        const auto* input = static_cast<const DX12Texture*>(textureBase)->GetResource();
        for (uint32_t index = 0; index < m_currentColorAttachmentCount; ++index) {
            if (input == ResolveCurrentColorAttachment(index, &resources)->GetColorResource(
                m_currentColorAttachments[index].colorIndex)) {
                return true;
            }
        }
        return false;
    };
    if (std::any_of(call.textures.begin(), call.textures.end(), overlapsOutput)
        || std::any_of(call.indirectReadTextures.begin(), call.indirectReadTextures.end(), overlapsOutput)) {
        FBZZ_LOG_ERROR("DX12Renderer::Submit: color attachment cannot be shader-readable in the same draw");
        return false;
    }
    return true;
}

ID3D12PipelineState* DX12Renderer::ResolveDrawPipeline(const DrawCall& call, const DX12Shader& shader,
                                                      const DX12PipelineState& state, const DX12RenderTarget* target)
{
    // MRT では明示した色の枚数を優先し、通常の RT とバックバッファは各自の記述を使う。
    const DXGI_FORMAT colorFormat = target ? target->GetColorFormat() : DX12Context::BACK_BUFFER_FORMAT;
    uint32_t colorCount = m_currentColorAttachmentCount;
    if (colorCount == 0) {
        colorCount = target ? target->GetColorCount() : 1;
    }
    const bool reversedZ = m_currentColorAttachmentCount == 0 && target && target->IsReversedZ();
    if (call.meshGroupCountX > 0) {
        return m_psoCache.GetOrCreateMesh(shader, state.GetDesc(), call.topology, colorFormat, colorCount, reversedZ);
    }
    return m_psoCache.GetOrCreate(shader, state.GetDesc(), call.topology, colorFormat, colorCount, reversedZ);
}

void DX12Renderer::BindDrawPipeline(ID3D12GraphicsCommandList& commands, ID3D12PipelineState& pipeline)
{
    // 共有コマンドリストへ他所 (ImGui / IblBaker) が記録していたら状態キャッシュを捨てる。
    if (m_seenPipelineStateGeneration != m_context.GetPipelineStateGeneration()) {
        InvalidateRootCbvCache();
        m_seenPipelineStateGeneration = m_context.GetPipelineStateGeneration();
    }

    ID3D12RootSignature* rootSignature = m_psoCache.GetRootSignature();
    const bool signatureChanged = m_lastGraphicsRootSignature != rootSignature;
    if (signatureChanged) {
        InvalidateRootCbvCache();
    }
    // 直接索引するルートシグネチャは、先に CBV/SRV/UAV ヒープを束縛する必要がある。
    if (ID3D12DescriptorHeap* srvHeap = m_context.GetResourceSrvHeap();
        m_lastDescriptorHeap != srvHeap)
    {
        ID3D12DescriptorHeap* heaps[] = {srvHeap};
        commands.SetDescriptorHeaps(1, heaps);
        m_lastDescriptorHeap = srvHeap;
    }

    // ルートシグネチャは変化したときだけ設定する。SetGraphicsRootSignature は全ルート引数を
    // 無効化する契約のため、毎 Draw 呼ぶと直後の root CBV 差分キャッシュの前提が崩れる。
    if (signatureChanged) {
        // 順序に注意: 無効化はルート引数のキャッシュを捨てると同時に直前値も nullptr へ戻すため、
        // 先に無効化してから「今設定した」ことを記録する。
        commands.SetGraphicsRootSignature(rootSignature);
        m_lastGraphicsRootSignature = rootSignature;
    }
    if (m_lastPipelineState != &pipeline) {
        commands.SetPipelineState(&pipeline);
        m_lastPipelineState = &pipeline;
    }
}

bool DX12Renderer::BindDrawResources(ID3D12GraphicsCommandList& commands, const DrawCall& call,
                                     ResourceManager& resources, const PreparedDraw& skinning, bool meshDispatch)
{
    // 同じ添字でも Compute の書き込みで資源状態は変わるため、Draw ごとに遷移を確認する。
    // Mesh Shader が読むテクスチャは、ピクセル段とメッシュ段の両方で読める状態にする。
    /// @see https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_resource_states D3D12 shader-resource states.
    const D3D12_RESOURCE_STATES graphicsShaderResourceState = meshDispatch
        ? D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE
        : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    BindlessIndicesConstants indices;
    indices.Reset();
    std::chrono::steady_clock::time_point diagnosticBegin;
    if (m_bindingDiagnostics) {
        diagnosticBegin = std::chrono::steady_clock::now();
        if (m_bindingDraws == 0) {
            m_bindingResourceOwner = resources.GetInstanceId();
            m_bindingResourceFrame = resources.FrameStamp();
        } else if (m_bindingResourceOwner != resources.GetInstanceId() || m_bindingResourceFrame != resources.FrameStamp()) {
            m_bindingResourceCoherent = false;
        }
        ++m_bindingDraws;
    }

    // 遷移と添字取得をまとめる設定でも、間接参照は状態だけを追跡する。
    if (m_bindingFusionEnabled) {
        PrepareDrawPixelReads<true>(call, resources, m_stateTracker, graphicsShaderResourceState, indices);
    } else {
        PrepareDrawPixelReads<false>(call, resources, m_stateTracker, graphicsShaderResourceState, indices);
        CollectDrawPixelIndices(call, resources, indices);
    }
    CollectDrawVertexIndices(call, resources, skinning, indices);
    const bool bound = BindDrawBindingIndices(commands, indices);

    if (m_bindingDiagnostics) {
        m_bindingSectionMs += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - diagnosticBegin).count();
    }
    return bound;
}

void DX12Renderer::CollectDrawVertexIndices(const DrawCall& call, ResourceManager& resources,
                                             const PreparedDraw& skinning, BindlessIndicesConstants& indices)
{
    if (auto* instanceBase = resources.Get(call.instanceBuffer)) {
        indices.vertex[vsSlot::INSTANCE] = instanceBase->GetBindlessIndex();
    }
    if (auto* selection = resources.Get(call.meshletSelection)) {
        indices.vertex[vsSlot::MESHLET_SELECTION] = selection->GetBindlessIndex();
    }
    for (uint32_t i = 0; i < call.vsBuffers.size(); ++i) {
        if (auto* bufferBase = resources.Get(call.vsBuffers[i])) {
            indices.vertex[vsSlot::BUFFER0 + i] = bufferBase->GetBindlessIndex();
        }
        if (skinning.UsesRawOutput(call.vsRawBuffers[i])) {
            indices.vertex[vsSlot::BUFFER0 + i] = skinning.work->OutputSrv(skinning.row);
        }
        else if (auto* bufferBase = resources.Get(call.vsRawBuffers[i])) {
            indices.vertex[vsSlot::BUFFER0 + i] = bufferBase->GetBindlessSrvIndex();
        }
    }
}

bool DX12Renderer::BindDrawBindingIndices(ID3D12GraphicsCommandList& commands,
                                             const BindlessIndicesConstants& indices)
{
    if (m_invalidBindlessAddress != 0 && indices.IsEmpty()) {
        commands.SetGraphicsRootConstantBufferView(kBindlessIndicesRootParam, m_invalidBindlessAddress);
        if (m_bindingDiagnostics) {
            ++m_emptyBindingReuses;
        }
        return true;
    }

    const auto block = m_uploadArena.Allocate(
        sizeof(BindlessIndicesConstants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
    if (block) {
        if (m_bindingDiagnostics) {
            ++m_bindingUploads;
        }
        std::memcpy(block.cpu, &indices, sizeof(indices));
        commands.SetGraphicsRootConstantBufferView(kBindlessIndicesRootParam, block.gpu);
        return true;
    }

    if (m_bindingDiagnostics) {
        ++m_bindingFailures;
    }
    return false;
}

void DX12Renderer::QueueDrawVertexReads(const DrawCall& call, ResourceManager& resources,
                                        const PreparedDraw& skinning)
{
    // 状態遷移は bindless でも必ず要る。添字が同じでも、間に挟まった別パス
    // (Compute の UAV 書き込み等) でリソースの状態は変わっている。
    if (auto* instanceBase = resources.Get(call.instanceBuffer)) {
        m_stateTracker.QueueTransition(
            static_cast<DX12StructuredBuffer*>(instanceBase)->GetResource(),
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    for (const auto& handle : call.vsBuffers) {
        if (auto* bufferBase = resources.Get(handle)) {
            m_stateTracker.QueueTransition(
                static_cast<DX12StructuredBuffer*>(bufferBase)->GetResource(),
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
    }
    for (const auto handle : call.vsRawBuffers) {
        if (skinning.UsesRawOutput(handle)) {
            m_stateTracker.QueueTransition(skinning.resource,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            m_context.RetainPreparedSkinning(call.preparedSkinning);
            continue;
        }
        if (auto* bufferBase = resources.Get(handle)) {
            auto* buffer = static_cast<DX12Buffer*>(bufferBase);
            // Upload-heap snapshots are already GENERIC_READ; GPU-written DEFAULT resources need an explicit SRV transition.
            if (buffer->IsGpuWritable()) {
                m_stateTracker.QueueTransition(buffer->GetSrvResource(),
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            }
        }
    }
    for (const auto handle : call.indirectReadBuffers) {
        auto* buffer = static_cast<DX12Buffer*>(resources.Get(handle));
        if (buffer->IsGpuWritable()) {
            m_stateTracker.QueueTransition(buffer->GetSrvResource(),
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
    }
    if (auto* selection = resources.Get(call.meshletSelection)) {
        m_stateTracker.QueueTransition(static_cast<DX12StructuredBuffer*>(selection)->GetResource(),
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    for (const auto handle : call.meshletPages) {
        m_stateTracker.QueueTransition(static_cast<DX12StructuredBuffer*>(resources.Get(handle))->GetResource(),
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    if (auto* argumentBase = resources.Get(call.indirectArguments)) {
        m_stateTracker.QueueTransition(
            static_cast<DX12StructuredBuffer*>(argumentBase)->GetResource(),
            D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    }
}

bool DX12Renderer::BindDrawInputAssembler(ID3D12GraphicsCommandList& commands, const DrawCall& call,
                                          ResourceManager& resources, const PreparedDraw& skinning)
{
    D3D_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    if (call.topology == PrimitiveTopology::LINE_LIST) {
        topology = D3D_PRIMITIVE_TOPOLOGY_LINELIST;
    }
    if (call.topology == PrimitiveTopology::TRIANGLE_STRIP) {
        topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
    }
    commands.IASetPrimitiveTopology(topology);

    // スキニング済みの場合はチケットの頂点を使う。通常の描画は DrawCall の頂点バッファを使う。
    if (skinning.work) {
        const bool readsVerticesAsRaw = std::any_of(call.vsRawBuffers.begin(), call.vsRawBuffers.end(),
            [&](ResourceHandle<BufferTag> handle) { return skinning.UsesRawOutput(handle); });
        auto vertexState = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
        if (readsVerticesAsRaw) {
            vertexState |= D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        }
        m_stateTracker.Transition(&commands, skinning.resource, vertexState);
        const D3D12_VERTEX_BUFFER_VIEW view{
            skinning.work->OutputAddress(skinning.row), skinning.work->OutputBytes(skinning.row), sizeof(Vertex)};
        if (view.BufferLocation == 0) {
            return false;
        }
        m_context.RetainPreparedSkinning(call.preparedSkinning);
        commands.IASetVertexBuffers(0, 1, &view);
    } else if (auto* vertexBase = resources.Get(call.vertexBuffer)) {
        auto* vertexBuffer = static_cast<DX12Buffer*>(vertexBase);
        // Compute が書いた頂点を IA が読める状態へ移す。
        if (vertexBuffer->IsGpuWritable()) {
            m_stateTracker.Transition(&commands, vertexBuffer->GetResource(),
                                      D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
        }
        const auto view = vertexBuffer->GetVertexView(m_uploadArena);
        if (view.BufferLocation == 0) {
            return false;
        }
        commands.IASetVertexBuffers(0, 1, &view);
    }

    if (auto* indexBase = resources.Get(call.indexBuffer)) {
        const auto view = static_cast<DX12Buffer*>(indexBase)->GetIndexView(m_uploadArena);
        if (view.BufferLocation == 0) {
            return false;
        }
        commands.IASetIndexBuffer(&view);
    }
    return true;
}

bool DX12Renderer::BindDrawConstants(ID3D12GraphicsCommandList& commands, const DrawCall& call,
                                     ResourceManager& resources)
{
    // b0〜b13 を差分で束縛する (未指定スロットは null CBV で埋める契約は従来どおり)。
    // 毎 Draw 全スロット設定すると最大 28 回のルート設定が出るため、直前の GPU VA を
    // スロットごとに記憶し変化分だけ設定する。
    for (uint32_t slot = 0; slot < call.constantBuffers.size(); ++slot) {
        const auto address = ResolveConstantAddress(call.constantBuffers[slot], resources);
        if (address == 0) {
            if (m_bindingDiagnostics && m_rejectedDrawPreparations == 0) {
                const auto handle = call.constantBuffers[slot];
                const auto* shader = resources.Get(call.shader);
                FBZZ_LOG_WARN("DX12 Upload: draw constant b%u unavailable (handle=%u:%u shader=%s)",
                    slot, handle.id, handle.gen, shader ? shader->GetPath().c_str() : "");
            }
            return false;
        }
        if (m_rootCbvCacheValid && m_lastRootCbv[slot] == address) {
            continue;
        }
        commands.SetGraphicsRootConstantBufferView(slot, address);
        m_lastRootCbv[slot] = address;
    }
    m_rootCbvCacheValid = true;
    return true;
}

D3D12_GPU_VIRTUAL_ADDRESS DX12Renderer::ResolveConstantAddress(ResourceHandle<ConstantBufferTag> handle,
                                                              ResourceManager& resources) const
{
    if (!handle.IsValid()) {
        return m_nullConstantAddress;
    }
    auto* buffer = resources.Get(handle);
    if (!buffer) {
        return 0;
    }
    return static_cast<DX12ConstantBuffer*>(buffer)->PrepareForSubmit();
}

void DX12Renderer::WarnDrawVertexStride(const DrawCall& call, ResourceManager& resources,
                                        const DX12Shader& shader, const PreparedDraw& skinning)
{
    // レイアウトが要求するサイズが実頂点を超えた場合だけ警告する。
    // POSITION のみ読むシェーダーなど、実頂点の一部を読む場合は成立する。
    uint32_t actualStride = 0;
    if (skinning.work) {
        actualStride = sizeof(Vertex);
    } else if (const auto* vertex = resources.Get(call.vertexBuffer)) {
        actualStride = static_cast<const DX12Buffer*>(vertex)->GetStride();
    }
    const uint32_t reflectedStride = shader.GetReflectedStride();
    if (actualStride == 0 || reflectedStride <= actualStride) {
        return;
    }
    if (m_strideWarned.insert(&shader).second) {
        FBZZ_LOG_WARN("[DX12] 頂点ストライド不一致! shader=%s vb=%uB reflected=%uB "
                      "(入力レイアウトのオフセットが頂点境界をまたぎジオメトリが壊れます)",
                      shader.GetPath().c_str(), actualStride, reflectedStride);
    }
}

void DX12Renderer::Dispatch(const ComputeCall& call, ResourceManager& resources)
{
    (void)TryDispatch(call, resources);
}

bool DX12Renderer::TryDispatch(const ComputeCall& call, ResourceManager& resources)
{
    if (!m_context.IsFrameOpen()) {
        return false;
    }
    const bool hasWorkgroups = call.dispatchX > 0 && call.dispatchY > 0 && call.dispatchZ > 0;
    if (!hasWorkgroups) {
        return false;
    }

    // キューの制約がある束縛や、同じスロット・資源への競合を記録前に確認する。
    if (!ValidateComputeSpecialReads(call, resources)) {
        return false;
    }
    if (!ValidateComputeIndirectResources(call, resources)) {
        return false;
    }
    auto* shader = static_cast<DX12Shader*>(resources.Get(call.shader));
    if (!shader) {
        return false;
    }
    if (!shader->IsCompute()) {
        return false;
    }

    ID3D12PipelineState* pipeline = m_psoCache.GetOrCreateCompute(*shader);
    if (!pipeline) {
        return false;
    }
    ID3D12GraphicsCommandList* commands = RecordingList();
    if (!commands) {
        return false;
    }
    if (!ValidateComputeDescriptors(call, resources)) {
        return false;
    }

    // 定数とディスクリプタ添字の領域を確保してから、コマンドリストの状態を変える。
    std::array<D3D12_GPU_VIRTUAL_ADDRESS, bindingCount::CONSTANT_BUFFERS> constantAddresses{};
    if (!PrepareComputeConstants(call, resources, constantAddresses)) {
        ++m_rejectedDispatchPreparations;
        return false;
    }
    const auto indicesBlock = m_uploadArena.Allocate(
        sizeof(BindlessIndicesConstants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
    if (!indicesBlock) {
        ++m_rejectedDispatchPreparations;
        return false;
    }

    // 読み書きの状態をそろえ、今回の Dispatch が使う添字を渡す。
    BindComputePipeline(*commands, *pipeline, constantAddresses);
    QueueComputeReads(call, resources);
    const auto writtenResources = QueueComputeWrites(call, resources);
    const auto indices = CollectComputeIndices(call, resources);
    std::memcpy(indicesBlock.cpu, &indices, sizeof(indices));
    commands->SetComputeRootConstantBufferView(kBindlessIndicesRootParam, indicesBlock.gpu);

    const std::string computeEventName = (m_asyncComputeActive ? "ComputeQueue " : "ComputeDirect ")
        + m_pixViewName + " shader=" + shader->GetPath();
    DX12ScopedPixEvent computeEvent(m_context, commands, computeEventName, PIX_COMPUTE_COLOR);
    // 読み書きに必要な遷移をまとめて記録してから Dispatch する。
    m_stateTracker.FlushBarriers(commands);
    commands->Dispatch(call.dispatchX, call.dispatchY, call.dispatchZ);
    ++m_recordedDispatches;

    // 描画キュー上で書き込む頂点バッファを通知し、後続アクセスと UAV の書き込みを同期する。
    if (auto* output = resources.Get(call.uavVertexBuffer); output && !m_asyncComputeActive) {
        output->NotifyGpuWrite();
    }
    for (const auto handle : call.indirectWriteVertexBuffers) {
        resources.Get(handle)->NotifyGpuWrite();
    }
    SynchronizeComputeWrites(*commands, writtenResources);
    return true;
}

void DX12Renderer::BindComputePipeline(ID3D12GraphicsCommandList& commands, ID3D12PipelineState& pipeline,
    std::span<const D3D12_GPU_VIRTUAL_ADDRESS> constantAddresses)
{
    // 次の Draw は Compute が設定した PSO やヒープを引き継ぐため、グラフィクス側の束縛を設定し直す。
    InvalidateRootCbvCache();

    // 添字で直接参照するヒープは、Compute のルートシグネチャより先に束縛する。
    /// @see https://microsoft.github.io/DirectX-Specs/d3d/HLSL_SM_6_6_DynamicResources.html#setting-and-changing-descriptor-heaps-and-root-signatures Setting and Changing Descriptor Heaps and Root Signatures
    ID3D12DescriptorHeap* heaps[] = {m_context.GetResourceSrvHeap()};
    commands.SetDescriptorHeaps(1, heaps);
    commands.SetComputeRootSignature(m_psoCache.GetComputeRootSignature());
    commands.SetPipelineState(&pipeline);
    for (uint32_t slot = 0; slot < constantAddresses.size(); ++slot) {
        commands.SetComputeRootConstantBufferView(slot, constantAddresses[slot]);
    }
}

bool DX12Renderer::ValidateComputeSpecialReads(const ComputeCall& call, ResourceManager& resources) const
{
    if (!ValidateComputeRayBindings(call, resources)) {
        return false;
    }

    // 直接束縛する raw SRV は、同じスロットのテクスチャ、構造化バッファ、TLAS と重複できない。
    for (uint32_t slot = 0; slot < call.srvRawBuffers.size(); ++slot) {
        const auto handle = call.srvRawBuffers[slot];
        if (!handle.IsValid()) {
            continue;
        }
        const bool slotOccupied = call.srvInputs[slot].IsValid() || call.srvBuffers[slot].IsValid()
            || call.accelerationStructures[slot].IsValid();
        if (slotOccupied) {
            FBZZ_LOG_ERROR("DX12Renderer: conflicting raw buffer dispatch slot %u", slot);
            return false;
        }
        if (!ValidateComputeRawRead(handle, call.uavVertexBuffer, resources)) {
            return false;
        }
    }

    // シェーダーが間接的に参照する資源にも、キューと書き込み競合の制約を適用する。
    for (const auto handle : call.indirectReadBuffers) {
        if (!ValidateComputeRawRead(handle, call.uavVertexBuffer, resources)) {
            return false;
        }
    }
    return true;
}

bool DX12Renderer::ValidateComputeRawRead(ResourceHandle<BufferTag> handle,
    ResourceHandle<BufferTag> vertexOutput, ResourceManager& resources) const
{
    const auto* buffer = resources.Get(handle);
    const bool canReadBuffer = !m_asyncComputeActive && buffer && handle != vertexOutput;
    const bool hasSrv = canReadBuffer && buffer->GetBindlessSrvIndex() != INVALID_BINDLESS_INDEX;
    if (!hasSrv) {
        FBZZ_LOG_ERROR("DX12Renderer: invalid or stale raw buffer dispatch binding");
        return false;
    }
    return true;
}

bool DX12Renderer::ValidateComputeRayBindings(const ComputeCall& call, ResourceManager& resources) const
{
    for (uint32_t slot = 0; slot < call.accelerationStructures.size(); ++slot) {
        const auto handle = call.accelerationStructures[slot];
        if (!handle.IsValid()) {
            continue;
        }
        const auto* structure = resources.Get(handle);
        const bool canQueryRays = !m_asyncComputeActive && m_context.SupportsInlineRaytracing();
        const bool isBuiltTlas = structure && structure->IsBuilt()
            && structure->GetKind() == AccelerationStructureKind::TOP_LEVEL;
        const bool slotAlreadyUsed = call.srvInputs[slot].IsValid() || call.srvBuffers[slot].IsValid()
            || call.srvRawBuffers[slot].IsValid();
        if (!canQueryRays || !isBuiltTlas || structure->GetBindlessIndex() == INVALID_BINDLESS_INDEX
            || slotAlreadyUsed) {
            FBZZ_LOG_ERROR("DX12Renderer: invalid TLAS dispatch binding at slot %u", slot);
            return false;
        }
    }
    return true;
}

bool DX12Renderer::ValidateComputeIndirectResources(const ComputeCall& call, ResourceManager& resources) const
{
    const auto& writeBuffers = call.indirectWriteVertexBuffers;
    for (auto current = writeBuffers.begin(); current != writeBuffers.end(); ++current) {
        const auto handle = *current;
        const auto* buffer = static_cast<const DX12Buffer*>(resources.Get(handle));
        const bool canWriteOnThisQueue = !m_asyncComputeActive && m_context.SupportsBindless();
        // ディスクリプタは遅延発行される。使用できるキュー・バッファを確かめてから引く。
        const bool hasWritableDescriptor = canWriteOnThisQueue && buffer && buffer->IsGpuWritable()
            && buffer->GetBindlessUavIndex() != INVALID_BINDLESS_INDEX;
        const auto& indirectReads = call.indirectReadBuffers;
        const auto& boundRawReads = call.srvRawBuffers;
        const bool alreadyBound = handle == call.uavVertexBuffer
            || std::find(indirectReads.begin(), indirectReads.end(), handle) != indirectReads.end()
            || std::find(boundRawReads.begin(), boundRawReads.end(), handle) != boundRawReads.end();
        const bool duplicateWrite = std::find(writeBuffers.begin(), current, handle) != current;
        if (!hasWritableDescriptor || alreadyBound || duplicateWrite) {
            FBZZ_LOG_ERROR("DX12Renderer: invalid, aliased or duplicate indirect vertex UAV");
            return false;
        }
    }
    for (const auto handle : call.indirectReadTextures) {
        const auto* texture = resources.Get(handle);
        if (m_asyncComputeActive || !texture || texture->GetBindlessIndex() == INVALID_BINDLESS_INDEX
            || std::find(call.uavOutputs.begin(), call.uavOutputs.end(), handle) != call.uavOutputs.end()) {
            FBZZ_LOG_ERROR("DX12Renderer: invalid, stale or writable indirect texture dispatch binding");
            return false;
        }
    }
    return true;
}

bool DX12Renderer::ValidateComputeDescriptors(const ComputeCall& call, ResourceManager& resources) const
{
    // 明示したハンドルの解放やディスクリプタ不足は、Dispatch を記録する前に拒否する。
    const auto srvIndex = [](const auto& resource) { return resource.GetBindlessIndex(); };
    if (!ValidateOptionalComputeDescriptors(call.srvInputs, resources, srvIndex)
        || !ValidateOptionalComputeDescriptors(call.srvBuffers, resources, srvIndex)) {
        return false;
    }
    const auto uavIndex = [](const auto& resource) { return resource.GetBindlessUavIndex(); };
    if (!ValidateOptionalComputeDescriptors(call.uavOutputs, resources, uavIndex)
        || !ValidateOptionalComputeDescriptors(call.uavBuffers, resources, uavIndex)) {
        return false;
    }
    if (!call.uavVertexBuffer.IsValid()) {
        return true;
    }
    const auto* vertexBuffer = static_cast<const DX12Buffer*>(resources.Get(call.uavVertexBuffer));
    return vertexBuffer && vertexBuffer->IsGpuWritable()
        && vertexBuffer->GetBindlessUavIndex() != INVALID_BINDLESS_INDEX;
}

bool DX12Renderer::PrepareComputeConstants(const ComputeCall& call, ResourceManager& resources,
                                           std::span<D3D12_GPU_VIRTUAL_ADDRESS> constantAddresses) const
{
    for (uint32_t slot = 0; slot < call.constantBuffers.size(); ++slot) {
        constantAddresses[slot] = ResolveConstantAddress(call.constantBuffers[slot], resources);
        if (!constantAddresses[slot]) {
            return false;
        }
    }
    return true;
}

void DX12Renderer::QueueComputeReads(const ComputeCall& call, ResourceManager& resources)
{
    // SRV の状態遷移。bindless ではディスクリプタを張らないが、遷移は従来どおり要る
    // (添字が同じでも間に挟まった別パスで状態は変わりうる)。
    for (uint32_t slot = 0; slot < kBindlessPixelSlotCount; ++slot) {
        if (auto* textureBase = resources.Get(call.srvInputs[slot])) {
            m_stateTracker.QueueTransition(
                static_cast<DX12Texture*>(textureBase)->GetResource(),
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        if (auto* bufferBase = resources.Get(call.srvBuffers[slot])) {
            m_stateTracker.QueueTransition(
                static_cast<DX12StructuredBuffer*>(bufferBase)->GetResource(),
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
    }
    const auto transitionRawRead = [&](ResourceHandle<BufferTag> handle) {
        if (auto* bufferBase = resources.Get(handle)) {
            auto* buffer = static_cast<DX12Buffer*>(bufferBase);
            // UPLOAD snapshot は GENERIC_READ を維持する。GPU 変形の DEFAULT 実体だけ遷移させる。
            if (buffer->IsGpuWritable()) {
                m_stateTracker.QueueTransition(buffer->GetSrvResource(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            }
        }
    };
    for (const auto handle : call.srvRawBuffers) {
        if (handle.IsValid()) {
            transitionRawRead(handle);
        }
    }
    for (const auto handle : call.indirectReadBuffers) {
        transitionRawRead(handle);
    }
    for (const auto handle : call.indirectReadTextures) {
        m_stateTracker.QueueTransition(static_cast<DX12Texture*>(resources.Get(handle))->GetResource(),
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
}

std::vector<ID3D12Resource*> DX12Renderer::QueueComputeWrites(const ComputeCall& call, ResourceManager& resources)
{
    // UAV の状態遷移と、Dispatch 後の UAV バリア対象の収集。
    std::vector<ID3D12Resource*> writtenResources;
    writtenResources.reserve(call.uavOutputs.size() + call.uavBuffers.size()
                             + 1 + call.indirectWriteVertexBuffers.size());
    const auto queueWrite = [&](ID3D12Resource* resource) {
        m_stateTracker.QueueTransition(resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        writtenResources.push_back(resource);
    };
    for (uint32_t slot = 0; slot < kBindlessUavSlotCount; ++slot) {
        if (auto* textureBase = resources.Get(call.uavOutputs[slot])) {
            auto* texture = static_cast<DX12Texture*>(textureBase);
            queueWrite(texture->GetResource());
        }
        if (IsStructuredUavSlot(call, slot)) {
            if (auto* bufferBase = resources.Get(call.uavBuffers[StructuredUavIndex(slot)])) {
                auto* buffer = static_cast<DX12StructuredBuffer*>(bufferBase);
                queueWrite(buffer->GetResource());
            }
        }
        // u4: GPU 書き込み可能な頂点バッファ (コンピュートスキニングの出力)。直前フレームでは
        // 頂点バッファとして読まれているので、書き込む前に UNORDERED_ACCESS へ戻す遷移が要る。
        if (slot == uav::SKINNED_VERTICES) {
            if (auto* bufferBase = resources.Get(call.uavVertexBuffer)) {
                auto* buffer = static_cast<DX12Buffer*>(bufferBase);
                if (buffer->IsGpuWritable()) {
                    queueWrite(buffer->GetResource());
                }
            }
        }
    }

    for (const auto handle : call.indirectWriteVertexBuffers) {
        auto* buffer = static_cast<DX12Buffer*>(resources.Get(handle));
        queueWrite(buffer->GetResource());
    }
    return writtenResources;
}

BindlessIndicesConstants DX12Renderer::CollectComputeIndices(const ComputeCall& call, ResourceManager& resources)
{
    // Texture と StructuredBuffer の既存の優先順を保つ。raw SRV と TLAS の重複は検証済み。
    BindlessIndicesConstants indices;
    indices.Reset();
    for (uint32_t slot = 0; slot < kBindlessPixelSlotCount; ++slot) {
        if (auto* textureBase = resources.Get(call.srvInputs[slot])) {
            indices.pixel[slot] = textureBase->GetBindlessIndex();
        }
        if (auto* bufferBase = resources.Get(call.srvBuffers[slot])) {
            indices.pixel[slot] = bufferBase->GetBindlessIndex();
        }
        if (auto* bufferBase = resources.Get(call.srvRawBuffers[slot])) {
            indices.pixel[slot] = bufferBase->GetBindlessSrvIndex();
        }
        if (auto* structure = resources.Get(call.accelerationStructures[slot])) {
            indices.pixel[slot] = structure->GetBindlessIndex();
        }
    }
    for (uint32_t slot = 0; slot < kBindlessUavSlotCount; ++slot) {
        if (auto* textureBase = resources.Get(call.uavOutputs[slot])) {
            indices.uav[slot] = textureBase->GetBindlessUavIndex();
        }
        if (IsStructuredUavSlot(call, slot)) {
            if (auto* bufferBase = resources.Get(call.uavBuffers[StructuredUavIndex(slot)])) {
                indices.uav[slot] = bufferBase->GetBindlessUavIndex();
            }
        }
        if (slot == uav::SKINNED_VERTICES) {
            if (auto* bufferBase = resources.Get(call.uavVertexBuffer)) {
                indices.uav[slot] = bufferBase->GetBindlessUavIndex();
            }
        }
    }
    return indices;
}

void DX12Renderer::SynchronizeComputeWrites(ID3D12GraphicsCommandList& commands,
                                            std::span<ID3D12Resource* const> writtenResources)
{
    if (writtenResources.empty()) {
        return;
    }
    if (!m_computeBatchActive) {
        RecordUavBarriers(commands, writtenResources);
        return;
    }

    // バッチ内の Dispatch は独立しているため、資源ごとに末尾で一度だけ同期する。
    for (auto* resource : writtenResources) {
        auto& pending = m_computeBatchWrittenResources;
        if (std::find(pending.begin(), pending.end(), resource) == pending.end()) {
            pending.push_back(resource);
        }
    }
}

bool DX12Renderer::BuildAccelerationStructure(
    ResourceHandle<AccelerationStructureTag> handle, ResourceManager& resources)
{
    auto* structure = static_cast<DX12AccelerationStructure*>(resources.Get(handle));
    if (!m_context.IsFrameOpen() || m_asyncComputeActive || m_computeBatchActive
        || !m_context.SupportsInlineRaytracing() || !structure || structure->IsBuilt()) {
        FBZZ_LOG_ERROR("DX12Renderer: acceleration structure build rejected");
        return false;
    }
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList4> commands;
    FBZZ_HR_CHECK(m_context.GetCommandList()->QueryInterface(IID_PPV_ARGS(&commands)));
    const std::string buildName = std::string(structure->GetKind() == AccelerationStructureKind::BOTTOM_LEVEL
        ? "AS Prepare BLAS " : "AS Prepare TLAS ")
        + std::to_string(handle.id) + ":" + std::to_string(handle.gen);
    DX12ScopedPixEvent buildEvent(m_context, m_context.GetCommandList(), buildName, 0xffe8ad58u);
    if (!structure->Build(*commands.Get(), m_uploadArena, resources)) {
        FBZZ_LOG_ERROR("DX12Renderer: acceleration structure inputs are unavailable");
        return false;
    }
    return true;
}

std::unique_ptr<IAccelerationStructure> DX12Renderer::CreateNativeAccelerationStructure(
    const AccelerationStructureDesc& desc, ResourceManager& resources)
{
    auto structure = std::make_unique<DX12AccelerationStructure>();
    if (!structure->Init(m_context, m_stateTracker, desc, resources)) {
        return nullptr;
    }
    return structure;
}

void DX12Renderer::BeginComputeBatch()
{
    // ネストは契約外。既存バッチを安全に閉じてから新しい収集を開始する。
    if (m_computeBatchActive) {
        EndComputeBatch();
    }
    m_computeBatchWrittenResources.clear();
    m_computeBatchActive = true;
}

void DX12Renderer::EndComputeBatch()
{
    if (!m_computeBatchActive) {
        return;
    }

    if (m_context.IsFrameOpen() && !m_computeBatchWrittenResources.empty()) {
        // Dispatch を積んだリストへ、パス全体の書き込みを確定する UAV バリアを積む。
        if (auto* commands = RecordingList()) {
            RecordUavBarriers(*commands, m_computeBatchWrittenResources);
        }
    }

    m_computeBatchWrittenResources.clear();
    m_computeBatchActive = false;
}

ID3D12GraphicsCommandList* DX12Renderer::RecordingList() const
{
    if (m_asyncComputeActive && m_context.IsComputeListOpen()) {
        return m_context.GetComputeList();
    }
    return m_context.GetCommandList();
}

void DX12Renderer::RestoreGraphicsTargetState(ResourceManager&)
{
    ID3D12GraphicsCommandList* commands = m_context.GetCommandList();
    if (!commands) {
        return;
    }
    if (IsCurrentRenderTargetLost()) {
        ReportLostRenderTarget("RestoreGraphicsTargetState");
        return;
    }
    // 独立した色面を束縛していた場合は、面ごとに RTV とその資源を解決する。
    if (m_currentColorAttachmentCount != 0) {
        std::array<D3D12_CPU_DESCRIPTOR_HANDLE, DX12RenderTarget::MAX_COLOR> rtvs{};
        for (uint32_t index = 0; index < m_currentColorAttachmentCount; ++index) {
            const auto* target = ResolveCurrentColorAttachment(index);
            const auto colorIndex = m_currentColorAttachments[index].colorIndex;
            m_stateTracker.QueueTransition(target->GetColorResource(colorIndex), D3D12_RESOURCE_STATE_RENDER_TARGET);
            rtvs[index] = target->GetRtv(colorIndex);
        }
        m_stateTracker.FlushBarriers(commands);
        commands->OMSetRenderTargets(m_currentColorAttachmentCount, rtvs.data(), FALSE, nullptr);
        ApplyRasterRegion(*commands);
        return;
    }

    // 非同期区間へ渡す前に COMMON にした描画先を、再び書き込める状態へ戻す。
    const auto* target = ResolveCurrentRenderTarget();
    if (target) {
        QueueDrawTargetWrites(*target);
    }
    m_stateTracker.FlushBarriers(commands);
    BindDrawTargetViews(*commands, target);
    ApplyRasterRegion(*commands);
}

void DX12Renderer::QueueDrawTargetWrites(const DX12RenderTarget& target)
{
    for (uint32_t index = 0; index < target.GetColorCount(); ++index) {
        m_stateTracker.QueueTransition(target.GetColorResource(index), D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
    if (target.HasDepth()) {
        m_stateTracker.QueueTransition(target.GetDepthResource(), D3D12_RESOURCE_STATE_DEPTH_WRITE);
    }
}

void DX12Renderer::BindDrawTargetViews(ID3D12GraphicsCommandList& commands, const DX12RenderTarget* target) const
{
    if (m_currentCubeRtv.ptr != 0) {
        // キャプチャ中はキューブ全体の代わりに、選択中の面と mip の RTV / DSV を戻す。
        const auto dsv = target ? target->GetDsv(m_currentCubeMip) : D3D12_CPU_DESCRIPTOR_HANDLE{};
        commands.OMSetRenderTargets(1, &m_currentCubeRtv, FALSE, dsv.ptr ? &dsv : nullptr);
        return;
    }
    if (!target) {
        const auto rtv = m_context.GetCurrentRtv();
        const auto dsv = m_context.GetDsv();
        commands.OMSetRenderTargets(1, &rtv, FALSE, &dsv);
        return;
    }

    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, DX12RenderTarget::MAX_COLOR> rtvs{};
    for (uint32_t index = 0; index < target->GetColorCount(); ++index) {
        rtvs[index] = target->GetRtv(index);
    }
    const auto dsv = target->GetDsv();
    commands.OMSetRenderTargets(target->GetColorCount(), rtvs.data(), FALSE,
                                target->HasDepth() ? &dsv : nullptr);
}

bool DX12Renderer::BeginAsyncCompute(ResourceManager& resources)
{
    if (m_asyncComputeActive) {
        return true;
    }
    if (!m_context.IsFrameOpen() || !m_context.SupportsAsyncCompute()) {
        return false;
    }

    // コンピュートキューは PIXEL_SHADER_RESOURCE / RENDER_TARGET / DEPTH_* を扱えない。
    // 区間へ渡す前に、描画キュー側で追跡中のリソースをまとめて COMMON へ落とす。
    /// @see Docs/design/async-compute.md §3
    m_stateTracker.QueueTransitionAllToCommon();
    m_stateTracker.FlushBarriers(m_context.GetCommandList());

    // ここで描画の列を割る。直前までの描画が GPU へ投入され、その完了をコンピュートが待つ。
    const uint64_t waitValue = m_context.SplitGraphicsList();
    if (waitValue == 0) {
        return false;
    }

    // 列を割ると Reset で全パイプライン状態が落ちる。束縛の記憶を捨て、描画先を張り直す。
    InvalidateRootCbvCache();
    RestoreGraphicsTargetState(resources);

    if (!m_context.BeginComputeList(waitValue)) {
        return false;
    }
    m_asyncComputeActive = true;
    return true;
}

void DX12Renderer::EndAsyncCompute()
{
    if (!m_asyncComputeActive) {
        return;
    }
    m_asyncComputeActive = false;
    m_context.EndComputeList();
    // コンピュート側で束縛したルートシグネチャ・ヒープは描画リストには載っていない。
    // 区間の前後で記憶が食い違わないよう、ここでも捨てる。
    InvalidateRootCbvCache();
}

void DX12Renderer::Resize(uint32_t width, uint32_t height)
{
    m_context.Resize(width, height);
}

void DX12Renderer::SetVSync(bool enabled)
{
    m_context.SetVSync(enabled);
}

bool DX12Renderer::GetVSync() const
{
    return m_context.GetVSync();
}

void DX12Renderer::SetRenderTarget(ResourceHandle<RenderTargetTag> handle, ResourceManager& resources)
{
    if (!m_context.IsFrameOpen()) {
        return;
    }
    ID3D12GraphicsCommandList* commands = m_context.GetCommandList();
    // リサイズ等で旧 RT が解放されていることがあるため、読み取りへの遷移前にハンドルから引き直す。
    TransitionCurrentTargetForRead();
    const auto* target = static_cast<const DX12RenderTarget*>(resources.Get(handle));

    // 無効なハンドルはバックバッファを選ぶ。独立した色面やキューブ面の指定を解除する。
    m_currentRenderTargetHandle = target ? handle : ResourceHandle<RenderTargetTag>{};
    m_currentRenderTargetOwner = target ? resources.GetInstanceId() : 0;
    m_currentColorAttachmentCount = 0;
    m_currentCubeRtv = {};
    m_currentCubeMip = 0;
    if (target) {
        QueueDrawTargetWrites(*target);
    }

    // 旧 RT の読み取り遷移と新 RT の書き込み遷移をまとめて記録してから、描画先を束縛する。
    m_stateTracker.FlushBarriers(commands);
    BindDrawTargetViews(*commands, target);
    const auto width = target ? target->GetWidth() : m_context.GetWidth();
    const auto height = target ? target->GetHeight() : m_context.GetHeight();
    SetRasterRegion(*commands, 0, 0, width, height);
}

bool DX12Renderer::SetRenderTargetColors(std::span<const RenderTargetColorAttachment> colors, ResourceManager& resources)
{
    const bool graphicsRecording = m_context.IsFrameOpen() && !m_asyncComputeActive;
    if (!graphicsRecording) {
        return false;
    }
    if (colors.empty() || colors.size() > DX12RenderTarget::MAX_COLOR) {
        return false;
    }
    auto* commands = m_context.GetCommandList();
    if (!commands) {
        return false;
    }
    std::array<ID3D12Resource*, DX12RenderTarget::MAX_COLOR> nativeColors{};
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, DX12RenderTarget::MAX_COLOR> rtvs{};
    // 先頭の面を基準に、寸法・形式・資源の重複を状態変更前に調べる。
    const auto* firstTarget = static_cast<const DX12RenderTarget*>(resources.Get(colors.front().target));
    if (!firstTarget) {
        return false;
    }
    const uint32_t width = firstTarget->GetWidth();
    const uint32_t height = firstTarget->GetHeight();
    const auto format = firstTarget->GetColorFormat();
    for (size_t index = 0; index < colors.size(); ++index) {
        const auto& attachment = colors[index];
        const auto* target = static_cast<const DX12RenderTarget*>(resources.Get(attachment.target));
        if (!IsValidColorOnlyAttachment(target, attachment.colorIndex, width, height, format)) {
            return false;
        }
        nativeColors[index] = target->GetColorResource(attachment.colorIndex);
        rtvs[index] = target->GetRtv(attachment.colorIndex);
        const auto previousColors = std::span{nativeColors}.first(index);
        const bool alreadyAttached = std::find(previousColors.begin(), previousColors.end(), nativeColors[index])
            != previousColors.end();
        if (!nativeColors[index] || !rtvs[index].ptr || alreadyAttached) {
            return false;
        }
    }
    // 全面を検証してから遷移と束縛を変更し、入力不正で既存の出力状態を失わない。
    TransitionCurrentTargetForRead();
    for (size_t index = 0; index < colors.size(); ++index) {
        m_stateTracker.QueueTransition(nativeColors[index], D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
    m_stateTracker.FlushBarriers(commands);
    m_currentColorAttachmentCount = static_cast<uint32_t>(colors.size());
    std::copy(colors.begin(), colors.end(), m_currentColorAttachments.begin());
    m_currentRenderTargetHandle = colors.front().target;
    m_currentRenderTargetOwner = resources.GetInstanceId();
    m_currentCubeRtv = {};
    m_currentCubeMip = 0;
    commands->OMSetRenderTargets(m_currentColorAttachmentCount, rtvs.data(), FALSE, nullptr);
    SetRasterRegion(*commands, 0, 0, width, height);
    return true;
}

void DX12Renderer::SetViewport(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
    if (!m_context.IsFrameOpen() || width == 0u || height == 0u) {
        return;
    }
    auto* commands = m_context.GetCommandList();
    if (!commands) {
        return;
    }

    // シャドウアトラスのタイルに絞るときは、ビューポートとシザーを一緒に更新する。
    SetRasterRegion(*commands, x, y, width, height);
}

void DX12Renderer::SetRasterRegion(ID3D12GraphicsCommandList& commands, uint32_t x, uint32_t y,
                                   uint32_t width, uint32_t height)
{
    m_currentViewport = {static_cast<float>(x), static_cast<float>(y),
                         static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
    m_currentScissor = {static_cast<LONG>(x), static_cast<LONG>(y),
                        static_cast<LONG>(x + width), static_cast<LONG>(y + height)};
    ApplyRasterRegion(commands);
}

void DX12Renderer::ApplyRasterRegion(ID3D12GraphicsCommandList& commands) const
{
    commands.RSSetViewports(1, &m_currentViewport);
    commands.RSSetScissorRects(1, &m_currentScissor);
}

void DX12Renderer::SetRenderTargetFace(
    ResourceHandle<RenderTargetTag> handle, uint32_t face, uint32_t mip, ResourceManager& resources)
{
    if (!m_context.IsFrameOpen()) {
        return;
    }
    const auto* target = static_cast<const DX12RenderTarget*>(resources.Get(handle));
    if (!target || !target->IsCubemap() || face >= 6 || mip >= target->GetMipCount()) {
        return;
    }
    // 編集対象の面と mip を記憶し、同じ mip の深度面と組にして束縛する。
    TransitionCurrentTargetForRead();
    m_currentRenderTargetHandle = handle;
    m_currentRenderTargetOwner = resources.GetInstanceId();
    m_currentColorAttachmentCount = 0;
    m_currentCubeRtv = target->GetFaceRtv(face, mip);
    m_currentCubeFace = face;
    m_currentCubeMip = mip;
    m_stateTracker.QueueTransition(target->GetCubeResource(), D3D12_RESOURCE_STATE_RENDER_TARGET);
    m_stateTracker.QueueTransition(target->GetDepthResource(), D3D12_RESOURCE_STATE_DEPTH_WRITE);
    auto* commands = m_context.GetCommandList();
    m_stateTracker.FlushBarriers(commands);
    const auto dsv = target->GetDsv(mip);
    commands->OMSetRenderTargets(1, &m_currentCubeRtv, FALSE, &dsv);

    const uint32_t mipSize = (std::max)(1u, target->GetWidth() >> mip);
    SetRasterRegion(*commands, 0, 0, mipSize, mipSize);
}

bool DX12Renderer::RenderDebugPreview(const DrawCall& call, ResourceHandle<RenderTargetTag> target,
                                      ResourceManager& resources)
{
    if (!m_context.IsFrameOpen()) {
        return false;
    }
    auto* previewTarget = static_cast<DX12RenderTarget*>(resources.Get(target));
    auto* previewShader = static_cast<DX12Shader*>(resources.Get(call.shader));
    auto* previewState = static_cast<DX12PipelineState*>(resources.Get(call.pipelineState));
    if (!previewTarget || !previewShader || !previewState) {
        return false;
    }
    if (!m_psoCache.GetOrCreate(*previewShader, previewState->GetDesc(), call.topology,
                               previewTarget->GetColorFormat(), previewTarget->GetColorCount(),
                               previewTarget->IsReversedZ())) {
        return false;
    }
    const auto previous = m_currentRenderTargetHandle;
    const auto previousOwner = m_currentRenderTargetOwner;
    const auto previousColors = m_currentColorAttachments;
    const auto previousColorCount = m_currentColorAttachmentCount;
    const auto viewport = m_currentViewport;
    const auto scissor = m_currentScissor;
    const bool cube = m_currentCubeRtv.ptr != 0;
    const uint32_t face = m_currentCubeFace;
    const uint32_t mip = m_currentCubeMip;

    SetRenderTarget(target, resources);
    const bool submitted = TrySubmitDraw(call, resources);
    // キューブ RT は通常添付の RTV を持たない。プレビューを外してから面として戻す。
    auto* previousResources = FindLiveResourceManager(previousOwner);
    if (!previousResources) {
        SetRenderTarget({}, resources);
    } else if (previousColorCount != 0) {
        if (!SetRenderTargetColors(std::span{previousColors.data(), previousColorCount}, *previousResources)) {
            SetRenderTarget({}, *previousResources);
        }
    } else if (cube) {
        SetRenderTarget({}, *previousResources);
        SetRenderTargetFace(previous, face, mip, *previousResources);
    } else {
        SetRenderTarget(previous, *previousResources);
    }
    m_currentViewport = viewport;
    m_currentScissor = scissor;
    ApplyRasterRegion(*m_context.GetCommandList());
    return submitted;
}

bool DX12Renderer::BakeSkyLight(
    ResourceHandle<RenderTargetTag> handle, ResourceManager& resources,
    uint32_t irradianceSize, uint32_t prefilterSize, uint32_t prefilterMips,
    uint32_t sampleCount, std::unique_ptr<ITexture>& outIrradiance,
    std::unique_ptr<ITexture>& outPrefilter)
{
    auto* targetBase = resources.Get(handle);
    auto* target = targetBase ? static_cast<DX12RenderTarget*>(targetBase) : nullptr;
    if (!target || !target->IsCubemap()) {
        return false;
    }
    DX12ScopedPixEvent bakeEvent(m_context, m_context.GetCommandList(),
        "Probe/Sky IBL Convolve target=" + std::to_string(handle.id)
            + ":" + std::to_string(handle.gen), 0xff65b891u);
    if (!m_iblBaker) {
        m_iblBaker = std::make_unique<DX12IblBaker>(
            &m_context, &m_stateTracker, &m_psoCache, &m_uploadArena);
    }
    std::unique_ptr<DX12Texture> irradiance;
    std::unique_ptr<DX12Texture> prefilter;
    if (!m_iblBaker->Convolve(*target, irradianceSize, prefilterSize, prefilterMips,
                              sampleCount, irradiance, prefilter)) {
        return false;
    }
    outIrradiance = std::move(irradiance);
    outPrefilter = std::move(prefilter);
    return true;
}

std::unique_ptr<IIblBaker> DX12Renderer::CreateIblBaker()
{
    return std::make_unique<DX12HdriBaker>(m_context.GetDevice(), m_psoCache.GetComputeRootSignature());
}

/// @note RT のカラーを CPU 側 ScratchImage として掴む。PNG 化と数値評価で同じ読み戻しを共有する。
static bool CaptureDX12RenderTargetImage(DX12Context& context, const DX12StateTracker& tracker, IRenderTarget* base,
                                         DirectX::ScratchImage& outImage)
{
    auto* target = static_cast<DX12RenderTarget*>(base);
    if (target == nullptr) {
        return false;
    }
    ID3D12Resource* resource = target->GetColorResource(0);
    if (resource == nullptr) {
        return false;
    }

    D3D12_RESOURCE_STATES state;
    if (!tracker.ReadState(resource, state)) {
        return false;
    }
    // 非表示ビューは ImGui がサンプリングしない。実際の状態を渡し、同期読み戻し後に同じ状態へ戻す。
    const HRESULT result = DirectX::CaptureTexture(context.GetCommandQueue(), resource, false, outImage, state, state);
    if (FAILED(result)) {
        FBZZ_LOG_ERROR("DX12Renderer: Render target readback failed (0x%08X)", static_cast<unsigned>(result));
    }
    return SUCCEEDED(result);
}

bool DX12Renderer::CaptureRenderTargetToPng(ResourceHandle<RenderTargetTag> rt, ResourceManager& resources,
                                            std::vector<uint8_t>& outPng, uint32_t& outWidth, uint32_t& outHeight)
{
    DirectX::ScratchImage captured;
    if (!CaptureDX12RenderTargetImage(m_context, m_stateTracker, resources.Get(rt), captured)) {
        return false;
    }
    return detail::EncodeCapturedImageToPng(captured, outPng, outWidth, outHeight);
}

bool DX12Renderer::CaptureRenderTargetToLinearRGBA(ResourceHandle<RenderTargetTag> rt, ResourceManager& resources,
                                                   std::vector<float>& outRgba, uint32_t& outWidth, uint32_t& outHeight)
{
    DirectX::ScratchImage captured;
    if (!CaptureDX12RenderTargetImage(m_context, m_stateTracker, resources.Get(rt), captured)) {
        return false;
    }
    return detail::ReadCapturedImageAsLinearRGBA(captured, outRgba, outWidth, outHeight);
}

std::unique_ptr<IBuffer> DX12Renderer::CreateNativeVertexBuffer(
    const void* data, size_t sizeBytes, uint32_t stride)
{
    auto buffer = std::make_unique<DX12Buffer>();
    if (!buffer->Init(&m_context, data, sizeBytes, stride, DX12Buffer::Kind::Vertex)) {
        return nullptr;
    }
    return buffer;
}

std::unique_ptr<IBuffer> DX12Renderer::CreateNativeGpuWritableVertexBuffer(
    size_t sizeBytes, uint32_t stride)
{
    auto buffer = std::make_unique<DX12Buffer>();
    if (!buffer->InitGpuWritableVertex(&m_context, &m_stateTracker, sizeBytes, stride)) {
        return nullptr;
    }
    return buffer;
}

std::unique_ptr<IBuffer> DX12Renderer::CreateNativeIndexBuffer(const void* data, uint32_t count)
{
    auto buffer = std::make_unique<DX12Buffer>();
    if (!buffer->Init(&m_context, data, static_cast<size_t>(count) * sizeof(uint32_t),
                      0, DX12Buffer::Kind::Index)) {
        return nullptr;
    }
    return buffer;
}

std::unique_ptr<IConstantBuffer> DX12Renderer::CreateNativeConstantBuffer(size_t sizeBytes)
{
    auto buffer = std::make_unique<DX12ConstantBuffer>();
    if (!buffer->Init(&m_uploadArena, sizeBytes)) {
        return nullptr;
    }
    return buffer;
}
std::unique_ptr<IShader> DX12Renderer::CreateNativeShader(const std::string& path)
{
    auto shader = std::make_unique<DX12Shader>();
    if (!shader->Init(path)) {
        return nullptr;
    }
    return shader;
}
std::unique_ptr<ITexture> DX12Renderer::CreateNativeTexture(const std::string& path)
{
    auto texture = std::make_unique<DX12Texture>();
    if (!texture->Init(&m_context, path)) {
        return nullptr;
    }
    texture->RegisterState(&m_stateTracker, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return texture;
}

std::unique_ptr<ITexture> DX12Renderer::CreateNativeTextureFromData(
    const uint8_t* rgba, uint32_t width, uint32_t height)
{
    auto texture = std::make_unique<DX12Texture>();
    if (!texture->InitFromData(&m_context, rgba, width, height)) {
        return nullptr;
    }
    texture->RegisterState(&m_stateTracker, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return texture;
}
std::unique_ptr<ITexture> DX12Renderer::CreateNativeTextureFromDataMips(
    const TextureMipData* mips, uint32_t mipCount)
{
    auto texture = std::make_unique<DX12Texture>();
    if (!texture->InitFromDataMips(&m_context, mips, mipCount)) {
        return nullptr;
    }
    texture->RegisterState(&m_stateTracker, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return texture;
}
std::unique_ptr<ITexture> DX12Renderer::CreateNativeTextureFromDataMipsAsync(
    const TextureMipData* mips, uint32_t mipCount, uint64_t& outUploadToken)
{
    auto texture = std::make_unique<DX12Texture>();
    if (!texture->InitFromDataMipsAsync(&m_context, mips, mipCount, outUploadToken)) {
        return nullptr;
    }
    texture->RegisterState(&m_stateTracker, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return texture;
}
bool DX12Renderer::IsUploadComplete(uint64_t uploadToken) const
{
    return m_context.IsFenceComplete(uploadToken);
}
std::unique_ptr<ITexture> DX12Renderer::CreateNativeTexture3DFromData(
    const uint8_t* rgba, uint32_t width, uint32_t height, uint32_t depth)
{
    auto texture = std::make_unique<DX12Texture>();
    if (!texture->InitFromData3D(&m_context, rgba, width, height, depth)) {
        return nullptr;
    }
    texture->RegisterState(&m_stateTracker, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return texture;
}
std::unique_ptr<ITexture> DX12Renderer::CreateNativeTextureFromRenderTarget(
    IRenderTarget& renderTarget, uint32_t index, RenderTargetTextureKind kind)
{
    auto& target = static_cast<DX12RenderTarget&>(renderTarget);
    ID3D12Resource* resource = kind == RenderTargetTextureKind::Depth
        ? target.GetDepthResource() : target.GetColorResource(index);
    const DXGI_FORMAT format = kind == RenderTargetTextureKind::Depth
        ? DXGI_FORMAT_R32_FLOAT : target.GetColorFormat();
    auto texture = std::make_unique<DX12Texture>();
    if (!texture->InitFromResource(&m_context, resource, format,
                                   target.GetWidth(), target.GetHeight())) {
        return nullptr;
    }
    return texture;
}
std::unique_ptr<IPipelineState> DX12Renderer::CreateNativePipelineState(const PipelineStateDesc& desc)
{
    return std::make_unique<DX12PipelineState>(desc);
}
std::unique_ptr<IRenderTarget> DX12Renderer::CreateNativeRenderTarget(
    uint32_t width, uint32_t height, const RenderTargetDesc& desc)
{
    auto target = std::make_unique<DX12RenderTarget>();
    if (!target->Init(&m_context, &m_stateTracker, width, height, desc)) {
        return nullptr;
    }
    return target;
}

std::unique_ptr<IRenderTarget> DX12Renderer::CreateNativeCubemapRenderTarget(
    uint32_t size, uint32_t mipCount)
{
    auto target = std::make_unique<DX12RenderTarget>();
    if (!target->InitCubemap(&m_context, &m_stateTracker, size, mipCount)) {
        return nullptr;
    }
    return target;
}

std::unique_ptr<ITexture> DX12Renderer::CreateNativeCubeTextureFromRenderTarget(IRenderTarget& renderTarget)
{
    auto& target = static_cast<DX12RenderTarget&>(renderTarget);
    if (!target.IsCubemap()) {
        return nullptr;
    }
    auto texture = std::make_unique<DX12Texture>();
    if (!texture->InitCubeFromResource(&m_context, target.GetCubeResource(), target.GetColorFormat(),
                                       target.GetWidth(), target.GetMipCount())) {
        return nullptr;
    }
    return texture;
}
std::unique_ptr<ITexture> DX12Renderer::CreateNativeComputeTexture(uint32_t width, uint32_t height)
{
    auto texture = std::make_unique<DX12Texture>();
    if (!texture->InitForCompute(&m_context, &m_stateTracker, width, height)) {
        return nullptr;
    }
    return texture;
}

std::unique_ptr<ITexture> DX12Renderer::CreateNativeComputeTexture3D(
    uint32_t width, uint32_t height, uint32_t depth)
{
    auto texture = std::make_unique<DX12Texture>();
    if (!texture->InitForCompute3D(&m_context, &m_stateTracker, width, height, depth)) {
        return nullptr;
    }
    return texture;
}

std::unique_ptr<ITexture> DX12Renderer::CreateNativeDynamicTexture(
    uint32_t width, uint32_t height, DynamicTextureFormat format)
{
    auto texture = std::make_unique<DX12Texture>();
    if (!texture->InitDynamic(&m_context, &m_stateTracker, width, height, format)) {
        return nullptr;
    }
    return texture;
}

std::unique_ptr<IStructuredBuffer> DX12Renderer::CreateNativeStructuredBuffer(
    const void* data, uint32_t count, uint32_t stride)
{
    auto buffer = std::make_unique<DX12StructuredBuffer>();
    if (!buffer->Init(&m_context, &m_stateTracker, data, count, stride, false)) {
        return nullptr;
    }
    return buffer;
}

std::unique_ptr<IStructuredBuffer> DX12Renderer::CreateNativeGpuLocalStructuredBuffer(
    const void* data, uint32_t count, uint32_t stride)
{
    auto buffer = std::make_unique<DX12StructuredBuffer>();
    if (!buffer->Init(&m_context, &m_stateTracker, data, count, stride, false, true)) {
        return nullptr;
    }
    return buffer;
}

std::unique_ptr<IStructuredBuffer> DX12Renderer::CreateNativeRWStructuredBuffer(
    const void* data, uint32_t count, uint32_t stride)
{
    auto buffer = std::make_unique<DX12StructuredBuffer>();
    if (!buffer->Init(&m_context, &m_stateTracker, data, count, stride, true)) {
        return nullptr;
    }
    return buffer;
}

bool DX12Renderer::InitializeGpuProfiler()
{
    D3D12_QUERY_HEAP_DESC queryDesc{};
    queryDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    queryDesc.Count = DX12Context::FRAME_COUNT * GPU_MAX_PASSES * TIMESTAMPS_PER_GPU_PASS;
    const HRESULT queryResult = m_context.GetDevice()->CreateQueryHeap(&queryDesc, IID_PPV_ARGS(&m_gpuQueryHeap));
    FBZZ_HR_CHECK(queryResult);
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = static_cast<UINT64>(queryDesc.Count) * sizeof(uint64_t);
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    const HRESULT bufferResult = m_context.GetDevice()->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr, IID_PPV_ARGS(&m_gpuReadback));
    FBZZ_HR_CHECK(bufferResult);
    void* mapped = nullptr;
    const HRESULT mapResult = m_gpuReadback->Map(0, nullptr, &mapped);
    FBZZ_HR_CHECK(mapResult);
    m_gpuMappedTimestamps = static_cast<uint64_t*>(mapped);
    const HRESULT frequencyResult = m_context.GetCommandQueue()->GetTimestampFrequency(&m_gpuTimestampFrequency);
    FBZZ_HR_CHECK(frequencyResult);
    return m_gpuTimestampFrequency != 0;
}

void DX12Renderer::GpuProfBeginFrame()
{
    // 旧 API は互換 no-op。物理 BeginFrame だけが query 領域を開始する。
}

void DX12Renderer::GpuProfBeginPass(const char* name)
{
    if (!m_context.IsFrameOpen()) {
        return;
    }
    // タイムスタンプの容量やリストの分割に関係なく、PIX ではパス名を確認できるようにする。
    m_context.EndPixEvent(m_context.GetCommandList(), m_pixPassToken);
    m_pixPassToken = m_context.BeginPixEvent(m_context.GetCommandList(), name ? name : "Unknown");
    if (!m_gpuProfilerRecording) {
        return;
    }
    uint32_t query = 0;
    if (m_gpuProfilerLedger.BeginPass(name ? name : "Unknown", query)) {
        m_gpuPassListSerial = m_context.GetGraphicsCommandListSerial();
        m_context.GetCommandList()->EndQuery(m_gpuQueryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, query);
    }
}

void DX12Renderer::GpuProfEndPass(const char* name)
{
    m_context.EndPixEvent(m_context.GetCommandList(), m_pixPassToken);
    m_pixPassToken = 0;
    if (!m_gpuProfilerRecording || !m_context.IsFrameOpen()) {
        return;
    }
    if (m_gpuPassListSerial != m_context.GetGraphicsCommandListSerial()) {
        // 別 list の timestamp 比較は stable power 未設定では保証されないため、分割・別キュー区間を未計測にする。
        /// @see https://learn.microsoft.com/en-us/windows/win32/direct3d12/queries Differences in Queries: disjoint timestamps
        m_gpuProfilerLedger.CancelOpenPass();
        return;
    }
    uint32_t query = 0;
    if (m_gpuProfilerLedger.EndPass(name ? name : "Unknown", query)) {
        m_context.GetCommandList()->EndQuery(m_gpuQueryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, query);
    }
}

void DX12Renderer::GpuProfEndFrame()
{
    // 旧 API は互換 no-op。Resolve と提出 Fence の捕捉は物理 EndFrame に閉じる。
}

void DX12Renderer::GpuProfCollect()
{
    if (!m_gpuMappedTimestamps || m_gpuTimestampFrequency == 0) {
        return;
    }
    const size_t queryCount = DX12Context::FRAME_COUNT * GPU_MAX_PASSES * TIMESTAMPS_PER_GPU_PASS;
    m_gpuProfilerLedger.Collect(m_context.GetCompletedFenceValue(),
        {m_gpuMappedTimestamps, queryCount}, m_gpuTimestampFrequency);
    if (!m_gpuProfilerLedger.GetSnapshot().supported) {
        m_gpuProfilerRecording = false;
    }
}

bool DX12Renderer::GpuProfBeginView(const GpuProfilerViewMetadata& metadata)
{
    if (!m_context.IsFrameOpen() || m_pixViewToken != 0 || m_pixPassToken != 0
        || metadata.width == 0 || metadata.height == 0) {
        return false;
    }
    char name[384]{};
    std::snprintf(name, sizeof(name),
        "View=%llu appFrame=%llu scene=%llu plan=%llu resources=%llu output=%u:%u extent=%ux%u",
        static_cast<unsigned long long>(metadata.viewId),
        static_cast<unsigned long long>(metadata.applicationFrameSerial),
        static_cast<unsigned long long>(metadata.sceneGeneration),
        static_cast<unsigned long long>(metadata.planGeneration),
        static_cast<unsigned long long>(metadata.resourceEpoch),
        metadata.outputId, metadata.outputGeneration, metadata.width, metadata.height);
    m_pixViewName = name;
    m_pixViewToken = m_context.BeginPixEvent(m_context.GetCommandList(), m_pixViewName, PIX_VIEW_COLOR);
    if (m_gpuProfilerRecording) {
        m_gpuProfilerLedger.BeginView(metadata);
    }
    // タイムスタンプを取れない場合にも、PIX のビューが開いていれば呼び出し側はパス名を記録できる。
    return m_pixViewToken != 0;
}

void DX12Renderer::GpuProfEndView()
{
    m_context.EndPixEvent(m_context.GetCommandList(), m_pixPassToken);
    m_context.EndPixEvent(m_context.GetCommandList(), m_pixViewToken);
    m_pixPassToken = m_pixViewToken = 0;
    m_pixViewName.clear();
    if (m_gpuProfilerRecording) {
        m_gpuProfilerLedger.EndView();
    }
}

} // namespace fbzz::renderer
