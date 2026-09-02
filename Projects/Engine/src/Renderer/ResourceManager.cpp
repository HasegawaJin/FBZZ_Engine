/// @file    ResourceManager.cpp
/// @brief   Renderer リソースの所有とハンドル解決。
/// @author  Hasegawa Jin
/// @date    2026-05-22
///
/// IRenderer の非公開生成 API を呼び、ResourceHandle と実体を対応付ける。
/// 上位システムが shared_ptr を直接保持しないための境界。
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/TexDescSerializer.hpp>
#include <cstdint>
#include <Engine/Renderer/Mesh.hpp>
#include <Engine/Renderer/IBuffer.hpp>
#include <Engine/Renderer/IConstantBuffer.hpp>
#include <Engine/Renderer/IPipelineState.hpp>
#include <Engine/Renderer/IRenderTarget.hpp>
#include <Engine/Renderer/IRenderer.hpp>
#include <Engine/Renderer/IShader.hpp>
#include <Engine/Renderer/IStructuredBuffer.hpp>
#include <Engine/Renderer/ITexture.hpp>
#include <Engine/Core/Logger.hpp>
#include <algorithm>
#include <cassert>

namespace fbzz::renderer {

namespace {
ResourceManager* s_activeResourceManager = nullptr;

// Windows の '\\' とアセット記述で使う '/' を同一キーにし、同じ実ファイルの二重キャッシュを防ぐ。
std::string TextureCacheKey(std::string_view path)
{
    std::string key;
    std::string spriteName;
    // Sprite参照はGPU上では親Textureを共有する。サブアセット名をキャッシュキーへ
    // 含めると同じ画像を重複ロードするため、ここで親パスへ正規化する。
    (void)asset::ParseSpriteReference(path, key, spriteName);
    std::replace(key.begin(), key.end(), '\\', '/');
    return key;
}

// テクスチャが占めるバイト数の概算。
//
// WHY 概算で足りるか: ITexture が公開するのは寸法だけで、フォーマットもミップ数も
//     バックエンドの内側にある。Analysis パネルが答えたいのは «どの用途が増え続けて
//     いるか» であって GPU の実測値ではないので、RGBA8 換算の桁が合っていれば読める。
//     ブロック圧縮の DDS は過大に、HDR/ミップ付きは過小に出る点だけは承知して使うこと。
std::size_t EstimateTextureBytes(const ITexture& texture, std::size_t bytesPerTexel = 4)
{
    return static_cast<std::size_t>(texture.GetWidth())
         * static_cast<std::size_t>(texture.GetHeight())
         * static_cast<std::size_t>(texture.GetDepth())
         * bytesPerTexel;
}
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
    // WHY: LoadTexture と同様にパス区切りを統一し、大文字小文字の違いによる
    //      同一シェーダーの二重ロードを防ぐ。DX11Shader 内部も同様に正規化する。
    std::string key(path);
    std::replace(key.begin(), key.end(), '\\', '/');
    auto it = m_shaderCache.find(key);
    if (it != m_shaderCache.end()) return it->second;

    auto shader = m_renderer.CreateNativeShader(key);
    if (!shader) {
        FBZZ_LOG_ERROR("Shader load failed: %s", key.c_str());
        return ResourceHandle<ShaderTag>::Null();
    }

