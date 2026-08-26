/// @file EnemySerpentComponent.hpp
/// @brief Enemy B「Serpent」— 地を這って噛みつき、触れた仲間へ極性を伝染させる導体 (企画書 8)
/// @author Hasegawa Jin
/// @date 2026-08-24
///
/// WHY 伝染をこの敵だけが持つか (企画書 8 / 10.1):
///   3.2 は「片銃連射でしか同極を重ねられない」を制約として置いている。Serpent は
///   その制約を唯一飛び越えられる敵として設計されていて、1 発で複数体を帯電させる
///   ＝ 7.9 の集束コンボの起点になる。伝染が無い Serpent は «中くらいの Mite» でしかない。
///
/// WHY 無極の相手にしか伝染させないか:
///   7 章のルール 3 行をそのまま適用すると、逆極の仲間へ伝染した瞬間に中和が起きる。
///   プレイヤーが組んだ線を敵が勝手に消しに来ることになり、3.1 の「敵を武器として使う」が
///   「敵に邪魔される」に反転する。同極への伝染も延長にしかならず、盤面は変わらないのに
///   残り時間だけが伸びて 3.3 の «残り何秒かを読む» が濁る。増えるのは «帯電した体数» だけ
///   でよく、それが 7.9 の作用半径拡大へそのまま効く。
///
/// WHY 接触判定を球で取るか:
///   「接触している仲間へ」が仕様だが、この敵の当たり判定は 10 節の胴体をまとめた
///   1 つのコライダーで、実際の «触れている» とは形が違う。物理接触に厳密に合わせると、
///   見た目は絡んでいるのに伝染しないフレームが出る。半径を露出させ、どこまでを接触と
///   見なすかを調整値として持つ。
///
/// WHY 移した先へ放電を残すか:
///   帯電そのものは受け取った側が光って知らせる (PolarityTarget) が、それだけでは
///   «自分が撃っていない敵がいつの間にか光っている» としか読めない。この敵が
///   «移した» ことは、線が繋がっている絵でしか伝わらない。1 発で盤面が広がるのが
///   Serpent の役どころなので、広がった経路が見えないと役ごと伝わらない。
#pragma once

#include <Engine/Scene/Components/RigidBodyComponent.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Scripts/Combat/EnemyAiBase.hpp>
#include <Scripts/Polarity/PolarityTargetComponent.hpp>
#include <Scripts/Utils/BodyBounds.hpp>
#include <Scripts/Utils/ElectricArc.hpp>
#include <Scripts/Utils/PolarityTypes.hpp>
#include <Scripts/Utils/SeLibrary.hpp>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

class EnemySerpentComponent : public EnemyAiBase {
    FBZZ_SCRIPT_DERIVED(EnemySerpentComponent, EnemyAiBase)

public:
    FBZZ_GROUP("Bite")
    FBZZ_FIELD_RANGE(float, attackRange, 3.0f, "Attack Range", 0.2f, 12.0f)
    FBZZ_TOOLTIP("鎌首が届く距離。全長 4.5m の胴体ぶん、Mite より遠くから噛める")
    FBZZ_FIELD_RANGE(float, attackDuration, 1.5f, "Attack Duration", 0.1f, 6.0f)
    FBZZ_TOOLTIP("噛みつきモーションの長さ。Attack.anim の尺 (1.5 秒) に合わせる")
    FBZZ_FIELD_RANGE(float, attackHitTime, 0.65f, "Hit Time", 0.0f, 6.0f)
    FBZZ_TOOLTIP("振り始めから牙が届くまでの秒数")
    FBZZ_FIELD_RANGE(float, attackHitRange, 3.6f, "Hit Range", 0.2f, 12.0f)
    FBZZ_FIELD_RANGE(float, lungeSpeed, 1.6f, "Lunge Speed", 0.0f, 10.0f)
    FBZZ_TOOLTIP("牙が届くまで前へ詰める速さ。0 にするとその場で振り切る")

