/// @file    DX12HdriBaker.cpp
/// @brief   IIblBaker の DX12 実装 — HDRI equirect → 4 DDS + .ibl をフレーム非依存で同期ベイクする。
/// @author  Hasegawa Jin
/// @date    2026-07-15
///
/// 処理フロー (DX11IblBaker と同じ CS 資産 = SM5.0 DXBC を流用):
/// [Phase 1]  equirect float* → GPU 2D → EquirectToCubemap CS × 6 面 → Env Cubemap mip0
/// → CaptureTexture で読み戻し → DirectXTex GenerateMipMaps で mip 連鎖生成
/// → *_env.dds 保存 + GPU へ mip 付きで再アップロード
/// [Phase 2]  IrradianceConvolution CS × 6 → *_irr.dds
/// PrefilteredEnvMap CS × (面 × mip) → *_prefilter.dds
/// BRDFIntegration CS × 1 → *_brdf.dds
/// [仕上げ]   FzIblHeader を .ibl に書き出す
///
/// DX12 固有事情:
/// - GenerateMips が無いため env mip 連鎖は CPU (DirectXTex) で作り GPU へ戻す。
/// - 即時実行が無いため自前コマンドリスト + フェンスで各フェーズを完全同期する。
#pragma comment(lib, "ole32.lib") // DirectXTex が WIC を使う経路のための保険

#include "DX12HdriBaker.hpp"

#include <Engine/Renderer/BindlessIndices.hpp>

#include "DX12Context.hpp"
#include "DX12PsoCache.hpp"
#include <Engine/Format/FzAssetFormat.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Util/StringUtils.hpp>

