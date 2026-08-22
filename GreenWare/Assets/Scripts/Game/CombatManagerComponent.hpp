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
#include <Scripts/Combat/EnemyHealthComponent.hpp>
#include <Scripts/Player/PlayerComponent.hpp>
#include <Scripts/Polarity/PolarityFieldComponent.hpp>
#include <Scripts/Utils/ManagerWatch.hpp>
#include <algorithm>
#include <functional>

using namespace fbzz::scene;

namespace sandbox {

class CombatManagerComponent : public Script {
    FBZZ_SCRIPT(CombatManagerComponent)

public:
    FBZZ_GROUP("Debug")
    FBZZ_FIELD_READ_ONLY(int, debugKills, 0, "Kills")
    FBZZ_FIELD_READ_ONLY(int, debugEnemyImpacts, 0, "Enemy Impacts")
    FBZZ_FIELD_READ_ONLY(int, debugAnchorImpacts, 0, "Anchor Impacts")
    FBZZ_FIELD_READ_ONLY(int, debugDamageToEnemies, 0, "Damage Dealt")
    FBZZ_FIELD_READ_ONLY(int, debugDamageToPlayer, 0, "Damage Taken")

    /// 誰から呼ばれるかは決まっていない (敵 AI・罠・盤面)。置き場所に依存しない窓口を持つ。
    [[nodiscard]] static CombatManagerComponent* Instance() { return s_instance; }

    [[nodiscard]] int Kills() const { return m_kills; }
    [[nodiscard]] int EnemyImpacts() const { return m_enemyImpacts; }
    [[nodiscard]] int AnchorImpacts() const { return m_anchorImpacts; }
    [[nodiscard]] int DamageDealt() const { return m_damageToEnemies; }
    [[nodiscard]] int DamageTaken() const { return m_damageToPlayer; }

    /// 生存している敵の数。全滅判定は進行側が持つが、数える規則は戦闘側の知識。
    [[nodiscard]] int CountEnemiesAlive() const;

    /// 敵を 1 体倒した瞬間に呼ばれる。進行側が撃破数の演出やウェーブ管理に使う。
    std::function<void()> onEnemyDefeated;

    /// プレイヤーへのダメージ。敵の接触攻撃など、盤面を経由しない経路はここを通す。
    /// 実際に減ったら true。無敵時間などで弾かれた場合は false。
    bool DamagePlayer(GameObject* player, int amount);

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

    // 盤面がシーンに 1 つも無いなら、極性衝突のダメージは永久に入らない。
    // 黙って進むと「当てても減らない」だけが残るので、1 度だけ名指しで止める。
    // ただし最初の数フレームで繋がらないのは、盤面の OnStart がこちらより後ろに
    // 並んでいるだけのことが多い。猶予を過ぎても繋がらないときだけ出す。
    if (m_fieldWatch.ShouldReport(m_subscribed)) {
        debug.LogError("CombatManagerComponent found no PolarityFieldComponent in the scene. "
                       "Polarity impacts will never deal damage.");
    }
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
    auto* component = scene.GetScript<PlayerComponent>(player);
    if (!component) return false;

    const int damage = std::max(amount, 1);
    const int before = component->Current();
    if (!component->TakeDamage(damage)) return false;

    // 無敵時間で弾かれた分を数えないよう、実際に減った量だけを積む。
    m_damageToPlayer += std::max(before - component->Current(), 0);
    debugDamageToPlayer = m_damageToPlayer;
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

    DamageEnemy(impact.mover, impact);

    // 柱・壁は動かないので受け手にならない。正面衝突で mover == struck になる経路も弾く。
    if (!impact.struckIsAnchor && impact.struck != impact.mover)
        DamageEnemy(impact.struck, impact);
}

} // namespace sandbox
