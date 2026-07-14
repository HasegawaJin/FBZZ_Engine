// FBZZ Engine
// DX12Texture.cpp | fbzz::renderer
// DirectXTex 読み込み画像を RGBA8 へ正規化して同期アップロードする
#include "DX12Texture.hpp"

#include "DX12Context.hpp"
#include "DX12StateTracker.hpp"
#include <DirectXTex.h>
#include <Engine/Core/Logger.hpp>
#include <Engine/Util/StringUtils.hpp>
#include <cstring>

namespace fbzz::renderer {

DX12Texture::~DX12Texture()
{
    if (m_tracker) m_tracker->Remove(m_resource.Get());
    if (m_context) m_context->DeferRelease(m_resource);
}

bool DX12Texture::Init(DX12Context* context, const std::string& path)
{
    if (!context)
        return false;
    m_context = context;
    DirectX::ScratchImage source;
    const std::wstring widePath = util::StringUtils::ToWide(path);
    HRESULT result = E_FAIL;
    if (path.ends_with(".dds") || path.ends_with(".DDS"))
        result = DirectX::LoadFromDDSFile(widePath.c_str(), DirectX::DDS_FLAGS_NONE, nullptr, source);
    else if (path.ends_with(".tga") || path.ends_with(".TGA"))
        result = DirectX::LoadFromTGAFile(widePath.c_str(), nullptr, source);
    else
        result = DirectX::LoadFromWICFile(widePath.c_str(), DirectX::WIC_FLAGS_NONE, nullptr, source);
    if (FAILED(result) || source.GetImageCount() == 0) {
        FBZZ_LOG_ERROR("DX12Texture: 画像を読み込めません: %s", path.c_str());
        return false;
    }

    const DirectX::Image* image = source.GetImage(0, 0, 0);
    DirectX::ScratchImage converted;
    if (image->format != DXGI_FORMAT_R8G8B8A8_UNORM) {
        result = DirectX::IsCompressed(image->format)
            ? DirectX::Decompress(*image, DXGI_FORMAT_R8G8B8A8_UNORM, converted)
            : DirectX::Convert(*image, DXGI_FORMAT_R8G8B8A8_UNORM,
                               DirectX::TEX_FILTER_DEFAULT, 0.0f, converted);
        if (FAILED(result)) {
            FBZZ_LOG_ERROR("DX12Texture: RGBA8 変換に失敗しました: %s", path.c_str());
            return false;
        }
        image = converted.GetImage(0, 0, 0);
    }
    return InitFromData(context, image->pixels,
                        static_cast<uint32_t>(image->width), static_cast<uint32_t>(image->height));
}

bool DX12Texture::InitFromData(
    DX12Context* context, const uint8_t* rgba, uint32_t width, uint32_t height)
{
    if (!context || !context->UploadTexture2D(rgba, width, height, m_resource)) {
        FBZZ_LOG_ERROR("DX12Texture: RGBA8 テクスチャ転送に失敗しました (%ux%u)", width, height);
        return false;
    }
    m_context = context;
    m_width = width;
    m_height = height;
    return CreateSrv(context);
}

bool DX12Texture::InitFromData3D(
    DX12Context* context, const uint8_t* rgba, uint32_t width, uint32_t height, uint32_t depth)
{
    if (!context || !rgba || width == 0 || height == 0 || depth == 0)
        return false;
    m_context = context;
    ID3D12Device* device = context->GetDevice();
    if (!device || !context->GetCommandQueue())
        return false;

    // R8G8B8A8_UNORM の 3D テクスチャを DEFAULT ヒープに作り、COPY_DEST で開始する。
    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = static_cast<UINT16>(depth);
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    if (FAILED(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_resource)))) {
        FBZZ_LOG_ERROR("DX12Texture: 3D テクスチャ生成失敗 (%ux%ux%u)", width, height, depth);
        return false;
    }

    // アップロードバッファへ「深度スライス × 行」を GPU 要求ピッチ (256B) に合わせて詰める。
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rowCount = 0;
    UINT64 rowSize = 0;
    UINT64 uploadSize = 0;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &rowCount, &rowSize, &uploadSize);

    D3D12_HEAP_PROPERTIES uploadHeap{};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC uploadDesc{};
    uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    uploadDesc.Width = uploadSize;
    uploadDesc.Height = 1;
    uploadDesc.DepthOrArraySize = 1;
    uploadDesc.MipLevels = 1;
    uploadDesc.SampleDesc.Count = 1;
    uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    Microsoft::WRL::ComPtr<ID3D12Resource> upload;
    if (FAILED(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &uploadDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload))))
        return false;
    uint8_t* mapped = nullptr;
    if (FAILED(upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped))))
        return false;
    const size_t sourceRowPitch = static_cast<size_t>(width) * 4u; // RGBA8
    for (uint32_t z = 0; z < depth; ++z) {
        for (uint32_t y = 0; y < rowCount; ++y) {
            std::memcpy(
                mapped + footprint.Offset
                    + (static_cast<size_t>(z) * rowCount + y) * footprint.Footprint.RowPitch,
                rgba + (static_cast<size_t>(z) * height + y) * sourceRowPitch,
                sourceRowPitch);
        }
    }
    upload->Unmap(0, nullptr);

    // 専用コマンドリストで同期コピー → PIXEL_SHADER_RESOURCE へ遷移 (DX11 の即時 Immutable 相当)。
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)))
        || FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                            IID_PPV_ARGS(&list))))
        return false;
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = m_resource.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = upload.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = footprint;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = m_resource.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    list->ResourceBarrier(1, &barrier);
    if (FAILED(list->Close()))
        return false;
    ID3D12CommandList* lists[] = {list.Get()};
    context->GetCommandQueue()->ExecuteCommandLists(1, lists);
    context->Flush();

    m_width = width;
    m_height = height;

    // Texture3D SRV。シェーダーの Texture3D スロット (雲ノイズ / 3D LUT) と次元を一致させる。
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = 1;
    // WHY: CopyDescriptorsのコピー元はshader-visibleヒープにできないためCPU stagingに置く。
    if (FAILED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_srvHeap))))
        return false;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture3D.MipLevels = 1;
    device->CreateShaderResourceView(m_resource.Get(), &srv, GetSrvCpu());
    return true;
}

