/// @file    Localization.cpp
/// @brief   エディター UI の表示言語の実体。
/// @author  Hasegawa Jin
/// @date    2026-09-08
#include <Editor/Util/Localization.hpp>

#include <algorithm>
#include <iterator>
#include <unordered_map>

namespace fbzz::editor::loc {

namespace {

/// 訳 1 つ。text は表示用、label は "訳###原文"。
///
/// WHY 2 本持つか: ラベルは毎フレーム何百回も引かれる。呼ばれるたびに
///     "訳" + "###" + "原文" を組み立てると、切り替えただけでエディターが重くなる。
///     訳の数だけ一度作って持てば済む。
struct Entry {
    std::string text;
    std::string label;
};

/// 鍵は原文へのポインタではなく中身。呼び出し側の "Save" と辞書の "Save" は
/// 別のリテラルでありうる (翻訳単位が違えば別アドレス)。
using Table = std::unordered_map<std::string_view, Entry>;

// 既定は日本語。設定ファイルが無い初回起動でも日本語で開く。
Language                 s_language = Language::Japanese;
Table                    s_table;
std::vector<std::string> s_missing;
/// 辞書を組んだか。SetLanguage を一度も呼ばずに描き始める経路
/// (プロジェクトを開く前のメニュー) でも訳が出るようにする。
bool                     s_built = false;

/// 原文と訳の対。辞書はここ 1 箇所で、原文がそのまま鍵になる。
struct Pair {
    const char* english;
    const char* translated;
};

constexpr Pair kJapanese[] = {
#include "Localization_ja.inl"
};

void BuildTable(Language language)
{
    s_table.clear();
    s_missing.clear();
    s_built = true;
    if (language == Language::English) return;

    s_table.reserve(std::size(kJapanese));
    for (const Pair& pair : kJapanese) {
        Entry entry;

        // 訳が空文字は «英語のままにすると決めた» 印。原文をそのまま返し、
        // 未訳リストにも出さない。
        //
        // WHY 辞書に載せるか: 載せないと «まだ訳していない» と区別が付かず、
        //     未訳リストがコンポーネント名で埋まって作業リストとして使えなくなる。
        //     決めたことは書いておく方が、次に見た人が同じ判断をやり直さずに済む。
        if (pair.translated == nullptr || pair.translated[0] == '\0') {
            entry.text  = pair.english;
            entry.label = pair.english;
            s_table.emplace(std::string_view(pair.english), std::move(entry));
            continue;
        }

        entry.text  = pair.translated;
        // ###原文 を足すと ImGui の ID は原文のときと同じになる (1.92.6 以降)。
        // 訳を当てても保存済みのドッキング配置や開閉状態が失われない。
        entry.label = entry.text + "###" + pair.english;
        s_table.emplace(std::string_view(pair.english), std::move(entry));
    }
}

/// 既に日本語 (非 ASCII) を含む文字列か。
///
/// WHY 要るか: GreenWare の FBZZ_GROUP / 表示名には最初から日本語で書かれたものがある。
///     それを «未訳» として記録すると、作業リストに «訳し終わっているもの» が並ぶ。
bool AlreadyLocalized(const char* text)
{
    for (const char* p = text; *p != '\0'; ++p)
        if (static_cast<unsigned char>(*p) >= 0x80) return true;
    return false;
}

/// 訳が無かった原文を控える。1 パネルずつ訳を足していくための作業リスト。
void RecordMissing(const char* english)
{
    if (AlreadyLocalized(english)) return;
    // 毎フレーム引かれるので、既に控えたものは足さない。件数は «残り作業» の
    // 目安であって網羅リストではないので、線形探索で足りる。
    if (std::find(s_missing.begin(), s_missing.end(), english) != s_missing.end()) return;
    s_missing.emplace_back(english);
}

} // namespace

const char* Id(Language language)
{
    return language == Language::Japanese ? "ja" : "en";
}

const char* DisplayName(Language language)
{
    return language == Language::Japanese ? "日本語" : "English";
}

Language FromId(std::string_view id)
{
    return id == "ja" ? Language::Japanese : Language::English;
}

void SetLanguage(Language language)
{
    s_language = language;
    BuildTable(language);
}

Language GetLanguage() { return s_language; }

const char* Text(const char* english)
{
    if (english == nullptr) return "";
    if (!s_built) BuildTable(s_language);
    if (s_language == Language::English) return english;

    const auto it = s_table.find(std::string_view(english));
    if (it == s_table.end()) {
        RecordMissing(english);
        return english;
    }
    return it->second.text.c_str();
}

const char* Label(const char* english)
{
    if (english == nullptr) return "";
    if (!s_built) BuildTable(s_language);
    if (s_language == Language::English) return english;

    const auto it = s_table.find(std::string_view(english));
    if (it == s_table.end()) {
        RecordMissing(english);
        // 訳が無いときは原文をそのまま返す。ID は原文から作られるので、
        // ここで ### を足しても足さなくても同じ ID になる。
        return english;
    }
    return it->second.label.c_str();
}

int TranslationCount() { return static_cast<int>(s_table.size()); }

const std::vector<std::string>& MissingKeys() { return s_missing; }

} // namespace fbzz::editor::loc