#include <DirectXTex.h> // d3d12.h (DX12HdriBaker.hpp 経由) の後に include し DX12 版 API を有効化
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace fbzz::renderer {

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

namespace {

// 定数バッファ構造体 (HLSL 側 cbuffer と 1:1 でレイアウトを合わせる)
struct alignas(16) CbIblFace {
    uint32_t faceIndex;
    uint32_t textureSize;
    uint32_t phiSteps;   // IrradianceConvolution の半球積分分割数 (Equirect は 0)
    uint32_t thetaSteps;
};
struct alignas(16) CbPrefilter {
    uint32_t faceIndex;
    uint32_t outputSize;
    float    roughness;
    uint32_t sampleCount;
    uint32_t envMipCount;
    uint32_t pad0, pad1, pad2;
};

// Compute Root Signature の固定スロット割り当て (DX12PsoCache と一致)
constexpr uint32_t ROOT_CB0        = 0;   // root CBV b0
// SRV / UAV テーブルは bindless 移行で撤去済み。添字は b14 (kBindlessIndicesRootParam) で配る。


constexpr DXGI_FORMAT CUBE_FORMAT     = DXGI_FORMAT_R16G16B16A16_FLOAT;
constexpr DXGI_FORMAT EQUIRECT_FORMAT = DXGI_FORMAT_R32G32B32A32_FLOAT;

// Editor DDS ベイクは高品質な半球積分 (DX11 と同じ 200×50=10000 サンプル) を使う。
constexpr uint32_t IRRADIANCE_PHI   = 200;
constexpr uint32_t IRRADIANCE_THETA = 50;

// shader-visible ヒープと CB リングの容量。1 フェーズ内の全 Dispatch 分を賄えれば十分
// (BeginRecording でリセットするため、最大フェーズ = irradiance6 + prefilter(6×最大8) + brdf1)。
constexpr uint32_t DESCRIPTOR_CAPACITY = 512;
constexpr size_t   CONSTANT_CAPACITY   = 64u * 1024u;

D3D12_HEAP_PROPERTIES HeapProps(D3D12_HEAP_TYPE type)
{
    D3D12_HEAP_PROPERTIES props{};
    props.Type = type;
    return props;
}

} // namespace

DX12HdriBaker::DX12HdriBaker(DX12Context* context, DX12PsoCache* psoCache)
    : m_context(context), m_psoCache(psoCache),
      m_device(context ? context->GetDevice() : nullptr)
{
}

DX12HdriBaker::~DX12HdriBaker()
{
    if (m_constantUpload && m_constantMapped) m_constantUpload->Unmap(0, nullptr);
    if (m_fenceEvent) CloseHandle(m_fenceEvent);
}

// メインエントリー

bool DX12HdriBaker::Bake(
    const IblBakeInput& input, const std::string& outputDir,
    const std::string& baseName, IblBakeOutput& output)
{
    if (!input.pixels || input.equirectW == 0 || input.equirectH == 0) {
        FBZZ_LOG_ERROR("DX12HdriBaker: ピクセルデータが空です");
        return false;
    }
    if (input.compiledShadersDir.empty()) {
        FBZZ_LOG_ERROR("DX12HdriBaker: compiledShadersDir が未設定です");
        return false;
    }
    if (!m_device || !m_context->GetCommandQueue()) {
        FBZZ_LOG_ERROR("DX12HdriBaker: DX12 デバイス/キューが無効です");
        return false;
    }
    if (!EnsureCommon() || !EnsurePipelines(input.compiledShadersDir))
        return false;

    std::error_code ec;
    fs::create_directories(util::StringUtils::ToWide(outputDir), ec);

    // ---- Phase 1: Equirect → Env Cubemap mip0 ----
    Resource equirectTex;
    Resource equirectUpload; // GPU 完了まで生存させる upload バッファ
    Resource envMip0 = CreateCubemap(input.envCubemapSize, 1);
    if (!envMip0) return false;

    BeginRecording();
    if (!UploadEquirect(input.pixels, input.equirectW, input.equirectH, equirectTex, equirectUpload))
        return false;
    {
        const uint32_t equirectSrv = CreateEquirectSrv(equirectTex.Get());
        for (uint32_t face = 0; face < 6; ++face) {
            const CbIblFace cb{face, input.envCubemapSize, 0, 0};
            const auto cbVA = PushConstants(&cb, sizeof(cb));
            const auto uav  = CreateFaceUav(envMip0.Get(), face, 0);
            if (!cbVA || equirectSrv == INVALID_BINDLESS_INDEX || uav == INVALID_BINDLESS_INDEX) return false;
            Dispatch(m_psoEquirect.Get(), cbVA, equirectSrv, uav, input.envCubemapSize);
        }
    }
    if (!ExecuteAndWait()) return false;

    // env mip0 を読み戻し、CPU で mip 連鎖を生成する (DX12 は GenerateMips が無いため)。
    DirectX::ScratchImage envMip0Image;
    if (FAILED(DirectX::CaptureTexture(
            m_context->GetCommandQueue(), envMip0.Get(), /*isCubeMap*/ true, envMip0Image,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS))) {
        FBZZ_LOG_ERROR("DX12HdriBaker: Env Cubemap の CaptureTexture に失敗しました");
        return false;
    }
    // 大きい HDRI では equirect / mip0 で数百 MB になり得るため、Phase 2 の割当前に解放する
    // (CaptureTexture は同期完了済みなので GPU が触り終えている)。
    equirectTex.Reset();
    equirectUpload.Reset();
    envMip0.Reset();

    DirectX::ScratchImage envChain;
    // FANT(=box) の非 WIC パスを強制する。WIC/COM 初期化状態に依存せず、FP16 cube を確実に処理する。
    const auto mipFilter = static_cast<DirectX::TEX_FILTER_FLAGS>(
        DirectX::TEX_FILTER_FANT | DirectX::TEX_FILTER_FORCE_NON_WIC);
    if (FAILED(DirectX::GenerateMipMaps(
            envMip0Image.GetImages(), envMip0Image.GetImageCount(), envMip0Image.GetMetadata(),
            mipFilter, /*levels: 0 = full*/ 0, envChain))) {
        FBZZ_LOG_ERROR("DX12HdriBaker: Env Cubemap の GenerateMipMaps に失敗しました");
        return false;
    }
    const uint32_t envMipCount = static_cast<uint32_t>(envChain.GetMetadata().mipLevels);

    auto makePath = [&](const char* suffix) {
        return outputDir + "/" + baseName + suffix + ".dds";
    };
    const std::string envPath       = makePath("_env");
    const std::string irrPath       = makePath("_irr");
    const std::string prefilterPath = makePath("_prefilter");
    const std::string brdfPath      = makePath("_brdf");

    // *_env.dds は mip 連鎖付きで保存 (実行時の Skybox/IBL 消費側が LOD を得られるようにする)。
    if (FAILED(DirectX::SaveToDDSFile(
            envChain.GetImages(), envChain.GetImageCount(), envChain.GetMetadata(),
            DirectX::DDS_FLAGS_NONE, util::StringUtils::ToWide(envPath).c_str()))) {
        FBZZ_LOG_ERROR("DX12HdriBaker: *_env.dds の保存に失敗しました [%s]", envPath.c_str());
        return false;
    }

    // 畳み込み用に mip 連鎖付き env cube を GPU へ再アップロードする。
    Resource envCube = CreateCubemap(input.envCubemapSize, envMipCount);
    if (!envCube) return false;
    std::vector<D3D12_SUBRESOURCE_DATA> envSubresources;
    if (FAILED(DirectX::PrepareUpload(
            m_device, envChain.GetImages(), envChain.GetImageCount(),
            envChain.GetMetadata(), envSubresources))) {
        FBZZ_LOG_ERROR("DX12HdriBaker: Env Cubemap の PrepareUpload に失敗しました");
        return false;
    }

    // ---- Phase 2: Irradiance / Prefilter / BRDF LUT ----
    Resource irradianceCube = CreateCubemap(input.irradianceSize, 1);
    Resource prefilterCube  = CreateCubemap(input.prefilteredSize, input.prefilteredMipCount);
    Resource brdfLut        = CreateLut(input.brdfLutSize);
    if (!irradianceCube || !prefilterCube || !brdfLut) return false;

    Resource envUpload; // env の subresource upload バッファ (GPU 完了まで生存)
    BeginRecording();
    // env cube は CreateCubemap が UAV 状態で作るので、アップロードのため COPY_DEST へ戻す。
    Transition(envCube.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!RecordUpload(envCube.Get(), envSubresources.data(),
                      static_cast<uint32_t>(envSubresources.size()), envUpload))
        return false;
    Transition(envCube.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    const uint32_t envSrv = CreateCubeSrv(envCube.Get(), envMipCount);
    if (envSrv == INVALID_BINDLESS_INDEX) return false;

    // Irradiance (6 面)
    for (uint32_t face = 0; face < 6; ++face) {
        const CbIblFace cb{face, input.irradianceSize, IRRADIANCE_PHI, IRRADIANCE_THETA};
        const auto cbVA = PushConstants(&cb, sizeof(cb));
        const auto uav  = CreateFaceUav(irradianceCube.Get(), face, 0);
        if (!cbVA || uav == INVALID_BINDLESS_INDEX) return false;
        Dispatch(m_psoIrradiance.Get(), cbVA, envSrv, uav, input.irradianceSize);
    }
    // Prefilter (面 × mip)。roughness は mip を [0,1] に均等割りする。
    for (uint32_t mip = 0; mip < input.prefilteredMipCount; ++mip) {
        const uint32_t mipSize = (std::max)(1u, input.prefilteredSize >> mip);
        const float roughness = input.prefilteredMipCount > 1
            ? static_cast<float>(mip) / static_cast<float>(input.prefilteredMipCount - 1) : 0.0f;
        for (uint32_t face = 0; face < 6; ++face) {
            const CbPrefilter cb{face, mipSize, roughness, input.sampleCount, envMipCount, 0, 0, 0};
            const auto cbVA = PushConstants(&cb, sizeof(cb));
            const auto uav  = CreateFaceUav(prefilterCube.Get(), face, mip);
            if (!cbVA || uav == INVALID_BINDLESS_INDEX) return false;
            Dispatch(m_psoPrefilter.Get(), cbVA, envSrv, uav, mipSize);
        }
    }
    // BRDF LUT (1 回。CB 無し / SRV 無し。シェーダーは GetDimensions で寸法を得る)
    {
        const auto uav = CreateLutUav(brdfLut.Get());
        if (uav == INVALID_BINDLESS_INDEX) return false;
        Dispatch(m_psoBrdf.Get(), 0, INVALID_BINDLESS_INDEX, uav, input.brdfLutSize);
    }
    if (!ExecuteAndWait()) return false;

    // 読み戻して DDS 保存 (irradiance / prefilter は cube、brdf は 2D)。
    if (!SaveDds(irradianceCube.Get(), true, irrPath)) return false;
    if (!SaveDds(prefilterCube.Get(), true, prefilterPath)) return false;
    if (!SaveDds(brdfLut.Get(), false, brdfPath)) return false;

    // ---- .ibl 記述子 ----
    // DDS パスは .ibl と同一ディレクトリからの相対パス (ファイル名のみ) で格納する。
    {
        asset::FzIblHeader header{};
        std::memcpy(header.magic, "FZIBL\0", 6);
        header.version             = static_cast<uint16_t>(asset::FZIBL_VERSION);
        header.prefilteredMipCount = input.prefilteredMipCount;
        header._pad                = 0;
        const std::string envRel       = baseName + "_env.dds";
        const std::string irrRel       = baseName + "_irr.dds";
        const std::string prefilterRel = baseName + "_prefilter.dds";
        const std::string brdfRel      = baseName + "_brdf.dds";
        std::strncpy(header.envCubemapPath,  envRel.c_str(),       255);
        std::strncpy(header.irradiancePath,  irrRel.c_str(),       255);
        std::strncpy(header.prefilteredPath, prefilterRel.c_str(), 255);
        std::strncpy(header.brdfLutPath,     brdfRel.c_str(),      255);

        const std::string iblPath = outputDir + "/" + baseName + ".ibl";
        std::ofstream ofs(util::StringUtils::ToWide(iblPath), std::ios::binary);
        if (!ofs.is_open()) {
            FBZZ_LOG_ERROR("DX12HdriBaker: .ibl ファイル書き込み失敗 [%s]", iblPath.c_str());
            return false;
        }
        ofs.write(reinterpret_cast<const char*>(&header), sizeof(header));
    }

    output.envCubemapPath      = envPath;
    output.irradiancePath      = irrPath;
    output.prefilteredPath     = prefilterPath;
    output.brdfLutPath         = brdfPath;
    output.prefilteredMipCount = input.prefilteredMipCount;
    return true;
}

// 初期化

bool DX12HdriBaker::EnsureCommon()
{
    if (m_commandList) return true; // 構築済み

    if (FAILED(m_device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_allocator)))) {
        FBZZ_LOG_ERROR("DX12HdriBaker: コマンドアロケーター生成失敗");
        return false;
    }
    if (FAILED(m_device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_allocator.Get(), nullptr,
            IID_PPV_ARGS(&m_commandList)))) {
        FBZZ_LOG_ERROR("DX12HdriBaker: コマンドリスト生成失敗");
        return false;
    }
    m_commandList->Close(); // 記録は BeginRecording で開始する

    if (FAILED(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)))) {
        FBZZ_LOG_ERROR("DX12HdriBaker: フェンス生成失敗");
        return false;
    }
    m_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!m_fenceEvent) {
        FBZZ_LOG_ERROR("DX12HdriBaker: フェンスイベント生成失敗");
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = DESCRIPTOR_CAPACITY;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(m_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_descriptorHeap)))) {
        FBZZ_LOG_ERROR("DX12HdriBaker: ディスクリプタヒープ生成失敗");
        return false;
    }
    m_descriptorIncrement = m_device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_RESOURCE_DESC cbDesc{};
    cbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    cbDesc.Width = CONSTANT_CAPACITY;
    cbDesc.Height = 1;
    cbDesc.DepthOrArraySize = 1;
    cbDesc.MipLevels = 1;
    cbDesc.SampleDesc.Count = 1;
    cbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    const auto uploadHeap = HeapProps(D3D12_HEAP_TYPE_UPLOAD);
    if (FAILED(m_device->CreateCommittedResource(
            &uploadHeap, D3D12_HEAP_FLAG_NONE, &cbDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr, IID_PPV_ARGS(&m_constantUpload)))) {
        FBZZ_LOG_ERROR("DX12HdriBaker: 定数バッファアップロード生成失敗");
        return false;
    }
    const D3D12_RANGE noRead{0, 0};
    if (FAILED(m_constantUpload->Map(0, &noRead, reinterpret_cast<void**>(&m_constantMapped)))) {
        FBZZ_LOG_ERROR("DX12HdriBaker: 定数バッファ Map 失敗");
        return false;
    }
    return true;
}

