/// @file    InputBindingsDocument.hpp
/// @brief   実行中の入力へ触れずに編集・検証できるバインド設定。
/// @author  Hasegawa Jin
/// @date    2026-11-03
#pragma once
#include <Engine/Api.hpp>
#include <Engine/Input/InputBinding.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace fbzz::input {
struct InputBindingsDocument {
    std::vector<InputAction> actions;
    std::vector<InputAxis> axes;
    /// @note TOML解析に失敗した場合は現在の設定を保持する。グローバル入力は変更しない。
    [[nodiscard]] FBZZ_ENGINE_API bool LoadFromToml(std::string_view text);
    [[nodiscard]] FBZZ_ENGINE_API std::string ToToml(bool explicitDefaults = false) const;
};
} /// @note namespace fbzz::input
