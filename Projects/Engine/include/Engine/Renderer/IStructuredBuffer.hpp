/// @file    IStructuredBuffer.hpp
/// @brief   GPU StructuredBuffer（SRV）の抽象インターフェース。
/// @author  Hasegawa Jin
/// @date    2026-06-14
///
/// DrawInstanced 時のインスタンスデータバッファとして VS の t0 にバインドされる。
/// IBuffer（頂点・インデックス）とは用途が異なるため独立したインターフェースとして定義する。
///
/// WHY: D3D11 では StructuredBuffer は D3D11_BIND_SHADER_RESOURCE + MISC_BUFFER_STRUCTURED で作成し、
/// SRV 経由で VS/PS からアクセスする。頂点バッファ（IASetVertexBuffers）とは
/// バインドステージが異なるため、IBuffer に混在させず専用インターフェースを設ける。
#pragma once

#include <cstddef>
#include <cstdint>

// INVALID_BINDLESS_INDEX の正本。テクスチャとバッファで無効値の意味を揃えるため共有する。
#include "ITexture.hpp"

namespace fbzz::renderer
{

class IStructuredBuffer
{
public:
    virtual ~IStructuredBuffer() = default;

    // CPU → GPU へデータを転送する（Map/Unmap、毎フレーム呼び出し可）
    virtual void Update(const void* data, size_t sizeBytes) = 0;

    virtual uint32_t GetElementCount() const = 0;
    virtual uint32_t GetStride()       const = 0;
    virtual size_t   GetSize()         const = 0;

    // シェーダーが ResourceDescriptorHeap[] へ渡す永続ディスクリプタ添字 (SRV)。
    // 契約は ITexture::GetBindlessIndex() と同じ。無効値は INVALID_BINDLESS_INDEX。
    // @see Docs/design/bindless.md
    virtual uint32_t GetBindlessIndex() const { return INVALID_BINDLESS_INDEX; }

    // UAV 側の添字。読み取り専用で作られたバッファは INVALID を返す。
    virtual uint32_t GetBindlessUavIndex() const { return INVALID_BINDLESS_INDEX; }
};

} // namespace fbzz::renderer
