/// @file    PauseMenuComponent.hpp
/// @brief   ゲーム中のポーズ画面。時間を止め、OPTIONS を同じシーンの中で開く
/// @author  Hasegawa Jin
/// @date    2026-09-15
///
/// 画面の作り (Stage_01〜03 に同じ構成を置く):
///   Pause_Canvas (UICanvas)              このスクリプトの置き場。常に有効
///     Pause_Root (UIImage)               ポーズ中だけ有効になる枠。画面いっぱいの暗幕でもある
///       ├ Pause_Mark / Pause_Rule / Pause_Title / Pause_Hint   見出し (2 つの頁で共通)
///       ├ Pause_Menu                     RESUME / OPTIONS / EXIT の頁
///       │   └ PauseRow_<NAME> (UIButton) 行全体の当たり判定
///       │       ├ Band  (UIImage + UIMenuBand.mat)
///       │       ├ Bar   (UIImage + UIMenuItem.mat)
///       │       └ Label (UIText)
///       ├ Pause_Options                  OPTIONS の頁。押している間だけ有効
///       │   └ [[OptionsScreenComponent]] と Options 画面と同じ名前の行
///       └ Cursor (UIImage + [[GameCursorComponent]])
///
/// WHY 暗幕を枠そのものにするか:
///   ストレッチ (UIRect の stretchX / stretchY) は «親の矩形» に対して効くので、
///   ただの入れ物の子に置くと親の矩形が 1x1 になり、画面の隅に 1px の点が出る。
///   枠自身を画面いっぱいの矩形にすれば、暗幕が 1 枚減るうえ、中の行のアンカーも
///   Canvas 直下に置いたときと同じに解ける。
///
/// WHY OPTIONS をシーン遷移にしないか:
///   遊んでいる最中に音量を直すのに盤面を降ろすと、戻ってきたときには敵の位置も
///   体力も作り直しになる。Options 画面の «行の名前» は scene.Find で引かれるだけ
///   なので、同じ名前の行をこの枠の中に置けば、あちらのスクリプトをそのまま
///   内側で動かせる (BGM と扉だけ standalone = false で黙らせる)。
///
/// WHY 時間を止めるのに Time::timeScale を直接書かないか:
///   書き手が 2 人になった瞬間に壊れる ([[TimeManagerComponent]] のヘッダー)。
///   ポーズは «解除するまで維持する» 層として向こうが既に持っているので、
///   ここは要求を出すだけにする。ヒットストップ (Override) の最中に開いても、
///   止めの解除がポーズを巻き込まない。
///
/// WHY 入力を «止まっているから大丈夫» としないか:
///   timeScale が 0 でも OnUpdate は回り続ける。斬撃の入力は押した瞬間に段が進む
///   ので、ポーズ中に押したぶんが «開けた瞬間に出る» 形で溜まる。プレイヤーには
///   毎フレーム拘束を言い直す ([[PlayerComponent]]::RequestSuspend)。
///
/// WHY カーソルを別オブジェクトに持たせるか:
///   視点操作は Locked (相対移動) で、メニューは Confined (絶対座標) と、
///   カーソルの扱いが正反対になる。GameCursorComponent は有効な間だけ要求を積む
///   ので、枠ごと有効・無効を切り替えれば、閉じた瞬間に視点操作の Locked へ戻る。
#pragma once

#include <Engine/Scene/Components/UIText.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Script.hpp>
#include <Scripts/Game/GameSettingsComponent.hpp>
#include <Scripts/Game/TimeManagerComponent.hpp>
#include <Scripts/Player/PlayerComponent.hpp>
#include <Scripts/UI/UiMotion.hpp>
#include <Scripts/UI/UiNavSe.hpp>
#include <Scripts/UI/UiTextFx.hpp>
#include <Scripts/Utils/InputActions.hpp>
#include <Scripts/Utils/SceneTransition.hpp>
#include <Scripts/Utils/SeLibrary.hpp>
#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>

using namespace fbzz::scene;
using namespace fbzz::math;

