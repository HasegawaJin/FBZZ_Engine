/// @file    BuildConsole.cpp
/// @brief   コンパイラ出力の解析・診断抽出・履歴管理。
/// @author  Hasegawa Jin
/// @date    2026-07-19
#include <Editor/Util/BuildConsole.hpp>

#include <Windows.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <regex>

namespace fbzz::editor {

namespace {

/// MSVC 診断行のパターン。
///   file(line): error C2065: msg          … コンパイルエラー (列なし)
///   file(line,col): error C2065: msg      … コンパイルエラー (列あり)
///   file : error LNK2019: msg             … リンカエラー (行なし)
///   file(line): warning C4244: msg        … 警告
/// @note (line) と (line,col) と行なしを 1 本で吸収するため line/col グループを optional にする。code グループ (`[A-Za-z]+\d+`) を必須にし "0 Error(s)" 等のサマリ行を誤検出しない。
const std::regex kDiagRegex(
    R"(^\s*(.+?)(?:\((\d+)(?:,(\d+))?\))?\s*:\s*(fatal error|error|warning)\s+([A-Za-z]+\d+)\s*:\s*(.*)$)",
    std::regex::optimize);

/// cl.exe はコンパイル対象のソース名を単独行でエコーする (例: "Foo.cpp")。
/// これを検出して「現在コンパイル中ファイル」に反映し、進捗表示へ使う。
const std::regex kSourceEchoRegex(
    R"(^\s*([A-Za-z0-9_\-.]+\.(?:cpp|cxx|cc|c|hlsl))\s*$)",
    std::regex::optimize);

/// "CMake Error at CMakeLists.txt:12 (add_library):" / "CMake Warning (dev) at foo.cmake:3 (...)"
const std::regex kCMakeDiagRegex(
    R"(^\s*CMake (Error|Warning)(?: \(dev\))? at (.+):(\d+))",
    std::regex::optimize);

/// 現在時刻を "HH:MM:SS" で返す。
std::string LocalClockString()
{
    SYSTEMTIME st{};
    GetLocalTime(&st);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond);
    return buf;
}

/// MSBuild は /m 並列時、行頭に `<node>>` (例: "2>") を付ける。
/// これを除去しないと file グループに "2>C:\..." が入り、行ジャンプが壊れる。
std::string StripBuildNodePrefix(const std::string& rawLine)
{
    std::string line = rawLine;
    size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    size_t d = i;
    while (d < line.size() && line[d] >= '0' && line[d] <= '9') ++d;
    if (d > i && d < line.size() && line[d] == '>')
        line.erase(0, d + 1);
    return line;
}

} // namespace

LogListLine MakeBuildLogLine(const std::string& text, std::uint64_t id)
{
    LogListLine out;
    out.id   = id;
    out.text = text;

    /// @note 大半の行は診断ではない。正規表現は «それらしい語» を含む行にだけ当てる。
    const bool mayBeError   = text.find("rror")   != std::string::npos || text.find("FAILED") != std::string::npos;
    const bool mayBeWarning = text.find("arning") != std::string::npos;
    if (!mayBeError && !mayBeWarning) return out;

    const std::string line = StripBuildNodePrefix(text);
    std::smatch m;
    if (std::regex_match(line, m, kDiagRegex)) {
        out.severity = (m[4].str() == "warning") ? LogListSeverity::Warning : LogListSeverity::Error;
        out.file     = m[1].str();
        out.line     = m[2].matched ? std::atoi(m[2].str().c_str()) : 0;
        return out;
    }
    if (std::regex_search(line, m, kCMakeDiagRegex)) {
        out.severity = (m[1].str() == "Warning") ? LogListSeverity::Warning : LogListSeverity::Error;
        out.file     = m[2].str();
        out.line     = std::atoi(m[3].str().c_str());
        return out;
    }

    /// @note 形式に当てはまらなくても、ninja / clang / スクリプトの出力は «error:» の形で出る。素の "error" では拾わない: MSBuild のサマリ "0 Error(s)" や "-- Looking for error.h" まで赤くなり本物が埋もれる。
    if (line.find(": error") != std::string::npos || line.find("error:") != std::string::npos
        || line.find("CMake Error") != std::string::npos || line.find("Build FAILED") != std::string::npos
        || line.find("FAILED:") != std::string::npos) {
        out.severity = LogListSeverity::Error;
    } else if (line.find(": warning") != std::string::npos || line.find("warning:") != std::string::npos
               || line.find("CMake Warning") != std::string::npos) {
        out.severity = LogListSeverity::Warning;
    }
    return out;
}

void BuildLogFeed::Reset(LogListView& view, std::uint64_t generation, std::uint64_t firstLine)
{
    view.ClearLines();
    m_generation  = generation;
    m_firstLine   = firstLine;
    m_parsedBytes = 0;
    m_seenBytes   = 0;
    m_lineBytes.clear();
    m_hasPartial  = false;
}

