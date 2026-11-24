/// @file    SettingsDocument.hpp
/// @brief   設定のTOMLとOperatorデータを変換し、部分変更の保持を検証する。
/// @author  Hasegawa Jin
/// @date    2026-11-03
#pragma once
#include <Editor/Op/OpData.hpp>
#include <string>

namespace fbzz::editor::settingsdocument {
[[nodiscard]] OpData Inspect(const std::string& text);
/// @note 配列の要素と省略項目は呼び出し側の型付きローダーとPreservesで検証する。
[[nodiscard]] bool Patch(const std::string& text, const OpData& patch, std::string& output);
/// @note 型付きローダーが無視・丸め・削除した要求値を成功として返さない。
[[nodiscard]] bool Preserves(const std::string& text, const OpData& patch);
} /// @note namespace fbzz::editor::settingsdocument
