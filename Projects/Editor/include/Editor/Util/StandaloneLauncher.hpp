// FBZZ Engine
// StandaloneLauncher.hpp | fbzz::editor
// エディタから Standalone モードのプロセスを起動するユーティリティ。
//
// WHY: エディタの「▶ Standalone」ボタン、および「Build and Run」から
//      同一バイナリを --standalone フラグ付きで子プロセスとして起動する。
//      CreateProcess で分離した子プロセスとして起動するため、
//      エディタの終了や UI 操作が配布ゲームに影響しない。
#pragma once
#include <string>

namespace fbzz::editor {

class StandaloneLauncher {
public:
    // 現在開いているプロジェクトを Standalone モードで起動する。
    // @param exePath     自身 (FBZZEditor.exe) のフルパス。GetModuleFileNameW で取得。
    // @param projectPath プロジェクトルートディレクトリの絶対パス。
    // @return            CreateProcess に成功した場合 true。
    static bool Launch(const std::string& exePath, const std::string& projectPath);

    // Build and Run 用: ビルド済みの exe を引数なしで起動する (Standalone 自動検出モード)。
    // @param exePath 起動する exe の絶対パス。
    static bool LaunchExe(const std::string& exePath, const std::string& workingDir);
};

} // namespace fbzz::editor
