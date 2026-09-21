/// @file    ResourceManager.cpp
/// @brief   Renderer リソースの所有とハンドル解決。
/// @author  Hasegawa Jin
/// @date    2026-05-22
/// @note IRenderer の非公開生成 API を呼び、ResourceHandle と実体を対応付ける。
/// @note 実体の所有はここ 1 か所に集約し、上位システムはハンドルだけを持つ。
#include <Graphics/Renderer/ResourceManager.hpp>
#include <Graphics/Pipeline/RenderResources.hpp>
#include <Graphics/Renderer/AssetPathService.hpp>
#include <cstdint>
#include <Graphics/Renderer/Mesh.hpp>
#include <Graphics/Renderer/IBuffer.hpp>
#include <Graphics/Renderer/IConstantBuffer.hpp>
#include <Graphics/Renderer/IPipelineState.hpp>
#include <Graphics/Renderer/IRenderTarget.hpp>
#include <Graphics/Renderer/IRenderer.hpp>
#include <Graphics/Renderer/IShader.hpp>
#include <Graphics/Renderer/IStructuredBuffer.hpp>
#include <Graphics/Renderer/ITexture.hpp>
#include <Core/Logger.hpp>

#include <algorithm>
#include <cassert>

namespace fbzz::renderer {

namespace {
ResourceManager* s_activeResourceManager = nullptr;

/// @note Windows の '\\' とアセット記述で使う '/' を同一キーにし、同じ実ファイルの二重キャッシュを防ぐ。
std::string TextureCacheKey(std::string_view path)
{
    /// @note Sprite参照はGPU上では親Textureを共有する。サブアセット名をキャッシュキーへ
    /// @note       含めると同じ画像を重複ロードするため、ここで親パスへ正規化する。
    std::string key = NormalizeTextureKey(path);
    std::replace(key.begin(), key.end(), '\\', '/');
    return key;
}

/// @note テクスチャが占めるバイト数の概算。
/// @note ITexture が公開するのは寸法だけでフォーマット/ミップ数はバックエンドの内側にあるため、
/// @note       RGBA8 換算で近似する。Analysis パネルが知りたいのは «どの用途が増え続けているか» で
/// @note       GPU 実測値ではない。ブロック圧縮 DDS は過大に、HDR/ミップ付きは過小に出る。
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
    /// @note 解放より先に数える理由: ReleaseOwnedResourcesForShutdown() は台帳ごと畳むため、
    /// @note       後で数えると常に 0 になる (以前はここが逆で、報告が出ることは無かった)。
    LogLiveDebugResources();
    ReleaseOwnedResourcesForShutdown();

