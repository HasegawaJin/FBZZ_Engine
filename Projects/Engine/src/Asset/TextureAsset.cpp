/// @file    TextureAsset.cpp
/// @brief   テクスチャアセットの読み込みとインポート設定の適用。
/// @author  Hasegawa Jin
/// @date    2026-06-18
#include <Engine/Asset/TextureAsset.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/TexDescSerializer.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace fbzz::asset {

namespace {

constexpr std::string_view kSpriteMarker = "::sprite::";

/// .meta 1 件の解析結果と、ID / 名前からの索引。シート 1 枚に 272 コマ入ることがあり、
/// 線形探索を毎ドロー走らせると «文字列比較 272 回 × 描画対象数» になる。読むのは
/// 1 回きりなので索引もそのとき作る。
struct CachedMeta {
    std::filesystem::file_time_type writeTime{};
    bool                            valid = false;
    TextureImportSettings           settings;
    std::unordered_map<std::string, std::size_t> byId;
    std::unordered_map<std::string, std::size_t> byName;
};

std::mutex& MetaCacheMutex()
{
    static std::mutex mutex;
    return mutex;
}

std::unordered_map<std::string, CachedMeta>& MetaCache()
{
    static std::unordered_map<std::string, CachedMeta> cache;
    return cache;
}

/// 壊れた参照は解決のたび (= 毎フレーム) に通るため、同じ参照は 1 度だけ報告する。
/// AssetManager の guid 参照と同じ方針 (Console のリングバッファを埋めない)。
std::unordered_set<std::string>& BrokenSpriteReports()
{
    static std::unordered_set<std::string> reported;
    return reported;
}

/// 元画像パス / Sprite 参照 / .meta パスのどれを渡しても .meta の実パスを返す。呼ぶ側は
/// «今持っている文字列» を渡すだけにしたいため 3 種類を受ける。ここで受けないと、
/// 書き込んだ側が `.meta.meta` を作るような取り違えが起きる。
std::string MetaPathFor(std::string_view texturePathOrRef)
{
    std::string texturePath;
    std::string token;
    (void)ParseSpriteReference(texturePathOrRef, texturePath, token);
    if (texturePath.empty()) return {};
    /// @note GUID 参照に .meta を付ける前に実画像へ解決する。後付けすると GUID 解決が
    /// @note 画像本体を返し、取り込み設定を読めなくなる。
    texturePath = AssetManager::ResolveAssetPath(texturePath);
    if (texturePath.empty()) return {};
    if (!texturePath.ends_with(".meta")) texturePath += ".meta";
    return texturePath;
}

/// 呼び出し側は MetaCacheMutex() を保持していること。
const CachedMeta* AcquireMeta(const std::string& metaPath)
{
    if (metaPath.empty()) return nullptr;

    std::error_code ec;
    const auto writeTime = std::filesystem::last_write_time(
        util::FileSystem::PathFromUtf8(metaPath), ec);

    /// @note 読めなかった場合の writeTime は既定値。「無いまま」も同じ値で一致するので、
    ///       存在しない .meta を毎フレーム開き直さずに済む。
    auto& cache = MetaCache();
    const auto found = cache.find(metaPath);
    if (found != cache.end() && found->second.writeTime == writeTime)
        return &found->second;

    CachedMeta entry;
    entry.writeTime = writeTime;
    if (!ec) {
        TextureAsset asset;
        const TexDescSerializer serializer;
        entry.valid = serializer.Load(metaPath, asset);
        if (entry.valid) entry.settings = std::move(asset.settings);
    }
    for (std::size_t i = 0; i < entry.settings.sprites.size(); ++i) {
        const SpriteRect& sprite = entry.settings.sprites[i];
        if (!sprite.id.empty())   entry.byId.emplace(sprite.id, i);
        if (!sprite.name.empty()) entry.byName.emplace(sprite.name, i);
    }
    /// @note 読み直したなら、この .meta 由来の «壊れている» 報告も出し直させる。
    ///       直したのに Console が沈黙したままだと、直った確認ができない。
    if (found != cache.end()) BrokenSpriteReports().clear();

    return &(cache[metaPath] = std::move(entry));
}

/// 索引付きの検索。ID を先に見る (FindSprite と同じ順序)。
const SpriteRect* FindInMeta(const CachedMeta& meta, const std::string& token, bool& outById)
{
    if (const auto byId = meta.byId.find(token); byId != meta.byId.end()) {
        outById = true;
        return &meta.settings.sprites[byId->second];
    }
    if (const auto byName = meta.byName.find(token); byName != meta.byName.end()) {
        outById = false;
        return &meta.settings.sprites[byName->second];
    }
    return nullptr;
}

void ReportBrokenSprite(std::string_view reference, const char* reason)
{
    if (!BrokenSpriteReports().insert(std::string(reference)).second) return;
    FBZZ_LOG_WARN("Sprite reference is broken [%.*s]\n  %s"
                  "\n  Sprite Editor で切り直すと ID が変わることがあります"
                  " (Slice の Delete Existing)。",
                  static_cast<int>(reference.size()), reference.data(), reason);
}

} /// @note namespace

