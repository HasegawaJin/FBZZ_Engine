/// @file    PlayerHealthBarComponent.hpp
/// @brief   シーンに置いた HUD の体力バーへ、プレイヤーの残量を流し込む
/// @author  Hasegawa Jin
/// @date    2026-08-22
///
/// WHY UI をランタイムで組まないか:
///   HUD はオーサリングの対象で、位置も太さも色も絵合わせで何度も触る。ランタイムで
///   組むと、その調整が全部 Inspector の数値経由になり、Canvas Editor で直接掴んで
///   動かせない。加えて、実行時に生成した Canvas は編集中の画面に存在しないため、
///   「再生しないと確認できない UI」になる。シーンに置いた実体をスクリプトが更新する
///   形にすれば、見た目の責任はシーン側、値の責任はスクリプト側に分かれる。
///
/// WHY 敵の体力バーは今もランタイム生成のままか:
///   あちらは WorldSpace Canvas を敵 1 体につき 1 枚必要とする。敵は増減するので
///   シーンに置いておけない。「画面に固定の 1 枚」と「対象ごとに 1 枚」は別物なので、
///   同じ作り方に揃えようとしない。
///
/// WHY PlayerHealthComponent を直接見ないか:
///   PlayerComponent は体力・移動・銃を内部モジュールとして所有しており、
///   PlayerHealthComponent は独立した ScriptComponent としてシーンに載っていない。
///   scene.GetScript<PlayerHealthComponent>() は必ず nullptr を返す。
#pragma once

#include <Engine/Scene/Components/UIImage.hpp>
#include <Engine/Scene/Components/UIText.hpp>
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

class PlayerHealthBarComponent : public Script {
    FBZZ_SCRIPT(PlayerHealthBarComponent)

public:
    FBZZ_GROUP("HUD")
    // 未設定なら名前で拾う。シーンを作り直しても既定の構成なら動く。
    FBZZ_REF(GameObject, healthFill, "Health Fill")
    FBZZ_TOOLTIP("残量で塗り潰す UIImage。Fill Origin は Left にしておく")
    FBZZ_REF(GameObject, healthBackground, "Health Background")
    FBZZ_TOOLTIP("バーの下地。色だけ反映する。未設定でも動作する")
    // WHY バーに数字を添えるか: バーは「あとどれくらいか」を一目で出すが、
    //     「あと何発耐えられるか」は読み取れない。HP 5 の設計では 1 目盛りの
    //     重みが大きく、残り 2 と 1 の差が生死を分ける。両方を並べて出す。
    FBZZ_REF(GameObject, healthLabel, "Health Label")
    FBZZ_TOOLTIP("見出しの UIText (\"HP\")。色だけ残量に追従する。未設定でも動作する")
    FBZZ_REF(GameObject, healthValue, "Health Value")
    FBZZ_TOOLTIP("残量の UIText。\"3 / 5\" の形で書き込む。未設定でも動作する")

    FBZZ_GROUP("Color")
    FBZZ_FIELD_COLOR(fullColor,  (Vector4{ 0.16f, 0.89f, 0.77f, 1.00f }), "Full")
    FBZZ_FIELD_COLOR(emptyColor, (Vector4{ 0.95f, 0.20f, 0.15f, 1.00f }), "Empty")
    FBZZ_FIELD_RANGE(float, labelBrightness, 0.72f, "Label Brightness", 0.0f, 1.0f)
    FBZZ_TOOLTIP("見出しはバーより落とす。同じ明るさだと数字とバーが競って読み順が決まらない")

    FBZZ_GROUP("Material")
    // WHY 名前を Inspector に出すか:
    //   ここに書く名前は .mat のシェーダーが宣言した変数名そのままで、綴りが違っても
    //   何も起きない (存在しない変数は無視される)。マテリアルを差し替えたときに
    //   スクリプトを直さず追従できるよう、コードではなく設定として持つ。
    //   Health Fill に UI マテリアルが割り当たっていないときは何もしない。
    FBZZ_FIELD(std::string, materialFillParam, "fillRatio", "Fill Ratio Param")
    FBZZ_TOOLTIP("残量 (0-1) を流し込むシェーダー変数名。空欄で送らない")
    FBZZ_FIELD(std::string, materialColorParam, "fillColor", "Fill Color Param")
    FBZZ_TOOLTIP("バーの色を流し込むシェーダー変数名。空欄で送らない")

    FBZZ_GROUP("Low Health")
    // WHY 残量が減ったら点滅させるか: 12.4 が「状態の変化は必ず画面で返す」と
    //     している。色だけだと戦闘中の視界の端では変化に気付けず、
    //     気付いた時には死んでいる。危険域だけ動きを足して視線を呼ぶ。
    FBZZ_FIELD_RANGE(float, lowHealthThreshold, 0.35f, "Threshold", 0.0f, 1.0f)
    FBZZ_TOOLTIP("この残量を下回ると点滅を始める。0 で点滅しない")
    FBZZ_FIELD_RANGE(float, lowHealthPulseDepth, 0.28f, "Pulse Depth", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, lowHealthPulseHz, 2.6f, "Pulse Hz", 0.0f, 12.0f)