    // WHY 0 か: シェーダーバイトコードの実サイズはバックエンドの内側にあり、
    //          ITexture / IBuffer のように寸法から復元することもできない。
    ResourceHandle<ShaderTag> handle = m_shaders.Insert(std::move(shader), 0, "Shader", __FILE__, __LINE__);
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
    // 追跡していたハンドルも一緒に無効になる。返しにいくと «リセット後に作られた
    // 別の実体» を巻き添えにするので、控えだけ捨てる。
    m_trackedMeshes.clear();
    ++m_resetVersion;
    FBZZ_LOG_INFO("ResourceManager: reset renderer resources (version=%llu)",
                  static_cast<unsigned long long>(m_resetVersion));
}

ResourceHandle<TextureTag> ResourceManager::LoadTexture(std::string_view path)
{
    const std::string key = TextureCacheKey(path);
    auto it = m_textureCache.find(key);
    if (it != m_textureCache.end()) return it->second;

    // ".meta" サイドカー表記と生画像パスを同じ公開 API で扱う (ResolveSourcePath が元画像へ解決)。
    // WHY: .mat / Scene は Assets/ 起点の相対パスを保存するが、DX11Texture は実ファイルパスを要求する。
    //      ResourceManager が AssetManager と同じ解決規則を通すことで、呼び出し側ごとの cwd 依存をなくす。
    std::string sourcePath;
    const std::string resolvedPath = asset::AssetManager::ResolveAssetPath(key);
    if (!asset::TexDescSerializer::ResolveSourcePath(resolvedPath, sourcePath)) {
        FBZZ_LOG_ERROR("Texture path resolution failed: %s", key.c_str());
        return ResourceHandle<TextureTag>::Null();
    }
    auto texture = m_renderer.CreateNativeTexture(sourcePath);
    if (!texture) {
        FBZZ_LOG_ERROR("Texture load failed: %s", key.c_str());
        return ResourceHandle<TextureTag>::Null();
    }

    const std::size_t textureBytes = EstimateTextureBytes(*texture);
    ResourceHandle<TextureTag> handle =
        m_textures.Insert(std::move(texture), textureBytes, "Texture", __FILE__, __LINE__);
    m_textureCache[key] = handle;
    return handle;
}

ResourceHandle<TextureTag> ResourceManager::ReloadTexture(std::string_view path)
{
    const std::string key = TextureCacheKey(path);
    auto it = m_textureCache.find(key);
    if (it == m_textureCache.end())
        return LoadTexture(path);

    std::string sourcePath;
    const std::string resolvedPath = asset::AssetManager::ResolveAssetPath(key);
    if (!asset::TexDescSerializer::ResolveSourcePath(resolvedPath, sourcePath)) {
        FBZZ_LOG_ERROR("ReloadTexture path resolution failed: %s", key.c_str());
        return it->second;
    }
    auto newTexture = m_renderer.CreateNativeTexture(sourcePath);
    if (!newTexture) {
        FBZZ_LOG_ERROR("ReloadTexture failed: %s", key.c_str());
        return it->second;
    }

    // ResourcePool のスロットを置換し、RenderSystem や Material が保持するハンドルを有効なまま保つ。
    const std::size_t reloadedBytes = EstimateTextureBytes(*newTexture);
    m_textures.Replace(it->second, std::move(newTexture), reloadedBytes);
    return it->second;
}

std::size_t ResourceManager::EvictTexture(std::string_view path)
{
    const std::string key = TextureCacheKey(path);
    if (key.empty()) return 0;

    // フォルダを渡された場合に配下ごと外す。末尾に '/' を付けて前方一致させることで、
    // "Assets/UI/Title" が "Assets/UI/TitleOld/..." を巻き込まないようにする。
    const std::string prefix = key + "/";

    std::size_t evicted = 0;
    for (auto it = m_textureCache.begin(); it != m_textureCache.end(); ) {
        if (it->first == key || it->first.rfind(prefix, 0) == 0) {
            m_textures.Remove(it->second);
            it = m_textureCache.erase(it);
            ++evicted;
        } else {
            ++it;
        }
    }
    return evicted;
}

ResourceHandle<TextureTag> ResourceManager::CreateTexture(const uint8_t* rgba, uint32_t width, uint32_t height)
{
    auto texture = m_renderer.CreateNativeTextureFromData(rgba, width, height);
    if (!texture) {
        FBZZ_LOG_ERROR("ResourceManager::CreateTexture failed (%ux%u)", width, height);
        return ResourceHandle<TextureTag>::Null();
    }
    // WHY サイズを先に控えるか: 引数の評価順は未規定で、std::move した後に
    //     *texture を読むと空のポインタを参照しうる。
    const std::size_t bytes = EstimateTextureBytes(*texture);
    return m_textures.Insert(std::move(texture), bytes, "TextureFromData", __FILE__, __LINE__);
}

ResourceHandle<TextureTag> ResourceManager::CreateTexture3D(
    const uint8_t* rgba, uint32_t width, uint32_t height, uint32_t depth)
{
    auto texture = m_renderer.CreateNativeTexture3DFromData(rgba, width, height, depth);
    if (!texture) {
        FBZZ_LOG_ERROR("ResourceManager::CreateTexture3D failed (%ux%ux%u)", width, height, depth);
        return ResourceHandle<TextureTag>::Null();
    }
    const std::size_t bytes = EstimateTextureBytes(*texture);
    return m_textures.Insert(std::move(texture), bytes, "Texture3DFromData", __FILE__, __LINE__);
}

ResourceHandle<BufferTag> ResourceManager::CreateVertexBuffer(const void* data, size_t bytes, uint32_t stride)
{
    return m_buffers.Insert(m_renderer.CreateNativeVertexBuffer(data, bytes, stride), bytes,
                            "VertexBuffer", __FILE__, __LINE__);
}

ResourceHandle<BufferTag> ResourceManager::CreateGpuWritableVertexBuffer(size_t bytes, uint32_t stride)
{
    return m_buffers.Insert(m_renderer.CreateNativeGpuWritableVertexBuffer(bytes, stride), bytes,
                            "GpuWritableVertexBuffer", __FILE__, __LINE__);
}

ResourceHandle<BufferTag> ResourceManager::CreateIndexBuffer(const void* data, uint32_t count)
{
    return m_buffers.Insert(m_renderer.CreateNativeIndexBuffer(data, count),
                            static_cast<std::size_t>(count) * sizeof(uint32_t),
                            "IndexBuffer", __FILE__, __LINE__);
}

ResourceHandle<ConstantBufferTag> ResourceManager::CreateConstantBuffer(size_t sizeBytes)
{
    return m_constantBuffers.Insert(m_renderer.CreateNativeConstantBuffer(sizeBytes), sizeBytes,
                                    "ConstantBuffer", __FILE__, __LINE__);
}

ResourceHandle<PipelineStateTag> ResourceManager::CreatePipelineState(const PipelineStateDesc& desc)
{
    // PSO は状態の束で、専有メモリと呼べる実体を持たない。
    return m_pipelineStates.Insert(m_renderer.CreateNativePipelineState(desc), 0,
                                   "PipelineState", __FILE__, __LINE__);
}

ResourceHandle<RenderTargetTag> ResourceManager::CreateRenderTarget(uint32_t width, uint32_t height, uint32_t colorCount)
{
    auto rt = m_renderer.CreateNativeRenderTarget(width, height, colorCount);
    if (!rt) return ResourceHandle<RenderTargetTag>::Null();

    std::vector<ResourceHandle<TextureTag>> colors;
    colors.reserve(colorCount);
    for (uint32_t i = 0; i < colorCount; ++i) {
        auto colorTexture = m_renderer.CreateNativeTextureFromRenderTarget(
            *rt,
            i,
            RenderTargetTextureKind::Color);
        const std::size_t colorBytes = colorTexture ? EstimateTextureBytes(*colorTexture) : 0;
        colors.push_back(m_textures.Insert(std::move(colorTexture), colorBytes,
                                           "RenderTargetColorTexture", __FILE__, __LINE__));
    }

    auto depthTexture = m_renderer.CreateNativeTextureFromRenderTarget(
        *rt,
        0,
        RenderTargetTextureKind::Depth);
    const std::size_t depthBytes = depthTexture ? EstimateTextureBytes(*depthTexture) : 0;
    ResourceHandle<TextureTag> depth =
        m_textures.Insert(std::move(depthTexture), depthBytes,
                          "RenderTargetDepthTexture", __FILE__, __LINE__);

    // WHY 0 か: RT のメモリは上で登録した色 / 深度テクスチャ側に計上済み。
    //          ここでも数えると同じ実体を二重に積む。
    ResourceHandle<RenderTargetTag> handle =
        m_renderTargets.Insert(std::move(rt), 0, "RenderTarget", __FILE__, __LINE__);
    const uint64_t key = Key(handle);
    m_renderTargetColors[key] = std::move(colors);
    m_renderTargetDepths[key] = depth;
    return handle;
}

ResourceHandle<RenderTargetTag> ResourceManager::CreateCubemapRenderTarget(uint32_t size, uint32_t mipCount)
{
    auto rt = m_renderer.CreateNativeCubemapRenderTarget(size, mipCount);
    if (!rt) return ResourceHandle<RenderTargetTag>::Null();

    // TextureCube SRV を 1 つの "カラーテクスチャ" として登録する。
    // WHY: 既存の m_renderTargetColors 経路に乗せることで、Release()/シャットダウン時の
    //      解放処理を通常 RT と共有できる (キューブ専用のクリーンアップを書かずに済む)。
    //      深度バッファは持たないため m_renderTargetDepths には登録しない。
    std::vector<ResourceHandle<TextureTag>> colors;
    if (auto cubeTex = m_renderer.CreateNativeCubeTextureFromRenderTarget(*rt)) {
        // 6 面ぶん。GetWidth/GetHeight は 1 面の寸法しか返さない。
        const std::size_t cubeBytes = EstimateTextureBytes(*cubeTex) * 6u;
        colors.push_back(m_textures.Insert(std::move(cubeTex), cubeBytes,
                                           "CubemapRenderTargetTexture", __FILE__, __LINE__));
    }

    ResourceHandle<RenderTargetTag> handle =
        m_renderTargets.Insert(std::move(rt), 0, "CubemapRenderTarget", __FILE__, __LINE__);
    m_renderTargetColors[Key(handle)] = std::move(colors);
    return handle;
}

ResourceHandle<TextureTag> ResourceManager::GetCubemapTexture(ResourceHandle<RenderTargetTag> rt)
{
    // キューブ SRV は index 0 のカラーテクスチャとして登録してある。
    return GetColorTexture(rt, 0);
}

ResourceHandle<TextureTag> ResourceManager::CreateComputeTexture(uint32_t width, uint32_t height)
{
    return m_textures.Insert(m_renderer.CreateNativeComputeTexture(width, height),
                             static_cast<std::size_t>(width) * height * 8u, // RGBA16F
                             "ComputeTexture", __FILE__, __LINE__);
}

ResourceHandle<TextureTag> ResourceManager::CreateComputeTexture3D(
    uint32_t width, uint32_t height, uint32_t depth)
{
    auto texture = m_renderer.CreateNativeComputeTexture3D(width, height, depth);
    if (!texture) {
        FBZZ_LOG_WARN("CreateComputeTexture3D: backend does not support 3D compute textures (%ux%ux%u)",
                      width, height, depth);
        return ResourceHandle<TextureTag>::Null();
    }
    const std::size_t bytes = EstimateTextureBytes(*texture, 8u); // RGBA16F
    return m_textures.Insert(std::move(texture), bytes, "ComputeTexture3D", __FILE__, __LINE__);
}

ResourceHandle<TextureTag> ResourceManager::CreateDynamicTexture(
    uint32_t width, uint32_t height, DynamicTextureFormat format)
{
    auto texture = m_renderer.CreateNativeDynamicTexture(width, height, format);
    if (!texture) {
        // 未対応バックエンドでは nullptr が返る。呼び出し側は Null ハンドルで縮退を判断する。
        FBZZ_LOG_WARN("CreateDynamicTexture: backend does not support dynamic textures (%ux%u)",
                      width, height);
        return ResourceHandle<TextureTag>::Null();
    }
    const std::size_t bytes =
        EstimateTextureBytes(*texture, format == DynamicTextureFormat::R8 ? 1u : 4u);
    return m_textures.Insert(std::move(texture), bytes, "DynamicTexture", __FILE__, __LINE__);
}

ResourceHandle<TextureTag> ResourceManager::RegisterTexture(std::unique_ptr<ITexture> texture)
{
    if (!texture) return ResourceHandle<TextureTag>::Null();
    const std::size_t bytes = EstimateTextureBytes(*texture);
    return m_textures.Insert(std::move(texture), bytes, "AdoptedTexture", __FILE__, __LINE__);
}

ResourceHandle<StructuredBufferTag> ResourceManager::CreateStructuredBuffer(const void* data, uint32_t elementCount, uint32_t stride)
{
    auto sb = m_renderer.CreateNativeStructuredBuffer(data, elementCount, stride);
    if (!sb) {
        FBZZ_LOG_ERROR("ResourceManager::CreateStructuredBuffer failed (count=%u stride=%u)", elementCount, stride);
        return ResourceHandle<StructuredBufferTag>::Null();
    }
    return m_structuredBuffers.Insert(std::move(sb), static_cast<std::size_t>(elementCount) * stride,
                                      "StructuredBuffer", __FILE__, __LINE__);
}

ResourceHandle<StructuredBufferTag> ResourceManager::CreateGpuLocalStructuredBuffer(
    const void* data, uint32_t elementCount, uint32_t stride)
{
    auto sb = m_renderer.CreateNativeGpuLocalStructuredBuffer(data, elementCount, stride);
    if (!sb) {
        FBZZ_LOG_ERROR("ResourceManager::CreateGpuLocalStructuredBuffer failed (count=%u stride=%u)",
                       elementCount, stride);
        return ResourceHandle<StructuredBufferTag>::Null();
    }
    return m_structuredBuffers.Insert(std::move(sb), static_cast<std::size_t>(elementCount) * stride,
                                      "GpuLocalStructuredBuffer", __FILE__, __LINE__);
}

ResourceHandle<StructuredBufferTag> ResourceManager::CreateRWStructuredBuffer(const void* data, uint32_t elementCount, uint32_t stride)
{
    auto sb = m_renderer.CreateNativeRWStructuredBuffer(data, elementCount, stride);
    if (!sb) {
        FBZZ_LOG_ERROR("ResourceManager::CreateRWStructuredBuffer failed (count=%u stride=%u)", elementCount, stride);
        return ResourceHandle<StructuredBufferTag>::Null();
    }
    return m_structuredBuffers.Insert(std::move(sb), static_cast<std::size_t>(elementCount) * stride,
                                      "RWStructuredBuffer", __FILE__, __LINE__);
}

IShader* ResourceManager::Get(ResourceHandle<ShaderTag> h) { return m_shaders.Get(h); }
ITexture* ResourceManager::Get(ResourceHandle<TextureTag> h) { return m_textures.Get(h); }
IBuffer* ResourceManager::Get(ResourceHandle<BufferTag> h) { return m_buffers.Get(h); }
IConstantBuffer* ResourceManager::Get(ResourceHandle<ConstantBufferTag> h) { return m_constantBuffers.Get(h); }
IPipelineState* ResourceManager::Get(ResourceHandle<PipelineStateTag> h) { return m_pipelineStates.Get(h); }
IRenderTarget* ResourceManager::Get(ResourceHandle<RenderTargetTag> h) { return m_renderTargets.Get(h); }
IStructuredBuffer* ResourceManager::Get(ResourceHandle<StructuredBufferTag> h) { return m_structuredBuffers.Get(h); }

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

void ResourceManager::Update(ResourceHandle<StructuredBufferTag> h, const void* data, size_t sizeBytes)
{
    if (IStructuredBuffer* sb = Get(h))
        sb->Update(data, sizeBytes);
}

void ResourceManager::Release(ResourceHandle<ShaderTag> h) { m_shaders.Remove(h); }
void ResourceManager::Release(ResourceHandle<TextureTag> h) { m_textures.Remove(h); }
void ResourceManager::Release(ResourceHandle<BufferTag> h)
{
    m_buffers.Remove(h);
}
void ResourceManager::Release(ResourceHandle<ConstantBufferTag> h) { m_constantBuffers.Remove(h); }
void ResourceManager::Release(ResourceHandle<PipelineStateTag> h) { m_pipelineStates.Remove(h); }
void ResourceManager::Release(ResourceHandle<StructuredBufferTag> h) { m_structuredBuffers.Remove(h); }
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
    // WHY: RenderTarget 由来の TextureTag は RT の SRV を参照するラッパーなので、
    //      マップを先に消して shutdown 時の重複解放経路を断つ。
    m_renderTargetColors.clear();
    m_renderTargetDepths.clear();
    m_shaderCache.clear();
    m_textureCache.clear();

