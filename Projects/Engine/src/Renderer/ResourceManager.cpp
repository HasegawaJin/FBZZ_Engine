// FBZZ Engine
// ResourceManager.cpp | fbzz::renderer
// Renderer リソースの所有とハンドル解決
// IRenderer の非公開生成 API を呼び、ResourceHandle と実体を対応付ける。
// 上位システムが shared_ptr を直接保持しないための境界。
#include <Engine/Renderer/ResourceManager.hpp>
#include <cstdint>
#include <Engine/Renderer/IBuffer.hpp>
#include <Engine/Renderer/IConstantBuffer.hpp>
#include <Engine/Renderer/IPipelineState.hpp>
#include <Engine/Renderer/IRenderTarget.hpp>
#include <Engine/Renderer/IRenderer.hpp>
#include <Engine/Renderer/IShader.hpp>
#include <Engine/Renderer/ITexture.hpp>
#include <Engine/Core/Logger.hpp>
#include <algorithm>
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
    ReleaseOwnedResourcesForShutdown();
    LogLiveDebugResources();

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

    auto shader = m_renderer.CreateNativeShader(key);
    if (!shader) {
        FBZZ_LOG_ERROR("Shader load failed: %s", key.c_str());
        return ResourceHandle<ShaderTag>::Null();
    }

    ResourceHandle<ShaderTag> handle = m_shaders.Insert(std::move(shader), "Shader", __FILE__, __LINE__);
    m_shaderCache[key] = handle;
    return handle;
}

ResourceHandle<ShaderTag> ResourceManager::ReloadShader(std::string_view path)
{
    const std::string key(path);
    auto it = m_shaderCache.find(key);
    if (it == m_shaderCache.end())
        return LoadShader(path);

    auto newShader = m_renderer.CreateNativeShader(key);
    if (!newShader) {
        FBZZ_LOG_ERROR("ReloadShader failed: %s", key.c_str());
        return it->second;
    }
    m_shaders.Replace(it->second, std::move(newShader));
    return it->second;
}

void ResourceManager::ReloadAllShaders()
{
    size_t count = 0;
    for (auto& [path, handle] : m_shaderCache) {
        auto newShader = m_renderer.CreateNativeShader(path);
        if (newShader) {
            m_shaders.Replace(handle, std::move(newShader));
            ++count;
        } else {
            FBZZ_LOG_WARN("ReloadAllShaders: failed to reload %s", path.c_str());
        }
    }
    FBZZ_LOG_INFO("ResourceManager: %zu shader(s) hot-reloaded", count);
}

void ResourceManager::Reset()
{
    // WHAT: 所有リソースを全て手放し、ResourceHandle の gen を進めて旧ハンドルを無効化する。
    // WHY: デバイスロスト復帰後に旧ネイティブリソースへ触るとクラッシュするため、
    //      RenderSystem 側は GetResetVersion() の変化を検知して static handle を再作成する。
    ReleaseOwnedResourcesForShutdown();
    ++m_resetVersion;
    FBZZ_LOG_INFO("ResourceManager: reset renderer resources (version=%llu)",
                  static_cast<unsigned long long>(m_resetVersion));
}

ResourceHandle<TextureTag> ResourceManager::LoadTexture(std::string_view path)
{
    const std::string key(path);
    auto it = m_textureCache.find(key);
    if (it != m_textureCache.end()) return it->second;

    auto texture = m_renderer.CreateNativeTexture(key);
    if (!texture) {
        FBZZ_LOG_ERROR("Texture load failed: %s", key.c_str());
        return ResourceHandle<TextureTag>::Null();
    }

    ResourceHandle<TextureTag> handle = m_textures.Insert(std::move(texture), "Texture", __FILE__, __LINE__);
    m_textureCache[key] = handle;
    return handle;
}

ResourceHandle<TextureTag> ResourceManager::CreateTexture(const uint8_t* rgba, uint32_t width, uint32_t height)
{
    auto texture = m_renderer.CreateNativeTextureFromData(rgba, width, height);
    if (!texture) {
        FBZZ_LOG_ERROR("ResourceManager::CreateTexture failed (%ux%u)", width, height);
        return ResourceHandle<TextureTag>::Null();
    }
    return m_textures.Insert(std::move(texture), "TextureFromData", __FILE__, __LINE__);
}

ResourceHandle<BufferTag> ResourceManager::CreateVertexBuffer(const void* data, size_t bytes, uint32_t stride)
{
    return m_buffers.Insert(m_renderer.CreateNativeVertexBuffer(data, bytes, stride), "VertexBuffer", __FILE__, __LINE__);
}

ResourceHandle<BufferTag> ResourceManager::CreateIndexBuffer(const void* data, uint32_t count)
{
    return m_buffers.Insert(m_renderer.CreateNativeIndexBuffer(data, count), "IndexBuffer", __FILE__, __LINE__);
}

ResourceHandle<ConstantBufferTag> ResourceManager::CreateConstantBuffer(size_t sizeBytes)
{
    return m_constantBuffers.Insert(m_renderer.CreateNativeConstantBuffer(sizeBytes), "ConstantBuffer", __FILE__, __LINE__);
}

ResourceHandle<PipelineStateTag> ResourceManager::CreatePipelineState(const PipelineStateDesc& desc)
{
    return m_pipelineStates.Insert(m_renderer.CreateNativePipelineState(desc), "PipelineState", __FILE__, __LINE__);
}

