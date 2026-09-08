/// @file    UiHintBarComponent.hpp
/// @brief   画面下の操作案内。«Ⓐ 決定  Ⓑ 戻る» をアイコン + 文字で並べる
/// @author  Hasegawa Jin
/// @date    2026-09-07
///
/// 使い方:
///   Entries に "Confirm:決定" のように «操作:文言» を並べる。操作は ParseAction が
///   受ける名前 (Confirm / Cancel / Navigate / Attack / Parry / Dodge / Jump / Pause)。
///   アイコンは接続機器でキーボードとパッドを切り替える。並びは右端から左へ (右揃え)。
///
/// シーン側に要るもの: Canvas の下に HintIcon_0..2 (UIImage) と HintText_0..2 (UIText)。
///   位置と大きさはここが毎フレーム決めるので、シーンの値は仮でよい。
///
/// WHY 実行時に作らないか (scene.Create を使わないか):
///   scene.Create は GameObject 配列を再確保する。同じ画面の他のスクリプト
///   (TitleMenu / OptionsScreen / StageSelect) は行の GameObject* を OnStart で掴んで
///   持ち続けるので、こちらの OnStart が後に走ると、あちらの指し先が消える。
///   置き場所はシーンに持たせ、ここは «見つけて動かす» だけにする。
///
/// WHY 見た目の規則をシーンに持たせないか:
///   案内は 4 画面すべてに同じ形で要る。間隔や大きさをシーンに書くと、2px 直す
///   たびに 4 シーンを開く。並べ方はここ 1 か所、文言だけがシーンごとに違う。
///
/// WHY 右下に置くか:
///   左下は法則の一文 (Title) やランク (Result) が使う。右下は 4 画面とも空いていて、
///   目線が «内容を読み終えて次に何を押すか» へ移る場所でもある。
///
/// WHY 押した瞬間にアイコンを «叩く» か:
///   案内は静止していると «飾り» になる。押したときにそのアイコンだけが一度膨らみ、
///   文字が白へ寄ると、案内と操作が繋がっていることが伝わる。
#pragma once

#include <Engine/Scene/Components/UIElement.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Script.hpp>
#include <Scripts/Game/GameSettingsComponent.hpp>
#include <Scripts/UI/UiMotion.hpp>
#include <Scripts/Utils/InputActions.hpp>
#include <Scripts/Utils/KeyIcons.hpp>
#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>
#include <vector>

using namespace fbzz::scene;
using namespace fbzz::math;

namespace sandbox {

class UiHintBarComponent : public Script {
    FBZZ_SCRIPT(UiHintBarComponent)

public:
    FBZZ_GROUP("登録数")
    FBZZ_LIST_FIELD(std::string, entries, "登録数")
    FBZZ_TOOLTIP("«操作:文言» を右から並べる順に。操作は Confirm / Cancel / Navigate / "
                 "Dodge / Jump / Parry / Pause / Attack")

    FBZZ_GROUP("配置")
    FBZZ_FIELD(float, rightMargin, 132.0f, "Right Margin")
    FBZZ_TOOLTIP("右端からの余白 [px]。左の余白 (132) と揃える")
    FBZZ_FIELD(float, bottomMargin, 58.0f, "Bottom Margin")
    FBZZ_FIELD(float, iconSize, 30.0f, "Icon Size")
    FBZZ_FIELD(float, fontSize, 16.0f, "Font Size")
    FBZZ_FIELD(float, gapIconText, 8.0f, "Icon-Text Gap")
    FBZZ_FIELD(float, gapEntries, 30.0f, "Entry Gap")

    FBZZ_GROUP("見た目")
    FBZZ_FIELD_COLOR(textColor, (Vector4{ 0.662745f, 0.650980f, 0.627451f, 1.0f }), "Text")
    FBZZ_FIELD_COLOR(hitColor, (Vector4{ 1.0f, 1.0f, 1.0f, 1.0f }), "Text Hit")
    FBZZ_FIELD_RANGE(float, introDelay, 0.9f, "Intro Delay", 0.0f, 3.0f)
    FBZZ_TOOLTIP("画面に入ってから案内が上がってくるまで。内容より後に出す")
    FBZZ_FIELD_RANGE(float, introSeconds, 0.45f, "導入", 0.05f, 2.0f)