    if (s_activeResourceManager == this)
        s_activeResourceManager = nullptr;
}

RenderResources& ResourceManager::Rendering()
{
    if (!m_renderResources) m_renderResources = std::make_unique<RenderResources>(*this);
    return *m_renderResources;
}

void ResourceManager::ReleaseRenderView(uint32_t key)
{
    if (m_renderResources) m_renderResources->ReleaseView(key);
}

ResourceManager* ResourceManager::Active()
{
    return s_activeResourceManager;
}

ResourceHandle<ShaderTag> ResourceManager::LoadShader(std::string_view path)
{
    /// @note LoadTexture と同様にパス区切りを統一し、大文字小文字の違いによる同一シェーダーの
    /// @note       二重ロードを防ぐ。バックエンドの IShader 内部も同様に正規化する。
    std::string key(path);
    std::replace(key.begin(), key.end(), '\\', '/');
    auto it = m_shaderCache.find(key);
    if (it != m_shaderCache.end()) return it->second;

    auto shader = m_renderer.CreateNativeShader(key);
    if (!shader) {
        FBZZ_LOG_ERROR("Shader load failed: %s", key.c_str());
        return ResourceHandle<ShaderTag>::Null();
    }

    /// @note 0 バイトとして登録: シェーダーバイトコードの実サイズはバックエンドの内側にあり、
    /// @note       ITexture/IBuffer のように寸法から復元することもできない。
    ResourceHandle<ShaderTag> handle = m_shaders.Insert(std::move(shader), 0, "Shader", __FILE__, __LINE__);
    m_shaderCache[key] = handle;
    return handle;
}

ResourceHandle<ShaderTag> ResourceManager::ReloadShader(std::string_view path)
{
    std::string key(path);
    std::replace(key.begin(), key.end(), '\\', '/');
    auto it = m_shaderCache.find(key);
    if (it == m_shaderCache.end())
        return LoadShader(path);

    auto newShader = m_renderer.CreateNativeShader(key);
    if (!newShader) {
        FBZZ_LOG_ERROR("ReloadShader failed: %s", key.c_str());
        return it->second;
    }
    if (!m_renderer.PrepareShaderReload()) {
        FBZZ_LOG_WARN("ReloadShader: renderer rejected reload of %s", key.c_str());
        return it->second;
    }
    m_shaders.Replace(it->second, std::move(newShader));
    ++m_shaderVersion;
    return it->second;
}

bool ResourceManager::ReloadAllShaders()
{
    std::vector<std::pair<ResourceHandle<ShaderTag>, std::unique_ptr<IShader>>> pending;
    pending.reserve(m_shaderCache.size());
    for (const auto& [path, handle] : m_shaderCache) {
        auto newShader = m_renderer.CreateNativeShader(path);
        if (!newShader) {
            FBZZ_LOG_WARN("ReloadAllShaders: failed to reload %s; keeping all previous shaders", path.c_str());
            return false;
        }
        pending.emplace_back(handle, std::move(newShader));
    }
    if (pending.empty()) return true;
    if (!m_renderer.PrepareShaderReload()) {
        FBZZ_LOG_WARN("ReloadAllShaders: renderer rejected reload; keeping all previous shaders");
        return false;
    }
    for (auto& [handle, shader] : pending)
        m_shaders.Replace(handle, std::move(shader));
    ++m_shaderVersion;
    FBZZ_LOG_INFO("ResourceManager: %zu shader(s) hot-reloaded", pending.size());
    return true;
}

void ResourceManager::Reset()
{
    /// @note 所有リソースを全て手放し、ResourceHandle の gen を進めて旧ハンドルを無効化する。
    /// @note 描画状態も破棄し、旧デバイスのハンドルやビュー履歴を次の描画へ持ち越さない。
    ReleaseOwnedResourcesForShutdown();
    ++m_resetVersion;
    FBZZ_LOG_INFO("ResourceManager: reset renderer resources (version=%llu)",
                  static_cast<unsigned long long>(m_resetVersion));
}

ResourceHandle<TextureTag> ResourceManager::LoadTexture(std::string_view path)
{
    return LoadTextureImpl(path, true);
}

ResourceHandle<TextureTag> ResourceManager::LoadTextureUnpinned(std::string_view path)
{
    return LoadTextureImpl(path, false);
}

ResourceHandle<TextureTag> ResourceManager::LoadTextureImpl(std::string_view path, bool pin)
{
    const std::string key = TextureCacheKey(path);
    /// @note 配った先がハンドルを持ち続けるかもしれない。一度でも配ったキーは非同期側から外さない。
    if (pin) m_texturesHandedOut.insert(key);
    auto it = m_textureCache.find(key);
    if (it != m_textureCache.end()) return it->second;

    /// @note `.meta` サイドカー表記と生画像パスを同じ公開 API で扱う (ResolveTextureSource が
    /// @note       元画像へ解決)。.mat/Scene は Assets/ 起点の相対パスを保存するがテクスチャ実装は
    /// @note       実ファイルパスを要求するため、AssetManager と同じ解決規則を通して cwd 依存をなくす。
    std::string sourcePath;
    if (!ResolveTextureSource(ResolveAssetPath(key), sourcePath)) {
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
    if (!ResolveTextureSource(ResolveAssetPath(key), sourcePath)) {
        FBZZ_LOG_ERROR("ReloadTexture path resolution failed: %s", key.c_str());
        return it->second;
    }
    auto newTexture = m_renderer.CreateNativeTexture(sourcePath);
    if (!newTexture) {
        FBZZ_LOG_ERROR("ReloadTexture failed: %s", key.c_str());
        return it->second;
    }

    /// @note ResourcePool のスロットを置換し、RenderSystem や Material が保持するハンドルを有効なまま保つ。
    const std::size_t reloadedBytes = EstimateTextureBytes(*newTexture);
    m_textures.Replace(it->second, std::move(newTexture), reloadedBytes);
    return it->second;
}

std::size_t ResourceManager::EvictTexture(std::string_view path)
{
    const std::string key = TextureCacheKey(path);
    if (key.empty()) return 0;

    /// @note フォルダを渡された場合に配下ごと外す。末尾に '/' を付けて前方一致させることで、
    /// @note       "Assets/UI/Title" が "Assets/UI/TitleOld/..." を巻き込まないようにする。
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

ResourceHandle<TextureTag> ResourceManager::CreateTexture(const uint8_t* rgba, uint32_t width, uint32_t height,
                                                          Where where)
{
    auto texture = m_renderer.CreateNativeTextureFromData(rgba, width, height);
    if (!texture) {
        FBZZ_LOG_ERROR("ResourceManager::CreateTexture failed (%ux%u)", width, height);
        return ResourceHandle<TextureTag>::Null();
    }
    /// @note サイズを先に控える理由: 引数の評価順は未規定で、std::move した後に
    /// @note       *texture を読むと空のポインタを参照しうる。
    const std::size_t bytes = EstimateTextureBytes(*texture);
    return m_textures.Insert(std::move(texture), bytes, "TextureFromData", where.file_name(), static_cast<int>(where.line()));
}

ResourceHandle<TextureTag> ResourceManager::CreateTextureWithMips(
    const TextureMipData* mips, uint32_t mipCount, Where where)
{
    auto texture = m_renderer.CreateNativeTextureFromDataMips(mips, mipCount);
    if (!texture) {
        FBZZ_LOG_ERROR("ResourceManager::CreateTextureWithMips failed (%u 段)", mipCount);
        return ResourceHandle<TextureTag>::Null();
    }
    const std::size_t bytes = EstimateTextureBytes(*texture);
    return m_textures.Insert(std::move(texture), bytes, "TextureFromDataMips", where.file_name(), static_cast<int>(where.line()));
}

ResourceHandle<TextureTag> ResourceManager::BeginTextureUpload(
    const TextureMipData* mips, uint32_t mipCount, uint64_t& outUploadToken, Where where)
{
    outUploadToken = 0;
    auto texture = m_renderer.CreateNativeTextureFromDataMipsAsync(mips, mipCount, outUploadToken);
    if (!texture) {
        FBZZ_LOG_ERROR("ResourceManager::BeginTextureUpload failed (%u 段)", mipCount);
        outUploadToken = 0;
        return ResourceHandle<TextureTag>::Null();
    }
    const std::size_t bytes = EstimateTextureBytes(*texture);
    return m_textures.Insert(std::move(texture), bytes, "TextureStreamed", where.file_name(), static_cast<int>(where.line()));
}

bool ResourceManager::IsUploadComplete(uint64_t uploadToken) const
{
    return uploadToken == 0 || m_renderer.IsUploadComplete(uploadToken);
}

ResourceHandle<TextureTag> ResourceManager::FindCachedTexture(std::string_view path) const
{
    const auto it = m_textureCache.find(TextureCacheKey(path));
    return it != m_textureCache.end() ? it->second : ResourceHandle<TextureTag>::Null();
}

ResourceHandle<TextureTag> ResourceManager::PublishTexture(std::string_view path, ResourceHandle<TextureTag> uploaded)
{
    const std::string key = TextureCacheKey(path);
    const auto it = m_textureCache.find(key);
    /// @note 転送中に同期 LoadTexture が同じ画像を読んでいれば、そちらが既に配られている。配られた方を正とする。
    if (it != m_textureCache.end() && Get(it->second) != nullptr) {
        if (uploaded != it->second) Release(uploaded);
        return it->second;
    }
    if (Get(uploaded) == nullptr) return ResourceHandle<TextureTag>::Null();
    m_textureCache[key] = uploaded;
    return uploaded;
}

bool ResourceManager::ReplaceTextureContents(ResourceHandle<TextureTag> target, ResourceHandle<TextureTag> uploaded)
{
    if (target == uploaded || Get(target) == nullptr || Get(uploaded) == nullptr) return false;
    std::unique_ptr<ITexture> texture = m_textures.Take(uploaded);
    const std::size_t bytes = EstimateTextureBytes(*texture);
    /// @note 旧実体はここで破棄される。DX12Texture のデストラクタが資源と bindless 枠をフェンス付きで返すので、
    /// @note       このフレームまでに記録した描画は旧実体を読み終えてから回収される。新しい実体の枠は次に
    /// @note       添字を引いたときに新しく割り当たる (公開中の枠を上書きしない)。
    /// @see https://learn.microsoft.com/en-us/windows/win32/direct3d12/fence-based-resource-management Fence-Based Resource Management
    m_textures.Replace(target, std::move(texture), bytes);
    return true;
}

bool ResourceManager::EvictStreamedTexture(std::string_view path)
{
    const std::string key = TextureCacheKey(path);
    if (m_texturesHandedOut.count(key) != 0) return false;
    const auto it = m_textureCache.find(key);
    if (it == m_textureCache.end()) return true;
    m_textures.Remove(it->second);
    m_textureCache.erase(it);
    return true;
}

std::size_t ResourceManager::GetTextureBytes(ResourceHandle<TextureTag> handle) const
{
    const ITexture* texture = m_textures.Get(handle);
    return texture ? EstimateTextureBytes(*texture) : 0;
}

ResourceHandle<TextureTag> ResourceManager::GetWhiteTexture()
{
    /// @note Reset() はスロットの世代を進めるため、控えたハンドルの生死で作り直しを判断する。
    if (Get(m_whiteTexture) == nullptr) {
        static constexpr uint8_t kWhite[4] = { 255, 255, 255, 255 };
        m_whiteTexture = CreateTexture(kWhite, 1, 1);
    }
    return m_whiteTexture;
}

ResourceHandle<TextureTag> ResourceManager::CreateTexture3D(
    const uint8_t* rgba, uint32_t width, uint32_t height, uint32_t depth, Where where)
{
    auto texture = m_renderer.CreateNativeTexture3DFromData(rgba, width, height, depth);
    if (!texture) {
        FBZZ_LOG_ERROR("ResourceManager::CreateTexture3D failed (%ux%ux%u)", width, height, depth);
        return ResourceHandle<TextureTag>::Null();
    }
    const std::size_t bytes = EstimateTextureBytes(*texture);
    return m_textures.Insert(std::move(texture), bytes, "Texture3DFromData", where.file_name(), static_cast<int>(where.line()));
}

ResourceHandle<BufferTag> ResourceManager::CreateVertexBuffer(const void* data, size_t bytes, uint32_t stride,
                                                              Where where)
{
    return m_buffers.Insert(m_renderer.CreateNativeVertexBuffer(data, bytes, stride), bytes,
                            "VertexBuffer", where.file_name(), static_cast<int>(where.line()));
}

ResourceHandle<BufferTag> ResourceManager::CreateGpuWritableVertexBuffer(size_t bytes, uint32_t stride,
                                                                         Where where)
{
    return m_buffers.Insert(m_renderer.CreateNativeGpuWritableVertexBuffer(bytes, stride), bytes,
                            "GpuWritableVertexBuffer", where.file_name(), static_cast<int>(where.line()));
}

ResourceHandle<BufferTag> ResourceManager::CreateIndexBuffer(const void* data, uint32_t count, Where where)
{
    return m_buffers.Insert(m_renderer.CreateNativeIndexBuffer(data, count),
                            static_cast<std::size_t>(count) * sizeof(uint32_t),
                            "IndexBuffer", where.file_name(), static_cast<int>(where.line()));
}

ResourceHandle<ConstantBufferTag> ResourceManager::CreateConstantBuffer(size_t sizeBytes, Where where)
{
    return m_constantBuffers.Insert(m_renderer.CreateNativeConstantBuffer(sizeBytes), sizeBytes,
                                    "ConstantBuffer", where.file_name(), static_cast<int>(where.line()));
}

ResourceHandle<PipelineStateTag> ResourceManager::CreatePipelineState(const PipelineStateDesc& desc, Where where)
{
    /// @note PSO は状態の束で、専有メモリと呼べる実体を持たない。
    return m_pipelineStates.Insert(m_renderer.CreateNativePipelineState(desc), 0,
                                   "PipelineState", where.file_name(), static_cast<int>(where.line()));
}

ResourceHandle<RenderTargetTag> ResourceManager::CreateRenderTarget(uint32_t width, uint32_t height, uint32_t colorCount,
                                                                    Where where)
{
    return CreateRenderTarget(width, height, RenderTargetDesc{ colorCount, Format::RGBA16F, true }, where);
}

ResourceHandle<RenderTargetTag> ResourceManager::CreateRenderTarget(uint32_t width, uint32_t height,
                                                                    const RenderTargetDesc& desc,
                                                                    Where where)
{
    const uint32_t colorCount = desc.colorCount;
    auto rt = m_renderer.CreateNativeRenderTarget(width, height, desc);
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
                                           "RenderTargetColorTexture", where.file_name(), static_cast<int>(where.line())));
    }

