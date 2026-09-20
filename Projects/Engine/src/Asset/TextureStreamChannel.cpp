/// @file    TextureStreamChannel.cpp
/// @brief   TextureAsset の非同期経路。
/// @author  Hasegawa Jin
/// @date    2026-09-19
#include <Engine/Asset/TextureStreamChannel.hpp>
#include <Engine/Asset/AssetStreamChannel.hpp>
#include <Engine/Asset/TexDescSerializer.hpp>
#include <Engine/Asset/TextureAsset.hpp>
#include <Engine/Asset/TextureStreamCache.hpp>
#include <Engine/Renderer/ITexture.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Renderer/TextureFileDecoder.hpp>
#include <Engine/Util/FileSystem.hpp>

#include <filesystem>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fbzz::asset {

namespace {

/// 落とせる品質段。4 段で 1/16 (面積 1/256) まで縮む。
constexpr AssetQuality kLowestTextureQuality = 4;

/// @brief メインスレッドで確定させた import 設定と、GPU 実体が既にあるかの判定。
struct TextureJobContext final : AssetJobContext {
    TextureImportSettings settings;
    /// 同じ画像が同じ品質で ResourceManager に載っていれば画素は要らない (転送もしない)。
    bool gpuAlreadyResident = false;
    /// 品質段キャッシュ (.fztc) の場所と、元画像の世代。場所が空ならキャッシュを使わない。
    std::string cachePath;
    uint64_t    sourceStamp = 0;
};

struct DecodedTexture final : IDecodedAsset {
    std::string                   sourcePath;
    TextureImportSettings         settings;
    renderer::DecodedTextureRGBA8 pixels;
    AssetQuality                  quality = 0;

    [[nodiscard]] std::size_t CpuBytes() const override { return pixels.ByteSize(); }
};

struct TextureCandidate final : AssetCandidate<TextureAsset> {
    /// BeginTextureUpload で作った実体か。パスキャッシュの既存を借りただけなら返さない。
    bool         ownsGpuHandle = false;
    AssetQuality quality = 0;
};

class TextureStreamChannel final : public AssetStreamChannel<TextureAsset> {
public:
    explicit TextureStreamChannel(renderer::ResourceManager& resources) : m_resources(resources) {}

    [[nodiscard]] bool SupportsQuality() const override { return true; }
    [[nodiscard]] AssetQuality LowestQuality() const override { return kLowestTextureQuality; }

    /// @note .meta の読み取りもここで行う。ワーカーへは確定した設定だけを渡す。
    [[nodiscard]] bool Snapshot(const std::string& key, AssetJobInput& out, std::string& outError) override
    {
        std::string sourcePath;
        if (!TexDescSerializer::ResolveSourcePath(AssetManager::ResolveAssetPath(key), sourcePath)
            || sourcePath.empty()) {
            outError = "テクスチャの元画像を解決できません: " + key;
            return false;
        }

        auto context = std::make_shared<TextureJobContext>();
        TextureAsset scratch;
        const std::string metaPath = sourcePath + ".meta";
        bool settingsLoaded = false;
        if (util::FileSystem::Exists(metaPath)) {
            TexDescSerializer serializer;
            settingsLoaded = serializer.Load(metaPath, scratch);
        }
        context->settings = settingsLoaded
            ? scratch.settings
            : DefaultSettingsForType(GuessTextureType(util::FileSystem::GetFilename(sourcePath)));
        context->gpuAlreadyResident = IsResidentAtQuality(sourcePath, out.quality);

        /// @note 元画像の世代はここで決める。ワーカーが読む間にファイルが差し替わっても、古い世代で書いた
        ///       キャッシュは次の Snapshot で食い違って捨てられる。
        if (const std::string cacheDir = AssetManager::StreamCacheDir(); !cacheDir.empty()) {
            std::error_code error;
            const auto path = util::FileSystem::PathFromUtf8(sourcePath);
            const auto size = std::filesystem::file_size(path, error);
            const auto writeTime = error ? std::filesystem::file_time_type{} : std::filesystem::last_write_time(path, error);
            if (!error) {
                context->cachePath = cacheDir + "/" + texturecache::CacheFileName(sourcePath);
                context->sourceStamp = texturecache::MakeSourceStamp(
                    static_cast<uint64_t>(size), static_cast<int64_t>(writeTime.time_since_epoch().count()));
            }
        }

        out.resolvedPath = std::move(sourcePath);
        out.context = std::move(context);
        return true;
    }

