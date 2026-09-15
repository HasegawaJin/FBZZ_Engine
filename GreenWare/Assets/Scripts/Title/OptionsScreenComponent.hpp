/// @file    OptionsScreenComponent.hpp
/// @brief   OPTIONS 画面。行ウィジェットの状態を設定値と表示につなぐ
/// @author  Hasegawa Jin
/// @date    2026-08-23
///
/// 画面の作り (Reference: Assets/UI/Reference/Options_*.png):
///   Nav<TAB>Label (UIButton)          左のタブ
///   Tab_<PAGE>                        タブ 1 枚ぶんの親。active は 1 つだけ
///     <TAB>_Row_<key> (UIButton)      行全体の当たり判定
///       ├ Divider / Label / Value     見た目
///       ├ Track / Fill / Knob         スライダーの見た目 (任意)
///       └ Slider (UISlider)           ドラッグを受ける不可視の矩形 (任意)
///   CtrlDiv_<key> (UIButton)          右の CONTROLS 1 行。押すと割り当てを差し替える
///   CtrlR_KBM / CtrlR_PAD             右の CONTROLS 一覧。機器で入れ替わる
///
/// 操作は [[GameCursorComponent]] のカーソル 1 本。マウス・パッド・矢印キーの
/// どれでも同じ経路を通る。
///
/// WHY CONTROLS を別のタブにしないか:
///   Reference の画面では CONTROLS は常時右に出ている一覧で、どのタブを見ていても
///   操作表として読める位置にある。差し替えのためだけにタブへ移すと、
///   「今の割り当てを確かめる」という本来の役目の方が遠くなる。
///   一覧をそのまま押せるようにすれば、見る場所と直す場所が同じになる。
///
/// WHY index を持たないか:
///   「どの行にいるか」はカーソルの位置で既に決まっていて、UISystem が UIButton の
///   状態として持っている。スクリプトが別に持つと、ずれた瞬間にどちらが正しいのか
///   判断できなくなる。
///
/// WHY タブを切り替えたとき行を «置き直す» か (SetActive で入れ替えるだけにしないか):
///   ページが一瞬で入れ替わると、どの行が新しく来たのかが目で追えず、
///   «同じ画面の文字が書き換わった» ように見える。上の行から順に数十 ms ずつ
///   遅れて滑り込むと、«別の一覧が来た» ことが分かる。順番は上からで固定 ─
///   タブの並び順に関わらず、一覧は常に上から読むため。
///
/// WHY ドラッグを自前で書かないか:
///   UISlider が掴み判定と値の写像を持っている。同じ計算をスクリプトへ写すと、
///   Canvas Scaler や入れ子の変換が絡んだ場面でだけずれる。
#pragma once

#include <Engine/Scene/Components/UIText.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Script.hpp>
#include <Scripts/Game/GameSettingsComponent.hpp>
#include <Scripts/Game/GameSettingsRegistry.hpp>
#include <Scripts/UI/UiMotion.hpp>
#include <Scripts/UI/UiTextFx.hpp>
#include <Scripts/Utils/BgmLibrary.hpp>
#include <Scripts/Utils/InputActions.hpp>
#include <Scripts/Utils/SceneTransition.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

using namespace fbzz::scene;
using namespace fbzz::math;

namespace sandbox {

// 振動の «試聴»。RumbleManager の Default Shape (0.55 / 0.30) に合わせてある。
// ここだけ別の配分にすると、試したときの手触りと戦闘で返る手触りが別物になる。
inline constexpr float kVibrationPreviewLow  = 0.55f;
inline constexpr float kVibrationPreviewHigh = 0.30f;
// つまみが動かなくなったら自然に切れる長さ。伸ばすと離した後まで手に残り、
// «今どの値か» ではなく «さっきどこを通ったか» を触っていることになる。
inline constexpr float kVibrationPreviewSeconds = 0.14f;

class OptionsScreenComponent : public Script {
    FBZZ_SCRIPT(OptionsScreenComponent)

public:
    FBZZ_GROUP("流れ")
    FBZZ_FIELD(std::string, backScene, "Title", "Back Scene")
    FBZZ_TOOLTIP("Esc / B で戻る先。戻る前に設定を保存する。空なら戻らない")
    FBZZ_FIELD(bool, standalone, true, "単独の画面")
    FBZZ_TOOLTIP("Options シーンの正本として使うか。切ると BGM を掛け替えず、扉 (ワイプ) も"
                 "描かず、Esc / B も受けない ─ ポーズ画面の中に置くときはこちら "
                 "(開け閉めと保存は PauseMenuComponent が持つ)")

    FBZZ_GROUP("見た目")
    FBZZ_FIELD_COLOR(navDimColor, (Vector4{ 0.435294f, 0.427451f, 0.407843f, 1.0f }), "Nav Dim")
    FBZZ_FIELD_COLOR(navActiveColor, (Vector4{ 1.0f, 1.0f, 1.0f, 1.0f }), "Nav Active")
    FBZZ_FIELD_COLOR(rowDimColor, (Vector4{ 0.662745f, 0.650980f, 0.627451f, 1.0f }), "Row Dim")
    FBZZ_TOOLTIP("カーソルが乗っていない行のラベル色。Reference の rgb(169,166,160)")
    FBZZ_FIELD_COLOR(rowActiveColor, (Vector4{ 1.0f, 1.0f, 1.0f, 1.0f }), "Row Active")
    FBZZ_FIELD_COLOR(valueDimColor, (Vector4{ 0.870588f, 0.858824f, 0.835294f, 1.0f }), "Value Dim")
    FBZZ_FIELD_RANGE(float, blendSeconds, 0.09f, "ブレンド", 0.0f, 1.0f)
    FBZZ_FIELD(float, navNudge, 8.0f, "Nav Nudge")
    FBZZ_TOOLTIP("選択中・ホバー中のタブ文字を右へ押し出す量 [px]")
    FBZZ_FIELD_RANGE(float, focusSeconds, 0.07f, "Focus Follow", 0.0f, 1.0f)
    FBZZ_TOOLTIP("フォーカスバーが行へ寄る速さ。0 で瞬間移動")

