/// @file    IConstantBuffer.hpp
/// @brief   定数バッファの抽象インターフェース。
/// @author  Hasegawa Jin
/// @date    2026-05-21
/// @note CPU 側の構造体を GPU へ転送する最小 API。
/// @note Upload のサイズと HLSL 側 cbuffer レイアウトを一致させる。
#pragma once
#include <cstddef>

namespace fbzz::renderer
{
    class IConstantBuffer
    {
    public:
        virtual ~IConstantBuffer() = default;

        /// @brief CPU からデータを書き込む。
        /// @note この呼び出し時点の内容は次に行われる Submit がキャプチャし、同じバッファへの
        /// @note       後続 Update から独立して使われる (DX12 は Upload ヒープへの memcpy)。
        virtual void Update(const void* data, size_t sizeBytes) = 0;
        /// @brief 定数バッファのサイズ (バイト単位)。
        virtual size_t GetSize() const = 0;
    };

} /// @note namespace fbzz::renderer
