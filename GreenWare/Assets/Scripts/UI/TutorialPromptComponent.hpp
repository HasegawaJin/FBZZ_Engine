/// @file    TutorialPromptComponent.hpp
/// @brief   チュートリアルが «今どのボタンか» を出す。アイコン 1 つと動詞 1 語
/// @author  Hasegawa Jin
/// @date    2026-09-14
///
/// WHY 目的の 1 行 (HUD_Objective) と分けるか:
///   目的は «何をするか» だけを言い、どのキーかは言わない ─ 割り当ては OPTIONS で
///   差し替えられるので、文言に焼くと嘘になる (GameFlowComponent の RefreshHud)。
///   だが教える段では «どのボタンか» こそが要る。文言はそのまま残し、ボタンは
///   アイコンで別に出す。差し替えても勝手に付いてくるのがアイコンの利点。
///
/// WHY 画面の中央下へ置くか:
///   右下は操作案内 (UiHintBar) の席で、左下は体力。中央下は «今まさに押すもの» が
///   目線の落ちる所に来る唯一の空きで、盤面 (ボスの脚元) からも近い。
///
/// WHY 押した瞬間に膨らませるか:
///   出しっぱなしの案内は 2 秒で «飾り» になる。押したときにアイコンだけが一度膨らむと、
///   案内と自分の指が繋がる。UiHintBar が同じことをしているので、語彙も揃う。
///
/// シーン側に要るもの: Canvas の下に `HUD_TutorialIcon` (UIImage) と
///   `HUD_TutorialText` (UIText)。位置と大きさはここが毎フレーム決めるので、
///   シーンの値は仮でよい。
///
/// WHY 実行時に作らないか: `scene.Create` は GameObject 配列を再確保する。同じ画面の
///   他のスクリプトが行の GameObject* を掴んで持ち続けているので、あちらの指し先が
///   消える (UiHintBarComponent と同じ理由)。置き場所はシーンに持たせる。
#pragma once

#include <Engine/Scene/Components/UIElement.hpp>
#include <Engine/Scene/EntityRef.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Script.hpp>
#include <Scripts/Game/GameSettingsComponent.hpp>
#include <Scripts/Game/TutorialDirectorComponent.hpp>
#include <Scripts/UI/UiMotion.hpp>
#include <Scripts/UI/UiTextFx.hpp>
#include <Scripts/Utils/InputActions.hpp>
#include <Scripts/Utils/KeyIcons.hpp>
#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>

using namespace fbzz::scene;
using namespace fbzz::math;

namespace sandbox {

class TutorialPromptComponent : public Script {
    FBZZ_SCRIPT(TutorialPromptComponent)

public:
    FBZZ_GROUP("置き場所")
    FBZZ_FIELD(std::string, iconName, "HUD_TutorialIcon", "Icon")
    FBZZ_FIELD(std::string, textName, "HUD_TutorialText", "Text")
    FBZZ_FIELD(std::string, portraitName, "HUD_TutorialPortrait", "案内役")
    // 文字の後ろへ敷く帯。**画面が沈んでいない間も読めるようにするための物**で、
    // 沈み (cueDim) とは役割が別 ── あちらは «世界を止めた» の合図、こちらは可読性。
    FBZZ_FIELD(std::string, bandName, "HUD_TutorialBand", "Band")
    FBZZ_TOOLTIP("空なら帯を出さない。シーンに無くても黙って諦める (案内自体は出る)")

    FBZZ_GROUP("配置")
    FBZZ_FIELD_RANGE(float, centerY, 800.0f, "縦位置 [px]", 0.0f, 1080.0f)
    FBZZ_TOOLTIP("1080 基準。体力バーとボスバーの間を外した高さに置く")
    FBZZ_FIELD_RANGE(float, iconSize, 70.0f, "アイコン", 16.0f, 200.0f)
    FBZZ_FIELD_RANGE(float, fontSize, 44.0f, "文字", 8.0f, 96.0f)
    FBZZ_FIELD_RANGE(float, gapIconText, 18.0f, "アイコンと文字の間", 0.0f, 60.0f)

