/// @file    ControlsGuideComponent.hpp
/// @brief   OPTIONS 右の CONTROLS を «操作説明» にする。行の出現・ホバー・解説欄・機器表示
/// @author  Hasegawa Jin
/// @date    2026-09-07
///
/// 画面の作り (Options.scene の既存要素 + ここが実行時に足すもの):
///   Ctrl_Rule / CtrlTitle                   見出し (UiReveal が出す)
///   `CtrlDiv_<key>` (UIButton) / `CtrlL_<key>`  行の当たりと左の文言 … ここが出現とホバーを持つ
///   CtrlIcon_* / CtrlR_*                    右のアイコンと補足 (OptionsScreen が中身を持つ)
///   CtrlGuide_Device                        見出しの右端に «今の機器» (richText)
///   CtrlGuide_Box / _Tag / _Body            一覧の下の解説欄。乗った行の説明を解読風に出す
///                                           (Box は UIPanel.mat、Body は richText)
///
/// @note 解説欄を実行時に作らない理由: scene.Create は GameObject 配列を再確保する。
///       同じ GameObject に居る OptionsScreenComponent は行の GameObject* を掴んで
///       持つので、こちらが後から作るとあちらの指し先が消える。
/// @note 解説欄を足す理由: 一覧は «どのキーか» しか言わない。乗った行の意味を 2 行で
///       言う欄があれば、OPTIONS が操作説明の場所を兼ねる。
/// @note OptionsScreenComponent と分ける理由: あちらは «設定の値の読み書き» が仕事。
///       触る要素も分けてある ─ あちらは CtrlR_* / CtrlIcon_* の中身、こちらは
///       CtrlDiv_* / CtrlL_* の位置と色、右側の α だけ。
/// @note 説明を解読風に出す理由: 文が «パッと» 入れ替わると読み始める前に前の文が
///       消えた感じが残る。左から確定していけば «今来た» と目で追える。
#pragma once

#include <Engine/Scene/Components/UIElement.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Script.hpp>
#include <Scripts/Game/GameSettingsComponent.hpp>
#include <Scripts/UI/UiMotion.hpp>
#include <Scripts/UI/UiTextFx.hpp>
#include <Scripts/Utils/InputActions.hpp>
#include <Scripts/Title/ElectrodePole.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

using namespace fbzz::scene;
using namespace fbzz::math;

namespace sandbox {

class ControlsGuideComponent : public Script {
    FBZZ_SCRIPT(ControlsGuideComponent)

public:
    FBZZ_GROUP("見た目")
    FBZZ_FIELD_COLOR(labelDimColor, (Vector4{ 0.662745f, 0.650980f, 0.627451f, 1.0f }), "Label Dim")
    FBZZ_FIELD_COLOR(labelActiveColor, (Vector4{ 1.0f, 1.0f, 1.0f, 1.0f }), "Label Active")
    FBZZ_FIELD_COLOR(bodyColor, (Vector4{ 0.870588f, 0.858824f, 0.835294f, 1.0f }), "Body")
    FBZZ_FIELD_COLOR(tagColor, (Vector4{ 0.556863f, 0.545098f, 0.521569f, 1.0f }), "Tag")
    FBZZ_FIELD(float, nudge, 6.0f, "押し出し")
    FBZZ_TOOLTIP("乗った行の文言を右へ押し出す量 [px]")

    FBZZ_GROUP("Guide Box")
    FBZZ_FIELD(float, boxX, 1268.0f, "X")
    /// 一覧の最後の行 (y = 730) の下。行を増減したら 1 行ぶん (44px) 動かす。
    FBZZ_FIELD(float, boxY, 806.0f, "Y")
    FBZZ_FIELD(float, boxW, 520.0f, "W")
    FBZZ_FIELD(float, boxH, 132.0f, "H")
    FBZZ_FIELD_RANGE(float, decodeSeconds, 0.32f, "Decode", 0.05f, 2.0f)

