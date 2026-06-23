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

    // GPU 側バッファを data で上書きする (Map/Unmap による CPU→GPU 転送)
    void Update(const void* data, std::size_t sizeBytes) override;

    // バイト単位の全体サイズ (DrawIndexed の引数計算に使用)
    std::size_t   GetSize()   const override { return m_size; }

    // 頂点バッファとして IA ステージにバインドする際に必要なストライド
    std::uint32_t GetStride() const override { return m_stride; }

    // DX11 内部実装 (Submit 時に static_cast してアクセスする)
    ID3D11Buffer* GetBuffer() const { return m_buffer.Get(); }

private:
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_buffer;
    ID3D11DeviceContext*                 m_context = nullptr;
    std::size_t                          m_size    = 0;
    std::uint32_t                        m_stride  = 0;
};

} // namespace fbzz::renderer