    FBZZ_GROUP("デバッグ")
    FBZZ_FIELD_READ_ONLY(int, debugBuilt, 0, "Built")
    FBZZ_FIELD_READ_ONLY(bool, debugPad, false, "Pad")

    void OnStart() override;
    void OnUpdate() override;

private:
    struct Entry {
        keyicon::Action action = keyicon::Action::Confirm;
        std::string     label;
        std::string     inputAction;   ///< 押したかを見る InputActions 名。空なら見ない
        EntityRef       icon;
        EntityRef       text;
        float           hit = 0.0f;    ///< 押した瞬間 1、減衰
    };
    std::vector<Entry> m_entries;
    bool  m_pad     = false;
    float m_elapsed = 0.0f;
    bool  m_axisHeld = false;

    [[nodiscard]] static bool ParseAction(std::string_view name, keyicon::Action& out, std::string& inputAction);
    /// シーンの HintIcon_i / HintText_i を掴む。足りない枠は捨てる。
    void Bind();
    void ApplyIcons();
    void Layout(float reveal);
    [[nodiscard]] bool UsingPad() const;
};

FBZZ_REFLECT(UiHintBarComponent)

inline bool UiHintBarComponent::ParseAction(std::string_view name, keyicon::Action& out,
                                            std::string& inputAction)
{
    using keyicon::Action;
    inputAction.clear();
    if (name == "Confirm")  { out = Action::Confirm;  inputAction = actions::kSubmit; return true; }
    if (name == "Cancel")   { out = Action::Cancel;   inputAction = actions::kCancel; return true; }
    if (name == "Navigate") { out = Action::MoveAxis; return true; }
    if (name == "Attack")   { out = Action::Attack;   inputAction = actions::kAttack; return true; }
    if (name == "Parry")    { out = Action::Parry;    inputAction = actions::kParry;  return true; }
    if (name == "Dodge")    { out = Action::Dodge;    inputAction = actions::kDodge;  return true; }
    if (name == "Jump")     { out = Action::Jump;     inputAction = actions::kJump;   return true; }
    if (name == "Pause")    { out = Action::Pause;    inputAction = actions::kPause;  return true; }
    return false;
}

inline bool UiHintBarComponent::UsingPad() const
{
    if (auto* s = GameSettingsComponent::Instance()) return s->Input().device == 1;
    return input.IsPadConnected();
}

inline void UiHintBarComponent::OnStart()
{
    m_entries.clear();
    for (const std::string& raw : entries) {
        const std::size_t colon = raw.find(':');
        if (colon == std::string::npos) {
            debug.LogWarning("UiHintBar: '" + raw + "' は «操作:文言» の形ではありません");
            continue;
        }
        Entry e;
        if (!ParseAction(raw.substr(0, colon), e.action, e.inputAction)) {
            debug.LogWarning("UiHintBar: 操作 '" + raw.substr(0, colon) + "' を知りません");
            continue;
        }
        e.label = raw.substr(colon + 1);
        m_entries.push_back(e);
    }
    Bind();
    m_pad = UsingPad();
    ApplyIcons();
    m_elapsed = 0.0f;
    Layout(0.0f);
}

inline void UiHintBarComponent::Bind()
{
    std::vector<Entry> bound;
    for (std::size_t i = 0; i < m_entries.size(); ++i) {
        Entry& e = m_entries[i];
        GameObject* icon = scene.Find("HintIcon_" + std::to_string(i));
        GameObject* text = scene.Find("HintText_" + std::to_string(i));
        if (!icon || !text) {
            debug.LogWarning("UiHintBar: HintIcon_/HintText_" + std::to_string(i) +
                             " がシーンにありません (案内 '" + e.label + "' は出ません)");
            continue;
        }
        // 使う枠は必ず起こす。使わない枠を下で消しているので、シーンに «余った枠» を
        // 畳んだ状態で保存しておける。ここで起こさないと、その画面で案内が 1 つ増えた
        // 瞬間に「シーンでは消えている枠」が二度と出てこない。
        icon->SetActive(true);
        text->SetActive(true);
        e.icon = EntityRef{ icon->GetID() };
        e.text = EntityRef{ text->GetID() };
        if (auto* t = text->GetComponent<UIText>()) {
            t->text     = e.label;
            t->fontSize = fontSize;
        }
        ui.SetText(text, e.label);
        bound.push_back(e);
    }
    // 使わない枠は消す (シーンに 3 枠置いてあり、画面によっては 2 つしか使わない)。
    for (std::size_t i = m_entries.size(); i < 8; ++i) {
        if (GameObject* icon = scene.Find("HintIcon_" + std::to_string(i))) icon->SetActive(false);
        if (GameObject* text = scene.Find("HintText_" + std::to_string(i))) text->SetActive(false);
    }
    m_entries = std::move(bound);
    debugBuilt = static_cast<int>(m_entries.size());
}

inline void UiHintBarComponent::ApplyIcons()
{
    for (Entry& e : m_entries) {
        GameObject* icon = e.icon.Resolve(scene);
        if (!icon) continue;
        // メニューの «選択» はキーボードでは W ではなくマウスで行う (GameCursor)。
        const std::string_view sprite =
            (e.action == keyicon::Action::MoveAxis && !m_pad) ? std::string_view(keyicon::kbm::kMouse)
                                                             : keyicon::Prompt(m_pad, e.action);
        if (!sprite.empty()) ui.SetImageTexture(icon, sprite);
    }
    debugPad = m_pad;
}

inline void UiHintBarComponent::Layout(float reveal)
{
    // 右端から左へ積む。文字の幅は実測 (transform.scale.x) を使う。まだ測れていない
    // フレームは字数から見積もる (1 フレームだけ位置がずれるが、次で直る)。
    const float e     = uimotion::OutCubic(reveal);
    const float alpha = uimotion::OutQuint(reveal * 1.25f);
    const float lift  = 18.0f * (1.0f - e);   // 出る前は少し下に居る
    const float baseY = 1080.0f - bottomMargin;
    float x = 1920.0f - rightMargin;

    for (std::size_t i = m_entries.size(); i-- > 0;) {
        Entry& en = m_entries[i];
        GameObject* text = en.text.Resolve(scene);
        GameObject* icon = en.icon.Resolve(scene);
        if (!text || !icon) continue;

        float textW = text->transform.scale.x;
        if (textW <= 1.0f) textW = fontSize * 0.9f * static_cast<float>(en.label.size() / 3 + 1);
        const float hit  = en.hit;
        const float grow = 1.0f + 0.28f * uimotion::OutBack(hit);   // 叩かれた瞬間だけ膨らむ
        const float size = iconSize * grow;

        x -= textW;
        text->transform.position = { x, baseY - fontSize * 0.5f + lift, 0.0f };
        ui.SetTextColor(text, { textColor.x + (hitColor.x - textColor.x) * hit,
                                textColor.y + (hitColor.y - textColor.y) * hit,
                                textColor.z + (hitColor.z - textColor.z) * hit,
                                textColor.w * alpha });

        x -= gapIconText + iconSize;
        icon->transform.position = { x - (size - iconSize) * 0.5f,
                                     baseY - size * 0.5f + lift, 0.0f };
        icon->transform.scale    = { size, size, 1.0f };
        ui.SetImageColor(icon, { 1.0f, 1.0f, 1.0f, alpha * (0.85f + 0.15f * hit) });

        x -= gapEntries;
    }
}

inline void UiHintBarComponent::OnUpdate()
{
    const float dt = (std::max)(time.UnscaledDeltaTime(), 0.0f);
    m_elapsed += dt;

    // 機器が変わったらアイコンを差し替える (OPTIONS で切り替えた直後にも効く)。
    const bool pad = UsingPad();
    if (pad != m_pad) { m_pad = pad; ApplyIcons(); }

    // 押した瞬間の «叩き»。軸 (選択) は倒し始めだけ。
    const Vector2 axis = input.GetMoveAxis();
    const bool axisHeld = std::abs(axis.x) > 0.4f || std::abs(axis.y) > 0.4f;
    for (Entry& e : m_entries) {
        bool pressed = false;
        if (!e.inputAction.empty()) pressed = input.GetActionDown(e.inputAction);
        else if (e.action == keyicon::Action::MoveAxis) pressed = axisHeld && !m_axisHeld;
        if (pressed) e.hit = 1.0f;
        uimotion::Decay(e.hit, dt, 0.14f);
    }
    m_axisHeld = axisHeld;

    Layout(uimotion::Stagger(m_elapsed - introDelay, 0, 0.0f, introSeconds));
}

} // namespace sandbox
