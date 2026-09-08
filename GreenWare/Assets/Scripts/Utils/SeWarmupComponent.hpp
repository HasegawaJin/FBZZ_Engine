/// @file    SeWarmupComponent.hpp
/// @brief   SE を先に読み込ませて、戦闘中の «初回再生» の引っ掛かりを消す
/// @author  Hasegawa Jin
/// @date    2026-09-08
///
/// WHY 要るか:
///   AudioManager::AcquireClip はパス単位のキャッシュで、**初回だけ**その場で
///   ファイルを読んで PCM へ展開する (src/Audio/AudioManager.cpp)。呼んでいるのは
///   再生を要求したスレッド ─ つまり戦闘中のメインスレッドで、読み込みが終わるまで
///   フレームが進まない。
///
///   斬撃はこれに一番刺さる。Bank::Pick() は «直前 2 つと同じ変奏を引かない» ので、
///   最初の十数発は毎回まだ読んでいないファイルに当たる。**斬った瞬間に固まる**のは
///   これで、素材を作り直してファイルが 1.7 倍になったぶん悪化していた。
///
/// WHY 音量 0 で «鳴らす» という形を取るか:
///   スクリプトから触れる音の入口は再生しかない (ScriptAudioProxy に Preload が無い)。
///   音量 0 の一発ものを通せば AcquireClip だけが起きて、耳には何も残らない。
///
/// WHY 1 フレームで全部やらないか:
///   190 本を一度に読むと、その 1 フレームが数百 ms 伸びる。読み込み画面なら
///   許されるが、ここはステージが始まった直後で、プレイヤーはもう操作できる。
///   1 フレームあたり perFrame 本ずつ流して、山を作らない。
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

    // 鳴らす口が要る。無ければここで足す (SeLibrary と同じ約束)。
    se::EnsureSource(scene, "SE", 0.0f);

    if (warmBlades) {
        // 段ごとに束が別なので、左右 3 段ぶんを名指しで積む。
        for (int stage = 0; stage < 3; ++stage) {
            Push(se::BladeSwing(BladeSide::Right, stage));
            Push(se::BladeSwing(BladeSide::Left,  stage));
        }
        for (BladeSide side : { BladeSide::Right, BladeSide::Left }) {
            Push(se::BladeHit(side));
            Push(se::BladeHitFinish(side));
            Push(se::BladeChargeSlash(side));
        }
        Push(se::kBladeChargeUpFull);
        Push(se::kBladeRepulse);
        Push(se::kBladeDash);
    }
    if (warmPlayer) {
        Push(se::kPlayerFootstepWalk);
        Push(se::kPlayerFootstepRun);
        Push(se::kPlayerDodge);
        Push(se::kPlayerDodgeEnd);
        Push(se::kPlayerLanding);
        Push(se::kPlayerDamaged);
        Push(se::kPlayerServoTurn);
        Push(se::kPlayerWeaponStow);
        Push(se::kPlayerWeaponDeploy);
        Push(se::kPlayerClimbGrab);
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
        Push(se::kBossDamaged);
        Push(se::kBossHatchOpen);
        Push(se::kBossHatchClose);
    }

    debugTotal = static_cast<int>(m_queue.size());
    debugDone  = 0;
}

inline void SeWarmupComponent::OnUpdate()
{
    if (m_head >= m_queue.size()) return;

    const std::size_t end =
        std::min(m_queue.size(), m_head + static_cast<std::size_t>(std::max(perFrame, 1)));
    for (; m_head < end; ++m_head) {
        // 音量 0 の一発もの。AcquireClip だけを起こして、耳には何も残さない。
        audio.PlayOneShot(m_queue[m_head], 0.0f);
    }
    debugDone = static_cast<int>(m_head);

    if (m_head >= m_queue.size()) {
        // 一度読めば AudioManager の m_pathToClip に残る。もう仕事は無い。
        m_queue.clear();
        m_queue.shrink_to_fit();
    }
}

} // namespace sandbox
