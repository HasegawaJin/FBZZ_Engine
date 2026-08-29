/// @file    DX11StructuredBuffer.cpp
/// @brief   DX11 の構造化バッファ実装。
/// @author  Hasegawa Jin
/// @date    2026-06-14
#include "DX11StructuredBuffer.hpp"
#include <Engine/Core/HResult.hpp>
#include <Engine/Core/Logger.hpp>
#include <cstring>

namespace fbzz::renderer
{

bool DX11StructuredBuffer::Init(ID3D11Device*        device,
                                 ID3D11DeviceContext* context,
                                 const void*          data,
                                 uint32_t             elementCount,
                                 uint32_t             stride,
                                 bool                 readWrite)
{
    if (!device || !context || elementCount == 0 || stride == 0)
    {
        FBZZ_LOG_ERROR("DX11StructuredBuffer::Init: invalid arguments (count=%u stride=%u)", elementCount, stride);
        return false;
    }

    m_context      = context;
    m_elementCount = elementCount;
    m_stride       = stride;
    m_readWrite    = readWrite;

    D3D11_BUFFER_DESC desc   = {};
    desc.ByteWidth            = elementCount * stride;
    desc.MiscFlags            = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    desc.StructureByteStride  = stride;

    if (readWrite)
    {
        // DEFAULT: CS が RWStructuredBuffer として書き込める。CPU 更新は UpdateSubresource。
        desc.Usage          = D3D11_USAGE_DEFAULT;
        desc.BindFlags      = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        desc.CPUAccessFlags = 0;
    }
    else
    {
        // DYNAMIC: 毎フレーム CPU から Map/WRITE_DISCARD で更新するインスタンスデータ等に使う。
        desc.Usage          = D3D11_USAGE_DYNAMIC;
        desc.BindFlags      = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    }

    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem                = data;

    HRESULT hr = device->CreateBuffer(&desc, data ? &initData : nullptr, m_buffer.GetAddressOf());
    if (FAILED(hr))
    {
        FBZZ_LOG_ERROR("DX11StructuredBuffer: CreateBuffer failed (0x%08X, count=%u stride=%u rw=%d)",
                       static_cast<unsigned>(hr), elementCount, stride, (int)readWrite);
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format                          = DXGI_FORMAT_UNKNOWN;
    srvDesc.ViewDimension                   = D3D11_SRV_DIMENSION_BUFFER;
    srvDesc.Buffer.FirstElement             = 0;
    srvDesc.Buffer.NumElements              = elementCount;

    hr = device->CreateShaderResourceView(m_buffer.Get(), &srvDesc, m_srv.GetAddressOf());
    if (FAILED(hr))
    {
        FBZZ_LOG_ERROR("DX11StructuredBuffer: CreateShaderResourceView failed (0x%08X)", static_cast<unsigned>(hr));
        return false;
    }

    if (readWrite)
    {
        D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.Format                           = DXGI_FORMAT_UNKNOWN;
        uavDesc.ViewDimension                    = D3D11_UAV_DIMENSION_BUFFER;
        uavDesc.Buffer.FirstElement              = 0;
        uavDesc.Buffer.NumElements               = elementCount;
        uavDesc.Buffer.Flags                     = 0;

        hr = device->CreateUnorderedAccessView(m_buffer.Get(), &uavDesc, m_uav.GetAddressOf());
        if (FAILED(hr))
        {
            FBZZ_LOG_ERROR("DX11StructuredBuffer: CreateUnorderedAccessView failed (0x%08X)", static_cast<unsigned>(hr));
            return false;
        }
    }

    return true;
}

void DX11StructuredBuffer::Update(const void* data, size_t sizeBytes)
{
    if (!m_buffer || !data || sizeBytes == 0) return;

    if (m_readWrite)
    {
        // DEFAULT バッファは Map 不可。UpdateSubresource で全体を上書きする。
        m_context->UpdateSubresource(m_buffer.Get(), 0, nullptr, data, 0, 0);
        return;
    }

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    HRESULT hr = m_context->Map(m_buffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr))
    {
        FBZZ_LOG_ERROR("DX11StructuredBuffer::Update: Map failed (0x%08X)", static_cast<unsigned>(hr));
        return;
    }

    const size_t copySize = (std::min)(sizeBytes, static_cast<size_t>(m_elementCount) * m_stride);
    std::memcpy(mapped.pData, data, copySize);
    m_context->Unmap(m_buffer.Get(), 0);
}

void DX11StructuredBuffer::UpdateRegion(const void* data, size_t srcOffsetBytes, size_t sizeBytes)
{
    if (!m_buffer || !data || sizeBytes == 0 || !m_readWrite) return;

    D3D11_BOX box = {};
    box.left   = static_cast<UINT>(srcOffsetBytes);
    box.right  = static_cast<UINT>(srcOffsetBytes + sizeBytes);
    box.top    = 0; box.bottom = 1;
    box.front  = 0; box.back   = 1;
    m_context->UpdateSubresource(m_buffer.Get(), 0, &box, data, 0, 0);
}

} // namespace fbzz::renderer
