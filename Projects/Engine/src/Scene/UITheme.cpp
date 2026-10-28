/// @file    UITheme.cpp
/// @brief   Production theme application with explicit local component overrides.
/// @author  Hasegawa Jin
/// @date    2026-10-24
#include <Engine/Scene/UITheme.hpp>
#include <Engine/Asset/UIThemeAsset.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Renderer/FontAtlas.hpp>
#include <Engine/Scene/Components/UIButton.hpp>
#include <Engine/Scene/Components/UIImage.hpp>
#include <Engine/Scene/Components/UIText.hpp>
#include <Engine/Scene/Components/UILayoutGroup.hpp>

namespace fbzz::scene {

std::string ResolveUIFontBasePath(const std::string& reference)
{
    if (reference.empty()) return {};
    const std::string extension = ".fnt";
    if (reference.starts_with("guid:") || reference.ends_with(extension)
        || renderer::FontAtlas::IsDynamicFontPath(reference)) {
        std::string resolved = asset::AssetManager::ResolveAssetPath(reference);
        if (resolved.ends_with(extension)) resolved.resize(resolved.size() - extension.size());
        return resolved;
    }
    std::string resolved = asset::AssetManager::ResolveAssetPath(reference + extension);
    if (resolved.ends_with(extension)) return resolved.substr(0, resolved.size() - extension.size());
    return reference;
}

void ApplyUIThemeToText(const asset::UIThemeStyle& style, UIText& text)
{
    if (!text.overrideThemeColor) text.color = style.textColor;
    if (!text.overrideThemeFont) text.fontPath = style.fontPath;
    if (!text.overrideThemeTypography) {
        text.fontSize = style.fontSize;
        text.letterSpacing = style.letterSpacing;
        text.lineSpacing = style.lineSpacing;
    }
}

void ApplyUIThemeToLayout(const asset::UIThemeStyle& style, UILayoutGroup& layout)
{
    if (!layout.overrideThemeSpacing) {
        layout.spacing = style.spacing;
        layout.spacingCross = style.spacingCross;
    }
    if (!layout.overrideThemePadding) {
        layout.paddingLeft = style.padding.x;
        layout.paddingTop = style.padding.y;
        layout.paddingRight = style.padding.z;
        layout.paddingBottom = style.padding.w;
    }
}

math::Vector4 ResolveUIImageColor(const UIImage& image, const asset::UIThemeStyle* style)
{
    return style && !image.overrideThemeColor ? style->imageColor : image.color;
}

math::Vector4 ResolveUIButtonImageColor(const math::Vector4& imageColor,
    const UIButton& button, const asset::UIThemeStyle* style, bool focused)
{
    if (style && !button.overrideThemeColors) {
        const math::Vector4& tint = !button.enabled || !button.isInteractable ? style->buttonDisabledColor
            : button.state == UIButtonState::PRESSED ? style->buttonPressedColor
            : focused ? style->buttonFocusColor
            : button.state == UIButtonState::HOVERED ? style->buttonHoverColor : style->buttonNormalColor;
        return {imageColor.x * tint.x, imageColor.y * tint.y,
            imageColor.z * tint.z, imageColor.w * tint.w};
    }
    const math::Vector4& tint = !button.enabled || !button.isInteractable ? button.disabledColor
        : button.state == UIButtonState::PRESSED ? button.pressedColor
        : button.state == UIButtonState::HOVERED ? button.hoverColor : button.normalColor;
    constexpr float MIN_COLOR_CHANNEL = 1.0e-5f;
    const auto relativeChannel = [=](float image, float normal, float target) {
        return normal > MIN_COLOR_CHANNEL ? image * (target / normal) : target;
    };
    return {relativeChannel(imageColor.x, button.normalColor.x, tint.x),
        relativeChannel(imageColor.y, button.normalColor.y, tint.y),
        relativeChannel(imageColor.z, button.normalColor.z, tint.z),
        relativeChannel(imageColor.w, button.normalColor.w, tint.w)};
}

} /// @note namespace fbzz::scene