bool DX12HdriBaker::EnsurePipelines(const std::string& compiledShadersDir)
{
    if (m_psoEquirect && m_loadedCompiledDir == compiledShadersDir)
        return true; // 同じディレクトリで構築済み

    m_psoEquirect   = LoadComputePso(compiledShadersDir + "IBL.EquirectToCubemap.cs.cso");
    m_psoIrradiance = LoadComputePso(compiledShadersDir + "IBL.IrradianceConvolution.cs.cso");
    m_psoPrefilter  = LoadComputePso(compiledShadersDir + "IBL.PrefilteredEnvMap.cs.cso");
    m_psoBrdf       = LoadComputePso(compiledShadersDir + "PostProcess.AmbientOcclusion.BRDFIntegration.cs.cso");
    if (!m_psoEquirect || !m_psoIrradiance || !m_psoPrefilter || !m_psoBrdf) {
        m_loadedCompiledDir.clear();
        return false;
    }
    m_loadedCompiledDir = compiledShadersDir;
    return true;
}

ComPtr<ID3D12PipelineState> DX12HdriBaker::LoadComputePso(const std::string& csoPath)
{
    const auto blob = LoadBinary(csoPath);
    if (blob.empty()) {
        FBZZ_LOG_ERROR("DX12HdriBaker: CSO を読み込めません [%s]", csoPath.c_str());
        return nullptr;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = m_psoCache->GetComputeRootSignature();
    desc.CS = {blob.data(), blob.size()};
    ComPtr<ID3D12PipelineState> pso;
    if (FAILED(m_device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso)))) {
        FBZZ_LOG_ERROR("DX12HdriBaker: Compute PSO 生成失敗 [%s]", csoPath.c_str());
        return nullptr;
    }
    return pso;
}