    FBZZ_GROUP("Conduction")
    FBZZ_FIELD(bool, conduct, true, "Conduct Polarity")
    FBZZ_TOOLTIP("自分が帯びている極を、近くの無極の敵へ移す (8 章の導体)")
    FBZZ_FIELD_RANGE(float, conductRadius, 2.6f, "Conduct Radius", 0.2f, 15.0f)
    FBZZ_TOOLTIP("ここまでを『接触している』と見なす。胴体の半径 + 余裕ぶん")
    FBZZ_FIELD_RANGE(float, conductInterval, 0.30f, "Conduct Interval", 0.05f, 3.0f)
    FBZZ_TOOLTIP("伝染を試す間隔。短いほど絡んだ瞬間に移るが、盤面を毎フレーム走査する")
    FBZZ_FIELD_RANGE_INT(int, conductMaxPerTick, 3, "Max Per Tick", 1, 16)
    FBZZ_TOOLTIP("1 回の伝染で帯電させる上限。群れの中で一斉に全員が光るのを防ぐ")
    FBZZ_FIELD_RANGE(float, conductArcSeconds, 0.4f, "Arc Seconds", 0.05f, 2.0f)
    FBZZ_TOOLTIP("移した相手との間に放電を残す時間。極がどこへ渡ったかを見せる")

    FBZZ_GROUP("Debug")
    FBZZ_FIELD_READ_ONLY(int, debugConducted, 0, "Conducted")

    void OnFixedUpdate() override;
    /// 放電の子オブジェクトごと片付ける。倒れた後に筋だけが空中へ残らないように。
    void OnDestroy() override;

protected:
    void OnEnemyStart() override;
    void OnEnemyUpdate() override;
    void OnEnemyDisable() override;

    [[nodiscard]] const se::Bank* MoveVoiceBank()    const override
    { return &se::kSerpentCrawlLoop; }
    [[nodiscard]] const se::Bank* DestroyVoiceBank() const override
    { return &se::kSerpentDestroy; }

private:
    /// 移した先 1 体ぶんの放電。時間で消えるので、消えた後も枠は使い回す。
    struct ConductLink {
        EntityRef         target;
        float             remaining = 0.0f;
        Polarity          polarity  = Polarity::None;
        ElectricArcBundle arc;
    };

    void TickAttack(float dt);
    /// 自分の極を周囲の無極の敵へ移す。移した体数を返す。
    int  Conduct();
    /// 移した相手へ放電を張る。枠が足りなければ最も古い 1 本を張り替える。
    void BeginConductArc(GameObject& target, Polarity polarity);
    /// 張ってある放電を 1 フレーム進め、時間切れの分を消す。
    void UpdateConductArcs(float dt);

    float m_attackTimer    = 0.0f;
    bool  m_attackLanded   = false;
    float m_conductTimer   = 0.0f;
    int   m_conductedTotal = 0;
    std::vector<ConductLink> m_links;
};

FBZZ_REFLECT(EnemySerpentComponent)


inline void EnemySerpentComponent::OnEnemyStart()
{
    // 這って進む敵。物理の回転に任せると胴体が横倒しになる。
    physics.SetFreezeRotation(true, true, true);

    m_attackTimer    = 0.0f;
    m_attackLanded   = false;
    m_conductTimer   = 0.0f;
    m_conductedTotal = 0;
    debugConducted   = 0;
}

// WHY 伝染を OnFixedUpdate ではなく OnUpdate 側に置くか:
//   伝染は速度を書かない «状態の伝播» で、物理ステップに同期させる理由が無い。
//   固定ステップに置くと、ステップが 1 フレームに複数回走る負荷時に伝染だけが倍速になる。
inline void EnemySerpentComponent::OnEnemyUpdate()
{
    // 放電は倒れた後も残り時間ぶんは走らせる。移した瞬間に撃破されたときだけ
    // 線が出ないのは、プレイヤーから見ると «移らなかった» と区別が付かない。
    UpdateConductArcs(Time::deltaTime);

    if (!conduct || !IsAlive()) return;

    m_conductTimer -= Time::deltaTime;
    if (m_conductTimer > 0.0f) return;
    m_conductTimer = std::max(conductInterval, 0.05f);

    m_conductedTotal += Conduct();
    debugConducted = m_conductedTotal;
}

