/// @file    ResourceTextureCache.cpp
/// @brief   テクスチャのパス解決、同期ロード・公開、ピン留めと回収。
/// @author  Hasegawa Jin
/// @date    2026-11-21
#include <Graphics/Renderer/ResourceManager.hpp>
#include <Graphics/Renderer/AssetPathService.hpp>
#include <Graphics/Renderer/TextureFileDecoder.hpp>
#include <Graphics/Renderer/IRenderer.hpp>
#include <Graphics/Renderer/ITexture.hpp>
#include <Core/Logger.hpp>
#include <algorithm>
#include <chrono>

namespace fbzz::renderer {
namespace {
/// @note Windows の '\\' とアセット記述で使う '/' を同一キーにし、同じ実ファイルの二重キャッシュを防ぐ。
std::string TextureSourceKey(std::string_view path)
{
    /// @note Sprite参照はGPU上では親Textureを共有する。サブアセット名をキャッシュキーへ
    /// @note       含めると同じ画像を重複ロードするため、ここで親パスへ正規化する。
    std::string key = NormalizeTextureKey(path);
    std::replace(key.begin(), key.end(), '\\', '/');
    return key;
}

/// @note Source resolver へ渡す表記は従来のまま保ち、検索 identity の casefold と区別する。
std::string TextureCacheKey(std::string_view path, bool flipGreen = false)
{
    std::string key = TextureSourceKey(path);
#if defined(_WIN32)
    /// @note Windows の通常のパス比較と Asset 層の ASCII casefold に揃え、stream 公開と reload のキーを共有する。
    /// @note 非 Windows の case-sensitive なパスと、既存の Sprite/GUID/相対参照の解決順は変更しない。
    for (char& character : key) {
        if (character >= 'A' && character <= 'Z')
            character = static_cast<char>(character + ('a' - 'A'));
    }
#endif
    if (flipGreen) key += "|flipGreen";
    return key;
}
} /// @note namespace

/// @brief 法線の G 反転は元画像を変更せず、全ミップへ同じ変換を適用する。
std::unique_ptr<ITexture> ResourceManager::TextureCache::CreateTextureForPath(const std::string& sourcePath,
                                                                 bool flipGreen)
{
    if (!flipGreen && !SupportsAutomaticTextureMips(sourcePath)) return m_owner.m_renderer.CreateNativeTexture(sourcePath);
    DecodedTextureRGBA8 decoded;
    std::string error;
    if (!DecodeTextureFileRGBA8(sourcePath, decoded, &error)
        || !PrepareTextureMipmaps(sourcePath, decoded, ResolveTextureMipSettings(sourcePath), &error)) {
        FBZZ_LOG_ERROR("Texture decode failed: %s", error.c_str());
        return nullptr;
    }
    if (flipGreen) FlipTextureGreen(decoded);
    std::vector<TextureMipData> mips;
    if (!decoded.ToMipData(mips)) return nullptr;
    return m_owner.m_renderer.CreateNativeTextureFromDataMips(mips.data(), static_cast<uint32_t>(mips.size()));
}

ResourceHandle<TextureTag> ResourceManager::TextureCache::Load(std::string_view path, bool pin,
                                                             bool flipGreen)
{
    const std::string sourceKey = TextureSourceKey(path);
    const std::string key = TextureCacheKey(path, flipGreen);
    /// @note 配った先がハンドルを持ち続けるかもしれない。一度でも配ったキーは非同期側から外さない。
    if (pin) m_pinnedKeys.insert(key);
    auto it = m_entries.find(key);
    if (it != m_entries.end()) return it->second;

    const auto loadBegin = std::chrono::steady_clock::now();
    const auto recordSynchronousLoad = [&](bool succeeded) {
        const double elapsedMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - loadBegin).count();
        ++m_loadStats.attempts;
        if (succeeded) ++m_loadStats.successes;
        else ++m_loadStats.failures;
        m_loadStats.totalMilliseconds += elapsedMs;
        m_loadStats.lastMilliseconds = elapsedMs;
        m_loadStats.maxMilliseconds = (std::max)(m_loadStats.maxMilliseconds, elapsedMs);
        m_loadStats.lastPath = key;
    };

