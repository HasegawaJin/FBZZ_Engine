// FBZZ Engine
// TexDescSerializer.cpp | fbzz::asset
// .tex TOML descriptor の読み書き
#include <Engine/Asset/TexDescSerializer.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <toml++/toml.hpp>
#include <filesystem>
#include <sstream>

namespace fbzz::asset {

namespace {

const char* TypeToStr(TextureType t) {
    switch (t) {
    case TextureType::Color:  return "color";
    case TextureType::Normal: return "normal";
    case TextureType::Data:   return "data";
    case TextureType::HDR:    return "hdr";
    case TextureType::UI:     return "ui";
    }
    return "color";
}

TextureType StrToType(std::string_view s) {
    if (s == "normal") return TextureType::Normal;
    if (s == "data")   return TextureType::Data;
    if (s == "hdr")    return TextureType::HDR;
    if (s == "ui")     return TextureType::UI;
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

    toml::table tex;
    tex.insert("source",              asset.sourcePath);
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

    toml::table root;
    root.insert("texture", std::move(tex));

    std::ostringstream ss;
    ss << root;
    return util::FileSystem::WriteText(absPath, ss.str());
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
        FBZZ_LOG_ERROR("TexDescSerializer: missing [texture] section [%s]", absPath.c_str());
        return false;
    }

    if (auto v = (*tex)["source"].value<std::string>()) outAsset.sourcePath = *v;

    TextureImportSettings& s = outAsset.settings;
    if (auto v = (*tex)["type"].value<std::string>())              s.type = StrToType(*v);
    // type が決まったらデフォルトを入れる (明示フィールドで上書き)
    s = DefaultSettingsForType(s.type);
    if (auto v = (*tex)["source"].value<std::string>())            outAsset.sourcePath = *v;

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

    return true;
}

} // namespace fbzz::asset
