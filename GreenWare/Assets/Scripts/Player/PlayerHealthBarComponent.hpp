/// @file    PlayerHealthBarComponent.hpp
/// @brief   シーンに置いた HUD の体力バーへ、プレイヤーの残量を流し込む
/// @author  Hasegawa Jin
/// @date    2026-08-22
///
/// @note HUD はシーンに配置した実体をスクリプトが更新する形にし、ランタイムでは組まない
///       (絵はシーン、値はスクリプトの責任分担。Canvas Editor で直接調整できる)。
///       敵の体力バーは対象ごとに増減するため例外的にランタイム生成のまま。
/// @note PlayerComponent が体力を内部で持ち、PlayerHealthComponent は独立した
///       ScriptComponent としてシーンに乗らない。`scene.GetScript<PlayerHealthComponent>()` は常に nullptr。
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
    /// 未設定なら名前で拾う。シーンを作り直しても既定の構成なら動く。
    FBZZ_REF(GameObject, healthFill, "Health Fill")
    FBZZ_TOOLTIP("残量で塗り潰す UIImage。Fill Origin は Left にしておく")
    FBZZ_REF(GameObject, healthBackground, "Health Background")
    FBZZ_TOOLTIP("バーの下地。色だけ反映する。未設定でも動作する")
    /// @note HP 5 は 1 目盛りの重みが大きく、残り 2 と 1 の差が生死を分ける。
    ///       バーだけでは「あと何発か」が読めないため数字も併記する。
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
    /// @note シェーダー変数名をそのまま書く。綴りが違っても無視されるだけで警告は出ない。
    ///       マテリアル未割り当てのときも何もしない。
    FBZZ_FIELD(std::string, materialFillParam, "fillRatio", "Fill Ratio Param")
    FBZZ_TOOLTIP("残量 (0-1) を流し込むシェーダー変数名。空欄で送らない")
    FBZZ_FIELD(std::string, materialColorParam, "fillColor", "Fill Color Param")
    FBZZ_TOOLTIP("バーの色を流し込むシェーダー変数名。空欄で送らない")

    FBZZ_GROUP("Low Health")
    /// @note 色の変化だけでは戦闘中の視界の端で見逃す。危険域だけ点滅を足して視線を呼ぶ。
    FBZZ_FIELD_RANGE(float, lowHealthThreshold, 0.35f, "Threshold", 0.0f, 1.0f)
    FBZZ_TOOLTIP("この残量を下回ると点滅を始める。0 で点滅しない")
    FBZZ_FIELD_RANGE(float, lowHealthPulseDepth, 0.28f, "脈動の深さ", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, lowHealthPulseHz, 2.6f, "脈動の周波数 [Hz]", 0.0f, 12.0f)

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
    EntityRef m_breathValue;
    EntityRef m_condition;
};

FBZZ_REFLECT(PlayerHealthBarComponent)

inline GameObject* PlayerHealthBarComponent::Resolve(const Ref<GameObject>& reference,
                                                     const char* name) const
{
    if (GameObject* object = reference.Get())
        return object;
    return scene.Find(name, true);
}

inline void PlayerHealthBarComponent::OnStart()
{
    if (!scene.GetScript<PlayerComponent>()) {
        debug.LogError("PlayerHealthBarComponent requires PlayerComponent on the same object.");
        return;
    }

    /// @note 参照の解決は Play 開始時に 1 度だけ。毎フレーム名前で探すと、見つからない構成で
    ///       静かに全シーン走査を続けることになる。
    GameObject* fill = Resolve(healthFill, kFillName);
    if (!fill) {
        debug.LogError("PlayerHealthBarComponent: health fill UIImage not found "
                       "(assign Health Fill, or name it HUD_HealthFill in the scene).");
        return;
    }
    m_fill = EntityRef{ fill->GetID() };

    if (GameObject* background = Resolve(healthBackground, kBackgroundName))
        m_background = EntityRef{ background->GetID() };
    /// @note 見出しと数字は無くても体力は読める。欠けていても止めない。
    if (GameObject* label = Resolve(healthLabel, kLabelName))
        m_label = EntityRef{ label->GetID() };
    if (GameObject* value = Resolve(healthValue, kValueName))
        m_value = EntityRef{ value->GetID() };

    ui.SetText(m_label.Resolve(scene), "体力");
    if (auto* object = scene.Find("HUD_BreathValue", true)) m_breathValue = EntityRef{ object->GetID() };
    if (auto* object = scene.Find("HUD_PlayerCondition", true)) m_condition = EntityRef{ object->GetID() };
}