// フェーズ制御

void DX12HdriBaker::BeginRecording()
{
    m_allocator->Reset();
    m_commandList->Reset(m_allocator.Get(), nullptr);
    ID3D12DescriptorHeap* heaps[] = {m_descriptorHeap.Get()};
    m_commandList->SetDescriptorHeaps(1, heaps);
    m_commandList->SetComputeRootSignature(m_psoCache->GetComputeRootSignature());
    m_descriptorOffset = 0;
    m_constantOffset = 0;
}

bool DX12HdriBaker::ExecuteAndWait()
{
    if (FAILED(m_commandList->Close())) {
        FBZZ_LOG_ERROR("DX12HdriBaker: コマンドリスト Close 失敗");
        return false;
    }
    ID3D12CommandList* lists[] = {m_commandList.Get()};
    ID3D12CommandQueue* queue = m_context->GetCommandQueue();
    queue->ExecuteCommandLists(1, lists);
    const uint64_t value = ++m_fenceValue;
    if (FAILED(queue->Signal(m_fence.Get(), value))) {
        FBZZ_LOG_ERROR("DX12HdriBaker: フェンス Signal 失敗");
        return false;
    }
    if (m_fence->GetCompletedValue() < value) {
        m_fence->SetEventOnCompletion(value, m_fenceEvent);
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }
    return true;
}

