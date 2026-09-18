/// @file    SourceOpen.hpp
/// @brief   診断・ログの「ファイル:行」を外部エディターで開くための共通ユーティリティ。
/// @author  Hasegawa Jin
/// @date    2026-08-12
///
/// @note Build Output パネルはビルド診断から、Console パネルはランタイムログから、
///       それぞれ同じ「該当ソース行へ飛ぶ」操作を提供する。VSCode CLI の探索と
///       フォールバック手順を 2 箇所に書くと片方だけ壊れるため、ここへ集約する。
#pragma once
#include <cstddef>
#include <string>
#include <vector>

namespace fbzz::editor {

/// file を外部エディターで開く。line > 0 なら行ジャンプまで行う。
/// VSCode (`code -g file:line`) を優先し、無ければ OS 既定の関連付けで開く。
/// file が空の場合は何もしない。
void OpenSourceInExternalEditor(const std::string& file, int line);

/// ファイル名 (拡張子込みのベース名) から実ファイルの絶対パスを解決する。
/// @note Logger は FBZZ_FILENAME でパスを捨ててベース名だけを埋め込むため、ログ行から得られるのは
///       "Foo.cpp" のような名前でしかなく、ジャンプにはソースツリーを引き直す必要がある。
///
/// roots 配下を再帰探索し、最初に見つかった一致を返す (見つからなければ空文字列)。
/// 探索結果はプロセス内でキャッシュされるため、同じ名前の 2 回目以降は即座に返る。
/// fileName が既に実在するパスの場合はそのまま返す。
[[nodiscard]] std::string ResolveSourceFileByName(const std::string& fileName,
                                                  const std::vector<std::string>& roots);

/// "[Foo.cpp:123] メッセージ本文" 形式の先頭から file / line を取り出す。
/// @note LogEntry は level と message しか持たず、呼び出し位置は Logger::*At が本文先頭へ
///       埋め込んだ文字列としてしか残っていない。
/// 戻り値 true のとき outFile / outLine / outBodyOffset が有効。
/// outBodyOffset は "] " の直後 (本文の開始位置) を指す。
bool ParseLogLocationPrefix(const std::string& message,
                            std::string& outFile,
                            int& outLine,
                            std::size_t& outBodyOffset);

} // namespace fbzz::editor
