/// @file    DeveloperMode.hpp
/// @brief   エンジン開発者向け機能を出すかどうかの、プロセスに 1 つの判定。
/// @author  Hasegawa Jin
/// @date    2026-09-19
/// @see Docs/design/developer-mode.md
#pragma once

namespace fbzz::core {

/// @note 有効 = 起動で強制 (--developer / FBZZ_DEVELOPER_MODE=1) または設定。主スレッドから読み書きする。
class DeveloperMode {
public:
    /// @brief 起動引数と環境変数から «起動で強制» を決める。入口が 1 回呼ぶ。
    static void InitFromCommandLine();

    [[nodiscard]] static bool IsEnabled();

    /// @brief 起動引数か環境変数でオンにされているか。このあいだは SetPreference(false) しても有効のまま。
    [[nodiscard]] static bool IsForcedByLaunch();

    /// @brief 保存される設定値 (エディターの設定)。起動での強制は含まない。
    [[nodiscard]] static bool Preference();
    static void SetPreference(bool enabled);
};

} /// @note namespace fbzz::core