    /// @note 深度を持たない RT では SRV も作らない。GetDepthTexture は無効ハンドルを返し、
    /// @note       束縛しようとした側で «読めない» ことが分かる。
    ResourceHandle<TextureTag> depth = ResourceHandle<TextureTag>::Null();
    if (desc.withDepth) {
        auto depthTexture = m_renderer.CreateNativeTextureFromRenderTarget(
            *rt,
            0,
            RenderTargetTextureKind::Depth);
        const std::size_t depthBytes = depthTexture ? EstimateTextureBytes(*depthTexture) : 0;
        depth = m_textures.Insert(std::move(depthTexture), depthBytes,
                                  "RenderTargetDepthTexture", where.file_name(), static_cast<int>(where.line()));
    }

    /// @note 0 バイトとして登録: RT のメモリは上で登録した色/深度テクスチャ側に計上済みで、
    /// @note       ここでも数えると同じ実体を二重に積む。
    ResourceHandle<RenderTargetTag> handle =
        m_renderTargets.Insert(std::move(rt), 0, "RenderTarget", where.file_name(), static_cast<int>(where.line()));
    const uint64_t key = Key(handle);
    m_renderTargetColors[key] = std::move(colors);
    m_renderTargetDepths[key] = depth;
    return handle;
}

ResourceHandle<RenderTargetTag> ResourceManager::CreateCubemapRenderTarget(uint32_t size, uint32_t mipCount,
                                                                           Where where)
{
    auto rt = m_renderer.CreateNativeCubemapRenderTarget(size, mipCount);
    if (!rt) return ResourceHandle<RenderTargetTag>::Null();

    /// @note TextureCube SRV を 1 つの「カラーテクスチャ」として登録する。既存の
    /// @note       m_renderTargetColors 経路に乗せることで Release()/シャットダウン時の解放処理を
    /// @note       通常 RT と共有でき、キューブ専用のクリーンアップを書かずに済む。深度バッファは
    /// @note       持たないため m_renderTargetDepths には登録しない。
    std::vector<ResourceHandle<TextureTag>> colors;
    if (auto cubeTex = m_renderer.CreateNativeCubeTextureFromRenderTarget(*rt)) {
        /// @note 6 面ぶん。GetWidth/GetHeight は 1 面の寸法しか返さない。
        const std::size_t cubeBytes = EstimateTextureBytes(*cubeTex) * 6u;
        colors.push_back(m_textures.Insert(std::move(cubeTex), cubeBytes,
                                           "CubemapRenderTargetTexture", where.file_name(), static_cast<int>(where.line())));
    }

    ResourceHandle<RenderTargetTag> handle =
        m_renderTargets.Insert(std::move(rt), 0, "CubemapRenderTarget", where.file_name(), static_cast<int>(where.line()));
    m_renderTargetColors[Key(handle)] = std::move(colors);
    return handle;
}

