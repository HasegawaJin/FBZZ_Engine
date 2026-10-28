/// @file    DX12Renderer.cpp
/// @brief   DirectX 12 バックバッファのフレーム記録とクリア操作。
/// @author  Hasegawa Jin
/// @date    2026-07-15
#include "DX12Renderer.hpp"

#include <Core/Logger.hpp>
#include <Core/HResult.hpp>
#include "DX12AccelerationStructure.hpp"
#include "DX12Buffer.hpp"
#include "DX12ConstantBuffer.hpp"
#include "DX12UploadArenaSizing.hpp"
#include "DX12PipelineState.hpp"
#include "DX12Shader.hpp"
#include "DX12Texture.hpp"
#include "DX12IblBaker.hpp"
#include "DX12HdriBaker.hpp"
#include "DX12RenderTarget.hpp"
#include "DX12StructuredBuffer.hpp"
#include <Graphics/Renderer/BindlessIndices.hpp>
#include <Graphics/Renderer/ResourceManager.hpp>
#include <Graphics/Renderer/Mesh.hpp>
#include "../../ResourceManagerLifetime.hpp"
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
std::atomic<uint64_t> s_gpuProfilerDeviceEpoch{1};
} /// @note namespace

/// @note out-of-line 定義。ここは DX12IblBaker.hpp を include 済みなので、m_iblBaker
/// @note (unique_ptr<DX12IblBaker>) のデリーターを完全型として実体化できる。
DX12Renderer::DX12Renderer() = default;
DX12Renderer::~DX12Renderer()
{
    prepared_skinning_detail::UnregisterBackend(*this);
    StopPreparedSkinningPool(m_preparedSkinningPool);
    if (m_preparedSkinningPool) m_context.Flush();
}

