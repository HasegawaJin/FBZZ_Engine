/// @file    UIThemeAsset.hpp
/// @brief   Canvas ベースのゲーム UI で共有する見た目の設定を永続化する。
/// @author  Hasegawa Jin
/// @date    2026-10-24
#pragma once
#include <Engine/Api.hpp>
#include <Engine/Asset/Data/DataAsset.hpp>
#include <Math/LinearAlgebra/Vector4.hpp>
#include <string>

namespace fbzz::asset {

/// @note Canvas のピクセル単位は UIText と UILayoutGroup に準じる。lineSpacing は行高に掛ける倍率。
/// @note フォント参照は GUID、プロジェクトパス、旧形式のアトラスのベースパスのいずれか。GPU オブジェクトは保持しない。
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
    FBZZ_ENGINE_API void Reflect(scene::IReflector& reflector) override;
    UIThemeStyle style;
};

[[nodiscard]] FBZZ_ENGINE_API bool IsUIThemeStyleValid(const UIThemeStyle& style);

/// @pre UISystem の既存の入口と同様、AssetManager と DataAssetRegistry の所有スレッドから呼ぶ。
/// @return アセットが未指定、削除済み、別型または不正なら false。out は変更しない。
/// @note Canvas パスごとに値をコピーする。編集、再読込、キャッシュ破棄、デバイスリセットをまたぐポインターは保持しない。
[[nodiscard]] FBZZ_ENGINE_API bool ResolveUIThemeStyle(const std::string& reference, UIThemeStyle& out);

} // namespace fbzz::asset
