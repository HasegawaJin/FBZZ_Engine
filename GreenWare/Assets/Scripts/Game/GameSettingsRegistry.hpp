/// @file    GameSettingsRegistry.hpp
/// @brief   設定項目の宣言簿。スクリプトが 1 行宣言すれば、保存・Option の行・読み口が揃う
/// @author  Hasegawa Jin
/// @date    2026-09-07
///
/// WHY 宣言簿を挟むか:
///   設定を 1 つ増やすのに、これまでは 5 か所を触る必要があった ─
///   `GameConfig` の field と Reflect、`OptionsScreenComponent` の行の表、
///   `GetValue` と `SetValue` の if 連鎖、選択肢の表。しかも 5 か所のうち
///   1 つ書き忘れると「Option には出るが保存されない」「保存はされるが誰も読まない」
///   という、画面からは区別の付かない壊れ方をする (2026-08-24 に実際に踏んだ)。
///   宣言を 1 つの構造体へ畳めば、書き忘れの余地そのものが消える。
///
/// WHY 値をここが持つか (GameConfig のような構造体を増やさないか):
///   スクリプトが増やす設定は、その数も型も事前に判らない。構造体で持つには
///   フィールドを書き足すことになり、結局「触る場所が増える」問題へ戻る。
///   id をキーにした 1 本の表なら、宣言が増えても表の行が増えるだけで済む。
///
/// WHY float 1 本に畳むか:
///   設定の値は「つまみの位置」か「選択肢の番号」か「入 / 切」しかない。
///   3 つとも float 1 本で表せて、見せ方 (Kind) だけが違う。型を分けると
///   Option 画面が型ごとの分岐を持つことになり、行を足すたびにそこを触る。
///
/// WHY シーンをまたいで残るか:
///   宣言簿は Scripts.dll の static なので、シーン遷移では消えない。
///   `GameSettingsComponent` が居ないシーン (StageSelect / Result) でも
///   `settings::Get()` は最後に効いていた値を返す。設定は「そのシーンの持ち物」
///   ではないので、シーンの構成に答えが左右されてはいけない。
///   DLL をリロードすると空へ戻るが、宣言する側は OnStart で毎回宣言するので
///   次のフレームには揃い直す。
#pragma once

#include <Engine/Scene/Script.hpp>
#include <algorithm>
#include <cmath>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace sandbox::settings {

/// 値の見せ方。入力の受け方 (スライダーか押すだけか) は Option 画面が決める。
enum class Kind : int {
    Percent,   ///< 0-1 を百分率で見せる。min/max は使わない
    Number,    ///< 実数。min/max がつまみの両端、decimals と suffix が表示を整える
    Toggle,    ///< ON / OFF
    Choice,    ///< choices の添字
};

/// 設定 1 項目の宣言。
///
/// WHY get / set を差せるようにするか:
///   表示・音量・入力の 3 つは値の置き場がエンジン側 (display / audio / input プロキシ)
///   にあり、宣言簿が値を持っても意味が無い。あちらを正本にしたまま Option の行と
///   スクリプトの読み口だけを共通化したいので、「値の出し入れだけ外から差す」形にする。
///   差さなければ宣言簿が値を持ち、config へ自動で往復する。
struct Setting {
    std::string id;        ///< config のキーであり、Option の行 ID (`<TAB>_Row_<id>`)
    std::string page;      ///< 出すページ。Option のタブ名 ("Tab_GAME" 等)
    Kind        kind  = Kind::Percent;
    float       min   = 0.0f;
    float       max   = 1.0f;
    float       defaultValue = 0.0f;
    int         decimals = 0;
    std::string suffix;
    std::vector<std::string> choices;   ///< Kind::Choice のときの選択肢

