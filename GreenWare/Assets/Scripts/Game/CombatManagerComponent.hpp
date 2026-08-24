/// @file CombatManagerComponent.hpp
/// @brief ダメージの適用を 1 箇所に集め、戦果を数える
/// @author Hasegawa Jin
/// @date 2026-08-22
///
/// WHY ダメージを「与える側」ではなくここで適用するか:
///   以前は極性衝突だけがここを通り、敵からプレイヤーへの接触ダメージは
///   EnemyChaserComponent が PlayerComponent::TakeDamage() を直接呼んでいた。
///   ダメージの経路が 2 本あると、「無敵時間を入れたい」「与ダメージを記録したい」
///   といった 1 つの要求が必ず 2 箇所の編集になり、片方を忘れた側だけ仕様から外れる。
///   実際、撃破数は数えられているのに被ダメージはどこにも残っていなかった。
///   誰が誰へ何点入れるかは、この 1 本の API を通す。
///
/// WHY ダメージ「値」までは持たないか:
///   敵ごとの耐久や攻撃力は敵の種類の性質で、EnemyHealthComponent /
///   EnemyChaserComponent に載っているのが正しい。ここが持つのは「適用と集計」で、
///   値まで吸い上げると敵を 1 種類足すたびにこのファイルが伸びる。
///
/// WHY GameFlowComponent から切り出すか:
///   戦闘の解決 (誰が何ダメージ受けたか) とゲーム進行の管理 (勝敗・HUD・シーン遷移) は、
///   触る理由も触る頻度も違う。ダメージ式は 18.2 が未決なので何度も触るが、リザルトへの
///   遷移条件はほとんど変わらない。同居していると、ダメージを 1 行変えるたびに
///   シーン遷移のコードを読む羽目になり、逆に遷移条件を直すと戦闘の解決まで巻き込む。
///
/// WHY 衝突の購読口をここ 1 箇所にするか:
///   PolarityFieldComponent::onImpact は単一のコールバックで、後から代入した側が勝つ。
///   複数のスクリプトが購読しにいくと「どちらが生きているか」が登録順に依存し、
///   ダメージが入ったり入らなかったりする。盤面からの衝突を受けるのはこのスクリプトだけ、
///   と決めて、戦果は取得用の API で他へ渡す。
#pragma once

#include <Engine/Scene/Script.hpp>
#include <Scripts/Combat/CharacterEvent.hpp>
#include <Scripts/Combat/EnemyHealthComponent.hpp>
#include <Scripts/Combat/EyeSpriteComponent.hpp>
#include <Scripts/Combat/IDamageable.hpp>
#include <Scripts/Polarity/PolarityFieldComponent.hpp>
#include <Scripts/Utils/ManagerWatch.hpp>
#include <algorithm>
#include <functional>

using namespace fbzz::scene;

namespace sandbox {

class CombatManagerComponent : public Script {
    FBZZ_SCRIPT(CombatManagerComponent)

public:
    // WHY チェインの長さをここが持つか: 「1 回の仕掛けがどれだけ連鎖したか」は
    //     衝突を数えることそのもので、既にこのスクリプトが数えている戦果の一種。
    //     表示側に持たせると、HUD を消しただけで数え方まで消える。
    FBZZ_GROUP("Chain")
    FBZZ_FIELD_RANGE(float, chainSeconds, 2.2f, "Chain Window", 0.2f, 10.0f)
    FBZZ_TOOLTIP("次の衝突がこの秒数以内なら同じチェインとして数える")

    FBZZ_GROUP("Debug")
    FBZZ_FIELD_READ_ONLY(int, debugKills, 0, "Kills")
    FBZZ_FIELD_READ_ONLY(int, debugEnemyImpacts, 0, "Enemy Impacts")
    FBZZ_FIELD_READ_ONLY(int, debugAnchorImpacts, 0, "Anchor Impacts")
    FBZZ_FIELD_READ_ONLY(int, debugDamageToEnemies, 0, "Damage Dealt")
    FBZZ_FIELD_READ_ONLY(int, debugDamageToPlayer, 0, "Damage Taken")
    FBZZ_FIELD_READ_ONLY(int, debugChain, 0, "Chain")
    FBZZ_FIELD_READ_ONLY(int, debugBestChain, 0, "Best Chain")
    FBZZ_FIELD_READ_ONLY(int, debugFaces, 0, "Faces")
    FBZZ_TOOLTIP("出来事を受け取れる目の数。想定より少なければ、そのキャラクターに "
                 "EyeSpriteComponent が付いていない")

    /// 誰から呼ばれるかは決まっていない (敵 AI・罠・盤面)。置き場所に依存しない窓口を持つ。
    [[nodiscard]] static CombatManagerComponent* Instance() { return s_instance; }