    FBZZ_GROUP("導入")
    FBZZ_FIELD_RANGE(float, introDelay, 0.45f, "遅延", 0.0f, 3.0f)
    FBZZ_FIELD_RANGE(float, introStagger, 0.05f, "のけぞり", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, introSeconds, 0.40f, "継続時間", 0.05f, 3.0f)
    FBZZ_FIELD(float, introSlide, 28.0f, "滑り")
    FBZZ_TOOLTIP("行が出る前の位置のずれ [px]。正で右から滑り込む")

    FBZZ_GROUP("デバッグ")
    FBZZ_FIELD_READ_ONLY(std::string, debugRow, "-", "ホバー中")

    void OnStart() override;
    void OnUpdate() override;

private:
    /// 1 行ぶんの説明。key は InputActions の ControlRow と同じ。
    struct Doc { const char* key; const char* title; const char* body; };
    static constexpr Doc kDocs[] = {
        { "move",   "移動",       "アリーナを走る。振っている間だけ足が鈍る。\n間合いの外へ抜けるのは回避、詰めるのは走り" },
        { "cam",    "カメラ",     "視点を回す。ボスは全高 6m あるので、寄せすぎると\n脚の跳ね上がりや首のしなり ─ 予兆が画面の外に出る" },
        { "blade",  "斬る",       "押すたびに 5 段まで繋がる (刀は段ごとに左右へ交互)。\n斬っても倒れない ─ 溜まるのは崩しゲージだけ" },
        { "parry",  "弾き / とどめ", "重機の踏みつけを刀で受ける。崩しゲージが一気に伸びる。\nボスが倒れている間は、同じボタンが «とどめ» になる" },
        { "dodge",  "回避",       "短く踏み込んで攻撃をかわす。かわしている間は無敵。\n当たる直前に合わせると時間が伸びる (ジャスト回避)" },
        { "jump",   "ジャンプ",   "地を這ってくる手 ─ コアの衝撃波の輪、蛇の突き上げ ─ は\n弾けない。跳んで越える" },
        { "climb",  "登る",       "ボスの脚のそばで押すと飛びつき、脚を伝って背へ登る。\n歩いていても掴まれる。もう一度押すと手を離す。\n背の蓋が開いているあいだ、露出したコアを斬れる" },
        { "pause",  "ポーズ",     "時間を止めて設定を開く。\n戦闘中の割り当て変更もここから" },
    };
    static constexpr const char* kIdleBody =
        "行に乗ると操作の説明が出る。行を押すと、次に押した入力へ\n割り当てを変えられる (移動とカメラは変えられない)";
    static constexpr const char* kRebindBody =
        "次に押した入力へ割り当てる…\n取り消すなら Esc か B";

    struct Row {
        std::string key;
        GameObject* hit   = nullptr;   ///< `CtrlDiv_<key>`
        GameObject* label = nullptr;   ///< `CtrlL_<key>`
        Vector3 hitOrigin   = {};
        Vector3 labelOrigin = {};
        Vector4 hitColor    = { 1.0f, 1.0f, 1.0f, 1.0f };
        float   amount = 0.0f;         ///< ホバー量
    };
    std::vector<Row> m_rows;
    /// 右側 (CtrlR_* / CtrlIcon_*)。α だけ出現に合わせる。
    std::vector<uimotion::Slot> m_right;

    GameObject* m_box    = nullptr;
    GameObject* m_tag    = nullptr;
    GameObject* m_body   = nullptr;
    GameObject* m_device = nullptr;
    float m_intro   = 0.0f;
    float m_decode  = 1.0e3f;       ///< 解読を始めてからの秒数
    std::uint32_t m_tick = 0;
    std::string m_bodyText;         ///< 今出している (解読前の) 文
    std::string m_lastRich;         ///< 最後に SetText した文字列 (同じなら書かない)
    int   m_hovered = -1;
    float m_rebindUntil = -1.0f;    ///< 割り当て待ちの案内を出しておく時刻
    bool  m_pad = false;

