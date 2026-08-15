// FBZZ Engine
// DX11Buffer.cpp | fbzz::renderer
// DX11 頂点・インデックスバッファ実装
// IBuffer の抽象 API を D3D11Buffer に対応付ける。
// 動的更新を前提に Map / Unmap で CPU から内容を書き換える。
//
// バッファ戦略:
//   D3D11_USAGE_DYNAMIC + D3D11_CPU_ACCESS_WRITE を採用。
//   頂点データを毎フレーム CPU から書き換えるユースケース (デバッグ描画・UI 等) に対応する。
//   更新が不要な静的ジオメトリは Step 2 以降で IMMUTABLE バッファに移行予定。
#include "DX11Buffer.hpp"
#include <Engine/Core/Logger.hpp>
#include <Engine/Core/HResult.hpp>

using namespace fbzz::core;
using namespace fbzz::renderer;
using namespace Microsoft::WRL;

bool DX11Buffer::Init(ID3D11Device*        device,
                      ID3D11DeviceContext* context,
                      const void*          data,
                      std::size_t          sizeBytes,
                      std::uint32_t        stride,
                      D3D11_BIND_FLAG      bindFlag)
{
    if (!device || !context || sizeBytes == 0)
    {
        Logger::Error("Invalid arguments for buffer initialization.");
        return false;
    }

    m_size    = sizeBytes;
    m_stride  = stride;
    m_context = context;

    D3D11_BUFFER_DESC bufferDesc         = {};
    bufferDesc.ByteWidth                 = static_cast<UINT>(m_size);
    bufferDesc.Usage                     = D3D11_USAGE_DYNAMIC;   // CPU 書き込み可能
    bufferDesc.BindFlags                 = bindFlag;
    bufferDesc.CPUAccessFlags            = D3D11_CPU_ACCESS_WRITE;
    bufferDesc.MiscFlags                 = 0;
    bufferDesc.StructureByteStride       = m_stride;

    D3D11_SUBRESOURCE_DATA subResData = {};
    subResData.pSysMem                = data;

    // data == nullptr の場合は初期データなしで確保 (後で Update を呼ぶ前提)
    FBZZ_HR_CHECK(device->CreateBuffer(
        &bufferDesc,
        data ? &subResData : nullptr,
        m_buffer.GetAddressOf()));

    return true;
}

bool DX11Buffer::InitGpuWritableVertex(ID3D11Device*        device,
                                       ID3D11DeviceContext* context,
                                       std::size_t          sizeBytes,
                                       std::uint32_t        stride)
{
    if (!device || !context || sizeBytes == 0 || stride == 0)
    {
        Logger::Error("Invalid arguments for GPU-writable vertex buffer initialization.");
        return false;
    }

    m_size    = sizeBytes;
    m_stride  = stride;
    m_context = context;

    // CS が書き、IA が読む。CPU は一切触らないので USAGE_DEFAULT / CPUAccessFlags = 0。
    // WHY STRUCTURED: UAV を RWStructuredBuffer<T> として受けたい。Raw (ByteAddress) でも
    //      書けるが、CS 側が Store4 の連続でオフセット計算を手書きすることになり、
    //      頂点レイアウトを変えたときに黙ってずれる。構造体で受ければ型で守れる。
    D3D11_BUFFER_DESC bufferDesc   = {};
    bufferDesc.ByteWidth           = static_cast<UINT>(m_size);
    bufferDesc.Usage               = D3D11_USAGE_DEFAULT;
    bufferDesc.BindFlags           = D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_UNORDERED_ACCESS;
    bufferDesc.CPUAccessFlags      = 0;
    bufferDesc.MiscFlags           = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    bufferDesc.StructureByteStride = stride;

    FBZZ_HR_CHECK(device->CreateBuffer(&bufferDesc, nullptr, m_buffer.GetAddressOf()));

    D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.Format              = DXGI_FORMAT_UNKNOWN; // Structured は UNKNOWN 固定
    uavDesc.ViewDimension       = D3D11_UAV_DIMENSION_BUFFER;
    uavDesc.Buffer.FirstElement = 0;
    uavDesc.Buffer.NumElements  = static_cast<UINT>(m_size / stride);
    FBZZ_HR_CHECK(device->CreateUnorderedAccessView(m_buffer.Get(), &uavDesc, m_uav.GetAddressOf()));

    return true;
}

void DX11Buffer::Update(const void* data, std::size_t sizeBytes)
{
    // D3D11_MAP_WRITE_DISCARD:
    //   GPU がまだ読み取り中の領域を待たず、ドライバが別の物理メモリ領域を割り当てる。
    //   これにより CPU/GPU の同期ストールを避けてストリーミング更新を実現する。
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    HRESULT hr = m_context->Map(m_buffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) return;
    std::memcpy(mapped.pData, data, sizeBytes);
    m_context->Unmap(m_buffer.Get(), 0);
}
