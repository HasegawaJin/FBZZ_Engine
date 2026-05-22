// FBZZ Engine
// ResourceManager.cpp | fbzz::renderer
// Central renderer resource ownership and handle lookup
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Renderer/IBuffer.hpp>
#include <Engine/Renderer/IConstantBuffer.hpp>
#include <Engine/Renderer/IPipelineState.hpp>
#include <Engine/Renderer/IRenderTarget.hpp>
#include <Engine/Renderer/IRenderer.hpp>
#include <Engine/Renderer/IShader.hpp>
#include <Engine/Renderer/ITexture.hpp>
#include <Engine/Core/Logger.hpp>
#include <cassert>

namespace fbzz::renderer {

namespace {
ResourceManager* s_activeResourceManager = nullptr;
}

ResourceManager::ResourceManager(IRenderer& renderer)
    : m_renderer(renderer)
{
    s_activeResourceManager = this;
}

ResourceManager::~ResourceManager()
{
    if (s_activeResourceManager == this)
        s_activeResourceManager = nullptr;
}

ResourceManager* ResourceManager::Active()
{
    return s_activeResourceManager;
}

ResourceHandle<ShaderTag> ResourceManager::LoadShader(std::string_view path)
{
    const std::string key(path);
    auto it = m_shaderCache.find(key);
    if (it != m_shaderCache.end()) return it->second;

    auto shader = m_renderer.CreateShader(key);
    if (!shader) {
        FBZZ_LOG_ERROR("Shader load failed: %s", key.c_str());
        return ResourceHandle<ShaderTag>::Null();
    }

    ResourceHandle<ShaderTag> handle = m_shaders.Insert(std::move(shader));
    m_shaderCache[key] = handle;
    return handle;
}

ResourceHandle<TextureTag> ResourceManager::LoadTexture(std::string_view path)
{
    const std::string key(path);
    auto it = m_textureCache.find(key);
    if (it != m_textureCache.end()) return it->second;

    auto texture = m_renderer.CreateTexture(key);
    if (!texture) {
        FBZZ_LOG_ERROR("Texture load failed: %s", key.c_str());
        return ResourceHandle<TextureTag>::Null();
    }

    ResourceHandle<TextureTag> handle = m_textures.Insert(std::move(texture));
    m_textureCache[key] = handle;
    return handle;
}

ResourceHandle<BufferTag> ResourceManager::CreateVertexBuffer(const void* data, size_t bytes, uint32_t stride)
{
    return m_buffers.Insert(m_renderer.CreateVertexBuffer(data, bytes, stride));
}

ResourceHandle<BufferTag> ResourceManager::CreateIndexBuffer(const void* data, uint32_t count)
{
    return m_buffers.Insert(m_renderer.CreateIndexBuffer(data, count));
}

ResourceHandle<ConstantBufferTag> ResourceManager::CreateConstantBuffer(size_t sizeBytes)
{
    return m_constantBuffers.Insert(m_renderer.CreateConstantBuffer(sizeBytes));
}

ResourceHandle<PipelineStateTag> ResourceManager::CreatePipelineState(const PipelineStateDesc& desc)
{
    return m_pipelineStates.Insert(m_renderer.CreatePipelineState(desc));
}

ResourceHandle<RenderTargetTag> ResourceManager::CreateRenderTarget(uint32_t width, uint32_t height, uint32_t colorCount)
{
    auto rt = m_renderer.CreateRenderTarget(width, height, colorCount);
    if (!rt) return ResourceHandle<RenderTargetTag>::Null();

    std::vector<ResourceHandle<TextureTag>> colors;
    colors.reserve(colorCount);
    for (uint32_t i = 0; i < colorCount; ++i)
        colors.push_back(m_textures.Insert(rt->GetColorTexture(i)));
    ResourceHandle<TextureTag> depth = m_textures.Insert(rt->GetDepthTexture());

    ResourceHandle<RenderTargetTag> handle = m_renderTargets.Insert(std::move(rt));
    const uint64_t key = Key(handle);
    m_renderTargetColors[key] = std::move(colors);
    m_renderTargetDepths[key] = depth;
    return handle;
}

ResourceHandle<TextureTag> ResourceManager::CreateComputeTexture(uint32_t width, uint32_t height)
{
    return m_textures.Insert(m_renderer.CreateComputeTexture(width, height));
}

IShader* ResourceManager::Get(ResourceHandle<ShaderTag> h) { return m_shaders.Get(h); }
ITexture* ResourceManager::Get(ResourceHandle<TextureTag> h) { return m_textures.Get(h); }
IBuffer* ResourceManager::Get(ResourceHandle<BufferTag> h) { return m_buffers.Get(h); }
IConstantBuffer* ResourceManager::Get(ResourceHandle<ConstantBufferTag> h) { return m_constantBuffers.Get(h); }
IPipelineState* ResourceManager::Get(ResourceHandle<PipelineStateTag> h) { return m_pipelineStates.Get(h); }
IRenderTarget* ResourceManager::Get(ResourceHandle<RenderTargetTag> h) { return m_renderTargets.Get(h); }
std::shared_ptr<IRenderTarget> ResourceManager::GetShared(ResourceHandle<RenderTargetTag> h) { return m_renderTargets.GetShared(h); }

ResourceHandle<TextureTag> ResourceManager::GetColorTexture(ResourceHandle<RenderTargetTag> rt, uint32_t index)
{
    auto it = m_renderTargetColors.find(Key(rt));
    if (it == m_renderTargetColors.end()) return ResourceHandle<TextureTag>::Null();
    if (index >= it->second.size()) return ResourceHandle<TextureTag>::Null();
    return it->second[index];
}

ResourceHandle<TextureTag> ResourceManager::GetDepthTexture(ResourceHandle<RenderTargetTag> rt)
{
    auto it = m_renderTargetDepths.find(Key(rt));
    if (it == m_renderTargetDepths.end()) return ResourceHandle<TextureTag>::Null();
    return it->second;
}

void ResourceManager::Update(ResourceHandle<BufferTag> h, const void* data, size_t sizeBytes)
{
    if (IBuffer* buffer = Get(h))
        buffer->Update(data, sizeBytes);
}

void ResourceManager::Update(ResourceHandle<ConstantBufferTag> h, const void* data, size_t sizeBytes)
{
    if (IConstantBuffer* cb = Get(h))
        cb->Update(data, sizeBytes);
}

void ResourceManager::Release(ResourceHandle<ShaderTag> h) { m_shaders.Remove(h); }
void ResourceManager::Release(ResourceHandle<TextureTag> h) { m_textures.Remove(h); }
void ResourceManager::Release(ResourceHandle<BufferTag> h)
{
    m_buffers.Remove(h);
}
void ResourceManager::Release(ResourceHandle<ConstantBufferTag> h) { m_constantBuffers.Remove(h); }
void ResourceManager::Release(ResourceHandle<PipelineStateTag> h) { m_pipelineStates.Remove(h); }
void ResourceManager::Release(ResourceHandle<RenderTargetTag> h)
{
    const uint64_t key = Key(h);
    if (auto colors = m_renderTargetColors.find(key); colors != m_renderTargetColors.end()) {
        for (ResourceHandle<TextureTag> texture : colors->second)
            Release(texture);
        m_renderTargetColors.erase(colors);
    }
    if (auto depth = m_renderTargetDepths.find(key); depth != m_renderTargetDepths.end()) {
        Release(depth->second);
        m_renderTargetDepths.erase(depth);
    }
    m_renderTargets.Remove(h);
}

uint64_t ResourceManager::Key(ResourceHandle<RenderTargetTag> h)
{
    return (static_cast<uint64_t>(h.id) << 32) | static_cast<uint64_t>(h.gen);
}

} // namespace fbzz::renderer