    [[nodiscard]] AssetDecodeResult Decode(const AssetJobInput& input) override
    {
        AssetDecodeResult result;
        const auto& context = static_cast<const TextureJobContext&>(*input.context);
        auto decoded = std::make_unique<DecodedTexture>();
        decoded->sourcePath = input.resolvedPath;
        decoded->settings = context.settings;
        decoded->quality = input.quality;
        if (!context.gpuAlreadyResident && !DecodePixels(input, context, decoded->pixels, result.message)) {
            result.error = AssetLoadError::DecodeFailed;
            return result;
        }
        result.decoded = std::move(decoded);
        return result;
    }

    /// @brief ワーカー: 品質段キャッシュから要る段だけを読む。無ければ元画像を全体展開してキャッシュを作る。
    /// @note 最高品質で元画像から読む場合は、同期経路と同じ関数 (DecodeTextureFileRGBA8) を通す。
    static bool DecodePixels(const AssetJobInput& input, const TextureJobContext& context,
                             renderer::DecodedTextureRGBA8& out, std::string& outError)
    {
        if (!context.cachePath.empty()
            && texturecache::ReadQuality(context.cachePath, context.sourceStamp, input.quality, out))
            return true;
        renderer::DecodedTextureRGBA8 full;
        if (!renderer::DecodeTextureFileRGBA8(input.resolvedPath, full, &outError)) return false;
        if (!context.cachePath.empty())
            (void)texturecache::Write(context.cachePath, context.sourceStamp, full, kLowestTextureQuality);
        out = texturecache::SelectQuality(full, input.quality);
        return true;
    }

    [[nodiscard]] std::uint64_t DeviceEpoch() const override { return m_resources.GetResetVersion(); }

    [[nodiscard]] std::unique_ptr<IAssetCandidate> BeginUpload(
        IDecodedAsset& decoded, std::uint64_t& outToken, std::string& outError) override
    {
        auto& texture = static_cast<DecodedTexture&>(decoded);
        outToken = 0;
        auto candidate = std::make_unique<TextureCandidate>();
        candidate->quality = texture.quality;
        candidate->asset = std::make_unique<TextureAsset>();
        candidate->asset->sourcePath = texture.sourcePath;
        candidate->asset->settings = texture.settings;
        candidate->asset->sourceWidth = texture.pixels.sourceWidth;
        candidate->asset->sourceHeight = texture.pixels.sourceHeight;

        /// @note ワーカーが動いている間に同期 LoadTexture が同じ品質で読んでいれば、その実体を使う。
        if (IsResidentAtQuality(texture.sourcePath, texture.quality)) {
            candidate->asset->gpuHandle = m_resources.FindCachedTexture(texture.sourcePath);
            if (candidate->asset->sourceWidth == 0) {
                if (const renderer::ITexture* resident = m_resources.Get(candidate->asset->gpuHandle)) {
                    candidate->asset->sourceWidth = resident->GetWidth();
                    candidate->asset->sourceHeight = resident->GetHeight();
                }
            }
            return candidate;
        }

        std::vector<renderer::TextureMipData> mips;
        if (!texture.pixels.ToMipData(mips)) {
            outError = "展開済みの画素がありません: " + texture.sourcePath;
            return nullptr;
        }
        candidate->asset->gpuHandle = m_resources.BeginTextureUpload(
            mips.data(), static_cast<uint32_t>(mips.size()), outToken);
        if (!candidate->asset->gpuHandle.IsValid()) {
            outError = "GPU 転送の投入に失敗しました: " + texture.sourcePath;
            return nullptr;
        }
        candidate->ownsGpuHandle = true;
        /// @note 画素は公開まで残す。転送中にデバイスが作り直されたら、ここから転送し直す。
        return candidate;
    }

    [[nodiscard]] bool IsUploadComplete(std::uint64_t token) const override
    {
        return m_resources.IsUploadComplete(token);
    }

