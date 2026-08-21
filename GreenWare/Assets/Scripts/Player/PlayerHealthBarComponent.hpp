/// @file PlayerHealthBarComponent.hpp
/// @brief シーンに置いた HUD の体力バーへ、プレイヤーの残量を流し込む
/// @author Hasegawa Jin
/// @date 2026-08-22
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
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Scripts/Player/PlayerComponent.hpp>

using namespace fbzz::scene;
using namespace fbzz::math;

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

    FBZZ_GROUP("Color")
    FBZZ_FIELD_COLOR(fullColor,  (Vector4{ 0.16f, 0.89f, 0.77f, 1.00f }), "Full")
    FBZZ_FIELD_COLOR(emptyColor, (Vector4{ 0.95f, 0.20f, 0.15f, 1.00f }), "Empty")

    void OnStart() override;
    void OnLateUpdate() override;

private:
    static constexpr const char* kFillName       = "HUD_HealthFill";
    static constexpr const char* kBackgroundName = "HUD_HealthBackground";

    [[nodiscard]] GameObject* Resolve(const Ref<GameObject>& reference, const char* name) const;

    EntityRef m_fill;
    EntityRef m_background;
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
    ui.SetImageFillAmount(fill, ratio);
    ui.SetImageColor(fill, emptyColor + (fullColor - emptyColor) * ratio);
}

} // namespace sandbox