    /// @note `.meta` サイドカー表記と生画像パスを同じ公開 API で扱う (ResolveTextureSource が
    /// @note       元画像へ解決)。.mat/Scene は Assets/ 起点の相対パスを保存するがテクスチャ実装は
    /// @note       実ファイルパスを要求するため、AssetManager と同じ解決規則を通して cwd 依存をなくす。
    std::string sourcePath;
    if (!ResolveTextureSource(ResolveAssetPath(sourceKey), sourcePath)) {
        recordSynchronousLoad(false);
        FBZZ_LOG_ERROR("Texture path resolution failed: %s", key.c_str());
        return ResourceHandle<TextureTag>::Null();
    }
    auto texture = CreateTextureForPath(sourcePath, flipGreen);
    if (!texture) {
        recordSynchronousLoad(false);
        FBZZ_LOG_ERROR("Texture load failed: %s", key.c_str());
        return ResourceHandle<TextureTag>::Null();
    }

    const std::size_t textureBytes = EstimateTextureBytes(*texture);
    ResourceHandle<TextureTag> handle =
        m_owner.m_textures.Insert(std::move(texture), textureBytes, "Texture", __FILE__, __LINE__);
    m_entries[key] = handle;
    recordSynchronousLoad(true);
    return handle;
}

ResourceHandle<TextureTag> ResourceManager::TextureCache::Reload(std::string_view path, bool flipGreen)
{
    const std::string key = TextureCacheKey(path, flipGreen);
    auto it = m_entries.find(key);
    if (it == m_entries.end())
        return Load(path, true, flipGreen);

    std::string sourcePath;
    if (!ResolveTextureSource(ResolveAssetPath(TextureSourceKey(path)), sourcePath)) {
        FBZZ_LOG_ERROR("ReloadTexture path resolution failed: %s", key.c_str());
        return it->second;
    }
    auto newTexture = CreateTextureForPath(sourcePath, flipGreen);
    if (!newTexture) {
        FBZZ_LOG_ERROR("ReloadTexture failed: %s", key.c_str());
        return it->second;
    }

    /// @note ResourcePool のスロットを置換し、RenderSystem や Material が保持するハンドルを有効なまま保つ。
    const auto* previousTexture = m_owner.Get(it->second);
    newTexture->AdoptContentVersion(previousTexture ? previousTexture->GetContentVersion() : 0);
    const std::size_t reloadedBytes = EstimateTextureBytes(*newTexture);
    m_owner.m_textures.Replace(it->second, std::move(newTexture), reloadedBytes);
    return it->second;
}

std::size_t ResourceManager::TextureCache::Evict(std::string_view path)
{
    const std::string key = TextureCacheKey(path);
    if (key.empty()) return 0;

    /// @note 区切りを含めて比較し、同じ名前で始まる隣のディレクトリを残す。
    const std::string prefix = key + "/";
    const std::string flippedKey = TextureCacheKey(path, true);
    std::size_t evicted = 0;
    std::erase_if(m_entries, [&](const auto& entry) {
        if (entry.first != key && entry.first != flippedKey && !entry.first.starts_with(prefix))
            return false;
        m_owner.m_textures.Remove(entry.second);
        ++evicted;
        return true;
    });
    return evicted;
}

ResourceHandle<TextureTag> ResourceManager::TextureCache::Find(std::string_view path,
                                                               bool flipGreen) const
{
    const auto it = m_entries.find(TextureCacheKey(path, flipGreen));
    return it != m_entries.end() ? it->second : ResourceHandle<TextureTag>::Null();
}

ResourceHandle<TextureTag> ResourceManager::TextureCache::Publish(std::string_view path,
                                                            ResourceHandle<TextureTag> uploaded,
                                                            bool flipGreen)
{
    const std::string key = TextureCacheKey(path, flipGreen);
    const auto it = m_entries.find(key);
    /// @note 転送中に同期 LoadTexture が同じ画像を読んでいれば、そちらが既に配られている。配られた方を正とする。
    if (it != m_entries.end() && m_owner.Get(it->second) != nullptr) {
        if (uploaded != it->second) m_owner.Release(uploaded);
        return it->second;
    }
    if (m_owner.Get(uploaded) == nullptr) return ResourceHandle<TextureTag>::Null();
    m_entries[key] = uploaded;
    return uploaded;
}

bool ResourceManager::TextureCache::EvictStreamed(std::string_view path, bool flipGreen)
{
    const std::string key = TextureCacheKey(path, flipGreen);
    if (m_pinnedKeys.count(key) != 0) return false;
    const auto it = m_entries.find(key);
    if (it == m_entries.end()) return true;
    m_owner.m_textures.Remove(it->second);
    m_entries.erase(it);
    return true;
}

void ResourceManager::TextureCache::Clear()
{
    m_entries.clear();
    m_pinnedKeys.clear();
}
} /// @note namespace fbzz::renderer
