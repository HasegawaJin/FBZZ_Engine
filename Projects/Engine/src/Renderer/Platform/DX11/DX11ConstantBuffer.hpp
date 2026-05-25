// FBZZ Engine
// DX11ConstantBuffer.hpp | fbzz::renderer
// DX11 定数バッファ実装
// IConstantBuffer を継承し、シェーダー定数転送を抽象化する。
// ネイティブバッファは Renderer 内部だけで扱う。
//
// 設計方針:
//   IConstantBuffer を継承し、シェーダー側の cbuffer レジスタに対応する。
//   HLSL の cbuffer は 16 バイトアライメントが必須のため、Init で自動パディングする。
//   DrawCall::constantBuffers[] (スロット 0〜3) を介して Submit 時に VS/PS 両方にバインドする。
#pragma once

#include <d3d11.h>
#include <wrl/client.h>
#include <cstddef>
#include <Engine/Renderer/IConstantBuffer.hpp>

namespace fbzz::renderer
{

class DX11ConstantBuffer : public IConstantBuffer
{
public:
    // sizeBytes は自動的に 16 バイト境界へ切り上げられる
    bool Init(ID3D11Device* device, ID3D11DeviceContext* context, size_t sizeBytes);

    // data を GPU 側 cbuffer へ転送する (MAP_WRITE_DISCARD による同期ストール回避)
    void   Update(const void* data, size_t sizeBytes) override;

    // パディング後の実際のバッファサイズ (16 の倍数)
    size_t GetSize() const override { return m_size; }

    // Submit 時に VSSetConstantBuffers / PSSetConstantBuffers へ渡す生ポインタ
    ID3D11Buffer* GetBuffer() const { return m_buffer.Get(); }

private:
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_buffer;
    ID3D11DeviceContext*                 m_context = nullptr;
    size_t                               m_size    = 0;
};

} // namespace fbzz::renderer
