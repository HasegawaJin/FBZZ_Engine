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
/// WHY ドラッグを自前で書かないか:
///   UISlider が掴み判定と値の写像を持っている。同じ計算をスクリプトへ写すと、
///   Canvas Scaler や入れ子の変換が絡んだ場面でだけずれる。
#pragma once

#include <Engine/Scene/Components/UIControls.hpp>
#include <Engine/Scene/Components/UIText.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Script.hpp>
#include <Scripts/Game/GameSettingsComponent.hpp>
#include <Scripts/Utils/InputActions.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>
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
    FBZZ_GROUP("Flow")
    FBZZ_FIELD(std::string, backScene, "Title", "Back Scene")
    FBZZ_TOOLTIP("Esc / B で戻る先。戻る前に設定を保存する。空なら戻らない")

    FBZZ_GROUP("Look")
    FBZZ_FIELD_COLOR(navDimColor, (Vector4{ 0.435294f, 0.427451f, 0.407843f, 1.0f }), "Nav Dim")
    FBZZ_FIELD_COLOR(navActiveColor, (Vector4{ 1.0f, 1.0f, 1.0f, 1.0f }), "Nav Active")
    FBZZ_FIELD_COLOR(rowDimColor, (Vector4{ 0.662745f, 0.650980f, 0.627451f, 1.0f }), "Row Dim")
    FBZZ_TOOLTIP("カーソルが乗っていない行のラベル色。Reference の rgb(169,166,160)")
    FBZZ_FIELD_COLOR(rowActiveColor, (Vector4{ 1.0f, 1.0f, 1.0f, 1.0f }), "Row Active")
    FBZZ_FIELD_COLOR(valueDimColor, (Vector4{ 0.870588f, 0.858824f, 0.835294f, 1.0f }), "Value Dim")
    FBZZ_FIELD_RANGE(float, blendSeconds, 0.09f, "Blend", 0.0f, 1.0f)

    FBZZ_GROUP("Debug")
    FBZZ_FIELD_READ_ONLY(std::string, debugTab, "-", "Tab")
    FBZZ_FIELD_READ_ONLY(std::string, debugRow, "-", "Hovered Row")

    void OnStart() override;
    void OnUpdate() override;

private:
    /// 値の見せ方。入力の受け方はウィジェットが持つので、ここは表示だけを決める。
    enum class Show {
        Percent,   ///< 0-1 を百分率で
        Number,    ///< 実数。decimals と suffix で整える
        Toggle,    ///< ON / OFF
        Choice,    ///< 選択肢の名前
    };

    struct Row {
        const char* page;   ///< 属するページ (Tab_ 以下の名前)
        const char* key;    ///< 行 ID。オブジェクト名は <TAB>_Row_<key>
        Show  show;
        float min = 0.0f, max = 1.0f;   ///< スライダー 0-1 と実値の対応
        const char* suffix = "";
        int   decimals = 0;
    };

    static const std::vector<Row>& Table();
    [[nodiscard]] static const std::vector<std::string>* Choices(const char* key);

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

    [[nodiscard]] float GetValue(const Row& row) const;
    void  SetValue(const Row& row, float value);
    /// 今つまんだ強さでパッドを 1 度回す。振動は数字を読んでも決められない。
    void  PreviewVibration(float scale01) const;
    [[nodiscard]] std::string FormatValue(const Row& row, float value) const;
    /// 行の子を名前で引く。子の名前 (Label / Value / Slider) は行をまたいで同じ。
    [[nodiscard]] static GameObject* Child(GameObject* parent, std::string_view name);

    static constexpr int kTabs = 4;
    static constexpr const char* kTabNames[kTabs] = { "INPUT", "GAME", "VIDEO", "AUDIO" };
    [[nodiscard]] std::string PageOfTab(int tab) const;

    GameObject* m_navBars[kTabs]   = {};
    GameObject* m_navLabels[kTabs] = {};
    GameObject* m_focusBar = nullptr;
    float m_navAmount[kTabs] = {};

    /// 表示中のページに属する行。SelectTab のたびに引き直す。
    /// WHY 毎フレーム探さないか: scene.Find はシーン全体の名前検索で、
    ///     行 6 本ぶんを毎フレーム回すと 200 オブジェクトを何度も走査することになる。
    struct RowBinding { const Row* row; GameObject* go; };
    std::vector<RowBinding> m_bound;

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
inline const std::vector<OptionsScreenComponent::Row>& OptionsScreenComponent::Table()
{
    using S = Show;
    static const std::vector<Row> table = {
        { "Tab_INPUT", "device",      S::Choice },
        { "Tab_INPUT", "mouseSens",   S::Number, 0.1f, 10.0f, "", 2 },

        { "Tab_INPUT_PAD", "device",    S::Choice },
        { "Tab_INPUT_PAD", "stickSens", S::Number, 0.1f, 10.0f, "", 2 },
        { "Tab_INPUT_PAD", "curve",     S::Choice },
        // WHY Percent ではないか: デッドゾーンは 0-0.5 の量で、スライダー全域を
        //     0-1 に写すと後半が「スティックを半分倒すまで無入力」という
        //     操作にならない領域になる。実値の範囲をそのまま持たせる。
        { "Tab_INPUT_PAD", "deadzone",  S::Number, 0.0f, 0.5f, "", 2 },
        { "Tab_INPUT_PAD", "vibration", S::Percent },

        { "Tab_GAME", "fov",      S::Number, 60.0f, 110.0f, "\xc2\xb0", 0 },
        { "Tab_GAME", "shake",    S::Percent },
        { "Tab_GAME", "hitstop",  S::Percent },
        { "Tab_GAME", "fovBurst", S::Toggle },
        { "Tab_GAME", "chain",    S::Toggle },

        { "Tab_VIDEO", "displayMode", S::Choice },
        { "Tab_VIDEO", "resolution",  S::Choice },
        { "Tab_VIDEO", "vsync",       S::Toggle },
        { "Tab_VIDEO", "fpsCap",      S::Choice },
        { "Tab_VIDEO", "brightness",  S::Number, 0.5f, 2.0f, "", 2 },
        { "Tab_VIDEO", "bloom",       S::Percent },
        { "Tab_VIDEO", "quality",     S::Choice },
        { "Tab_VIDEO", "renderScale", S::Number, 0.5f, 2.0f, "", 2 },

        { "Tab_AUDIO", "master", S::Percent },
        { "Tab_AUDIO", "sfx",    S::Percent },
        { "Tab_AUDIO", "bgm",    S::Percent },
        { "Tab_AUDIO", "ui",     S::Percent },
    };
    return table;
}

