// FBZZ Engine
// StandaloneLauncher.hpp | fbzz::editor
// Build and Run 用の子プロセス起動ユーティリティ。
//
// WHY: Build Settings の「Build and Run」は、生成済み配布 exe をエディタとは別プロセスで起動する。
//      CreateProcess で分離した子プロセスとして起動するため、
//      エディタの終了や UI 操作が配布ゲームに影響しない。
#pragma once
#include <string>

namespace fbzz::editor {

class StandaloneLauncher {
public:
    // Build and Run 用: ビルド済みの exe を引数なしで起動する (Standalone 自動検出モード)。
    // @param exePath 起動する exe の絶対パス。
    static bool LaunchExe(const std::string& exePath, const std::string& workingDir);
};

} // namespace fbzz::editor
