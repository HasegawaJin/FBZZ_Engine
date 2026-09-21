/// @file    DX12IblBaker.cpp
/// @brief   TextureCube SRVから面×mip UAVへIBL Computeを記録する。
/// @author  Hasegawa Jin
/// @date    2026-07-15
#include "DX12IblBaker.hpp"

#include "DX12Context.hpp"
#include "DX12PsoCache.hpp"
#include "DX12RenderTarget.hpp"
#include "DX12Shader.hpp"
#include "DX12StateTracker.hpp"
#include "DX12Texture.hpp"
#include "DX12UploadArena.hpp"
#include <Graphics/Renderer/AssetPathService.hpp>
#include <Graphics/Renderer/BindlessIndices.hpp>
#include <Core/Logger.hpp>
#include <array>
#include <algorithm>
#include <cstring>
#include <vector>
#include <wrl/client.h>

namespace fbzz::renderer {

struct DX12IblBaker::CubeOutput {
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> descriptors;
    uint32_t size = 0;
    uint32_t mipCount = 0;
    uint32_t increment = 0;
};

namespace {
struct alignas(16) FaceConstants { uint32_t face; uint32_t size; uint32_t phiSteps; uint32_t thetaSteps; };
struct alignas(16) PrefilterConstants {
    uint32_t face; uint32_t size; float roughness; uint32_t sampleCount;
    uint32_t envMipCount; uint32_t padding[3];
};
}

DX12IblBaker::DX12IblBaker(DX12Context* context, DX12StateTracker* tracker,
                           DX12PsoCache* psoCache, DX12UploadArena* uploadArena)
    : m_context(context), m_tracker(tracker), m_psoCache(psoCache), m_uploadArena(uploadArena) {}

/// @note 失敗したら unique_ptr を手放す。持ったままにすると次回の !m_xxxShader が false になり、
/// @note 初期化されていないシェーダーで true を返してしまう。そうなると IBL が静かに焼かれなく
/// @note なり、環境光を失った暗い画のままプロセスを再起動するまで回復しない。
bool DX12IblBaker::EnsureShaders()
{
    const auto ensure = [this](std::unique_ptr<DX12Shader>& shader, const char* assetPath) {
        if (shader) return true;
        const std::string resolved = ResolveAssetPath(assetPath);
        auto created = std::make_unique<DX12Shader>();
        if (!created->Init(resolved)) {
            if (!m_shaderFailureReported) {
                FBZZ_LOG_ERROR("DX12IblBaker: 計算シェーダーの読み込みに失敗 [%s] (解決先: %s)",
                               assetPath, resolved.c_str());
                m_shaderFailureReported = true;
            }
            return false;
        }
        shader = std::move(created);
        return true;
    };

    if (!ensure(m_irradianceShader, "Assets/Shaders/IBL/IrradianceConvolution.cs.hlsl")
        || !ensure(m_prefilterShader, "Assets/Shaders/IBL/PrefilteredEnvMap.cs.hlsl")) {
        return false;
    }
    /// @note 揃ったら次の失敗をまた報告できるようにする。
    m_shaderFailureReported = false;
    return true;
}

bool DX12IblBaker::CreateOutput(uint32_t size, uint32_t mipCount, CubeOutput& output)
{
    output.size = size;
    output.mipCount = mipCount;
    ID3D12Device* device = m_context->GetDevice();
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = size;
    desc.Height = size;
    desc.DepthOrArraySize = 6;
    desc.MipLevels = static_cast<UINT16>(mipCount);
    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&output.resource)))) return false;
    D3D12_DESCRIPTOR_HEAP_DESC descriptors{};
    descriptors.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    descriptors.NumDescriptors = 6 * mipCount;
    if (FAILED(device->CreateDescriptorHeap(&descriptors, IID_PPV_ARGS(&output.descriptors)))) return false;
    output.increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    for (uint32_t mip = 0; mip < mipCount; ++mip) {
        for (uint32_t face = 0; face < 6; ++face) {
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
            uav.Format = desc.Format;
            uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
            uav.Texture2DArray.MipSlice = mip;
            uav.Texture2DArray.FirstArraySlice = face;
            uav.Texture2DArray.ArraySize = 1;
            auto handle = output.descriptors->GetCPUDescriptorHandleForHeapStart();
            handle.ptr += static_cast<SIZE_T>(mip * 6 + face) * output.increment;
            device->CreateUnorderedAccessView(output.resource.Get(), nullptr, &uav, handle);
        }
    }
    m_tracker->Register(output.resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    return true;
}