namespace sandbox {

class PauseMenuComponent : public Script {
    FBZZ_SCRIPT(PauseMenuComponent)

public:
    FBZZ_GROUP("流れ")
    FBZZ_FIELD(std::string, exitScene, "Title", "EXIT の行き先")
    FBZZ_TOOLTIP("EXIT で戻るシーン。空なら EXIT を押しても何も起きない")

    FBZZ_GROUP("見た目")
    FBZZ_FIELD_COLOR(labelDimColor, (Vector4{ 0.462745f, 0.454902f, 0.439216f, 1.0f }), "Label Dim")
    FBZZ_TOOLTIP("カーソルが乗っていない行の文字色。タイトルのメニューと同じ rgb(118,116,112)")
    FBZZ_FIELD_COLOR(labelActiveColor, (Vector4{ 1.0f, 1.0f, 1.0f, 1.0f }), "Label Active")
    FBZZ_FIELD_RANGE(float, dimAlpha, 0.78f, "暗幕", 0.0f, 1.0f)
    FBZZ_TOOLTIP("盤面を覆う黒の濃さ。薄いと文字が読めず、濃いと «別の画面» に見える")
    FBZZ_FIELD_RANGE(float, blendSeconds, 0.09f, "ブレンド", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, releaseSeconds, 0.18f, "解放", 0.0f, 1.0f)
    FBZZ_TOOLTIP("カーソルが離れたあと消えるまでの速さ。寄りより遅くする")
    FBZZ_FIELD(float, hoverNudge, 10.0f, "押し出し")
    FBZZ_TOOLTIP("ホバー中に文字を右へ押し出す量 [px]")

    FBZZ_GROUP("開閉")
    FBZZ_FIELD_RANGE(float, openSeconds, 0.16f, "暗幕の速さ", 0.02f, 1.0f)
    FBZZ_FIELD_RANGE(float, rowStagger, 0.05f, "のけぞり", 0.0f, 0.5f)
    FBZZ_FIELD_RANGE(float, rowSeconds, 0.22f, "行の出現", 0.05f, 1.0f)
    FBZZ_FIELD(float, rowSlide, -40.0f, "滑り")
    FBZZ_TOOLTIP("出る前の位置のずれ [px]。負で左から滑り込む")

    FBZZ_GROUP("デバッグ")
    FBZZ_FIELD_READ_ONLY(std::string, debugState, "Closed", "状態")
    FBZZ_FIELD_READ_ONLY(std::string, debugHovered, "-", "ホバー中")

    /// 今ポーズが開いているか。演出を止めたい側 (カメラ・HUD) が読む。
    ///
    /// WHY TimeManager の IsPaused を見せないか: あちらは «時間が止まっているか» で、
    ///     ポーズ以外の理由で止まる余地がある。«メニューが開いている» を知りたい側は
    ///     こちらを読む。
    [[nodiscard]] static bool IsOpen() { return s_open; }

    void OnStart() override;
    void OnUpdate() override;
    void OnDestroy() override;

private:
    enum class Row { Resume, Options, Exit, Count };
    static constexpr int kRowCount = static_cast<int>(Row::Count);
    static constexpr const char* kRowNames[kRowCount] = { "RESUME", "OPTIONS", "EXIT" };

    /// 閉じるときに巻き戻す速さ (開くときに対する倍率)。
    ///
    /// WHY 開閉で速さを変えるか: 開くのは «見せる» ので溜めがあってよいが、閉じるのは
    ///     «遊びへ戻る» で、待たされた分だけ操作が遅れて感じる。
    static constexpr float kCloseRate = 1.8f;