    // «ここだ» の合図が出ている間だけ、画面の真ん中へ寄って大きくなる。
    //
    // WHY 普段から中央に置かないか: 中央は盤面そのものが居る場所で、常駐させると
    //   ボスの足元 (予兆が出る所) を文字で塞ぐ。普段は目線の下、**言い切る瞬間だけ
    //   目線の上へ** ─ 位置が変わること自体が «今のは特別» の合図になる。
    FBZZ_GROUP("教える瞬間")
    FBZZ_FIELD_RANGE(float, cueCenterY, 486.0f, "中央での縦位置 [px]", 0.0f, 1080.0f)
    FBZZ_TOOLTIP("1080 基準。画面の中心 (540) より少し上 ─ 真ん中はボスの胴が居る")
    FBZZ_FIELD_RANGE(float, cueScale, 1.55f, "大きさの倍率", 1.0f, 3.0f)
    // 合図の «寄り» は速く、«戻り» はゆっくり。«来た» は一瞬で、«読む» には時間が要る。
    //
    // WHY 片道ずつ速さを分けるか: 1 本の曲線で往復させると、寄るのを速くすれば
    //   戻るのも速くなる。戻りが速いと、読み終わる前に中央から逃げていく。
    FBZZ_FIELD_RANGE(float, cueRiseSeconds, 0.10f, "寄る速さ [s]", 0.01f, 1.0f)
    FBZZ_FIELD_RANGE(float, cueFallSeconds, 0.34f, "戻る速さ [s]", 0.01f, 2.0f)

    FBZZ_GROUP("見た目")
    FBZZ_FIELD_COLOR(textColor, (Vector4{ 0.92f, 0.95f, 1.0f, 1.0f }), "文字")
    FBZZ_FIELD_COLOR(bandColor, (Vector4{ 0.02f, 0.02f, 0.03f, 0.52f }), "帯")
    FBZZ_FIELD_RANGE(float, bandPadX, 46.0f, "帯の横の余白 [px]", 0.0f, 200.0f)
    FBZZ_FIELD_RANGE(float, bandPadY, 20.0f, "帯の縦の余白 [px]", 0.0f, 120.0f)

    // 文言が変わってから «読めた頃» に光を 1 度だけ通す。
    //
    // WHY 解読 (Decode) にしないか: ここは 0.8 秒で読ませるのが仕事で、しかもスローを
    //   掛けてまで «今» を言っている。確定するまで待たせる演出は読みやすさを削るだけ。
    //   走査なら **最初のフレームから全部読める**まま、格だけ上げられる。
    //
    // WHY 遅らせるか: 出た瞬間に光らせると «光った» が先に目に入って、文字を読む前に
    //   1 拍失う。読み終わる頃に舐めるから «特別な一言» として残る。
    FBZZ_GROUP("走査")
    FBZZ_FIELD_RANGE(float, sweepDelay, 0.30f, "通すまで [s]", 0.0f, 2.0f)
    FBZZ_TOOLTIP("文言が変わってからここまで待って光を通す。0 で出た瞬間")
    FBZZ_FIELD_RANGE(float, sweepSeconds, 0.40f, "通る速さ [s]", 0.05f, 2.0f)
    FBZZ_FIELD_RANGE(float, sweepWidth, 0.38f, "光の幅", 0.05f, 1.0f)
    FBZZ_FIELD_COLOR(sweepColor, (Vector4{ 1.0f, 0.98f, 0.88f, 1.0f }), "光")
    FBZZ_FIELD_RANGE(float, fadeSeconds, 0.25f, "出入り [s]", 0.05f, 1.5f)
    FBZZ_TOOLTIP("押させたい操作が変わったときの入れ替わりの速さ")
    FBZZ_FIELD_RANGE(float, idlePulse, 0.10f, "呼吸", 0.0f, 0.5f)
    FBZZ_TOOLTIP("押されていない間の膨らみ。0 で静止。«まだ押していない» を静かに促す")
    FBZZ_FIELD_RANGE(float, idlePulseHz, 1.1f, "呼吸の速さ [Hz]", 0.1f, 4.0f)

    FBZZ_GROUP("デバッグ")
    FBZZ_FIELD_READ_ONLY(std::string, debugAction, "-", "操作")
    FBZZ_FIELD_READ_ONLY(bool, debugPad, false, "パッド")

    void OnStart() override;
    void OnUpdate() override;

private:
    /// 論理名からアイコンの «役» を引く。UiHintBar の ParseAction と対になる表。
    [[nodiscard]] static bool IconOf(std::string_view action, keyicon::Action& out);