    m_renderTargets.ReleaseOwnedForShutdown();
    m_textures.ReleaseOwnedForShutdown();
    m_buffers.ReleaseOwnedForShutdown();
    m_constantBuffers.ReleaseOwnedForShutdown();
    m_pipelineStates.ReleaseOwnedForShutdown();
    m_structuredBuffers.ReleaseOwnedForShutdown();
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

// ── Mesh のバッファの寿命 ─────────────────────────────────────────────────────

// WHY ReleaseMeshBuffers(Mesh&) と分かれているか: こちらが呼ばれるのは «Mesh の実体が
// もう無い» ときで、Mesh を逆参照できない。控えておいたハンドルだけを頼りに返す。
std::size_t ResourceManager::ReleaseTrackedMeshBuffers(TrackedMesh& tracked)
{
    std::size_t released = 0;
    if (tracked.vertexBuffer.IsValid()) { Release(tracked.vertexBuffer); ++released; }
    if (tracked.indexBuffer.IsValid())  { Release(tracked.indexBuffer);  ++released; }
    tracked.vertexBuffer = {};
    tracked.indexBuffer  = {};
    return released;
}

void ResourceManager::TrackMeshBuffers(const std::shared_ptr<Mesh>& mesh)
{
    if (!mesh) return;

    for (TrackedMesh& tracked : m_trackedMeshes) {
        if (tracked.key != mesh.get()) continue;

        // WHY 期限切れなら先に返すか: 同じ番地に別の Mesh が生まれることがある。
        //     そのまま上書きすると、前の Mesh のバッファを返す機会が永久に失われる。
        if (tracked.mesh.expired()) ReleaseTrackedMeshBuffers(tracked);

        tracked.mesh         = mesh;
        tracked.vertexBuffer = mesh->vertexBuffer;
        tracked.indexBuffer  = mesh->indexBuffer;
        return;
    }

    m_trackedMeshes.push_back(
        TrackedMesh{ mesh.get(), mesh, mesh->vertexBuffer, mesh->indexBuffer });
}

std::size_t ResourceManager::ReleaseMeshBuffers(Mesh& mesh)
{
    std::size_t released = 0;
    if (mesh.vertexBuffer.IsValid()) { Release(mesh.vertexBuffer); ++released; }
    if (mesh.indexBuffer.IsValid())  { Release(mesh.indexBuffer);  ++released; }
    mesh.vertexBuffer = {};
    mesh.indexBuffer  = {};
    return released;
}

std::size_t ResourceManager::SweepOrphanedMeshBuffers()
{
    std::size_t released = 0;
    for (std::size_t i = 0; i < m_trackedMeshes.size();) {
        if (!m_trackedMeshes[i].mesh.expired()) {
            ++i;
            continue;
        }
        released += ReleaseTrackedMeshBuffers(m_trackedMeshes[i]);
        // 順序に意味は無いので、末尾と入れ替えて縮める。
        m_trackedMeshes[i] = m_trackedMeshes.back();
        m_trackedMeshes.pop_back();
    }
    return released;
}

std::size_t ResourceManager::GetLiveDebugResourceCount() const
{
    return m_shaders.GetLiveDebugCount()
         + m_textures.GetLiveDebugCount()
         + m_buffers.GetLiveDebugCount()
         + m_constantBuffers.GetLiveDebugCount()
         + m_pipelineStates.GetLiveDebugCount()
         + m_renderTargets.GetLiveDebugCount()
         + m_structuredBuffers.GetLiveDebugCount();
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

    const std::size_t renderTargetCount = m_renderTargets.GetLiveDebugCount();
    if (index < renderTargetCount) return m_renderTargets.GetLiveDebugInfo(index);
    index -= renderTargetCount;

    return m_structuredBuffers.GetLiveDebugInfo(index);
}

bool SizedRenderTarget::Ensure(ResourceManager& resources,
                               uint32_t width, uint32_t height, uint32_t colorCount)
{
    const uint64_t resetVersion = resources.GetResetVersion();
    if (m_resetVersion != resetVersion) {
        // WHY 返さずに捨てるか: リセットではマネージャーが実体ごと畳んでいる。
        //     こちらのハンドルは無効で、返しにいっても意味が無い (最悪、同じスロットへ
        //     入ってきた別の実体を巻き添えにする)。控えだけ捨てて作り直す。
        m_resetVersion = resetVersion;
        m_handle       = {};
        m_width = m_height = m_colorCount = 0;
    }

    if (m_handle.IsValid() && m_width == width && m_height == height && m_colorCount == colorCount)
        return false;

    if (m_handle.IsValid()) resources.Release(m_handle);
    m_handle     = resources.CreateRenderTarget(width, height, colorCount);
    m_width      = width;
    m_height     = height;
    m_colorCount = colorCount;
    return true;
}

void SizedRenderTarget::Release(ResourceManager& resources)
{
    if (m_handle.IsValid() && m_resetVersion == resources.GetResetVersion())
        resources.Release(m_handle);
    m_handle = {};
    m_width = m_height = m_colorCount = 0;
}

} // namespace fbzz::renderer