TextureImportSettings DefaultSettingsForType(TextureType type)
{
    TextureImportSettings s;
    switch (type) {
    case TextureType::Color:
        s.type        = TextureType::Color;
        s.srgb        = true;
        /// @note BC1/BC3 depending on alpha
        s.compression = TextureCompression::Auto;
        s.mipmaps     = true;
        s.mipFilter   = MipFilter::Kaiser;
        s.filter      = TextureFilter::Anisotropic;
        s.anisoLevel  = 8;
        s.flipGreen   = false;
        break;

    case TextureType::Normal:
        s.type             = TextureType::Normal;
        s.srgb             = false;
        s.compression      = TextureCompression::BC5;
        s.mipmaps          = true;
        s.mipFilter        = MipFilter::Kaiser;
        s.normalizeMipmaps = true;
        s.filter           = TextureFilter::Anisotropic;
        s.anisoLevel       = 8;
        s.flipGreen        = false;
        break;

    case TextureType::Data:
        s.type        = TextureType::Data;
        s.srgb        = false;
        s.compression = TextureCompression::BC4;
        s.mipmaps     = true;
        s.mipFilter   = MipFilter::Box;
        s.filter      = TextureFilter::Bilinear;
        s.anisoLevel  = 1;
        s.flipGreen   = false;
        break;

    case TextureType::HDR:
        s.type        = TextureType::HDR;
        s.srgb        = false;
        s.compression = TextureCompression::BC6H;
        s.mipmaps     = true;
        s.mipFilter   = MipFilter::Kaiser;
        s.filter      = TextureFilter::Trilinear;
        s.anisoLevel  = 1;
        s.flipGreen   = false;
        break;

    case TextureType::UI:
        s.type        = TextureType::UI;
        s.srgb        = true;
        s.compression = TextureCompression::BC3;
        s.mipmaps     = false;
        s.filter      = TextureFilter::Bilinear;
        s.wrapU       = TextureWrap::Clamp;
        s.wrapV       = TextureWrap::Clamp;
        s.anisoLevel  = 1;
        s.flipGreen   = false;
        break;

    case TextureType::Sprite:
        /// @note SpriteはUIと同じサンプリング既定値を使い、矩形境界からの色漏れを防ぐ。
        s.type          = TextureType::Sprite;
        s.srgb          = true;
        s.compression   = TextureCompression::BC3;
        s.mipmaps       = false;
        s.filter        = TextureFilter::Bilinear;
        s.wrapU         = TextureWrap::Clamp;
        s.wrapV         = TextureWrap::Clamp;
        s.anisoLevel    = 1;
        s.flipGreen     = false;
        s.spriteMode    = SpriteMode::Single;
        s.pixelsPerUnit = 100.0f;
        break;
    }
    return s;
}

