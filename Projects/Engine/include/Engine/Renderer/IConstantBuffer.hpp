/// @file    IConstantBuffer.hpp
/// @brief   定数バッファの抽象インターフェース。
/// @author  Hasegawa Jin
/// @date    2026-05-21
///
/// CPU 側の構造体を GPU へ転送する最小 API。
/// Upload のサイズと HLSL 側 cbuffer レイアウトを一致させる。
#pragma once
#include <cstddef>

namespace fbzz::renderer
{
    class IConstantBuffer
    {
    public:
        virtual ~IConstantBuffer() = default;

        // CPU からデータを書き込む。この呼び出し時点の内容は、次に行われる Submit が
        // キャプチャし、同じバッファへの後続 Update から独立して使われる。
        // 実装はバックエンド依存 (DX12 は upload heap への書き込み)。
        // DX12: Upload ヒープへの memcpy

        virtual void Update(const void* data, size_t sizeBytes) = 0;
        // 定数バッファのサイズ (バイト単位)
        virtual size_t GetSize() const = 0;
    };

} // namespace fbzz::renderer