    void Bind();
    void SetBody(std::string_view text);
    void PaintRows(float dt);
    void PaintGuide(float dt);
    void RefreshDevice();
    [[nodiscard]] bool UsingPad() const;
    static void CaptureSlot(std::vector<uimotion::Slot>& into, GameObject* go);
};

FBZZ_REFLECT(ControlsGuideComponent)

inline bool ControlsGuideComponent::UsingPad() const
{
    if (auto* s = GameSettingsComponent::Instance()) return s->Input().device == 1;
    return input.IsPadConnected();
}

inline void ControlsGuideComponent::CaptureSlot(std::vector<uimotion::Slot>& into, GameObject* go)
{
    /// @note 無い要素も空の枠として積む。«1 行 = 10 枠» の並びで行番号を割り出すため。
    uimotion::Slot s;
    if (!go) { into.push_back(s); return; }
    s.go     = go;
    s.origin = go->transform.position;
    s.image  = go->GetComponent<UIImage>() != nullptr;
    s.text   = go->GetComponent<UIText>() != nullptr;
    if (auto* img = go->GetComponent<UIImage>())     s.color = img->color;
    else if (auto* txt = go->GetComponent<UIText>()) s.color = txt->color;
    into.push_back(s);
}

inline void ControlsGuideComponent::OnStart()
{
    m_rows.clear();
    m_right.clear();
    for (const actions::ControlRow& cr : actions::kControlRows) {
        Row r;
        r.key   = cr.key;
        r.hit   = scene.Find(std::string("CtrlDiv_") + cr.key, true);
        r.label = scene.Find(std::string("CtrlL_") + cr.key, true);
        if (!r.hit || !r.label) {
            debug.LogWarning(std::string("ControlsGuide: CtrlDiv_/CtrlL_") + cr.key + " が見つかりません");
            continue;
        }
        r.hitOrigin   = r.hit->transform.position;
        r.labelOrigin = r.label->transform.position;
        if (auto* img = r.hit->GetComponent<UIImage>()) r.hitColor = img->color;
        m_rows.push_back(r);

        /// @note 右側は機器ごとに 2 組ある。両方控えて、出ている方だけ描かれる。
        for (const char* group : { "KBM", "PAD" }) {
            CaptureSlot(m_right, scene.Find(std::string("CtrlR_") + group + "_" + cr.key, true));
            for (int i = 0; i < 4; ++i)
                CaptureSlot(m_right, scene.Find(std::string("CtrlIcon_") + group + "_" + cr.key
                                                + "_" + std::to_string(i), true));
        }
    }
    Bind();
    m_pad = UsingPad();
    RefreshDevice();
    m_intro = 0.0f;
    SetBody(kIdleBody);
    PaintRows(0.0f);
    PaintGuide(0.0f);
}

inline void ControlsGuideComponent::Bind()
{
    m_box    = scene.Find("CtrlGuide_Box", true);
    m_tag    = scene.Find("CtrlGuide_Tag", true);
    m_body   = scene.Find("CtrlGuide_Body", true);
    m_device = scene.Find("CtrlGuide_Device", true);
    if (!m_box || !m_tag || !m_body || !m_device)
        debug.LogWarning("ControlsGuide: CtrlGuide_Box / _Tag / _Body / _Device のどれかがシーンにありません");

    /// @note 色付きの文字列を流し込むので、richText はスクリプト側でも立てておく
    ///       (シーンで外されていても動く)。
    if (m_body)   if (auto* t = m_body->GetComponent<UIText>())   t->richText = true;
    if (m_device) if (auto* t = m_device->GetComponent<UIText>()) t->richText = true;

    if (m_box) {
        m_box->transform.position = { boxX, boxY, 0.0f };
        m_box->transform.scale    = { boxW, boxH, 1.0f };
        /// @note 解説欄は «枠のある紙» ではなく «一覧の続きの薄い面»。枠は 1px、角は小さく。
        ui.SetMaterialColor(m_box, "fillColor",   { 0.055f, 0.060f, 0.072f, 0.80f });
        ui.SetMaterialColor(m_box, "borderColor", { 0.25f, 0.26f, 0.29f, 0.9f });
        ui.SetMaterialVector4(m_box, "cornerRadius", { 3.0f, 3.0f, 3.0f, 3.0f });
        ui.SetMaterialFloat(m_box, "borderWidth", 1.0f);
        ui.SetMaterialFloat(m_box, "gradientStrength", 0.18f);
        ui.SetMaterialFloat(m_box, "shadowAlpha", 0.0f);
    }
}

inline void ControlsGuideComponent::RefreshDevice()
{
    GameObject* go = m_device;
    if (!go) return;
    /// @note 極の色で機器を言う。パッドは −、キーボードは ＋ … ではなく、どちらも
    ///       «今つながっている» という 1 つの事実なので、白系 + 薄い極色の走査だけ。
    const std::string label = m_pad ? "GAMEPAD" : "KEYBOARD & MOUSE";
    ui.SetText(go, textfx::TwoTone(label, PoleColor(Pole::Plus),
                                    { 0.62f, 0.61f, 0.58f, 1.0f },
                                    PoleColor(Pole::Minus), 0.45f));
}

inline void ControlsGuideComponent::SetBody(std::string_view text)
{
    if (m_bodyText == text) return;
    m_bodyText = std::string(text);
    m_decode   = 0.0f;
}

inline void ControlsGuideComponent::PaintRows(float dt)
{
    for (std::size_t i = 0; i < m_rows.size(); ++i) {
        Row& r = m_rows[i];
        const float t     = uimotion::Stagger(m_intro - introDelay, static_cast<int>(i), introStagger, introSeconds);
        const float e     = uimotion::OutCubic(t);
        const float alpha = uimotion::OutQuint(t * 1.25f);
        const float slide = introSlide * (1.0f - e);

        r.hit->transform.position = { r.hitOrigin.x + slide, r.hitOrigin.y, r.hitOrigin.z };
        ui.SetImageColor(r.hit, { r.hitColor.x, r.hitColor.y, r.hitColor.z, r.hitColor.w * alpha });

        const bool lit = (static_cast<int>(i) == m_hovered);
        r.amount = uimotion::Approach(r.amount, lit ? 1.0f : 0.0f, dt, lit ? 0.08f : 0.16f);
        const float a = r.amount;
        r.label->transform.position = { r.labelOrigin.x + slide + nudge * uimotion::OutCubic(a),
                                        r.labelOrigin.y, r.labelOrigin.z };
        ui.SetTextColor(r.label, {
            labelDimColor.x + (labelActiveColor.x - labelDimColor.x) * a,
            labelDimColor.y + (labelActiveColor.y - labelDimColor.y) * a,
            labelDimColor.z + (labelActiveColor.z - labelDimColor.z) * a,
            (labelDimColor.w + (labelActiveColor.w - labelDimColor.w) * a) * alpha,
        });
    }
    /// @note 右側の α。行の番号は名前から引かず、順に並んでいる前提で 1 行あたり
    ///       (1 + 4) × 2 組。出現は行と同じ拍にしたいので行の index を割り出す。
    const std::size_t perRow = 10;
    for (std::size_t i = 0; i < m_right.size(); ++i) {
        uimotion::Slot& s = m_right[i];
        if (!s.go) continue;
        const int row = static_cast<int>(i / perRow);
        const float t     = uimotion::Stagger(m_intro - introDelay, row, introStagger, introSeconds);
        const float alpha = uimotion::OutQuint(t * 1.25f);
        const Vector4 c = { s.color.x, s.color.y, s.color.z, s.color.w * alpha };
        if (s.image)     ui.SetImageColor(s.go, c);
        else if (s.text) ui.SetTextColor(s.go, c);
    }
}

inline void ControlsGuideComponent::PaintGuide(float dt)
{
    /// @note 欄そのものは一覧の最後の行の後に出る。
    const int   last  = static_cast<int>(m_rows.size());
    const float t     = uimotion::Stagger(m_intro - introDelay, last, introStagger, introSeconds);
    const float e     = uimotion::OutCubic(t);
    const float alpha = uimotion::OutQuint(t * 1.25f);
    if (GameObject* go = m_box) {
        go->transform.position = { boxX + introSlide * (1.0f - e), boxY, 0.0f };
        ui.SetImageColor(go, { 1.0f, 1.0f, 1.0f, alpha });
    }
    if (GameObject* go = m_tag) {
        go->transform.position = { boxX + 18.0f + introSlide * (1.0f - e), boxY + 14.0f, 0.0f };
        ui.SetTextColor(go, { tagColor.x, tagColor.y, tagColor.z, tagColor.w * alpha });
    }
    if (GameObject* go = m_device)
        ui.SetTextColor(go, { 1.0f, 1.0f, 1.0f, alpha });

    /// @note 本文の解読。tick は 1/30 秒ごとに進める (毎フレームだと速すぎて «ノイズ» に見える)。
    m_decode += dt;
    m_tick = static_cast<std::uint32_t>(m_decode * 30.0f);
    const float p = std::clamp(m_decode / (std::max)(decodeSeconds, 0.05f), 0.0f, 1.0f);
    const Vector4 hot = { 1.0f, 1.0f, 1.0f, 1.0f };
    const Vector4 scramble = { 0.48f, 0.50f, 0.56f, 0.9f };
    std::string rich = textfx::Decode(m_bodyText, p, 0x9E3779B9u ^ static_cast<std::uint32_t>(m_bodyText.size()),
                                      m_tick, bodyColor, hot, scramble);
    if (GameObject* go = m_body) {
        go->transform.position = { boxX + 18.0f + introSlide * (1.0f - e), boxY + 40.0f, 0.0f };
        ui.SetTextColor(go, { 1.0f, 1.0f, 1.0f, alpha });
        if (rich != m_lastRich) { ui.SetText(go, rich); m_lastRich = std::move(rich); }
    }
}

inline void ControlsGuideComponent::OnUpdate()
{
    const float dt = (std::max)(time.UnscaledDeltaTime(), 0.0f);
    m_intro += dt;

    const bool pad = UsingPad();
    if (pad != m_pad) { m_pad = pad; RefreshDevice(); }

    /// @note どの行に乗っているか。行を押したら «割り当て待ち» の案内へ (OptionsScreen が
    ///       実際の差し替えを持つ。ここは案内の文だけ)。
    m_hovered = -1;
    debugRow  = "-";
    for (std::size_t i = 0; i < m_rows.size(); ++i) {
        Row& r = m_rows[i];
        if (ui.IsHovered(r.hit) || ui.IsPressed(r.hit)) { m_hovered = static_cast<int>(i); debugRow = r.key; }
        if (ui.WasClicked(r.hit)) {
            /// @note 軸 (移動・カメラ) は差し替えられない (InputActions の ControlRow.action が空)。
            bool rebindable = false;
            for (const actions::ControlRow& cr : actions::kControlRows)
                if (r.key == cr.key && cr.action && *cr.action) rebindable = true;
            if (rebindable) m_rebindUntil = m_intro + 4.0f;
        }
    }
    if (input.GetActionDown(actions::kCancel)) m_rebindUntil = -1.0f;

    if (m_intro < m_rebindUntil) {
        SetBody(kRebindBody);
    } else if (m_hovered >= 0) {
        const std::string& key = m_rows[static_cast<std::size_t>(m_hovered)].key;
        for (const Doc& d : kDocs)
            if (key == d.key) { SetBody(d.body); break; }
    } else {
        SetBody(kIdleBody);
    }

    PaintRows(dt);
    PaintGuide(dt);
}

} // namespace sandbox
