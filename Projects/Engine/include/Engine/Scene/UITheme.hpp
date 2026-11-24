/// @file    UITheme.hpp
/// @brief   ゲーム UI の計測、配置、描画で共有する、元データを変更しない表示スタイルの解決。
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

/// @note 動的フォントファイル、.fnt の GUID またはパス、従来の拡張子なしアトラス基底パスを受け取る。
[[nodiscard]] FBZZ_ENGINE_API std::string ResolveUIFontBasePath(const std::string& reference);

/// @note 作業用のコピーを渡す。保存済みのコンポーネント値はテーマ解除・ランタイム切替時のフォールバックとして保持する。
FBZZ_ENGINE_API void ApplyUIThemeToText(const asset::UIThemeStyle& style, UIText& workingCopy);
FBZZ_ENGINE_API void ApplyUIThemeToLayout(const asset::UIThemeStyle& style, UILayoutGroup& workingCopy);
[[nodiscard]] FBZZ_ENGINE_API math::Vector4 ResolveUIImageColor(const UIImage& image, const asset::UIThemeStyle* style);

/// @note 状態の優先順は無効、押下、フォーカス、ホバー、通常。フォーカスは同じ個体の UINavigation::focused で示す。
/// @note テーマの状態色は画像の色に乗算する。テーマ未使用時やローカル上書き時は既存の相対色を維持する。
[[nodiscard]] FBZZ_ENGINE_API math::Vector4 ResolveUIButtonImageColor(const math::Vector4& imageColor,
    const UIButton& button, const asset::UIThemeStyle* style, bool focused);

}
