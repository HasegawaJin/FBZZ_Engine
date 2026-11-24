/// @file    CrashHandler.hpp
/// @brief   落ちたときのミニダンプとレポートの書き出し、次回起動時の通知。
/// @author  Hasegawa Jin
/// @date    2026-09-19
/// @see Docs/design/crash-report.md
#pragma once
#include <Engine/Api.hpp>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace fbzz::core {

/// @brief ディスクに残った 1 件のクラッシュレポート。
struct CrashRecord {
    std::filesystem::path directory;  ///< @note `<根>/Saved/Crashes/<時刻>_<pid>/`
    std::string           summary;    ///< @note report.txt の «何が起きたか» の 1 行 (UTF-8)
};

/// @brief わざと起こす落ち方。どれも別々の受け口を通る。
/// @see Docs/design/developer-mode.md §3
enum class CrashTrigger {
    AccessViolation,
    StackOverflow,
    Abort,
    PureCall,
    InvalidParameter,
    Report,  ///< @note 落とさずに WriteReport だけを呼ぶ
};

/// @brief プロセスに 1 つの例外フィルターと、ログの末尾を持つ。
/// @note すべて主スレッドから呼ぶ。書き出しだけは任意のスレッドの落ちた瞬間に走る。
class CrashHandler {
public:
    /// @brief 例外フィルター・abort・purecall・不正引数の受け口を差し替え、ログの末尾を取り始める。
    /// @param root `Saved/Crashes` を作る親。入口の作業ディレクトリを渡す。
    /// @param appName report.txt と通知に出す名前 (UTF-8)。
    /// @return dbghelp.dll を読めなければ false (受け口は差し替えない)。2 回目は根と名前だけ差し替える。
    FBZZ_ENGINE_API static bool Install(const std::filesystem::path& root, std::string_view appName);

    /// @brief 受け口を Install 前へ戻し、ログの取得を止める。
    FBZZ_ENGINE_API static void Uninstall();

    [[nodiscard]] FBZZ_ENGINE_API static bool IsInstalled();

    /// @brief 落ちていないが続けられないときに、今の状態をレポートとして残す。
    /// @param reason report.txt の «何が起きたか» に書く (UTF-8)。
    /// @return 書いたディレクトリ。Install 前か書けなければ空。
    /// @pre Install 済み。
    FBZZ_ENGINE_API static std::filesystem::path WriteReport(std::string_view reason);

    /// @brief `reported` の印が無いレポートを新しい順に返す。
    [[nodiscard]] FBZZ_ENGINE_API static std::vector<CrashRecord> FindUnreported(const std::filesystem::path& root);

    /// @brief 次から FindUnreported に出ないよう印を付ける。
    FBZZ_ENGINE_API static void MarkReported(const CrashRecord& record);

    /// @brief レポートの経路を確かめるためにわざと落とす。Report 以外は戻らない。
    /// @return 開発者モードでない、または Install 前なら何もせず false。Report は書けたら true。
    FBZZ_ENGINE_API static bool Trigger(CrashTrigger kind);

    /// @brief Trigger の名前 ("access_violation" …) を列の順で返す。Op の引数の候補に使う。
    [[nodiscard]] FBZZ_ENGINE_API static const std::vector<std::string>& TriggerNames();

    /// @return 知らない名前なら false (out は未変更)。
    [[nodiscard]] FBZZ_ENGINE_API static bool ParseTrigger(std::string_view name, CrashTrigger& out);

    /// @brief 未報告のレポートを知らせる。
    /// @param interactive true ならダイアログで知らせて印を付ける。false なら Logger に WARN を出すだけで印は付けない。
    /// @return 未報告の件数。
    FBZZ_ENGINE_API static size_t NotifyUnreported(const std::filesystem::path& root, std::string_view appName, bool interactive);
};

} /// @note namespace fbzz::core