ResourceHandle<TextureTag> ResourceManager::GetCubemapTexture(ResourceHandle<RenderTargetTag> rt)
{
    /// @note キューブ SRV は index 0 のカラーテクスチャとして登録してある。
    return GetColorTexture(rt, 0);
}

ResourceHandle<TextureTag> ResourceManager::CreateComputeTexture(uint32_t width, uint32_t height, Where where)
{
    return m_textures.Insert(m_renderer.CreateNativeComputeTexture(width, height),
                             /// @note RGBA16F
                             static_cast<std::size_t>(width) * height * 8u,
                             "ComputeTexture", where.file_name(), static_cast<int>(where.line()));
}

ResourceHandle<TextureTag> ResourceManager::CreateComputeTexture3D(
    uint32_t width, uint32_t height, uint32_t depth, Where where)
{
    auto texture = m_renderer.CreateNativeComputeTexture3D(width, height, depth);
    if (!texture) {
        FBZZ_LOG_WARN("CreateComputeTexture3D: backend does not support 3D compute textures (%ux%ux%u)",
                      width, height, depth);
        return ResourceHandle<TextureTag>::Null();
    }
    /// @note RGBA16F
    const std::size_t bytes = EstimateTextureBytes(*texture, 8u);
    return m_textures.Insert(std::move(texture), bytes, "ComputeTexture3D", where.file_name(), static_cast<int>(where.line()));
}

