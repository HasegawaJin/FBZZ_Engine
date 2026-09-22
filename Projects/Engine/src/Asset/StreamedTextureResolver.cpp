/// @file    StreamedTextureResolver.cpp
/// @brief   描画側のテクスチャ参照を AssetStreamer の利用権へ結ぶ。
/// @author  Hasegawa Jin
/// @date    2026-09-19
#include <Engine/Asset/StreamedTextureResolver.hpp>
#include <Engine/Asset/TextureAsset.hpp>
#include <Engine/Renderer/ITexture.hpp>
#include <Engine/Renderer/ResourceManager.hpp>

#include <algorithm>
#include <cmath>
#include <string>

namespace fbzz::asset {

namespace {

/// @note 自動選択で落とす最低の段。経路の最低品質 (テクスチャは 4) と揃える。
constexpr AssetQuality kAutoQualityLowest = 4;

} /// @note namespace

StreamedTextureResolver& StreamedTextureResolver::Engine()
{
    /// @note 台帳を先に構築させる。静的変数は構築の逆順に壊れるので、利用権を持つこちらが先に壊れ、
    /// @note 利用権の解放が壊れた台帳を触らない。
    (void)AssetStreamer::Engine();
    static StreamedTextureResolver resolver;
    return resolver;
}

StreamedTextureResolver::Entry* StreamedTextureResolver::Acquire(std::string_view reference)
{
    std::string key(reference);
    auto it = m_entries.find(key);
    if (it == m_entries.end()) {
        /// @note Sprite 参照は親テクスチャを共有する。接尾辞込みで要求すると同じ画像を Sprite の数だけ読む。
        std::string texturePath;
        std::string spriteToken;
        const std::string target = ParseSpriteReference(reference, texturePath, spriteToken) ? texturePath : key;
        auto lease = AssetStreamer::Engine().Request<TextureAsset>(target);
        if (!lease.IsHeld()) return nullptr;
        it = m_entries.emplace(std::move(key), Entry{ std::move(lease) }).first;
    }
    it->second.lastUsedFrame = m_frame;
    return &it->second;
}

const TextureAsset* StreamedTextureResolver::Complete(Entry& entry)
{
    AssetStreamer& streamer = AssetStreamer::Engine();
    if (const TextureAsset* ready = streamer.TryGet(entry.lease.Handle())) return ready;
    const AssetLoadState state = streamer.GetStatus(entry.lease.Handle()).state;
    if (state == AssetLoadState::Failed || m_missPolicy == MissPolicy::Placeholder || entry.syncFailed)
        return nullptr;
    /// @note 既定の方針。これまでの LoadTexture と同じく引いたフレームに絵を出す。先読みが済んでいれば
    /// @note ここへは来ない (公開済みを返す)。
    if (!streamer.CompleteNow(entry.lease.Handle())) {
        entry.syncFailed = true;
        return nullptr;
    }
    return streamer.TryGet(entry.lease.Handle());
}

AssetQuality StreamedTextureResolver::QualityForScreenSize(uint32_t textureWidth, uint32_t textureHeight,
                                                           float screenPixels, AssetQuality lowest)
{
    const uint32_t longest = (std::max)(textureWidth, textureHeight);
    if (screenPixels <= 0.0f || longest == 0) return 0;
    const float ratio = static_cast<float>(longest) / screenPixels;
    if (ratio <= 1.0f) return 0;
    /// @note 1 段の余裕: 拡大縮小や UV の繰り返しで画面上のテクセル密度は物体の大きさより高くなりがちで、
    /// @note ぴったりの段を選ぶとぼけが目に見える。
    const int level = static_cast<int>(std::floor(std::log2(ratio))) - 1;
    return static_cast<AssetQuality>(std::clamp(level, 0, static_cast<int>(lowest)));
}

renderer::ResourceHandle<renderer::TextureTag> StreamedTextureResolver::ResolveGpu(
    renderer::ResourceManager& resources, std::string_view reference, float screenPixels)
{
    if (reference.empty()) return {};
    Entry* entry = Acquire(reference);
    if (!entry) return resources.LoadTexture(reference);
    /// @note 同じテクスチャを貼った物体のうち最も大きく映るものに合わせる。1 つでも大きさ不明なら最高品質。
    if (screenPixels <= 0.0f) entry->screenSizeKnown = false;
    entry->maxScreenPixels = (std::max)(entry->maxScreenPixels, screenPixels);
    const TextureAsset* asset = Complete(*entry);
    return asset ? asset->gpuHandle : renderer::ResourceHandle<renderer::TextureTag>{};
}

const TextureAsset* StreamedTextureResolver::ResolveAsset(renderer::ResourceManager& /*resources*/,
                                                          std::string_view reference)
{
    if (reference.empty()) return nullptr;
    Entry* entry = Acquire(reference);
    if (entry) entry->screenSizeKnown = false;
    return entry ? Complete(*entry) : nullptr;
}

renderer::ResourceHandle<renderer::TextureTag> StreamedTextureResolver::ResolveGpuWithSourceSize(
    renderer::ResourceManager& resources, std::string_view reference,
    uint32_t& outSourceWidth, uint32_t& outSourceHeight)
{
    outSourceWidth = 0;
    outSourceHeight = 0;
    renderer::ResourceHandle<renderer::TextureTag> handle;
    if (const TextureAsset* asset = ResolveAsset(resources, reference)) {
        handle = asset->gpuHandle;
        outSourceWidth = asset->sourceWidth;
        outSourceHeight = asset->sourceHeight;
    } else {
        handle = ResolveGpu(resources, reference);
    }
    if ((outSourceWidth == 0 || outSourceHeight == 0) && resources.Get(handle) != nullptr) {
        outSourceWidth = resources.Get(handle)->GetWidth();
        outSourceHeight = resources.Get(handle)->GetHeight();
    }
    return handle;
}

void StreamedTextureResolver::EndFrame()
{
    AssetStreamer& streamer = AssetStreamer::Engine();
    for (auto& [reference, entry] : m_entries) {
        /// @note このフレームに引かれたものだけ決め直す。引かれなかったものは直前の品質のまま猶予を待つ。
        if (entry.lastUsedFrame == m_frame) {
            AssetQuality quality = 0;
            if (m_autoQuality && entry.screenSizeKnown) {
                if (const TextureAsset* asset = streamer.TryGet(entry.lease.Handle()))
                    quality = QualityForScreenSize(asset->sourceWidth, asset->sourceHeight,
                                                   entry.maxScreenPixels, kAutoQualityLowest);
            }
            if (quality != entry.appliedQuality) {
                entry.lease.SetDesiredQuality(quality);
                entry.appliedQuality = quality;
            }
        }
        entry.maxScreenPixels = 0.0f;
        entry.screenSizeKnown = true;
    }

    ++m_frame;
    if (m_releaseAfterFrames == 0) return;
    for (auto it = m_entries.begin(); it != m_entries.end(); ) {
        if (m_frame - it->second.lastUsedFrame > m_releaseAfterFrames) it = m_entries.erase(it);
        else ++it;
    }
}

void StreamedTextureResolver::Reset()
{
    m_entries.clear();
}

} /// @note namespace fbzz::asset
