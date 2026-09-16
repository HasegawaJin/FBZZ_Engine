/// @file    IBuffer.hpp
/// @brief   GPU バッファの抽象インターフェース。
/// @author  Hasegawa Jin
/// @date    2026-05-21
///
/// 具体バックエンドのバッファ実装を隠し、サイズや stride だけを公開する。
/// 生成は ResourceManager 経由に限定する。
#pragma once
#include <cstddef>
#include <cstdint>

// INVALID_BINDLESS_INDEX の正本。
#include "ITexture.hpp"

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

        // GPU 書き込み可能な頂点バッファ (コンピュートスキニングの出力) の UAV 添字。
        // 書き込み経路を持たないバッファは INVALID_BINDLESS_INDEX を返す。
        // @see Docs/design/bindless.md
        virtual std::uint32_t GetBindlessUavIndex() const { return INVALID_BINDLESS_INDEX; }
    };

} // namespace fbzz::renderer