// ヘルパ

DX12HdriBaker::Descriptor DX12HdriBaker::AllocateDescriptor()
{
    Descriptor result;
    if (m_descriptorOffset >= DESCRIPTOR_CAPACITY) {
        FBZZ_LOG_ERROR("DX12HdriBaker: ディスクリプタヒープ枯渇");
        return result; // ptr == 0 で失敗を伝える
    }
    const SIZE_T shift = static_cast<SIZE_T>(m_descriptorOffset) * m_descriptorIncrement;
    result.cpu = m_descriptorHeap->GetCPUDescriptorHandleForHeapStart();
    result.cpu.ptr += shift;
    result.gpu = m_descriptorHeap->GetGPUDescriptorHandleForHeapStart();
    result.gpu.ptr += shift;
    result.index = m_descriptorOffset;
    ++m_descriptorOffset;
    return result;
}

D3D12_GPU_VIRTUAL_ADDRESS DX12HdriBaker::PushConstants(const void* data, size_t size)
{
    // root CBV は 256B アラインが必須。
    constexpr size_t kAlign = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
    const size_t offset = (m_constantOffset + kAlign - 1) & ~(kAlign - 1);
    if (offset + size > CONSTANT_CAPACITY) {
        FBZZ_LOG_ERROR("DX12HdriBaker: 定数バッファリング枯渇");
        return 0;
    }
    std::memcpy(m_constantMapped + offset, data, size);
    m_constantOffset = offset + size;
    return m_constantUpload->GetGPUVirtualAddress() + offset;
}