ResourceHandle<TextureTag> ResourceManager::CreateDynamicTexture(
    uint32_t width, uint32_t height, DynamicTextureFormat format, Where where)
{
    auto texture = m_renderer.CreateNativeDynamicTexture(width, height, format);
    if (!texture) {
        /// @note 未対応バックエンドでは nullptr が返る。呼び出し側は Null ハンドルで縮退を判断する。
        FBZZ_LOG_WARN("CreateDynamicTexture: backend does not support dynamic textures (%ux%u)",
                      width, height);
        return ResourceHandle<TextureTag>::Null();
    }
    const std::size_t bytes =
        EstimateTextureBytes(*texture, format == DynamicTextureFormat::R8 ? 1u : 4u);
    return m_textures.Insert(std::move(texture), bytes, "DynamicTexture", where.file_name(), static_cast<int>(where.line()));
}

ResourceHandle<TextureTag> ResourceManager::RegisterTexture(std::unique_ptr<ITexture> texture, Where where)
{
    if (!texture) return ResourceHandle<TextureTag>::Null();
    const std::size_t bytes = EstimateTextureBytes(*texture);
    return m_textures.Insert(std::move(texture), bytes, "AdoptedTexture", where.file_name(), static_cast<int>(where.line()));
}

ResourceHandle<StructuredBufferTag> ResourceManager::CreateStructuredBuffer(const void* data, uint32_t elementCount, uint32_t stride,
                                                                            Where where)
{
    auto sb = m_renderer.CreateNativeStructuredBuffer(data, elementCount, stride);
    if (!sb) {
        FBZZ_LOG_ERROR("ResourceManager::CreateStructuredBuffer failed (count=%u stride=%u)", elementCount, stride);
        return ResourceHandle<StructuredBufferTag>::Null();
    }
    return m_structuredBuffers.Insert(std::move(sb), static_cast<std::size_t>(elementCount) * stride,
                                      "StructuredBuffer", where.file_name(), static_cast<int>(where.line()));
}