void BuildLogFeed::Sync(const std::string& text, std::uint64_t generation, std::uint64_t firstLine,
                        LogListView& view)
{
    if (generation != m_generation || firstLine < m_firstLine) {
        Reset(view, generation, firstLine);
    } else if (firstLine != m_firstLine) {
        const std::uint64_t dropped = firstLine - m_firstLine;
        if (dropped > m_lineBytes.size()) {
            Reset(view, generation, firstLine);
        } else {
            for (std::uint64_t i = 0; i < dropped; ++i) {
                m_parsedBytes -= m_lineBytes.front();
                m_lineBytes.pop_front();
            }
            view.DropFrontLines(static_cast<std::size_t>(dropped));
            m_firstLine = firstLine;
            /// @note 下の «変化なし» 判定を通さない
            m_seenBytes = ~static_cast<std::size_t>(0);
        }
    }
    if (text.size() < m_parsedBytes) Reset(view, generation, firstLine);
    if (text.size() == m_seenBytes) return;
    m_seenBytes = text.size();

    /// @note 書きかけだった末尾行は、続きが来たかもしれないので作り直す。id は同じ値で積み直る。
    if (m_hasPartial) {
        view.PopBackLine();
        m_hasPartial = false;
    }

    std::size_t pos = m_parsedBytes;
    while (true) {
        const std::size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) break;
        std::string line = text.substr(pos, nl - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        view.AppendLine(MakeBuildLogLine(line, m_firstLine + m_lineBytes.size()));
        m_lineBytes.push_back(nl + 1 - pos);
        pos = nl + 1;
    }
    m_parsedBytes = pos;

    if (pos < text.size()) {
        std::string tail = text.substr(pos);
        if (!tail.empty() && tail.back() == '\r') tail.pop_back();
        view.AppendLine(MakeBuildLogLine(tail, m_firstLine + m_lineBytes.size()));
        m_hasPartial = true;
    }
}

void BuildConsole::BeginBuild(BuildRecord::Kind kind)
{
    BuildRecord rec;
    rec.kind       = kind;
    rec.result     = BuildRecord::Result::Building;
    rec.startClock = LocalClockString();

    m_history.push_back(std::move(rec));
    while (m_history.size() > MAX_HISTORY)
        m_history.pop_front();

    m_building     = true;
    m_buildingKind = kind;
    m_consumedLen  = 0;
    m_lineBuffer.clear();
    m_liveLog.clear();
    m_liveLogFirstLine = 0;
    ++m_liveLogGeneration;
    m_currentFile.clear();
    m_startTickMs  = GetTickCount64();
    /// @note ここで通知を消さない: ビルドを始めたことは失敗が直った証拠ではなく、直ったかは EndBuild で分かる。開始時に消すと走っている間だけバーが消えて見える。
}

BuildRecord* BuildConsole::CurrentRecord()
{
    if (!m_building || m_history.empty()) return nullptr;
    return &m_history.back();
}

