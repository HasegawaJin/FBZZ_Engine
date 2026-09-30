/// @file    Boss03BossComponent.hpp
/// @brief   ボス 3 の «問い合わせ口»。HUD と進行がここを読む
/// @author  Hasegawa Jin
/// @date    2026-09-15
///
/// @note `IBoss` は状態の外部公開、`Boss03AiComponent` は意思決定を持つ。ボス 1・2 と同じ分け方で揃える。
/// @note `IDamageable::Registry()` は GameObject 1 個につき 1 枠で後勝ち上書き。HP は `EnemyHealthComponent` 側が持ち、`GameFlowComponent` と `BossHealthBarComponent` が名指しで参照する。
#pragma once

#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Scripts/Combat/Boss03AiComponent.hpp>
#include <Scripts/Combat/Boss03AnimParams.hpp>
#include <Scripts/Combat/BossBreakComponent.hpp>
#include <Scripts/Combat/EnemyHealthComponent.hpp>
#include <Scripts/Combat/IBoss.hpp>
#include <Scripts/Utils/SeLibrary.hpp>
#include <algorithm>
#include <string>

using namespace fbzz::scene;

namespace sandbox {

class Boss03BossComponent : public Script, public IBoss {
    FBZZ_SCRIPT_DERIVED(Boss03BossComponent, Script, IBoss)
    FBZZ_REQUIRE_SCRIPT(Boss03AiComponent, EnemyHealthComponent, BossBreakComponent)

public:
    FBZZ_GROUP("識別")
    FBZZ_FIELD(std::string, bossName, "焔翼の熾天使 SERAPH", "名前")
    FBZZ_TOOLTIP("ボスバーに出す表示名。シーン上の GameObject 名とは別物")

    FBZZ_GROUP("デバッグ")
    FBZZ_OBSERVE(int, debugWings, std::max(PartsRemaining(), 0), "翼")
    FBZZ_OBSERVE(int, debugPhase, CurrentPhase(), "位相")
    FBZZ_OBSERVE(bool, debugEngaged, IsEngaged(), "交戦中")

    [[nodiscard]] const char* BossName() const override { return bossName.c_str(); }

    /// @note 段階は «残り翼» から出す。フェーズ変数を別に持たない。
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
    [[nodiscard]] bool CanExecute() const override
    { const auto* ai = Ai(); return ai && ai->CanExecute(); }
    [[nodiscard]] bool UsesBodyHitbox() const override { return true; }
    [[nodiscard]] bool CanTakeBodyDamage() const override
    { const auto* ai = Ai(); return ai && ai->CanTakeBodyDamage(); }
    bool ApplyBodyDamage(int amount) override
    { auto* ai = Ai(); return ai && ai->ApplyBodyDamage(amount); }

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

    /// @note 進行は残り翼。6 枚もいで決着 (Docs/boss03.md)。
    [[nodiscard]] int PartsRemaining() const override
    {
        const auto* ai = Ai();
        return ai ? ai->WingsRemaining() : -1;
    }
    [[nodiscard]] int PartsTotal() const override { return kBoss03WingCount; }

    void OnStart() override;
    /// @note 名簿から降りる。載ったままだと、畳んだシーンのボスを進行が探し当てる。
    void OnDestroy() override { IBoss::Unbind(scene.Self(), this); }

private:
    [[nodiscard]] Boss03AiComponent* Ai() const { return scene.GetScript<Boss03AiComponent>(); }
};

FBZZ_REFLECT(Boss03BossComponent)

inline void Boss03BossComponent::OnStart()
{
    /// @note 進行管理が既存の IBoss 名簿を読むため、開始・破棄で登録を対にする。
    IBoss::Bind(scene.Self(), this);

    if (auto* health = scene.GetScript<EnemyHealthComponent>()) {
        health->SetFlinchVoice(&se::kBossDamaged);
        health->SetDestroyVoice(&se::kBossDestroy);
    }
}

} /// @note namespace sandbox
