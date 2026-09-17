/// @file    BuildConsole.hpp
/// @brief   スクリプト DLL / HLSL コンパイルの出力・診断・履歴を集約するハブ。
/// @author  Hasegawa Jin
/// @date    2026-07-19
///
/// @note BuildConsole を唯一の情報源とし、Build Output パネル・StatusBar・ツールバー通知が同じデータを読む。
///       使い方: BeginBuild(kind) → 毎 Tick IngestFullLog(差分取込) → EndBuild / EndBuildCancelled。
#pragma once
#include <Editor/Util/LogListView.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace fbzz::editor {

/// コンパイラ出力 1 行から抽出した診断 (MSVC 形式のエラー / 警告)。
struct BuildDiagnostic {
    enum class Severity { Error, Warning };

    Severity    severity = Severity::Error;
    std::string file;             ///< 例: "C:/proj/Assets/Scripts/Foo.hpp" (空の場合はリンカ以外の全体エラー)
    int         line   = 0;       ///< 1 始まり。0 は行番号なし (リンカエラー等)
    int         column = 0;       ///< 0 は列番号なし
    std::string code;             ///< 例: "C2065" / "LNK2019" (空もあり)
    std::string message;          ///< 診断本文
    std::string raw;              ///< 元の 1 行 (クリップボードコピー用)
};

/// ビルド 1 回分の記録。履歴 (deque) に積んで一覧・再表示する。
struct BuildRecord {
    enum class Kind   { Script, Hlsl };
    enum class Result { Building, Success, Failed, Cancelled };

    Kind        kind     = Kind::Script;
    Result      result   = Result::Building;
    int         exitCode = 0;
    std::string startClock;               ///< "14:32:05" (ローカル時刻)
    float       durationSec = 0.0f;       ///< 開始〜確定までの経過秒
    int         errorCount  = 0;
    int         warnCount   = 0;
    std::vector<BuildDiagnostic> diagnostics;
};

class BuildConsole {
public:
    /// 履歴保持件数。ポートフォリオ用途では直近 20 件あれば十分。
    static constexpr size_t MAX_HISTORY   = 20;
    /// ライブログのバイト上限。巨大な MSBuild 出力でメモリを圧迫しないよう先頭を切り捨てる。
    static constexpr size_t MAX_LOG_BYTES = 256 * 1024;

    /// 新しいビルドを開始する。ライブ状態 (現在ファイル・ログ・診断) をリセットし、
    /// Result::Building の新規レコードを履歴末尾へ積む。
    void BeginBuild(BuildRecord::Kind kind);

    /// Compiler::GetLog() の「全文」を渡すと、前回消費位置からの差分だけをパースして取り込む。
    /// @note Compiler は差分 API を持たず全文を返すため、消費済みバイト数を BuildConsole 側で管理する。
    void IngestFullLog(const std::string& fullLog);

    /// ビルドを確定する。exitCode==0 かつ success==true で Success、それ以外は Failed。
    void EndBuild(bool success, int exitCode);
    /// Cancel されたビルドを確定する (通知は出さない)。
    void EndBuildCancelled();

    /// @name ライブ状態 (Building 中の UI 表示用)
    /// @{
    bool               IsBuilding()  const { return m_building; }
    const std::string& CurrentFile() const { return m_currentFile; }  ///< 現在コンパイル中の .cpp 名 (無ければ空)
    const std::string& LiveLog()     const { return m_liveLog; }
    /// LiveLog 先頭行の通し番号。上限超過で前方を捨てたぶんだけ進む。
    uint64_t LiveLogFirstLine()  const { return m_liveLogFirstLine; }
    /// BeginBuild のたびに進む。LiveLog が別ビルドの中身へ入れ替わったことを表示側へ伝える。
    uint64_t LiveLogGeneration() const { return m_liveLogGeneration; }
    /// @}

    /// @name 履歴
    /// @{
    const std::deque<BuildRecord>& History() const { return m_history; }
    const BuildRecord*             Latest()  const { return m_history.empty() ? nullptr : &m_history.back(); }
    void ClearHistory();
    /// @}