ResourceHandle<RenderTargetTag> ResourceManager::CreateRenderTarget(uint32_t width, uint32_t height, uint32_t colorCount)
{
    auto rt = m_renderer.CreateNativeRenderTarget(width, height, colorCount);
    if (!rt) return ResourceHandle<RenderTargetTag>::Null();

    std::vector<ResourceHandle<TextureTag>> colors;
    colors.reserve(colorCount);
    for (uint32_t i = 0; i < colorCount; ++i)
        colors.push_back(m_textures.Insert(rt->GetColorTexture(i), "RenderTargetColorTexture", __FILE__, __LINE__));
    ResourceHandle<TextureTag> depth = m_textures.Insert(rt->GetDepthTexture(), "RenderTargetDepthTexture", __FILE__, __LINE__);

    ResourceHandle<RenderTargetTag> handle = m_renderTargets.Insert(std::move(rt), "RenderTarget", __FILE__, __LINE__);
    const uint64_t key = Key(handle);
    m_renderTargetColors[key] = std::move(colors);
    m_renderTargetDepths[key] = depth;
    return handle;
}

ResourceHandle<TextureTag> ResourceManager::CreateComputeTexture(uint32_t width, uint32_t height)
{
    return m_textures.Insert(m_renderer.CreateNativeComputeTexture(width, height), "ComputeTexture", __FILE__, __LINE__);
}

IShader* ResourceManager::Get(ResourceHandle<ShaderTag> h) { return m_shaders.Get(h); }
ITexture* ResourceManager::Get(ResourceHandle<TextureTag> h) { return m_textures.Get(h); }
IBuffer* ResourceManager::Get(ResourceHandle<BufferTag> h) { return m_buffers.Get(h); }
IConstantBuffer* ResourceManager::Get(ResourceHandle<ConstantBufferTag> h) { return m_constantBuffers.Get(h); }
IPipelineState* ResourceManager::Get(ResourceHandle<PipelineStateTag> h) { return m_pipelineStates.Get(h); }
IRenderTarget* ResourceManager::Get(ResourceHandle<RenderTargetTag> h) { return m_renderTargets.Get(h); }

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

void ResourceManager::ReleaseOwnedResourcesForShutdown()
{
    // WHY: RenderTarget は内部で color/depth texture の shared_ptr を持つため、
    //      TexturePool より先に解放しないと、RT 内部所有を外部リークと誤判定してしまう。
    m_renderTargetColors.clear();
    m_renderTargetDepths.clear();
    m_shaderCache.clear();
    m_textureCache.clear();

    m_renderTargets.ReleaseOwnedForShutdown();
    m_textures.ReleaseOwnedForShutdown();
    m_buffers.ReleaseOwnedForShutdown();
    m_constantBuffers.ReleaseOwnedForShutdown();
    m_pipelineStates.ReleaseOwnedForShutdown();
    m_shaders.ReleaseOwnedForShutdown();
}

void ResourceManager::LogLiveDebugResources() const
{
    const std::size_t liveCount = GetLiveDebugResourceCount();
    if (liveCount == 0) {
        return;
    }

    FBZZ_LOG_WARN("ResourceManager shutdown: %zu externally-held renderer resources remain", liveCount);

    const std::size_t reportCount = (std::min)(liveCount, static_cast<std::size_t>(16));
    for (std::size_t i = 0; i < reportCount; ++i) {
        const core::AllocationInfo* info = GetLiveDebugResource(i);
        if (info == nullptr) {
            continue;
        }

        FBZZ_LOG_WARN(
            "  #%llu %s %zu bytes (%s:%d)",
            static_cast<unsigned long long>(info->allocationId),
            info->allocatorName,
            info->size,
            info->file,
            info->line);
    }

    if (liveCount > reportCount) {
        FBZZ_LOG_WARN("  ... %zu more externally-held renderer resources", liveCount - reportCount);
    }
}

std::size_t ResourceManager::GetLiveDebugResourceCount() const
{
    return m_shaders.GetLiveDebugCount()
         + m_textures.GetLiveDebugCount()
         + m_buffers.GetLiveDebugCount()
         + m_constantBuffers.GetLiveDebugCount()
         + m_pipelineStates.GetLiveDebugCount()
         + m_renderTargets.GetLiveDebugCount();
}

const core::AllocationInfo* ResourceManager::GetLiveDebugResource(std::size_t index) const
{
    const std::size_t shaderCount = m_shaders.GetLiveDebugCount();
    if (index < shaderCount) return m_shaders.GetLiveDebugInfo(index);
    index -= shaderCount;

    const std::size_t textureCount = m_textures.GetLiveDebugCount();
    if (index < textureCount) return m_textures.GetLiveDebugInfo(index);
    index -= textureCount;

    const std::size_t bufferCount = m_buffers.GetLiveDebugCount();
    if (index < bufferCount) return m_buffers.GetLiveDebugInfo(index);
    index -= bufferCount;

    const std::size_t constantBufferCount = m_constantBuffers.GetLiveDebugCount();
    if (index < constantBufferCount) return m_constantBuffers.GetLiveDebugInfo(index);
    index -= constantBufferCount;

    const std::size_t pipelineStateCount = m_pipelineStates.GetLiveDebugCount();
    if (index < pipelineStateCount) return m_pipelineStates.GetLiveDebugInfo(index);
    index -= pipelineStateCount;

    return m_renderTargets.GetLiveDebugInfo(index);
}

} // namespace fbzz::renderer
