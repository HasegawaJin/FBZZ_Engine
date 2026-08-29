/// @file    DX12Texture.cpp
/// @brief   DirectXTex 読み込み画像を RGBA8 へ正規化して同期アップロードする。
/// @author  Hasegawa Jin
/// @date    2026-07-15
#include "DX12Texture.hpp"

#include "DX12Context.hpp"
#include "DX12StateTracker.hpp"
#include <DirectXTex.h>
#include <Engine/Core/Logger.hpp>
#include <Engine/Util/StringUtils.hpp>
#include <cstring>
#include <vector>

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

bool DX12Texture::InitForCompute3D(
    DX12Context* context, DX12StateTracker* tracker,
    uint32_t width, uint32_t height, uint32_t depth)
{
    if (!context || !tracker || width == 0 || height == 0 || depth == 0) return false;
    m_context = context;

    // Typed UAV Store の対応は機種依存。2D 版と同じ順で試し、どちらも駄目なら諦める。
    m_format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    const auto supportsTypedStore = [&](DXGI_FORMAT format) {
        D3D12_FEATURE_DATA_FORMAT_SUPPORT support{ format };
        return SUCCEEDED(context->GetDevice()->CheckFeatureSupport(
                   D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support)))
            && (support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) != 0;
    };
    if (!supportsTypedStore(m_format)) {
        m_format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        if (!supportsTypedStore(m_format)) {
            FBZZ_LOG_ERROR("DX12Texture: 3D Compute 用 Typed UAV Store 対応フォーマットがありません");
            return false;
        }
    }

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    desc.Width            = width;
    desc.Height           = height;
    desc.DepthOrArraySize = static_cast<UINT16>(depth);
    desc.MipLevels        = 1;
    desc.Format           = m_format;
    desc.SampleDesc.Count = 1;
    desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(context->GetDevice()->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_resource)))) {
        FBZZ_LOG_ERROR("DX12Texture: 3D Compute テクスチャ生成失敗 (%ux%ux%u)", width, height, depth);
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = 2;
    if (FAILED(context->GetDevice()->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_srvHeap))))
        return false;
    m_descriptorIncrement = context->GetDevice()->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format                  = m_format;
    srv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE3D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture3D.MipLevels     = 1;
    context->GetDevice()->CreateShaderResourceView(m_resource.Get(), &srv, GetSrvCpu());

    // UAV は 2D 版と同じ理由で既定ビュー (リソース記述から生成) を使う。
    // GetUavCpu() が存在フラグを見るので、生成呼び出しより前に立てる。
    m_hasUav = true;
    context->GetDevice()->CreateUnorderedAccessView(
        m_resource.Get(), nullptr, nullptr, GetUavCpu());

    m_width  = width;
    m_height = height;
    m_depth  = depth;
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

bool DX12Texture::InitDynamic(DX12Context* context, DX12StateTracker* tracker,
                              uint32_t width, uint32_t height, DynamicTextureFormat format)
{
    if (!context || width == 0 || height == 0) return false;
    ID3D12Device* device = context->GetDevice();
    if (!device) return false;

    m_context        = context;
    m_isDynamic      = true;
    m_bytesPerPixel  = (format == DynamicTextureFormat::R8) ? 1u : 4u;
    m_format         = (format == DynamicTextureFormat::R8)
        ? DXGI_FORMAT_R8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;

    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width            = width;
    desc.Height           = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels        = 1;   // 動的アトラスはミップを持たない
    desc.Format           = m_format;
    desc.SampleDesc.Count = 1;
    desc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    if (FAILED(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_resource)))) {
        FBZZ_LOG_ERROR("DX12Texture::InitDynamic: リソース生成失敗 (%ux%u)", width, height);
        return false;
    }

    m_width  = width;
    m_height = height;

    // 生成直後は COPY_DEST。ゼロクリアを 1 回流してから PIXEL_SHADER_RESOURCE へ移す。
    // WHY: DEFAULT ヒープの中身は未定義であり、まだ焼いていない領域のゴミが
    //      フォントアトラスでは「字の周りの謎の模様」として見えてしまう。
    RegisterState(tracker, D3D12_RESOURCE_STATE_COPY_DEST);
    const std::vector<uint8_t> zeros(
        static_cast<size_t>(width) * height * m_bytesPerPixel, 0u);
    if (!UploadRegionInternal(0, 0, width, height, zeros.data(),
                              width * m_bytesPerPixel,
                              D3D12_RESOURCE_STATE_COPY_DEST))
        return false;

    return CreateSrv(context, m_format);
}

