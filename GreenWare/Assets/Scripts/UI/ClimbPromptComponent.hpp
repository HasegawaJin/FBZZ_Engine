/// @file    ClimbPromptComponent.hpp
/// @brief   登れる状態のときだけ «E 登る» を画面へ出す
/// @author  Hasegawa Jin
/// @date    2026-09-10
///
/// @note 登れる判定は PlayerClimbComponent::CanMount() に一任する (間合いの二重管理を避ける)。
/// @note 出し入れは SetActive でなく透明度でフェードする (境界の出入りで点滅させないため)。
/// @note 要 Canvas 配下: iconName の UIImage と labelName の UIText。位置・大きさは毎フレームここが決める。
#pragma once

#include <Engine/Scene/EntityRef.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Scripts/Game/GameSettingsComponent.hpp>
#include <Scripts/Player/PlayerClimbComponent.hpp>
#include <Scripts/UI/UiMotion.hpp>
#include <Scripts/Utils/KeyIcons.hpp>
#include <algorithm>
#include <string>
#include <string_view>

/// @note Scene.hpp が要る: GetScript の template 本体は Scene.hpp の末尾にあり、
///       GameObject.hpp では Scene が前方宣言のみのため解決できない。
using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

/// @note 2026-09-10 に登攀ごと取り下げ、2026-09-14 に案内の枠を
///       TutorialPromptComponent へ譲った。既定が指す `HUD_ClimbIcon` /
///       `HUD_ClimbLabel` は**もうシーンに無い** (`HUD_Tutorial*` へ改名済み)。
///       戻すときは枠を別に足すこと ─ 同じ枠を 2 人で書くと取り合いになる。
class ClimbPromptComponent : public Script {
    FBZZ_SCRIPT(ClimbPromptComponent)

public:
    FBZZ_GROUP("置き場所")
    FBZZ_FIELD(std::string, iconName, "HUD_ClimbIcon", "アイコン")
    FBZZ_TOOLTIP("Canvas の下に置いた UIImage の名前。キーとパッドで絵が替わる")
    FBZZ_FIELD(std::string, labelName, "HUD_ClimbLabel", "文言")
    FBZZ_TOOLTIP("Canvas の下に置いた UIText の名前")
    FBZZ_FIELD(std::string, label, "登る", "文言の中身")

    FBZZ_GROUP("配置")
    FBZZ_FIELD_RANGE(float, centerX, 960.0f, "横の中心", 0.0f, 1920.0f)
    FBZZ_FIELD_RANGE(float, baseY, 760.0f, "縦の位置", 0.0f, 1080.0f)
    FBZZ_TOOLTIP("1080 が画面の下端。プレイヤーの足元より少し下へ置くと、"
                 "ボスを見上げたまま視界の端で読める")
    FBZZ_FIELD_RANGE(float, iconSize, 48.0f, "アイコンの大きさ", 8.0f, 200.0f)
    FBZZ_FIELD_RANGE(float, fontSize, 28.0f, "文字の大きさ", 8.0f, 120.0f)
    FBZZ_FIELD_RANGE(float, gap, 14.0f, "アイコンと文字の間", 0.0f, 100.0f)

    FBZZ_GROUP("出し入れ")
    FBZZ_FIELD_RANGE(float, fadeInSeconds, 0.14f, "出るまで", 0.0f, 2.0f)
    FBZZ_FIELD_RANGE(float, fadeOutSeconds, 0.22f, "消えるまで", 0.0f, 2.0f)
    FBZZ_TOOLTIP("消えは出るよりゆっくり。縁を歩いたときに点滅させないため")
    FBZZ_FIELD_RANGE(float, lift, 16.0f, "せり上がり", 0.0f, 100.0f)
    FBZZ_TOOLTIP("出る前にどれだけ下に居るか [px]。0 でその場に湧く")

    FBZZ_GROUP("色")
    FBZZ_FIELD_COLOR(textColor, (Vector4{ 0.92f, 0.95f, 1.0f, 1.0f }), "文字")

    FBZZ_GROUP("デバッグ")
    FBZZ_FIELD_READ_ONLY(bool, debugCanMount, false, "登れる")
    FBZZ_FIELD_READ_ONLY(float, debugReveal, 0.0f, "出ている量")

    void OnStart()  override;
    void OnUpdate() override;

private:
    /// パッドで遊んでいるか。設定が «自動» ならつながっているかで決める。
    [[nodiscard]] bool UsingPad() const;
    /// 名前で掴み直す。実行時に作られる物ではないので、見つかれば覚えたままでよい。
    [[nodiscard]] GameObject* Icon();
    [[nodiscard]] GameObject* Label();
    /// 今の出ている量で並べる。0 なら畳む。
    void Layout(float reveal);