TextureType GuessTextureType(std::string_view filename)
{
    /// @note ファイル名末尾のステムを小文字で検索する。
    ///       例: "wall_n.png" → stem = "wall_n" → Normal
    std::string lower;
    lower.reserve(filename.size());
    for (char c : filename) lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));

    /// @note 拡張子を除く
    const auto dotPos = lower.rfind('.');
    std::string_view stem = (dotPos != std::string_view::npos)
        ? std::string_view(lower).substr(0, dotPos)
        : std::string_view(lower);

    /// @note HDR ファイルは拡張子で判定
    if (lower.ends_with(".hdr") || lower.ends_with(".exr"))
        return TextureType::HDR;

    /// @note UI hint
    if (stem.ends_with("_ui") || stem.ends_with("_icon") ||
        stem.ends_with("_hud") || stem.starts_with("ui_"))
        return TextureType::UI;

    /// @note Normal map suffixes
    static constexpr std::string_view kNormalSuffixes[] = {
        "_n", "_nrm", "_normal", "_nmap", "_bump", "_normalmap"
    };
    for (auto& suf : kNormalSuffixes)
        if (stem.ends_with(suf)) return TextureType::Normal;

    /// @note Data / linear map suffixes (roughness, metallic, AO, opacity, mask, emissive mask)
    static constexpr std::string_view kDataSuffixes[] = {
        "_r", "_rough", "_roughness",
        "_m", "_metal", "_metallic",
        "_ao", "_occlusion", "_occ",
        "_mask", "_opacity", "_alpha",
        /// @note displacement
        "_d",
        /// @note height
        "_h",
    };
    for (auto& suf : kDataSuffixes)
        if (stem.ends_with(suf)) return TextureType::Data;

    /// @note Emissive is still sRGB color
    if (stem.ends_with("_e") || stem.ends_with("_emissive") || stem.ends_with("_emission"))
        return TextureType::Color;

    return TextureType::Color;
}

std::string MakeSpriteReference(
    std::string_view texturePath, std::string_view spriteToken)
{
    std::string result(texturePath);
    result.append(kSpriteMarker);
    result.append(spriteToken);
    return result;
}

bool ParseSpriteReference(
    std::string_view reference, std::string& outTexturePath, std::string& outSpriteToken)
{
    const size_t markerPos = reference.find(kSpriteMarker);
    if (markerPos == std::string_view::npos || markerPos == 0
        || markerPos + kSpriteMarker.size() >= reference.size()) {
        outTexturePath.assign(reference);
        outSpriteToken.clear();
        return false;
    }
    outTexturePath.assign(reference.substr(0, markerPos));
    outSpriteToken.assign(reference.substr(markerPos + kSpriteMarker.size()));
    return true;
}

const SpriteRect* FindSprite(
    const TextureImportSettings& settings, std::string_view spriteToken)
{
    if (spriteToken.empty()) return nullptr;
    const auto byId = std::find_if(settings.sprites.begin(), settings.sprites.end(),
        [&](const SpriteRect& sprite) {
            return !sprite.id.empty() && sprite.id == spriteToken;
        });
    if (byId != settings.sprites.end()) return &*byId;

    const auto byName = std::find_if(settings.sprites.begin(), settings.sprites.end(),
        [&](const SpriteRect& sprite) {
            return !sprite.name.empty() && sprite.name == spriteToken;
        });
    return byName != settings.sprites.end() ? &*byName : nullptr;
}

bool GetCachedTextureImportSettings(
    std::string_view texturePathOrRef, TextureImportSettings& outSettings)
{
    const std::lock_guard lock(MetaCacheMutex());
    const CachedMeta* meta = AcquireMeta(MetaPathFor(texturePathOrRef));
    if (meta == nullptr || !meta->valid) return false;
    outSettings = meta->settings;
    return true;
}

void InvalidateTextureImportSettings(std::string_view texturePathOrRef)
{
    const std::lock_guard lock(MetaCacheMutex());
    MetaCache().erase(MetaPathFor(texturePathOrRef));
    /// @note 切り直した直後は «壊れている» の判定もやり直す。
    BrokenSpriteReports().clear();
}

std::string LookupSpriteId(std::string_view texturePath, std::string_view spriteToken)
{
    const std::lock_guard lock(MetaCacheMutex());
    const CachedMeta* meta = AcquireMeta(MetaPathFor(texturePath));
    if (meta == nullptr || !meta->valid) return {};
    bool byId = false;
    const SpriteRect* sprite = FindInMeta(*meta, std::string(spriteToken), byId);
    return sprite != nullptr ? sprite->id : std::string{};
}

