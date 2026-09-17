/// @file    DX12RenderTarget.cpp
/// @brief   RGBA16F MRTとSRV読み取り可能なR32深度の生成。
/// @author  Hasegawa Jin
/// @date    2026-07-15
#include "DX12RenderTarget.hpp"

#include "DX12Context.hpp"
#include "DX12StateTracker.hpp"
#include <Engine/Core/Logger.hpp>
#include <cassert>
#include <bit>

namespace fbzz::renderer {

DX12RenderTarget::~DX12RenderTarget()
{
    if (m_tracker) {
        for (auto& color : m_colors) m_tracker->Remove(color.Get());
        m_tracker->Remove(m_depth.Get());
        m_tracker->Remove(m_cube.Get());
    }
    if (m_context) {
        for (auto& color : m_colors) m_context->DeferRelease(color);
        m_context->DeferRelease(m_depth);
        m_context->DeferRelease(m_cube);
    }
}

bool DX12RenderTarget::InitCubemap(
    DX12Context* context, DX12StateTracker* tracker, uint32_t size, uint32_t mipCount)
{
    if (!context || !tracker || size == 0) return false;
    m_tracker = tracker;
    m_context = context;
    m_width = m_height = size;
    m_colorCount = 1;
    m_mipCount = mipCount > 0 ? mipCount : 1;
    m_isCubemap = true;
    ID3D12Device* device = context->GetDevice();
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeap{};
    rtvHeap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeap.NumDescriptors = 6 * m_mipCount;
    if (FAILED(device->CreateDescriptorHeap(&rtvHeap, IID_PPV_ARGS(&m_rtvHeap)))) return false;
    D3D12_DESCRIPTOR_HEAP_DESC srvHeap{};
    srvHeap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeap.NumDescriptors = 1;
    if (FAILED(device->CreateDescriptorHeap(&srvHeap, IID_PPV_ARGS(&m_srvHeap)))) return false;
    m_rtvIncrement = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    m_srvIncrement = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = size;
    desc.Height = size;
    desc.DepthOrArraySize = 6;
    desc.MipLevels = static_cast<UINT16>(m_mipCount);
    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE clear{};
    clear.Format = desc.Format;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear, IID_PPV_ARGS(&m_cube)))) return false;
    for (uint32_t face = 0; face < 6; ++face) {
        for (uint32_t mip = 0; mip < m_mipCount; ++mip) {
            D3D12_RENDER_TARGET_VIEW_DESC rtv{};
            rtv.Format = desc.Format;
            rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
            rtv.Texture2DArray.MipSlice = mip;
            rtv.Texture2DArray.FirstArraySlice = face;
            rtv.Texture2DArray.ArraySize = 1;
            device->CreateRenderTargetView(m_cube.Get(), &rtv, GetFaceRtv(face, mip));
        }
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = desc.Format;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.TextureCube.MipLevels = m_mipCount;
    device->CreateShaderResourceView(m_cube.Get(), &srv, GetCubeSrv());
    tracker->Register(m_cube.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return true;
}

/// Format → DXGI。DX11 側 (DX11RenderTarget.cpp) と同じ対応にすること。
static DXGI_FORMAT ToDxgi(Format format)
{
    switch (format) {
    case Format::RGBA16F:    return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case Format::RGBA8:      return DXGI_FORMAT_R8G8B8A8_UNORM;
    case Format::R11G11B10F: return DXGI_FORMAT_R11G11B10_FLOAT;
    case Format::RG16F:      return DXGI_FORMAT_R16G16_FLOAT;
    case Format::R16F:       return DXGI_FORMAT_R16_FLOAT;
    case Format::R8:         return DXGI_FORMAT_R8_UNORM;
    }
    return DXGI_FORMAT_R16G16B16A16_FLOAT;
}

bool DX12RenderTarget::Init(DX12Context* context, DX12StateTracker* tracker,
                            uint32_t width, uint32_t height, const RenderTargetDesc& desc)
{
    const uint32_t colorCount = desc.colorCount;
    assert(colorCount <= MAX_COLOR);
    assert((colorCount > 0 || desc.withDepth) && "カラーも深度も無い RT は作れない");
    if (!context || !tracker || width == 0 || height == 0 || colorCount > MAX_COLOR)
        return false;
    m_tracker = tracker;
    m_context = context;
    m_width = width;
    m_height = height;
    m_colorCount = colorCount;
    ID3D12Device* device = context->GetDevice();
    D3D12_DESCRIPTOR_HEAP_DESC heap{};
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heap.NumDescriptors = colorCount > 0 ? colorCount : 1;
    if (FAILED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&m_rtvHeap)))) return false;
    if (desc.withDepth) {
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        heap.NumDescriptors = 1;
        if (FAILED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&m_dsvHeap)))) return false;
    }
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap.NumDescriptors = colorCount + 1;
    if (FAILED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&m_srvHeap)))) return false;
    m_rtvIncrement = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    m_srvIncrement = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC colorDesc{};
    colorDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    colorDesc.Width = width;
    colorDesc.Height = height;
    colorDesc.DepthOrArraySize = 1;
    colorDesc.MipLevels = 1;
    colorDesc.Format = ToDxgi(desc.format);
    colorDesc.SampleDesc.Count = 1;
    colorDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    for (uint32_t index = 0; index < colorCount; ++index) {
        if (FAILED(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &colorDesc,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&m_colors[index]))))
            return false;
        device->CreateRenderTargetView(m_colors[index].Get(), nullptr, GetRtv(index));
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = colorDesc.Format;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(m_colors[index].Get(), &srv, GetColorSrv(index));
        tracker->Register(m_colors[index].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }

    if (!desc.withDepth)
        return true;

    D3D12_RESOURCE_DESC depthDesc = colorDesc;
    depthDesc.Format = DXGI_FORMAT_R32_TYPELESS;
    depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE depthClear{};
    depthClear.Format = DXGI_FORMAT_D32_FLOAT;
    depthClear.DepthStencil.Depth = 1.0f;
    if (FAILED(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &depthDesc,
            D3D12_RESOURCE_STATE_DEPTH_WRITE, &depthClear, IID_PPV_ARGS(&m_depth))))
        return false;
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
    dsv.Format = DXGI_FORMAT_D32_FLOAT;
    dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    device->CreateDepthStencilView(m_depth.Get(), &dsv, GetDsv());
    D3D12_SHADER_RESOURCE_VIEW_DESC depthSrv{};
    depthSrv.Format = DXGI_FORMAT_R32_FLOAT;
    depthSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    depthSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    depthSrv.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(m_depth.Get(), &depthSrv, GetDepthSrv());
    tracker->Register(m_depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE);
    return true;
}