    void Open();
    /// 閉じ始める。実際に枠が消えるのは巻き戻しが 0 に着いたとき。
    void BeginClose();
    void SetOptionsPage(bool open);
    void Submit(Row row);
    /// 行 1 本ぶんの見た目を、出現の進行度 × ホバー量から書く。
    void Paint(int index, float reveal);
    [[nodiscard]] float RowReveal(int index) const;
    /// 盤面の時間を止める / 返す。
    void ApplyHold(bool held);
    [[nodiscard]] PlayerComponent* Player() const;
    /// 行の子を名前で引く。行ごとに Bar / Label という同じ名前を使っているので、
    /// シーン全体から引く scene.Find では、どの行のものか決まらない。
    [[nodiscard]] static GameObject* Child(GameObject* parent, std::string_view name);

    static inline bool s_open = false;

    GameObject* m_root    = nullptr;   ///< 枠であり暗幕
    GameObject* m_menu    = nullptr;
    GameObject* m_options = nullptr;
    GameObject* m_title   = nullptr;
    GameObject* m_rows[kRowCount]   = {};
    GameObject* m_bands[kRowCount]  = {};
    GameObject* m_bars[kRowCount]   = {};
    GameObject* m_labels[kRowCount] = {};
    Vector3 m_rowOrigin[kRowCount]   = {};
    Vector3 m_labelOrigin[kRowCount] = {};
    std::string m_text[kRowCount];   ///< ラベルの素の文言 (シーンが正本)
    std::string m_rich[kRowCount];   ///< 最後に流し込んだ文字列。同じなら書かない
    float m_amount[kRowCount]   = {};   ///< ホバー量 0..1
    float m_flash[kRowCount]    = {};   ///< 決定の光。1 から 0 へ減衰
    float m_litSince[kRowCount] = {};   ///< 点いてからの秒数 (走査の位相)
    bool  m_lit[kRowCount]      = {};