inline void EnemySerpentComponent::OnEnemyDisable()
{
    for (ConductLink& link : m_links) {
        link.remaining = 0.0f;
        link.arc.Extinguish(*this);
    }
}

inline void EnemySerpentComponent::OnDestroy()
{
    for (ConductLink& link : m_links) link.arc.Detach(*this);
    m_links.clear();
}

inline int EnemySerpentComponent::Conduct()
{
    const auto* self = scene.GetScript<PolarityTargetComponent>();
    if (!self || !self->IsCharged()) return 0;

    GameObject* selfObject = scene.Self();
    if (!selfObject) return 0;

    const Polarity mine   = self->Current();
    const Vector3  center = bodybounds::CenterWorld(*selfObject, 1.0f);

    int moved = 0;
    for (GameObject* other : physics.OverlapSphere(center, std::max(conductRadius, 0.01f))) {
        if (moved >= std::max(conductMaxPerTick, 1)) break;
        if (!other || other == selfObject || !other->activeInHierarchy()) continue;

        auto* target = scene.GetScript<PolarityTargetComponent>(other);
        // 無極だけが対象。同極は延長にしかならず、逆極は中和になってしまう。
        if (!target || target->Current() != Polarity::None) continue;

        (void)target->Apply(mine);
        BeginConductArc(*other, mine);
        ++moved;
    }
    return moved;
}

inline void EnemySerpentComponent::BeginConductArc(GameObject& target, Polarity polarity)
{
    const GameObject* self  = scene.Self();
    const auto        index = self ? self->GetID().index : 0u;

    // 枠は 1 回の伝染で張れる本数まで。減らしても畳まないのは、鳴っている最中の
    // 1 本を «設定を下げた瞬間に» 消さないため。余った枠は使われないだけで済む。
    while (static_cast<int>(m_links.size()) < std::clamp(conductMaxPerTick, 1, 16)) {
        ConductLink link;
        // 鍵は個体と枠で一意にする。取り違えると 2 本の放電が同じ筋を奪い合う。
        link.arc.SetKey("Serpent" + std::to_string(index) + "_"
                        + std::to_string(m_links.size()));
        m_links.push_back(std::move(link));
    }
    if (m_links.empty()) return;

    ConductLink* slot = &m_links.front();
    for (ConductLink& link : m_links)
        if (link.remaining < slot->remaining) slot = &link;

    slot->target    = EntityRef{ target.GetID() };
    slot->remaining = std::max(conductArcSeconds, 0.05f);
    slot->polarity  = polarity;
}

inline void EnemySerpentComponent::UpdateConductArcs(float dt)
{
    GameObject* selfObject = scene.Self();

    for (ConductLink& link : m_links) {
        if (link.remaining <= 0.0f) continue;
        link.remaining -= dt;

        GameObject* target = link.target.Resolve(scene);
        if (!selfObject || !target || link.remaining <= 0.0f) {
            link.remaining = 0.0f;
            link.arc.Extinguish(*this);
            continue;
        }

        const float fade = Clamp01(link.remaining / std::max(conductArcSeconds, 0.05f));

        ElectricArcStyle style;
        style.strandCount = 2;
        style.segments    = 14;
        style.amplitude   = 0.26f;
        style.width       = 0.07f;
        style.strikeRate  = 30.0f;
        // 距離で消さない。伝染は触れている相手にしか起きないので «届かなかった放電»
        // というものが無い。消えるのは時間だけで決める。
        style.strikeRange = 0.0f;
        style.intensity   = 2.4f * fade;
        style.coreTint    = 0.7f;
        // 両端とも同じ極。渡した先が何色になったのかを線そのものが名乗る。
        style.fromColor   = PolarityColor(link.polarity);
        style.toColor     = style.fromColor;

        link.arc.Update(*this, bodybounds::CenterWorld(*selfObject, 1.0f),
                        bodybounds::CenterWorld(*target, 1.0f), style, dt);
    }
}