bool DX12Texture::UpdateRegion(uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                               const void* pixels, uint32_t srcRowPitch)
{
    if (!m_isDynamic || !m_resource || !m_context || !pixels) return false;
    if (width == 0 || height == 0) return true;

    if (x + width > m_width || y + height > m_height) {
        FBZZ_LOG_ERROR("DX12Texture::UpdateRegion: region (%u,%u,%u,%u) exceeds texture %ux%u",
                       x, y, width, height, m_width, m_height);
        return false;
    }

    // 通常運用時のリソース状態は PIXEL_SHADER_RESOURCE。そこから COPY_DEST へ落として戻す。
    return UploadRegionInternal(x, y, width, height, pixels, srcRowPitch,
                                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

bool DX12Texture::UploadRegionInternal(uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                                       const void* pixels, uint32_t srcRowPitch,
                                       D3D12_RESOURCE_STATES currentState)
{
    ID3D12Device* device = m_context->GetDevice();
    ID3D12CommandQueue* queue = m_context->GetCommandQueue();
    if (!device || !queue) return false;

    // 更新矩形ぶんだけのフットプリントを作る。
    // WHY: アトラス全面ではなく矩形だけをアップロードバッファに詰めることで、
    //      2048x2048 のアトラスでもグリフ 1 個の追記が数 KB の転送で済む。
    D3D12_RESOURCE_DESC regionDesc{};
    regionDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    regionDesc.Width            = width;
    regionDesc.Height           = height;
    regionDesc.DepthOrArraySize = 1;
    regionDesc.MipLevels        = 1;
    regionDesc.Format           = m_format;
    regionDesc.SampleDesc.Count = 1;
    regionDesc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT   rowCount   = 0;
    UINT64 rowSize    = 0;
    UINT64 uploadSize = 0;
    device->GetCopyableFootprints(&regionDesc, 0, 1, 0, &footprint, &rowCount, &rowSize, &uploadSize);

    D3D12_HEAP_PROPERTIES uploadHeap{};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC uploadDesc{};
    uploadDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    uploadDesc.Width            = uploadSize;
    uploadDesc.Height           = 1;
    uploadDesc.DepthOrArraySize = 1;
    uploadDesc.MipLevels        = 1;
    uploadDesc.SampleDesc.Count = 1;
    uploadDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    Microsoft::WRL::ComPtr<ID3D12Resource> upload;
    if (FAILED(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &uploadDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload))))
        return false;

    void* mapped = nullptr;
    if (FAILED(upload->Map(0, nullptr, &mapped)))
        return false;
    {
        auto* destination = static_cast<uint8_t*>(mapped) + footprint.Offset;
        const auto* source = static_cast<const uint8_t*>(pixels);
        const size_t copyBytes = static_cast<size_t>(width) * m_bytesPerPixel;
        for (UINT row = 0; row < rowCount; ++row) {
            std::memcpy(destination + static_cast<size_t>(row) * footprint.Footprint.RowPitch,
                        source + static_cast<size_t>(row) * srcRowPitch,
                        copyBytes);
        }
    }
    upload->Unmap(0, nullptr);

    Microsoft::WRL::ComPtr<ID3D12CommandAllocator>    allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)))
        || FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(),
                                            nullptr, IID_PPV_ARGS(&list))))
        return false;

    const bool needsTransition = (currentState != D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource   = m_resource.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    if (needsTransition) {
        barrier.Transition.StateBefore = currentState;
        barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
        list->ResourceBarrier(1, &barrier);
    }

    D3D12_TEXTURE_COPY_LOCATION destinationLocation{};
    destinationLocation.pResource        = m_resource.Get();
    destinationLocation.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    destinationLocation.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION sourceLocation{};
    sourceLocation.pResource       = upload.Get();
    sourceLocation.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    sourceLocation.PlacedFootprint = footprint;
    list->CopyTextureRegion(&destinationLocation, x, y, 0, &sourceLocation, nullptr);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    list->ResourceBarrier(1, &barrier);

    if (FAILED(list->Close()))
        return false;
    ID3D12CommandList* lists[] = { list.Get() };
    queue->ExecuteCommandLists(1, lists);

    // WHY (同期 Flush): アップロードバッファをこの関数の寿命で解放するため、
    //   GPU が読み終わるまで待つ必要がある。呼び出しはフレームに 1 回以下
    //   (DynamicFontSource がダーティ矩形をまとめてから流す) に抑えられているため、
    //   既存の UploadTexture2D と同じ同期方式で十分と判断した。
    m_context->Flush();

    // ステートトラッカーへ現在状態を伝え、以降のパスが二重遷移しないようにする。
    if (m_tracker)
        m_tracker->Register(m_resource.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return true;
}

} // namespace fbzz::renderer
