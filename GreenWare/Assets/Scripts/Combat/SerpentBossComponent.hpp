/// @file    SerpentBossComponent.hpp
/// @brief   ポラリティ・サーペントの «問い合わせ口»。HUD と進行がここを読む
/// @author  Hasegawa Jin
/// @date    2026-08-31
///
/// WHY IDamageable をここに重ねないか (boss-serpent-scripts.md からの変更点):
///   企画は «HP は IDamageable の口で受ける» と書いているが、実装では HP を持つのは
///   `EnemyHealthComponent` の方になる。理由は 2 つ:
///     - `IDamageable::Registry()` は GameObject をキーにした 1 対 1 の名簿で、
///       同じオブジェクトに 2 つ載せると後から名乗った方が前のを黙って上書きする。
///       撃破が «どちらの HP» で決まるかがスクリプトの並び順で変わってしまう。
///     - `GameFlowComponent` と `BossHealthBarComponent` は既に
///       `EnemyHealthComponent` を名指しで引いている (無いとステージが終わらない)。
///   «ボスバーがどちらを読むかで割れる» という企画の懸念はそのままで、
///   割れないようにする答えが «IBoss 側に重ねない» になっている ─ ボス 1 の
///   `BossPolarityCoreComponent` と同じ形。
///
/// WHY 極を返さないか:
///   この蛇は極を塗り替えない (boss-serpent.md「ボス 1 との違い」)。体に乗るのは
///   プレイヤーが剣で置いた極だけで、ボス自身は何極でもない。Neutral を返すのは
///   «今は無防備» の意味ではなく «この軸をこのボスは持たない» という宣言。
#pragma once

#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Scripts/Combat/EnemyHealthComponent.hpp>
#include <Scripts/Combat/IBoss.hpp>
#include <Scripts/Combat/SerpentAiComponent.hpp>
#include <Scripts/Combat/SerpentBodyComponent.hpp>
#include <Scripts/Utils/SeLibrary.hpp>
#include <string>

using namespace fbzz::scene;

namespace sandbox {

class SerpentBossComponent : public Script, public IBoss {
    FBZZ_SCRIPT_DERIVED(SerpentBossComponent, Script, IBoss)

public:
    FBZZ_GROUP("Identity")
    FBZZ_FIELD(std::string, bossName, "POLARITY SERPENT", "Name")
    FBZZ_TOOLTIP("ボスバーに出す表示名。シーン上の GameObject 名とは別物")

    FBZZ_GROUP("Debug")
    FBZZ_FIELD_READ_ONLY(int, debugSegments, 28, "Segments")
    FBZZ_FIELD_READ_ONLY(int, debugPhase, 1, "Phase")
    FBZZ_FIELD_READ_ONLY(bool, debugEngaged, false, "Engaged")

    // ── IBoss ───────────────────────────────────────────────────────────────
    [[nodiscard]] const char* BossName() const override { return bossName.c_str(); }

    /// この蛇では意味を持たない。ボスは極を塗り替えないので «何極か» が無い。
    [[nodiscard]] Polarity CurrentPolarity() const override { return Polarity::None; }
    /// 切り替えを持たないので負を返す (IBoss.hpp の取り決め)。
    [[nodiscard]] float PolaritySwitchRemaining() const override { return -1.0f; }

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

    void OnStart() override;
    void OnUpdate() override;
    /// 名簿から降りる。載ったままだと、畳んだシーンのボスを進行が探し当てる。
    void OnDestroy() override { IBoss::Unbind(scene.Self(), this); }

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
    // «ボスとして» 名乗る。型で引く経路はこの環境では空を返すので、
    // ここを書き忘れると倒してもステージが終わらない (IBoss.hpp の WHY)。
    IBoss::Bind(scene.Self(), this);

    // 被弾音と撃破音を蛇の束へ差し替える。共通の束は雑魚の装甲が鳴る音で、
    // ボスに当てると «同じくらいのものに当たった» と読める。
    if (auto* health = scene.GetScript<EnemyHealthComponent>()) {
        health->SetFlinchVoice(&se::kBossDamaged);
        health->SetDestroyVoice(&se::kSerpentDestroy);
    } else {
        // 進行はボスの EnemyHealthComponent で終わりを決める。無いと «倒しても
        // 何も起きない» ステージになり、しかも進行側は何も言わない。
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