    [[nodiscard]] int Kills() const { return m_kills; }
    [[nodiscard]] int EnemyImpacts() const { return m_enemyImpacts; }
    [[nodiscard]] int AnchorImpacts() const { return m_anchorImpacts; }
    [[nodiscard]] int DamageDealt() const { return m_damageToEnemies; }
    [[nodiscard]] int DamageTaken() const { return m_damageToPlayer; }

    /// 続いている連鎖の長さ。途切れると 0 に戻る。
    [[nodiscard]] int ChainCount() const { return m_chain; }
    /// このプレイでの最長。リザルトで使う。
    [[nodiscard]] int BestChain() const { return m_bestChain; }
    /// 連鎖が途切れるまでの残り (1 → 直後 / 0 → 途切れた)。表示の減衰に使う。
    [[nodiscard]] float ChainRemaining01() const
    {
        return chainSeconds <= 0.0f ? 0.0f
                                    : std::clamp(m_chainRemaining / chainSeconds, 0.0f, 1.0f);
    }

    /// 生存している敵の数。全滅判定は進行側が持つが、数える規則は戦闘側の知識。
    [[nodiscard]] int CountEnemiesAlive() const;

    /// 敵を 1 体倒した瞬間に呼ばれる。進行側が撃破数の演出やウェーブ管理に使う。
    std::function<void()> onEnemyDefeated;

    /// プレイヤーへのダメージ。敵の接触攻撃など、盤面を経由しない経路はここを通す。
    /// 実際に減ったら true。無敵時間などで弾かれた場合は false。
    bool DamagePlayer(GameObject* player, int amount);

    /// character の身に起きたことを、そのキャラクターの反応へ配る。
    ///
    /// 被弾と撃破はここが自分で流すので、外から呼ぶのは「見つけた」「攻撃を出した」など
    /// ダメージを伴わない出来事だけでよい。
    ///
    /// WHY 反応先を呼び出し元に選ばせないか:
    ///   同じ «倒れた» に対して、表情・アニメーション・HUD・ボイスがそれぞれ反応する。
    ///   呼び出し元が反応先を名指しすると、反応を 1 つ足すたびに敵 AI とプレイヤーの
    ///   両方を触ることになり、片方だけ足し忘れた種類の敵ができる。
    ///   誰に何が起きたかを言うのは呼び出し元、どこへ配るかを決めるのはここ。
    void Notify(GameObject* character, CharacterEvent event) const;

    void OnStart() override;
    void OnUpdate() override;
    void OnDestroy() override { if (s_instance == this) s_instance = nullptr; }

private:
    static inline CombatManagerComponent* s_instance = nullptr;

    /// 盤面の購読。開始順に依存しないよう、繋がるまで毎フレーム試す。
    void EnsureSubscribed();
    void ResolveImpact(const PolarityImpact& impact);
    /// 敵 1 体ぶんのダメージ適用。撃破したら戦果を進める。
    void DamageEnemy(GameObject* target, const PolarityImpact& impact);

    bool m_subscribed = false;
    // 「盤面が居ない」の報告口。購読は繋がるまで毎フレーム試すので、
    // 最初の数フレームで繋がらないのは並び順の都合でしかない (ManagerWatch.hpp)。
    ManagerWatch m_fieldWatch;

    int m_kills = 0;
    int m_enemyImpacts = 0;
    int m_anchorImpacts = 0;
    int m_damageToEnemies = 0;
    int m_damageToPlayer = 0;