bool DX12Renderer::Init(HWND hwnd, uint32_t width, uint32_t height)
{
    m_bindingFusionEnabled = false;
    m_bindingDiagnostics = false;
    if (const char* mode = std::getenv("FBZZ_DX12_BINDING_SCAN_FUSION"))
        m_bindingFusionEnabled = std::strcmp(mode, "1") == 0;
    if (const char* diagnostic = std::getenv("FBZZ_DX12_BINDLESS_INDICES_DIAGNOSTICS"))
        m_bindingDiagnostics = std::strcmp(diagnostic, "1") == 0;
    if (std::getenv("FBZZ_DX12_BINDING_SCAN_FUSION") || m_bindingDiagnostics)
        std::fprintf(stderr, "DX12_BINDING_SCAN_CONFIG fused=%u diagnostic=%u\n",
            static_cast<unsigned>(m_bindingFusionEnabled), static_cast<unsigned>(m_bindingDiagnostics));

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
    if (!m_context.Initialize(hwnd, width, height)) {
        FBZZ_LOG_ERROR("DX12Renderer::Init: DX12Context 初期化失敗");
        return false;
    }
    if (!m_uploadArena.Initialize(m_context.GetDevice(), uploadSizing.bytesPerFrame)) {
        FBZZ_LOG_ERROR("DX12Renderer::Init: UploadArena 初期化失敗");
        m_context.Shutdown();
        return false;
    }
    /// @note DX12UploadArena owns two committed upload resources; this is capacity, not observed usage.
    FBZZ_LOG_INFO("DX12Renderer: UploadArena initialized (%zu bytes/frame, 2 frames, %zu bytes total)",
        uploadSizing.bytesPerFrame, uploadSizing.bytesPerFrame * 2u);
    if (uploadSizing.specified) {
        /// @note Explicit diagnostic configuration remains observable when the harness suppresses INFO logs.
        std::fprintf(stderr, "UPLOAD_ARENA_CONFIG bytesPerFrame=%zu frameCount=2 totalBytes=%zu\n",
            uploadSizing.bytesPerFrame, uploadSizing.bytesPerFrame * 2u);
    }
    if (!m_psoCache.Initialize(m_context.GetDevice(), m_context.SupportsBindless())) {
        FBZZ_LOG_ERROR("DX12Renderer::Init: PsoCache/RootSignature 初期化失敗");
        m_uploadArena.Shutdown();
        m_context.Shutdown();
        return false;
    }
    m_gpuDeviceEpoch = s_gpuProfilerDeviceEpoch.fetch_add(1, std::memory_order_relaxed);
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
    if (renderer.m_asyncComputeActive) return {};
    if (!renderer.m_preparedSkinningPool) renderer.m_preparedSkinningPool = CreatePreparedSkinningPool(renderer.m_context);
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
    if (m_gpuReadback && m_gpuMappedTimestamps) m_gpuReadback->Unmap(0, nullptr);
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
    if (m_context.BeginFrame()) {
        m_bindingDraws = m_bindingUploads = m_bindingFailures = 0;
        m_bindingResourceOwner = m_bindingResourceFrame = 0;
        m_bindingResourceCoherent = true;
        m_bindingSectionMs = 0;
        BeginPreparedSkinningFrame(m_preparedSkinningPool);
        /// @note 既存 BeginFrame の slot fence 待機後に回収し、未回収 query を上書きしない。
        GpuProfCollect();
        m_gpuProfilerFrame = m_context.GetFrameIndex();
        m_gpuProfilerRecording = m_gpuProfilerLedger.BeginFrame(
            m_gpuProfilerFrame, ++m_gpuPhysicalFrameSerial, m_gpuDeviceEpoch);
        m_pixViewToken = 0;
        m_pixPassToken = 0;
        m_pixViewName.clear();
        m_pixFrameToken = m_context.BeginPixEvent(m_context.GetCommandList(),
            "Frame physical=" + std::to_string(m_gpuPhysicalFrameSerial)
                + " device=" + std::to_string(m_gpuDeviceEpoch), 0xff4878a8u);
        m_computeBatchActive = false;
        m_computeBatchWrittenResources.clear();
        m_currentRenderTargetHandle = {};
    m_currentRenderTargetOwner = 0;
        m_currentColorAttachmentCount = 0;
        m_currentCubeRtv = {};
        m_currentViewport = { 0.0f, 0.0f, static_cast<float>(m_context.GetWidth()),
                              static_cast<float>(m_context.GetHeight()), 0.0f, 1.0f };
        m_currentScissor = { 0, 0, static_cast<LONG>(m_context.GetWidth()),
                             static_cast<LONG>(m_context.GetHeight()) };
        /// @note コマンドリストは BeginFrame で Reset される = 全パイプライン状態が既定へ戻る。
        /// @note       ここで直前値を捨てないと、実際には束縛されていない状態を「設定済み」と誤認する。
        InvalidateRootCbvCache();
        m_uploadArena.BeginFrame(m_context.GetFrameIndex());
        m_nullConstantAddress = 0;
        /// @note Root CBV はサイズを持たないため、未指定 CB の全読み取り範囲 (最大 64 KiB) をゼロで確保する。
        /// @see https://microsoft.github.io/DirectX-Specs/d3d/ResourceBinding.html#using-descriptors-directly-in-the-root-arguments
        const auto nullConstant = m_uploadArena.Allocate(
            D3D12_REQ_CONSTANT_BUFFER_ELEMENT_COUNT * 16u,
            D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
        if (nullConstant) {
            std::memset(nullConstant.cpu, 0, nullConstant.size);
            m_nullConstantAddress = nullConstant.gpu;
        }
        /// @note 全枠 INVALID の添字ブロック。アリーナが枯渇した Draw でもここを差せば、
        /// @note       シェーダー側の有効判定で «束縛されていない» と分かる (ゼロ埋めでは添字 0 を
        /// @note       有効なディスクリプタとして読んでしまう)。
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
}

void DX12Renderer::EndFrame()
{
    /// @note 呼び出し側が閉じ忘れても、UAV 書き込みを未同期のまま Submit しない。
    EndComputeBatch();
    const bool recorded = m_gpuProfilerRecording;
    const uint32_t slot = m_gpuProfilerFrame;
    if (recorded) {
        const uint32_t count = m_gpuProfilerLedger.FinishFrame();
        if (count != 0 && m_context.IsFrameOpen()) {
            const uint32_t firstQuery = slot * GPU_MAX_PASSES * 2;
            /// @note Resolve は最終提出へ記録し、その提出後の実 Fence だけで回収可否を判定する。
            /// @see https://learn.microsoft.com/en-us/windows/win32/direct3d12/queries Queries
            m_context.GetCommandList()->ResolveQueryData(m_gpuQueryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                firstQuery, count * 2, m_gpuReadback.Get(), static_cast<UINT64>(firstQuery) * sizeof(uint64_t));
        }
    }
    m_gpuProfilerRecording = false;
    m_context.EndPixEvent(m_context.GetCommandList(), m_pixPassToken);
    m_context.EndPixEvent(m_context.GetCommandList(), m_pixViewToken);
    m_context.EndPixEvent(m_context.GetCommandList(), m_pixFrameToken);
    m_pixPassToken = m_pixViewToken = m_pixFrameToken = 0;
    m_pixViewName.clear();
    const uint64_t actualFence = m_context.EndFrame();
    EndPreparedSkinningFrame(m_preparedSkinningPool, actualFence);
    if (recorded) m_gpuProfilerLedger.SubmitFrame(slot, actualFence);
    if (m_bindingDiagnostics)
        std::fprintf(stderr, "DX12_BINDING_SCAN_JSON {\"frame\":%llu,\"resourceOwner\":%llu,\"observedResourceFrame\":%llu,\"resourceCoherent\":%s,\"fused\":%s,\"visits\":%llu,\"uploads\":%llu,\"failures\":%llu,\"bindingSectionMs\":%.9f}\n",
            static_cast<unsigned long long>(m_gpuPhysicalFrameSerial), static_cast<unsigned long long>(m_bindingResourceOwner),
            static_cast<unsigned long long>(m_bindingResourceFrame), m_bindingResourceCoherent ? "true" : "false",
            m_bindingFusionEnabled ? "true" : "false", static_cast<unsigned long long>(m_bindingDraws),
            static_cast<unsigned long long>(m_bindingUploads), static_cast<unsigned long long>(m_bindingFailures), m_bindingSectionMs);
}

void DX12Renderer::Clear(const math::Vector4& color)
{
    if (!m_context.IsFrameOpen())
        return;
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
        for (uint32_t index = 0; index < target->GetColorCount(); ++index)
            m_context.GetCommandList()->ClearRenderTargetView(
                target->GetRtv(index), clearColor, 0, nullptr);
        /// @note IRenderer::Clear は深度も最遠値へ戻す契約。RenderSystem は Clear(色) しか呼ばないので、
        /// @note       残すと前の値のまま深度比較が全滅する。
        if (target->HasDepth()) {
            m_context.GetCommandList()->ClearDepthStencilView(
                target->GetDsv(), D3D12_CLEAR_FLAG_DEPTH, FarDepth(target->IsReversedZ()), 0, 0, nullptr);
        }
    } else {
        m_context.MarkBackBufferWritten();
        m_context.GetCommandList()->ClearRenderTargetView(m_context.GetCurrentRtv(), clearColor, 0, nullptr);
        /// @note バックバッファも DX11 と同じく色クリア時に深度を 1.0 へ戻す。
        m_context.GetCommandList()->ClearDepthStencilView(
            m_context.GetDsv(), D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    }
}

void DX12Renderer::ClearDepth()
{
    if (m_currentColorAttachmentCount != 0) return;
    if (m_context.IsFrameOpen()) {
        if (IsCurrentRenderTargetLost()) {
            ReportLostRenderTarget("ClearDepth");
            return;
        }
        DX12RenderTarget* const target = ResolveCurrentRenderTarget();
        if (target && !target->HasDepth()) return;
        if (!target) m_context.MarkBackBufferWritten();
        const auto dsv = target ? target->GetDsv(target->IsCubemap() ? m_currentCubeMip : 0) : m_context.GetDsv();
        const float farDepth = FarDepth(target && target->IsReversedZ());
        m_context.GetCommandList()->ClearDepthStencilView(
            dsv, D3D12_CLEAR_FLAG_DEPTH, farDepth, 0, 0, nullptr);
    }
}

DX12RenderTarget* DX12Renderer::ResolveCurrentRenderTarget(ResourceManager* resources) const
{
    if (!m_currentRenderTargetHandle.IsValid()) return nullptr;
    auto* owner = resources ? resources : FindLiveResourceManager(m_currentRenderTargetOwner);
    if (!owner || owner->GetInstanceId() != m_currentRenderTargetOwner) return nullptr;
    auto* base = owner->Get(m_currentRenderTargetHandle);
    return base ? static_cast<DX12RenderTarget*>(base) : nullptr;
}

DX12RenderTarget* DX12Renderer::ResolveCurrentColorAttachment(uint32_t index, ResourceManager* resources) const
{
    if (index >= m_currentColorAttachmentCount) return nullptr;
    auto* owner = resources ? resources : FindLiveResourceManager(m_currentRenderTargetOwner);
    if (!owner || owner->GetInstanceId() != m_currentRenderTargetOwner) return nullptr;
    const auto& attachment = m_currentColorAttachments[index];
    auto* base = owner->Get(attachment.target);
    auto* target = base ? static_cast<DX12RenderTarget*>(base) : nullptr;
    return target && !target->IsCubemap() && attachment.colorIndex < target->GetColorCount() ? target : nullptr;
}

void DX12Renderer::TransitionCurrentTargetForRead()
{
    if (m_currentColorAttachmentCount != 0) {
        for (uint32_t index = 0; index < m_currentColorAttachmentCount; ++index)
            if (auto* target = ResolveCurrentColorAttachment(index))
                m_stateTracker.QueueTransition(target->GetColorResource(m_currentColorAttachments[index].colorIndex),
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        return;
    }
    if (auto* target = ResolveCurrentRenderTarget()) {
        if (target->IsCubemap())
            m_stateTracker.QueueTransition(target->GetCubeResource(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        else for (uint32_t index = 0; index < target->GetColorCount(); ++index)
            m_stateTracker.QueueTransition(target->GetColorResource(index), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        if (target->HasDepth())
            m_stateTracker.QueueTransition(target->GetDepthResource(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
}

bool DX12Renderer::IsCurrentRenderTargetLost(ResourceManager* resources) const
{
    for (uint32_t index = 0; index < m_currentColorAttachmentCount; ++index)
        if (!ResolveCurrentColorAttachment(index, resources)) return true;
    return m_currentRenderTargetHandle.IsValid() && ResolveCurrentRenderTarget(resources) == nullptr;
}

void DX12Renderer::ReportLostRenderTarget(const char* where)
{
    if (m_reportedLostRenderTarget) return;
    m_reportedLostRenderTarget = true;
    FBZZ_LOG_WARN("DX12Renderer::%s: 束縛中の RT が解放済み (handle=%u:%u)。"
                  "SetRenderTarget し直すまで描画を捨てます",
                  where, m_currentRenderTargetHandle.id, m_currentRenderTargetHandle.gen);
}

bool DX12Renderer::PrepareShaderReload()
{
    /// @note Flush は提出済みのコマンドだけを待つ。記録中の PSO を解放してはならない。
    if (m_context.IsFrameOpen()) {
        FBZZ_LOG_WARN("DX12Renderer: shader reload rejected while a frame is recording");
        return false;
    }
    m_context.Flush();
    m_psoCache.ClearPipelines();
    InvalidateRootCbvCache();
    return true;
}

void DX12Renderer::Submit(const DrawCall& call, ResourceManager& resources)
{
    if (!m_context.IsFrameOpen())
        return;
    const auto* prepared = call.preparedSkinning
        ? &prepared_skinning_detail::TicketFactory::Native(*call.preparedSkinning) : nullptr;
    const uint32_t preparedRow = call.preparedSkinningRow;
    const auto* preparedResource = prepared ? prepared->OutputResource(preparedRow) : nullptr;
    if (prepared && (!preparedResource || prepared->DeviceIdentity() != m_context.GetDevice())) return;
    const auto preparedRaw = [&](ResourceHandle<BufferTag> handle) {
        return prepared && preparedRow < prepared->writes.size() && handle == prepared->writes[preparedRow].output;
    };
    auto* shaderBase = resources.Get(call.shader);
    auto* stateBase = resources.Get(call.pipelineState);
    if (!shaderBase || !stateBase) {
        if (!m_reportedMissingDrawResource) {
            FBZZ_LOG_WARN("DX12Renderer::Submit: 描画リソース不足 "
                          "(shader=%u:%u valid=%d, pso=%u:%u valid=%d)",
                          call.shader.id, call.shader.gen, shaderBase != nullptr,
                          call.pipelineState.id, call.pipelineState.gen, stateBase != nullptr);
            m_reportedMissingDrawResource = true;
        }
        return;
    }
    auto* shader = static_cast<DX12Shader*>(shaderBase);
    auto* state = static_cast<DX12PipelineState*>(stateBase);
    const bool hasIndirectArguments = call.indirectArguments.IsValid();
    const bool meshDispatch = call.meshGroupCountX > 0;
    const bool meshShader = shader->IsMeshShader();
    if (meshDispatch != meshShader) {
        FBZZ_LOG_ERROR("DX12Renderer::Submit: shader stage and meshGroupCountX disagree (shader=%s)",
                       shader->GetPath().c_str());
        return;
    }
    if (!hasIndirectArguments && (call.indirectArgumentOffset != 0 || call.indirectCommandCount != 1)) {
        FBZZ_LOG_ERROR("DX12Renderer::Submit: indirect argument offset/count set without indirectArguments");
        return;
    }
    if (call.indirectInstanceOffset && (!hasIndirectArguments || meshDispatch)) {
        FBZZ_LOG_ERROR("DX12Renderer::Submit: indirectInstanceOffset requires indexed indirect arguments");
        return;
    }
    if (!meshDispatch && (call.meshGroupCountY != 0 || call.meshGroupCountZ != 0)) {
        FBZZ_LOG_ERROR("DX12Renderer::Submit: mesh group Y/Z set without a mesh dispatch");
        return;
    }
    if (meshDispatch) {
        if (!m_context.SupportsMeshShaders()
            || (call.topology != PrimitiveTopology::TRIANGLE_LIST
                && call.topology != PrimitiveTopology::LINE_LIST)
            || call.vertexBuffer.IsValid() || call.indexBuffer.IsValid()) {
            FBZZ_LOG_ERROR("DX12Renderer::Submit: unsupported mesh shader capability, topology or IA binding");
            return;
        }
        if (!hasIndirectArguments) {
            if (call.meshGroupCountY == 0 || call.meshGroupCountZ == 0
                || call.meshGroupCountX >= 65536 || call.meshGroupCountY >= 65536
                || call.meshGroupCountZ >= 65536) {
                FBZZ_LOG_ERROR("DX12Renderer::Submit: direct mesh group counts exceed D3D12 limits");
                return;
            }
            const uint64_t groupProduct = static_cast<uint64_t>(call.meshGroupCountX)
                * call.meshGroupCountY * call.meshGroupCountZ;
            if (groupProduct > (1u << 22u)) {
                FBZZ_LOG_ERROR("DX12Renderer::Submit: direct mesh group count product exceeds D3D12 limits");
                return;
            }
        }
    }
    if (call.topology != PrimitiveTopology::TRIANGLE_LIST
        && call.topology != PrimitiveTopology::LINE_LIST
        && (hasIndirectArguments || meshDispatch)) {
        FBZZ_LOG_ERROR("DX12Renderer::Submit: invalid primitive topology for indirect/mesh draw");
        return;
    }
    for (uint32_t slot = 0; slot < call.vsRawBuffers.size(); ++slot) {
        if (call.vsRawBuffers[slot].IsValid() && call.vsBuffers[slot].IsValid()) {
            FBZZ_LOG_ERROR("DX12Renderer::Submit: raw and structured VS buffer conflict at t%u",
                           14u + slot);
            return;
        }
        if (call.vsRawBuffers[slot].IsValid()) {
            if (preparedRaw(call.vsRawBuffers[slot])) continue;
            auto* rawBufferBase = resources.Get(call.vsRawBuffers[slot]);
            if (!rawBufferBase || rawBufferBase->GetBindlessSrvIndex() == INVALID_BINDLESS_INDEX) {
                FBZZ_LOG_ERROR("DX12Renderer::Submit: invalid or unreadable raw VS/MS buffer at t%u",
                               14u + slot);
                return;
            }
        }
    }
    for (const auto handle : call.indirectReadBuffers) {
        const auto* buffer = resources.Get(handle);
        if (!m_context.SupportsBindless() || !buffer || buffer->GetBindlessSrvIndex() == INVALID_BINDLESS_INDEX) {
            FBZZ_LOG_ERROR("DX12Renderer::Submit: stale or unreadable indirect vertex buffer");
            return;
        }
    }
    if (hasIndirectArguments) {
        const bool meshArguments = meshDispatch;
        const uint32_t argumentStride = meshArguments
            ? static_cast<uint32_t>(sizeof(D3D12_DISPATCH_MESH_ARGUMENTS))
            : static_cast<uint32_t>(sizeof(D3D12_DRAW_INDEXED_ARGUMENTS)
                + (call.indirectInstanceOffset ? sizeof(uint32_t) : 0u));
        auto* argumentBase = resources.Get(call.indirectArguments);
        auto* indexedCommandSignature = m_context.GetDrawIndexedCommandSignature();
        auto* indexedInstanceOffsetCommandSignature = m_psoCache.GetIndexedInstanceOffsetCommandSignature();
        auto* meshCommandSignature = m_context.GetDispatchMeshCommandSignature();
        if (!call.indirectCommandCount || call.indirectArgumentOffset % 4u != 0
            || (meshArguments ? (!m_context.SupportsMeshShaders() || !meshCommandSignature)
                              : ((call.indirectInstanceOffset
                                      ? !indexedInstanceOffsetCommandSignature : !indexedCommandSignature)
                                  || !call.indexBuffer.IsValid()))) {
            FBZZ_LOG_ERROR("DX12Renderer::Submit: invalid indirect command count/alignment/capability/binding");
            return;
        }
        if (!argumentBase) {
            FBZZ_LOG_ERROR("DX12Renderer::Submit: stale indirect argument buffer handle");
            return;
        }
        auto* argumentBuffer = static_cast<DX12StructuredBuffer*>(argumentBase);
        const uint64_t argumentBufferSize = static_cast<uint64_t>(argumentBuffer->GetSize());
        if (argumentBuffer->GetStride() != argumentStride
            || argumentBuffer->GetBindlessUavIndex() == INVALID_BINDLESS_INDEX
            || call.indirectArgumentOffset > argumentBufferSize
            || call.indirectCommandCount > (argumentBufferSize - call.indirectArgumentOffset) / argumentStride) {
            FBZZ_LOG_ERROR("DX12Renderer::Submit: indirect argument buffer stride/UAV/range invalid "
                           "(stride=%u expected=%u offset=%llu count=%u size=%llu)",
                           argumentBuffer->GetStride(), argumentStride,
                           static_cast<unsigned long long>(call.indirectArgumentOffset),
                           call.indirectCommandCount,
                           static_cast<unsigned long long>(argumentBufferSize));
            return;
        }
        if (call.instanceBuffer == call.indirectArguments
            || std::find(call.vsBuffers.begin(), call.vsBuffers.end(), call.indirectArguments) != call.vsBuffers.end()
            || std::find(call.psBuffers.begin(), call.psBuffers.end(), call.indirectArguments) != call.psBuffers.end()
            || std::find(call.auxiliaryPsBuffers.begin(), call.auxiliaryPsBuffers.end(),
                         call.indirectArguments) != call.auxiliaryPsBuffers.end()) {
            FBZZ_LOG_ERROR("DX12Renderer::Submit: indirect argument buffer cannot be shader-readable in the same draw");
            return;
        }
        if (!meshArguments) {
            auto* indexBase = resources.Get(call.indexBuffer);
            if (!indexBase || static_cast<DX12Buffer*>(indexBase)->GetKind() != DX12Buffer::Kind::Index) {
                FBZZ_LOG_ERROR("DX12Renderer::Submit: DRAW_INDEXED indirect draw requires a valid index buffer");
                return;
            }
        }
    }
    if (IsCurrentRenderTargetLost(&resources)) {
        ReportLostRenderTarget("Submit");
        return;
    }
    const DX12RenderTarget* const currentTarget = ResolveCurrentRenderTarget(&resources);
    if (m_currentColorAttachmentCount != 0) {
        const auto overlapsOutput = [&](ResourceHandle<TextureTag> handle) {
            const auto* textureBase = resources.Get(handle);
            if (!textureBase) return false;
            const auto* input = static_cast<const DX12Texture*>(textureBase)->GetResource();
            for (uint32_t index = 0; index < m_currentColorAttachmentCount; ++index)
                if (input == ResolveCurrentColorAttachment(index, &resources)->GetColorResource(
                    m_currentColorAttachments[index].colorIndex)) return true;
            return false;
        };
        if (std::any_of(call.textures.begin(), call.textures.end(), overlapsOutput)
            || std::any_of(call.indirectReadTextures.begin(), call.indirectReadTextures.end(), overlapsOutput)) {
            FBZZ_LOG_ERROR("DX12Renderer::Submit: color attachment cannot be shader-readable in the same draw");
            return;
        }
    }
    const DXGI_FORMAT renderTargetFormat = currentTarget
        ? currentTarget->GetColorFormat() : DX12Context::BACK_BUFFER_FORMAT;
    const uint32_t renderTargetCount = m_currentColorAttachmentCount != 0 ? m_currentColorAttachmentCount
        : currentTarget ? currentTarget->GetColorCount() : 1;
    const bool reversedZ = m_currentColorAttachmentCount == 0 && currentTarget && currentTarget->IsReversedZ();
    ID3D12PipelineState* pso = meshDispatch
        ? m_psoCache.GetOrCreateMesh(*shader, state->GetDesc(), call.topology,
                                     renderTargetFormat, renderTargetCount, reversedZ)
        : m_psoCache.GetOrCreate(*shader, state->GetDesc(), call.topology,
                                 renderTargetFormat, renderTargetCount, reversedZ);
    if (!pso) return;

    ID3D12GraphicsCommandList* commands = m_context.GetCommandList();
    if (!commands) {
        FBZZ_LOG_ERROR("DX12Renderer::Submit: graphics command list is unavailable");
        return;
    }
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList6> meshCommands;
    if (meshDispatch && !hasIndirectArguments
        && FAILED(commands->QueryInterface(IID_PPV_ARGS(&meshCommands)))) {
        FBZZ_LOG_ERROR("DX12Renderer::Submit: Mesh Shader command list interface is unavailable");
        return;
    }

    /// @note Probe/Sky capture draws occur outside the main view graph; record their face and shader.
    const std::string cubeEventName = m_currentCubeRtv.ptr != 0
        ? "CubeCapture target=" + std::to_string(m_currentRenderTargetHandle.id)
            + ":" + std::to_string(m_currentRenderTargetHandle.gen)
            + " face=" + std::to_string(m_currentCubeFace)
            + " mip=" + std::to_string(m_currentCubeMip) + " shader=" + shader->GetPath()
        : std::string{};
    DX12ScopedPixEvent cubeEvent(m_context, cubeEventName.empty() ? nullptr : commands,
                                 cubeEventName, 0xff65b891u);

    /// @note 共有コマンドリストへ他所 (ImGui / IblBaker) が記録していたら状態キャッシュを捨てる。
    if (m_seenPipelineStateGeneration != m_context.GetPipelineStateGeneration()) {
        InvalidateRootCbvCache();
        m_seenPipelineStateGeneration = m_context.GetPipelineStateGeneration();
    }

    ID3D12RootSignature* rootSignature = m_psoCache.GetRootSignature();
    const bool signatureChanged = m_lastGraphicsRootSignature != rootSignature;
    if (signatureChanged) InvalidateRootCbvCache();
    /// @note 直接索引するルートシグネチャは、先に CBV/SRV/UAV ヒープを束縛する必要がある。
    if (ID3D12DescriptorHeap* srvHeap = m_context.GetResourceSrvHeap();
        m_lastDescriptorHeap != srvHeap)
    {
        ID3D12DescriptorHeap* heaps[] = {srvHeap};
        commands->SetDescriptorHeaps(1, heaps);
        m_lastDescriptorHeap = srvHeap;
    }

    /// @note ルートシグネチャは変化したときだけ設定する。SetGraphicsRootSignature は全ルート引数を
    /// @note       無効化する契約のため、毎 Draw 呼ぶと直後の root CBV 差分キャッシュの前提が崩れる。
    if (signatureChanged) {
        /// @note 順序に注意: 無効化はルート引数のキャッシュを捨てると同時に直前値も nullptr へ戻すため、
        /// @note       先に無効化してから「今設定した」ことを記録する。
        commands->SetGraphicsRootSignature(rootSignature);
        m_lastGraphicsRootSignature = rootSignature;
    }
    if (m_lastPipelineState != pso) {
        commands->SetPipelineState(pso);
        m_lastPipelineState = pso;
    }
    /// @note 状態遷移はテーブル再利用時も必ず発行する。同じテクスチャ集合でも間に挟まった別パス
    /// @note       (Compute の UAV 書き込み等) で状態が変わりうるため、コピーは省けても遷移は省けない。
    /// @note Mesh Water の頂点評価も texture を読むため、mesh draw は pixel/non-pixel 両方で読める状態にする。
    /// @see https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_resource_states D3D12 shader-resource states.
    const D3D12_RESOURCE_STATES graphicsShaderResourceState = meshDispatch
        ? D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE
        : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    BindlessIndicesConstants indices;
    indices.Reset();
    const bool fusedBindings = m_bindingFusionEnabled;
    std::chrono::steady_clock::time_point diagnosticBegin;
    if (m_bindingDiagnostics) {
        diagnosticBegin = std::chrono::steady_clock::now();
        if (m_bindingDraws == 0) {
            m_bindingResourceOwner = resources.GetInstanceId(); m_bindingResourceFrame = resources.FrameStamp();
        } else if (m_bindingResourceOwner != resources.GetInstanceId() || m_bindingResourceFrame != resources.FrameStamp())
            m_bindingResourceCoherent = false;
        ++m_bindingDraws;
    }
    for (uint32_t slot = 0; slot < call.textures.size(); ++slot) {
        if (auto* textureBase = resources.Get(call.textures[slot])) {
            m_stateTracker.QueueTransition(static_cast<DX12Texture*>(textureBase)->GetResource(),
                                           graphicsShaderResourceState);
            if (fusedBindings) indices.pixel[slot] = textureBase->GetBindlessIndex();
        }
    }
    for (const auto handle : call.indirectReadTextures) {
        if (auto* textureBase = resources.Get(handle))
            m_stateTracker.QueueTransition(static_cast<DX12Texture*>(textureBase)->GetResource(),
                graphicsShaderResourceState);
    }
    /// @note t29〜t30 の StructuredBuffer はピクセルテーブルに入る。mesh draw では共有 shader-resource 状態へ遷移する。
    /// @note       クラスタライトのインデックスリストは直前に CS が UAV として書いているため、読取状態が必要。
    for (uint32_t i = 0; i < call.psBuffers.size(); ++i) {
        if (auto* bufferBase = resources.Get(call.psBuffers[i])) {
            m_stateTracker.QueueTransition(static_cast<DX12StructuredBuffer*>(bufferBase)->GetResource(),
                                           graphicsShaderResourceState);
            if (fusedBindings) indices.pixel[kPsBufferBaseSlot + i] = bufferBase->GetBindlessIndex();
        }
    }
    for (uint32_t i = 0; i < call.auxiliaryPsBuffers.size(); ++i) {
        if (auto* bufferBase = resources.Get(call.auxiliaryPsBuffers[i])) {
            m_stateTracker.QueueTransition(static_cast<DX12StructuredBuffer*>(bufferBase)->GetResource(),
                                           graphicsShaderResourceState);
            if (fusedBindings) indices.pixel[kAuxiliaryPsBufferBaseSlot + i] = bufferBase->GetBindlessIndex();
        }
    }

    /// @name bindless 添字ブロック (b14)
    /// @note 移行はシェーダー単位で段階的に進めるため、テーブルと添字を併存させる
    /// @note       (どちらも同じリソースを指す)。添字はドローごとに変わるので毎回組み直し、
    /// @note       Reset() で全枠を INVALID 埋めして前ドローの値の混入を防ぐ。
    {
        /// @note Descriptor getters retain texture/PS/aux order; indirect dependencies never receive indices. VS getter/transition order remains unchanged.
        if (!fusedBindings) {
            for (uint32_t slot = 0; slot < call.textures.size(); ++slot) {
                if (auto* textureBase = resources.Get(call.textures[slot]))
                    indices.pixel[slot] = textureBase->GetBindlessIndex();
            }
            for (uint32_t i = 0; i < call.psBuffers.size(); ++i) {
                if (auto* bufferBase = resources.Get(call.psBuffers[i]))
                    indices.pixel[kPsBufferBaseSlot + i] = bufferBase->GetBindlessIndex();
            }
            for (uint32_t i = 0; i < call.auxiliaryPsBuffers.size(); ++i) {
                if (auto* bufferBase = resources.Get(call.auxiliaryPsBuffers[i]))
                    indices.pixel[kAuxiliaryPsBufferBaseSlot + i] = bufferBase->GetBindlessIndex();
            }
        }
        if (auto* instanceBase = resources.Get(call.instanceBuffer))
            indices.vertex[0] = instanceBase->GetBindlessIndex();
        for (uint32_t i = 0; i < call.vsBuffers.size(); ++i) {
            if (auto* bufferBase = resources.Get(call.vsBuffers[i]))
                indices.vertex[1 + i] = bufferBase->GetBindlessIndex();
            if (preparedRaw(call.vsRawBuffers[i]))
                indices.vertex[1 + i] = prepared->OutputSrv(preparedRow);
            else if (auto* bufferBase = resources.Get(call.vsRawBuffers[i]))
                indices.vertex[1 + i] = bufferBase->GetBindlessSrvIndex();
        }

        const auto block = m_uploadArena.Allocate(
            sizeof(BindlessIndicesConstants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
        if (block) {
            if (m_bindingDiagnostics) ++m_bindingUploads;
            std::memcpy(block.cpu, &indices, sizeof(indices));
            commands->SetGraphicsRootConstantBufferView(kBindlessIndicesRootParam, block.gpu);
        } else {
            if (m_bindingDiagnostics) ++m_bindingFailures;
            if (m_invalidBindlessAddress) {
            /// @note アリーナ枯渇時のフォールバック。ゼロ埋めの m_nullConstantAddress は使わない
            /// @note       (添字 0 はヒープ先頭の有効なディスクリプタを指すため、無関係なリソースを読んでしまう)。
            /// @note       全枠 INVALID の専用ブロックを差せばシェーダー側の有効判定で弾ける。
                commands->SetGraphicsRootConstantBufferView(kBindlessIndicesRootParam,
                                                            m_invalidBindlessAddress);
            }
        }
    }

    if (m_bindingDiagnostics)
        m_bindingSectionMs += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - diagnosticBegin).count();

    /// @note 状態遷移は bindless でも必ず要る。添字が同じでも、間に挟まった別パス
    /// @note       (Compute の UAV 書き込み等) でリソースの状態は変わっている。
    if (auto* instanceBase = resources.Get(call.instanceBuffer))
        m_stateTracker.QueueTransition(
            static_cast<DX12StructuredBuffer*>(instanceBase)->GetResource(),
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    for (const auto& handle : call.vsBuffers)
        if (auto* bufferBase = resources.Get(handle))
            m_stateTracker.QueueTransition(
                static_cast<DX12StructuredBuffer*>(bufferBase)->GetResource(),
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    for (const auto handle : call.vsRawBuffers) {
        if (preparedRaw(handle)) {
            m_stateTracker.QueueTransition(const_cast<ID3D12Resource*>(static_cast<const ID3D12Resource*>(preparedResource)),
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            m_context.RetainPreparedSkinning(call.preparedSkinning);
            continue;
        }
        if (auto* bufferBase = resources.Get(handle)) {
            auto* buffer = static_cast<DX12Buffer*>(bufferBase);
            /// @note Upload-heap snapshots are already GENERIC_READ; GPU-written DEFAULT resources need an explicit SRV transition.
            if (buffer->IsGpuWritable())
                m_stateTracker.QueueTransition(buffer->GetSrvResource(),
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
    }
    for (const auto handle : call.indirectReadBuffers) {
        auto* buffer = static_cast<DX12Buffer*>(resources.Get(handle));
        if (buffer->IsGpuWritable()) m_stateTracker.QueueTransition(buffer->GetSrvResource(),
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    if (auto* argumentBase = resources.Get(call.indirectArguments))
        m_stateTracker.QueueTransition(
            static_cast<DX12StructuredBuffer*>(argumentBase)->GetResource(),
            D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);

    /// @note テクスチャ/バッファ SRV ループで溜めた遷移をここで 1 回の ResourceBarrier にまとめて発行する。
    /// @note       Draw 呼び出しより前であれば記録順の制約を満たす。
    m_stateTracker.FlushBarriers(commands);
    if (!meshDispatch) {
        commands->IASetPrimitiveTopology(call.topology == PrimitiveTopology::LINE_LIST
            ? D3D_PRIMITIVE_TOPOLOGY_LINELIST : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    }

    if (!meshDispatch && prepared) {
        auto* resource = const_cast<ID3D12Resource*>(static_cast<const ID3D12Resource*>(preparedResource));
        const bool rawRead = std::any_of(call.vsRawBuffers.begin(), call.vsRawBuffers.end(), preparedRaw);
        const auto state = rawRead ? D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
            : D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
        m_stateTracker.Transition(commands, resource, state);
        const D3D12_VERTEX_BUFFER_VIEW view{prepared->OutputAddress(preparedRow), prepared->OutputBytes(preparedRow), sizeof(Vertex)};
        if (!view.BufferLocation) return;
        m_context.RetainPreparedSkinning(call.preparedSkinning);
        commands->IASetVertexBuffers(0, 1, &view);
    } else if (!meshDispatch) if (auto* vertexBase = resources.Get(call.vertexBuffer)) {
        auto* vertexBuffer = static_cast<DX12Buffer*>(vertexBase);
        /// @note コンピュートスキニングの出力を頂点として読む場合、CS が書いた直後は UNORDERED_ACCESS の
        /// @note       ままなので VERTEX_AND_CONSTANT_BUFFER へ遷移させる。DX11 と違い DX12 は状態遷移が明示的で、
        /// @note       抜けると読み出しが未定義になる (デバッグレイヤーが警告、実機では古い内容やゴミが出る)。
        if (vertexBuffer->IsGpuWritable()) {
            m_stateTracker.Transition(commands, vertexBuffer->GetResource(),
                                      D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
        }
        const auto view = vertexBuffer->GetVertexView(m_uploadArena);
        if (view.BufferLocation == 0) return;
        commands->IASetVertexBuffers(0, 1, &view);
    }
    if (!meshDispatch) if (auto* indexBase = resources.Get(call.indexBuffer)) {
        const auto view = static_cast<DX12Buffer*>(indexBase)->GetIndexView(m_uploadArena);
        if (view.BufferLocation == 0) return;
        commands->IASetIndexBuffer(&view);
    }

    /// @note b0〜b13 を差分で束縛する (未指定スロットは null CBV で埋める契約は従来どおり)。
    /// @note       毎 Draw 全スロット設定すると最大 28 回のルート設定が出るため、直前の GPU VA を
    /// @note       スロットごとに記憶し変化分だけ設定する。
    for (uint32_t slot = 0; slot < call.constantBuffers.size(); ++slot) {
        D3D12_GPU_VIRTUAL_ADDRESS address = 0;
        if (auto* constantBase = resources.Get(call.constantBuffers[slot]))
            address = static_cast<DX12ConstantBuffer*>(constantBase)->PrepareForSubmit();
        if (address == 0)
            address = m_nullConstantAddress;
        if (address == 0)
            continue;
        if (m_rootCbvCacheValid && m_lastRootCbv[slot] == address)
            continue;
        commands->SetGraphicsRootConstantBufferView(slot, address);
        m_lastRootCbv[slot] = address;
    }
    m_rootCbvCacheValid = true;

    /// @name 診断ログ
    /// @note リフレクション推定ストライドが実バッファより「大きい」場合のみ警告する。
    /// @note       小さい場合 (例: 44B 頂点から先頭 POSITION 12B だけ読む ShadowMap / Skydome) は、
    /// @note       IASetVertexBuffers のストライドは実バッファ値なので正しく先頭要素を読める正当なパターン。
    /// @note       大きい場合は要素オフセットが頂点境界をまたぎ、ジオメトリが壊れる。
    auto* vertexBufferBase = resources.Get(call.vertexBuffer);
    const uint32_t actualStride = prepared && !meshDispatch ? sizeof(Vertex) : vertexBufferBase && !meshDispatch
        ? static_cast<DX12Buffer*>(vertexBufferBase)->GetStride() : 0;
    const uint32_t reflectedStride = meshDispatch ? 0 : shader->GetReflectedStride();
    if (actualStride != 0 && reflectedStride > actualStride
        && m_strideWarned.insert(shader).second) {
        FBZZ_LOG_WARN("[DX12] 頂点ストライド不一致! shader=%s vb=%uB reflected=%uB "
                      "(入力レイアウトのオフセットが頂点境界をまたぎジオメトリが壊れます)",
                      shader->GetPath().c_str(), actualStride, reflectedStride);
    }
    /// @note ExecuteIndirect が書いた root constant は完了時に 0 へ戻る。direct draw も b15 を明示初期化する。
    /// @see https://github.com/microsoft/DirectX-Specs/blob/master/d3d/IndirectDrawing.md State leakage.
    commands->SetGraphicsRoot32BitConstant(kIndirectInstanceOffsetRootParam, 0, 0);
    if (prepared && !call.preparedSkinning->IsWriteCurrent(preparedRow)) return;
    if (!currentTarget) m_context.MarkBackBufferWritten();
    if (hasIndirectArguments) {
        auto* argumentBuffer = static_cast<DX12StructuredBuffer*>(resources.Get(call.indirectArguments));
        if (prepared && !call.preparedSkinning->IsWriteCurrent(preparedRow)) return;
        /// @see https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-executeindirect ExecuteIndirect argument buffer state and range.
        ID3D12CommandSignature* signature = meshDispatch ? m_context.GetDispatchMeshCommandSignature()
            : call.indirectInstanceOffset ? m_psoCache.GetIndexedInstanceOffsetCommandSignature()
                                          : m_context.GetDrawIndexedCommandSignature();
        commands->ExecuteIndirect(signature,
                                  call.indirectCommandCount, argumentBuffer->GetResource(),
                                  call.indirectArgumentOffset, nullptr, 0);
    } else if (meshDispatch) {
        meshCommands->DispatchMesh(call.meshGroupCountX, call.meshGroupCountY, call.meshGroupCountZ);
    } else {
        const uint32_t instanceCount = call.instanceCount == 0 ? 1 : call.instanceCount;
        if (call.indexCount > 0) {
            commands->DrawIndexedInstanced(call.indexCount, instanceCount, call.startIndex,
                                           static_cast<INT>(call.baseVertex), 0);
        } else if (call.vertexCount > 0) {
            commands->DrawInstanced(call.vertexCount, instanceCount, 0, 0);
        }
    }
}

void DX12Renderer::Dispatch(const ComputeCall& call, ResourceManager& resources)
{
    (void)TryDispatch(call, resources);
}

bool DX12Renderer::TryDispatch(const ComputeCall& call, ResourceManager& resources)
{
    if (!m_context.IsFrameOpen() || !call.dispatchX || !call.dispatchY || !call.dispatchZ) return false;
    for (uint32_t slot = 0; slot < call.accelerationStructures.size(); ++slot) {
        const auto handle = call.accelerationStructures[slot];
        if (!handle.IsValid()) continue;
        auto* structure = resources.Get(handle);
        if (m_asyncComputeActive || !m_context.SupportsInlineRaytracing() || !structure
            || !structure->IsBuilt() || structure->GetKind() != AccelerationStructureKind::TOP_LEVEL
            || structure->GetBindlessIndex() == INVALID_BINDLESS_INDEX
            || call.srvInputs[slot].IsValid() || call.srvBuffers[slot].IsValid()
            || call.srvRawBuffers[slot].IsValid()) {
            FBZZ_LOG_ERROR("DX12Renderer: invalid TLAS dispatch binding at slot %u", slot);
            return false;
        }
    }
    const auto validateRawRead = [&](ResourceHandle<BufferTag> handle) {
        auto* buffer = resources.Get(handle);
        if (m_asyncComputeActive || !buffer || handle == call.uavVertexBuffer
            || buffer->GetBindlessSrvIndex() == INVALID_BINDLESS_INDEX) {
            FBZZ_LOG_ERROR("DX12Renderer: invalid or stale raw buffer dispatch binding");
            return false;
        }
        return true;
    };
    for (uint32_t slot = 0; slot < call.srvRawBuffers.size(); ++slot) {
        const auto handle = call.srvRawBuffers[slot];
        if (!handle.IsValid()) continue;
        if (call.srvInputs[slot].IsValid() || call.srvBuffers[slot].IsValid()
            || call.accelerationStructures[slot].IsValid()) {
            FBZZ_LOG_ERROR("DX12Renderer: conflicting raw buffer dispatch slot %u", slot);
            return false;
        }
        if (!validateRawRead(handle)) return false;
    }
    for (const auto handle : call.indirectReadBuffers) {
        if (!validateRawRead(handle)) return false;
    }
    for (size_t i = 0; i < call.indirectWriteVertexBuffers.size(); ++i) {
        const auto handle = call.indirectWriteVertexBuffers[i];
        const auto* buffer = resources.Get(handle);
        if (m_asyncComputeActive || !buffer || !m_context.SupportsBindless()
            || !static_cast<const DX12Buffer*>(buffer)->IsGpuWritable()
            || buffer->GetBindlessUavIndex() == INVALID_BINDLESS_INDEX
            || handle == call.uavVertexBuffer
            || std::find(call.indirectReadBuffers.begin(), call.indirectReadBuffers.end(), handle) != call.indirectReadBuffers.end()
            || std::find(call.srvRawBuffers.begin(), call.srvRawBuffers.end(), handle) != call.srvRawBuffers.end()
            || std::find(call.indirectWriteVertexBuffers.begin(), call.indirectWriteVertexBuffers.begin() + i, handle)
                != call.indirectWriteVertexBuffers.begin() + i) {
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
    auto* shaderBase = resources.Get(call.shader);
    if (!shaderBase) return false;
    auto* shader = static_cast<DX12Shader*>(shaderBase);
    if (!shader->IsCompute()) return false;
    ID3D12PipelineState* pso = m_psoCache.GetOrCreateCompute(*shader);
    if (!pso) return false;
    ID3D12GraphicsCommandList* commands = RecordingList();
    if (!commands) return false;
    /// @note 明示したハンドルの解放や descriptor 不足を null binding の成功へ変換しない。
    for (const auto handle : call.srvInputs) {
        if (!handle.IsValid()) continue;
        const auto* texture = resources.Get(handle);
        if (!texture || texture->GetBindlessIndex() == INVALID_BINDLESS_INDEX) return false;
    }
    for (const auto handle : call.srvBuffers) {
        if (!handle.IsValid()) continue;
        const auto* buffer = resources.Get(handle);
        if (!buffer || buffer->GetBindlessIndex() == INVALID_BINDLESS_INDEX) return false;
    }
    for (const auto handle : call.uavOutputs) {
        if (!handle.IsValid()) continue;
        const auto* texture = resources.Get(handle);
        if (!texture || texture->GetBindlessUavIndex() == INVALID_BINDLESS_INDEX) return false;
    }
    for (const auto handle : call.uavBuffers) {
        if (!handle.IsValid()) continue;
        const auto* buffer = resources.Get(handle);
        if (!buffer || buffer->GetBindlessUavIndex() == INVALID_BINDLESS_INDEX) return false;
    }
    if (call.uavVertexBuffer.IsValid()) {
        const auto* buffer = resources.Get(call.uavVertexBuffer);
        if (!buffer || !static_cast<const DX12Buffer*>(buffer)->IsGpuWritable()
            || buffer->GetBindlessUavIndex() == INVALID_BINDLESS_INDEX) return false;
    }
    std::array<D3D12_GPU_VIRTUAL_ADDRESS, 14> constantAddresses{};
    for (uint32_t slot = 0; slot < call.constantBuffers.size(); ++slot) {
        constantAddresses[slot] = m_nullConstantAddress;
        if (call.constantBuffers[slot].IsValid()) {
            auto* buffer = resources.Get(call.constantBuffers[slot]);
            if (!buffer) return false;
            constantAddresses[slot] = static_cast<DX12ConstantBuffer*>(buffer)->PrepareForSubmit();
        }
        if (!constantAddresses[slot]) return false;
    }
    const auto indicesBlock = m_uploadArena.Allocate(
        sizeof(BindlessIndicesConstants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
    if (!indicesBlock) return false;
    /// @note Compute へ切り替えるとグラフィクス側のパイプライン状態・ルート束縛は当てにできない。
    /// @note       Submit 側の差分キャッシュをここで必ず捨てる (捨て忘れると次の Draw が束縛を省いて壊れる)。
    InvalidateRootCbvCache();
    ID3D12DescriptorHeap* heaps[] = {m_context.GetResourceSrvHeap()};
    /// @note Directly indexed heaps must be bound before the root signature.
    /// @see https://microsoft.github.io/DirectX-Specs/d3d/HLSL_SM_6_6_DynamicResources.html#setting-and-changing-descriptor-heaps-and-root-signatures SM 6.6 binding order
    commands->SetDescriptorHeaps(1, heaps);
    commands->SetComputeRootSignature(m_psoCache.GetComputeRootSignature());
    commands->SetPipelineState(pso);
    for (uint32_t slot = 0; slot < call.constantBuffers.size(); ++slot) {
        commands->SetComputeRootConstantBufferView(slot, constantAddresses[slot]);
    }

    /// @note SRV の状態遷移。bindless ではディスクリプタを張らないが、遷移は従来どおり要る
    /// @note       (添字が同じでも間に挟まった別パスで状態は変わりうる)。
    for (uint32_t slot = 0; slot < kBindlessPixelSlotCount; ++slot) {
        if (auto* textureBase = resources.Get(call.srvInputs[slot]))
            m_stateTracker.QueueTransition(
                static_cast<DX12Texture*>(textureBase)->GetResource(),
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (auto* bufferBase = resources.Get(call.srvBuffers[slot]))
            m_stateTracker.QueueTransition(
                static_cast<DX12StructuredBuffer*>(bufferBase)->GetResource(),
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    const auto transitionRawRead = [&](ResourceHandle<BufferTag> handle) {
        if (auto* bufferBase = resources.Get(handle)) {
            auto* buffer = static_cast<DX12Buffer*>(bufferBase);
            /// @note UPLOAD snapshot は GENERIC_READ を維持する。GPU 変形の DEFAULT 実体だけ遷移させる。
            if (buffer->IsGpuWritable())
                m_stateTracker.QueueTransition(buffer->GetSrvResource(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
    };
    for (const auto handle : call.srvRawBuffers) {
        if (handle.IsValid()) transitionRawRead(handle);
    }
    for (const auto handle : call.indirectReadBuffers)
        transitionRawRead(handle);
    for (const auto handle : call.indirectReadTextures)
        m_stateTracker.QueueTransition(static_cast<DX12Texture*>(resources.Get(handle))->GetResource(),
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    /// @note UAV の状態遷移と、Dispatch 後の UAV バリア対象の収集。
    std::vector<ID3D12Resource*> writtenResources(11u + call.indirectWriteVertexBuffers.size());
    uint32_t writtenCount = 0;
    for (uint32_t slot = 0; slot < kBindlessUavSlotCount; ++slot) {
        if (auto* textureBase = resources.Get(call.uavOutputs[slot])) {
            auto* texture = static_cast<DX12Texture*>(textureBase);
            m_stateTracker.QueueTransition(texture->GetResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            writtenResources[writtenCount++] = texture->GetResource();
        }
        if (slot >= 2 && slot <= 3) {
            if (auto* bufferBase = resources.Get(call.uavBuffers[slot - 2])) {
                auto* buffer = static_cast<DX12StructuredBuffer*>(bufferBase);
                m_stateTracker.QueueTransition(buffer->GetResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                writtenResources[writtenCount++] = buffer->GetResource();
            }
        }
        /// @note u4: GPU 書き込み可能な頂点バッファ (コンピュートスキニングの出力)。直前フレームでは
        /// @note       頂点バッファとして読まれているので、書き込む前に UNORDERED_ACCESS へ戻す遷移が要る。
        if (slot == 4) {
            if (auto* bufferBase = resources.Get(call.uavVertexBuffer)) {
                auto* buffer = static_cast<DX12Buffer*>(bufferBase);
                if (buffer->IsGpuWritable()) {
                    m_stateTracker.QueueTransition(buffer->GetResource(),
                                                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    writtenResources[writtenCount++] = buffer->GetResource();
                }
            }
        }
    }

    for (const auto handle : call.indirectWriteVertexBuffers) {
        auto* buffer = static_cast<DX12Buffer*>(resources.Get(handle));
        m_stateTracker.QueueTransition(buffer->GetResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        writtenResources[writtenCount++] = buffer->GetResource();
    }

    /// @name bindless 添字ブロック (b14)
    /// @note Texture / StructuredBuffer の既存競合は後勝ちを保つ。Raw Buffer と TLAS の競合は記録前に拒否する。
    {
        BindlessIndicesConstants indices;
        indices.Reset();
        for (uint32_t slot = 0; slot < kBindlessPixelSlotCount; ++slot) {
            if (auto* textureBase = resources.Get(call.srvInputs[slot]))
                indices.pixel[slot] = textureBase->GetBindlessIndex();
            if (auto* bufferBase = resources.Get(call.srvBuffers[slot]))
                indices.pixel[slot] = bufferBase->GetBindlessIndex();
            if (auto* bufferBase = resources.Get(call.srvRawBuffers[slot]))
                indices.pixel[slot] = bufferBase->GetBindlessSrvIndex();
            if (auto* structure = resources.Get(call.accelerationStructures[slot]))
                indices.pixel[slot] = structure->GetBindlessIndex();
        }
        for (uint32_t slot = 0; slot < kBindlessUavSlotCount; ++slot) {
            if (auto* textureBase = resources.Get(call.uavOutputs[slot]))
                indices.uav[slot] = textureBase->GetBindlessUavIndex();
            if (slot >= 2 && slot <= 3) {
                if (auto* bufferBase = resources.Get(call.uavBuffers[slot - 2]))
                    indices.uav[slot] = bufferBase->GetBindlessUavIndex();
            }
            if (slot == 4) {
                if (auto* bufferBase = resources.Get(call.uavVertexBuffer))
                    indices.uav[slot] = bufferBase->GetBindlessUavIndex();
            }
        }
        std::memcpy(indicesBlock.cpu, &indices, sizeof(indices));
        commands->SetComputeRootConstantBufferView(kBindlessIndicesRootParam, indicesBlock.gpu);
    }
    const std::string computeEventName = (m_asyncComputeActive ? "ComputeQueue " : "ComputeDirect ")
        + m_pixViewName + " shader=" + shader->GetPath();
    DX12ScopedPixEvent computeEvent(m_context, commands, computeEventName, 0xff9d78c9u);
    /// @note SRV/UAV ループで溜めた遷移をここで 1 回の ResourceBarrier にまとめて発行する (Dispatch より前)。
    m_stateTracker.FlushBarriers(commands);
    commands->Dispatch(call.dispatchX, call.dispatchY, call.dispatchZ);
    if (auto* output = resources.Get(call.uavVertexBuffer); output && !m_asyncComputeActive)
        output->NotifyGpuWrite();
    for (const auto handle : call.indirectWriteVertexBuffers)
        resources.Get(handle)->NotifyGpuWrite();
    if (writtenCount > 0) {
        if (m_computeBatchActive) {
            /// @note バッチ内 Dispatch は相互依存しない契約なので、ここでは記録だけ行う。
            /// @note       同じ UAV が複数回現れてもパス末尾のバリアは 1 個で十分。
            for (uint32_t index = 0; index < writtenCount; ++index) {
                if (std::find(m_computeBatchWrittenResources.begin(),
                              m_computeBatchWrittenResources.end(),
                              writtenResources[index]) == m_computeBatchWrittenResources.end()) {
                    m_computeBatchWrittenResources.push_back(writtenResources[index]);
                }
            }
        } else {
            /// @note 通常 Dispatch は後続 Dispatch が同じ UAV を読む可能性があるため即時同期する。
            std::vector<D3D12_RESOURCE_BARRIER> uavBarriers(writtenCount);
            for (uint32_t index = 0; index < writtenCount; ++index) {
                uavBarriers[index].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                uavBarriers[index].UAV.pResource = writtenResources[index];
            }
            commands->ResourceBarrier(writtenCount, uavBarriers.data());
        }
    }
    return true;
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
    if (!structure->Init(m_context, m_stateTracker, desc, resources)) return nullptr;
    return structure;
}

void DX12Renderer::BeginComputeBatch()
{
    /// @note ネストは契約外。既存バッチを安全に閉じてから新しい収集を開始する。
    if (m_computeBatchActive) EndComputeBatch();
    m_computeBatchWrittenResources.clear();
    m_computeBatchActive = true;
}

void DX12Renderer::EndComputeBatch()
{
    if (!m_computeBatchActive) return;

    if (m_context.IsFrameOpen() && !m_computeBatchWrittenResources.empty()) {
        std::vector<D3D12_RESOURCE_BARRIER> barriers(m_computeBatchWrittenResources.size());
        for (size_t index = 0; index < m_computeBatchWrittenResources.size(); ++index) {
            barriers[index].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            barriers[index].UAV.pResource = m_computeBatchWrittenResources[index];
        }
        /// @note Dispatch ごとの API 呼び出しをやめ、パス全体を 1 回の UAV barrier 群で確定する。
        /// @note 非同期区間の中ならコンピュートリストへ積む。Dispatch を記録した列と別の列へ
        /// @note       バリアを積むと、守りたい順序の外に出てしまう。
        if (ID3D12GraphicsCommandList* commands = RecordingList())
            commands->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
    }

    m_computeBatchWrittenResources.clear();
    m_computeBatchActive = false;
}

ID3D12GraphicsCommandList* DX12Renderer::RecordingList() const
{
    if (m_asyncComputeActive && m_context.IsComputeListOpen())
        return m_context.GetComputeList();
    return m_context.GetCommandList();
}

void DX12Renderer::RestoreGraphicsTargetState(ResourceManager&)
{
    ID3D12GraphicsCommandList* commands = m_context.GetCommandList();
    if (!commands) return;
    if (IsCurrentRenderTargetLost()) {
        ReportLostRenderTarget("RestoreGraphicsTargetState");
        return;
    }
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
        commands->RSSetViewports(1, &m_currentViewport);
        commands->RSSetScissorRects(1, &m_currentScissor);
        return;
    }

    auto* target = ResolveCurrentRenderTarget();
    if (target) {
        /// @note 区間へ入る前に COMMON へ落としてあるので、描ける状態へ戻す。状態表は 1 か所なので
        /// @note       ここで積んでおけば、次に束縛する側の判断とも食い違わない。
        for (uint32_t index = 0; index < target->GetColorCount(); ++index)
            m_stateTracker.QueueTransition(target->GetColorResource(index),
                                           D3D12_RESOURCE_STATE_RENDER_TARGET);
        if (target->HasDepth())
            m_stateTracker.QueueTransition(target->GetDepthResource(),
                                           D3D12_RESOURCE_STATE_DEPTH_WRITE);
    }
    m_stateTracker.FlushBarriers(commands);

    if (m_currentCubeRtv.ptr != 0) {
        /// @note キューブ面を描いている途中 (SkyCapture)。面の RTV をそのまま張り直す。
        const auto dsv = target ? target->GetDsv(m_currentCubeMip) : D3D12_CPU_DESCRIPTOR_HANDLE{};
        commands->OMSetRenderTargets(1, &m_currentCubeRtv, FALSE, dsv.ptr ? &dsv : nullptr);
    } else if (target) {
        std::array<D3D12_CPU_DESCRIPTOR_HANDLE, DX12RenderTarget::MAX_COLOR> rtvs{};
        for (uint32_t index = 0; index < target->GetColorCount(); ++index)
            rtvs[index] = target->GetRtv(index);
        const auto dsv = target->GetDsv();
        commands->OMSetRenderTargets(target->GetColorCount(), rtvs.data(), FALSE,
                                     target->HasDepth() ? &dsv : nullptr);
    } else {
        const auto rtv = m_context.GetCurrentRtv();
        const auto dsv = m_context.GetDsv();
        commands->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
    }
    commands->RSSetViewports(1, &m_currentViewport);
    commands->RSSetScissorRects(1, &m_currentScissor);
}

bool DX12Renderer::BeginAsyncCompute(ResourceManager& resources)
{
    if (m_asyncComputeActive) return true;
    if (!m_context.IsFrameOpen() || !m_context.SupportsAsyncCompute()) return false;

    /// @note コンピュートキューは PIXEL_SHADER_RESOURCE / RENDER_TARGET / DEPTH_* を扱えない。
    /// @note       区間へ渡す前に、描画キュー側で追跡中のリソースをまとめて COMMON へ落とす。
    /// @see Docs/design/async-compute.md §3
    m_stateTracker.QueueTransitionAllToCommon();
    m_stateTracker.FlushBarriers(m_context.GetCommandList());

    /// @note ここで描画の列を割る。直前までの描画が GPU へ投入され、その完了をコンピュートが待つ。
    const uint64_t waitValue = m_context.SplitGraphicsList();
    if (waitValue == 0) return false;

    /// @note 列を割ると Reset で全パイプライン状態が落ちる。束縛の記憶を捨て、描画先を張り直す。
    InvalidateRootCbvCache();
    RestoreGraphicsTargetState(resources);

    if (!m_context.BeginComputeList(waitValue)) return false;
    m_asyncComputeActive = true;
    return true;
}

void DX12Renderer::EndAsyncCompute()
{
    if (!m_asyncComputeActive) return;
    m_asyncComputeActive = false;
    m_context.EndComputeList();
    /// @note コンピュート側で束縛したルートシグネチャ・ヒープは描画リストには載っていない。
    /// @note       区間の前後で記憶が食い違わないよう、ここでも捨てる。
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
    if (!m_context.IsFrameOpen()) return;
    ID3D12GraphicsCommandList* commands = m_context.GetCommandList();
    /// @note 前の RT はハンドルから引き直す。生ポインタのまま触ると、束縛したあとに解放された
    /// @note       RT (リサイズで作り直された中間 RT 等) を «読める状態へ戻す» つもりで破棄済みの
    /// @note       オブジェクトから番地を引くことになる。解放済みなら Get が nullptr を返し、
    /// @note       戻し忘れたぶんの遷移は次にそのリソースを束縛する側が積み直す。
    TransitionCurrentTargetForRead();
    auto* targetBase = resources.Get(handle);
    auto* target = targetBase ? static_cast<DX12RenderTarget*>(targetBase) : nullptr;
    m_currentRenderTargetHandle = target ? handle : ResourceHandle<RenderTargetTag>{};
    m_currentRenderTargetOwner = target ? resources.GetInstanceId() : 0;
    m_currentColorAttachmentCount = 0;
    m_currentCubeRtv = {};
    m_currentCubeMip = 0;
    if (!target) {
        m_stateTracker.FlushBarriers(commands);
        const auto rtv = m_context.GetCurrentRtv();
        const auto dsv = m_context.GetDsv();
        commands->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
        D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(m_context.GetWidth()),
                                static_cast<float>(m_context.GetHeight()), 0.0f, 1.0f};
        D3D12_RECT scissor{0, 0, static_cast<LONG>(m_context.GetWidth()),
                           static_cast<LONG>(m_context.GetHeight())};
        commands->RSSetViewports(1, &viewport);
        commands->RSSetScissorRects(1, &scissor);
        m_currentViewport = viewport;
        m_currentScissor = scissor;
        return;
    }
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, DX12RenderTarget::MAX_COLOR> rtvs{};
    for (uint32_t index = 0; index < target->GetColorCount(); ++index) {
        m_stateTracker.QueueTransition(target->GetColorResource(index),
                                       D3D12_RESOURCE_STATE_RENDER_TARGET);
        rtvs[index] = target->GetRtv(index);
    }
    const bool hasDepth = target->HasDepth();
    if (hasDepth) {
        m_stateTracker.QueueTransition(target->GetDepthResource(),
                                       D3D12_RESOURCE_STATE_DEPTH_WRITE);
    }
    /// @note 旧 RT の解放遷移と新 RT のバインド遷移を 1 回の ResourceBarrier にまとめる。
    m_stateTracker.FlushBarriers(commands);
    const auto dsv = target->GetDsv();
    commands->OMSetRenderTargets(target->GetColorCount(), rtvs.data(), FALSE,
                                 hasDepth ? &dsv : nullptr);
    D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(target->GetWidth()),
                            static_cast<float>(target->GetHeight()), 0.0f, 1.0f};
    D3D12_RECT scissor{0, 0, static_cast<LONG>(target->GetWidth()),
                       static_cast<LONG>(target->GetHeight())};
    commands->RSSetViewports(1, &viewport);
    commands->RSSetScissorRects(1, &scissor);
    m_currentViewport = viewport;
    m_currentScissor = scissor;
}

bool DX12Renderer::SetRenderTargetColors(std::span<const RenderTargetColorAttachment> colors, ResourceManager& resources)
{
    if (!m_context.IsFrameOpen() || m_asyncComputeActive || colors.empty()
        || colors.size() > DX12RenderTarget::MAX_COLOR) return false;
    auto* commands = m_context.GetCommandList();
    if (!commands) return false;
    std::array<ID3D12Resource*, DX12RenderTarget::MAX_COLOR> nativeColors{};
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, DX12RenderTarget::MAX_COLOR> rtvs{};
    uint32_t width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    for (size_t index = 0; index < colors.size(); ++index) {
        const auto& attachment = colors[index];
        auto* base = resources.Get(attachment.target);
        auto* target = base ? static_cast<DX12RenderTarget*>(base) : nullptr;
        if (!target || target->IsCubemap() || target->HasDepth()
            || attachment.colorIndex >= target->GetColorCount()) return false;
        if (index == 0) {
            width = target->GetWidth(); height = target->GetHeight(); format = target->GetColorFormat();
        } else if (target->GetWidth() != width || target->GetHeight() != height
            || target->GetColorFormat() != format) return false;
        nativeColors[index] = target->GetColorResource(attachment.colorIndex);
        rtvs[index] = target->GetRtv(attachment.colorIndex);
        if (!nativeColors[index] || !rtvs[index].ptr) return false;
        for (size_t previous = 0; previous < index; ++previous)
            if (nativeColors[index] == nativeColors[previous]) return false;
    }
    /// @note 全面を検証してから遷移と束縛を変更し、入力不正で既存の出力状態を失わない。
    TransitionCurrentTargetForRead();
    for (size_t index = 0; index < colors.size(); ++index)
        m_stateTracker.QueueTransition(nativeColors[index], D3D12_RESOURCE_STATE_RENDER_TARGET);
    m_stateTracker.FlushBarriers(commands);
    m_currentColorAttachmentCount = static_cast<uint32_t>(colors.size());
    std::copy(colors.begin(), colors.end(), m_currentColorAttachments.begin());
    m_currentRenderTargetHandle = colors.front().target;
    m_currentRenderTargetOwner = resources.GetInstanceId();
    m_currentCubeRtv = {};
    m_currentCubeMip = 0;
    commands->OMSetRenderTargets(m_currentColorAttachmentCount, rtvs.data(), FALSE, nullptr);
    m_currentViewport = {0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
    m_currentScissor = {0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    commands->RSSetViewports(1, &m_currentViewport);
    commands->RSSetScissorRects(1, &m_currentScissor);
    return true;
}

void DX12Renderer::SetViewport(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
    /// @note SetRenderTarget が RT 全体へ張ったビューポートを、その一部へ絞り込む。
    /// @note       カスケードシャドウが 1 枚のアトラスをタイル分割して使う (IRenderer::SetViewport 参照)。
    if (!m_context.IsFrameOpen() || width == 0u || height == 0u) return;
    auto* commands = m_context.GetCommandList();
    if (!commands) return;

    D3D12_VIEWPORT viewport{ static_cast<float>(x), static_cast<float>(y),
                             static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f };
    /// @note シザーもタイルへ合わせる。DX12 はビューポート外でもシザーが広いままだと
    /// @note       隣のタイルへピクセルが漏れる (DX11 と違いシザーが既定で無制限ではない)。
    D3D12_RECT scissor{ static_cast<LONG>(x), static_cast<LONG>(y),
                        static_cast<LONG>(x + width), static_cast<LONG>(y + height) };
    commands->RSSetViewports(1, &viewport);
    commands->RSSetScissorRects(1, &scissor);
    m_currentViewport = viewport;
    m_currentScissor = scissor;
}

void DX12Renderer::SetRenderTargetFace(
    ResourceHandle<RenderTargetTag> handle, uint32_t face, uint32_t mip, ResourceManager& resources)
{
    if (!m_context.IsFrameOpen()) return;
    auto* targetBase = resources.Get(handle);
    auto* target = targetBase ? static_cast<DX12RenderTarget*>(targetBase) : nullptr;
    if (!target || !target->IsCubemap() || face >= 6 || mip >= target->GetMipCount()) return;
    TransitionCurrentTargetForRead();
    m_currentRenderTargetHandle = handle;
    m_currentRenderTargetOwner = resources.GetInstanceId();
    m_currentColorAttachmentCount = 0;
    m_currentCubeRtv = target->GetFaceRtv(face, mip);
    m_currentCubeFace = face;
    m_currentCubeMip = mip;
    m_stateTracker.QueueTransition(target->GetCubeResource(), D3D12_RESOURCE_STATE_RENDER_TARGET);
    m_stateTracker.QueueTransition(target->GetDepthResource(), D3D12_RESOURCE_STATE_DEPTH_WRITE);
    m_stateTracker.FlushBarriers(m_context.GetCommandList());
    const auto dsv = target->GetDsv(mip);
    m_context.GetCommandList()->OMSetRenderTargets(1, &m_currentCubeRtv, FALSE, &dsv);
    const uint32_t mipSize = (std::max)(1u, target->GetWidth() >> mip);
    D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(mipSize),
                            static_cast<float>(mipSize), 0.0f, 1.0f};
    D3D12_RECT scissor{0, 0, static_cast<LONG>(mipSize), static_cast<LONG>(mipSize)};
    m_context.GetCommandList()->RSSetViewports(1, &viewport);
    m_context.GetCommandList()->RSSetScissorRects(1, &scissor);
    m_currentViewport = viewport;
    m_currentScissor = scissor;
}

bool DX12Renderer::RenderDebugPreview(const DrawCall& call, ResourceHandle<RenderTargetTag> target,
                                      ResourceManager& resources)
{
    if (!m_context.IsFrameOpen() || !resources.Get(target) || !resources.Get(call.shader)
        || !resources.Get(call.pipelineState)) return false;
    auto* previewTarget = static_cast<DX12RenderTarget*>(resources.Get(target));
    auto* previewShader = static_cast<DX12Shader*>(resources.Get(call.shader));
    auto* previewState = static_cast<DX12PipelineState*>(resources.Get(call.pipelineState));
    if (!m_psoCache.GetOrCreate(*previewShader, previewState->GetDesc(), call.topology,
                               previewTarget->GetColorFormat(), previewTarget->GetColorCount(),
                               previewTarget->IsReversedZ())) return false;
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
    Submit(call, resources);
    /// @note キューブ RT は通常添付の RTV を持たない。プレビューを外してから面として戻す。
    auto* previousResources = FindLiveResourceManager(previousOwner);
    if (previousResources) {
        if (previousColorCount != 0) {
            if (!SetRenderTargetColors(std::span{previousColors.data(), previousColorCount}, *previousResources))
                SetRenderTarget({}, *previousResources);
        } else {
            SetRenderTarget(cube ? ResourceHandle<RenderTargetTag>{} : previous, *previousResources);
            if (cube) SetRenderTargetFace(previous, face, mip, *previousResources);
        }
    } else SetRenderTarget({}, resources);
    auto* commands = m_context.GetCommandList();
    commands->RSSetViewports(1, &viewport);
    commands->RSSetScissorRects(1, &scissor);
    m_currentViewport = viewport;
    m_currentScissor = scissor;
    return true;
}

bool DX12Renderer::BakeSkyLight(
    ResourceHandle<RenderTargetTag> handle, ResourceManager& resources,
    uint32_t irradianceSize, uint32_t prefilterSize, uint32_t prefilterMips,
    uint32_t sampleCount, std::unique_ptr<ITexture>& outIrradiance,
    std::unique_ptr<ITexture>& outPrefilter)
{
    auto* targetBase = resources.Get(handle);
    auto* target = targetBase ? static_cast<DX12RenderTarget*>(targetBase) : nullptr;
    if (!target || !target->IsCubemap()) return false;
    DX12ScopedPixEvent bakeEvent(m_context, m_context.GetCommandList(),
        "Probe/Sky IBL Convolve target=" + std::to_string(handle.id)
            + ":" + std::to_string(handle.gen), 0xff65b891u);
    if (!m_iblBaker)
        m_iblBaker = std::make_unique<DX12IblBaker>(
            &m_context, &m_stateTracker, &m_psoCache, &m_uploadArena);
    std::unique_ptr<DX12Texture> irradiance;
    std::unique_ptr<DX12Texture> prefilter;
    if (!m_iblBaker->Convolve(*target, irradianceSize, prefilterSize, prefilterMips,
                              sampleCount, irradiance, prefilter)) return false;
    outIrradiance = std::move(irradiance);
    outPrefilter = std::move(prefilter);
    return true;
}

std::unique_ptr<IIblBaker> DX12Renderer::CreateIblBaker()
{
    /// @note Editor が所有する一時ベイカー。m_context / m_psoCache はレンダラー寿命内で有効。
    return std::make_unique<DX12HdriBaker>(&m_context, &m_psoCache);
}

/// @note RT のカラーを CPU 側 ScratchImage として掴む。PNG 化と数値評価で同じ読み戻しを共有する。
static bool CaptureDX12RenderTargetImage(DX12Context& context, IRenderTarget* base,
                                         DirectX::ScratchImage& outImage)
{
    auto* target = static_cast<DX12RenderTarget*>(base);
    if (target == nullptr) return false;
    ID3D12Resource* resource = target->GetColorResource(0);
    if (resource == nullptr) return false;

    /// @note Scene View RT は直前フレームで ImGui サンプリング用に PIXEL_SHADER_RESOURCE へ遷移済み。
    /// @note       CaptureTexture は自前の CommandQueue/フェンス同期で COPY_SOURCE へ遷移→読み戻し→元状態へ戻す。
    /// @note       呼び出しはフレーム外 (OnUpdate) なのでレンダラーの CommandList とは競合しない。
    return SUCCEEDED(DirectX::CaptureTexture(context.GetCommandQueue(), resource, false, outImage,
                                             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
}

bool DX12Renderer::CaptureRenderTargetToPng(ResourceHandle<RenderTargetTag> rt, ResourceManager& resources,
                                            std::vector<uint8_t>& outPng, uint32_t& outWidth, uint32_t& outHeight)
{
    DirectX::ScratchImage captured;
    if (!CaptureDX12RenderTargetImage(m_context, resources.Get(rt), captured)) return false;
    return detail::EncodeCapturedImageToPng(captured, outPng, outWidth, outHeight);
}

bool DX12Renderer::CaptureRenderTargetToLinearRGBA(ResourceHandle<RenderTargetTag> rt, ResourceManager& resources,
                                                   std::vector<float>& outRgba, uint32_t& outWidth, uint32_t& outHeight)
{
    DirectX::ScratchImage captured;
    if (!CaptureDX12RenderTargetImage(m_context, resources.Get(rt), captured)) return false;
    return detail::ReadCapturedImageAsLinearRGBA(captured, outRgba, outWidth, outHeight);
}

std::unique_ptr<IBuffer> DX12Renderer::CreateNativeVertexBuffer(
    const void* data, size_t sizeBytes, uint32_t stride)
{
    auto buffer = std::make_unique<DX12Buffer>();
    if (!buffer->Init(&m_context, data, sizeBytes, stride, DX12Buffer::Kind::Vertex))
        return nullptr;
    return buffer;
}

std::unique_ptr<IBuffer> DX12Renderer::CreateNativeGpuWritableVertexBuffer(
    size_t sizeBytes, uint32_t stride)
{
    auto buffer = std::make_unique<DX12Buffer>();
    if (!buffer->InitGpuWritableVertex(&m_context, &m_stateTracker, sizeBytes, stride))
        return nullptr;
    return buffer;
}

std::unique_ptr<IBuffer> DX12Renderer::CreateNativeIndexBuffer(const void* data, uint32_t count)
{
    auto buffer = std::make_unique<DX12Buffer>();
    if (!buffer->Init(&m_context, data, static_cast<size_t>(count) * sizeof(uint32_t),
                      0, DX12Buffer::Kind::Index))
        return nullptr;
    return buffer;
}

std::unique_ptr<IConstantBuffer> DX12Renderer::CreateNativeConstantBuffer(size_t sizeBytes)
{
    auto buffer = std::make_unique<DX12ConstantBuffer>();
    if (!buffer->Init(&m_uploadArena, sizeBytes))
        return nullptr;
    return buffer;
}
std::unique_ptr<IShader> DX12Renderer::CreateNativeShader(const std::string& path)
{
    auto shader = std::make_unique<DX12Shader>();
    if (!shader->Init(path))
        return nullptr;
    return shader;
}
std::unique_ptr<ITexture> DX12Renderer::CreateNativeTexture(const std::string& path)
{
    auto texture = std::make_unique<DX12Texture>();
    if (!texture->Init(&m_context, path))
        return nullptr;
    texture->RegisterState(&m_stateTracker, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return texture;
}

std::unique_ptr<ITexture> DX12Renderer::CreateNativeTextureFromData(
    const uint8_t* rgba, uint32_t width, uint32_t height)
{
    auto texture = std::make_unique<DX12Texture>();
    if (!texture->InitFromData(&m_context, rgba, width, height))
        return nullptr;
    texture->RegisterState(&m_stateTracker, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return texture;
}
std::unique_ptr<ITexture> DX12Renderer::CreateNativeTextureFromDataMips(
    const TextureMipData* mips, uint32_t mipCount)
{
    auto texture = std::make_unique<DX12Texture>();
    if (!texture->InitFromDataMips(&m_context, mips, mipCount))
        return nullptr;
    texture->RegisterState(&m_stateTracker, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return texture;
}
std::unique_ptr<ITexture> DX12Renderer::CreateNativeTextureFromDataMipsAsync(
    const TextureMipData* mips, uint32_t mipCount, uint64_t& outUploadToken)
{
    auto texture = std::make_unique<DX12Texture>();
    if (!texture->InitFromDataMipsAsync(&m_context, mips, mipCount, outUploadToken))
        return nullptr;
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
    if (!texture->InitFromData3D(&m_context, rgba, width, height, depth))
        return nullptr;
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
                                   target.GetWidth(), target.GetHeight()))
        return nullptr;
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
    if (!target->Init(&m_context, &m_stateTracker, width, height, desc))
        return nullptr;
    return target;
}

std::unique_ptr<IRenderTarget> DX12Renderer::CreateNativeCubemapRenderTarget(
    uint32_t size, uint32_t mipCount)
{
    auto target = std::make_unique<DX12RenderTarget>();
    if (!target->InitCubemap(&m_context, &m_stateTracker, size, mipCount)) return nullptr;
    return target;
}

std::unique_ptr<ITexture> DX12Renderer::CreateNativeCubeTextureFromRenderTarget(IRenderTarget& renderTarget)
{
    auto& target = static_cast<DX12RenderTarget&>(renderTarget);
    if (!target.IsCubemap()) return nullptr;
    auto texture = std::make_unique<DX12Texture>();
    if (!texture->InitCubeFromResource(&m_context, target.GetCubeResource(), target.GetColorFormat(),
                                       target.GetWidth(), target.GetMipCount())) return nullptr;
    return texture;
}
std::unique_ptr<ITexture> DX12Renderer::CreateNativeComputeTexture(uint32_t width, uint32_t height)
{
    auto texture = std::make_unique<DX12Texture>();
    if (!texture->InitForCompute(&m_context, &m_stateTracker, width, height)) return nullptr;
    return texture;
}

std::unique_ptr<ITexture> DX12Renderer::CreateNativeComputeTexture3D(
    uint32_t width, uint32_t height, uint32_t depth)
{
    auto texture = std::make_unique<DX12Texture>();
    if (!texture->InitForCompute3D(&m_context, &m_stateTracker, width, height, depth)) return nullptr;
    return texture;
}

std::unique_ptr<ITexture> DX12Renderer::CreateNativeDynamicTexture(
    uint32_t width, uint32_t height, DynamicTextureFormat format)
{
    auto texture = std::make_unique<DX12Texture>();
    if (!texture->InitDynamic(&m_context, &m_stateTracker, width, height, format)) return nullptr;
    return texture;
}

std::unique_ptr<IStructuredBuffer> DX12Renderer::CreateNativeStructuredBuffer(
    const void* data, uint32_t count, uint32_t stride)
{
    auto buffer = std::make_unique<DX12StructuredBuffer>();
    if (!buffer->Init(&m_context, &m_stateTracker, data, count, stride, false)) return nullptr;
    return buffer;
}

std::unique_ptr<IStructuredBuffer> DX12Renderer::CreateNativeGpuLocalStructuredBuffer(
    const void* data, uint32_t count, uint32_t stride)
{
    auto buffer = std::make_unique<DX12StructuredBuffer>();
    if (!buffer->Init(&m_context, &m_stateTracker, data, count, stride, false, true)) return nullptr;
    return buffer;
}

std::unique_ptr<IStructuredBuffer> DX12Renderer::CreateNativeRWStructuredBuffer(
    const void* data, uint32_t count, uint32_t stride)
{
    auto buffer = std::make_unique<DX12StructuredBuffer>();
    if (!buffer->Init(&m_context, &m_stateTracker, data, count, stride, true)) return nullptr;
    return buffer;
}

bool DX12Renderer::InitializeGpuProfiler()
{
    D3D12_QUERY_HEAP_DESC queryDesc{};
    queryDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    queryDesc.Count = DX12Context::FRAME_COUNT * GPU_MAX_PASSES * 2;
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
    /// @note 旧 API は互換 no-op。物理 BeginFrame だけが query 領域を開始する。
}

void DX12Renderer::GpuProfBeginPass(const char* name)
{
    if (!m_context.IsFrameOpen()) return;
    /// @note PIX scopes are independent of timestamp query capacity and list-split cancellation.
    m_context.EndPixEvent(m_context.GetCommandList(), m_pixPassToken);
    m_pixPassToken = m_context.BeginPixEvent(m_context.GetCommandList(), name ? name : "Unknown");
    if (!m_gpuProfilerRecording) return;
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
    if (!m_gpuProfilerRecording || !m_context.IsFrameOpen()) return;
    if (m_gpuPassListSerial != m_context.GetGraphicsCommandListSerial()) {
        /// @note 別 list の timestamp 比較は stable power 未設定では保証されないため、分割・別キュー区間を未計測にする。
        /// @see https://learn.microsoft.com/en-us/windows/win32/direct3d12/queries Differences in Queries: disjoint timestamps
        m_gpuProfilerLedger.CancelOpenPass();
        return;
    }
    uint32_t query = 0;
    if (m_gpuProfilerLedger.EndPass(name ? name : "Unknown", query))
        m_context.GetCommandList()->EndQuery(m_gpuQueryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, query);
}

void DX12Renderer::GpuProfEndFrame()
{
    /// @note 旧 API は互換 no-op。Resolve と提出 Fence の捕捉は物理 EndFrame に閉じる。
}

void DX12Renderer::GpuProfCollect()
{
    if (!m_gpuMappedTimestamps || m_gpuTimestampFrequency == 0) return;
    m_gpuProfilerLedger.Collect(m_context.GetCompletedFenceValue(),
        {m_gpuMappedTimestamps, static_cast<size_t>(DX12Context::FRAME_COUNT) * GPU_MAX_PASSES * 2},
        m_gpuTimestampFrequency);
    if (!m_gpuProfilerLedger.GetSnapshot().supported) m_gpuProfilerRecording = false;
}

bool DX12Renderer::GpuProfBeginView(const GpuProfilerViewMetadata& metadata)
{
    if (!m_context.IsFrameOpen() || m_pixViewToken != 0 || m_pixPassToken != 0
        || metadata.width == 0 || metadata.height == 0) return false;
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
    m_pixViewToken = m_context.BeginPixEvent(m_context.GetCommandList(), m_pixViewName, 0xff589cb4u);
    if (m_gpuProfilerRecording) m_gpuProfilerLedger.BeginView(metadata);
    /// @note Returning a recording scope enables named pass hooks even when timestamps are unavailable.
    return m_pixViewToken != 0;
}

void DX12Renderer::GpuProfEndView()
{
    m_context.EndPixEvent(m_context.GetCommandList(), m_pixPassToken);
    m_context.EndPixEvent(m_context.GetCommandList(), m_pixViewToken);
    m_pixPassToken = m_pixViewToken = 0;
    m_pixViewName.clear();
    if (m_gpuProfilerRecording) m_gpuProfilerLedger.EndView();
}

} /// @note namespace fbzz::renderer