inline float PlayerHealthBarComponent::LowHealthPulse(float ratio) const
{
    if (lowHealthThreshold <= 0.0f || ratio > lowHealthThreshold)
        return 1.0f;

    /// @note 残量が薄いほど速く強く。閾値ちょうどで振れ幅 0 から始めるので、
    ///       危険域へ入った瞬間に点滅が唐突に始まらない。
    const float depth = Clamp01(1.0f - ratio / lowHealthThreshold);
    const float phase = Time::unscaledTime * lowHealthPulseHz * TWO_PI;
    return 1.0f + std::sin(phase) * lowHealthPulseDepth * depth;
}

inline void PlayerHealthBarComponent::OnLateUpdate()
{
    const auto* player = scene.GetScript<PlayerComponent>();
    /// @note PlayerComponent は必須 fzdata 欠如で自分を無効化する。その間の
    ///       NormalizedHealth() は未設定の tuning を読むため、enabled を確認してから使う。
    if (!player || !player->enabled) return;

    GameObject* fill = m_fill.Resolve(scene);
    if (!fill) return;

    const float ratio = player->NormalizedHealth();
    /// @note 点滅は実時間で回す。ヒットストップ中に止まると、致命傷を受けた瞬間の
    ///       一番見せたいフレームだけが static な絵になる。
    const float pulse = LowHealthPulse(ratio);
    const Vector4 base = emptyColor + (fullColor - emptyColor) * ratio;
    /// @note アルファは動かさない。透けると背景の明暗でバーの読みが変わる。
    const Vector4 color{ base.x * pulse, base.y * pulse, base.z * pulse, base.w };

    ui.SetImageFillAmount(fill, ratio);
    ui.SetImageColor(fill, color);

    /// @note fillAmount は組み込みの矩形塗り潰しで .mat 不要。マテリアル側は角丸や発光まで
    ///       扱えるが .mat が要る。シーンの組み方次第でどちらを使うか変わるため両方へ送る
    ///       (未割り当ての要素では捨てられるだけで何も描かない)。
    if (!materialFillParam.empty())
        ui.SetMaterialFloat(fill, materialFillParam, ratio);
    if (!materialColorParam.empty())
        ui.SetMaterialColor(fill, materialColorParam, color);

    ui.SetText(m_value.Resolve(scene),
               std::to_string(player->Current()) + " / " + std::to_string(player->MaxHealth()));
    ui.SetTextColor(m_value.Resolve(scene), color);

    if (auto* object = m_breathValue.Resolve(scene)) {
        const int percent = static_cast<int>(Clamp01(player->NormalizedBreath()) * 100.0f);
        ui.SetText(object, std::to_string(percent) + "%");
        ui.SetTextColor(object, player->IsBreathExhausted() ? emptyColor : fullColor);
    }
    if (auto* object = m_condition.Resolve(scene)) {
        const bool low = ratio > 0.0f && ratio <= lowHealthThreshold;
        std::string condition = ratio <= 0.0f ? "戦闘不能" : low ? "体力低下" : "";
        if (player->IsBreathExhausted()) {
            if (!condition.empty()) condition += "  /  ";
            condition += "スタミナ切れ";
        }
        ui.SetText(object, condition);
        ui.SetTextColor(object, emptyColor);
    }
    const float labelScale = Clamp01(labelBrightness);
    ui.SetTextColor(m_label.Resolve(scene),
                    Vector4{ color.x * labelScale, color.y * labelScale,
                             color.z * labelScale, color.w });
}

} // namespace sandbox
