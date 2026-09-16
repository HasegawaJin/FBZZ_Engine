/// @file    Localization.hpp
/// @brief   エディター UI の表示言語 (English / 日本語) を切り替える。
/// @author  Hasegawa Jin
/// @date    2026-09-08
///
/// WHY キーではなく «英語の原文» を辞書の鍵にするか:
///   キー方式 (`LOC("menu.file.save")`) は既存の 9,000 個以上ある文字列を全部
///   置き換え終わるまで画面が «menu.file.save» だらけになる。原文を鍵にすれば、
///   訳が無い文字列は英語のまま出る ── 未対応の箇所が «壊れて» ではなく
///   «英語のまま» 残るので、1 パネルずつ訳を足していける。訳の抜けを探すのも
///   MissingKeys() で機械的にできる。
///
/// WHY 表示用と «ウィジェットのラベル用» を分けるか:
///   ImGui はラベル文字列から ID を作る。訳した文字列をそのまま渡すと ID が変わり、
///   保存済みのドッキング配置 (imgui_layout.ini)・開いた見出し・テーブルの列幅が
///   言語を切り替えるたびに失われる。ImGui 1.92.6 以降は "訳###原文" の ID が
///   "原文" と一致するので、ラベルには必ず原文の ### を付ける ── これで
///   英語のままのビルドで保存した配置も、日本語へ切り替えた後もそのまま効く。
///
///   ただし ###原文 は Text / TextUnformatted では**そのまま画面に出てしまう**
///   (これらは ## 以降を削らない)。用途で使い分けること。
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
