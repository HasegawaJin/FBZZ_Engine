// FBZZ Engine
// IRenderer.hpp | fbzz::renderer
// Renderer backend interface
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include "ComputeCall.hpp"
#include "DrawCall.hpp"
#include "IBuffer.hpp"
#include "IConstantBuffer.hpp"
#include "IPipelineState.hpp"
#include "IRenderTarget.hpp"
#include "IShader.hpp"
#include "ITexture.hpp"
#include "RenderState.hpp"
#include "ResourceHandle.hpp"
#include "SamplerMode.hpp"
#include <Math/Vector4.hpp>

namespace fbzz::renderer {

class ResourceManager;

class IRenderer {
public:
    virtual ~IRenderer() = default;

    virtual void BeginFrame() = 0;
    virtual void EndFrame() = 0;
    virtual void Clear(const math::Vector4& color) = 0;

    virtual std::shared_ptr<IBuffer> CreateVertexBuffer(const void* data, size_t sizeBytes, uint32_t stride) = 0;
    virtual std::shared_ptr<IBuffer> CreateIndexBuffer(const void* data, uint32_t count) = 0;
    virtual std::shared_ptr<IConstantBuffer> CreateConstantBuffer(size_t sizeBytes) = 0;
    virtual std::shared_ptr<IShader> CreateShader(const std::string& path) = 0;
    virtual std::shared_ptr<ITexture> CreateTexture(const std::string& path) = 0;
    virtual std::shared_ptr<IPipelineState> CreatePipelineState(const PipelineStateDesc& desc) = 0;
    virtual std::shared_ptr<IRenderTarget> CreateRenderTarget(uint32_t width, uint32_t height, uint32_t colorCount = 1) = 0;
    virtual std::shared_ptr<ITexture> CreateComputeTexture(uint32_t width, uint32_t height) = 0;

    virtual void Submit(const DrawCall& call, ResourceManager& resources) = 0;
    virtual void Dispatch(const ComputeCall& call, ResourceManager& resources) = 0;

    virtual void Resize(uint32_t width, uint32_t height) = 0;
    virtual uint32_t GetWidth() const = 0;
    virtual uint32_t GetHeight() const = 0;

    virtual void SetRenderTarget(std::shared_ptr<IRenderTarget> rt) = 0;
    virtual void SetRenderTarget(ResourceHandle<RenderTargetTag> rt, ResourceManager& resources) = 0;
    virtual void ClearDepth(float depth = 1.0f) = 0;

    virtual void SetSampler(uint32_t slot, SamplerMode mode) = 0;

    virtual void ImGuiInit(void* hwnd) = 0;
    virtual void ImGuiShutdown() = 0;
    virtual void ImGuiNewFrame() = 0;
    virtual void ImGuiRenderDrawData() = 0;
    virtual void* GetImTextureID(std::shared_ptr<IRenderTarget> rt, int slot = 0) = 0;
    virtual void* GetImTextureID(ResourceHandle<RenderTargetTag> rt, ResourceManager& resources, int slot = 0) = 0;
};

} // namespace fbzz::renderer