    /// 値の実体を外へ預けるときの出し入れ。両方空なら下の value が正本。
    ///
    /// WHY 両方を対で持つか: 片方だけ差せると「読むのは宣言簿 / 書くのは外」という
    ///     ねじれた状態が作れてしまい、Option で動かした値が次のフレームに
    ///     元へ戻る (外の値で上書きされる) という形で壊れる。
    std::function<float()>     get;
    std::function<void(float)> set;

    /// 値が変わった直後に呼ぶ。効かせる先が値の置き場と別のときに使う。
    std::function<void(float)> onApply;

    /// config へ往復させるか。get/set を差した項目は外が保存するので既定で false になる。
    bool persist = true;

    /// get/set を差していないときの値の実体。
    float value = 0.0f;

    /// 選択肢の数。Choice 以外は 0。
    [[nodiscard]] int ChoiceCount() const
    { return kind == Kind::Choice ? static_cast<int>(choices.size()) : 0; }

    /// 許される範囲へ丸める。Toggle と Choice は整数へ落とす。
    [[nodiscard]] float Sanitize(float raw) const
    {
        switch (kind) {
        case Kind::Percent: return std::clamp(raw, 0.0f, 1.0f);
        case Kind::Toggle:  return raw >= 0.5f ? 1.0f : 0.0f;
        case Kind::Choice: {
            const int count = ChoiceCount();
            if (count <= 0) return 0.0f;
            return static_cast<float>(
                std::clamp(static_cast<int>(std::lround(raw)), 0, count - 1));
        }
        case Kind::Number: break;
        }
        return std::clamp(raw, std::min(min, max), std::max(min, max));
    }
};

namespace detail {

/// 宣言簿の実体。
///
/// WHY 関数の中の static か: ヘッダーオンリーで 1 実体に保つための定石。
///     inline 変数でも同じだが、初期化順が絡む静的アクセサから触るので
///     「最初に呼ばれたときに作られる」ことが保証される形にしておく。
inline std::vector<Setting>& Entries()
{
    static std::vector<Setting> entries;
    return entries;
}

/// 宣言が増減した回数。読み込みし直す側 (GameSettingsComponent) が変化を見る札。
inline int& Revision()
{
    static int revision = 0;
    return revision;
}

/// 値が書き換わった回数。
///
/// WHY 宣言の版と分けるか: «読み込み直す» 理由 (項目が増えた) と «保存する»
///     理由 (値が変わった) は別の出来事で、同じ札で数えるとどちらか一方が
///     もう一方を空振りさせる。組み込み項目は MutableXxx() が編集を申告するが、
///     宣言簿が値を持つ項目にはその口が無い ─ ここで数えないと、
///     スクリプトが足した設定だけが «保存されない» という形で落ちる。
inline int& ValueRevision()
{
    static int revision = 0;
    return revision;
}

} // namespace detail

/// 宣言されている全項目。
///
/// WHY 参照を返すのに Setting* を持ち回らせないか: 宣言が増えると vector が
///     再確保され、控えていたポインタは無効になる。走査は「その場で回す」こと。
[[nodiscard]] inline const std::vector<Setting>& All() { return detail::Entries(); }

/// 宣言の版。増えるたびに 1 進む。
[[nodiscard]] inline int Revision() { return detail::Revision(); }

/// 値の版。宣言簿が値を持つ項目が書き換わるたびに 1 進む。
/// 保存する側 (GameSettingsComponent) が «書き戻す必要があるか» を見る。
[[nodiscard]] inline int ValueRevision() { return detail::ValueRevision(); }

/// id で引く。無ければ nullptr。返ったポインタは次の Declare まで有効。
[[nodiscard]] inline Setting* Find(std::string_view id)
{
    for (Setting& entry : detail::Entries())
        if (entry.id == id) return &entry;
    return nullptr;
}

[[nodiscard]] inline const Setting* FindConst(std::string_view id) { return Find(id); }