    EntityRef m_icon;
    EntityRef m_label;
    float     m_reveal = 0.0f;
    /// 直近に書いた入力機器。替わったフレームだけ絵を貼り直す。
    bool      m_pad     = false;
    bool      m_padKnown = false;
    /// 直近に書いた表示状態。SetActive を毎フレーム叩かないため。
    bool      m_shown = false;
};

FBZZ_REFLECT(ClimbPromptComponent)

inline void ClimbPromptComponent::OnStart()
{
    m_icon = {}; m_label = {};
    m_reveal = 0.0f;
    m_padKnown = false;
    /// @note 最初の 1 回は必ず «畳む» を書かせる
    m_shown = true;
    debugCanMount = false;
    debugReveal   = 0.0f;
}

inline bool ClimbPromptComponent::UsingPad() const
{
    if (auto* settings = GameSettingsComponent::Instance()) return settings->Input().device == 1;
    return input.IsPadConnected();
}

inline GameObject* ClimbPromptComponent::Icon()
{
    if (GameObject* cached = m_icon.Resolve(scene)) return cached;
    GameObject* found = iconName.empty() ? nullptr : scene.Find(iconName, true);
    if (found) m_icon = EntityRef{ found->GetID() };
    return found;
}

inline GameObject* ClimbPromptComponent::Label()
{
    if (GameObject* cached = m_label.Resolve(scene)) return cached;
    GameObject* found = labelName.empty() ? nullptr : scene.Find(labelName, true);
    if (found) m_label = EntityRef{ found->GetID() };
    return found;
}

inline void ClimbPromptComponent::Layout(float reveal)
{
    GameObject* icon  = Icon();
    GameObject* text  = Label();
    if (!icon || !text) return;

    const bool show = reveal > 0.001f;
    if (show != m_shown) {
        icon->SetActive(show);
        text->SetActive(show);
        m_shown = show;
    }
    if (!show) return;

    /// @note 入力機器が替わったときだけ絵を貼り直す。毎フレーム貼るとテクスチャの
    ///       引き直しが入り、案内 1 つのために毎フレームアセットを触ることになる。
    const bool pad = UsingPad();
    if (!m_padKnown || pad != m_pad) {
        m_pad = pad;
        m_padKnown = true;
        const std::string_view sprite = keyicon::Prompt(pad, keyicon::Action::Climb);
        if (!sprite.empty()) ui.SetImageTexture(icon, sprite);
    }

    const float ease  = uimotion::OutCubic(reveal);
    const float alpha = uimotion::OutQuint(Clamp01(reveal * 1.25f));
    const float y     = baseY + std::max(lift, 0.0f) * (1.0f - ease);

    /// @note 文字の幅は実測 (transform.scale.x)。まだ測れていないフレームは字数から
    ///       見積もる ─ 1 フレームだけ中心がずれるが、次で直る (UiHintBar と同じ)。
    float textW = text->transform.scale.x;
    if (textW <= 1.0f)
        textW = fontSize * static_cast<float>(label.size() / 3 + 1);

    /// @note アイコンと文字を «対» として中央へ寄せる。
    const float total = iconSize + std::max(gap, 0.0f) + textW;
    const float left  = centerX - total * 0.5f;

    icon->transform.position = { left, y - iconSize * 0.5f, 0.0f };
    icon->transform.scale    = { iconSize, iconSize, 1.0f };
    ui.SetImageColor(icon, { 1.0f, 1.0f, 1.0f, alpha });

    text->transform.position = { left + iconSize + std::max(gap, 0.0f),
                                 y - fontSize * 0.5f, 0.0f };
    ui.SetText(text, label);
    ui.SetTextColor(text, { textColor.x, textColor.y, textColor.z, textColor.w * alpha });
}

inline void ClimbPromptComponent::OnUpdate()
{
    const float dt = std::max(Time::deltaTime, 0.0f);

    bool can = false;
    if (auto* climb = scene.GetScript<PlayerClimbComponent>()) can = climb->CanMount();
    debugCanMount = can;

    const float seconds = can ? std::max(fadeInSeconds, 0.0f) : std::max(fadeOutSeconds, 0.0f);
    /// @note 尺が 0 なら即座に振り切る。割り算を避けるためだけの分岐ではなく、
    ///       «演出を切る» を Inspector から選べるようにするため。
    if (seconds <= 0.0f) m_reveal = can ? 1.0f : 0.0f;
    else                 m_reveal = Clamp01(m_reveal + (can ? dt : -dt) / seconds);
    debugReveal = m_reveal;

    Layout(m_reveal);
}

} // namespace sandbox