D3D12_CPU_DESCRIPTOR_HANDLE DX12RenderTarget::GetRtv(uint32_t index) const
{
    auto handle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(index) * m_rtvIncrement;
    return handle;
}

/// 深度を持たない RT では DSV ヒープ自体を作らない。呼び出し側は HasDepth() で分岐する。
D3D12_CPU_DESCRIPTOR_HANDLE DX12RenderTarget::GetDsv() const
{
    return m_dsvHeap ? m_dsvHeap->GetCPUDescriptorHandleForHeapStart()
                     : D3D12_CPU_DESCRIPTOR_HANDLE{};
}
D3D12_CPU_DESCRIPTOR_HANDLE DX12RenderTarget::GetColorSrv(uint32_t index) const
{
    auto handle = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(index) * m_srvIncrement;
    return handle;
}
D3D12_CPU_DESCRIPTOR_HANDLE DX12RenderTarget::GetDepthSrv() const
{
    auto handle = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(m_colorCount) * m_srvIncrement;
    return handle;
}
D3D12_GPU_DESCRIPTOR_HANDLE DX12RenderTarget::GetColorSrvGpu(uint32_t index) const
{
    auto handle = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<UINT64>(index) * m_srvIncrement;
    return handle;
}
ID3D12Resource* DX12RenderTarget::GetColorResource(uint32_t index) const
{
    if (m_isCubemap) return index == 0 ? m_cube.Get() : nullptr;
    return index < m_colorCount ? m_colors[index].Get() : nullptr;
}

D3D12_CPU_DESCRIPTOR_HANDLE DX12RenderTarget::GetFaceRtv(uint32_t face, uint32_t mip) const
{
    if (!m_isCubemap || face >= 6 || mip >= m_mipCount) return {};
    auto handle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(face * m_mipCount + mip) * m_rtvIncrement;
    return handle;
}
void* DX12RenderTarget::GetNativeSRV(int slot) const
{
    if (slot < 0 || static_cast<uint32_t>(slot) >= m_colorCount) return nullptr;
    if (m_isCubemap)
        return std::bit_cast<void*>(m_srvHeap->GetGPUDescriptorHandleForHeapStart().ptr);
    return std::bit_cast<void*>(GetColorSrvGpu(static_cast<uint32_t>(slot)).ptr);
}

void DX12RenderTarget::TransitionColorForRead(ID3D12GraphicsCommandList* commands, uint32_t index)
{
    if (m_tracker && index < m_colorCount)
        m_tracker->Transition(commands, m_isCubemap ? m_cube.Get() : m_colors[index].Get(),
                              D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

} // namespace fbzz::renderer