    void OnStart() override;
    void OnLateUpdate() override;

private:
    static constexpr const char* kFillName       = "HUD_HealthFill";
    static constexpr const char* kBackgroundName = "HUD_HealthBackground";
    static constexpr const char* kLabelName      = "HUD_HealthLabel";
    static constexpr const char* kValueName      = "HUD_HealthValue";

    [[nodiscard]] GameObject* Resolve(const Ref<GameObject>& reference, const char* name) const;
    /// 危険域だけ 1 を挟んで明滅させる。安全域では常に 1 を返す。
    [[nodiscard]] float LowHealthPulse(float ratio) const;

    EntityRef m_fill;
    EntityRef m_background;
    EntityRef m_label;
    EntityRef m_value;
};

FBZZ_REFLECT(PlayerHealthBarComponent)

inline GameObject* PlayerHealthBarComponent::Resolve(const Ref<GameObject>& reference,
                                                     const char* name) const
{
    if (GameObject* object = reference.Get())
        return object;
    return scene.Find(name);
}

inline void PlayerHealthBarComponent::OnStart()
{
    if (!scene.GetScript<PlayerComponent>()) {
        debug.LogError("PlayerHealthBarComponent requires PlayerComponent on the same object.");
        return;
    }

    // 参照の解決は Play 開始時に 1 度だけ。毎フレーム名前で探すと、見つからない構成で
    // 静かに全シーン走査を続けることになる。
    GameObject* fill = Resolve(healthFill, kFillName);
    if (!fill) {
        debug.LogError("PlayerHealthBarComponent: health fill UIImage not found "
                       "(assign Health Fill, or name it HUD_HealthFill in the scene).");
        return;
    }
    m_fill = EntityRef{ fill->GetID() };

    if (GameObject* background = Resolve(healthBackground, kBackgroundName))
        m_background = EntityRef{ background->GetID() };
    // 見出しと数字は無くても体力は読める。欠けていても止めない。
    if (GameObject* label = Resolve(healthLabel, kLabelName))
        m_label = EntityRef{ label->GetID() };
    if (GameObject* value = Resolve(healthValue, kValueName))
        m_value = EntityRef{ value->GetID() };

    ui.SetText(m_label.Resolve(scene), "HP");
}

inline float PlayerHealthBarComponent::LowHealthPulse(float ratio) const
{
    if (lowHealthThreshold <= 0.0f || ratio > lowHealthThreshold)
        return 1.0f;

    // 残量が薄いほど速く強く。閾値ちょうどで振れ幅 0 から始めるので、
    // 危険域へ入った瞬間に点滅が唐突に始まらない。
    const float depth = Clamp01(1.0f - ratio / lowHealthThreshold);
    const float phase = Time::unscaledTime * lowHealthPulseHz * TWO_PI;
    return 1.0f + std::sin(phase) * lowHealthPulseDepth * depth;
}

inline void PlayerHealthBarComponent::OnLateUpdate()
{
    const auto* player = scene.GetScript<PlayerComponent>();
    // WHY enabled を見るか: PlayerComponent は必須 fzdata が無いと自分を無効化する。
    //     その状態の NormalizedHealth() は未設定の tuning を辿るため、読んではいけない。
    if (!player || !player->enabled) return;

    GameObject* fill = m_fill.Resolve(scene);
    if (!fill) return;

    const float ratio = player->NormalizedHealth();
    // 点滅は実時間で回す。ヒットストップ中に止まると、致命傷を受けた瞬間の
    // 一番見せたいフレームだけが static な絵になる。
    const float pulse = LowHealthPulse(ratio);
    const Vector4 base = emptyColor + (fullColor - emptyColor) * ratio;
    // アルファは動かさない。透けると背景の明暗でバーの読みが変わる。
    const Vector4 color{ base.x * pulse, base.y * pulse, base.z * pulse, base.w };

    ui.SetImageFillAmount(fill, ratio);
    ui.SetImageColor(fill, color);

    // WHY 両方へ送るか: fillAmount は矩形を切り落とす組み込みの塗り潰しで、
    //     マテリアルを割り当てていない構成でも動く。マテリアル側の充填量は
    //     角丸の内側や先端の発光まで扱える代わりに .mat が要る。
    //     どちらを使うかはシーンの組み方で決まるので、スクリプトは両方へ流す。
    //     マテリアルが無い要素では下の 2 行は溜まるだけで何も描かない。
    if (!materialFillParam.empty())
        ui.SetMaterialFloat(fill, materialFillParam, ratio);
    if (!materialColorParam.empty())
        ui.SetMaterialColor(fill, materialColorParam, color);

    ui.SetText(m_value.Resolve(scene),
               std::to_string(player->Current()) + " / " + std::to_string(player->MaxHealth()));
    ui.SetTextColor(m_value.Resolve(scene), color);

    const float labelScale = Clamp01(labelBrightness);
    ui.SetTextColor(m_label.Resolve(scene),
                    Vector4{ color.x * labelScale, color.y * labelScale,
                             color.z * labelScale, color.w });
}

} // namespace sandbox
