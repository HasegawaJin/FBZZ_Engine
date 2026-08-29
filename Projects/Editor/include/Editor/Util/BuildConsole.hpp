/// @file    BuildConsole.hpp
/// @brief   スクリプト DLL / HLSL コンパイルの出力・診断・履歴を集約するハブ。
/// @author  Hasegawa Jin
/// @date    2026-07-19
///
/// WHY: これまでコンパイル結果は「StatusBar に一過性テキスト」＋「汎用 Console に生ログ」
/// という二重の弱点があり、失敗内容へ辿り着けなかった。BuildConsole を唯一の情報源とし、
/// Build Output パネル・StatusBar・ツールバー通知の 3 つの UI が同じデータを読む。
///
/// 使い方 (EditorApp 側):
/// 1. ビルド開始時          : BeginBuild(kind)
/// 2. Tick ごと (Building中) : IngestFullLog(compiler.GetLog())   // 差分だけ取り込む
/// 3. ビルド確定時          : EndBuild(success, exitCode) / EndBuildCancelled()
#pragma once
#include <deque>
#include <string>
#include <vector>

namespace fbzz::editor {

// コンパイラ出力 1 行から抽出した診断 (MSVC 形式のエラー / 警告)。
struct BuildDiagnostic {
    enum class Severity { Error, Warning };

    Severity    severity = Severity::Error;
    std::string file;             // 例: "C:/proj/Assets/Scripts/Foo.hpp" (空の場合はリンカ以外の全体エラー)
    int         line   = 0;       // 1 始まり。0 は行番号なし (リンカエラー等)
    int         column = 0;       // 0 は列番号なし
    std::string code;             // 例: "C2065" / "LNK2019" (空もあり)
    std::string message;          // 診断本文
    std::string raw;              // 元の 1 行 (クリップボードコピー用)
};

// ビルド 1 回分の記録。履歴 (deque) に積んで一覧・再表示する。
struct BuildRecord {
    enum class Kind   { Script, Hlsl };
    enum class Result { Building, Success, Failed, Cancelled };

    Kind        kind     = Kind::Script;
    Result      result   = Result::Building;
    int         exitCode = 0;
    std::string startClock;               // "14:32:05" (ローカル時刻)
    float       durationSec = 0.0f;       // 開始〜確定までの経過秒
    int         errorCount  = 0;
    int         warnCount   = 0;
    std::vector<BuildDiagnostic> diagnostics;
};

class BuildConsole {
public:
    // 履歴保持件数。ポートフォリオ用途では直近 20 件あれば十分。
    static constexpr size_t MAX_HISTORY   = 20;
    // ライブログのバイト上限。巨大な MSBuild 出力でメモリを圧迫しないよう先頭を切り捨てる。
    static constexpr size_t MAX_LOG_BYTES = 256 * 1024;

    // 新しいビルドを開始する。ライブ状態 (現在ファイル・ログ・診断) をリセットし、
    // Result::Building の新規レコードを履歴末尾へ積む。
    void BeginBuild(BuildRecord::Kind kind);

    // Compiler::GetLog() の「全文」を渡すと、前回消費位置からの差分だけをパースして取り込む。
    // WHY: Compiler は差分 API を持たず全文を返すため、消費済みバイト数を BuildConsole 側で管理する。
    void IngestFullLog(const std::string& fullLog);

    // ビルドを確定する。exitCode==0 かつ success==true で Success、それ以外は Failed。
    void EndBuild(bool success, int exitCode);
    // Cancel されたビルドを確定する (通知は出さない)。
    void EndBuildCancelled();

    // --- ライブ状態 (Building 中の UI 表示用) ---
    bool               IsBuilding()  const { return m_building; }
    const std::string& CurrentFile() const { return m_currentFile; }  // 現在コンパイル中の .cpp 名 (無ければ空)
    const std::string& LiveLog()     const { return m_liveLog; }

    // --- 履歴 ---
    const std::deque<BuildRecord>& History() const { return m_history; }
    const BuildRecord*             Latest()  const { return m_history.empty() ? nullptr : &m_history.back(); }
    void ClearHistory();

    // --- 失敗通知 (ツールバー下の通知バー) ---
    // 最新レコードが Failed で、かつユーザーが Dismiss していない場合に true。
    // 新しいビルド開始 or 成功で通知は自動的にリセットされる。
    bool HasActiveFailure() const;
    void DismissNotification() { m_notificationDismissed = true; }

    // 最新の失敗レコード (通知・ジャンプ用)。無ければ nullptr。
    const BuildRecord* LatestFailure() const;

private:
    // 完全な 1 行を解析し、診断抽出 or 「現在コンパイル中ファイル」更新を行う。
    void ConsumeLine(const std::string& line);
    // 現在ビルド中レコードへの参照 (末尾)。Building 中のみ有効。
    BuildRecord* CurrentRecord();

    std::deque<BuildRecord> m_history;
    bool        m_building     = false;
    size_t      m_consumedLen  = 0;    // IngestFullLog が消費済みの fullLog バイト数
    std::string m_lineBuffer;          // 改行未満の端数を次回まで保持
    std::string m_liveLog;             // 現在ビルドの全表示ログ (MAX_LOG_BYTES 上限)
    std::string m_currentFile;         // cl.exe がエコーした現在コンパイル中ファイル名
    bool        m_notificationDismissed = false;
    unsigned long long m_startTickMs = 0;  // duration 計測用 (GetTickCount64)
};

} // namespace fbzz::editor
