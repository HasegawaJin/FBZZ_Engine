#pragma once
#include <cstdint>
#include <memory>
#include "ITexture.hpp"

namespace fbzz::renderer {

    class IRenderTarget {
    public:
        virtual ~IRenderTarget() = default;

        virtual uint32_t GetWidth()      const = 0;
        virtual uint32_t GetHeight()     const = 0;
        virtual uint32_t GetColorCount() const = 0;

        // カラーバッファ index 枚目を SRV として取得する (DrawCall::textures[] にセットして次パスで読む)
        virtual std::shared_ptr<ITexture> GetColorTexture(uint32_t index = 0) const = 0;
    };

} // namespace fbzz::renderer