bool DX12IblBaker::DispatchIrradiance(DX12RenderTarget& environment, CubeOutput& output)
{
    ID3D12PipelineState* pso = m_psoCache->GetOrCreateCompute(*m_irradianceShader);
    if (!pso) return false;
    ID3D12GraphicsCommandList* commands = m_context->GetCommandList();
    /// @note 共有コマンドリストへ compute のルートシグネチャ・PSO を設定するため、
    /// @note       DX12Renderer::Submit 側の状態キャッシュを無効化させる。
    m_context->MarkPipelineStateDirty();
    commands->SetComputeRootSignature(m_psoCache->GetComputeRootSignature());
    commands->SetPipelineState(pso);
    ID3D12DescriptorHeap* heaps[] = {m_context->GetResourceSrvHeap()};
    commands->SetDescriptorHeaps(1, heaps);
    m_tracker->Transition(commands, environment.GetCubeResource(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    for (uint32_t face = 0; face < 6; ++face) {
        const FaceConstants constants{face, output.size, 64, 16};
        const auto cb = m_uploadArena->Allocate(sizeof(constants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
        if (!cb) return false;
        std::memcpy(cb.cpu, &constants, sizeof(constants));
        commands->SetComputeRootConstantBufferView(0, cb.gpu);
        /// @note bindless: 面ごとの UAV は «リソース自身の添字» を持てない一時ビューなので、
        /// @note       その場で枠へ発行し、ベイク終了後にまとめて返す。
        BindlessIndicesConstants indices;
        indices.Reset();
        indices.pixel[0] = PublishTransient(environment.GetCubeSrv());
        auto faceUav = output.descriptors->GetCPUDescriptorHandleForHeapStart();
        faceUav.ptr += static_cast<SIZE_T>(face) * output.increment;
        indices.uav[0] = PublishTransient(faceUav);
        const auto indexBlock = m_uploadArena->Allocate(
            sizeof(indices), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
        if (!indexBlock) return false;
        std::memcpy(indexBlock.cpu, &indices, sizeof(indices));
        commands->SetComputeRootConstantBufferView(kBindlessIndicesRootParam, indexBlock.gpu);
        commands->Dispatch((output.size + 7) / 8, (output.size + 7) / 8, 1);
    }
    return true;
}

bool DX12IblBaker::DispatchPrefilter(
    DX12RenderTarget& environment, CubeOutput& output, uint32_t sampleCount)
{
    ID3D12PipelineState* pso = m_psoCache->GetOrCreateCompute(*m_prefilterShader);
    if (!pso) return false;
    ID3D12GraphicsCommandList* commands = m_context->GetCommandList();
    /// @note 共有コマンドリストへ compute のルートシグネチャ・PSO を設定するため、
    /// @note       DX12Renderer::Submit 側の状態キャッシュを無効化させる。
    m_context->MarkPipelineStateDirty();
    commands->SetComputeRootSignature(m_psoCache->GetComputeRootSignature());
    commands->SetPipelineState(pso);
    for (uint32_t mip = 0; mip < output.mipCount; ++mip) {
        const uint32_t mipSize = (std::max)(1u, output.size >> mip);
        const float roughness = output.mipCount > 1
            ? static_cast<float>(mip) / static_cast<float>(output.mipCount - 1) : 0.0f;
        for (uint32_t face = 0; face < 6; ++face) {
            const PrefilterConstants constants{face, mipSize, roughness, sampleCount, 1, {0, 0, 0}};
            const auto cb = m_uploadArena->Allocate(sizeof(constants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
            if (!cb) return false;
            std::memcpy(cb.cpu, &constants, sizeof(constants));
            commands->SetComputeRootConstantBufferView(0, cb.gpu);
            BindlessIndicesConstants indices;
            indices.Reset();
            indices.pixel[0] = PublishTransient(environment.GetCubeSrv());
            auto faceUav = output.descriptors->GetCPUDescriptorHandleForHeapStart();
            faceUav.ptr += static_cast<SIZE_T>(mip * 6 + face) * output.increment;
            indices.uav[0] = PublishTransient(faceUav);
            const auto indexBlock = m_uploadArena->Allocate(
                sizeof(indices), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
            if (!indexBlock) return false;
            std::memcpy(indexBlock.cpu, &indices, sizeof(indices));
            commands->SetComputeRootConstantBufferView(kBindlessIndicesRootParam, indexBlock.gpu);
            commands->Dispatch((mipSize + 7) / 8, (mipSize + 7) / 8, 1);
        }
    }
    return true;
}

uint32_t DX12IblBaker::PublishTransient(D3D12_CPU_DESCRIPTOR_HANDLE source)
{
    const uint32_t slot = m_context->PublishBindlessDescriptor(source);
    if (slot != DX12Context::INVALID_BINDLESS_INDEX)
        m_transientBindless.push_back(slot);
    return slot;
}

void DX12IblBaker::ReleaseTransients()
{
    /// @note FreeBindlessSlot はフェンス通過まで再利用させないので、記録済みの Dispatch が
    /// @note       まだこの添字を読んでいても安全。
    for (const uint32_t slot : m_transientBindless)
        m_context->FreeBindlessSlot(slot);
    m_transientBindless.clear();
}
bool DX12IblBaker::Convolve(
    DX12RenderTarget& environment, uint32_t irradianceSize, uint32_t prefilterSize,
    uint32_t prefilterMips, uint32_t sampleCount,
    std::unique_ptr<DX12Texture>& irradiance, std::unique_ptr<DX12Texture>& prefilter)
{
    /// @note 3 つの要因を 1 行にまとめると、IBL が焼かれない理由が画面からも Console からも
    /// @note       分からなくなる。環境光が丸ごと落ちる経路なので、どれで抜けたかは残す。
    if (!m_context->IsFrameOpen()) {
        FBZZ_LOG_WARN("DX12IblBaker: フレーム外から呼ばれたため IBL を焼けません");
        return false;
    }
    if (!environment.IsCubemap()) {
        FBZZ_LOG_WARN("DX12IblBaker: 環境レンダーターゲットがキューブマップではありません");
        return false;
    }
    if (!EnsureShaders()) return false;

    /// @note スコープガードにする理由: 以降の失敗経路が 3 つあり、どれかで返し忘れると
    /// @note       ベイクを繰り返すたびに bindless 枠が減り続ける (最後は枯渇して IBL が焼けなくなる)。
    struct TransientGuard {
        DX12IblBaker* baker;
        ~TransientGuard() { baker->ReleaseTransients(); }
    } transientGuard{this};

    CubeOutput irradianceOutput;
    CubeOutput prefilterOutput;
    if (!CreateOutput(irradianceSize, 1, irradianceOutput)
        || !CreateOutput(prefilterSize, prefilterMips, prefilterOutput)
        || !DispatchIrradiance(environment, irradianceOutput)
        || !DispatchPrefilter(environment, prefilterOutput, sampleCount)) {
        m_tracker->Remove(irradianceOutput.resource.Get());
        m_tracker->Remove(prefilterOutput.resource.Get());
        m_context->DeferRelease(irradianceOutput.resource);
        m_context->DeferRelease(prefilterOutput.resource);
        return false;
    }
    D3D12_RESOURCE_BARRIER barriers[2]{};
    for (uint32_t index = 0; index < 2; ++index) barriers[index].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barriers[0].UAV.pResource = irradianceOutput.resource.Get();
    barriers[1].UAV.pResource = prefilterOutput.resource.Get();
    m_context->GetCommandList()->ResourceBarrier(2, barriers);
    m_tracker->Transition(m_context->GetCommandList(), irradianceOutput.resource.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    m_tracker->Transition(m_context->GetCommandList(), prefilterOutput.resource.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    irradiance = std::make_unique<DX12Texture>();
    prefilter = std::make_unique<DX12Texture>();
    if (!irradiance->InitCubeFromResource(m_context, irradianceOutput.resource.Get(),
                                          DXGI_FORMAT_R16G16B16A16_FLOAT, irradianceSize, 1)
        || !prefilter->InitCubeFromResource(m_context, prefilterOutput.resource.Get(),
                                            DXGI_FORMAT_R16G16B16A16_FLOAT, prefilterSize, prefilterMips))
    {
        m_tracker->Remove(irradianceOutput.resource.Get());
        m_tracker->Remove(prefilterOutput.resource.Get());
        m_context->DeferRelease(irradianceOutput.resource);
        m_context->DeferRelease(prefilterOutput.resource);
        return false;
    }
    irradiance->RegisterState(m_tracker, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    prefilter->RegisterState(m_tracker, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return true;
}

} /// @note namespace fbzz::renderer
