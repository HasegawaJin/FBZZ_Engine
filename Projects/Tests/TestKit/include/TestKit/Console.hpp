/// @file    Console.hpp
/// @brief   テスト結果を «読める状態で» 画面に残すためのコンソール制御。
/// @author  Hasegawa Jin
/// @date    2026-09-02
///
/// Visual Studio から exe を起動すると、コンソールは終了と同時に消える。
/// 標準出力へ出しているだけでは «結果がどこにも出ていない» ように見えるため、
/// コンソールを確保し、DPI に合わせた読める字にして、最後に入力待ちで止める。
///
/// ただし CTest / テスト列挙 / CI から呼ばれたときは何もしない。
/// そこで止まるとビルドやパイプラインがハングする。
#pragma once

namespace fbzz::testkit {

struct ConsoleOptions {
    /// 96dpi 基準の文字の高さ [px]。実際の高さは画面の DPI で拡大する。
    int fontHeight = 17;
    /// 等幅であること。字形が崩れる環境向けに差し替えられるようにしてある。
    const wchar_t* faceName = L"Consolas";
    /// スクロールバックの行数。テストが数百件になっても先頭まで遡れる量。
    int bufferLines = 20000;
};

/// 自動実行 (CTest / gtest のテスト列挙 / CI) から呼ばれているか。
/// InitGoogleMock は argv からフラグを取り除くので、**呼ぶ前に**判定すること。
[[nodiscard]] bool IsAutomatedRun(int argc, char** argv);

/// コンソールが無ければ作り、標準入出力を繋ぎ、UTF-8 と読める文字サイズに整える。
///
/// @return 終了時に窓ごと消えるか。true なら結果を読む前に閉じてしまうので、
///         呼び出し側は WaitForKey で止めること。
///         判定は «このコンソールに繋がっているプロセスが自分だけか»。
///         Visual Studio の F5 や exe のダブルクリックは自分だけ = true、
///         ターミナルや VS Code のタスクから起動した場合はシェルも居るので false。
bool EnsureConsole(const ConsoleOptions& options = {});

/// キー入力を待つ。EnsureConsole が true を返したときだけ呼ぶこと。
void WaitForKey(const char* message = "何かキーを押すと閉じます...");

} // namespace fbzz::testkit