    FBZZ_GROUP("Page")
    FBZZ_FIELD(float, pageSlide, 34.0f, "滑り")
    FBZZ_TOOLTIP("タブを切り替えたとき、行が出る前の位置のずれ [px]。正で右から滑り込む")
    FBZZ_FIELD_RANGE(float, pageStagger, 0.045f, "のけぞり", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, pageSeconds, 0.34f, "継続時間", 0.05f, 2.0f)
    FBZZ_FIELD_RANGE(float, pageDelay, 0.10f, "First Delay", 0.0f, 2.0f)
    FBZZ_TOOLTIP("画面に入った最初の 1 回だけ、題字の出現 (UiReveal) を待つ秒数")

    FBZZ_GROUP("デバッグ")
    FBZZ_FIELD_READ_ONLY(std::string, debugTab, "-", "Tab")
    FBZZ_FIELD_READ_ONLY(std::string, debugRow, "-", "Hovered Row")

    void OnStart() override;
    void OnUpdate() override;

private:
    bool m_wipeWriting = false;   ///< 扉を自分で postprocess へ書いたか (transition::Drive)
    /// 見せ方も範囲も選択肢も宣言簿 (GameSettingsRegistry.hpp) が持つ。
    ///
    /// WHY 画面側に行の表を持たないか (2026-09-07 に外した):
    ///   以前はここに 24 行の表があり、値の出し入れも 2 本の if 連鎖で書いていた。
    ///   設定を 1 つ増やすのに «構造体 / 表 / GetValue / SetValue / 選択肢» の
    ///   5 か所を触ることになり、しかも 1 つ書き忘れても画面には «出るが保存
    ///   されない» としか出ない。宣言を 1 か所へ畳めば、書き忘れる場所が消える。
    ///
    /// WHY 宣言の page ではなくシーンの有無で行を決めるか:
    ///   INPUT のページはキーボードとパッドで 2 枚あり、同じ id (device) の行が
    ///   両方に居る。宣言に «どのページか» を持たせると 1 つしか書けない。
    ///   «そのページに行のオブジェクトがあるか» で決めれば、置いた場所がそのまま
    ///   答えになり、行を増やすのもシーン側だけで済む。
    void SelectTab(int tab);
    void BindSettings();
    void ApplyDeviceGroups();
    void LayoutControlIcons();
    void PollRows();
    void RefreshNav(float dt);

    /// @name キーコンフィグ
    /// CONTROLS の行を押すと、その操作の割り当てを次に押した入力へ差し替える。
    ///@{
    void PollControls();
    void BeginRebind(const actions::ControlRow& row);
    /// リバインドの完了・取消をさばく。待機が続いているなら true。
    [[nodiscard]] bool UpdateRebind();
    /// 1 行ぶんの右側 (アイコン or 文字) を今の割り当てへ合わせる。
    void RefreshControlRow(std::size_t rowIndex);
    void RefreshAllControlRows();
    [[nodiscard]] int CurrentDevice() const;
    ///@}

    /// 宣言簿へ書き、この画面だけの副作用 (試聴・ページの入れ替え) を足す。
    void  SetValue(const settings::Setting& setting, float value);
    /// 今つまんだ強さでパッドを 1 度回す。振動は数字を読んでも決められない。
    void  PreviewVibration(float scale01) const;
    [[nodiscard]] std::string FormatValue(const settings::Setting& setting, float value) const;
    /// 行の子を名前で引く。子の名前 (Label / Value / Slider) は行をまたいで同じ。
    [[nodiscard]] static GameObject* Child(GameObject* parent, std::string_view name);

    static constexpr int kTabs = 4;
    static constexpr const char* kTabNames[kTabs] = { "INPUT", "GAME", "VIDEO", "AUDIO" };
    [[nodiscard]] std::string PageOfTab(int tab) const;

    GameObject* m_navBars[kTabs]   = {};
    GameObject* m_navLabels[kTabs] = {};
    GameObject* m_focusBar = nullptr;
    float m_navAmount[kTabs] = {};
    Vector3 m_navOrigin[kTabs] = {};   ///< タブ文字の置き場所 (寄りの基準)
    std::string m_navText[kTabs];      ///< タブの素の文言
    std::string m_navRich[kTabs];      ///< 最後に流し込んだ文字列
    float m_navClock = 0.0f;           ///< 走査の位相
    /// フォーカスバー。行へ «寄る» ので、目標と今の位置を別に持つ。
    float m_focusY      = 0.0f;
    float m_focusTarget = 0.0f;
    float m_focusAmount = 0.0f;
    bool  m_focusPlaced = false;

    /// ページの出現。行ごとに «置いてあった位置» と «子の色» を控える。
    ///
    /// WHY GameObject* で引くか: SelectTab は宣言簿の版が変わるたびに m_bound を
    ///     作り直す。そのたびに位置を控え直すと、滑り込みの途中で控えた位置が
    ///     «本来の位置» にすり替わって行が流れる。行の実体で引けば、同じ行は
    ///     最初に控えた位置を持ち続ける。
    struct PageRow {
        Vector3                     origin = {};
        std::vector<uimotion::Slot> parts;   ///< Divider / Track / Fill / Knob
    };
    std::unordered_map<GameObject*, PageRow> m_pageRows;
    std::string m_pageName;        ///< 今出しているページ。変わったときだけ出現をやり直す
    float m_page     = 0.0f;       ///< ページを出し始めてからの秒数
    bool  m_pageDone = true;
    bool  m_firstPage = true;      ///< 画面に入って最初のページか (題字を待つ)