inline const std::vector<std::string>* OptionsScreenComponent::Choices(const char* key)
{
    static const std::vector<std::string> device      = { "マウス＆キーボード", "ゲームパッド" };
    static const std::vector<std::string> curve       = { "リニア", "標準", "強め" };
    static const std::vector<std::string> displayMode = { "ウィンドウ", "フルスクリーン" };
    static const std::vector<std::string> fpsCap      = { "無制限", "30", "60", "120", "144", "240" };
    static const std::vector<std::string> quality     = { "低", "中", "高", "最高" };
    const std::string k = key;
    if (k == "device")      return &device;
    if (k == "curve")       return &curve;
    if (k == "displayMode") return &displayMode;
    if (k == "fpsCap")      return &fpsCap;
    if (k == "quality")     return &quality;
    return nullptr;   // resolution は実行時のモニター依存なので別扱い
}

inline int FpsCapValue(int index)
{
    static const int values[] = { 0, 30, 60, 120, 144, 240 };
    return values[std::clamp(index, 0, 5)];
}

inline GameObject* OptionsScreenComponent::Child(GameObject* parent, std::string_view name)
{
    if (!parent) return nullptr;
    for (int i = 0, n = parent->GetChildCount(); i < n; ++i)
        if (GameObject* child = parent->GetChild(i); child && child->name == name) return child;
    return nullptr;
}

// ── 値の読み書き ────────────────────────────────────────────────────────────
inline float OptionsScreenComponent::GetValue(const Row& row) const
{
    auto* s = GameSettingsComponent::Instance();
    if (!s) return 0.0f;
    const std::string k = row.key;
    const auto& v = s->Video();  const auto& a = s->Audio();
    const auto& i = s->Input();  const auto& g = s->Game();

    if (k == "device")      return static_cast<float>(i.device);
    if (k == "mouseSens")   return i.mouseSens;
    if (k == "stickSens")   return i.stickSens;
    if (k == "curve")       return static_cast<float>(i.curve);
    if (k == "deadzone")    return i.deadzone;
    if (k == "vibration")   return i.vibration;
    if (k == "fov")         return g.fov;
    if (k == "shake")       return g.shake;
    if (k == "hitstop")     return g.hitstop;
    if (k == "fovBurst")    return g.fovBurst ? 1.0f : 0.0f;
    if (k == "chain")       return g.chain ? 1.0f : 0.0f;
    if (k == "displayMode") return v.fullscreen ? 1.0f : 0.0f;
    if (k == "resolution")  return static_cast<float>(s->ResolutionIndex());
    if (k == "vsync")       return v.vsync ? 1.0f : 0.0f;
    if (k == "fpsCap") {
        for (int n = 0; n < 6; ++n) if (FpsCapValue(n) == v.targetFps) return static_cast<float>(n);
        return 2.0f;
    }
    if (k == "brightness")  return v.brightness;
    if (k == "bloom")       return v.bloom;
    // 未選択のうちは ProjectSettings の設定がそのまま効いている。保存値ではなく
    // 実際に効いている段を出す。保存値 (-1) を出すと「低」に見えてしまう。
    if (k == "quality")     return static_cast<float>(
        v.quality >= 0 ? v.quality : static_cast<int>(graphics.GetQualityPreset()));
    if (k == "renderScale") return v.renderScale;
    if (k == "master")      return a.master;
    if (k == "sfx")         return a.se;
    if (k == "bgm")         return a.bgm;
    if (k == "ui")          return a.ui;
    return 0.0f;
}

