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

        // 深度バッファを SRV として取得する (shadowMap → t8 等、次パスで読む)
        virtual std::shared_ptr<ITexture> GetDepthTexture() const = 0;

        // ImGui Viewport 表示用ネイティブテクスチャハンドル
        // DX11: ID3D11ShaderResourceView*  DX12: GPU descriptor handle (uint64_t として扱う)
        virtual void* GetNativeSRV(int slot = 0) const = 0;
    };

} // namespace fbzz::renderer