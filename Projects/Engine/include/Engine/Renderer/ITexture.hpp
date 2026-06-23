// FBZZ Engine
// ITexture.hpp | fbzz::renderer
// Texture の抽象インターフェース
// SRV / UAV などのネイティブ表現を上位から隠す。
// ファイル読み込みと生成は ResourceManager / AssetManager 経由にする。
#pragma once
#include <cstdint>

namespace fbzz::renderer
{
    class ITexture
    {
        public:
        virtual ~ITexture() = default;

        virtual std::uint32_t GetWidth() const = 0;
        virtual std::uint32_t GetHeight() const = 0;
    };
} // namespace fbzz::renderer
