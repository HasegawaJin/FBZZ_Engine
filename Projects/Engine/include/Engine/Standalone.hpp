/// @file    Standalone.hpp
/// @brief   C# ゲームを実行する native host の起動入口。
/// @author  Hasegawa Jin
/// @date    2026-10-30
#pragma once
#include <Engine/Api.hpp>

namespace fbzz {
/// @param applicationName UTF-8 の表示名。呼出し中だけ参照する。
/// @return 起動・設定の失敗は 1。通常終了は 0。
/// @note プロセスのコマンドライン、作業ディレクトリと Application の寿命を所有する。メインスレッドで一度だけ呼ぶ。
[[nodiscard]] FBZZ_ENGINE_API int RunStandaloneApplication(const char* applicationName);
} /// @note namespace fbzz
