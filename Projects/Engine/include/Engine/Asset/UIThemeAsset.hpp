/// @file    UIThemeAsset.hpp
/// @brief   Persistent shared visual values for Canvas-based game UI.
/// @author  Hasegawa Jin
/// @date    2026-10-24
#pragma once
#include <Engine/Asset/DataAsset.hpp>
#include <Math/Vector4.hpp>
#include <string>

namespace fbzz::asset {

/// @note Canvas pixels match UIText and UILayoutGroup; lineSpacing is a line-height multiplier.
/// @note Font references contain a GUID, project path, or legacy atlas base path; GPU objects are never stored here.
struct UIThemeStyle {
    math::Vector4 imageColor{1.0f, 1.0f, 1.0f, 1.0f};
    math::Vector4 textColor{1.0f, 1.0f, 1.0f, 1.0f};
    math::Vector4 buttonNormalColor{1.0f, 1.0f, 1.0f, 1.0f};
    math::Vector4 buttonHoverColor{0.85f, 0.85f, 0.85f, 1.0f};
    math::Vector4 buttonPressedColor{0.7f, 0.7f, 0.7f, 1.0f};
    math::Vector4 buttonDisabledColor{0.5f, 0.5f, 0.5f, 0.5f};
    math::Vector4 buttonFocusColor{0.85f, 0.85f, 0.85f, 1.0f};
    std::string fontPath;
    float fontSize = 42.0f;
    float letterSpacing = 4.0f;
    float lineSpacing = 1.0f;
    float spacing = 8.0f;
    float spacingCross = 8.0f;
    math::Vector4 padding{};
};

class UIThemeAsset final : public DataAsset {
public:
    static constexpr const char* TYPE_NAME = "UIThemeAsset";
    const char* GetTypeName() const override { return TYPE_NAME; }
    void Reflect(scene::IReflector& reflector) override;
    UIThemeStyle style;
};

[[nodiscard]] bool IsUIThemeStyleValid(const UIThemeStyle& style);

/// @pre Call on the AssetManager/DataAssetRegistry owner thread, like the existing UISystem entry point.
/// @return False leaves out unchanged for an empty, removed, wrong-type or invalid asset.
/// @note Copies values per Canvas pass; no pointer is retained across edits, reloads, cache clears or device resets.
[[nodiscard]] bool ResolveUIThemeStyle(const std::string& reference, UIThemeStyle& out);

} /// @note namespace fbzz::asset
