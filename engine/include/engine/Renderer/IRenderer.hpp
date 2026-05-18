#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include "DrawCall.hpp"
#include "IBuffer.hpp"
#include "IConstantBuffer.hpp"
#include "IPipelineState.hpp"
#include "IRenderTarget.hpp"
#include "IShader.hpp"
#include "ITexture.hpp"
#include "RenderState.hpp"
#include "SamplerMode.hpp"
#include <math/Vector4.hpp>

namespace fbzz::renderer 
{

    class IRenderer 
    {
    public:
        virtual ~IRenderer() = default;

        // フレーム制御
        virtual void BeginFrame() = 0;
        virtual void EndFrame()   = 0;
        virtual void Clear(const math::Vector4& color) = 0;

        // リソース生成
        virtual std::shared_ptr<IBuffer>        CreateVertexBuffer(const void* data, size_t sizeBytes, uint32_t stride) = 0;
        virtual std::shared_ptr<IBuffer>        CreateIndexBuffer(const void* data, uint32_t count) = 0;
        virtual std::shared_ptr<IConstantBuffer> CreateConstantBuffer(size_t sizeBytes) = 0;
        virtual std::shared_ptr<IShader>        CreateShader(const std::string& path) = 0;
        virtual std::shared_ptr<ITexture>       CreateTexture(const std::string& path) = 0;
        virtual std::shared_ptr<IPipelineState> CreatePipelineState(const PipelineStateDesc& desc) = 0;

        // 描画 (DrawCall は完全自己完結)
        virtual void Submit(const DrawCall& call) = 0;

        // ウィンドウリサイズ (Window::ResizeCallback から呼ぶ)
        virtual void Resize(uint32_t width, uint32_t height) = 0;

        // オフスクリーン RT (詳細: render_target.md)
        // colorCount: 同時出力カラーバッファ数 (1=通常, 2=GBuffer 等 MRT)
        virtual std::shared_ptr<IRenderTarget> CreateRenderTarget(uint32_t width, uint32_t height, uint32_t colorCount = 1) = 0;
        virtual void SetRenderTarget(std::shared_ptr<IRenderTarget> rt) = 0;  // nullptr = バックバッファ

        // サンプラー (詳細: sampler.md)
        virtual void SetSampler(uint32_t slot, SamplerMode mode) = 0;
    };

} // namespace fbzz::renderer