DX12HdriBaker::Resource DX12HdriBaker::CreateCubemap(uint32_t size, uint32_t mipCount)
{
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = size;
    desc.Height = size;
    desc.DepthOrArraySize = 6;
    desc.MipLevels = static_cast<UINT16>(mipCount);
    desc.Format = CUBE_FORMAT;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    const auto heap = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
    Resource resource;
    if (FAILED(m_device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            nullptr, IID_PPV_ARGS(&resource)))) {
        FBZZ_LOG_ERROR("DX12HdriBaker: Cubemap 生成失敗 (size=%u mip=%u)", size, mipCount);
        return nullptr;
    }
    return resource;
}

DX12HdriBaker::Resource DX12HdriBaker::CreateLut(uint32_t size)
{
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = size;
    desc.Height = size;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = CUBE_FORMAT; // 実行時 ComputeTexture と同じ RGBA16F。利用側は RG のみ読む
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    const auto heap = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
    Resource resource;
    if (FAILED(m_device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            nullptr, IID_PPV_ARGS(&resource)))) {
        FBZZ_LOG_ERROR("DX12HdriBaker: BRDF LUT 生成失敗 (size=%u)", size);
        return nullptr;
    }
    return resource;
}

uint32_t DX12HdriBaker::CreateEquirectSrv(ID3D12Resource* equirect)
{
    const Descriptor slot = AllocateDescriptor();
    if (!slot.cpu.ptr) return INVALID_BINDLESS_INDEX;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = EQUIRECT_FORMAT;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    m_device->CreateShaderResourceView(equirect, &srv, slot.cpu);
    return slot.index;
}

