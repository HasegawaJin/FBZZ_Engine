// FBZZ Engine
// DX11Buffer.hpp | fbzz::renderer
// DX11 頂点・インデックスバッファ実装
// IBuffer を継承し、上位レイヤーから D3D11Buffer を隠蔽する。
// 頂点 stride と byte size を保持し、DrawCall 解決時に使う。
//
// 設計方針:
//   IBuffer を継承し、上位レイヤーが DX11 の詳細に依存しないよう抽象化する。
//   バッファは D3D11_USAGE_DYNAMIC + MAP_WRITE_DISCARD で生成し、
//   毎フレーム部分更新 (頂点ストリーミング等) に対応する。
#pragma once

#include <Engine/Renderer/IBuffer.hpp>
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>
#include <memory>

namespace fbzz::renderer
{

class DX11Buffer : public IBuffer
{
public:
    // bindFlag: D3D11_BIND_VERTEX_BUFFER または D3D11_BIND_INDEX_BUFFER
    // data が nullptr の場合、バッファは未初期化で確保される (後で Update する想定)
    bool Init(ID3D11Device*        device,
              ID3D11DeviceContext* context,
              const void*          data,
              std::size_t          sizeBytes,
              std::uint32_t        stride,
              D3D11_BIND_FLAG      bindFlag);

    // InitGpuWritableVertex — CS が書き込み、IA が頂点として読むバッファを確保する。
    // WHY: コンピュートスキニングの出力先。スキニング結果を一度だけ計算して
    //      シャドウ・GBuffer・Forward で共有するために、同じバッファへ
    //      「UAV として書く」と「頂点バッファとして読む」の両方を許す必要がある。
    // NOTE: 通常の Init と違い USAGE_DEFAULT / CPU アクセスなしで作る。
    //       UAV は Structured (StructureByteStride = stride) として作るので、
    //       CS 側は RWStructuredBuffer<T> で受ける。
    bool InitGpuWritableVertex(ID3D11Device* device,
                               ID3D11DeviceContext* context,
                               std::size_t   sizeBytes,
                               std::uint32_t stride);

    // GPU 側バッファを data で上書きする (Map/Unmap による CPU→GPU 転送)
    void Update(const void* data, std::size_t sizeBytes) override;

    // バイト単位の全体サイズ (DrawIndexed の引数計算に使用)
    std::size_t   GetSize()   const override { return m_size; }

    // 頂点バッファとして IA ステージにバインドする際に必要なストライド
    std::uint32_t GetStride() const override { return m_stride; }

    // DX11 内部実装 (Submit 時に static_cast してアクセスする)
    ID3D11Buffer* GetBuffer() const { return m_buffer.Get(); }

    // InitGpuWritableVertex で作った場合のみ非 null。Dispatch が u スロットへ束縛する。
    ID3D11UnorderedAccessView* GetUAV() const { return m_uav.Get(); }

private:
    Microsoft::WRL::ComPtr<ID3D11Buffer>              m_buffer;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> m_uav;
    ID3D11DeviceContext*                              m_context = nullptr;
    std::size_t                                       m_size    = 0;
    std::uint32_t                                     m_stride  = 0;
};

} // namespace fbzz::renderer