    [[nodiscard]] bool Publish(RawAssetHandle handle, IAssetCandidate& candidate) override
    {
        auto& texture = static_cast<TextureCandidate&>(candidate);
        if (!texture.asset) return false;
        if (texture.ownsGpuHandle) {
            const auto uploaded = texture.asset->gpuHandle;
            const TextureAsset* current = TypedGet(handle);
            bool adopted = false;
            if (current && m_resources.Get(current->gpuHandle) != nullptr
                && m_resources.ReplaceTextureContents(current->gpuHandle, uploaded)) {
                /// @note 品質変更・再読み込み: 配ってあるハンドルはそのまま、中身だけ新しい実体へ移る。
                texture.asset->gpuHandle = current->gpuHandle;
                adopted = true;
            } else {
                /// @note パスキャッシュへ載せた時点で所有は ResourceManager へ移る (同期経路と同じ)。
                ///       既に同期経路の実体が載っていればそちらが正で、こちらの転送物は返される。
                texture.asset->gpuHandle = m_resources.PublishTexture(texture.asset->sourcePath, uploaded);
                adopted = texture.asset->gpuHandle == uploaded;
            }
            texture.ownsGpuHandle = false;
            if (adopted) m_residentQuality[texture.asset->sourcePath] = texture.quality;
            else m_residentQuality.erase(texture.asset->sourcePath);
        }
        return AssetStreamChannel<TextureAsset>::Publish(handle, candidate);
    }

    void Discard(IAssetCandidate& candidate, bool deviceStillValid) override
    {
        auto& texture = static_cast<TextureCandidate&>(candidate);
        /// @note 転送中でも Release してよい。GPU 実体の返却はバックエンドのフェンス管理が遅らせる。
        ///       デバイスが作り直された後のハンドルは既に無効なので触らない。
        if (texture.ownsGpuHandle && deviceStillValid && texture.asset)
            m_resources.Release(texture.asset->gpuHandle);
        texture.ownsGpuHandle = false;
    }

    [[nodiscard]] std::uint64_t EstimateResidentBytes(RawAssetHandle handle) const override
    {
        const TextureAsset* asset = TypedGet(handle);
        return asset ? m_resources.GetTextureBytes(asset->gpuHandle) : 0;
    }

protected:
    /// @note LoadTexture で外へ配られたことのある GPU 実体は外さない。その場合は TextureAsset のスロットも残す。
    [[nodiscard]] bool ReleaseResident(RawAssetHandle handle) override
    {
        const TextureAsset* asset = TypedGet(handle);
        if (!asset) return true;
        /// @note 同じ画像を別の書き方 (絶対パスと Assets/ 起点) で引いた TextureAsset は GPU 実体を共有する。
        ///       片方が生きている間は実体を外さない。
        for (const auto& slot : AssetStore<TextureAsset>::Get().slots) {
            if (slot.occupied && slot.asset && slot.asset.get() != asset
                && slot.asset->gpuHandle == asset->gpuHandle)
                return false;
        }
        if (!m_resources.EvictStreamedTexture(asset->sourcePath)) return false;
        m_residentQuality.erase(asset->sourcePath);
        return true;
    }

private:
    /// @brief パスキャッシュの実体がその品質で作られたものか。同期経路で載ったものは最高品質として扱う。
    [[nodiscard]] bool IsResidentAtQuality(const std::string& sourcePath, AssetQuality quality) const
    {
        if (!m_resources.FindCachedTexture(sourcePath).IsValid()) return false;
        const auto it = m_residentQuality.find(sourcePath);
        const AssetQuality resident = it != m_residentQuality.end() ? it->second : 0;
        return resident == quality;
    }

    renderer::ResourceManager& m_resources;
    /// この経路がパスキャッシュへ載せた実体の品質段。載っていなければ同期経路の最高品質。
    std::unordered_map<std::string, AssetQuality> m_residentQuality;
};

} // namespace

std::shared_ptr<IAssetStreamChannel> CreateTextureStreamChannel(renderer::ResourceManager& resources)
{
    return std::make_shared<TextureStreamChannel>(resources);
}

} // namespace fbzz::asset