    int   m_chain = 0;
    int   m_bestChain = 0;
    float m_chainRemaining = 0.0f;
};

FBZZ_REFLECT(CombatManagerComponent)

inline void CombatManagerComponent::OnStart()
{
    if (s_instance && s_instance != this) {
        debug.LogWarning("CombatManagerComponent: another instance is already active. "
                         "The most recently started one takes over.");
    }
    s_instance = this;

    m_kills = 0;
    m_enemyImpacts = 0;
    m_anchorImpacts = 0;
    m_damageToEnemies = 0;
    m_damageToPlayer = 0;
    m_chain = 0;
    m_bestChain = 0;
    m_chainRemaining = 0.0f;
    m_subscribed = false;
    m_fieldWatch.Reset();

    EnsureSubscribed();
}

inline void CombatManagerComponent::EnsureSubscribed()
{
    if (m_subscribed) return;

    // WHY 繋がるまで試し続けるか: 盤面とこのマネージャーは別の GameObject に居るので、
    //     どちらの OnStart が先に走るかはシーンの並び次第になる。開始時の 1 回だけで
    //     諦めると、並べ替えただけでダメージが入らなくなる。
    auto* field = PolarityFieldComponent::Instance();
    if (!field) return;

    field->onImpact = [this](const PolarityImpact& impact) { ResolveImpact(impact); };
    m_subscribed = true;
}

inline void CombatManagerComponent::OnUpdate()
{
    EnsureSubscribed();
    debugFaces = EyeSpriteComponent::RegisteredCount();

    // WHY 実時間で数えるか: 連鎖の途中は必ずヒットストップが掛かる。縮んだ時間で
    //     数えると、派手に決まった連鎖ほど猶予が伸びて別物の判定になる。
    if (m_chainRemaining > 0.0f) {
        m_chainRemaining -= std::max(time.UnscaledDeltaTime(), 0.0f);
        if (m_chainRemaining <= 0.0f) {
            m_chainRemaining = 0.0f;
            m_chain = 0;
            debugChain = 0;
        }
    }

    // 盤面がシーンに 1 つも無いなら、極性衝突のダメージは永久に入らない。
    // 黙って進むと「当てても減らない」だけが残るので、1 度だけ名指しで止める。
    // ただし最初の数フレームで繋がらないのは、盤面の OnStart がこちらより後ろに
    // 並んでいるだけのことが多い。猶予を過ぎても繋がらないときだけ出す。
    if (m_fieldWatch.ShouldReport(m_subscribed)) {
        debug.LogError("CombatManagerComponent found no PolarityFieldComponent in the scene. "
                       "Polarity impacts will never deal damage.");
    }
}

inline void CombatManagerComponent::Notify(GameObject* character, CharacterEvent event) const
{
    if (!character) return;

    // 反応を持たないキャラクターは黙って素通りさせる。目を付けていないだけで
    // 敵が 1 種類まるごと «壊れている» ように見えてはいけない。
    if (auto* eyes = EyeSpriteComponent::For(character)) eyes->React(event);
}

inline int CombatManagerComponent::CountEnemiesAlive() const
{
    int count = 0;
    for (GameObject* object : scene.FindObjectsOfType<EnemyHealthComponent>()) {
        const auto* health = scene.GetScript<EnemyHealthComponent>(object);
        if (object && object->activeInHierarchy() && health && health->IsAlive()) ++count;
    }
    return count;
}

inline bool CombatManagerComponent::DamagePlayer(GameObject* player, int amount)
{
    // WHY PlayerComponent ではなく IDamageable で引くか: 「殴られる側」であることだけが
    //     ここでの関心で、それがプレイヤーかどうかは知らなくてよい。将来プレイヤーが
    //     乗り物に乗る / 分身を出すといった構成になっても、この関数は変わらない。
    auto* target = scene.GetScript<IDamageable>(player);
    if (!target) return false;

    const int before = target->CurrentHealth();
    if (!target->ApplyDamage(std::max(amount, 1))) return false;

    // 無敵時間で弾かれた分を数えないよう、実際に減った量だけを積む。
    m_damageToPlayer += std::max(before - target->CurrentHealth(), 0);
    debugDamageToPlayer = m_damageToPlayer;

    Notify(player, target->IsAlive() ? CharacterEvent::Hurt : CharacterEvent::Defeated);
    return true;
}

inline void CombatManagerComponent::DamageEnemy(GameObject* target, const PolarityImpact& impact)
{
    auto* health = scene.GetScript<EnemyHealthComponent>(target);
    if (!health) return;

    const int before = health->Current();
    const bool defeated = health->TakeImpact(impact);
    m_damageToEnemies += std::max(before - health->Current(), 0);
    debugDamageToEnemies = m_damageToEnemies;

    Notify(target, defeated ? CharacterEvent::Defeated : CharacterEvent::Hurt);
    if (!defeated) return;

    ++m_kills;
    debugKills = m_kills;
    if (onEnemyDefeated) onEnemyDefeated();
}

inline void CombatManagerComponent::ResolveImpact(const PolarityImpact& impact)
{
    if (impact.struckIsAnchor) {
        ++m_anchorImpacts;
        debugAnchorImpacts = m_anchorImpacts;
    } else {
        ++m_enemyImpacts;
        debugEnemyImpacts = m_enemyImpacts;
    }

    // 猶予の中に次が来れば同じ連鎖。1 回の仕掛けから何手続いたかを数える。
    ++m_chain;
    m_chainRemaining = std::max(chainSeconds, 0.0f);
    m_bestChain      = std::max(m_bestChain, m_chain);
    debugChain       = m_chain;
    debugBestChain   = m_bestChain;

    DamageEnemy(impact.mover, impact);

    // WHY 受け手がアンカーでも減らすか: 7.6 で柱を廃した後、動かない側に残ったのは
    //     «重すぎて飛ばない敵» (Roller) と撃破コアだけになった。10.1 は Roller を
    //     「衝突 2 回で撃破」と決めているので、的であることはダメージを切る理由にならない。
    //     地形は EnemyHealthComponent を持たないため、DamageEnemy が自然に空振りする。
    //     正面衝突で mover == struck になる経路だけは弾く。
    if (impact.struck != impact.mover)
        DamageEnemy(impact.struck, impact);
}

} // namespace sandbox
