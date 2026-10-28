/// @file    SourceOpen.hpp
/// @brief   診断・ログの「ファイル:行」を外部エディターで開くための共通ユーティリティ。
/// @author  Hasegawa Jin
/// @date    2026-08-12
/// @note Build Output と Console が同じ外部エディター探索・フォールバック手順を使う。
#pragma once
#include <cstddef>
#include <string>
#include <vector>

namespace fbzz::editor {

struct ManagedSourceLocation {
    std::string file;
    int line = 0;
};

/// @brief 外部エディターで開き、line > 0 のとき指定行へ移動する。空の file は無視する。
/// @note VSCode を優先し、無ければ OS 既定の関連付けで開く。
void OpenSourceInExternalEditor(const std::string& file, int line);

/// @brief roots 内で最初に一致した実ファイルのパスを返す。見つからなければ空文字列。
/// @note Logger がベース名だけを保存するため再帰探索し、結果をプロセス内でキャッシュする。
/// @note fileName が既に実在するパスの場合はそのまま返す。
[[nodiscard]] std::string ResolveSourceFileByName(const std::string& fileName,
                                                  const std::vector<std::string>& roots);

/// @brief 診断や stack trace のファイル表記を、指定 root を使って実ファイルへ解決する。
/// @return 見つからない場合は空文字列。
[[nodiscard]] std::string ResolveSourceFilePath(const std::string& filePath,
                                                const std::vector<std::string>& roots);

/// @brief `[Foo.cpp:123] 本文` の位置情報を読む。
/// @note LogEntry の呼び出し位置は Logger が本文先頭へ埋め込む文字列だけに保存される。
/// @return true のとき出力が有効。outBodyOffset は `] ` の直後を指す。
bool ParseLogLocationPrefix(const std::string& message,
                            std::string& outFile,
                            int& outLine,
                            std::size_t& outBodyOffset);

/// @brief .NET stack trace の `in <file>.cs:line <n>` から最初の C# source location を読む。
/// @return source frame が無い、または行番号が不正なら false。
[[nodiscard]] bool ParseManagedSourceLocation(const std::string& message,
                                              std::string& outFile,
                                              int& outLine);
/// @brief .NET stack trace の全 C# source location を frame 順で返す。
[[nodiscard]] std::vector<ManagedSourceLocation> ParseManagedSourceLocations(const std::string& message);

/// @brief 解決可能な C# stack frame を選び、project Assets 配下を優先する。
/// @return 解決できる frame が無い場合は空の file を返す。
[[nodiscard]] ManagedSourceLocation ResolveManagedSourceLocation(
    const std::string& message,
    const std::vector<std::string>& roots,
    const std::string& preferredRoot);

} /// @note namespace fbzz::editor
