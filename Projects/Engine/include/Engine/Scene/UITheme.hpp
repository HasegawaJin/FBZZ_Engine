/// @file    UITheme.hpp
/// @brief   Nonmutating visual resolution used by game UI measurement, layout and rendering.
/// @author  Hasegawa Jin
/// @date    2026-10-24
#pragma once
#include <Engine/Api.hpp>
#include <Math/Vector4.hpp>
#include <string>

namespace fbzz::asset { struct UIThemeStyle; }
namespace fbzz::scene {
struct UIButton;
struct UIImage;
struct UIText;
struct UILayoutGroup;

/// @note Accepts a dynamic font file, a .fnt file GUID/path, or the existing extensionless atlas base path.
[[nodiscard]] FBZZ_ENGINE_API std::string ResolveUIFontBasePath(const std::string& reference);

/// @note Pass a working copy; serialized component values remain the fallback for removal or runtime theme switching.
FBZZ_ENGINE_API void ApplyUIThemeToText(const asset::UIThemeStyle& style, UIText& workingCopy);
FBZZ_ENGINE_API void ApplyUIThemeToLayout(const asset::UIThemeStyle& style, UILayoutGroup& workingCopy);
[[nodiscard]] FBZZ_ENGINE_API math::Vector4 ResolveUIImageColor(const UIImage& image, const asset::UIThemeStyle* style);

/// @note State priority is disabled, pressed, focused, hovered, normal. Focus is UINavigation::focused on the same object.
/// @note Themed state colors multiply image tint; unthemed/local overrides preserve the existing relative tint behavior.
[[nodiscard]] FBZZ_ENGINE_API math::Vector4 ResolveUIButtonImageColor(const math::Vector4& imageColor,
    const UIButton& button, const asset::UIThemeStyle* style, bool focused);

} /// @note namespace fbzz::scene