/// 宣言する。同じ id を再宣言したときは「宣言の内容だけ」差し替え、値は保つ。
///
/// WHY 値を保つか: 宣言する側は OnStart で毎回宣言する。差し替えのたびに既定へ
///     戻すと、シーンを移るたびにプレイヤーの設定が消える。宣言は「どんな項目か」
///     であって「今いくつか」ではない。
///
/// WHY 版を進めるのが新規のときだけか: 版は「読み込み直す必要があるか」を表す。
///     既にある項目の宣言を直しても、その値は既に config から読めている。
inline void Declare(Setting setting)
{
    if (setting.id.empty()) return;

    // 値の置き場を外へ預けた項目は、保存も外の担当。二重に持つと、
    // どちらが正本なのか宣言からは読めなくなる。
    if (setting.get || setting.set) setting.persist = false;

    if (Setting* existing = Find(setting.id)) {
        const float kept = existing->value;
        *existing = std::move(setting);
        existing->value = kept;
        return;
    }

    setting.value = setting.Sanitize(setting.defaultValue);
    detail::Entries().push_back(std::move(setting));
    ++detail::Revision();
}

/// 宣言を消す。DLL リロードを跨がないので普段は要らない。
inline void Clear()
{
    detail::Entries().clear();
    ++detail::Revision();
}

/// 現在値。未宣言なら fallback。
///
/// WHY get() の結果を value へ写すか: 値の置き場が外にある項目でも、
///     その置き場が消えた後 (GameSettingsComponent の居ないシーン) に
///     「最後に効いていた値」を返せるようにするため。
[[nodiscard]] inline float Get(std::string_view id, float fallback = 0.0f)
{
    Setting* entry = Find(id);
    if (!entry) return fallback;
    if (entry->get) entry->value = entry->Sanitize(entry->get());
    return entry->value;
}

[[nodiscard]] inline bool GetBool(std::string_view id, bool fallback = false)
{
    return Get(id, fallback ? 1.0f : 0.0f) >= 0.5f;
}

[[nodiscard]] inline int GetInt(std::string_view id, int fallback = 0)
{
    return static_cast<int>(std::lround(Get(id, static_cast<float>(fallback))));
}

/// Kind::Choice の現在の選択肢名。範囲外・未宣言は空。
[[nodiscard]] inline std::string GetChoice(std::string_view id)
{
    const Setting* entry = Find(id);
    if (!entry || entry->choices.empty()) return {};
    const int index = std::clamp(static_cast<int>(std::lround(Get(id))), 0,
                                 static_cast<int>(entry->choices.size()) - 1);
    return entry->choices[static_cast<std::size_t>(index)];
}

/// 値を変える。丸め → 置き場へ書く → onApply、の順。未宣言なら何もしない。
inline void Set(std::string_view id, float value)
{
    Setting* entry = Find(id);
    if (!entry) return;

    const float sanitized = entry->Sanitize(value);
    // 値の置き場を外へ預けた項目は、あちらが自分で «編集された» を申告する
    // (MutableXxx)。二重に数えると、触っていない設定まで保存対象になる。
    if (entry->persist) ++detail::ValueRevision();
    entry->value = sanitized;
    if (entry->set) entry->set(sanitized);
    if (entry->onApply) entry->onApply(sanitized);
}

inline void SetBool(std::string_view id, bool value) { Set(id, value ? 1.0f : 0.0f); }
inline void SetInt(std::string_view id, int value) { Set(id, static_cast<float>(value)); }

/// 宣言簿が値を持っている項目を既定へ戻す。
///
/// WHY 外へ預けた項目を戻さないか: あちらの既定は VideoConfig{} のような
///     «構造体の初期化子» が正本で、宣言簿はそれを知らない (宣言には
///     defaultValue を書いていない)。ここで一緒に戻すと、書いていない 0 が
///     視野角や明るさへ流れ込む。外の項目は外が戻す。
inline void ResetToDefaults()
{
    for (Setting& entry : detail::Entries()) {
        if (!entry.persist) continue;
        Set(entry.id, entry.defaultValue);
    }
}