    /// 開いてからの秒数。閉じるときは同じ値を巻き戻す。
    float m_clock = 0.0f;
    /// 帯の斜線の位相。開いている間だけ進む。
    float m_phase = 0.0f;
    bool  m_opening   = false;   ///< 開いている (閉じ始めたら false)
    bool  m_optionsOn = false;
    /// EXIT を押した後。扉が閉じ切るまで何も受けない。
    bool  m_leaving = false;
    /// 時間の止め手が居ないことを 1 度だけ言うためのラッチ。
    bool  m_warnedNoTime = false;
};

FBZZ_REFLECT(PauseMenuComponent)

inline GameObject* PauseMenuComponent::Child(GameObject* parent, std::string_view name)
{
    if (!parent) return nullptr;
    for (int i = 0, n = parent->GetChildCount(); i < n; ++i)
        if (GameObject* child = parent->GetChild(i); child && child->name == name) return child;
    return nullptr;
}

inline PlayerComponent* PauseMenuComponent::Player() const
{
    GameObject* player = scene.FindWithTag("Player");
    return player ? scene.GetScript<PlayerComponent>(player) : nullptr;
}

inline void PauseMenuComponent::OnStart()
{
    se::EnsureSource(scene, "UI");

    m_root    = scene.Find("Pause_Root");
    m_menu    = scene.Find("Pause_Menu");
    m_options = scene.Find("Pause_Options");
    m_title   = scene.Find("Pause_Title");
    if (!m_root || !m_menu) {
        debug.LogError("PauseMenuComponent: Pause_Root / Pause_Menu が見つかりません。"
                       "ポーズは開きません");
        m_root = nullptr;
        return;
    }

    for (int i = 0; i < kRowCount; ++i) {
        m_rows[i]   = Child(m_menu, std::string("PauseRow_") + kRowNames[i]);
        m_bands[i]  = Child(m_rows[i], "Band");
        m_bars[i]   = Child(m_rows[i], "Bar");
        m_labels[i] = Child(m_rows[i], "Label");
        if (!m_rows[i] || !m_labels[i]) {
            debug.LogWarning(std::string("PauseMenuComponent: PauseRow_") + kRowNames[i]
                             + " (と子の Label) が見つかりません");
            continue;
        }
        m_rowOrigin[i]   = m_rows[i]->transform.position;
        m_labelOrigin[i] = m_labels[i]->transform.position;
        // 色付きの文字列を流すので richText を立てる。素の文言は控えておく。
        if (auto* text = m_labels[i]->GetComponent<UIText>()) {
            text->richText = true;
            m_text[i]      = text->text;
        }
    }

    // 枠は閉じた状態で始める。シーンに有効なまま置かれていても、ここで必ず畳む。
    SetOptionsPage(false);
    m_root->SetActive(false);
    s_open = false;
}

inline void PauseMenuComponent::OnDestroy()
{
    // 開いたままシーンが降りることがある (EXIT・被弾死の直後)。止めたまま次の盤面へ
    // 持ち込むと、新しいシーンが «最初から動かない» という形で壊れる。
    if (s_open) ApplyHold(false);
    s_open = false;
}

inline float PauseMenuComponent::RowReveal(int index) const
{
    return uimotion::Stagger(m_clock, index, rowStagger, rowSeconds);
}

inline void PauseMenuComponent::ApplyHold(bool held)
{
    if (auto* timeManager = TimeManagerComponent::Instance()) {
        timeManager->SetPaused(held);
    } else if (held && !m_warnedNoTime) {
        debug.LogError("PauseMenuComponent: TimeManagerComponent がシーンに居ません。"
                       "メニューは開きますが盤面は動き続けます");
        m_warnedNoTime = true;
    }
    // WHY 拘束の «解除» をここでしないか: RequestSuspend は押し続けている間だけ効く
    //     1 フレームぶんの要求で、false を渡すのは «入力は止めずに拘束する» の意味に
    //     なる。返すときは言うのをやめるだけでよい。
}

inline void PauseMenuComponent::Open()
{
    if (!m_root) return;
    m_root->SetActive(true);
    m_opening = true;
    m_leaving = false;
    m_clock   = 0.0f;
    s_open    = true;
    SetOptionsPage(false);
    // 暗幕は 0 から。シーンに置いた色のまま 1 フレーム出ると、開いた瞬間に
    // «黒が点滅した» ように見える。
    ui.SetImageColor(m_root, { 0.0f, 0.0f, 0.0f, 0.0f });
    for (int i = 0; i < kRowCount; ++i) {
        m_amount[i]   = 0.0f;
        m_flash[i]    = 0.0f;
        m_lit[i]      = false;
        m_litSince[i] = 0.0f;
        m_rich[i].clear();
        // 1 フレーム目から «出ていない» で描く。OnUpdate を待つと、置いた位置で
        // 1 フレームだけ見えてから引っ込む。
        Paint(i, 0.0f);
    }
    ApplyHold(true);
    audio.PlayOneShot(uinav::kConfirm);
}

inline void PauseMenuComponent::BeginClose()
{
    if (!m_opening) return;
    m_opening = false;
    SetOptionsPage(false);
    audio.PlayOneShot(uinav::kCancel);
}

inline void PauseMenuComponent::SetOptionsPage(bool open)
{
    m_optionsOn = open && m_options != nullptr;
    if (m_options) m_options->SetActive(m_optionsOn);
    // WHY 行を残さず畳むか: OPTIONS の一覧は行と同じ帯の上へ出る。薄く残すと
    //     «押せない行» と «押せる行» が重なって、どちらを触っているのか読めない。
    if (m_menu) m_menu->SetActive(!m_optionsOn);
    // 見出しは 2 つの頁で共通。文言だけが «今どちらに居るか» を言う。
    if (m_title) ui.SetText(m_title, m_optionsOn ? "OPTIONS" : "PAUSED");
}

inline void PauseMenuComponent::Submit(Row row)
{
    switch (row) {
    case Row::Resume:
        BeginClose();
        return;
    case Row::Options:
        audio.PlayOneShot(uinav::kConfirm);
        SetOptionsPage(true);
        return;
    case Row::Exit:
        break;
    default:
        return;
    }

    if (exitScene.empty()) {
        debug.Log("PauseMenuComponent: EXIT の行き先が空です");
        return;
    }
    // 設定は Option を閉じた時点でも保存するが、そこを通らずに EXIT へ抜ける経路が
    // ある (行を触らずにタブだけ変えた、など)。降りる前にもう一度書く。
    if (auto* settings = GameSettingsComponent::Instance()) settings->Save();
    audio.PlayOneShot(uinav::kConfirm);
    if (!transition::Begin(exitScene)) {
        debug.LogError("PauseMenuComponent: シーンへの扉を開けません -> " + exitScene);
        return;
    }
    // 止めたまま扉を閉じる。盤面が動き出すのは次のシーンの TimeManager が
    // 立ち上がったとき ─ 扉の裏で 0.5 秒ぶん戦闘が進むのを防ぐ。
    m_leaving = true;
}

inline void PauseMenuComponent::Paint(int index, float reveal)
{
    const float e     = uimotion::OutCubic(reveal);
    const float alpha = uimotion::OutQuint(reveal * 1.25f);
    const float t     = m_amount[index];

    if (m_rows[index]) {
        m_rows[index]->transform.position = {
            m_rowOrigin[index].x + rowSlide * (1.0f - e),
            m_rowOrigin[index].y, m_rowOrigin[index].z,
        };
    }
    if (m_bars[index]) {
        ui.SetImageColor(m_bars[index], { 1.0f, 1.0f, 1.0f, alpha });
        ui.SetMaterialFloat(m_bars[index], "selected", t);
    }
    if (m_bands[index]) {
        ui.SetImageColor(m_bands[index], { 1.0f, 1.0f, 1.0f, alpha });
        ui.SetMaterialFloat(m_bands[index], "selected", t);
        ui.SetMaterialFloat(m_bands[index], "flash", m_flash[index]);
        ui.SetMaterialFloat(m_bands[index], "phase", m_phase);
    }
    if (!m_labels[index]) return;

    // 寄りは OutCubic で «押された» 形に。t は指数で寄るので、そのままだと最後が鈍い。
    m_labels[index]->transform.position = {
        m_labelOrigin[index].x + hoverNudge * uimotion::OutCubic(t),
        m_labelOrigin[index].y, m_labelOrigin[index].z,
    };
    const Vector4 c = textfx::Mix(labelDimColor, labelActiveColor, t);
    // 文字の色は文字列側 (UiTextFx) が持ち、UIText.color は α だけにする。
    std::string rich;
    if (t > 0.01f) {
        // 点いている間は 1.6 秒に 1 本、光が左から右へ字面を舐める (タイトルと同じ語彙)。
        const float head = -0.45f + 1.9f * std::fmod(m_litSince[index], 1.6f) / 1.6f;
        rich = textfx::Sweep(m_text[index], c, { 1.0f, 1.0f, 1.0f, 1.0f }, head, 0.38f);
    } else {
        rich = textfx::Wrap(m_text[index], c);
    }
    if (rich != m_rich[index]) {
        ui.SetText(m_labels[index], rich);
        m_rich[index] = std::move(rich);
    }
    ui.SetTextColor(m_labels[index], { 1.0f, 1.0f, 1.0f, c.w * alpha });
}

inline void PauseMenuComponent::OnUpdate()
{
    if (!m_root) return;

    // 実時間で進める。盤面を止めている当人がゲーム時間で数えると、開いた瞬間に
    // 自分の演出まで止まる。
    const float dt = (std::max)(time.UnscaledDeltaTime(), 0.0f);

    // 開く。扉 (ワイプ) が動いている間は受けない ─ リザルトへ落ちる途中で開くと、
    // 止めたまま次のシーンへ渡ることになる。
    const bool pressed = input.GetActionDown(actions::kPause);
    if (!s_open) {
        const bool opened = pressed && !transition::Active();
        if (opened) Open();
        debugState = opened ? "Menu" : "Closed";
        return;
    }

    // 開いている間は «拘束している» を言い続ける。1 フレームでも途切れると、
    // その隙に入った入力が開けた瞬間に出る。閉じ切るまで (巻き戻しの最中も) 言う。
    if (auto* player = Player()) player->RequestSuspend(true);

    if (m_leaving) {
        // 扉の裏。見た目だけ書き続ける (行が固まって見えないように)。
        m_phase += dt;
        for (int i = 0; i < kRowCount; ++i) Paint(i, RowReveal(i));
        debugState = "Leaving";
        return;
    }

    // 閉じる操作は割り当て変更やアクション層の無効化で失わせない。
    const bool cancel = input.GetKeyDown(fbzz::input::KeyCode::ESCAPE);
    bool returnedFromOptions = false;
    if (pressed || cancel) {
        // OPTIONS を開いている間の Pause は «一段戻る»。ポーズごと閉じると、
        // 音量を直しに来ただけで盤面へ放り出される。
        if (m_optionsOn) {
            if (auto* settings = GameSettingsComponent::Instance()) settings->Save();
            SetOptionsPage(false);
            audio.PlayOneShot(uinav::kCancel);
            returnedFromOptions = true;
        } else if (!m_opening) {
            // 閉じかけで押し直されたら開き直す。時間はまだ止めたままなので、
            // 巻き戻しの向きを変えるだけでよい。
            m_opening = true;
            audio.PlayOneShot(uinav::kConfirm);
        } else {
            BeginClose();
        }
    }

    m_clock = std::clamp(m_clock + (m_opening ? dt : -dt * kCloseRate), 0.0f,
                         rowStagger * static_cast<float>(kRowCount - 1) + rowSeconds);
    if (!m_opening && m_clock <= 0.0f) {
        // 巻き戻し切った。ここで初めて盤面へ時間を返す ─ 行が残っているうちに
        // 返すと、メニューの裏で 0.1 秒ぶん殴られる。
        m_root->SetActive(false);
        ApplyHold(false);
        s_open     = false;
        debugState = "Closed";
        return;
    }

    m_phase += dt;
    const float cover =
        uimotion::OutCubic(uimotion::Clamp01(m_clock / (std::max)(openSeconds, 0.01f)));
    ui.SetImageColor(m_root, { 0.0f, 0.0f, 0.0f, dimAlpha * cover });

    const float attack  = blendSeconds   <= 0.0f ? 0.0f : blendSeconds;
    const float release = releaseSeconds <= 0.0f ? attack : releaseSeconds;
    debugState   = m_optionsOn ? "Options" : (m_opening ? "Menu" : "Closing");
    debugHovered = "-";

    Row submitted = Row::Count;
    for (int i = 0; i < kRowCount; ++i) {
        const float reveal = RowReveal(i);
        // 置き切る前の行と、OPTIONS を出している間の行はカーソルを受けない。
        // 非表示中の onClick は描画パスで更新されない。再表示したフレームで
        // 読むと、OPTIONS を開いたクリックを再実行して戻れなくなる。
        const bool ready = reveal >= 1.0f && m_opening && !m_optionsOn && !returnedFromOptions;
        const bool lit = ready && m_rows[i] && (ui.IsHovered(m_rows[i]) || ui.IsPressed(m_rows[i]));
        if (lit) debugHovered = kRowNames[i];
        if (lit && !m_lit[i]) {
            audio.PlayOneShot(uinav::kMove);
            m_flash[i] = (std::max)(m_flash[i], 0.28f);
        }
        m_lit[i]      = lit;
        m_litSince[i] = lit ? m_litSince[i] + dt : 0.0f;
        m_amount[i]   = uimotion::Approach(m_amount[i], lit ? 1.0f : 0.0f, dt, lit ? attack : release);
        uimotion::Decay(m_flash[i], dt, 0.16f);
        if (ready && ui.WasClicked(m_rows[i])) {
            m_flash[i] = 1.0f;
            submitted  = static_cast<Row>(i);
        }
        Paint(i, reveal);
    }
    // 光った状態を今フレームに出してから畳む (押した行が消えながら光る)。
    if (submitted != Row::Count) Submit(submitted);
}

} // namespace sandbox