    /// @name 失敗通知 (ツールバー下の通知バー)
    /// @{
    /// 状態は Kind ごとに持つ。
    /// @note 履歴は Script と HLSL が混在するため、出す条件は履歴末尾ではなく
    ///       種類ごとの «最後に確定した結果» で判定する (LatestFailure が種類ごとに返す)。
    ///
    /// いずれかの Kind に未 Dismiss の失敗があれば true。
    bool HasActiveFailure() const;
    /// 通知に出すべき失敗レコード。無ければ nullptr。
    /// 両方失敗しているときは «新しい方» を返す。
    const BuildRecord* LatestFailure() const;
    /// 現在通知に出ている失敗を黙らせる (その Kind だけ)。次の失敗でまた出る。
    void DismissNotification();
    /// @}

private:
    /// 完全な 1 行を解析し、診断抽出 or 「現在コンパイル中ファイル」更新を行う。
    void ConsumeLine(const std::string& line);
    /// 現在ビルド中レコードへの参照 (末尾)。Building 中のみ有効。
    BuildRecord* CurrentRecord();

    std::deque<BuildRecord> m_history;
    bool        m_building     = false;
    size_t      m_consumedLen  = 0;    ///< IngestFullLog が消費済みの fullLog バイト数
    std::string m_lineBuffer;          ///< 改行未満の端数を次回まで保持
    std::string m_liveLog;             ///< 現在ビルドの全表示ログ (MAX_LOG_BYTES 上限)
    uint64_t    m_liveLogFirstLine  = 0;
    uint64_t    m_liveLogGeneration = 0;
    std::string m_currentFile;         ///< cl.exe がエコーした現在コンパイル中ファイル名

    /// Kind ごとの通知状態。添字は BuildRecord::Kind の値 (Script=0 / Hlsl=1)。
    struct FailureState {
        bool     failed    = false;  ///< 最後に «確定した» ビルドが失敗だったか
        bool     dismissed = false;  ///< ユーザーが閉じたか
        uint64_t sequence  = 0;      ///< 新しさの比較用 (両方失敗しているときの優先順)
    };
    static constexpr size_t kKindCount = 2;
    std::array<FailureState, kKindCount> m_failures{};
    uint64_t m_failureSequence = 0;

    /// 現在ビルド中のレコードの種類 (EndBuild が状態を書く先を決める)。
    BuildRecord::Kind m_buildingKind = BuildRecord::Kind::Script;

    unsigned long long m_startTickMs = 0;  ///< duration 計測用 (GetTickCount64)
};

/// ビルド出力 1 行を LogListView の行へ変換する。MSVC / CMake の診断形式なら
/// 重大度と file:line を埋め、ダブルクリックで該当箇所を開けるようにする。
[[nodiscard]] LogListLine MakeBuildLogLine(const std::string& text, std::uint64_t id);

/// 伸びていくビルドログ全文を、LogListView へ差分で流し込む。
///
/// @note 1 行ごとに正規表現を当てるため、Tick ごとに全文を割り直すと数千行のビルドで重くなる。
class BuildLogFeed {
public:
    /// @param text       ログ全文
    /// @param generation ビルドごとに変わる値。変わったら一覧を作り直す
    /// @param firstLine  text 先頭行の通し番号 (前方を捨てたぶんだけ進む)
    void Sync(const std::string& text, std::uint64_t generation, std::uint64_t firstLine,
              LogListView& view);

private:
    void Reset(LogListView& view, std::uint64_t generation, std::uint64_t firstLine);

    std::uint64_t           m_generation  = ~0ull;
    std::uint64_t           m_firstLine   = 0;
    std::size_t             m_parsedBytes = 0;   ///< 改行まで確定して取り込んだバイト数
    std::size_t             m_seenBytes   = 0;   ///< 前回見た全文の長さ (変化の検出用)
    std::deque<std::size_t> m_lineBytes;         ///< 確定行ごとのバイト数 (前方切り捨ての追従用)
    bool                    m_hasPartial  = false; ///< 一覧末尾が改行待ちの書きかけ行か
};

} // namespace fbzz::editor