inline void EnemySerpentComponent::OnFixedUpdate()
{
    const float dt = time.FixedDeltaTime();

    if (IsPolarityDriven()) {
        // 振りかけの噛みつきは捨てる。Animator は Attack ステートを exitTime で抜けており、
        // 残したまま再開すると «モーションが無いのに牙が届く» になる。
        debugState    = "Polarity";
        m_attackTimer = 0.0f;
        return;
    }

    if (!IsAlive()) {
        debugState    = "Dead";
        m_attackTimer = 0.0f;
        StopHorizontal();
        return;
    }

    if (m_attackTimer > 0.0f) {
        TickAttack(dt);
        return;
    }

    if (IsMovementLocked()) {
        debugState = "Locked";
        StopHorizontal();
        return;
    }

    GameObject* player = Player();
    if (!player) {
        debugState = "No Player";
        StopHorizontal();
        return;
    }

    Vector3 direction = player->transform.worldPosition - transform.worldPosition;
    direction.y = 0.0f;
    const float distanceSq = direction.LengthSq();
    if (distanceSq < EPSILON) {
        debugState = "Overlap";
        StopHorizontal();
        return;
    }

    direction = direction.Normalized();
    FaceDirection(direction, dt);

    if (distanceSq <= attackRange * attackRange) {
        StopHorizontal();
        if (!AttackReady()) {
            debugState = "Cooldown";
            return;
        }
        debugState     = "Bite";
        m_attackTimer  = std::max(attackDuration, 0.05f);
        m_attackLanded = false;
        BeginAttackCooldown();
        animator.SetTrigger(enemyanim::kAttack);
        // 鎌首をもたげる音。牙が届くのは attackHitTime 後なので、ここは «来る» の予告。
        se::Play(audio, se::kSerpentRear);
        return;
    }

    Vector3 velocity = physics.GetVelocity();
    velocity.x = direction.x * std::max(moveSpeed, 0.0f);
    velocity.z = direction.z * std::max(moveSpeed, 0.0f);
    physics.SetVelocity(velocity);
    debugState = "Crawling";
}

inline void EnemySerpentComponent::TickAttack(float dt)
{
    debugState = "Bite";

    const float total   = std::max(attackDuration, 0.05f);
    const float elapsed = total - m_attackTimer;
    m_attackTimer -= dt;

    GameObject* player = Player();
    if (player && elapsed < attackHitTime) {
        Vector3 toPlayer = player->transform.worldPosition - transform.worldPosition;
        toPlayer.y = 0.0f;
        FaceDirection(toPlayer, dt);

        // 牙が届くまでは鎌首を伸ばしながら詰める。1.5 秒をその場で振り切る攻撃は、
        // 1 歩下がられた時点で «当たらないのに硬直だけ長い» になり、間合いの
        // 読み合いが «近づかない» の一手に潰れる。届いた後は伸び切って止まる。
        const Vector3 forward  = toPlayer.NormalizedOr(Vector3::ZERO);
        Vector3       velocity = physics.GetVelocity();
        velocity.x = forward.x * std::max(lungeSpeed, 0.0f);
        velocity.z = forward.z * std::max(lungeSpeed, 0.0f);
        physics.SetVelocity(velocity);
    } else {
        StopHorizontal();
    }

    if (!m_attackLanded && elapsed >= attackHitTime) {
        m_attackLanded = true;
        // 避けられていても顎は閉じる。当たったときだけ鳴らすと «空振り» が無音になり、
        // 何が起きて助かったのかが分からない。
        se::Play(audio, se::kSerpentBite);
        if (player) {
            Vector3 toPlayer = player->transform.worldPosition - transform.worldPosition;
            toPlayer.y = 0.0f;
            if (toPlayer.LengthSq() <= attackHitRange * attackHitRange)
                (void)HitPlayer(player);
        }
    }

    if (m_attackTimer <= 0.0f) m_attackTimer = 0.0f;
}

} // namespace sandbox
