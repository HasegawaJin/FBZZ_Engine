/// @file    TexDescSerializer.cpp
/// @brief   .tex TOML descriptor の読み書き。
/// @author  Hasegawa Jin
/// @date    2026-06-18
#include <Engine/Asset/TexDescSerializer.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <toml++/toml.hpp>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <sstream>

namespace fbzz::asset {

namespace {

std::uint64_t HashSpriteIdentity(std::string_view value, std::uint64_t seed)
{
    std::uint64_t hash = seed;
    for (const unsigned char c : value) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    return hash;
}

/// @brief ID を持たない旧 .meta へ、パス・名前・並び順から再現可能な ID を割り当てる。
/// @note 読み込みのたびにランダム ID を作ると、移行保存前に生成した参照が次回起動で切れる。
std::string MakeSpriteId(
    std::string_view assetIdentity, std::string_view name, std::size_t index)
{
    std::string key(assetIdentity);
    key.push_back('|');
    key.append(name);
    key.push_back('|');
    key.append(std::to_string(index));
    const std::uint64_t high = HashSpriteIdentity(key, 1469598103934665603ULL);
    const std::uint64_t low = HashSpriteIdentity(key, 1099511628211ULL);
    char buffer[40];
    std::snprintf(buffer, sizeof(buffer), "sprite-%016llx%016llx",
        static_cast<unsigned long long>(high),
        static_cast<unsigned long long>(low));
    return buffer;
}

const char* TypeToStr(TextureType t) {
    switch (t) {
    case TextureType::Color:  return "color";
    case TextureType::Normal: return "normal";
    case TextureType::Data:   return "data";
    case TextureType::HDR:    return "hdr";
    case TextureType::UI:     return "ui";
    case TextureType::Sprite: return "sprite";
    }
    return "color";
}

TextureType StrToType(std::string_view s) {
    if (s == "normal") return TextureType::Normal;
    if (s == "data")   return TextureType::Data;
    if (s == "hdr")    return TextureType::HDR;
    if (s == "ui")     return TextureType::UI;
    if (s == "sprite") return TextureType::Sprite;
    return TextureType::Color;
}

const char* CompToStr(TextureCompression c) {
    switch (c) {
    case TextureCompression::Auto: return "Auto";
    case TextureCompression::BC1:  return "BC1";
    case TextureCompression::BC3:  return "BC3";
    case TextureCompression::BC4:  return "BC4";
    case TextureCompression::BC5:  return "BC5";
    case TextureCompression::BC6H: return "BC6H";
    case TextureCompression::BC7:  return "BC7";
    case TextureCompression::None: return "None";
    }
    return "Auto";
}

TextureCompression StrToComp(std::string_view s) {
    if (s == "BC1")  return TextureCompression::BC1;
    if (s == "BC3")  return TextureCompression::BC3;
    if (s == "BC4")  return TextureCompression::BC4;
    if (s == "BC5")  return TextureCompression::BC5;
    if (s == "BC6H") return TextureCompression::BC6H;
    if (s == "BC7")  return TextureCompression::BC7;
    if (s == "None") return TextureCompression::None;
    return TextureCompression::Auto;
}

const char* FilterToStr(TextureFilter f) {
    switch (f) {
    case TextureFilter::Point:       return "Point";
    case TextureFilter::Bilinear:    return "Bilinear";
    case TextureFilter::Trilinear:   return "Trilinear";
    case TextureFilter::Anisotropic: return "Anisotropic";
    }
    return "Trilinear";
}

TextureFilter StrToFilter(std::string_view s) {
    if (s == "Point")       return TextureFilter::Point;
    if (s == "Bilinear")    return TextureFilter::Bilinear;
    if (s == "Anisotropic") return TextureFilter::Anisotropic;
    return TextureFilter::Trilinear;
}

const char* WrapToStr(TextureWrap w) {
    switch (w) {
    case TextureWrap::Repeat: return "Repeat";
    case TextureWrap::Clamp:  return "Clamp";
    case TextureWrap::Mirror: return "Mirror";
    case TextureWrap::Border: return "Border";
    }
    return "Repeat";
}

TextureWrap StrToWrap(std::string_view s) {
    if (s == "Clamp")  return TextureWrap::Clamp;
    if (s == "Mirror") return TextureWrap::Mirror;
    if (s == "Border") return TextureWrap::Border;
    return TextureWrap::Repeat;
}

const char* MipFilterToStr(MipFilter f) {
    switch (f) {
    case MipFilter::Box:     return "Box";
    case MipFilter::Kaiser:  return "Kaiser";
    case MipFilter::Lanczos: return "Lanczos";
    }
    return "Kaiser";
}

MipFilter StrToMipFilter(std::string_view s) {
    if (s == "Box")     return MipFilter::Box;
    if (s == "Lanczos") return MipFilter::Lanczos;
    return MipFilter::Kaiser;
}

const char* AlphaModeToStr(AlphaMode a) {
    switch (a) {
    case AlphaMode::Straight:     return "Straight";
    case AlphaMode::Premultiplied: return "Premultiplied";
    case AlphaMode::None:         return "None";
    }
    return "Straight";
}

AlphaMode StrToAlphaMode(std::string_view s) {
    if (s == "Premultiplied") return AlphaMode::Premultiplied;
    if (s == "None")          return AlphaMode::None;
    return AlphaMode::Straight;
}

} // namespace

bool TexDescSerializer::Save(const TextureAsset& asset, const std::string& absPath) const
{
    const TextureImportSettings& s = asset.settings;

    /// @note 既存 .meta の [meta] セクション (guid 等) を先に読む。guid は Sprite ID を
    ///       補完する種でもあり、Load 側も [meta] guid を種にしているため、ここで absPath を
    ///       種にすると同じ Sprite に別の ID が付き、書いた瞬間に全参照が切れる。
    /// @note guid は AssetDatabase が発行する恒久 ID。テクスチャ設定の保存で消すとこの画像への
    ///       参照が全て切れるため、[texture] 以外は必ず残す。
    toml::table root;
    std::string spriteIdentity = absPath;
    std::string existing;
    if (util::FileSystem::ReadText(absPath, existing)) {
        std::istringstream iss(existing);
        const auto parsed = toml::parse(iss);
        if (parsed) {
            if (const auto* field = parsed.table()["vector_field"].as_table())
                root.insert("vector_field", *field);
            if (const auto* meta = parsed.table()["meta"].as_table()) {
                root.insert("meta", *meta);
                if (auto guid = (*meta)["guid"].value<std::string>(); guid && !guid->empty())
                    spriteIdentity = *guid;
            }
        }
    }

    toml::table tex;
    /// @note source= は持たない。元画像は `<name>.<ext>.meta` から末尾 `.meta` を除いて導出する。
    tex.insert("type",                std::string(TypeToStr(s.type)));
    tex.insert("srgb",                s.srgb);
    tex.insert("compression",         std::string(CompToStr(s.compression)));
    tex.insert("compression_quality", static_cast<int64_t>(static_cast<int>(s.compressionQuality)));
    tex.insert("mipmaps",             s.mipmaps);
    tex.insert("mip_filter",          std::string(MipFilterToStr(s.mipFilter)));
    tex.insert("mip_sharpen",         static_cast<double>(s.mipSharpen));
    tex.insert("mip_bias",            static_cast<double>(s.mipBias));
    tex.insert("normalize_mipmaps",   s.normalizeMipmaps);
    tex.insert("flip_green",          s.flipGreen);
    tex.insert("max_size",            static_cast<int64_t>(s.maxSize));
    tex.insert("wrap_u",              std::string(WrapToStr(s.wrapU)));
    tex.insert("wrap_v",              std::string(WrapToStr(s.wrapV)));
    tex.insert("filter",              std::string(FilterToStr(s.filter)));
    tex.insert("aniso",               static_cast<int64_t>(s.anisoLevel));
    tex.insert("alpha_mode",          std::string(AlphaModeToStr(s.alphaMode)));
    tex.insert("alpha_dither",        s.alphaDither);
    if (s.type == TextureType::Sprite) {
        tex.insert("sprite_mode",
                   s.spriteMode == SpriteMode::Multiple ? "Multiple" : "Single");
        tex.insert("pixels_per_unit", static_cast<double>(s.pixelsPerUnit));

        std::vector<SpriteRect> spriteSources = s.sprites;
        if (spriteSources.empty()) {
            SpriteRect sprite;
            sprite.name = util::FileSystem::PathToUtf8(
                util::FileSystem::PathFromUtf8(asset.sourcePath).stem());
            if (sprite.name.empty()) sprite.name = "Sprite";
            spriteSources.push_back(std::move(sprite));
        }

        /// @note 名前は「別名キー」なので、テクスチャ内で一意でなければ参照が曖昧になる。
        ///       直すのは編集側 (Sprite Editor) の仕事なので、ここでは黙って書き換えず報告だけする。
        for (std::size_t i = 0; i < spriteSources.size(); ++i) {
            for (std::size_t j = i + 1; j < spriteSources.size(); ++j) {
                if (spriteSources[i].name.empty()
                    || spriteSources[i].name != spriteSources[j].name) continue;
                FBZZ_LOG_WARN("TexDescSerializer: duplicated sprite name '%s' in [%s]. "
                              "名前で書いた参照はどちらを指すか決まりません。",
                              spriteSources[i].name.c_str(), absPath.c_str());
                break;
            }
        }

        toml::array sprites;
        for (std::size_t index = 0; index < spriteSources.size(); ++index) {
            const SpriteRect& source = spriteSources[index];
            toml::table sprite;
            sprite.insert("id", source.id.empty()
                ? MakeSpriteId(spriteIdentity, source.name, index) : source.id);
            sprite.insert("name", source.name);
            sprite.insert("x", static_cast<int64_t>(source.x));
            sprite.insert("y", static_cast<int64_t>(source.y));
            sprite.insert("width", static_cast<int64_t>(source.width));
            sprite.insert("height", static_cast<int64_t>(source.height));
            sprite.insert("pivot_x", static_cast<double>(source.pivotX));
            sprite.insert("pivot_y", static_cast<double>(source.pivotY));
            sprite.insert("border_left", static_cast<double>(source.borderLeft));
            sprite.insert("border_top", static_cast<double>(source.borderTop));
            sprite.insert("border_right", static_cast<double>(source.borderRight));
            sprite.insert("border_bottom", static_cast<double>(source.borderBottom));
            sprites.push_back(std::move(sprite));
        }
        tex.insert("sprites", std::move(sprites));
    }

    root.insert("texture", std::move(tex));

    std::ostringstream ss;
    ss << root;
    if (!util::FileSystem::WriteText(absPath, ss.str())) return false;

    /// @note 書いた本人が共有キャッシュを潰す。書き込み時刻でも気付けるが、
    ///       同一秒内の連続 Apply では時刻が動かないことがある。
    InvalidateTextureImportSettings(absPath);
    return true;
}

bool TexDescSerializer::Load(const std::string& absPath, TextureAsset& outAsset) const
{
    std::string text;
    if (!util::FileSystem::ReadText(absPath, text)) {
        FBZZ_LOG_WARN("TexDescSerializer: cannot read [%s]", absPath.c_str());
        return false;
    }
    std::istringstream iss(text);
    const auto parsed = toml::parse(iss);
    if (!parsed) {
        FBZZ_LOG_ERROR("TexDescSerializer: TOML parse failed [%s]", absPath.c_str());
        return false;
    }
    const auto& tbl = parsed.table();
    const auto* tex = tbl["texture"].as_table();
    if (!tex) {
        /// @note guid のみの .meta ([meta] セクションだけ) は正当な形式。
        ///       テクスチャ設定なし = デフォルト適用なので、エラーではなく静かに false を返す。
        return false;
    }

    /// @note sourcePath はサイドカーには書かれない。呼び出し元 (ImageImporter) が元画像パスを設定する。
    TextureImportSettings& s = outAsset.settings;
    const std::string spriteIdentity = tbl["meta"]["guid"].value<std::string>()
        .value_or(absPath);
    if (auto v = (*tex)["type"].value<std::string>())              s.type = StrToType(*v);
    /// @note type が決まったらデフォルトを入れる (明示フィールドで上書き)
    s = DefaultSettingsForType(s.type);

    if (auto v = (*tex)["srgb"].value<bool>())                     s.srgb              = *v;
    if (auto v = (*tex)["compression"].value<std::string>())       s.compression       = StrToComp(*v);
    if (auto v = (*tex)["compression_quality"].value<int64_t>())   s.compressionQuality= static_cast<CompQuality>(*v);
    if (auto v = (*tex)["mipmaps"].value<bool>())                  s.mipmaps           = *v;
    if (auto v = (*tex)["mip_filter"].value<std::string>())        s.mipFilter         = StrToMipFilter(*v);
    if (auto v = (*tex)["mip_sharpen"].value<double>())            s.mipSharpen        = static_cast<float>(*v);
    if (auto v = (*tex)["mip_bias"].value<double>())               s.mipBias           = static_cast<float>(*v);
    if (auto v = (*tex)["normalize_mipmaps"].value<bool>())        s.normalizeMipmaps  = *v;
    if (auto v = (*tex)["flip_green"].value<bool>())               s.flipGreen         = *v;
    if (auto v = (*tex)["max_size"].value<int64_t>())              s.maxSize           = static_cast<uint32_t>(*v);
    if (auto v = (*tex)["wrap_u"].value<std::string>())            s.wrapU             = StrToWrap(*v);
    if (auto v = (*tex)["wrap_v"].value<std::string>())            s.wrapV             = StrToWrap(*v);
    if (auto v = (*tex)["filter"].value<std::string>())            s.filter            = StrToFilter(*v);
    if (auto v = (*tex)["aniso"].value<int64_t>())                 s.anisoLevel        = static_cast<uint32_t>(*v);
    if (auto v = (*tex)["alpha_mode"].value<std::string>())        s.alphaMode         = StrToAlphaMode(*v);
    if (auto v = (*tex)["alpha_dither"].value<bool>())             s.alphaDither       = *v;
    if (auto v = (*tex)["sprite_mode"].value<std::string>())
        s.spriteMode = *v == "Multiple" ? SpriteMode::Multiple : SpriteMode::Single;
    if (auto v = (*tex)["pixels_per_unit"].value<double>())
        s.pixelsPerUnit = std::max(0.001f, static_cast<float>(*v));
    if (const auto* sprites = (*tex)["sprites"].as_array()) {
        s.sprites.clear();
        std::size_t spriteIndex = 0;
        for (const auto& node : *sprites) {
            const auto* spriteTable = node.as_table();
            if (spriteTable == nullptr) continue;
            SpriteRect sprite;
            if (auto v = (*spriteTable)["id"].value<std::string>()) sprite.id = *v;
            if (auto v = (*spriteTable)["name"].value<std::string>()) sprite.name = *v;
            if (auto v = (*spriteTable)["x"].value<int64_t>()) sprite.x = static_cast<uint32_t>(std::max<int64_t>(0, *v));
            if (auto v = (*spriteTable)["y"].value<int64_t>()) sprite.y = static_cast<uint32_t>(std::max<int64_t>(0, *v));
            if (auto v = (*spriteTable)["width"].value<int64_t>()) sprite.width = static_cast<uint32_t>(std::max<int64_t>(0, *v));
            if (auto v = (*spriteTable)["height"].value<int64_t>()) sprite.height = static_cast<uint32_t>(std::max<int64_t>(0, *v));
            if (auto v = (*spriteTable)["pivot_x"].value<double>()) sprite.pivotX = std::clamp(static_cast<float>(*v), 0.0f, 1.0f);
            if (auto v = (*spriteTable)["pivot_y"].value<double>()) sprite.pivotY = std::clamp(static_cast<float>(*v), 0.0f, 1.0f);
            if (auto v = (*spriteTable)["border_left"].value<double>()) sprite.borderLeft = std::max(0.0f, static_cast<float>(*v));
            if (auto v = (*spriteTable)["border_top"].value<double>()) sprite.borderTop = std::max(0.0f, static_cast<float>(*v));
            if (auto v = (*spriteTable)["border_right"].value<double>()) sprite.borderRight = std::max(0.0f, static_cast<float>(*v));
            if (auto v = (*spriteTable)["border_bottom"].value<double>()) sprite.borderBottom = std::max(0.0f, static_cast<float>(*v));
            if (!sprite.name.empty()) {
                if (sprite.id.empty())
                sprite.id = MakeSpriteId(
                        spriteIdentity, sprite.name, spriteIndex);
                s.sprites.push_back(std::move(sprite));
                ++spriteIndex;
            }
        }
    }

    return true;
}

bool TexDescSerializer::ResolveSourcePath(
    std::string_view texturePath, std::string& outSourcePath)
{
    outSourcePath.clear();
    if (texturePath.empty()) return false;

    std::string inputPath;
    std::string spriteName;
    (void)ParseSpriteReference(texturePath, inputPath, spriteName);
    std::string extension = util::FileSystem::GetExtension(inputPath);
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    /// @note 生画像は既存パスをそのままロードする。
    if (extension != ".meta") {
        outSourcePath = inputPath;
        return true;
    }

    /// @note 二重拡張子サイドカー: `Foo.png.meta` から末尾 `.meta` を除いた `Foo.png` が元画像。
    ///       source= を持たず、ファイル名だけで元画像を一意に導出する (TOML パース不要で高速)。
    constexpr std::string_view kMetaExt = ".meta";
    outSourcePath = inputPath.substr(0, inputPath.size() - kMetaExt.size());

    /// @note メタの入れ子 ("Foo.meta.meta") や拡張子なしは不正。元画像拡張子が再び .meta なら失敗させる。
    std::string sourceExtension = util::FileSystem::GetExtension(outSourcePath);
    std::transform(sourceExtension.begin(), sourceExtension.end(), sourceExtension.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (sourceExtension.empty() || sourceExtension == ".meta") {
        FBZZ_LOG_ERROR("TexDescSerializer: invalid .meta source path [%s]", inputPath.c_str());
        outSourcePath.clear();
        return false;
    }
    return true;
}

} // namespace fbzz::asset