bool DX12Texture::InitFromResource(DX12Context* context, ID3D12Resource* resource,
                                   DXGI_FORMAT srvFormat, uint32_t width, uint32_t height)
{
    if (!context || !resource || width == 0 || height == 0)
        return false;
    m_context = context;
    m_resource = resource;
    m_width = width;
    m_height = height;
    return CreateSrv(context, srvFormat);
}

bool DX12Texture::InitCubeFromResource(DX12Context* context, ID3D12Resource* resource,
                                       DXGI_FORMAT srvFormat, uint32_t size, uint32_t mipCount)
{
    if (!context || !resource || size == 0 || mipCount == 0) return false;
    m_context = context;
    m_resource = resource;
    m_width = m_height = size;
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = 1;
    if (FAILED(context->GetDevice()->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_srvHeap)))) return false;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = srvFormat;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.TextureCube.MipLevels = mipCount;
    context->GetDevice()->CreateShaderResourceView(m_resource.Get(), &srv, GetSrvCpu());
    return true;
}

bool DX12Texture::CreateSrv(DX12Context* context, DXGI_FORMAT format)
{
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = 1;
    if (FAILED(context->GetDevice()->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_srvHeap))))
        return false;
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = format;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    context->GetDevice()->CreateShaderResourceView(m_resource.Get(), &srvDesc, GetSrvCpu());
    return true;
}

void DX12Texture::RegisterState(DX12StateTracker* tracker, D3D12_RESOURCE_STATES state)
{
    m_tracker = tracker;
    if (m_tracker) m_tracker->Register(m_resource.Get(), state);
}

bool DX12Texture::InitForCompute(
    DX12Context* context, DX12StateTracker* tracker, uint32_t width, uint32_t height)
{
    if (!context || !tracker || width == 0 || height == 0) return false;
    m_context = context;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    // WHY: Typed UAV Storeは機種依存のため、Debug Layerで拒否される形式を事前に避ける。
    m_format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    D3D12_FEATURE_DATA_FORMAT_SUPPORT formatSupport{m_format};
    if (FAILED(context->GetDevice()->CheckFeatureSupport(
            D3D12_FEATURE_FORMAT_SUPPORT, &formatSupport, sizeof(formatSupport)))
        || (formatSupport.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) == 0) {
        m_format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        D3D12_FEATURE_DATA_FORMAT_SUPPORT fallbackSupport{m_format};
        if (FAILED(context->GetDevice()->CheckFeatureSupport(
                D3D12_FEATURE_FORMAT_SUPPORT, &fallbackSupport, sizeof(fallbackSupport)))
            || (fallbackSupport.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) == 0) {
            FBZZ_LOG_ERROR("DX12Texture: Compute用Typed UAV Store対応フォーマットがありません");
            return false;
        }
    }
    desc.Format = m_format;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(context->GetDevice()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_resource)))) return false;
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = 2;
    if (FAILED(context->GetDevice()->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_srvHeap)))) return false;
    m_descriptorIncrement = context->GetDevice()->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = m_format;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    context->GetDevice()->CreateShaderResourceView(m_resource.Get(), &srv, GetSrvCpu());
    // 単一Mip・単一ArrayのTexture2Dなので、既定UAVを使う。
    // WHY: 手動ViewDimension/Format指定はGPUごとのTyped UAV制約やMip記述子不整合を
    //      Debug Layerでエラーにしやすい。リソース記述子から生成する既定ビューなら、
    //      実際に作成されたFormatと完全に一致する。
    // GetUavCpu() はUAVビューの存在フラグを検査するため、生成呼び出し前に有効化する。
    m_hasUav = true;
    context->GetDevice()->CreateUnorderedAccessView(
        m_resource.Get(), nullptr, nullptr, GetUavCpu());
    m_width = width;
    m_height = height;
    RegisterState(tracker, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    return true;
}

D3D12_CPU_DESCRIPTOR_HANDLE DX12Texture::GetSrvCpu() const
{
    return m_srvHeap ? m_srvHeap->GetCPUDescriptorHandleForHeapStart() : D3D12_CPU_DESCRIPTOR_HANDLE{};
}

D3D12_CPU_DESCRIPTOR_HANDLE DX12Texture::GetUavCpu() const
{
    if (!m_srvHeap || !m_hasUav) return {};
    auto handle = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += m_descriptorIncrement;
    return handle;
}

void DX12Texture::TransitionForPixelRead(ID3D12GraphicsCommandList* commands)
{
    if (m_tracker)
        m_tracker->Transition(commands, m_resource.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

} // namespace fbzz::renderer
