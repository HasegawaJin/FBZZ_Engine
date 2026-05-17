#pragma once
#include <cstdint>
#include <memory>
#include "ITexture.hpp"

namespace fbzz::renderer {

    class IRenderTarget {
    public:
        virtual ~IRenderTarget() = default;

        virtual uint32_t GetWidth()  const = 0;
        virtual uint32_t GetHeight() const = 0;

        // カラーバッファをシェーダーのテクスチャとして渡す用
        virtual std::shared_ptr<ITexture> GetColorTexture() const = 0;
    };

} // namespace fbzz::renderer