uint32_t DX12HdriBaker::CreateCubeSrv(ID3D12Resource* cube, uint32_t mipCount)
{
    const Descriptor slot = AllocateDescriptor();
    if (!slot.cpu.ptr) return INVALID_BINDLESS_INDEX;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = CUBE_FORMAT;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.TextureCube.MostDetailedMip = 0;
    srv.TextureCube.MipLevels = mipCount;
    m_device->CreateShaderResourceView(cube, &srv, slot.cpu);
    return slot.index;
}

uint32_t DX12HdriBaker::CreateFaceUav(
    ID3D12Resource* cube, uint32_t face, uint32_t mip)
{
    const Descriptor slot = AllocateDescriptor();
    if (!slot.cpu.ptr) return INVALID_BINDLESS_INDEX;
    // Cubemap の各面 = Texture2DArray の 1 スライス。シェーダーは g_output[uint3(x,y,0)] を書くため
    // ArraySize=1 のビューにすることでスライス 0 = 対象面へ写る。
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = CUBE_FORMAT;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
    uav.Texture2DArray.MipSlice = mip;
    uav.Texture2DArray.FirstArraySlice = face;
    uav.Texture2DArray.ArraySize = 1;
    m_device->CreateUnorderedAccessView(cube, nullptr, &uav, slot.cpu);
    return slot.index;
}

uint32_t DX12HdriBaker::CreateLutUav(ID3D12Resource* lut)
{
    const Descriptor slot = AllocateDescriptor();
    if (!slot.cpu.ptr) return INVALID_BINDLESS_INDEX;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = CUBE_FORMAT;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    m_device->CreateUnorderedAccessView(lut, nullptr, &uav, slot.cpu);
    return slot.index;
}

