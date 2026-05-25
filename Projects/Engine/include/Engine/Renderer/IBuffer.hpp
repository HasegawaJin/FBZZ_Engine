// FBZZ Engine
// IBuffer.hpp | fbzz::renderer
// GPU バッファの抽象インターフェース
// 具体バックエンドのバッファ実装を隠し、サイズや stride だけを公開する。
// 生成は ResourceManager 経由に限定する。
#pragma once
#include <cstddef>
#include <cstdint>

namespace fbzz::renderer
{
    class IBuffer
    {
    public:
        virtual ~IBuffer() = default;
        
        // CPU 側からデータを再書き込み (動的バッファ用)
        virtual void Update(const void* data, size_t sizeBytes) = 0;

        virtual size_t GetSize() const = 0;
        // 頂点バッファのみ有効。インデックスバッファでは 0 を返す。
        virtual std::uint32_t GetStride() const = 0;
    };

} // namespace fbzz::renderer
