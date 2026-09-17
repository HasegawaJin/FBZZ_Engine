/// @file    PlayerBreathBarComponent.hpp
/// @brief   シーンに置いた HUD の息バーへ、プレイヤーの残量を流し込む
/// @author  Hasegawa Jin
/// @date    2026-09-14
///
/// @note HUD の実体はシーン側にあり、片方を外した構成でももう片方は動く必要がある。
///       1 枚にまとめると、息バーを置いていないシーンで体力バーまでエラーで止まる。
/// @note 体力は «あと何発耐えられるか» が要るので数字を添えるが、息は «次の 1 回が
///       出せるか» でありバーの長さそのもの。数字を足すと読む順番が決まらなくなる。
#pragma once

#include <Engine/Scene/Components/UIImage.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Scripts/Player/PlayerComponent.hpp>
#include <cmath>
#include <string>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

class PlayerBreathBarComponent : public Script {
    FBZZ_SCRIPT(PlayerBreathBarComponent)

public:
    FBZZ_GROUP("HUD")
    FBZZ_REF(GameObject, breathFill, "Breath Fill")
    FBZZ_TOOLTIP("残量で塗り潰す UIImage。Fill Origin は Left にしておく")
    FBZZ_REF(GameObject, breathBackground, "Breath Background")
    FBZZ_TOOLTIP("バーの下地。**息切れはここが言う** ─ 残量 0 では塗り潰しが 1 ドットも "
                 "描かれないので、塗り側に色を乗せても画面には何も出ない")

    FBZZ_GROUP("Color")
    FBZZ_FIELD_COLOR(fullColor, (Vector4{ 0.18f, 0.56f, 0.28f, 1.00f }), "Full")
    FBZZ_TOOLTIP("満タンの色。金属の下地に乗る生命維持系の緑")
    FBZZ_FIELD_COLOR(lowColor, (Vector4{ 0.08f, 0.24f, 0.12f, 1.00f }), "Low")
    FBZZ_FIELD_COLOR(exhaustColor, (Vector4{ 0.95f, 0.36f, 0.18f, 1.00f }), "Exhausted")
    FBZZ_TOOLTIP("息切れ中の色。回避もガードも出せない «状態» なので、減り方ではなく色で言う")

    FBZZ_GROUP("Material")
    /// 名前をコードではなく設定で持つ理由は PlayerHealthBarComponent と同じ。
    FBZZ_FIELD(std::string, materialFillParam, "fillRatio", "Fill Ratio Param")
    FBZZ_TOOLTIP("残量 (0-1) を流し込むシェーダー変数名。空欄で送らない")
    FBZZ_FIELD(std::string, materialColorParam, "fillColor", "Fill Color Param")
    FBZZ_TOOLTIP("バーの色を流し込むシェーダー変数名。空欄で送らない")

    FBZZ_GROUP("息切れ")
    /// @note 息は «減ったから危ない» ではなく «出せない» が問題で、色を変えるだけだと
    ///       視界の端では «少し減った» と区別が付かないため点滅させる。
    FBZZ_FIELD_RANGE(float, exhaustPulseDepth, 0.30f, "脈動の深さ", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, exhaustPulseHz, 5.0f, "脈動の周波数 [Hz]", 0.0f, 12.0f)
    /// @note 息切れの色は状態、こちらは押した瞬間の返事。押したのに何も起きないと、
    ///       入力が拾われなかったのか息が無いのか判らないため別に返す。
    FBZZ_FIELD_RANGE(float, denyFlashSeconds, 0.22f, "空押しの閃き [秒]", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, denyFlashGain, 0.9f, "空押しの明るさ", 0.0f, 3.0f)
    FBZZ_FIELD_COLOR(backgroundColor, (Vector4{ 0.02f, 0.02f, 0.03f, 0.78f }), "下地 (通常)")
    FBZZ_TOOLTIP("息が残っている間の下地の色。シーンに置いた値と揃えておくこと")
    FBZZ_FIELD_RANGE(float, backgroundExhaustAlpha, 0.55f, "下地 (息切れ) の濃さ", 0.0f, 1.0f)

