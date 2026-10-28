/// @file    UIThemeAsset.cpp
/// @brief   Shared game UI theme reflection, registration and value resolution.
/// @author  Hasegawa Jin
/// @date    2026-10-24
#include <Engine/Asset/UIThemeAsset.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Asset/DataAssetFactory.hpp>
#include <Engine/Asset/DataAssetRegistry.hpp>
#include <Engine/Asset/GuidRefCodec.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Util/StringUtils.hpp>
#include <cmath>
#include <string_view>

namespace fbzz::asset {
namespace {

/// @note The font picker emits atlas bases; only a registered .fnt neighbor upgrades that representation to a GUID.
std::string EncodeFontReference(const std::string& reference)
{
    const std::string encoded = EncodeGuidRef(reference);
    if (reference.empty() || AssetDatabase::IsGuidRef(encoded)) return encoded;
    const std::string lower = util::StringUtils::ToLowerCopy(reference);
    if (lower.ends_with(".fnt") || lower.ends_with(".ttf")
        || lower.ends_with(".ttc") || lower.ends_with(".otf")) return encoded;
    const std::string atlas = EncodeGuidRef(reference + ".fnt");
    return AssetDatabase::IsGuidRef(atlas) ? atlas : encoded;
}

} /// @note namespace

void UIThemeAsset::Reflect(scene::IReflector& r)
{
    r.Group("Game UI Colors");
    r.ColorField("imageColor", style.imageColor);
    r.ColorField("textColor", style.textColor);
    r.Group("Button States");
    r.ColorField("buttonNormalColor", style.buttonNormalColor);
    r.ColorField("buttonHoverColor", style.buttonHoverColor);
    r.ColorField("buttonPressedColor", style.buttonPressedColor);
    r.ColorField("buttonDisabledColor", style.buttonDisabledColor);
    r.ColorField("buttonFocusColor", style.buttonFocusColor);
    r.Group("Text");
    std::string fontPath = EncodeFontReference(style.fontPath);
    r.FileField("fontPath", fontPath, ".fnt,.ttf,.ttc,.otf",
        "Font GUID, font file, or legacy atlas base path. Empty uses the project default font.");
    /// @note Generic DataAsset persistence writes reflected strings directly; encode registered font files here for rename stability.
    style.fontPath = EncodeFontReference(fontPath);
    r.FloatRange("fontSize", style.fontSize, 1.0f, 512.0f);
    r.FloatRange("letterSpacing", style.letterSpacing, 0.0f, 128.0f);
    r.FloatRange("lineSpacing", style.lineSpacing, 0.1f, 4.0f);
    r.Group("Layout Group");
    r.FloatRange("spacing", style.spacing, 0.0f, 1024.0f);
    r.FloatRange("spacingCross", style.spacingCross, 0.0f, 1024.0f);
    r.Field("padding", style.padding);
    r.Tooltip("Left, top, right, bottom padding in Canvas pixels (0..512 each).");
}

bool IsUIThemeStyleValid(const UIThemeStyle& style)
{
    const auto inRange = [](float value, float minimum, float maximum) {
        return std::isfinite(value) && value >= minimum && value <= maximum;
    };
    const auto colorValid = [&](const math::Vector4& color) {
        return inRange(color.x, 0.0f, 1.0f) && inRange(color.y, 0.0f, 1.0f)
            && inRange(color.z, 0.0f, 1.0f) && inRange(color.w, 0.0f, 1.0f);
    };
    return colorValid(style.imageColor) && colorValid(style.textColor)
        && colorValid(style.buttonNormalColor) && colorValid(style.buttonHoverColor)
        && colorValid(style.buttonPressedColor) && colorValid(style.buttonDisabledColor)
        && colorValid(style.buttonFocusColor)
        && inRange(style.fontSize, 1.0f, 512.0f)
        && inRange(style.letterSpacing, 0.0f, 128.0f)
        && inRange(style.lineSpacing, 0.1f, 4.0f)
        && inRange(style.spacing, 0.0f, 1024.0f)
        && inRange(style.spacingCross, 0.0f, 1024.0f)
        && inRange(style.padding.x, 0.0f, 512.0f) && inRange(style.padding.y, 0.0f, 512.0f)
        && inRange(style.padding.z, 0.0f, 512.0f) && inRange(style.padding.w, 0.0f, 512.0f);
}

bool ResolveUIThemeStyle(const std::string& reference, UIThemeStyle& out)
{
    if (reference.empty()) return false;
    const std::string absolute = AssetManager::ResolveAssetPath(reference);
    /// @note Registry keeps last-good cached values on failed reload; deleting the file must still select inline fallback.
    if (absolute.empty() || !util::FileSystem::Exists(absolute)) return false;
    const auto* data = DataAssetRegistry::Resolve(reference);
    if (!data || std::string_view(data->GetTypeName()) != UIThemeAsset::TYPE_NAME) return false;
    const auto& style = static_cast<const UIThemeAsset&>(*data).style;
    if (!IsUIThemeStyleValid(style)) return false;
    out = style;
    return true;
}

} /// @note namespace fbzz::asset

FBZZ_REGISTER_BUILTIN_DATA_ASSET(::fbzz::asset::UIThemeAsset);