ResourceHandle<StructuredBufferTag> ResourceManager::CreateGpuLocalStructuredBuffer(
    const void* data, uint32_t elementCount, uint32_t stride, Where where)
{
    auto sb = m_renderer.CreateNativeGpuLocalStructuredBuffer(data, elementCount, stride);
    if (!sb) {
        FBZZ_LOG_ERROR("ResourceManager::CreateGpuLocalStructuredBuffer failed (count=%u stride=%u)",
                       elementCount, stride);
        return ResourceHandle<StructuredBufferTag>::Null();
    }
    return m_structuredBuffers.Insert(std::move(sb), static_cast<std::size_t>(elementCount) * stride,
                                      "GpuLocalStructuredBuffer", where.file_name(), static_cast<int>(where.line()));
}

ResourceHandle<StructuredBufferTag> ResourceManager::CreateRWStructuredBuffer(const void* data, uint32_t elementCount, uint32_t stride,
                                                                              Where where)
{
    auto sb = m_renderer.CreateNativeRWStructuredBuffer(data, elementCount, stride);
    if (!sb) {
        FBZZ_LOG_ERROR("ResourceManager::CreateRWStructuredBuffer failed (count=%u stride=%u)", elementCount, stride);
        return ResourceHandle<StructuredBufferTag>::Null();
    }
    return m_structuredBuffers.Insert(std::move(sb), static_cast<std::size_t>(elementCount) * stride,
                                      "RWStructuredBuffer", where.file_name(), static_cast<int>(where.line()));
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
    if (m_renderResources) m_renderResources->ReleaseOutput(h);
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
    m_renderResources.reset();
    /// @note RenderTarget 由来の TextureTag は RT の SRV を参照するラッパーなので、
    /// @note       マップを先に消して shutdown 時の重複解放経路を断つ。
    m_renderTargetColors.clear();
    m_renderTargetDepths.clear();
    m_shaderCache.clear();
    m_textureCache.clear();
    m_texturesHandedOut.clear();

    m_renderTargets.ReleaseOwnedForShutdown();
    m_textures.ReleaseOwnedForShutdown();
    m_buffers.ReleaseOwnedForShutdown();
    m_constantBuffers.ReleaseOwnedForShutdown();
    m_pipelineStates.ReleaseOwnedForShutdown();
    m_structuredBuffers.ReleaseOwnedForShutdown();
    m_shaders.ReleaseOwnedForShutdown();
}

