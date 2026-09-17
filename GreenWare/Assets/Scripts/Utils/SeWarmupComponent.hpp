/// @file    SeWarmupComponent.hpp
/// @brief   SE を先に読み込ませて、戦闘中の «初回再生» の引っ掛かりを消す
/// @author  Hasegawa Jin
/// @date    2026-09-08
///
/// @note AudioManager::AcquireClip はパス単位のキャッシュで、初回だけその場でファイルを
///       読んで PCM へ展開する (src/Audio/AudioManager.cpp)。呼ぶのは再生を要求したスレッド
///       ─ 戦闘中のメインスレッドで、読み込みが終わるまでフレームが進まない。斬撃は
///       Bank::Pick() が «直前 2 つと同じ変奏を引かない» ため最初の十数発が毎回未読の
///       ファイルに当たり、斬った瞬間に固まる。190 本を 1 フレームで読むとそのフレームが
///       数百 ms 伸びるため、perFrame 本ずつ流して山を作らない。
#pragma once

#include <Engine/Scene/Script.hpp>
#include <Scripts/Utils/SeLibrary.hpp>
#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

using namespace fbzz::scene;

namespace sandbox {

class SeWarmupComponent : public Script {
    FBZZ_SCRIPT(SeWarmupComponent)

public:
    FBZZ_GROUP("先読み")
    FBZZ_FIELD_RANGE_INT(int, perFrame, 6, "1 フレームあたり", 1, 64)
    FBZZ_TOOLTIP("1 フレームで読み込む本数。増やすと早く終わるが、その間フレームが伸びる")
    FBZZ_FIELD(bool, warmBlades, true, "斬撃")
    FBZZ_TOOLTIP("振り・当たり・締め・溜め斬り。**引っ掛かりが一番出るのはここ**")
    FBZZ_FIELD(bool, warmPlayer, true, "プレイヤー")
    FBZZ_FIELD(bool, warmImpact, true, "衝撃・コア")
    FBZZ_FIELD(bool, warmBoss, true, "ボス")

    FBZZ_GROUP("デバッグ")
    FBZZ_FIELD_READ_ONLY(int, debugTotal, 0, "対象")
    FBZZ_FIELD_READ_ONLY(int, debugDone, 0, "済み")
    FBZZ_FIELD_READ_ONLY(int, debugFailed, 0, "読み込み失敗")

    void OnStart()  override;
    void OnUpdate() override;

private:
    void Push(const se::Bank& bank);

    std::vector<std::string> m_queue;
    std::size_t              m_head = 0;
};

FBZZ_REFLECT(SeWarmupComponent)

inline void SeWarmupComponent::Push(const se::Bank& bank)
{
    for (std::string_view path : bank.variants)
        if (!path.empty()) m_queue.emplace_back(path);
}

inline void SeWarmupComponent::OnStart()
{
    m_queue.clear();
    m_head = 0;

    if (warmBlades) {
        Push(se::kSwordSwingDown);
        Push(se::kSwordSwingUp);
        Push(se::kSwordSwingSpin);
        Push(se::kSwordSwingSlide);
        Push(se::kSwordSwingAir);
        Push(se::kSwordSwingFinish);
        Push(se::kSwordSwingCharge);
        Push(se::kSwordHit);
        Push(se::kSwordHitHeavy);
        Push(se::kSwordExecute);
        Push(se::kSwordParry);
        Push(se::kSwordParryJust);
        Push(se::kSwordGuard);
        Push(se::kBladeDash);
        Push(se::kSwordReady);
        Push(se::kBladeChargeUpLoop);
        Push(se::kBladeChargeUpFull);
        Push(se::kSwordFlux);
        Push(se::kSwordCadence);
        Push(se::kSwordCore);
    }
    if (warmPlayer) {
        Push(se::kPlayerMotorLoop);
        Push(se::kPlayerDodge);
        Push(se::kPlayerDodgeEnd);
        Push(se::kPlayerLanding);
        Push(se::kPlayerDamaged);
        Push(se::kPlayerServoTurn);
        Push(se::kPlayerWeaponStow);
        Push(se::kPlayerWeaponDeploy);
        Push(se::kPlayerClimbGrab);
        Push(se::kPlayerJump);
    }
    if (warmImpact) {
        Push(se::kImpactLight);
        Push(se::kImpactMid);
        Push(se::kImpactHeavy);
        Push(se::kImpactWall);
        Push(se::kImpactDebris);
        Push(se::kImpactCoreHit);
        Push(se::kImpactFinal);
        Push(se::kImpactPushKill);
    }
    if (warmBoss) {
        Push(se::kBossStepWalk);
        Push(se::kBossStepCharge);
        Push(se::kBossServoLoop);
        Push(se::kBossChargeRun);
        Push(se::kBossChargeWindup);
        Push(se::kBossChargeCrash);
        Push(se::kBossStunLoop);
        Push(se::kBossStunRecover);
        Push(se::kBossAppear);
        Push(se::kBossLanding);
        Push(se::kSerpentCrawlLoop);
        Push(se::kSerpentRushLoop);
        Push(se::kSerpentRear);
        Push(se::kBossDamaged);
        Push(se::kBossHatchOpen);
        Push(se::kBossHatchClose);
    }

    debugTotal = static_cast<int>(m_queue.size());
    debugDone  = 0;
    debugFailed = 0;
}

inline void SeWarmupComponent::OnUpdate()
{
    if (m_head >= m_queue.size()) return;

    const std::size_t end =
        std::min(m_queue.size(), m_head + static_cast<std::size_t>(std::max(perFrame, 1)));
    for (; m_head < end; ++m_head) {
        if (!audio.Preload(m_queue[m_head])) ++debugFailed;
    }
    debugDone = static_cast<int>(m_head);

    if (m_head >= m_queue.size()) {
        /// @note 一度読めば AudioManager の m_pathToClip に残る。もう仕事は無い。
        m_queue.clear();
        m_queue.shrink_to_fit();
    }
}

} // namespace sandbox
