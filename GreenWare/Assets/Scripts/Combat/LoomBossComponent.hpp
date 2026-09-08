/// @file    LoomBossComponent.hpp
/// @brief   ポラリティ・ルームの «問い合わせ口»。HUD と進行がここを読む
/// @author  Hasegawa Jin
/// @date    2026-09-08
///
/// ⚠ 骨だけ。設計は Docs/boss-loom.md。モデル / アニメ / シーンはまだ無い。
///
/// WHY AI と分けるか (SerpentBossComponent と同じ形):
///   `IBoss` は «今どうなっているか» を外へ見せる口で、`LoomAiComponent` は
///   «どう決めているか» を持つ。同じクラスに畳むと、HUD の都合で状態機械へ
///   フィールドが生えていく。ボス 1 と 2 が既にこの分け方をしているので揃える。
///
/// WHY IDamageable をここに重ねないか:
///   `IDamageable::Registry()` は GameObject をキーにした 1 対 1 の名簿で、
///   同じオブジェクトに 2 つ載せると後から名乗った方が黙って上書きする。
///   HP を持つのは `EnemyHealthComponent` の方 ─ `GameFlowComponent` と
///   `BossHealthBarComponent` がそれを名指しで引いている。
#pragma once

#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Scripts/Combat/BossBreakComponent.hpp>
#include <Scripts/Combat/EnemyHealthComponent.hpp>
#include <Scripts/Combat/IBoss.hpp>
#include <Scripts/Combat/LoomAiComponent.hpp>
#include <Scripts/Utils/SeLibrary.hpp>
#include <string>

using namespace fbzz::scene;

namespace sandbox {

class LoomBossComponent : public Script, public IBoss {
    FBZZ_SCRIPT_DERIVED(LoomBossComponent, Script, IBoss)

public:
    FBZZ_GROUP("識別")
    FBZZ_FIELD(std::string, bossName, "POLARITY LOOM", "名前")
    FBZZ_TOOLTIP("ボスバーに出す表示名。シーン上の GameObject 名とは別物")

    FBZZ_GROUP("デバッグ")
    FBZZ_FIELD_READ_ONLY(int, debugArms, kLoomArmCount, "腕")
    FBZZ_FIELD_READ_ONLY(int, debugPhase, 1, "位相")
    FBZZ_FIELD_READ_ONLY(bool, debugEngaged, false, "交戦中")

    [[nodiscard]] const char* BossName() const override { return bossName.c_str(); }

    /// 段階は «残り腕» から出す。フェーズ変数を別に持たない。
    [[nodiscard]] int CurrentPhase() const override
    {
        const auto* ai = Ai();
        return ai ? ai->CurrentPhase() : 1;
    }
    [[nodiscard]] int PhaseCount() const override { return 2; }

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

    /// 進行は残り腕。4 本もいで決着 (Docs/boss-loom.md)。
    [[nodiscard]] int PartsRemaining() const override
    {
        const auto* ai = Ai();
        return ai ? ai->ArmsRemaining() : -1;
    }
    [[nodiscard]] int PartsTotal() const override { return kLoomArmCount; }

    void OnStart() override;
    void OnUpdate() override;
    /// 名簿から降りる。載ったままだと、畳んだシーンのボスを進行が探し当てる。
    void OnDestroy() override { IBoss::Unbind(scene.Self(), this); }

private:
    [[nodiscard]] LoomAiComponent* Ai() const { return scene.GetScript<LoomAiComponent>(); }
};

FBZZ_REFLECT(LoomBossComponent)

inline void LoomBossComponent::OnStart()
{
    // «ボスとして» 名乗る。型で引く経路はこの環境では空を返すので、
    // ここを書き忘れると倒してもステージが終わらない (IBoss.hpp の WHY)。
    IBoss::Bind(scene.Self(), this);

    if (!Ai())
        debug.LogError("LoomBossComponent: no LoomAiComponent on the loom. "
                       "Phase, topple and execute cannot be read.");

    if (auto* health = scene.GetScript<EnemyHealthComponent>()) {
        health->SetFlinchVoice(&se::kBossDamaged);
    } else {
        // 進行はボスの EnemyHealthComponent で終わりを決める。無いと «倒しても
        // 何も起きない» ステージになり、しかも進行側は何も言わない。
        debug.LogError("LoomBossComponent: no EnemyHealthComponent on the loom. "
                       "GameFlowComponent decides victory from it, so the stage can never end.");
    }

    if (!scene.GetScript<BossBreakComponent>())
        debug.LogError("LoomBossComponent: no BossBreakComponent on the loom. "
                       "The break gauge never fills, so the boss never topples and "
                       "the arms can never be executed.");
}

inline void LoomBossComponent::OnUpdate()
{
    const auto* ai = Ai();
    debugArms    = ai ? ai->ArmsRemaining() : 0;
    debugPhase   = CurrentPhase();
    debugEngaged = IsEngaged();
}

} // namespace sandbox
