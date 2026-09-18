/// @file    GameSettingsRegistry.hpp
/// @brief   設定項目の宣言簿。スクリプトが 1 行宣言すれば、保存・Option の行・読み口が揃う
/// @author  Hasegawa Jin
/// @date    2026-09-07
///
/// @note 設定を 1 つ増やすと保存・Option の行・読み口が自動で揃う。宣言簿を挟まないと
///       `GameConfig` の field/Reflect・Option 画面の行の表・`GetValue`/`SetValue` の
///       分岐をそれぞれ触ることになり、書き忘れが画面から見分けられない壊れ方をする。
/// @note スクリプトが増やす設定は数も型も事前に判らないため、id をキーにした 1 本の表で
///       持つ。値は「つまみ位置・選択肢番号・入切」のいずれかなので float 1 本に畳み、
///       見せ方 (Kind) だけを分ける。
/// @note 宣言簿は Scripts.dll の static でシーンをまたいで残る。`GameSettingsComponent`
///       が居ないシーンでも `settings::Get()` は最後の値を返す。
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
/// @note 表示・音量・入力は値の置き場がエンジン側 (display / audio / input プロキシ)
///       にあるため、get/set を差して「値の出し入れだけ外から借りる」形にできる。
///       差さなければ宣言簿が値を持ち、config へ自動で往復する。
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
    /// @note get/set は対で差す。片方だけ差すと「読むのは宣言簿・書くのは外」という
    ///       ねじれが生まれ、Option で動かした値が次のフレームで外の値に上書きされる。
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
/// @note 関数の中の static はヘッダーオンリーで 1 実体に保つための定石。初期化順が絡む
///       静的アクセサから触るため、「最初に呼ばれたときに作られる」形にしておく。
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
/// @note 宣言の版とは別に数える。«読み込み直す» (項目が増えた) と «保存する»
///       (値が変わった) は別の出来事で、同じ札にすると片方を空振りさせる。
///       組み込み項目は MutableXxx() が編集を申告するが、宣言簿が値を持つ項目には
///       その口が無く、ここで数えないと保存対象から漏れる。
inline int& ValueRevision()
{
    static int revision = 0;
    return revision;
}

} // namespace detail

/// 宣言されている全項目。
///
/// @note `Setting*` を持ち回らない。宣言が増えると vector が再確保され、控えていた
///       ポインタは無効になる。走査は都度その場で回すこと。
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
/// @note 値は保つ。宣言する側は OnStart で毎回宣言するため、差し替えのたびに既定へ
///       戻すとシーンを移るたびに設定が消える。宣言は「どんな項目か」を表すだけで
///       「今いくつか」ではない。
/// @note 版を進めるのは新規のときだけ。版は「読み込み直す必要があるか」を表し、
///       既存項目の宣言を直しても値は既に config から読めている。
inline void Declare(Setting setting)
{
    if (setting.id.empty()) return;

    /// @note 値の置き場を外へ預けた項目は、保存も外の担当。二重に持つと、
    ///       どちらが正本なのか宣言からは読めなくなる。
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
/// @note get() の結果を value へ写す。値の置き場が外にある項目でも、その置き場が
///       消えた後 (`GameSettingsComponent` の居ないシーン) に「最後に効いていた値」
///       を返せるようにするため。
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
    /// @note 値の置き場を外へ預けた項目は、あちらが自分で «編集された» を申告する
    ///       (MutableXxx)。二重に数えると、触っていない設定まで保存対象になる。
    if (entry->persist) ++detail::ValueRevision();
    entry->value = sanitized;
    if (entry->set) entry->set(sanitized);
    if (entry->onApply) entry->onApply(sanitized);
}

inline void SetBool(std::string_view id, bool value) { Set(id, value ? 1.0f : 0.0f); }
inline void SetInt(std::string_view id, int value) { Set(id, static_cast<float>(value)); }

/// 宣言簿が値を持っている項目を既定へ戻す。
///
/// @note 外へ預けた項目は戻さない。あちらの既定は `VideoConfig{}` のような構造体の
///       初期化子が正本で、宣言には defaultValue を書いていないため、ここで戻すと
///       書いていない 0 が視野角や明るさへ流れ込む。
inline void ResetToDefaults()
{
    for (Setting& entry : detail::Entries()) {
        if (!entry.persist) continue;
        Set(entry.id, entry.defaultValue);
    }
}

/// config の 1 テーブルへ往復する器。
///
/// @note 宣言簿は static な自由関数の集まりで `Reflect` を持たせるには型が要るため、
///       器を 1 つ挟んで「宣言簿の今の中身」を書き出す薄い層にする。
class Store final : public fbzz::scene::IScriptSerializable {
public:
    /// 値の置き場を宣言簿が持っている項目だけを往復させる。
    ///
    /// @note 外へ預けた項目は書かない。あちらは `[video]` / `[input]` のように別の
    ///       テーブルへ既に保存されており、両方へ書くと片方だけ直した設定ファイルが
    ///       どちらの値で復元されるか判らなくなる。
    void Reflect(fbzz::scene::IReflector& r) override
    {
        for (Setting& entry : detail::Entries()) {
            if (!entry.persist) continue;
            r.Field(entry.id.c_str(), entry.value);
        }
    }

    /// 読み戻した値を «効かせる» ところまで通す。Read の直後に呼ぶ。
    ///
    /// @note Reflect の中では効かせない。Reflect は書き出しにも使われるため、
    ///       そちらで onApply が走ると保存するたびに演出が鳴ってしまう。
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
