// FBZZ Engine
// TextureAsset.cpp | fbzz::asset
#include <Engine/Asset/TextureAsset.hpp>
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
    std::string_view texturePath, std::string_view spriteIdOrLegacyName)
{
    constexpr std::string_view MARKER = "::sprite::";
    std::string result(texturePath);
    result.append(MARKER);
    result.append(spriteIdOrLegacyName);
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
    const TextureImportSettings& settings, std::string_view spriteName)
{
    const auto found = std::find_if(settings.sprites.begin(), settings.sprites.end(),
        [&](const SpriteRect& sprite) {
            // 新規参照は永続 ID、旧 .scene / .mat は表示名を保持しているため両方を解決する。
            return (!sprite.id.empty() && sprite.id == spriteName)
                || sprite.name == spriteName
                || std::find(sprite.legacyNames.begin(), sprite.legacyNames.end(), spriteName)
                    != sprite.legacyNames.end();
        });
    return found != settings.sprites.end() ? &*found : nullptr;
}

} // namespace fbzz::asset
