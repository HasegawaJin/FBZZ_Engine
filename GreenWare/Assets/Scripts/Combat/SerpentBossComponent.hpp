/// @file    SerpentBossComponent.hpp
/// @brief   ポラリティ・サーペントの «問い合わせ口»。HUD と進行がここを読む
/// @author  Hasegawa Jin
/// @date    2026-08-31
///
/// @note IDamageable はここに重ねない。HP は `EnemyHealthComponent` が持つ ─
///       `IDamageable::Registry()` は GameObject 単位の名簿で、2 つ載せると後勝ちで
///       黙って上書きされる。`GameFlowComponent` / `BossHealthBarComponent` も
///       `EnemyHealthComponent` を名指しで引く (ボス 1 の `BossCoreComponent` と同じ形)。
#pragma once

#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Scripts/Combat/EnemyHealthComponent.hpp>
#include <Scripts/Combat/IBoss.hpp>
#include <Scripts/Combat/SerpentAiComponent.hpp>
#include <Scripts/Combat/SerpentBodyComponent.hpp>
#include <Scripts/Combat/SerpentSpineComponent.hpp>
#include <Scripts/Utils/SeLibrary.hpp>
#include <string>

using namespace fbzz::scene;

namespace sandbox {

class SerpentBossComponent : public Script, public IBoss {
    FBZZ_SCRIPT_DERIVED(SerpentBossComponent, Script, IBoss)

public:
    FBZZ_GROUP("識別")
    FBZZ_FIELD(std::string, bossName, "双獄の大蛇 NIDHOGG", "名前")
    FBZZ_TOOLTIP("ボスバーに出す表示名。シーン上の GameObject 名とは別物")

    FBZZ_GROUP("デバッグ")
    FBZZ_FIELD_READ_ONLY(int, debugSegments, 28, "分割数")
    FBZZ_FIELD_READ_ONLY(int, debugPhase, 1, "位相")
    FBZZ_FIELD_READ_ONLY(bool, debugEngaged, false, "交戦中")

    /// @name IBoss
    /// @{
    [[nodiscard]] const char* BossName() const override { return bossName.c_str(); }

    /// 段階は «残り節数» から出す。フェーズ変数を別に持たない。
    [[nodiscard]] int CurrentPhase() const override
    {
        const auto* body = Body();
        return body ? body->Phase() : 1;
    }
    [[nodiscard]] int PhaseCount() const override { return SerpentBodyComponent::PhaseCount(); }

    [[nodiscard]] bool IsStaggered() const override
    {
        const auto* ai = Ai();
        return ai && ai->IsStaggered();
    }
    [[nodiscard]] bool IsEngaged() const override
    {
        const auto* ai = Ai();
        return ai && ai->IsEngaged();
    }
    /// @}

    /// @name 弾いて崩す (Docs/break-parry.md)
    /// @{
    [[nodiscard]] bool IsToppled() const override
    {
        const auto* ai = Ai();
        return ai && ai->IsToppled();
    }
    void OnParried(const fbzz::math::Vector3& hitPoint) override
    {
        if (auto* ai = Ai()) ai->OnParried(hitPoint);
    }
    bool Execute(GameObject* part, const fbzz::math::Vector3& from) override
    {
        auto* ai = Ai();
        return ai && ai->Execute(part, from);
    }
    /// 進行は残り節数。28 から 6 まで削って決着。
    [[nodiscard]] int PartsRemaining() const override
    {
        const auto* body = Body();
        return body ? body->SegmentCount() : -1;
    }
    [[nodiscard]] int PartsTotal() const override { return serpent::kSegmentCount; }

    /// 蛇の «居場所» は頭。ルートはシーンに置いた座標のまま動かない。
    [[nodiscard]] bool FocusPoint(fbzz::math::Vector3& out) const override
    {
        const auto* spine = scene.GetScript<SerpentSpineComponent>();
        if (!spine) return false;
        out = spine->HeadPosition();
        return true;
    }

    void OnStart() override;
    void OnUpdate() override;
    /// 名簿から降りる。載ったままだと、畳んだシーンのボスを進行が探し当てる。
    void OnDestroy() override { IBoss::Unbind(scene.Self(), this); }
    /// @}

private:
    [[nodiscard]] SerpentBodyComponent* Body() const
    {
        return scene.GetScript<SerpentBodyComponent>();
    }
    [[nodiscard]] SerpentAiComponent* Ai() const
    {
        return scene.GetScript<SerpentAiComponent>();
    }
};

FBZZ_REFLECT(SerpentBossComponent)

inline void SerpentBossComponent::OnStart()
{
    /// @note «ボスとして» 名乗る。型で引く経路はこの環境では空を返すので、
    ///       ここを書き忘れると倒してもステージが終わらない (理由は IBoss.hpp を参照)。
    IBoss::Bind(scene.Self(), this);

    /// @note 被弾音と撃破音を蛇の束へ差し替える。共通の束は雑魚の装甲が鳴る音で、
    ///       ボスに当てると «同じくらいのものに当たった» と読める。
    if (auto* health = scene.GetScript<EnemyHealthComponent>()) {
        health->SetFlinchVoice(&se::kBossDamaged);
        health->SetDestroyVoice(&se::kSerpentDestroy);
    } else {
        /// @note 進行はボスの EnemyHealthComponent で終わりを決める。無いと «倒しても
        ///       何も起きない» ステージになり、しかも進行側は何も言わない。
        debug.LogError("SerpentBossComponent: no EnemyHealthComponent on the serpent. "
                       "GameFlowComponent decides victory from it, so the stage can never end.");
    }

    if (!Body())
        debug.LogError("SerpentBossComponent: no SerpentBodyComponent on the serpent. "
                       "Phase and segment count cannot be read.");
}

inline void SerpentBossComponent::OnUpdate()
{
    const auto* body = Body();
    debugSegments = body ? body->SegmentCount() : 0;
    debugPhase    = CurrentPhase();
    debugEngaged  = IsEngaged();
}

} // namespace sandbox
