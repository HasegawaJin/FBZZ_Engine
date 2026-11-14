/// @file    IStructuredBuffer.hpp
/// @brief   GPU StructuredBuffer（SRV）の抽象インターフェース。
/// @author  Hasegawa Jin
/// @date    2026-06-14
/// @note DrawInstanced 時のインスタンスデータバッファとして VS の t0 にバインドされる。
/// @note D3D11 は BIND_SHADER_RESOURCE + MISC_BUFFER_STRUCTURED で作成し SRV 経由で VS/PS から
/// @note       読む。バインドステージが IBuffer (頂点) と異なるため独立したインターフェースにする。
#pragma once

#include <cstddef>
#include <cstdint>

/// @note INVALID_BINDLESS_INDEX の正本。テクスチャとバッファで無効値の意味を揃えるため共有する。
#include "ITexture.hpp"

namespace fbzz::renderer
{

class IStructuredBuffer
{
public:
    virtual ~IStructuredBuffer() = default;

    /// @note CPU → GPU へデータを転送する（Map/Unmap、毎フレーム呼び出し可）
    virtual void Update(const void* data, size_t sizeBytes) = 0;
    /// @return False for unsupported or out-of-range updates; bytes outside the range remain unchanged.
    virtual bool UpdateRegion(const void* data, size_t sizeBytes, size_t offsetBytes)
    {
        if (offsetBytes!=0 || sizeBytes!=GetSize()) return false;
        Update(data,sizeBytes); return true;
    }

    virtual uint32_t GetElementCount() const = 0;
    virtual uint32_t GetStride()       const = 0;
    virtual size_t   GetSize()         const = 0;

    /// @note Source rows are tightly packed. A failed copy may have staged earlier rows in this call.
    virtual bool UpdateRows(const void* data, size_t rowBytes, size_t rowCount, size_t destinationPitch, size_t offsetBytes)
    {
        if (!data || rowBytes == 0 || rowCount == 0 || destinationPitch < rowBytes
            || offsetBytes > GetSize() || rowBytes > GetSize() - offsetBytes
            || rowCount - 1 > (GetSize() - offsetBytes - rowBytes) / destinationPitch) return false;
        if (rowBytes == destinationPitch) return UpdateRegion(data, rowBytes * rowCount, offsetBytes);
        const auto* source = static_cast<const uint8_t*>(data);
        for (size_t row = 0; row < rowCount; ++row)
            if (!UpdateRegion(source + row * rowBytes, rowBytes, offsetBytes + row * destinationPitch)) return false;
        return true;
    }

    /// @note シェーダーが ResourceDescriptorHeap[] へ渡す永続ディスクリプタ添字 (SRV)。
    /// @note 契約は ITexture::GetBindlessIndex() と同じ。無効値は INVALID_BINDLESS_INDEX。
    /// @see Docs/design/bindless.md
    virtual uint32_t GetBindlessIndex() const { return INVALID_BINDLESS_INDEX; }

    /// @note UAV 側の添字。読み取り専用で作られたバッファは INVALID を返す。
    virtual uint32_t GetBindlessUavIndex() const { return INVALID_BINDLESS_INDEX; }
};

} /// @note namespace fbzz::renderer
