// FBZZ Engine
// DX12Renderer.cpp | fbzz::renderer
// DirectX 12 バックバッファのフレーム記録とクリア操作
#include "DX12Renderer.hpp"

#include <Engine/Core/Logger.hpp>
#include "DX12Buffer.hpp"
#include "DX12ConstantBuffer.hpp"
#include "DX12PipelineState.hpp"
#include "DX12Shader.hpp"
#include "DX12Texture.hpp"
#include "DX12IblBaker.hpp"
#include "DX12HdriBaker.hpp"
#include "DX12RenderTarget.hpp"
#include "DX12StructuredBuffer.hpp"
#include <Engine/Renderer/ResourceManager.hpp>
#include "../RenderTargetCapture.hpp" // AI 連携: RT → PNG エンコード共通処理
#include <DirectXTex.h>
#include <cstring>
#include <algorithm>
#include <cstdio>

namespace fbzz::renderer {

// out-of-line 定義。ここは DX12IblBaker.hpp を include 済みなので、m_iblBaker
// (unique_ptr<DX12IblBaker>) のデリーターを完全型として実体化できる。
DX12Renderer::DX12Renderer() = default;
DX12Renderer::~DX12Renderer() = default;

bool DX12Renderer::Init(HWND hwnd, uint32_t width, uint32_t height)
{
    constexpr size_t UPLOAD_BYTES_PER_FRAME = 16u * 1024u * 1024u;
    if (!m_context.Initialize(hwnd, width, height)) {
        FBZZ_LOG_ERROR("DX12Renderer::Init: DX12Context 初期化失敗");
        return false;
    }
    if (!m_uploadArena.Initialize(m_context.GetDevice(), UPLOAD_BYTES_PER_FRAME)) {
        FBZZ_LOG_ERROR("DX12Renderer::Init: UploadArena 初期化失敗");
        m_context.Shutdown();
        return false;
    }
    if (!m_psoCache.Initialize(m_context.GetDevice())) {
        FBZZ_LOG_ERROR("DX12Renderer::Init: PsoCache/RootSignature 初期化失敗");
        m_uploadArena.Shutdown();
        m_context.Shutdown();
        return false;
    }
    if (!InitializeGpuProfiler()) {
        FBZZ_LOG_WARN("DX12Renderer: GPUプロファイラーを初期化できませんでした");
    }
    return true;
}

void DX12Renderer::Shutdown()
{
    m_context.Flush();
    m_iblBaker.reset();
    if (m_gpuReadback && m_gpuMappedTimestamps) m_gpuReadback->Unmap(0, nullptr);
    m_gpuMappedTimestamps = nullptr;
    m_gpuReadback.Reset();
    m_gpuQueryHeap.Reset();
    m_gpuResults.clear();
    m_psoCache.Shutdown();
    m_stateTracker.Clear();
    m_uploadArena.Shutdown();
    m_context.Shutdown();
}

void DX12Renderer::BeginFrame()
{
    if (m_context.BeginFrame()) {
        m_currentRenderTarget = nullptr;
        m_currentCubeRtv = {};
        // WHAT: shader-visible SRVリングはフレーム単位で巻き戻るため、前フレームのGPUテーブル
        //       キャッシュは無効。次Submitで必ず再割当・再コピーさせる。
        m_lastPixelTableValid = false;
        m_lastVertexTableValid = false;
        m_uploadArena.BeginFrame(m_context.GetFrameIndex());
        m_nullConstantAddress = 0;
        const auto nullConstant = m_uploadArena.Allocate(
            D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT,
            D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
        if (nullConstant) {
            std::memset(nullConstant.cpu, 0, nullConstant.size);
            m_nullConstantAddress = nullConstant.gpu;
        }
    }
}

void DX12Renderer::EndFrame()
{
    m_context.EndFrame();
}

void DX12Renderer::Clear(const math::Vector4& color)
{
    if (!m_context.IsFrameOpen())
        return;
    const float clearColor[] = {color.x, color.y, color.z, color.w};
    if (m_currentRenderTarget) {
        if (m_currentRenderTarget->IsCubemap() && m_currentCubeRtv.ptr) {
            m_context.GetCommandList()->ClearRenderTargetView(m_currentCubeRtv, clearColor, 0, nullptr);
            return;
        }
        for (uint32_t index = 0; index < m_currentRenderTarget->GetColorCount(); ++index)
            m_context.GetCommandList()->ClearRenderTargetView(
                m_currentRenderTarget->GetRtv(index), clearColor, 0, nullptr);
        // WHY: DX11Renderer::Clear は色と同時に深度も 1.0 へクリアする契約で、
        //      RenderSystem は GBuffer / HDR パス開始時に Clear(色) しか呼ばない。
        //      DX12 側で深度を残すと初期値 0 のまま LESS 比較が全滅し、
        //      深度テストを使う全ジオメトリが 1 ピクセルも描画されない。
        m_context.GetCommandList()->ClearDepthStencilView(
            m_currentRenderTarget->GetDsv(), D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    } else {
        m_context.GetCommandList()->ClearRenderTargetView(m_context.GetCurrentRtv(), clearColor, 0, nullptr);
        // バックバッファも DX11 と同じく色クリア時に深度を 1.0 へ戻す。
        m_context.GetCommandList()->ClearDepthStencilView(
            m_context.GetDsv(), D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    }
}

void DX12Renderer::ClearDepth(float depth)
{
    if (m_context.IsFrameOpen()) {
        if (m_currentRenderTarget && m_currentRenderTarget->IsCubemap()) return;
        const auto dsv = m_currentRenderTarget ? m_currentRenderTarget->GetDsv() : m_context.GetDsv();
        m_context.GetCommandList()->ClearDepthStencilView(
            dsv, D3D12_CLEAR_FLAG_DEPTH, depth, 0, 0, nullptr);
    }
}

void DX12Renderer::Submit(const DrawCall& call, ResourceManager& resources)
{
    if (!m_context.IsFrameOpen())
        return;
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
    const DXGI_FORMAT renderTargetFormat = m_currentRenderTarget
        ? m_currentRenderTarget->GetColorFormat() : DX12Context::BACK_BUFFER_FORMAT;
    const uint32_t renderTargetCount = m_currentRenderTarget
        ? m_currentRenderTarget->GetColorCount() : 1;
    ID3D12PipelineState* pso = m_psoCache.GetOrCreate(
        *shader, state->GetDesc(), call.topology, renderTargetFormat, renderTargetCount);
    if (!pso) return;

    ID3D12GraphicsCommandList* commands = m_context.GetCommandList();
    commands->SetGraphicsRootSignature(m_psoCache.GetRootSignature());
    commands->SetPipelineState(pso);
    ID3D12DescriptorHeap* heaps[] = {m_context.GetResourceSrvHeap()};
    commands->SetDescriptorHeaps(1, heaps);
    // WHAT: 直前Drawと同じテクスチャ集合なら CopyDescriptorsSimple 一式を省略し、
    //       前回割り当てた shader-visible テーブルをそのまま再バインドする。
    //       (リソースの状態遷移だけは、他パスでステートが変わっている可能性があるため常に発行する)
    if (m_lastPixelTableValid && call.textures == m_lastPixelTextures) {
        for (uint32_t slot = 0; slot < call.textures.size(); ++slot) {
            if (auto* textureBase = resources.Get(call.textures[slot])) {
                auto* texture = static_cast<DX12Texture*>(textureBase);
                m_stateTracker.QueueTransition(texture->GetResource(),
                                               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            }
        }
        commands->SetGraphicsRootDescriptorTable(14, m_lastPixelTableGpu);
    } else {
        const auto textureTable = m_context.AllocatePixelSrvTable();
        if (textureTable) {
            auto destination = textureTable.cpu;
            for (uint32_t slot = 0; slot < call.textures.size(); ++slot) {
                D3D12_CPU_DESCRIPTOR_HANDLE source = m_context.GetNullPixelSrv(slot);
                if (auto* textureBase = resources.Get(call.textures[slot])) {
                    auto* texture = static_cast<DX12Texture*>(textureBase);
                    m_stateTracker.QueueTransition(texture->GetResource(),
                                                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                    source = texture->GetSrvCpu();
                }
                m_context.GetDevice()->CopyDescriptorsSimple(
                    1, destination, source, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                destination.ptr += m_context.GetSrvDescriptorIncrement();
            }
            commands->SetGraphicsRootDescriptorTable(14, textureTable.gpu);
            m_lastPixelTextures = call.textures;
            m_lastPixelTableGpu = textureTable.gpu;
            m_lastPixelTableValid = true;
        } else {
            commands->SetGraphicsRootDescriptorTable(14, m_context.GetNullPixelSrvTable());
            m_lastPixelTableValid = false;
        }
    }

    const std::array<ResourceHandle<StructuredBufferTag>, 3> vertexSignature{
        call.instanceBuffer, call.vsBuffers[0], call.vsBuffers[1]};
    if (m_lastVertexTableValid && vertexSignature == m_lastVertexBuffers) {
        if (resources.Get(call.instanceBuffer))
            m_stateTracker.QueueTransition(
                static_cast<DX12StructuredBuffer*>(resources.Get(call.instanceBuffer))->GetResource(),
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        for (const auto& handle : call.vsBuffers)
            if (resources.Get(handle))
                m_stateTracker.QueueTransition(
                    static_cast<DX12StructuredBuffer*>(resources.Get(handle))->GetResource(),
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        commands->SetGraphicsRootDescriptorTable(15, m_lastVertexTableGpu);
    } else {
        const auto vertexTable = m_context.AllocateVertexSrvTable();
        if (vertexTable) {
            auto destination = vertexTable.cpu;
            for (uint32_t slot = 0; slot < 16; ++slot) {
                D3D12_CPU_DESCRIPTOR_HANDLE source = m_context.GetNullBufferSrv(slot);
                DX12StructuredBuffer* buffer = nullptr;
                if (slot == 0 && resources.Get(call.instanceBuffer))
                    buffer = static_cast<DX12StructuredBuffer*>(resources.Get(call.instanceBuffer));
                else if (slot >= 14 && slot <= 15 && resources.Get(call.vsBuffers[slot - 14]))
                    buffer = static_cast<DX12StructuredBuffer*>(resources.Get(call.vsBuffers[slot - 14]));
                if (buffer) {
                    m_stateTracker.QueueTransition(buffer->GetResource(),
                                                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                    source = buffer->GetSrv();
                }
                m_context.GetDevice()->CopyDescriptorsSimple(
                    1, destination, source, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                destination.ptr += m_context.GetSrvDescriptorIncrement();
            }
            commands->SetGraphicsRootDescriptorTable(15, vertexTable.gpu);
            m_lastVertexBuffers = vertexSignature;
            m_lastVertexTableGpu = vertexTable.gpu;
            m_lastVertexTableValid = true;
        } else {
            commands->SetGraphicsRootDescriptorTable(15, m_context.GetNullVertexSrvTable());
            m_lastVertexTableValid = false;
        }
    }
    // WHAT: テクスチャ/バッファ SRV ループで溜めた遷移をここで 1 回の ResourceBarrier にまとめて発行する。
    //       Draw 呼び出し (このあと) より前であれば記録順の制約を満たす。
    m_stateTracker.FlushBarriers(commands);
    for (uint32_t slot = 0; slot < call.constantBuffers.size(); ++slot) {
        if (m_nullConstantAddress != 0)
            commands->SetGraphicsRootConstantBufferView(slot, m_nullConstantAddress);
    }
    commands->IASetPrimitiveTopology(call.topology == PrimitiveTopology::LINE_LIST
        ? D3D_PRIMITIVE_TOPOLOGY_LINELIST : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    if (auto* vertexBase = resources.Get(call.vertexBuffer)) {
        const auto view = static_cast<DX12Buffer*>(vertexBase)->GetVertexView();
        commands->IASetVertexBuffers(0, 1, &view);
    }
    if (auto* indexBase = resources.Get(call.indexBuffer)) {
        const auto view = static_cast<DX12Buffer*>(indexBase)->GetIndexView();
        commands->IASetIndexBuffer(&view);
    }

    for (uint32_t slot = 0; slot < call.constantBuffers.size(); ++slot) {
        auto* constantBase = resources.Get(call.constantBuffers[slot]);
        if (!constantBase)
            continue;
        const D3D12_GPU_VIRTUAL_ADDRESS address =
            static_cast<DX12ConstantBuffer*>(constantBase)->PrepareForSubmit();
        if (address != 0)
            commands->SetGraphicsRootConstantBufferView(slot, address);
    }

    // ---- 診断ログ ----
    // リフレクション推定ストライドが実バッファより「大きい」場合のみ警告する。
    // 小さい場合 (例: 44B 頂点から先頭 POSITION 12B だけ読む ShadowMap / Skydome) は、
    // IASetVertexBuffers のストライドは実バッファ値なので正しく先頭要素を読める正当なパターン。
    // 大きい場合は要素オフセットが頂点境界をまたぎ、ジオメトリが壊れる。
    auto* vertexBufferBase = resources.Get(call.vertexBuffer);
    const uint32_t actualStride = vertexBufferBase
        ? static_cast<DX12Buffer*>(vertexBufferBase)->GetStride() : 0;
    const uint32_t reflectedStride = shader->GetReflectedStride();
    if (actualStride != 0 && reflectedStride > actualStride
        && m_strideWarned.insert(shader).second) {
        FBZZ_LOG_WARN("[DX12] 頂点ストライド不一致! shader=%s vb=%uB reflected=%uB "
                      "(入力レイアウトのオフセットが頂点境界をまたぎジオメトリが壊れます)",
                      shader->GetPath().c_str(), actualStride, reflectedStride);
    }
    const uint32_t instanceCount = call.instanceCount == 0 ? 1 : call.instanceCount;
    if (call.indexCount > 0) {
        commands->DrawIndexedInstanced(call.indexCount, instanceCount, call.startIndex,
                                       static_cast<INT>(call.baseVertex), 0);
    } else if (call.vertexCount > 0) {
        commands->DrawInstanced(call.vertexCount, instanceCount, 0, 0);
    }
}

void DX12Renderer::Dispatch(const ComputeCall& call, ResourceManager& resources)
{
    if (!m_context.IsFrameOpen()) return;
    auto* shaderBase = resources.Get(call.shader);
    if (!shaderBase) return;
    auto* shader = static_cast<DX12Shader*>(shaderBase);
    ID3D12PipelineState* pso = m_psoCache.GetOrCreateCompute(*shader);
    if (!pso) return;
    ID3D12GraphicsCommandList* commands = m_context.GetCommandList();
    commands->SetComputeRootSignature(m_psoCache.GetComputeRootSignature());
    commands->SetPipelineState(pso);
    ID3D12DescriptorHeap* heaps[] = {m_context.GetResourceSrvHeap()};
    commands->SetDescriptorHeaps(1, heaps);
    for (uint32_t slot = 0; slot < call.constantBuffers.size(); ++slot) {
        D3D12_GPU_VIRTUAL_ADDRESS address = m_nullConstantAddress;
        if (auto* base = resources.Get(call.constantBuffers[slot])) {
            const auto current = static_cast<DX12ConstantBuffer*>(base)->PrepareForSubmit();
            if (current) address = current;
        }
        if (address) commands->SetComputeRootConstantBufferView(slot, address);
    }

    const auto srvTable = m_context.AllocatePixelSrvTable();
    if (!srvTable) return;
    auto srvDestination = srvTable.cpu;
    for (uint32_t slot = 0; slot < 32; ++slot) {
        D3D12_CPU_DESCRIPTOR_HANDLE source = m_context.GetNullPixelSrv(slot);
        if (auto* textureBase = resources.Get(call.srvInputs[slot])) {
            auto* texture = static_cast<DX12Texture*>(textureBase);
            m_stateTracker.QueueTransition(texture->GetResource(),
                                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            source = texture->GetSrvCpu();
        }
        if (slot >= 14 && slot <= 15) {
            if (auto* bufferBase = resources.Get(call.srvBuffers[slot - 14])) {
                auto* buffer = static_cast<DX12StructuredBuffer*>(bufferBase);
                m_stateTracker.QueueTransition(buffer->GetResource(),
                                               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                source = buffer->GetSrv();
            }
        }
        m_context.GetDevice()->CopyDescriptorsSimple(
            1, srvDestination, source, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        srvDestination.ptr += m_context.GetSrvDescriptorIncrement();
    }
    commands->SetComputeRootDescriptorTable(14, srvTable.gpu);

    const auto uavTable = m_context.AllocateUavTable();
    if (!uavTable) return;
    auto uavDestination = uavTable.cpu;
    std::array<ID3D12Resource*, 10> writtenResources{};
    uint32_t writtenCount = 0;
    for (uint32_t slot = 0; slot < 8; ++slot) {
        D3D12_CPU_DESCRIPTOR_HANDLE source = m_context.GetNullUav(slot);
        if (auto* textureBase = resources.Get(call.uavOutputs[slot])) {
            auto* texture = static_cast<DX12Texture*>(textureBase);
            m_stateTracker.QueueTransition(texture->GetResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            source = texture->GetUavCpu();
            writtenResources[writtenCount++] = texture->GetResource();
        }
        if (slot >= 2 && slot <= 3) {
            if (auto* bufferBase = resources.Get(call.uavBuffers[slot - 2])) {
                auto* buffer = static_cast<DX12StructuredBuffer*>(bufferBase);
                m_stateTracker.QueueTransition(buffer->GetResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                source = buffer->GetUav();
                writtenResources[writtenCount++] = buffer->GetResource();
            }
        }
        m_context.GetDevice()->CopyDescriptorsSimple(
            1, uavDestination, source, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        uavDestination.ptr += m_context.GetSrvDescriptorIncrement();
    }
    commands->SetComputeRootDescriptorTable(15, uavTable.gpu);
    // WHAT: SRV/UAV ループで溜めた遷移をここで 1 回の ResourceBarrier にまとめて発行する (Dispatch より前)。
    m_stateTracker.FlushBarriers(commands);
    commands->Dispatch(call.dispatchX, call.dispatchY, call.dispatchZ);
    if (writtenCount > 0) {
        // WHAT: このDispatchが書き込んだUAVの読み出し前ハザードを防ぐバリアも1回にまとめて発行する。
        std::array<D3D12_RESOURCE_BARRIER, 10> uavBarriers{};
        for (uint32_t index = 0; index < writtenCount; ++index) {
            uavBarriers[index].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            uavBarriers[index].UAV.pResource = writtenResources[index];
        }
        commands->ResourceBarrier(writtenCount, uavBarriers.data());
    }
}

void DX12Renderer::Resize(uint32_t width, uint32_t height)
{
    m_context.Resize(width, height);
}

void DX12Renderer::SetRenderTarget(ResourceHandle<RenderTargetTag> handle, ResourceManager& resources)
{
    if (!m_context.IsFrameOpen()) return;
    ID3D12GraphicsCommandList* commands = m_context.GetCommandList();
    if (m_currentRenderTarget) {
        for (uint32_t index = 0; index < m_currentRenderTarget->GetColorCount(); ++index)
            m_stateTracker.QueueTransition(m_currentRenderTarget->GetColorResource(index),
                                           D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        m_stateTracker.QueueTransition(m_currentRenderTarget->GetDepthResource(),
                                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    auto* targetBase = resources.Get(handle);
    m_currentRenderTarget = targetBase ? static_cast<DX12RenderTarget*>(targetBase) : nullptr;
    m_currentCubeRtv = {};
    if (!m_currentRenderTarget) {
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
        return;
    }
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, DX12RenderTarget::MAX_COLOR> rtvs{};
    for (uint32_t index = 0; index < m_currentRenderTarget->GetColorCount(); ++index) {
        m_stateTracker.QueueTransition(m_currentRenderTarget->GetColorResource(index),
                                       D3D12_RESOURCE_STATE_RENDER_TARGET);
        rtvs[index] = m_currentRenderTarget->GetRtv(index);
    }
    m_stateTracker.QueueTransition(m_currentRenderTarget->GetDepthResource(),
                                   D3D12_RESOURCE_STATE_DEPTH_WRITE);
    // WHAT: 旧RTの解放遷移と新RTのバインド遷移をまとめて1回のResourceBarrierで発行する。
    m_stateTracker.FlushBarriers(commands);
    const auto dsv = m_currentRenderTarget->GetDsv();
    commands->OMSetRenderTargets(m_currentRenderTarget->GetColorCount(), rtvs.data(), FALSE, &dsv);
    D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(m_currentRenderTarget->GetWidth()),
                            static_cast<float>(m_currentRenderTarget->GetHeight()), 0.0f, 1.0f};
    D3D12_RECT scissor{0, 0, static_cast<LONG>(m_currentRenderTarget->GetWidth()),
                       static_cast<LONG>(m_currentRenderTarget->GetHeight())};
    commands->RSSetViewports(1, &viewport);
    commands->RSSetScissorRects(1, &scissor);
}

void DX12Renderer::SetRenderTargetFace(
    ResourceHandle<RenderTargetTag> handle, uint32_t face, uint32_t mip, ResourceManager& resources)
{
    if (!m_context.IsFrameOpen()) return;
    auto* targetBase = resources.Get(handle);
    auto* target = targetBase ? static_cast<DX12RenderTarget*>(targetBase) : nullptr;
    if (!target || !target->IsCubemap() || face >= 6 || mip >= target->GetMipCount()) return;
    m_currentRenderTarget = target;
    m_currentCubeRtv = target->GetFaceRtv(face, mip);
    m_stateTracker.Transition(m_context.GetCommandList(), target->GetCubeResource(),
                              D3D12_RESOURCE_STATE_RENDER_TARGET);
    m_context.GetCommandList()->OMSetRenderTargets(1, &m_currentCubeRtv, FALSE, nullptr);
    const uint32_t mipSize = (std::max)(1u, target->GetWidth() >> mip);
    D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(mipSize),
                            static_cast<float>(mipSize), 0.0f, 1.0f};
    D3D12_RECT scissor{0, 0, static_cast<LONG>(mipSize), static_cast<LONG>(mipSize)};
    m_context.GetCommandList()->RSSetViewports(1, &viewport);
    m_context.GetCommandList()->RSSetScissorRects(1, &scissor);
}
void DX12Renderer::SetSampler(uint32_t, SamplerMode) {}

bool DX12Renderer::BakeSkyLight(
    ResourceHandle<RenderTargetTag> handle, ResourceManager& resources,
    uint32_t irradianceSize, uint32_t prefilterSize, uint32_t prefilterMips,
    uint32_t sampleCount, std::unique_ptr<ITexture>& outIrradiance,
    std::unique_ptr<ITexture>& outPrefilter)
{
    auto* targetBase = resources.Get(handle);
    auto* target = targetBase ? static_cast<DX12RenderTarget*>(targetBase) : nullptr;
    if (!target || !target->IsCubemap()) return false;
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
    // Editor が所有する一時ベイカー。m_context / m_psoCache はレンダラー寿命内で有効。
    return std::make_unique<DX12HdriBaker>(&m_context, &m_psoCache);
}

// RT のカラーを CPU 側 ScratchImage として掴む。PNG 化と数値評価で同じ読み戻しを共有する。
static bool CaptureDX12RenderTargetImage(DX12Context& context, IRenderTarget* base,
                                         DirectX::ScratchImage& outImage)
{
    auto* target = static_cast<DX12RenderTarget*>(base);
    if (target == nullptr) return false;
    ID3D12Resource* resource = target->GetColorResource(0);
    if (resource == nullptr) return false;

    // Scene View RT は直前フレームで ImGui サンプリング用に PIXEL_SHADER_RESOURCE へ遷移済み。
    // CaptureTexture は自前の CommandQueue/フェンス同期で COPY_SOURCE へ遷移→読み戻し→元状態へ戻す。
    // 呼び出しはフレーム外 (OnUpdate) なのでレンダラーの CommandList とは競合しない。
    return SUCCEEDED(DirectX::CaptureTexture(context.GetCommandQueue(), resource, /*isCubeMap*/ false, outImage,
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
    uint32_t width, uint32_t height, uint32_t colorCount)
{
    auto target = std::make_unique<DX12RenderTarget>();
    if (!target->Init(&m_context, &m_stateTracker, width, height, colorCount))
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

std::unique_ptr<IStructuredBuffer> DX12Renderer::CreateNativeStructuredBuffer(
    const void* data, uint32_t count, uint32_t stride)
{
    auto buffer = std::make_unique<DX12StructuredBuffer>();
    if (!buffer->Init(&m_context, &m_stateTracker, data, count, stride, false)) return nullptr;
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
    if (FAILED(m_context.GetDevice()->CreateQueryHeap(&queryDesc, IID_PPV_ARGS(&m_gpuQueryHeap))))
        return false;
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
    if (FAILED(m_context.GetDevice()->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr, IID_PPV_ARGS(&m_gpuReadback)))) return false;
    void* mapped = nullptr;
    if (FAILED(m_gpuReadback->Map(0, nullptr, &mapped))) return false;
    m_gpuMappedTimestamps = static_cast<uint64_t*>(mapped);
    return SUCCEEDED(m_context.GetCommandQueue()->GetTimestampFrequency(&m_gpuTimestampFrequency))
        && m_gpuTimestampFrequency != 0;
}

void DX12Renderer::GpuProfBeginFrame()
{
    if (!m_gpuQueryHeap || !m_context.IsFrameOpen()) return;
    m_gpuProfilerFrame = m_context.GetFrameIndex();
    GpuQueryFrame& frame = m_gpuQueryFrames[m_gpuProfilerFrame];
    frame.count = 0;
    frame.recording = true;
    frame.pending = false;
}

void DX12Renderer::GpuProfBeginPass(const char* name)
{
    GpuQueryFrame& frame = m_gpuQueryFrames[m_gpuProfilerFrame];
    if (!frame.recording || frame.count >= GPU_MAX_PASSES) return;
    std::snprintf(frame.names[frame.count].data(), frame.names[frame.count].size(),
                  "%s", name ? name : "Unknown");
    const uint32_t query = (m_gpuProfilerFrame * GPU_MAX_PASSES + frame.count) * 2;
    m_context.GetCommandList()->EndQuery(m_gpuQueryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, query);
}

void DX12Renderer::GpuProfEndPass(const char*)
{
    GpuQueryFrame& frame = m_gpuQueryFrames[m_gpuProfilerFrame];
    if (!frame.recording || frame.count >= GPU_MAX_PASSES) return;
    const uint32_t query = (m_gpuProfilerFrame * GPU_MAX_PASSES + frame.count) * 2 + 1;
    m_context.GetCommandList()->EndQuery(m_gpuQueryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, query);
    ++frame.count;
}

void DX12Renderer::GpuProfEndFrame()
{
    GpuQueryFrame& frame = m_gpuQueryFrames[m_gpuProfilerFrame];
    if (!frame.recording || frame.count == 0) {
        frame.recording = false;
        return;
    }
    const uint32_t firstQuery = m_gpuProfilerFrame * GPU_MAX_PASSES * 2;
    const uint32_t queryCount = frame.count * 2;
    const UINT64 destinationOffset = static_cast<UINT64>(firstQuery) * sizeof(uint64_t);
    m_context.GetCommandList()->ResolveQueryData(
        m_gpuQueryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, firstQuery, queryCount,
        m_gpuReadback.Get(), destinationOffset);
    frame.recording = false;
    frame.pending = true;
}

void DX12Renderer::GpuProfCollect()
{
    if (!m_gpuMappedTimestamps || m_gpuTimestampFrequency == 0) return;
    m_gpuResults.clear();
    const uint64_t completedFence = m_context.GetCompletedFenceValue();
    for (uint32_t frameIndex = 0; frameIndex < DX12Context::FRAME_COUNT; ++frameIndex) {
        GpuQueryFrame& frame = m_gpuQueryFrames[frameIndex];
        if (!frame.pending || completedFence < m_context.GetFrameFenceValue(frameIndex)) continue;
        const uint32_t base = frameIndex * GPU_MAX_PASSES * 2;
        for (uint32_t pass = 0; pass < frame.count; ++pass) {
            const uint64_t begin = m_gpuMappedTimestamps[base + pass * 2];
            const uint64_t end = m_gpuMappedTimestamps[base + pass * 2 + 1];
            if (end < begin) continue;
            const double milliseconds = static_cast<double>(end - begin) * 1000.0
                                      / static_cast<double>(m_gpuTimestampFrequency);
            m_gpuResults.push_back({frame.names[pass].data(), milliseconds});
        }
        frame.pending = false;
    }
}

} // namespace fbzz::renderer
