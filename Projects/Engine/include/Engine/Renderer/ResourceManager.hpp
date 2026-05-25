// FBZZ Engine
// ResourceManager.hpp | fbzz::renderer
// Renderer リソースの所有とハンドル解決
// IRenderer の非公開生成 API を呼べる唯一の窓口。
// 上位システムは shared_ptr ではなく ResourceHandle を保持する。
#pragma once
#include <Engine/Renderer/ResourceHandle.hpp>
#include <Engine/Renderer/ResourcePool.hpp>
#include <Engine/Renderer/RenderState.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace fbzz::renderer {

class IBuffer;
class IConstantBuffer;
class IPipelineState;
class IRenderTarget;
class IRenderer;
class IShader;
class ITexture;

class ResourceManager {
public:
    explicit ResourceManager(IRenderer& renderer);
    ~ResourceManager();
    // Application が初期化時に登録する唯一のインスタンス。
    // System から ResourceManager& を受け取れない場面で使うフォールバック。
    static ResourceManager* Active();

    ResourceHandle<ShaderTag> LoadShader(std::string_view path);
    ResourceHandle<TextureTag> LoadTexture(std::string_view path);
    ResourceHandle<TextureTag> CreateTexture(const uint8_t* rgba, uint32_t width, uint32_t height);

    ResourceHandle<BufferTag> CreateVertexBuffer(const void* data, size_t bytes, uint32_t stride);
    ResourceHandle<BufferTag> CreateIndexBuffer(const void* data, uint32_t count);
    ResourceHandle<ConstantBufferTag> CreateConstantBuffer(size_t sizeBytes);
    ResourceHandle<PipelineStateTag> CreatePipelineState(const PipelineStateDesc& desc);
    ResourceHandle<RenderTargetTag> CreateRenderTarget(uint32_t width, uint32_t height, uint32_t colorCount = 1);
    ResourceHandle<TextureTag> CreateComputeTexture(uint32_t width, uint32_t height);

    IShader* Get(ResourceHandle<ShaderTag> h);
    ITexture* Get(ResourceHandle<TextureTag> h);
    IBuffer* Get(ResourceHandle<BufferTag> h);
    IConstantBuffer* Get(ResourceHandle<ConstantBufferTag> h);
    IPipelineState* Get(ResourceHandle<PipelineStateTag> h);
    IRenderTarget* Get(ResourceHandle<RenderTargetTag> h);

    ResourceHandle<TextureTag> GetColorTexture(ResourceHandle<RenderTargetTag> rt, uint32_t index = 0);
    ResourceHandle<TextureTag> GetDepthTexture(ResourceHandle<RenderTargetTag> rt);

    void Update(ResourceHandle<BufferTag> h, const void* data, size_t sizeBytes);
    void Update(ResourceHandle<ConstantBufferTag> h, const void* data, size_t sizeBytes);

    void Release(ResourceHandle<ShaderTag> h);
    void Release(ResourceHandle<TextureTag> h);
    void Release(ResourceHandle<BufferTag> h);
    void Release(ResourceHandle<ConstantBufferTag> h);
    void Release(ResourceHandle<PipelineStateTag> h);
    void Release(ResourceHandle<RenderTargetTag> h);

private:
    static uint64_t Key(ResourceHandle<RenderTargetTag> h);

    IRenderer& m_renderer;

    ResourcePool<IShader, ShaderTag> m_shaders;
    ResourcePool<ITexture, TextureTag> m_textures;
    ResourcePool<IBuffer, BufferTag> m_buffers;
    ResourcePool<IConstantBuffer, ConstantBufferTag> m_constantBuffers;
    ResourcePool<IPipelineState, PipelineStateTag> m_pipelineStates;
    ResourcePool<IRenderTarget, RenderTargetTag> m_renderTargets;

    std::unordered_map<std::string, ResourceHandle<ShaderTag>> m_shaderCache;
    std::unordered_map<std::string, ResourceHandle<TextureTag>> m_textureCache;
    std::unordered_map<uint64_t, std::vector<ResourceHandle<TextureTag>>> m_renderTargetColors;
    std::unordered_map<uint64_t, ResourceHandle<TextureTag>> m_renderTargetDepths;
};

} // namespace fbzz::renderer
