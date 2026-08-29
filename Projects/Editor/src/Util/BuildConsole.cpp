/// @file    BuildConsole.cpp
/// @brief   コンパイラ出力の解析・診断抽出・履歴管理。
/// @author  Hasegawa Jin
/// @date    2026-07-19
#include <Editor/Util/BuildConsole.hpp>

#include <Windows.h>   // GetTickCount64 / GetLocalTime
#include <cstdio>
#include <cstdlib>     // atoi
#include <regex>

namespace fbzz::editor {

namespace {

// MSVC 診断行のパターン。
//   file(line): error C2065: msg          … コンパイルエラー (列なし)
//   file(line,col): error C2065: msg      … コンパイルエラー (列あり)
//   file : error LNK2019: msg             … リンカエラー (行なし)
//   file(line): warning C4244: msg        … 警告
// WHY: (line) と (line,col) と行なしを 1 本で吸収するため line/col グループを optional にする。
//      code グループ ([A-Za-z]+\d+) を必須にすることで "0 Error(s)" 等のサマリ行を誤検出しない。
const std::regex kDiagRegex(
    R"(^\s*(.+?)(?:\((\d+)(?:,(\d+))?\))?\s*:\s*(fatal error|error|warning)\s+([A-Za-z]+\d+)\s*:\s*(.*)$)",
    std::regex::optimize);

// cl.exe はコンパイル対象のソース名を単独行でエコーする (例: "Foo.cpp")。
// これを検出して「現在コンパイル中ファイル」に反映し、進捗表示へ使う。
const std::regex kSourceEchoRegex(
    R"(^\s*([A-Za-z0-9_\-.]+\.(?:cpp|cxx|cc|c|hlsl))\s*$)",
    std::regex::optimize);

// 現在時刻を "HH:MM:SS" で返す。
std::string LocalClockString()
{
    SYSTEMTIME st{};
    GetLocalTime(&st);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond);
    return buf;
}

} // namespace

void BuildConsole::BeginBuild(BuildRecord::Kind kind)
{
    BuildRecord rec;
    rec.kind       = kind;
    rec.result     = BuildRecord::Result::Building;
    rec.startClock = LocalClockString();

    m_history.push_back(std::move(rec));
    while (m_history.size() > MAX_HISTORY)
        m_history.pop_front();

    m_building              = true;
    m_consumedLen           = 0;
    m_lineBuffer.clear();
    m_liveLog.clear();
    m_currentFile.clear();
    m_notificationDismissed = false;
    m_startTickMs           = GetTickCount64();
}

BuildRecord* BuildConsole::CurrentRecord()
{
    if (!m_building || m_history.empty()) return nullptr;
    return &m_history.back();
}

void BuildConsole::IngestFullLog(const std::string& fullLog)
{
    if (!m_building) return;

    // WHY: Compiler は Start() で m_log を clear するため、1 ビルド中は単調増加。
    //      稀に (Reset 直後の空文字など) 短くなった場合は消費位置を巻き戻して整合させる。
    if (fullLog.size() < m_consumedLen) {
        m_consumedLen = 0;
        m_lineBuffer.clear();
    }
    if (fullLog.size() == m_consumedLen) return;

    const std::string delta = fullLog.substr(m_consumedLen);
    m_consumedLen = fullLog.size();

    // ライブ表示用ログへ追記し、上限を超えたら先頭を切り捨てる。
    m_liveLog += delta;
    if (m_liveLog.size() > MAX_LOG_BYTES)
        m_liveLog.erase(0, m_liveLog.size() - MAX_LOG_BYTES);

    // 端数バッファに連結し、完全な行だけを取り出して解析する。
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

    // MSBuild は /m 並列時、行頭に "<node>>" (例: "2>") を付ける。
    // これを除去しないと file グループに "2>C:\..." が入り、行ジャンプが壊れる。
    std::string line = rawLine;
    {
        size_t i = 0;
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
        size_t d = i;
        while (d < line.size() && line[d] >= '0' && line[d] <= '9') ++d;
        if (d > i && d < line.size() && line[d] == '>')
            line.erase(0, d + 1);
    }

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

        // WHY: MSBuild は同一エラーをプロジェクト単位で重複出力することがある。
        //      直前と完全一致する診断は畳んで一覧のノイズを減らす。
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
    // 端数バッファに残った最終行 (改行で終わらない出力) も解析する。
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
    // 成功時は前回失敗の通知フラグをリセット (次の失敗で再表示できるようにする)。
    if (success) m_notificationDismissed = true;
}

void BuildConsole::EndBuildCancelled()
{
    if (BuildRecord* rec = CurrentRecord()) {
        rec->result      = BuildRecord::Result::Cancelled;
        rec->durationSec = static_cast<float>(GetTickCount64() - m_startTickMs) / 1000.0f;
    }
    m_building = false;
    m_currentFile.clear();
    m_notificationDismissed = true;
}

void BuildConsole::ClearHistory()
{
    // Building 中のレコードは残し、確定済みだけを消す。
    if (m_building && !m_history.empty()) {
        BuildRecord current = std::move(m_history.back());
        m_history.clear();
        m_history.push_back(std::move(current));
    } else {
        m_history.clear();
    }
    m_notificationDismissed = true;
}

bool BuildConsole::HasActiveFailure() const
{
    if (m_notificationDismissed) return false;
    const BuildRecord* latest = Latest();
    return latest && latest->result == BuildRecord::Result::Failed;
}

const BuildRecord* BuildConsole::LatestFailure() const
{
    // 履歴を末尾から遡り、最初に見つかった Failed を返す。
    for (auto it = m_history.rbegin(); it != m_history.rend(); ++it) {
        if (it->result == BuildRecord::Result::Failed) return &*it;
    }
    return nullptr;
}

} // namespace fbzz::editor
