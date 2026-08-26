// FBZZ Engine
// DX11StructuredBuffer.hpp | fbzz::renderer
// IStructuredBuffer の DX11 実装。
// readWrite=false (DYNAMIC): SRV のみ。DrawInstanced インスタンスデータや CS 入力に使う。
// readWrite=true  (DEFAULT): SRV + UAV。CS が RWStructuredBuffer として書き込む GPU 粒子プール等に使う。
//
// WHY: StructuredBuffer<T> は HLSL 側で InstanceID から直接インデックスできるため、
//      IASetVertexBuffers のインスタンスストリームより HLSL の記述が直感的になる。
//      また stride が 16 バイト境界に制限されないため、20 バイトのような非整合な
//      構造体も扱える。
// WHY readWrite=true: D3D11 では DYNAMIC バッファは UAV 不可のため、CS 書き込みが必要な場合は
//      DEFAULT + UpdateSubresource で CPU から初期データを供給する設計にする。
#pragma once

#include <Engine/Renderer/IStructuredBuffer.hpp>
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>

namespace fbzz::renderer
{

class DX11StructuredBuffer : public IStructuredBuffer
{
public:
    // elementCount * stride バイトの StructuredBuffer を作成する。
    // readWrite=false: DYNAMIC + SRV のみ (CPU Map/Unmap で更新)
    // readWrite=true : DEFAULT + SRV + UAV (CS が RWStructuredBuffer として書き込む)
    // data が nullptr の場合は未初期化で確保し、後で Update() する想定。
    bool Init(ID3D11Device*        device,
              ID3D11DeviceContext* context,
              const void*          data,
              uint32_t             elementCount,
              uint32_t             stride,
              bool                 readWrite = false);

    // DYNAMIC バッファ専用: Map/WRITE_DISCARD で CPU → GPU 転送
    // readWrite=true の DEFAULT バッファでは UpdateSubresource を使うため、このメソッドは no-op
    void Update(const void* data, size_t sizeBytes) override;

    // DEFAULT (readWrite) バッファ専用: UpdateSubresource で一部または全体を更新する
    // srcOffset: 書き込み先のバイトオフセット、sizeBytes=0 で全体を更新
    void UpdateRegion(const void* data, size_t srcOffsetBytes, size_t sizeBytes);

    uint32_t GetElementCount() const override { return m_elementCount; }
    uint32_t GetStride()       const override { return m_stride; }
    size_t   GetSize()         const override { return static_cast<size_t>(m_elementCount) * m_stride; }
    bool     IsReadWrite()     const          { return m_readWrite; }

    ID3D11ShaderResourceView*  GetSRV() const { return m_srv.Get(); }
    ID3D11UnorderedAccessView* GetUAV() const { return m_uav.Get(); }

private:
    Microsoft::WRL::ComPtr<ID3D11Buffer>              m_buffer;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>  m_srv;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> m_uav;
    ID3D11DeviceContext*                              m_context     = nullptr;
    uint32_t                                          m_elementCount = 0;
    uint32_t                                          m_stride       = 0;
    bool                                              m_readWrite    = false;
};

} // namespace fbzz::renderer