    /// 行 1 本の «出現 × ホバー» の見た目を書く。
    void PaintRow(std::size_t index, GameObject* go, bool hovered);
    /// 出現の途中で切り替えるとき、行を本来の位置へ戻してから隠す。
    void SettlePage();
    [[nodiscard]] float RowReveal(std::size_t index) const;

    /// 表示中のページに属する行。SelectTab のたびに引き直す。
    /// WHY 毎フレーム探さないか: scene.Find はシーン全体の名前検索で、
    ///     行 6 本ぶんを毎フレーム回すと 200 オブジェクトを何度も走査することになる。
    ///
    /// WHY Setting* ではなく id を控えるか: 宣言が増えると宣言簿の vector が
    ///     再確保され、控えたポインタは無効になる。id なら宣言簿が伸びても指し続ける。
    struct RowBinding {
        std::string id;
        GameObject* go;
        float sliderValue = 0.0f;
    };
    std::vector<RowBinding> m_bound;
    /// m_bound を作ったときの宣言簿の版。増えていたら行を引き直す。
    int m_boundRevision = -1;
    /// m_bound を作り直すたびに 1 進む。
    ///
    /// WHY 要るか: 入力機器の行を押すと SetValue の中で SelectTab が走り、
    ///     走査中の m_bound がその場で作り直される。番号が変わったら走査を
    ///     打ち切らないと、消えた要素を指したまま回り続ける。
    int m_bindGeneration = 0;

    int  m_tab = 2;                  // Reference と同じ VIDEO から開く
    bool m_ctrlLaidOut = false;
    bool m_lastCancel  = false;
    bool m_warnedNoSettings = false;

    /// 差し替え待ちの操作。空なら待機していない。
    std::string m_rebindAction;
    int  m_rebindDevice = 0;
    /// 差し替える枠。押された入力で機器の種類が変わっても、持ち主はここで決まる。
    int  m_rebindIndex  = -1;
    /// 待機に入る前の割り当て。別の機器のボタンを押されたときの戻り先。
    ScriptInputBinding m_rebindPrevious;