    void OnStart() override;
    void OnLateUpdate() override;

private:
    static constexpr const char* kFillName       = "HUD_BreathFill";
    static constexpr const char* kBackgroundName = "HUD_BreathBackground";

    [[nodiscard]] GameObject* Resolve(const Ref<GameObject>& reference, const char* name) const;

    EntityRef m_fill;
    EntityRef m_background;
};

FBZZ_REFLECT(PlayerBreathBarComponent)

inline GameObject* PlayerBreathBarComponent::Resolve(const Ref<GameObject>& reference,
                                                     const char* name) const
{
    if (GameObject* object = reference.Get())
        return object;
    return scene.Find(name);
}

inline void PlayerBreathBarComponent::OnStart()
{
    if (!scene.GetScript<PlayerComponent>()) {
        debug.LogError("PlayerBreathBarComponent requires PlayerComponent on the same object.");
        return;
    }

    GameObject* fill = Resolve(breathFill, kFillName);
    if (!fill) {
        debug.LogError("PlayerBreathBarComponent: breath fill UIImage not found "
                       "(assign Breath Fill, or name it HUD_BreathFill in the scene).");
        return;
    }
    m_fill = EntityRef{ fill->GetID() };

    if (GameObject* background = Resolve(breathBackground, kBackgroundName))
        m_background = EntityRef{ background->GetID() };
}

inline void PlayerBreathBarComponent::OnLateUpdate()
{
    const auto* player = scene.GetScript<PlayerComponent>();
    /// @note PlayerComponent は必須 fzdata が無いと自分を無効化する。その状態の値は読めない。
    if (!player || !player->enabled) return;

    GameObject* fill = m_fill.Resolve(scene);
    if (!fill) return;

    const float ratio     = player->NormalizedBreath();
    const bool  exhausted = player->IsBreathExhausted();

    Vector4 color = lowColor + (fullColor - lowColor) * ratio;
    float   gain  = 1.0f;
    if (exhausted) {
        color = exhaustColor;
        /// @note 実時間で回す。息が切れるのはヒットストップの最中でもあり、そこで点滅が
        ///       止まると «一番見せたい 1 コマ» だけ動かない絵になる。
        const float phase = Time::unscaledTime * exhaustPulseHz * TWO_PI;
        gain = 1.0f + std::sin(phase) * exhaustPulseDepth;
    }
    if (denyFlashSeconds > 0.0f && player->BreathDeniedWithin(denyFlashSeconds))
        gain += denyFlashGain;

    /// @note アルファは動かさない。透けると背景の明暗でバーの読みが変わる。
    const Vector4 shown{ color.x * gain, color.y * gain, color.z * gain, color.w };

    ui.SetImageFillAmount(fill, ratio);
    ui.SetImageColor(fill, shown);

    /// @note 組み込みの塗り潰しとマテリアル側の充填量へ両方流す理由は体力バーと同じ。
    if (!materialFillParam.empty())
        ui.SetMaterialFloat(fill, materialFillParam, ratio);
    if (!materialColorParam.empty())
        ui.SetMaterialColor(fill, materialColorParam, shown);

    /// @note 下地は «空になった枠» を言う層。塗りが 0 幅のときに息切れを伝えられるのは
    ///       ここだけなので、切れている間だけ枠そのものを咎めの色へ持ち上げる。
    if (GameObject* background = m_background.Resolve(scene)) {
        Vector4 base = backgroundColor;
        if (exhausted) {
            base = Vector4{ exhaustColor.x * gain, exhaustColor.y * gain,
                            exhaustColor.z * gain, Clamp01(backgroundExhaustAlpha) };
        }
        ui.SetImageColor(background, base);
    }
}

} // namespace sandbox