bool DX12HdriBaker::RecordUpload(
    ID3D12Resource* dest, const D3D12_SUBRESOURCE_DATA* subs,
    uint32_t count, Resource& uploadKeepAlive)
{
    const D3D12_RESOURCE_DESC destDesc = dest->GetDesc();
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts(count);
    std::vector<UINT> numRows(count);
    std::vector<UINT64> rowSizes(count);
    UINT64 totalBytes = 0;
    m_device->GetCopyableFootprints(&destDesc, 0, count, 0,
        layouts.data(), numRows.data(), rowSizes.data(), &totalBytes);

    D3D12_RESOURCE_DESC bufDesc{};
    bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufDesc.Width = totalBytes;
    bufDesc.Height = 1;
    bufDesc.DepthOrArraySize = 1;
    bufDesc.MipLevels = 1;
    bufDesc.SampleDesc.Count = 1;
    bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    const auto uploadHeap = HeapProps(D3D12_HEAP_TYPE_UPLOAD);
    if (FAILED(m_device->CreateCommittedResource(
            &uploadHeap, D3D12_HEAP_FLAG_NONE, &bufDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr, IID_PPV_ARGS(&uploadKeepAlive)))) {
        FBZZ_LOG_ERROR("DX12HdriBaker: アップロードバッファ生成失敗 (%llu bytes)",
                       static_cast<unsigned long long>(totalBytes));
        return false;
    }
    uint8_t* mapped = nullptr;
    const D3D12_RANGE noRead{0, 0};
    if (FAILED(uploadKeepAlive->Map(0, &noRead, reinterpret_cast<void**>(&mapped)))) {
        FBZZ_LOG_ERROR("DX12HdriBaker: アップロードバッファ Map 失敗");
        return false;
    }
    // GPU の要求する行ピッチ (256B アライン) に合わせて subresource を行単位でコピーする。
    for (uint32_t i = 0; i < count; ++i) {
        const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& layout = layouts[i];
        const auto* src = static_cast<const uint8_t*>(subs[i].pData);
        for (uint32_t row = 0; row < numRows[i]; ++row) {
            std::memcpy(
                mapped + layout.Offset + static_cast<SIZE_T>(layout.Footprint.RowPitch) * row,
                src + subs[i].RowPitch * row,
                static_cast<size_t>(rowSizes[i]));
        }
    }
    uploadKeepAlive->Unmap(0, nullptr);

    for (uint32_t i = 0; i < count; ++i) {
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = dest;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = i;
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = uploadKeepAlive.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = layouts[i];
        m_commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    return true;
}

bool DX12HdriBaker::UploadEquirect(
    const float* pixels, uint32_t width, uint32_t height,
    Resource& out, Resource& uploadKeepAlive)
{
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = EQUIRECT_FORMAT;
    desc.SampleDesc.Count = 1;
    const auto heap = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
    if (FAILED(m_device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr, IID_PPV_ARGS(&out)))) {
        FBZZ_LOG_ERROR("DX12HdriBaker: Equirect テクスチャ生成失敗 (%ux%u)", width, height);
        return false;
    }
    D3D12_SUBRESOURCE_DATA data{};
    data.pData = pixels;
    data.RowPitch = static_cast<LONG_PTR>(width) * 4 * sizeof(float); // RGBA float
    data.SlicePitch = data.RowPitch * height;
    if (!RecordUpload(out.Get(), &data, 1, uploadKeepAlive))
        return false;
    Transition(out.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    return true;
}

void DX12HdriBaker::Transition(
    ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    m_commandList->ResourceBarrier(1, &barrier);
}

void DX12HdriBaker::Dispatch(
    ID3D12PipelineState* pso, D3D12_GPU_VIRTUAL_ADDRESS cb,
    uint32_t srvIndex, uint32_t uavIndex, uint32_t size)
{
    m_commandList->SetPipelineState(pso);
    if (cb) m_commandList->SetComputeRootConstantBufferView(ROOT_CB0, cb);
    // IBL の CS は SRV を t0 (pixel[0])、UAV を u0 (uav[0]) で宣言している。
    // 添字は «このベイカーが束縛している自前ヒープ» 内の位置。
    BindlessIndicesConstants indices;
    indices.Reset();
    indices.pixel[0] = srvIndex;
    indices.uav[0]   = uavIndex;
    const auto indexBlock = PushConstants(&indices, sizeof(indices));
    if (indexBlock)
        m_commandList->SetComputeRootConstantBufferView(kBindlessIndicesRootParam, indexBlock);
    const uint32_t groups = (size + 7u) / 8u;
    m_commandList->Dispatch(groups, groups, 1);
}

bool DX12HdriBaker::SaveDds(ID3D12Resource* resource, bool isCubeMap, const std::string& absPath)
{
    // 直前の Dispatch 出力先。ExecuteAndWait 済みでアイドルかつ UAV 状態のまま読み戻す。
    DirectX::ScratchImage image;
    if (FAILED(DirectX::CaptureTexture(
            m_context->GetCommandQueue(), resource, isCubeMap, image,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS))) {
        FBZZ_LOG_ERROR("DX12HdriBaker: CaptureTexture 失敗 [%s]", absPath.c_str());
        return false;
    }
    if (FAILED(DirectX::SaveToDDSFile(
            image.GetImages(), image.GetImageCount(), image.GetMetadata(),
            DirectX::DDS_FLAGS_NONE, util::StringUtils::ToWide(absPath).c_str()))) {
        FBZZ_LOG_ERROR("DX12HdriBaker: SaveToDDSFile 失敗 [%s]", absPath.c_str());
        return false;
    }
    return true;
}

std::vector<uint8_t> DX12HdriBaker::LoadBinary(const std::string& path)
{
    std::ifstream file(util::StringUtils::ToWide(path), std::ios::binary | std::ios::ate);
    if (!file.is_open()) return {};
    const auto size = static_cast<size_t>(file.tellg());
    file.seekg(0);
    std::vector<uint8_t> bytes(size);
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
    return bytes;
}

} // namespace fbzz::renderer
