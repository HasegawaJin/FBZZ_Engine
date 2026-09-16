/// @file    DX11ConstantBuffer.cpp
/// @brief   DX11 定数バッファ実装。
/// @author  Hasegawa Jin
/// @date    2026-05-21
///
/// IConstantBuffer の Upload を D3D11 の Map / Unmap に対応付ける。
/// HLSL 側 cbuffer の 16 byte アライメントを呼び出し側と合わせる。
#include "DX11ConstantBuffer.hpp"
#include <Engine/Core/Logger.hpp>
#include <Engine/Core/HResult.hpp>
#include <memory>

namespace fbzz::renderer
{

bool DX11ConstantBuffer::Init(ID3D11Device* device, ID3D11DeviceContext* context, size_t sizeBytes)
{
    if (!device || !context || sizeBytes == 0)
    {
        FBZZ_LOG_ERROR("Invalid arguments for constant buffer initialization.");
        return false;
    }

    m_context = context;

    // HLSL の cbuffer は 16 バイトアライメントが必須。
    // (sizeBytes + 15) & ~15 で 16 の倍数に切り上げることでハードウェア要件を満たす。
    m_size = (sizeBytes + 15) & ~15;

    D3D11_BUFFER_DESC desc   = {};
    desc.ByteWidth           = static_cast<UINT>(m_size);
    desc.Usage               = D3D11_USAGE_DYNAMIC;           // 毎フレーム更新を想定
    desc.BindFlags           = D3D11_BIND_CONSTANT_BUFFER;
    desc.CPUAccessFlags      = D3D11_CPU_ACCESS_WRITE;

    // 定数バッファは初期データなしで確保し、最初の Update() で値を流し込む
    FBZZ_HR_CHECK(device->CreateBuffer(&desc, nullptr, m_buffer.GetAddressOf()));

    return true;
}

void DX11ConstantBuffer::Update(const void* data, size_t sizeBytes)
{
    // MAP_WRITE_DISCARD で旧フレームのデータを破棄しながら上書き。
    // これにより GPU が前フレームの cbuffer を参照中でもストールせずに書き換えられる。
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    HRESULT hr = m_context->Map(m_buffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) return;
    std::memcpy(mapped.pData, data, sizeBytes);
    m_context->Unmap(m_buffer.Get(), 0);
}

} // namespace fbzz::renderer