inline void OptionsScreenComponent::SetValue(const Row& row, float value)
{
    auto* s = GameSettingsComponent::Instance();
    if (!s) return;
    const std::string k = row.key;
    auto& v = s->MutableVideo();  auto& a = s->MutableAudio();
    auto& i = s->MutableInput();  auto& g = s->MutableGame();
    const int n = static_cast<int>(std::lround(value));

    if      (k == "device")      { i.device = n; ApplyDeviceGroups(); SelectTab(m_tab); }
    else if (k == "mouseSens")   i.mouseSens = value;
    else if (k == "stickSens")   i.stickSens = value;
    else if (k == "curve")       i.curve = n;
    else if (k == "deadzone")    i.deadzone = value;
    else if (k == "vibration")   { i.vibration = value; PreviewVibration(value); }
    else if (k == "fov")         g.fov = value;
    else if (k == "shake")       g.shake = value;
    else if (k == "hitstop")     g.hitstop = value;
    else if (k == "fovBurst")    g.fovBurst = n != 0;
    else if (k == "chain")       g.chain = n != 0;
    else if (k == "displayMode") v.fullscreen = n != 0;
    else if (k == "resolution")  { s->SetResolutionIndex(n); return; }   // 内部で Apply する
    else if (k == "vsync")       v.vsync = n != 0;
    else if (k == "fpsCap")      v.targetFps = FpsCapValue(n);
    else if (k == "brightness")  v.brightness = value;
    else if (k == "bloom")       v.bloom = value;
    // 画質は「プレイヤーが選んだ」という事実まで記録する必要がある。
    // 素の代入だと、ProjectSettings の描画設定を上書きしてよいかが判らない。
    else if (k == "quality")     { s->SetQualityPreset(n); return; }
    else if (k == "renderScale") v.renderScale = value;
    else if (k == "master")      a.master = value;
    else if (k == "sfx")         a.se = value;
    else if (k == "bgm")         a.bgm = value;
    else if (k == "ui")          a.ui = value;
    s->Apply();
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

inline std::string OptionsScreenComponent::FormatValue(const Row& row, float value) const
{
    char buffer[64] = {};
    switch (row.show) {
    case Show::Percent:
        std::snprintf(buffer, sizeof(buffer), "%d%%", static_cast<int>(std::lround(value * 100.0f)));
        return buffer;
    case Show::Number:
        std::snprintf(buffer, sizeof(buffer), "%.*f%s", row.decimals, value, row.suffix);
        return buffer;
    case Show::Toggle:
        return value >= 0.5f ? "ON" : "OFF";
    case Show::Choice: {
        const int index = static_cast<int>(std::lround(value));
        if (const std::string k = row.key; k == "resolution") {
            auto* s = GameSettingsComponent::Instance();
            if (!s || s->Resolutions().empty()) return "-";
            const auto& r = s->Resolutions()[std::clamp<size_t>(index, 0, s->Resolutions().size() - 1)];
            std::snprintf(buffer, sizeof(buffer), "%u \xc3\x97 %u", r.width, r.height);
            return buffer;
        }
        const auto* list = Choices(row.key);
        if (!list || list->empty()) return "-";
        return (*list)[std::clamp<size_t>(index, 0, list->size() - 1)];
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
    for (int i = 0; i < kTabs; ++i) {
        m_navBars[i]   = scene.Find(std::string("Nav") + kTabNames[i] + "Bar");
        m_navLabels[i] = scene.Find(std::string("Nav") + kTabNames[i] + "Label");
        if (!m_navBars[i] || !m_navLabels[i])
            debug.LogWarning(std::string("OptionsScreen: Nav") + kTabNames[i] + " が見つかりません");
        m_navAmount[i] = (i == m_tab) ? 1.0f : 0.0f;
    }
    m_focusBar = scene.Find("FocusBar");

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
    if (s->Input().device == 0 && input.IsPadConnected() && !config.Has("input"))
        s->MutableInput().device = 1;

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
    for (const char* name : { "Tab_INPUT", "Tab_INPUT_PAD", "Tab_GAME", "Tab_VIDEO", "Tab_AUDIO" })
        if (auto* go = scene.Find(name)) go->SetActive(page == name);

    const std::string tag = page.substr(4);   // "Tab_VIDEO" -> "VIDEO"
    m_bound.clear();
    for (const Row& row : Table()) {
        if (page != row.page) continue;
        if (GameObject* go = scene.Find(tag + "_Row_" + row.key))
            m_bound.push_back({ &row, go });
    }
    debugTab = kTabNames[m_tab];
}

inline void OptionsScreenComponent::OnUpdate()
{
    const float dt = (std::max)(time.UnscaledDeltaTime(), 0.0f);

    if (!m_settingsBound) BindSettings();

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
    const bool cancel = input.GetKeyDown(fbzz::input::KeyCode::ESCAPE)
                     || (input.IsPadConnected()
                         && input.GetPadButtonDown(fbzz::input::GamepadButton::B));
    if (cancel && !m_lastCancel && !backScene.empty()) {
        if (auto* s = GameSettingsComponent::Instance()) s->Save();
        scene.LoadScene(backScene);
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
    // WHY 早期に抜けるか: GetValue が 0 を返すので、そのまま同期すると
    //     「掴んでも離すと 0 へ戻る」形でスライダーが潰れる。値が無いことと
    //     値が 0 であることは別で、無いなら UI をオーサリング値のまま残す。
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
    for (const auto& [rowPtr, go] : m_bound) {
        const Row& row = *rowPtr;
        const bool hovered = ui.IsHovered(go) || ui.IsPressed(go);
        if (hovered) debugRow = row.key;

        float value = GetValue(row);

        GameObject* sliderGO = Child(go, "Slider");
        auto* slider = sliderGO ? sliderGO->GetComponent<UISlider>() : nullptr;
        if (slider) {
            if (slider->onValueChanged) {
                // ドラッグの結果はウィジェットが持っている。0-1 を実値へ写すだけ。
                value = (row.show == Show::Percent)
                    ? slider->value
                    : row.min + slider->value * (row.max - row.min);
                SetValue(row, value);
            } else if (!slider->runtimeDragging) {
                // 外から変わった値 (プリセット適用・初期化) をつまみへ戻す。
                const float ratio = (row.show == Show::Percent)
                    ? value
                    : (value - row.min) / (std::max)(row.max - row.min, 1e-4f);
                slider->value = std::clamp(ratio, 0.0f, 1.0f);
            }
        } else if (ui.WasClicked(go)) {
            // スライダーの無い行は、押すたびに次の値へ送る。
            if (row.show == Show::Toggle) {
                value = value >= 0.5f ? 0.0f : 1.0f;
            } else if (row.show == Show::Choice) {
                int count = 2;
                if (const std::string k = row.key; k == "resolution") {
                    auto* s = GameSettingsComponent::Instance();
                    count = s ? static_cast<int>(s->Resolutions().size()) : 1;
                } else if (const auto* list = Choices(row.key)) {
                    count = static_cast<int>(list->size());
                }
                if (count > 0)
                    value = static_cast<float>((static_cast<int>(std::lround(value)) + 1) % count);
            }
            SetValue(row, value);
            value = GetValue(row);
        }

        // ── 見た目 ────────────────────────────────────────────────────────
        if (GameObject* label = Child(go, "Label"))
            ui.SetTextColor(label, hovered ? rowActiveColor : rowDimColor);
        if (GameObject* valueText = Child(go, "Value")) {
            ui.SetText(valueText, FormatValue(row, value));
            ui.SetTextColor(valueText, hovered ? rowActiveColor : valueDimColor);
        }
        if (slider) {
            // 素材は左右に 32px の余白を持つ。芯の ratio 割は (pad + ratio*芯幅) / PNG幅。
            constexpr float kTrackW = 230.0f, kPngW = 295.0f, kPad = 32.0f, kKnobHalfW = 1.5f;
            const float ratio = std::clamp(slider->value, 0.0f, 1.0f);
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
            const Vector3 p = m_focusBar->transform.position;
            m_focusBar->transform.position = { 395.0f, go->transform.position.y + 20.0f, p.z };
            ui.SetMaterialFloat(m_focusBar, "selected", 1.0f);
        }
    }
}

inline void OptionsScreenComponent::RefreshNav(float dt)
{
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
        ui.SetTextColor(m_navLabels[i], {
            navDimColor.x + (navActiveColor.x - navDimColor.x) * t,
            navDimColor.y + (navActiveColor.y - navDimColor.y) * t,
            navDimColor.z + (navActiveColor.z - navDimColor.z) * t,
            navDimColor.w + (navActiveColor.w - navDimColor.w) * t,
        });
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