std::string LookupSpriteName(std::string_view texturePath, std::string_view spriteToken)
{
    const std::lock_guard lock(MetaCacheMutex());
    const CachedMeta* meta = AcquireMeta(MetaPathFor(texturePath));
    if (meta == nullptr || !meta->valid) return {};
    bool byId = false;
    const SpriteRect* sprite = FindInMeta(*meta, std::string(spriteToken), byId);
    return sprite != nullptr ? sprite->name : std::string{};
}

ResolvedSprite ResolveSpriteReference(
    std::string_view reference, float textureWidth, float textureHeight)
{
    ResolvedSprite result;
    result.isSpriteReference =
        ParseSpriteReference(reference, result.texturePath, result.spriteId);

    const bool hasTextureSize = textureWidth > 0.0f && textureHeight > 0.0f;
    /// @note Sprite でない画像の「切り抜き」は画像そのもの。ここを埋めておけば、
    ///       呼ぶ側は Sprite かどうかで分岐せずに原寸やタイル寸法を出せる。
    if (hasTextureSize) result.sizePixels = { textureWidth, textureHeight };
    if (!result.isSpriteReference) {
        result.status = SpriteResolveStatus::NotASpriteReference;
        return result;
    }
    /// @note 元画像がまだ読めていないだけ。待てば直るので «壊れている» とは言わない。
    if (!hasTextureSize) {
        result.status = SpriteResolveStatus::TextureSizeUnknown;
        return result;
    }

    const std::lock_guard lock(MetaCacheMutex());
    const CachedMeta* meta = AcquireMeta(MetaPathFor(result.texturePath));
    if (meta == nullptr || !meta->valid) {
        result.status = SpriteResolveStatus::MetaMissing;
        ReportBrokenSprite(reference, "元画像の .meta を読めません (Sprite 型でインポートされていますか)");
        return result;
    }

    bool matchedById = false;
    const SpriteRect* sprite = FindInMeta(*meta, result.spriteId, matchedById);

    /// @note sprites を 1 つも持たない Single Texture は «全面 1 枚» を暗黙で持つ。
    ///       画像名で参照できるようにしておかないと、Single だけ参照の書き方が変わる。
    SpriteRect implicitSingle;
    if (sprite == nullptr
        && meta->settings.type == TextureType::Sprite
        && meta->settings.spriteMode == SpriteMode::Single) {
        implicitSingle.name = util::FileSystem::PathToUtf8(
            util::FileSystem::PathFromUtf8(result.texturePath).stem());
        if (implicitSingle.name == result.spriteId) {
            sprite = &implicitSingle;
            result.status = SpriteResolveStatus::ResolvedAsImplicitSingle;
        }
    } else if (sprite != nullptr) {
        result.status = matchedById
            ? SpriteResolveStatus::ResolvedById
            : SpriteResolveStatus::ResolvedByName;
    }

    if (sprite == nullptr) {
        result.status = SpriteResolveStatus::SpriteNotFound;
        ReportBrokenSprite(reference, "この ID / 名前の Sprite が .meta にありません");
        return result;
    }

    /// @note 幅 / 高さ 0 は「画像全体」を意味する (SpriteRect のコメント参照)。
    const float spriteWidth  = sprite->width  > 0 ? static_cast<float>(sprite->width)  : textureWidth;
    const float spriteHeight = sprite->height > 0 ? static_cast<float>(sprite->height) : textureHeight;

    result.uvMin = { static_cast<float>(sprite->x) / textureWidth,
                     static_cast<float>(sprite->y) / textureHeight };
    result.uvMax = { (static_cast<float>(sprite->x) + spriteWidth)  / textureWidth,
                     (static_cast<float>(sprite->y) + spriteHeight) / textureHeight };
    result.border = { sprite->borderLeft, sprite->borderTop,
                      sprite->borderRight, sprite->borderBottom };
    result.sizePixels    = { spriteWidth, spriteHeight };
    result.pivot         = { sprite->pivotX, sprite->pivotY };
    result.pixelsPerUnit = meta->settings.pixelsPerUnit;
    result.displayName   = sprite->name;
    result.resolved      = true;
    return result;
}

} /// @note namespace fbzz::asset
