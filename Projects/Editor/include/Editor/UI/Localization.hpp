/// @file    Localization.hpp
/// @brief   エディター UI の表示言語 (English / 日本語) を切り替える。
/// @author  Hasegawa Jin
/// @date    2026-09-08
///
/// @note 辞書の鍵は «英語の原文» であって呼び名キーではない。訳が無い文字列は英語のまま出るので
///       (キー方式のような «menu.file.save» の露出が無い)、1 パネルずつ訳を足せる。抜けは MissingKeys() で拾う。
///
/// @note ImGui はラベル文字列から ID を作るため、訳した文字列をそのまま渡すと保存済みのドッキング配置・
///       開いた見出し・列幅が言語切替のたびに失われる。ImGui 1.92.6 以降は `"訳###原文"` の ID が
///       `"原文"` と一致するため、ラベルには必ず原文の `###` を付ける。ただし `###原文` は
///       Text / TextUnformatted では `##` 以降が削られずそのまま画面に出るため用途で使い分ける。
///
///     LOC("Save")   → "保存###Save"  … Button / MenuItem / Selectable / Checkbox /
///                                       Begin / Combo / SeparatorText / CollapsingHeader
///     LOCT("Save")  → "保存"          … Text / TextUnformatted / SetTooltip / 文字列連結
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace fbzz::editor::loc {

enum class Language {
    English = 0,
    Japanese,
};

/// 選べる言語の一覧。UI のコンボはこれを回して作る。
inline constexpr Language kLanguages[] = { Language::English, Language::Japanese };

/// 設定ファイルに書く安定識別子 ("en" / "ja")。表示名を変えても変わらない。
[[nodiscard]] const char* Id(Language language);
/// UI に出す名前。**常にその言語自身の表記**で返す ── 日本語表示のまま
/// English を探すときに «英語» としか書かれていないと選べない。
[[nodiscard]] const char* DisplayName(Language language);
/// 未知の識別子は English を返す。
[[nodiscard]] Language FromId(std::string_view id);

void                     SetLanguage(Language language);
[[nodiscard]] Language   GetLanguage();

/// 表示専用の訳。訳が無ければ英語の原文をそのまま返す。
[[nodiscard]] const char* Text(const char* english);
/// ウィジェットのラベル。"訳###原文" を返すので ImGui の ID は原文のまま変わらない。
[[nodiscard]] const char* Label(const char* english);

/// 現在の言語が持つ訳の数。
[[nodiscard]] int TranslationCount();
/// この起動中に引かれたが訳が無かった原文。1 パネルずつ訳を足すときの作業リスト。
/// English 表示中は集めない (全部が «未訳» になり意味を持たないため)。
[[nodiscard]] const std::vector<std::string>& MissingKeys();

} // namespace fbzz::editor::loc

/// ウィジェットのラベル用。ImGui の ID は原文のまま保たれる。
#define LOC(text)  ::fbzz::editor::loc::Label(text)
/// 表示テキスト用 (Text / TextUnformatted / ツールチップ)。
#define LOCT(text) ::fbzz::editor::loc::Text(text)