namespace {

struct OriginTotal {
    const char* allocatorName = "Unknown";
    const char* file = "Unknown";
    int         line = 0;
    std::size_t count = 0;
    std::size_t bytes = 0;
};

/// @note 生存リソースを «発生位置ごとの本数» に畳んで、多い順に返す。
/// @note プロセス寿命のキャッシュ (シェーダー/テクスチャ/既定メッシュ) は最後まで生きているのが
/// @note       正しく、生の一覧では数百行のうちどれが漏れなのか読めない。同じ file:line の本数で
/// @note       並べれば «撒いた数だけ増えているもの» が一目で分かる。
std::vector<OriginTotal> SummarizeByOrigin(const std::vector<core::AllocationInfo>& live)
{
    std::vector<OriginTotal> totals;
    for (const core::AllocationInfo& info : live) {
        const auto found = std::find_if(totals.begin(), totals.end(), [&](const OriginTotal& t) {
            return t.line == info.line && t.file == info.file && t.allocatorName == info.allocatorName;
        });
        OriginTotal& total = (found != totals.end())
            ? *found
            : totals.emplace_back(OriginTotal{ info.allocatorName, info.file, info.line, 0, 0 });
        ++total.count;
        total.bytes += info.size;
    }
    std::sort(totals.begin(), totals.end(), [](const OriginTotal& a, const OriginTotal& b) {
        return a.count > b.count;
    });
    return totals;
}

void LogOriginTotals(const std::vector<OriginTotal>& totals, std::size_t maxRows)
{
    const std::size_t reportCount = (std::min)(totals.size(), maxRows);
    for (std::size_t i = 0; i < reportCount; ++i) {
        const OriginTotal& total = totals[i];
        FBZZ_LOG_WARN("  x%zu %s %zu bytes (%s:%d)",
                      total.count, total.allocatorName, total.bytes, total.file, total.line);
    }
    if (totals.size() > reportCount) {
        FBZZ_LOG_WARN("  ... %zu more origins", totals.size() - reportCount);
    }
}

} /// @note namespace

void ResourceManager::LogLiveDebugResources() const
{
    std::vector<core::AllocationInfo> live;
    CollectLiveDebugResources(live);
    if (live.empty()) {
        return;
    }

    FBZZ_LOG_WARN("ResourceManager shutdown: %zu renderer resources still held (%zu bytes) — "
                  "process-lifetime caches are expected here; look for one origin with an unusual count",
                  live.size(), GetLiveDebugResourceBytes());
    LogOriginTotals(SummarizeByOrigin(live), 16);
}

std::size_t ResourceManager::GetLiveDebugResourceBytes() const
{
    return m_shaders.GetLiveDebugBytes()
         + m_textures.GetLiveDebugBytes()
         + m_buffers.GetLiveDebugBytes()
         + m_constantBuffers.GetLiveDebugBytes()
         + m_pipelineStates.GetLiveDebugBytes()
         + m_renderTargets.GetLiveDebugBytes()
         + m_structuredBuffers.GetLiveDebugBytes();
}