    /// 文字列の幅を «文字の高さ何個ぶんか» で見積もる。
    ///
    /// WHY バイト数で数えないか: 文言は日本語で、UTF-8 では 1 文字 3 バイト。
    ///     `size()` で数えると幅を 3 倍に見積もり、アイコンが画面の左へ大きく外れる。
    ///     実測 (transform.scale.x) が届くまでの 1 フレームだけの話だが、
    ///     押した瞬間に 1 フレーム飛ぶのは «押したのに変な所へ出た» として目に付く。
    ///
    /// WHY 全角を 1.0 とするか: 和文は字面がほぼ正方形で、字送りは文字の高さに等しい。
    ///     半角 (英数・記号) はその半分あたり。
    [[nodiscard]] static float GlyphWidths(const std::string& text)
    {
        float widths = 0.0f;
        for (const char raw : text) {
            const auto byte = static_cast<unsigned char>(raw);
            if ((byte & 0xC0u) == 0x80u) continue;      // 継続バイトは数えない
            widths += (byte < 0x80u) ? 0.55f : 1.0f;    // 半角 / 全角
        }
        return widths;
    }
    [[nodiscard]] bool UsingPad() const;
    void Bind();
    void Apply(float reveal, float grow);
    void Hide();

    EntityRef   m_icon;
    EntityRef   m_text;
    EntityRef   m_band;
    EntityRef   m_portrait;
    std::string m_action;   ///< 今出している操作の論理名
    std::string m_label;
    bool  m_pad    = false;
    float m_cue    = 0.0f;  ///< 合図の強さ [0,1]。Director から毎フレーム貰う
    float m_reveal = 0.0f;  ///< 出ている度合い [0,1]
    float m_hit    = 0.0f;  ///< 押した瞬間 1、減衰
    /// 文言が変わってからの経過 [実時間 秒]。走査の位置をここから引く。
    float m_labelAge = 0.0f;
    /// 今フレーム、リッチテキスト (走査) を置いているか。素へ戻すのを 1 度で済ませる札。
    bool  m_sweeping = false;
    float m_time   = 0.0f;
};

FBZZ_REFLECT(TutorialPromptComponent)

inline bool TutorialPromptComponent::IconOf(std::string_view action, keyicon::Action& out)
{
    using keyicon::Action;
    if (action == actions::kParry)  { out = Action::Parry;  return true; }
    if (action == actions::kDodge)  { out = Action::Dodge;  return true; }
    if (action == actions::kAttack) { out = Action::Attack; return true; }
    if (action == actions::kJump)   { out = Action::Jump;   return true; }
    return false;
}

inline bool TutorialPromptComponent::UsingPad() const
{
    if (auto* s = GameSettingsComponent::Instance()) return s->Input().device == 1;
    return input.IsPadConnected();
}

inline void TutorialPromptComponent::Bind()
{
    if (GameObject* portrait = scene.Find(portraitName)) m_portrait = EntityRef{ portrait->GetID() };
    if (GameObject* icon = scene.Find(iconName)) m_icon = EntityRef{ icon->GetID() };
    if (GameObject* text = scene.Find(textName)) m_text = EntityRef{ text->GetID() };
    // 帯は無くても案内は成立する。見つからないことを咎めない。
    if (!bandName.empty())
        if (GameObject* band = scene.Find(bandName)) m_band = EntityRef{ band->GetID() };
    if (!m_icon.Resolve(scene) || !m_text.Resolve(scene))
        debug.LogWarning("TutorialPrompt: '" + iconName + "' / '" + textName +
                         "' がシーンにありません (押すボタンの案内は出ません)");
}

inline void TutorialPromptComponent::OnStart()
{
    Bind();
    m_pad    = UsingPad();
    m_reveal = 0.0f;
    m_hit    = 0.0f;
    m_time   = 0.0f;
    m_action.clear();
    m_label.clear();
    Hide();
}

inline void TutorialPromptComponent::Hide()
{
    if (GameObject* portrait = m_portrait.Resolve(scene)) ui.SetImageColor(portrait, { 1.0f, 1.0f, 1.0f, 0.0f });
    if (GameObject* icon = m_icon.Resolve(scene)) ui.SetImageColor(icon, { 1.0f, 1.0f, 1.0f, 0.0f });
    if (GameObject* text = m_text.Resolve(scene)) ui.SetTextColor(text, { 0.0f, 0.0f, 0.0f, 0.0f });
    if (GameObject* band = m_band.Resolve(scene)) ui.SetImageColor(band, { 0.0f, 0.0f, 0.0f, 0.0f });
}

inline void TutorialPromptComponent::Apply(float reveal, float grow)
{
    GameObject* icon = m_icon.Resolve(scene);
    GameObject* text = m_text.Resolve(scene);
    if (!icon || !text) return;

    const float alpha = uimotion::OutQuint(reveal);

    // 合図の間は中央へ寄って大きくなる。寄り切るのは速く、戻るのはゆっくり ──
    // «来た» は一瞬で、«読む» には時間が要る。
    const float cue   = uimotion::OutCubic(m_cue);
    const auto* lesson = TutorialDirectorComponent::Instance();
    const bool focus = lesson && lesson->StaminaFocusActive();
    const float baseY = focus ? 360.0f : centerY + (cueCenterY - centerY) * cue;
    const float zoom  = 1.0f + (std::max(cueScale, 1.0f) - 1.0f) * cue;

    const float iconW = iconSize * zoom;
    const float font  = std::min(fontSize * zoom, 1100.0f / std::max(GlyphWidths(m_label), 1.0f));
    const float gap   = gapIconText * zoom;
    const float size  = iconW * grow;

    // 実測の幅で «アイコン + 間 + 文字» を中央へ寄せる。まだ測れていないフレームは
    // 字数から見積もる (1 フレームだけずれるが、次で直る ── UiHintBar と同じ)。
    //
    // WHY 実測をそのまま使えないか: 実測は «前フレームの font で描いた幅»。合図で
    //     文字が大きくなる最中は毎フレーム変わるので、そのまま使うと中央から
    //     ずれ続ける。倍率が動いている間は見積もりへ寄せる。
    const float textW = font * GlyphWidths(m_label);
    const float portraitW = m_portrait.Resolve(scene) ? 150.0f : 0.0f;
    const float portraitSpace = portraitW > 0.0f ? portraitW + gap : 0.0f;
    const float total = portraitSpace + iconW + gap + textW;
    const float left  = 960.0f - total * 0.5f;
    // 出る前は少し下から上がってくる。
    const float lift  = 14.0f * (1.0f - uimotion::OutCubic(reveal));

    if (GameObject* portrait = m_portrait.Resolve(scene)) {
        const float bounce = std::sin(std::min(m_labelAge / 0.35f, 1.0f) * 3.14159265f) * 10.0f;
        portrait->transform.position = { left, baseY - portraitW * 0.5f + lift - bounce, 0.0f };
        portrait->transform.scale = { portraitW, portraitW, 1.0f };
        ui.SetImageColor(portrait, { 1.0f, 1.0f, 1.0f, alpha });
    }
    icon->transform.position = { left + portraitSpace - (size - iconW) * 0.5f,
                                 baseY - size * 0.5f + lift, 0.0f };
    icon->transform.scale    = { size, size, 1.0f };
    ui.SetImageColor(icon, { 1.0f, 1.0f, 1.0f, alpha });

    if (auto* t = text->GetComponent<UIText>()) t->fontSize = font;
    text->transform.position = { left + portraitSpace + iconW + gap, baseY - font * 0.5f + lift, 0.0f };

    // 走査。読める状態は 1 フレーム目から保ったまま、光だけが端から端へ抜ける。
    //
    // 頂点色は UIText.color に掛かるので、走査の間は color を白 + α にして、
    // 色は文字列の側で組む (UiTextFx.hpp の «絶対色で書く» の約束)。
    const float head = (m_labelAge - sweepDelay) / std::max(sweepSeconds, 0.05f);
    const auto* director = TutorialDirectorComponent::Instance();
    const bool celebrating = director && director->IsCelebrating();
    const bool  sweep = !celebrating && !m_label.empty() && head > -sweepWidth && head < 1.0f + sweepWidth;
    if (sweep) {
        ui.SetTextColor(text, { 1.0f, 1.0f, 1.0f, textColor.w * alpha });
        ui.SetText(text, textfx::Sweep(m_label, textColor, sweepColor, head, sweepWidth));
        m_sweeping = true;
    } else {
        // 抜け切ったら素の文字へ 1 度だけ戻す。毎フレーム組み直す理由が無い。
        if (m_sweeping) { ui.SetText(text, m_label); m_sweeping = false; }
        ui.SetTextColor(text, celebrating ? Vector4{ 1.0f, 0.87f, 0.40f, alpha }
                                         : Vector4{ textColor.x, textColor.y, textColor.z, textColor.w * alpha });
    }

    // 帯は «アイコン + 文字» をそのまま囲う。文字の長さで幅が変わるので、
    // 固定の板ではなく毎フレーム測って敷く ─ 短い «回避» に帯だけ長く残ると、
    // 中身の無い板が画面の真ん中を占める。
    if (GameObject* band = m_band.Resolve(scene)) {
        const float padX = bandPadX * zoom;
        const float padY = bandPadY * zoom;
        const float h    = std::max(iconW, font) + padY * 2.0f;
        band->transform.position = { left - padX, baseY - h * 0.5f + lift, 0.0f };
        band->transform.scale    = { total + padX * 2.0f, h, 1.0f };
        ui.SetImageColor(band, { bandColor.x, bandColor.y, bandColor.z,
                                 bandColor.w * alpha });
    }
}

inline void TutorialPromptComponent::OnUpdate()
{
    const float dt = std::max(time.UnscaledDeltaTime(), 0.0f);
    m_time += dt;

    const auto* director = TutorialDirectorComponent::Instance();
    const char* want     = director ? director->PromptAction() : "";
    const char* label    = director ? director->PromptLabel()  : "";
    // 合図は «その瞬間の強さ» をそのまま使わず、寄りと戻りで別々の速さで追う。
    // Director 側は «残り / 全体» をそのまま返すので、中央へ寄るのが合図の頭で
    // 一番強く、以後ゆっくり下がる ── 追従を分けないと寄りも戻りも同じ速さになる。
    {
        const float want = director ? director->CueStrength() : 0.0f;
        const float span = want > m_cue ? std::max(cueRiseSeconds, 0.01f)
                                        : std::max(cueFallSeconds, 0.01f);
        const float step = dt / span;
        m_cue = want > m_cue ? std::min(want, m_cue + step) : std::max(want, m_cue - step);
    }

    // 機器が変わったらアイコンを差し替える (OPTIONS で切り替えた直後にも効く)。
    const bool pad = UsingPad();
    const bool deviceChanged = pad != m_pad;
    m_pad = pad;
    debugPad = pad;

    // 押させたい操作が変わったら、文字とアイコンを差し替えて出し直す。
    //
    // WHY 一度引っ込めてから出さないか: 段が進んだ «良いこと» の直後に案内が
    //     消えて出るのは、間違えて消えたようにも見える。中身だけ入れ替えて、
    //     出入りは «有るか無いか» のときだけにする。
    // WHY 文言の変化も見るか: 合図の最中はボタンが同じまま **文字だけ** «今だ、横へ
    //     逃げろ» へ差し替わる。操作名だけを見ていると、そこで文字が更新されない。
    const bool labelChanged  = m_label != (label ? label : "");
    const bool actionChanged = m_action != (want ? want : "");

    if (labelChanged) {
        m_label = label ? label : "";
        m_labelAge = 0.0f;
        m_sweeping = false;
        if (GameObject* text = m_text.Resolve(scene)) ui.SetText(text, m_label);
    }
    m_labelAge += dt;
    if (actionChanged || deviceChanged) {
        m_action = want ? want : "";
        debugAction = m_action.empty() ? "-" : m_action;
        keyicon::Action which = keyicon::Action::Confirm;
        if (GameObject* icon = m_icon.Resolve(scene))
            if (IconOf(m_action, which)) {
                const std::string_view sprite = keyicon::Prompt(m_pad, which);
                if (!sprite.empty()) ui.SetImageTexture(icon, sprite);
            }
        // 叩きの残りは «操作が変わったとき» だけ捨てる。文言が言い換わるたびに
        // 捨てると、合図の最中に押しても膨らまない。
        if (actionChanged) m_hit = 0.0f;
    }

    // 出したい操作が無ければ引っ込める。アイコンを引けない操作も出さない ─
    // 絵の無い枠だけが浮くと «何か出そこなった» に見える。
    keyicon::Action resolved = keyicon::Action::Confirm;
    const bool wantShow = !m_action.empty() && IconOf(m_action, resolved);

    const float target = wantShow ? 1.0f : 0.0f;
    const float step   = dt / std::max(fadeSeconds, 0.01f);
    m_reveal = m_reveal < target ? std::min(target, m_reveal + step)
                                 : std::max(target, m_reveal - step);
    if (m_reveal <= 0.0f) { Hide(); return; }

    // 押した瞬間だけ膨らむ。押されていない間は静かに呼吸する。
    if (wantShow && input.GetActionDown(m_action)) m_hit = 1.0f;
    uimotion::Decay(m_hit, dt, 0.14f);
    const float breathe = 1.0f + idlePulse * 0.5f
                        * (1.0f + std::sin(m_time * idlePulseHz * 6.2831853f));
    const float grow = (breathe - idlePulse * 0.5f) + 0.30f * uimotion::OutBack(m_hit);

    Apply(m_reveal, grow);
}

} // namespace sandbox
