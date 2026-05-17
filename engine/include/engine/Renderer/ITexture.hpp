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