    static constexpr std::size_t kControlRowCount =
        sizeof(actions::kControlRows) / sizeof(actions::kControlRows[0]);
    /// シーンに書かれていた補足文字。差し替えを解除したときの戻り先。
    /// WHY 控えるか: 待機中は同じ場所へ「…」を出す。元の文言を持っていないと、
    ///     既定へ戻したあとも案内文が居座る。
    std::string m_ctrlTail[2][kControlRowCount];
    /// 設定の実体が見つかってから 1 度だけ行う初期化を済ませたか。
    /// WHY OnStart でやらないか: スクリプトの開始順は保証されないので、
    ///     GameSettingsComponent がこちらより後に始まると Instance() が null になる。
    bool m_settingsBound = false;
};

FBZZ_REFLECT(OptionsScreenComponent)

// ── 行の表 ──────────────────────────────────────────────────────────────────
inline GameObject* OptionsScreenComponent::Child(GameObject* parent, std::string_view name)
{
    if (!parent) return nullptr;
    for (int i = 0, n = parent->GetChildCount(); i < n; ++i)
        if (GameObject* child = parent->GetChild(i); child && child->name == name) return child;
    return nullptr;
}

// ── 値の読み書き ────────────────────────────────────────────────────────────
// 値の出し入れは宣言簿が持つ。ここに残るのは «この画面でだけ起きること» の 2 つで、
// どちらも設定の値ではなく操作への応答なので、宣言の側へは移さない。
inline void OptionsScreenComponent::SetValue(const settings::Setting& setting, float value)
{
    const std::string id = setting.id;   // Set が宣言簿を触るので、先に写しておく
    settings::Set(id, value);

    // 入力機器を変えると INPUT のページそのものが入れ替わる。行の束を引き直さないと、
    // 前のページの行を掴んだまま別のページを見ることになる。
    if (id == "device") {
        ApplyDeviceGroups();
        SelectTab(m_tab);
        return;
    }
    // 振動は数字を読んでも決められない。つまんだ強さでその場で 1 度回す。
    if (id == "vibration") PreviewVibration(value);
}

inline void OptionsScreenComponent::PreviewVibration(float scale01) const
{
    // WHY ここだけ RumbleManager を通さず直に書くか: あれは «同時に届いた要求を合成する»
    //     ための係で、戦闘シーンにしか居ない。この画面には振動を要求する相手が他に
    //     一人も居ないので、合成すべきものが無い。
    //
    // WHY 溜めずに «変わったフレームだけ» 出し直すか: SetVibration は指定秒で自動停止する。
    //     つまみが止まれば呼ばれなくなり、こちらが止めなくても切れる。離した・タブを
    //     変えた・画面を抜けた、のどれでも回りっぱなしにならない。
    const float level = std::clamp(scale01, 0.0f, 1.0f);
    if (level <= 0.0f) return;
    input.SetVibration(kVibrationPreviewLow * level, kVibrationPreviewHigh * level,
                       kVibrationPreviewSeconds);
}

inline std::string OptionsScreenComponent::FormatValue(const settings::Setting& setting,
                                                       float value) const
{
    char buffer[64] = {};
    switch (setting.kind) {
    case settings::Kind::Percent:
        std::snprintf(buffer, sizeof(buffer), "%d%%", static_cast<int>(std::lround(value * 100.0f)));
        return buffer;
    case settings::Kind::Number:
        std::snprintf(buffer, sizeof(buffer), "%.*f%s", setting.decimals, value,
                      setting.suffix.c_str());
        return buffer;
    case settings::Kind::Toggle:
        return value >= 0.5f ? "ON" : "OFF";
    case settings::Kind::Choice: {
        if (setting.choices.empty()) return "-";
        const std::size_t index = static_cast<std::size_t>(
            std::clamp(static_cast<int>(std::lround(value)), 0,
                       static_cast<int>(setting.choices.size()) - 1));
        return setting.choices[index];
    }
    }
    return "-";
}

// ── ライフサイクル ──────────────────────────────────────────────────────────
inline std::string OptionsScreenComponent::PageOfTab(int tab) const
{
    if (tab != 0) return std::string("Tab_") + kTabNames[tab];
    auto* s = GameSettingsComponent::Instance();
    return (s && s->Input().device == 1) ? "Tab_INPUT_PAD" : "Tab_INPUT";
}

inline void OptionsScreenComponent::OnStart()
{
    // 音量のつまみを触る画面なので、無音では «今いくつか» が耳で分からない。
    // WHY ポーズの中では鳴らさないか: 盤面には既に曲が掛かっていて、掛け替えると
    //     «音量を直しに来ただけ» で戦闘の曲が止まる。閉じても戻せない。
    if (standalone) bgm::Play(audio, bgm::kOptions);

    for (int i = 0; i < kTabs; ++i) {
        m_navBars[i]   = scene.Find(std::string("Nav") + kTabNames[i] + "Bar");
        m_navLabels[i] = scene.Find(std::string("Nav") + kTabNames[i] + "Label");
        if (!m_navBars[i] || !m_navLabels[i])
            debug.LogWarning(std::string("OptionsScreen: Nav") + kTabNames[i] + " が見つかりません");
        m_navAmount[i] = (i == m_tab) ? 1.0f : 0.0f;
        if (m_navLabels[i]) {
            m_navOrigin[i] = m_navLabels[i]->transform.position;
            if (auto* t = m_navLabels[i]->GetComponent<UIText>()) {
                t->richText  = true;   // 色付き文字列を流す
                m_navText[i] = t->text;
            }
        }
    }
    m_focusBar = scene.Find("FocusBar");
    if (m_focusBar) {
        m_focusY = m_focusTarget = m_focusBar->transform.position.y;
        // 最初はどの行にも居ないので消しておく (出したまま置くと 1 本目の行に
        // 乗る前から光っている)。
        ui.SetMaterialFloat(m_focusBar, "selected", 0.0f);
    }

    // CONTROLS の補足文字はシーンが正本。差し替え表示で潰す前に控える。
    for (int deviceIndex = 0; deviceIndex < 2; ++deviceIndex) {
        const char* group = deviceIndex == 1 ? "PAD" : "KBM";
        for (std::size_t i = 0; i < kControlRowCount; ++i) {
            GameObject* tail = scene.Find(std::string("CtrlR_") + group + "_"
                                          + actions::kControlRows[i].key);
            const auto* text = tail ? tail->GetComponent<UIText>() : nullptr;
            m_ctrlTail[deviceIndex][i] = text ? text->text : std::string{};
        }
    }

    SelectTab(m_tab);
}

/// 設定の実体が見つかった最初のフレームに 1 度だけ行う。
inline void OptionsScreenComponent::BindSettings()
{
    auto* s = GameSettingsComponent::Instance();
    if (!s) return;

    // パッドが挿さっていて、まだ一度も選んでいないなら PAD 側で開く。
    // WHY 常に上書きしないか: Option で明示的にキーボードを選んだ設定を、
    //     パッドを挿しただけで奪わない。
    if (s->Input().device == 0 && input.IsPadConnected() && !config.Has("input")) {
        s->MutableInput().device = 1;
        s->Apply();
    }

    ApplyDeviceGroups();
    SelectTab(m_tab);          // INPUT のページは device で変わるので引き直す
    m_settingsBound = true;
}

inline void OptionsScreenComponent::ApplyDeviceGroups()
{
    auto* s = GameSettingsComponent::Instance();
    const bool pad = s && s->Input().device == 1;
    if (auto* go = scene.Find("CtrlR_KBM")) go->SetActive(!pad);
    if (auto* go = scene.Find("CtrlR_PAD")) go->SetActive(pad);
    // 差し替え済みの行は絵ではなく文字で出る。機器を切り替えた側の一覧へも反映する。
    RefreshAllControlRows();
}

inline void OptionsScreenComponent::SelectTab(int tab)
{
    m_tab = (tab + kTabs) % kTabs;
    const std::string page = PageOfTab(m_tab);
    const bool pageChanged = (page != m_pageName);
    // 別のページへ移るなら、今のページの行を本来の位置へ戻してから隠す
    // (滑り込みの途中で隠すと、次に出したときその位置が «置き場所» になる)。
    if (pageChanged) SettlePage();
    for (const char* name : { "Tab_INPUT", "Tab_INPUT_PAD", "Tab_GAME", "Tab_VIDEO", "Tab_AUDIO" })
        if (auto* go = scene.Find(name)) go->SetActive(page == name);

    // 宣言簿の全項目に対して «このページに行のオブジェクトがあるか» を見る。
    // 置いてある行だけが出るので、設定を増やすのは «宣言 1 つ + 行 1 つ» で済む。
    const std::string tag = page.substr(4);   // "Tab_VIDEO" -> "VIDEO"
    m_bound.clear();
    for (const settings::Setting& setting : settings::All()) {
        if (GameObject* go = scene.Find(tag + "_Row_" + setting.id)) {
            m_bound.push_back({ setting.id, go });
            if (GameObject* slider = Child(go, "Slider")) {
                const float value = settings::Get(setting.id);
                const float ratio = setting.kind == settings::Kind::Percent ? value
                    : (value - setting.min) / (std::max)(setting.max - setting.min, 1e-4f);
                ui.SetSliderRange(slider, 0.0f, 1.0f);
                ui.SetSliderValue(slider, std::clamp(ratio, 0.0f, 1.0f));
                m_bound.back().sliderValue = ui.GetSliderValue(slider);
            }
        }
    }
    m_boundRevision = settings::Revision();
    ++m_bindGeneration;
    debugTab = kTabNames[m_tab];

    // 行の置き場所と子の色を控える。控え済みの行 (同じページの引き直し) はそのまま。
    if (pageChanged) m_pageRows.clear();
    for (const RowBinding& b : m_bound) {
        if (!b.go || m_pageRows.count(b.go)) continue;
        PageRow row;
        row.origin = b.go->transform.position;
        for (const char* part : { "Divider", "Track", "Fill", "Knob" }) {
            GameObject* child = Child(b.go, part);
            if (!child) continue;
            uimotion::Slot slot;
            slot.go     = child;
            slot.origin = child->transform.position;
            slot.image  = true;
            slot.color  = ui.GetImageColor(child);
            row.parts.push_back(slot);
        }
        m_pageRows.emplace(b.go, row);
    }
    if (pageChanged) {
        m_pageName = page;
        // 最初の 1 回だけ題字の出現を待つ。タブを押した後は待たない
        // (押した手応えが遅れると «効いていない» と思わせる)。
        m_page     = m_firstPage ? -pageDelay : 0.0f;
        m_pageDone = m_bound.empty();
        m_firstPage = false;
        // 1 フレーム目から «出ていない» で描く。
        for (std::size_t i = 0; i < m_bound.size(); ++i) PaintRow(i, m_bound[i].go, false);
    }
}

inline float OptionsScreenComponent::RowReveal(std::size_t index) const
{
    return uimotion::Stagger(m_page, static_cast<int>(index), pageStagger, pageSeconds);
}

inline void OptionsScreenComponent::SettlePage()
{
    for (auto& [go, row] : m_pageRows) {
        if (!go) continue;
        go->transform.position = row.origin;
        for (uimotion::Slot& part : row.parts)
            if (part.go) ui.SetImageColor(part.go, part.color);
    }
    // Label / Value の α は PollRows が毎フレーム 1 で書き直すので、ここでは触らない。
}

inline void OptionsScreenComponent::PaintRow(std::size_t index, GameObject* go, bool hovered)
{
    auto it = m_pageRows.find(go);
    if (it == m_pageRows.end() || !go) return;
    PageRow& row = it->second;

    const float t     = RowReveal(index);
    const float e     = uimotion::OutCubic(t);
    const float alpha = uimotion::OutQuint(t * 1.25f);

    go->transform.position = { row.origin.x + pageSlide * (1.0f - e), row.origin.y, row.origin.z };
    for (uimotion::Slot& part : row.parts) {
        if (!part.go) continue;
        ui.SetImageColor(part.go, { part.color.x, part.color.y, part.color.z, part.color.w * alpha });
    }
    if (GameObject* label = Child(go, "Label")) {
        const Vector4 c = hovered ? rowActiveColor : rowDimColor;
        ui.SetTextColor(label, { c.x, c.y, c.z, c.w * alpha });
    }
    if (GameObject* valueText = Child(go, "Value")) {
        const Vector4 c = hovered ? rowActiveColor : valueDimColor;
        ui.SetTextColor(valueText, { c.x, c.y, c.z, c.w * alpha });
    }
}

inline void OptionsScreenComponent::OnUpdate()
{
    const float dt = (std::max)(time.UnscaledDeltaTime(), 0.0f);

    // ページの出現は扉 (ワイプ) が開いている最中も進める。止めると、扉が
    // 開き切った瞬間に行が一斉に飛び出す。
    if (!m_pageDone) {
        m_page += dt;
        const float total = pageStagger * static_cast<float>(m_bound.empty() ? 0 : m_bound.size() - 1)
                          + pageSeconds;
        if (m_page >= total) { m_page = total; m_pageDone = true; }
    }
    // フォーカスバーは行へ «寄る»。瞬間移動だと、隣の行へ移るたびに 2 本あるように見える。
    if (m_focusBar) {
        m_focusY = uimotion::Approach(m_focusY, m_focusTarget, dt, focusSeconds);
        const Vector3 p = m_focusBar->transform.position;
        m_focusBar->transform.position = { p.x, m_focusY, p.z };
        ui.SetMaterialFloat(m_focusBar, "selected", m_focusAmount);
    }

    // 扉 (ワイプ)。塗っている / 剥がしている最中は入力を受けない。
    // 遊びのシーン (ポーズの中) では ScreenEffectManager が書き手なので、進めるだけにする
    // ─ 2 人が PostProcessSettings を書くと互いを消す (SceneTransition.hpp)。
    if (transition::Drive(dt, scene, postprocess, m_wipeWriting, !standalone)) {
        // 見た目だけは書き続ける (出現の途中で扉が閉まっても行が固まらない)。
        for (std::size_t i = 0; i < m_bound.size(); ++i) PaintRow(i, m_bound[i].go, false);
        RefreshNav(dt);
        return;
    }

    if (!m_settingsBound) BindSettings();
    // スクリプトが後から設定を宣言したら、その行を拾い直す。
    // WHY 毎フレーム引き直さないか: SelectTab は宣言の数だけ scene.Find を回す。
    //     版が変わったときだけで足り、変わらない限りは 1 回の整数比較で済む。
    if (m_boundRevision != settings::Revision()) SelectTab(m_tab);

    // 差し替え待ちの間は他を一切受けない。押した「次の 1 入力」がそのまま
    // 割り当てになるので、同じ入力でタブが動いたり画面を抜けたりすると、
    // 何に割り当たったのかがプレイヤーから見えなくなる。
    if (UpdateRebind()) {
        RefreshNav(dt);
        return;
    }

    // タブはカーソルで押す。ホバーだけでは切り替えない。
    // WHY: 行へ向かってカーソルを動かす途中でタブの上を通るため、ホバー追従だと
    //      触っていないタブへ勝手に飛ぶ。
    for (int i = 0; i < kTabs; ++i)
        if (m_navLabels[i] && ui.WasClicked(m_navLabels[i]) && i != m_tab) SelectTab(i);

    // 戻る。画面上に戻るボタンが無いのでカーソルとは別系統で受ける。
    // WHY ポーズの中では受けないか: 同じ Esc を «一段戻る» として使う相手が外に居る。
    //     両方が受けると、閉じたのがページなのか画面なのかが押した人から見えなくなる。
    const bool cancel = standalone
                     && (input.GetKeyDown(fbzz::input::KeyCode::ESCAPE)
                         || (input.IsPadConnected()
                             && input.GetPadButtonDown(fbzz::input::GamepadButton::B)));
    if (cancel && !m_lastCancel && !backScene.empty()) {
        if (auto* s = GameSettingsComponent::Instance()) s->Save();
        (void)transition::Begin(backScene);
        return;
    }
    m_lastCancel = cancel;

    PollRows();
    PollControls();
    RefreshNav(dt);
    if (!m_ctrlLaidOut) LayoutControlIcons();
}

// ── キーコンフィグ ───────────────────────────────────────────────────────────
inline int OptionsScreenComponent::CurrentDevice() const
{
    auto* s = GameSettingsComponent::Instance();
    return (s && s->Input().device == 1) ? 1 : 0;
}

inline void OptionsScreenComponent::PollControls()
{
    for (const actions::ControlRow& row : actions::kControlRows) {
        if (!row.action || !*row.action) continue;   // 軸と点付与は差し替えられない
        GameObject* hit = scene.Find(std::string("CtrlDiv_") + row.key);
        if (hit && ui.WasClicked(hit)) {
            BeginRebind(row);
            return;
        }
    }
}

inline void OptionsScreenComponent::BeginRebind(const actions::ControlRow& row)
{
    auto* s = GameSettingsComponent::Instance();
    if (!s) return;

    const int device = CurrentDevice();
    const int index  = s->BindingIndexFor(row.action, device);
    if (index < 0) {
        debug.LogWarning(std::string("OptionsScreen: ") + row.action
                         + " に今の入力機器のバインドがありません");
        return;
    }

    m_rebindAction   = row.action;
    m_rebindDevice   = device;
    m_rebindIndex    = index;
    m_rebindPrevious = input.GetActionBinding(row.action, index);
    input.BeginRebindAction(row.action, index);

    // 待機中であることは行の右側に出す。押す場所と結果が同じ行に並ぶ。
    const char* group = device == 1 ? "PAD" : "KBM";
    for (int i = 0; i < 4; ++i)
        if (auto* icon = scene.Find(std::string("CtrlIcon_") + group + "_" + row.key
                                    + "_" + std::to_string(i)))
            icon->SetActive(false);
    if (auto* tail = scene.Find(std::string("CtrlR_") + group + "_" + row.key))
        ui.SetText(tail, "…  BS:既定 / ESC:取消");
}

inline bool OptionsScreenComponent::UpdateRebind()
{
    if (m_rebindAction.empty()) return false;

    // 既定へ戻す。待機中にだけ受けるのは、押す場所とその意味が
    // 「今この行を触っている」という文脈の中にしか無いから。
    //
    // WHY 完了フラグを捨てるか: エンジンの待機は「次に押された入力」を拾うので、
    //     この BackSpace 自体が割り当てとして確定している。ResetBinding が
    //     上書きし直すが、完了の合図を残すと次の待機の頭で誤って消費される。
    if (input.GetKeyDown(fbzz::input::KeyCode::BACKSPACE)) {
        input.CancelRebind();
        (void)input.ConsumeRebindCompleted();
        if (auto* s = GameSettingsComponent::Instance())
            s->ResetBinding(m_rebindAction, m_rebindDevice);
        m_rebindAction.clear();
        RefreshAllControlRows();
        return true;
    }

    // 取消。待機に入る前へ戻す。
    if (input.GetKeyDown(fbzz::input::KeyCode::ESCAPE)) {
        input.CancelRebind();
        if (m_rebindPrevious.IsValid())
            input.SetActionBinding(m_rebindAction, m_rebindIndex, m_rebindPrevious);
        m_rebindAction.clear();
        m_lastCancel = true;   // 同じ押下で画面まで抜けないようにする
        RefreshAllControlRows();
        return true;
    }

    if (!input.ConsumeRebindCompleted()) {
        if (input.IsRebinding()) return true;
        // 待機がこちらの知らないところで終わった (取消が別経路で走った)。
        // 表示だけ元へ戻し、次のフレームから通常の操作を受け直す。
        m_rebindAction.clear();
        RefreshAllControlRows();
        return false;
    }

    auto* s = GameSettingsComponent::Instance();
    if (!s) {
        m_rebindAction.clear();
        return false;
    }

    // WHY 別機器の入力を弾くか: 「キーボードの回避」の行でパッドの A を押されると、
    //     その行はもうキーボードのバインドではなくなる。以降 device で引き当てる
    //     経路が全て別の枠を指し始めるので、割り当ての持ち主が判らなくなる。
    const ScriptInputBinding recorded = input.GetActionBinding(m_rebindAction, m_rebindIndex);
    const bool isPad = recorded.source == 2 || recorded.source == 3;
    if (!recorded.IsValid() || isPad != (m_rebindDevice == 1)) {
        if (m_rebindPrevious.IsValid())
            input.SetActionBinding(m_rebindAction, m_rebindIndex, m_rebindPrevious);
        debug.LogWarning("OptionsScreen: 今の入力機器と違うボタンが押されたため、"
                         "割り当ては変更しませんでした");
    } else {
        s->CaptureBinding(m_rebindAction, m_rebindDevice, m_rebindIndex);
    }

    m_rebindAction.clear();
    RefreshAllControlRows();
    return false;
}

inline void OptionsScreenComponent::RefreshControlRow(std::size_t rowIndex)
{
    auto* s = GameSettingsComponent::Instance();
    const actions::ControlRow& row = actions::kControlRows[rowIndex];
    if (!s || !row.action || !*row.action) return;

    const int device = CurrentDevice();
    const char* group = device == 1 ? "PAD" : "KBM";
    const bool custom = s->IsBindingOverridden(row.action, device);

    // WHY 差し替えたら絵をやめて文字にするか: アイコンのシートは既定の割り当てぶんしか
    //     用意していない。任意のキーに絵を当てようとすると、無い絵の穴埋めが必要になり、
    //     しかも当たっている絵と当たっていない絵が混ざる。差し替えた行だけは
    //     エンジンが持っている表示名 ("F" / "Pad0:A") をそのまま文字で出す。
    for (int i = 0; i < 4; ++i)
        if (auto* icon = scene.Find(std::string("CtrlIcon_") + group + "_" + row.key
                                    + "_" + std::to_string(i)))
            icon->SetActive(!custom);

    GameObject* tail = scene.Find(std::string("CtrlR_") + group + "_" + row.key);
    if (!tail) return;
    if (custom) {
        const int index = s->BindingIndexFor(row.action, device);
        ui.SetText(tail, input.DescribeActionBinding(row.action, index));
    } else {
        ui.SetText(tail, m_ctrlTail[device][rowIndex]);
    }
}

inline void OptionsScreenComponent::RefreshAllControlRows()
{
    for (std::size_t i = 0; i < kControlRowCount; ++i) RefreshControlRow(i);
    // 文字幅が変わるので右揃えを取り直す。実測が出るまで LayoutControlIcons が再試行する。
    m_ctrlLaidOut = false;
}

// ── 行 ──────────────────────────────────────────────────────────────────────
inline void OptionsScreenComponent::PollRows()
{
    // 設定の実体が無いときは何も触らない。
    // WHY 早期に抜けるか: 読む側は宣言簿があるので値は返るが、書く側 (組み込み項目の
    //     set) は実体を通る。実体が居ないまま行を触らせると、つまみを掴んでも
    //     どこにも書かれず「離すと戻る」形で潰れる。読めることと直せることは別。
    if (!GameSettingsComponent::Instance()) {
        if (!m_warnedNoSettings) {
            debug.LogError("OptionsScreen: GameSettingsComponent がシーンに居ません。"
                           "設定の読み書きができないため、行の値は反映されません");
            m_warnedNoSettings = true;
        }
        debugRow = "(no settings)";
        return;
    }

    debugRow = "-";
    bool anyHovered = false;
    const float dt = (std::max)(time.UnscaledDeltaTime(), 0.0f);
    const int generation = m_bindGeneration;
    for (std::size_t index = 0; index < m_bound.size(); ++index) {
        // SetValue の中で行の束が作り直されたら、そこで走査を打ち切る
        // (m_bindGeneration の WHY)。
        if (m_bindGeneration != generation) return;

        const std::string id = m_bound[index].id;
        GameObject* go = m_bound[index].go;
        const settings::Setting* found = settings::Find(id);
        if (!found || !go) continue;

        const bool hovered = ui.IsHovered(go) || ui.IsPressed(go);
        if (hovered) debugRow = id;

        const bool  percent = found->kind == settings::Kind::Percent;
        const float minimum = found->min;
        const float maximum = found->max;
        float value = settings::Get(id);

        GameObject* sliderGO = Child(go, "Slider");
        if (sliderGO) {
            const float ratio = ui.GetSliderValue(sliderGO);
            // 描画パスの一時通知が次の Script 更新まで残らなくても、最後に同期した値との差は残る。
            if (ratio != m_bound[index].sliderValue) {
                value = percent ? ratio : minimum + ratio * (maximum - minimum);
                SetValue(*found, value);
                if (m_bindGeneration != generation) return;
                value = settings::Get(id);
            }
            const float appliedRatio = percent ? value
                : (value - minimum) / (std::max)(maximum - minimum, 1e-4f);
            ui.SetSliderValue(sliderGO, std::clamp(appliedRatio, 0.0f, 1.0f));
            m_bound[index].sliderValue = ui.GetSliderValue(sliderGO);
        } else if (ui.WasClicked(go)) {
            // スライダーの無い行は、押すたびに次の値へ送る。
            if (found->kind == settings::Kind::Toggle) {
                value = value >= 0.5f ? 0.0f : 1.0f;
            } else if (found->kind == settings::Kind::Choice) {
                const int count = found->ChoiceCount();
                if (count > 0)
                    value = static_cast<float>((static_cast<int>(std::lround(value)) + 1) % count);
            }
            SetValue(*found, value);
            if (m_bindGeneration != generation) return;
            value = settings::Get(id);
        }

        // SetValue が宣言簿を触りうるので、書き出しの直前に引き直す。
        found = settings::Find(id);
        if (!found) continue;
        const settings::Setting& row = *found;

        // ── 見た目 ────────────────────────────────────────────────────────
        if (GameObject* valueText = Child(go, "Value"))
            ui.SetText(valueText, FormatValue(row, value));
        // 出現 (位置と α) とホバー (色) はまとめて 1 か所で書く。
        PaintRow(index, go, hovered);
        if (sliderGO) {
            // 素材は左右に 32px の余白を持つ。芯の ratio 割は (pad + ratio*芯幅) / PNG幅。
            constexpr float kTrackW = 230.0f, kPngW = 295.0f, kPad = 32.0f, kKnobHalfW = 1.5f;
            const float ratio = std::clamp(ui.GetSliderValue(sliderGO), 0.0f, 1.0f);
            if (GameObject* fill = Child(go, "Fill"))
                ui.SetImageFillAmount(fill, (kPad + ratio * kTrackW) / kPngW);
            if (GameObject* knob = Child(go, "Knob"))
                if (GameObject* track = Child(go, "Track")) {
                    const Vector3 p = knob->transform.position;
                    knob->transform.position = {
                        track->transform.position.x + ratio * kTrackW - kKnobHalfW, p.y, p.z };
                }
        }
        if (hovered && m_focusBar) {
            // フォーカスバーは行の左。行の枠から出すので文言に依らない。
            // 目標だけ置き、寄るのは OnUpdate (滑り込み中の行の x は目標にしない)。
            auto it = m_pageRows.find(go);
            const float rowY = it != m_pageRows.end() ? it->second.origin.y
                                                      : go->transform.position.y;
            const Vector3 p = m_focusBar->transform.position;
            m_focusBar->transform.position = { 395.0f, p.y, p.z };
            m_focusTarget = rowY + 20.0f;
            if (!m_focusPlaced) { m_focusY = m_focusTarget; m_focusPlaced = true; }
            anyHovered = true;
        }
    }
    // どの行にも乗っていなければバーは消える (乗るまで前の行に居座らない)。
    m_focusAmount = uimotion::Approach(m_focusAmount, anyHovered ? 1.0f : 0.0f, dt,
                                       anyHovered ? blendSeconds : blendSeconds * 2.0f);
}

inline void OptionsScreenComponent::RefreshNav(float dt)
{
    m_navClock += dt;
    const float response = blendSeconds <= 0.0f ? 1.0f : 1.0f - std::exp(-dt / blendSeconds);
    for (int i = 0; i < kTabs; ++i) {
        // 選択中のタブは常時点灯。ホバー中のタブも「押せる」ことを見せるため点ける。
        const bool lit = (i == m_tab)
            || (m_navLabels[i] && (ui.IsHovered(m_navLabels[i]) || ui.IsPressed(m_navLabels[i])));
        const float target = lit ? 1.0f : 0.0f;
        m_navAmount[i] += (target - m_navAmount[i]) * response;
        if (std::abs(target - m_navAmount[i]) < 0.001f) m_navAmount[i] = target;

        if (m_navBars[i]) ui.SetMaterialFloat(m_navBars[i], "selected", m_navAmount[i]);
        if (!m_navLabels[i]) continue;
        const float t = m_navAmount[i];
        // 点いたタブは文字が芯から押し出される (TitleMenu の行と同じ語彙)。
        m_navLabels[i]->transform.position = {
            m_navOrigin[i].x + navNudge * uimotion::OutCubic(t), m_navOrigin[i].y, m_navOrigin[i].z,
        };
        // 色は文字列側 (UiTextFx)。選択中のタブだけ 2.4 秒に 1 本、光が字面を舐める。
        const Vector4 c = textfx::Mix(navDimColor, navActiveColor, t);
        std::string rich;
        if (i == m_tab) {
            const float head = -0.45f + 1.9f * std::fmod(m_navClock, 2.4f) / 2.4f;
            rich = textfx::Sweep(m_navText[i], c, { 1.0f, 1.0f, 1.0f, 1.0f }, head, 0.38f);
        } else {
            rich = textfx::Wrap(m_navText[i], c);
        }
        if (rich != m_navRich[i]) { ui.SetText(m_navLabels[i], rich); m_navRich[i] = std::move(rich); }
        ui.SetTextColor(m_navLabels[i], { 1.0f, 1.0f, 1.0f, c.w });
    }
}

// CONTROLS 右列を「アイコン + 補足文字」で右端 1788 に揃える。
//
// WHY 実行時に並べるか: アイコンの左へ置く文字の幅は書体と字送りで決まり、
//     シーンを作る時点では測れない。UIText の実測 (transform.scale.x に写る) を使う。
// WHY 一度だけか: 文言は固定。実測が出るまで再試行して 1 回で確定させる。
inline void OptionsScreenComponent::LayoutControlIcons()
{
    static constexpr float kRight = 1788.0f;   // CONTROLS 罫線の右端
    // 描画する四角の一辺。素材は 64px セルの中央 75% にしか絵が無いので、
    // 見えるキーの高さは kIconCell * kIconInk になる。
    static constexpr float kIconCell = 34.0f;
    static constexpr float kIconInk  = 0.75f;  // セルに対する絵の割合 (素材の実測)
    // 絵と絵のあいだ。四角どうしの隙間ではなく「見た目の隙間」で指定する。
    // WHY: セル側に 12.5% ずつ余白が入っているので、四角の間隔で決めると
    //      指定した倍以上あいて、WASD が 4 つのばらばらなキーに見える。
    static constexpr float kIconGap = 6.0f;

    auto* settings = GameSettingsComponent::Instance();

    bool measured = true;
    for (int deviceIndex = 0; deviceIndex < 2; ++deviceIndex) {
        const char* dev = deviceIndex == 1 ? "PAD" : "KBM";
        for (const actions::ControlRow& row : actions::kControlRows) {
            const std::string suffix = std::string(dev) + "_" + row.key;
            float cursor = kRight;

            if (GameObject* tail = scene.Find("CtrlR_" + suffix)) {
                const auto* text = tail->GetComponent<UIText>();
                if (text && !text->text.empty()) {
                    const float width = tail->transform.scale.x;
                    if (width <= 0.0f) measured = false;
                    cursor -= width + kIconGap;
                }
            }
            // 差し替えた行は絵を出さない。場所だけ空けると、右端に文字があるのに
            // その左が不自然に空いた行になる。
            const bool custom = settings && row.action && *row.action
                             && settings->IsBindingOverridden(row.action, deviceIndex);
            GameObject* icons[4] = {};
            int count = 0;
            while (!custom && count < 4) {
                GameObject* icon =
                    scene.Find("CtrlIcon_" + suffix + "_" + std::to_string(count));
                if (!icon) break;
                icons[count++] = icon;
            }
            for (int i = count - 1; i >= 0; --i) {
                // cursor は「絵の右端」。四角の左上へ直すには余白の半分だけ戻す。
                cursor -= kIconCell * kIconInk;
                const Vector3 p = icons[i]->transform.position;
                icons[i]->transform.position = {
                    cursor - kIconCell * (1.0f - kIconInk) * 0.5f, p.y, p.z };
                cursor -= kIconGap;
            }
        }
    }
    m_ctrlLaidOut = measured;
}

} // namespace sandbox
