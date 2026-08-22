// FBZZ Engine
// TextureAsset.cpp | fbzz::asset
#include <Engine/Asset/TextureAsset.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/TexDescSerializer.hpp>
#include <algorithm>
#include <cctype>
#include <string_view>

namespace fbzz::asset {

TextureImportSettings DefaultSettingsForType(TextureType type)
{
    TextureImportSettings s;
    switch (type) {
    case TextureType::Color:
        s.type        = TextureType::Color;
        s.srgb        = true;
        s.compression = TextureCompression::Auto; // BC1/BC3 depending on alpha
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
        // SpriteはUIと同じサンプリング既定値を使い、矩形境界からの色漏れを防ぐ。
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
    // ファイル名末尾のステムを小文字で検索する。
    // 例: "wall_n.png" → stem = "wall_n" → Normal
    std::string lower;
    lower.reserve(filename.size());
    for (char c : filename) lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));

    // 拡張子を除く
    const auto dotPos = lower.rfind('.');
    std::string_view stem = (dotPos != std::string_view::npos)
        ? std::string_view(lower).substr(0, dotPos)
        : std::string_view(lower);

    // HDR ファイルは拡張子で判定
    if (lower.ends_with(".hdr") || lower.ends_with(".exr"))
        return TextureType::HDR;

    // UI hint
    if (stem.ends_with("_ui") || stem.ends_with("_icon") ||
        stem.ends_with("_hud") || stem.starts_with("ui_"))
        return TextureType::UI;

    // Normal map suffixes
    static constexpr std::string_view kNormalSuffixes[] = {
        "_n", "_nrm", "_normal", "_nmap", "_bump", "_normalmap"
    };
    for (auto& suf : kNormalSuffixes)
        if (stem.ends_with(suf)) return TextureType::Normal;

    // Data / linear map suffixes (roughness, metallic, AO, opacity, mask, emissive mask)
    static constexpr std::string_view kDataSuffixes[] = {
        "_r", "_rough", "_roughness",
        "_m", "_metal", "_metallic",
        "_ao", "_occlusion", "_occ",
        "_mask", "_opacity", "_alpha",
        "_d",  // displacement
        "_h",  // height
    };
    for (auto& suf : kDataSuffixes)
        if (stem.ends_with(suf)) return TextureType::Data;

    // Emissive is still sRGB color
    if (stem.ends_with("_e") || stem.ends_with("_emissive") || stem.ends_with("_emission"))
        return TextureType::Color;

    return TextureType::Color;
}

std::string MakeSpriteReference(
    std::string_view texturePath, std::string_view spriteId)
{
    constexpr std::string_view MARKER = "::sprite::";
    std::string result(texturePath);
    result.append(MARKER);
    result.append(spriteId);
    return result;
}

bool ParseSpriteReference(
    std::string_view reference, std::string& outTexturePath, std::string& outSpriteName)
{
    constexpr std::string_view MARKER = "::sprite::";
    const size_t markerPos = reference.find(MARKER);
    if (markerPos == std::string_view::npos || markerPos == 0
        || markerPos + MARKER.size() >= reference.size()) {
        outTexturePath.assign(reference);
        outSpriteName.clear();
        return false;
    }
    outTexturePath.assign(reference.substr(0, markerPos));
    outSpriteName.assign(reference.substr(markerPos + MARKER.size()));
    return true;
}

const SpriteRect* FindSprite(
    const TextureImportSettings& settings, std::string_view spriteId)
{
    const auto found = std::find_if(settings.sprites.begin(), settings.sprites.end(),
        [&](const SpriteRect& sprite) {
            return !sprite.id.empty() && sprite.id == spriteId;
        });
    return found != settings.sprites.end() ? &*found : nullptr;
}

ResolvedSprite ResolveSpriteReference(
    std::string_view reference, float textureWidth, float textureHeight)
{
    ResolvedSprite result;
    result.isSpriteReference =
        ParseSpriteReference(reference, result.texturePath, result.spriteId);

    const bool hasTextureSize = textureWidth > 0.0f && textureHeight > 0.0f;
    // Sprite でない画像の「切り抜き」は画像そのもの。ここを埋めておけば、
    // 呼ぶ側は Sprite かどうかで分岐せずに原寸やタイル寸法を出せる。
    if (hasTextureSize) result.sizePixels = { textureWidth, textureHeight };
    if (!result.isSpriteReference || !hasTextureSize) return result;

    TextureAsset textureAsset;
    const TexDescSerializer serializer;
    const std::string metaPath =
        AssetManager::ResolveAssetPath(result.texturePath + ".meta");
    if (!serializer.Load(metaPath, textureAsset)) return result;

    const SpriteRect* sprite = FindSprite(textureAsset.settings, result.spriteId);
    if (sprite == nullptr) return result;

    // 幅 / 高さ 0 は「画像全体」を意味する (SpriteRect のコメント参照)。
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
    result.pixelsPerUnit = textureAsset.settings.pixelsPerUnit;
    result.resolved      = true;
    return result;
}

} // namespace fbzz::asset