/// config の 1 テーブルへ往復する器。
///
/// WHY 宣言簿そのものを IScriptSerializable にしないか:
///   宣言簿は static な自由関数の集まりで、`Reflect` を持たせるには型が要る。
///   器を 1 つ挟めば「宣言簿の今の中身」を書き出す薄い層で済み、
///   宣言簿の側は永続化の作法を知らないままでいられる。
class Store final : public fbzz::scene::IScriptSerializable {
public:
    /// 値の置き場を宣言簿が持っている項目だけを往復させる。
    ///
    /// WHY 外へ預けた項目を書かないか: あちらは `[video]` / `[input]` のように
    ///     別のテーブルへ既に保存されている。両方へ書くと、片方だけ直した
    ///     設定ファイルがどちらの値で復元されるか判らなくなる。
    void Reflect(fbzz::scene::IReflector& r) override
    {
        for (Setting& entry : detail::Entries()) {
            if (!entry.persist) continue;
            r.Field(entry.id.c_str(), entry.value);
        }
    }

    /// 読み戻した値を «効かせる» ところまで通す。Read の直後に呼ぶ。
    ///
    /// WHY Reflect の中で効かせないか: Reflect は書き出しにも使われる。
    ///     そちらで onApply が走ると、保存するたびに演出が鳴る。
    void ApplyAll()
    {
        for (Setting& entry : detail::Entries()) {
            if (!entry.persist) continue;
            entry.value = entry.Sanitize(entry.value);
            if (entry.onApply) entry.onApply(entry.value);
        }
    }
};

/// config へ書き出す器。1 実体でよい (中身は宣言簿を見るだけ)。
[[nodiscard]] inline Store& PersistentStore()
{
    static Store store;
    return store;
}

/// @name 宣言のための小さな入口
/// 呼ぶ側が `Setting{}` を組み立てずに済む形。細かく指定したいときは Declare を直に使う。
///@{
inline void DeclareToggle(std::string id, std::string page, bool defaultValue,
                          std::function<void(float)> onApply = {})
{
    Setting setting;
    setting.id           = std::move(id);
    setting.page         = std::move(page);
    setting.kind         = Kind::Toggle;
    setting.defaultValue = defaultValue ? 1.0f : 0.0f;
    setting.onApply      = std::move(onApply);
    Declare(std::move(setting));
}

inline void DeclarePercent(std::string id, std::string page, float defaultValue,
                           std::function<void(float)> onApply = {})
{
    Setting setting;
    setting.id           = std::move(id);
    setting.page         = std::move(page);
    setting.kind         = Kind::Percent;
    setting.defaultValue = defaultValue;
    setting.onApply      = std::move(onApply);
    Declare(std::move(setting));
}

inline void DeclareNumber(std::string id, std::string page, float defaultValue,
                          float minimum, float maximum, int decimals = 2,
                          std::string suffix = {},
                          std::function<void(float)> onApply = {})
{
    Setting setting;
    setting.id           = std::move(id);
    setting.page         = std::move(page);
    setting.kind         = Kind::Number;
    setting.defaultValue = defaultValue;
    setting.min          = minimum;
    setting.max          = maximum;
    setting.decimals     = decimals;
    setting.suffix       = std::move(suffix);
    setting.onApply      = std::move(onApply);
    Declare(std::move(setting));
}

inline void DeclareChoice(std::string id, std::string page, int defaultIndex,
                          std::vector<std::string> choices,
                          std::function<void(float)> onApply = {})
{
    Setting setting;
    setting.id           = std::move(id);
    setting.page         = std::move(page);
    setting.kind         = Kind::Choice;
    setting.defaultValue = static_cast<float>(defaultIndex);
    setting.choices      = std::move(choices);
    setting.onApply      = std::move(onApply);
    Declare(std::move(setting));
}
///@}

} // namespace sandbox::settings