void ResourceManager::TickLeakWatchdog()
{
#if defined(FBZZ_GPU_VALIDATION)
    /// @note 連続で増え続けた «フレーム数» のしきい値。まばらな生成 (シーン読み込み・
    /// @note       プールの立ち上がり) で鳴らないよう、数秒ぶん増え続けたときだけ疑う。
    constexpr uint32_t kGrowthFrames = 180;
    /// @note 同じ実行で何度も出しても読み切れない。上限を決めて黙る。
    constexpr uint32_t kMaxReports = 3;

    /// @note エディタは 1 フレームで Scene / Game の 2 面を描く。フレームが変わったときだけ数える。
    if (m_watchdogFrame == FrameStamp())
        return;
    m_watchdogFrame = FrameStamp();

    const std::size_t bytes = GetLiveDebugResourceBytes();
    if (bytes > m_watchdogLastBytes) {
        ++m_watchdogGrowthFrames;
    } else {
        m_watchdogGrowthFrames = 0;
    }
    m_watchdogLastBytes = bytes;

    if (m_watchdogGrowthFrames < kGrowthFrames || m_watchdogReportCount >= kMaxReports)
        return;
    m_watchdogGrowthFrames = 0;
    ++m_watchdogReportCount;

    std::vector<core::AllocationInfo> live;
    CollectLiveDebugResources(live);
    FBZZ_LOG_WARN("ResourceManager: renderer resources grew for %u consecutive frames "
                  "(now %zu resources / %zu bytes). 発生位置の多い順:",
                  kGrowthFrames, live.size(), bytes);
    LogOriginTotals(SummarizeByOrigin(live), 8);
#endif
}

/// @name Mesh のバッファの寿命

std::size_t ResourceManager::ReleaseMeshBuffers(Mesh& mesh)
{
    std::size_t released = 0;
    if (mesh.vertexBuffer.IsValid()) { Release(mesh.vertexBuffer); ++released; }
    if (mesh.indexBuffer.IsValid())  { Release(mesh.indexBuffer);  ++released; }
    mesh.vertexBuffer = {};
    mesh.indexBuffer  = {};
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

void ResourceManager::CollectLiveDebugResources(std::vector<core::AllocationInfo>& out) const
{
    m_shaders.CollectLiveDebugInfo(out);
    m_textures.CollectLiveDebugInfo(out);
    m_buffers.CollectLiveDebugInfo(out);
    m_constantBuffers.CollectLiveDebugInfo(out);
    m_pipelineStates.CollectLiveDebugInfo(out);
    m_renderTargets.CollectLiveDebugInfo(out);
    m_structuredBuffers.CollectLiveDebugInfo(out);
}

bool SizedRenderTarget::Ensure(ResourceManager& resources,
                               uint32_t width, uint32_t height, uint32_t colorCount,
                               Where where)
{
    RenderTargetDesc desc{};
    desc.colorCount = colorCount;
    return Ensure(resources, width, height, desc, where);
}

bool SizedRenderTarget::Ensure(ResourceManager& resources,
                               uint32_t width, uint32_t height, const RenderTargetDesc& desc,
                               Where where)
{
    const uint64_t resetVersion = resources.GetResetVersion();
    if (m_resetVersion != resetVersion) {
        /// @note 返さず捨てる理由: リセットではマネージャーが実体ごと畳んでおり、こちらの
        /// @note       ハンドルは無効で返しても意味が無い (最悪、同じスロットへ入ってきた別の実体を
        /// @note       巻き添えにする)。控えだけ捨てて作り直す。
        m_resetVersion = resetVersion;
        m_handle       = {};
        m_width = m_height = 0;
    }

    const bool sameDesc = m_desc.colorCount == desc.colorCount && m_desc.format == desc.format
        && m_desc.withDepth == desc.withDepth && m_desc.reversedZ == desc.reversedZ;
    if (m_handle.IsValid() && m_width == width && m_height == height && sameDesc)
        return false;

    if (m_handle.IsValid()) resources.Release(m_handle);
    m_handle = resources.CreateRenderTarget(width, height, desc, where);
    m_width  = width;
    m_height = height;
    m_desc   = desc;
    return true;
}

void SizedRenderTarget::Release(ResourceManager& resources)
{
    if (m_handle.IsValid() && m_resetVersion == resources.GetResetVersion())
        resources.Release(m_handle);
    m_handle = {};
    m_width = m_height = 0;
}

} /// @note namespace fbzz::renderer