void BuildConsole::IngestFullLog(const std::string& fullLog)
{
    if (!m_building) return;

    /// @note Compiler は Start() で m_log を clear するため 1 ビルド中は単調増加。稀に (Reset 直後の空文字など) 短くなった場合は消費位置を巻き戻して整合させる。
    if (fullLog.size() < m_consumedLen) {
        m_consumedLen = 0;
        m_lineBuffer.clear();
    }
    if (fullLog.size() == m_consumedLen) return;

    const std::string delta = fullLog.substr(m_consumedLen);
    m_consumedLen = fullLog.size();

    /// @note ライブ表示用ログへ追記し、上限を超えたら先頭を切り捨てる。
    m_liveLog += delta;
    if (m_liveLog.size() > MAX_LOG_BYTES) {
        /// @note 行の途中で切ると先頭の欠けた行が残り、表示側の行番号もずれる。次の改行まで捨てる。
        const size_t over = m_liveLog.size() - MAX_LOG_BYTES;
        const size_t nl   = m_liveLog.find('\n', over);
        const size_t cut  = (nl == std::string::npos) ? over : nl + 1;
        m_liveLogFirstLine += static_cast<uint64_t>(
            std::count(m_liveLog.begin(), m_liveLog.begin() + static_cast<std::ptrdiff_t>(cut), '\n'));
        m_liveLog.erase(0, cut);
    }

    /// @note 端数バッファに連結し、完全な行だけを取り出して解析する。
    m_lineBuffer += delta;
    size_t pos = 0;
    while (true) {
        const size_t nl = m_lineBuffer.find('\n', pos);
        if (nl == std::string::npos) break;
        std::string line = m_lineBuffer.substr(pos, nl - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        ConsumeLine(line);
        pos = nl + 1;
    }
    if (pos > 0) m_lineBuffer.erase(0, pos);
}

void BuildConsole::ConsumeLine(const std::string& rawLine)
{
    BuildRecord* rec = CurrentRecord();
    if (!rec) return;

    const std::string line = StripBuildNodePrefix(rawLine);

    std::smatch m;
    if (std::regex_match(line, m, kDiagRegex)) {
        BuildDiagnostic diag;
        diag.raw     = line;
        diag.file    = m[1].str();
        diag.line    = m[2].matched ? std::atoi(m[2].str().c_str()) : 0;
        diag.column  = m[3].matched ? std::atoi(m[3].str().c_str()) : 0;
        const std::string sev = m[4].str();
        diag.severity = (sev == "warning") ? BuildDiagnostic::Severity::Warning
                                           : BuildDiagnostic::Severity::Error;
        diag.code    = m[5].str();
        diag.message = m[6].str();

        /// @note MSBuild は同一エラーをプロジェクト単位で重複出力することがあるため、直前と完全一致する診断は畳んで一覧のノイズを減らす。
        if (rec->diagnostics.empty() || rec->diagnostics.back().raw != diag.raw) {
            if (diag.severity == BuildDiagnostic::Severity::Warning) rec->warnCount++;
            else                                                      rec->errorCount++;
            rec->diagnostics.push_back(std::move(diag));
        }
        return;
    }

    if (std::regex_match(line, m, kSourceEchoRegex))
        m_currentFile = m[1].str();
}

void BuildConsole::EndBuild(bool success, int exitCode)
{
    /// @note 端数バッファに残った最終行 (改行で終わらない出力) も解析する。
    if (!m_lineBuffer.empty()) {
        std::string tail = m_lineBuffer;
        if (!tail.empty() && tail.back() == '\r') tail.pop_back();
        ConsumeLine(tail);
        m_lineBuffer.clear();
    }

    if (BuildRecord* rec = CurrentRecord()) {
        rec->result      = success ? BuildRecord::Result::Success : BuildRecord::Result::Failed;
        rec->exitCode    = exitCode;
        rec->durationSec = static_cast<float>(GetTickCount64() - m_startTickMs) / 1000.0f;
    }

    m_building = false;
    m_currentFile.clear();

    /// @note 確定した結果を «その種類の» 通知状態へ書く: 1 つのフラグに畳むと HLSL の成功でスクリプトの失敗通知まで消えてしまい、直っていないものを黙らせることになる。
    FailureState& state = m_failures[static_cast<size_t>(m_buildingKind)];
    state.failed    = !success;
    state.dismissed = false;
    state.sequence  = ++m_failureSequence;
}

void BuildConsole::EndBuildCancelled()
{
    if (BuildRecord* rec = CurrentRecord()) {
        rec->result      = BuildRecord::Result::Cancelled;
        rec->durationSec = static_cast<float>(GetTickCount64() - m_startTickMs) / 1000.0f;
    }
    m_building = false;
    m_currentFile.clear();
    /// @note 中断は «結果» ではないので、その種類の通知状態は前のまま据え置く。
}

void BuildConsole::ClearHistory()
{
    /// @note Building 中のレコードは残し、確定済みだけを消す。
    if (m_building && !m_history.empty()) {
        BuildRecord current = std::move(m_history.back());
        m_history.clear();
        m_history.push_back(std::move(current));
    } else {
        m_history.clear();
    }
    /// @note 履歴を消せば通知の中身も辿れなくなるので、通知自体も畳む。
    for (FailureState& state : m_failures) state = {};
}

bool BuildConsole::HasActiveFailure() const
{
    return LatestFailure() != nullptr;
}

const BuildRecord* BuildConsole::LatestFailure() const
{
    /// @note 未 Dismiss の失敗を抱えている Kind のうち、いちばん新しいものを選ぶ。履歴には直った後の古い失敗も残るため、通知に出してよいのは «その種類の最後の結果が失敗» のものだけ。
    const BuildRecord* best     = nullptr;
    uint64_t           bestSeq  = 0;
    for (size_t kind = 0; kind < kKindCount; ++kind) {
        const FailureState& state = m_failures[kind];
        if (!state.failed || state.dismissed) continue;
        if (best != nullptr && state.sequence < bestSeq) continue;

        /// @note その種類の最新の Failed レコードを履歴から引く (診断とエラー件数の出所)。
        for (auto it = m_history.rbegin(); it != m_history.rend(); ++it) {
            if (static_cast<size_t>(it->kind) != kind) continue;
            if (it->result != BuildRecord::Result::Failed) continue;
            best    = &*it;
            bestSeq = state.sequence;
            break;
        }
    }
    return best;
}

void BuildConsole::DismissNotification()
{
    /// @note 今バーに出ているものだけを黙らせる。もう片方の失敗は残す。
    if (const BuildRecord* shown = LatestFailure())
        m_failures[static_cast<size_t>(shown->kind)].dismissed = true;
}

} // namespace fbzz::editor
