// FBZZ Engine
// IConstantBuffer.hpp | fbzz::renderer
// Renderer constant buffer interface
#pragma once
#include <cstddef>

namespace fbzz::renderer
{
    class IConstantBuffer
    {
    public:
        virtual ~IConstantBuffer() = default;

        // CPU からデータを書き込む
        // DX11: UpdateSubresource / Map+Unmap
        // DX12: Upload ヒープへの memcpy

        virtual void Update(const void* data, size_t sizeBytes) = 0;
        // 定数バッファのサイズ (バイト単位)
        virtual size_t GetSize() const = 0;
    };

} // namespace fbzz::renderer
