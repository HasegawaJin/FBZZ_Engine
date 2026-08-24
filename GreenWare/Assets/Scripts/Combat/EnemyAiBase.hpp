/// @file EnemyAiBase.hpp
/// @brief 敵 AI が共通で持つ「標的・停止条件・接触攻撃」の土台
/// @author Hasegawa Jin
/// @date 2026-08-23
///
/// WHY 基底クラスにするか (インターフェースではなく):
///   IDamageable と違い、ここで共有したいのは宣言ではなく実装そのもの。標的の取り直し、
///   極性で引かれている間は自分から動かない判定、接触ダメージを CombatManager へ通す
///   経路は、Mite も Roller も Serpent も一字一句同じものが要る。宣言だけ配ると
///   4 箇所へ同じ中身が写り、そのうち 1 つだけ直され忘れる。
///
/// WHY OnStart / OnUpdate を final にするか:
///   共通の初期化と毎フレームの下ごしらえ (クールダウンの消化・標的の取り直し) を
///   基底が持ちつつ、派生にも初期化を書かせたい。素直に virtual のまま override させると
///   派生が EnemyAiBase::OnStart() を呼び忘れた瞬間に、標的が永久に nullptr のまま
///   「なぜか動かない敵」ができる。呼び忘れが起こらないよう入口を閉じて、
///   派生には OnEnemyStart / OnEnemyUpdate という別の穴を開ける。
#pragma once

#include <Engine/Scene/Components/RigidBodyComponent.hpp>
#include <Engine/Scene/Script.hpp>
#include <Scripts/Combat/CharacterEvent.hpp>
#include <Scripts/Combat/EnemyHealthComponent.hpp>
#include <Scripts/Game/CombatManagerComponent.hpp>
#include <Scripts/Polarity/PolarityBodyComponent.hpp>
#include <Scripts/Polarity/PolarityTargetComponent.hpp>
#include <algorithm>
#include <cmath>
#include <string>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

/// 敵 3 種の .animcontroller が共有するパラメーター名。
///
/// WHY 定数にするか: 綴りを間違えても AnimatorComponent::SetFloat は黙って何もしない。
///     「アニメーションだけ動かない」という、コードを読んでも原因が出ない壊れ方をする。
///     .animcontroller 側の綴りと突き合わせる場所を 1 箇所に閉じる。
namespace enemyanim {
inline constexpr const char* kSpeed  = "Speed";  ///< 水平速度 (m/s)。Idle ⇄ Move のブレンド
inline constexpr const char* kAttack = "Attack"; ///< 攻撃モーションの発火
inline constexpr const char* kHit    = "Hit";    ///< 加算レイヤーの被弾リアクション
inline constexpr const char* kIsDead = "IsDead"; ///< 死亡ステートへの分岐
} // namespace enemyanim

class EnemyAiBase : public Script {
    FBZZ_SCRIPT_BASE(EnemyAiBase, Script)
    // 引力運動に必要な剛体。派生では書き直さないこと (基底の宣言を上書きしてしまう)。
    FBZZ_REQUIRE_COMPONENT(RigidBodyComponent)

public:
    FBZZ_GROUP("Movement")
    FBZZ_FIELD_RANGE(float, moveSpeed, 3.2f, "Move Speed", 0.0f, 20.0f)
    FBZZ_FIELD_RANGE(float, turnSpeed, 10.0f, "Turn Speed", 0.0f, 30.0f)

    FBZZ_GROUP("Attack")
    FBZZ_FIELD_RANGE_INT(int, attackDamage, 1, "Attack Damage", 1, 100)
    FBZZ_FIELD_RANGE(float, attackCooldown, 1.0f, "Attack Cooldown", 0.05f, 10.0f)

    // WHY 追いかける条件と分けるか:
    //   3 種とも「見えたら追う」であって、索敵範囲は動きの条件ではない。ここを追跡の
    //   スイッチにすると、表情のために付けた数値が敵の強さを変えることになる。
    //   持たせるのは「気づいたことを知らせる」だけで、動きは派生の判断のまま置く。
    FBZZ_GROUP("Awareness")
    FBZZ_FIELD_RANGE(float, spotDistance, 14.0f, "Spot Distance", 0.0f, 60.0f)
    FBZZ_TOOLTIP("プレイヤーがこの距離へ入ったら「見つけた」と知らせる。動きは変わらない")
    FBZZ_FIELD_RANGE(float, spotRelease, 4.0f, "Spot Release", 0.0f, 30.0f)
    FBZZ_TOOLTIP("見失うのは Spot Distance + この距離。同じ距離で切り替えると境目で反応が連打される")

    FBZZ_GROUP("Target")
    FBZZ_FIELD(std::string, playerTag, "Player", "Player Tag")

    FBZZ_GROUP("Debug")
    FBZZ_FIELD_READ_ONLY(std::string, debugState, "Idle", "State")
    FBZZ_FIELD_READ_ONLY(int, debugSpotted, 0, "Spotted")

    void OnStart()  final;
    void OnUpdate() final;

protected:
    /// 派生の初期化。基底の初期化が済んだ後に呼ばれる。
    virtual void OnEnemyStart() {}
    /// 派生の毎フレーム処理。クールダウンの消化と標的の取り直しが済んだ後に呼ばれる。
    virtual void OnEnemyUpdate() {}

    /// 現在の標的。見失っていれば nullptr。
    [[nodiscard]] GameObject* Player() const { return m_player.Resolve(scene); }

    /// 自分から動いてはいけない状態か (死亡・引力・気絶・被弾硬直)。
    [[nodiscard]] bool IsMovementLocked() const;

    /// 極性に動かされている最中か。true の間は速度を一切書いてはいけない。
    ///
    /// WHY 分けて持つか: PolarityBodyComponent は引力・溜め・ノックバックを速度そのもので
    ///     表現していて、AI は同じ固定ステップの *後* に走る。ここで水平成分を 0 に戻すと
    ///     引力が書いた値が毎回消え、敵はその場で震えるだけになる。AI がすべきなのは
    ///     「自分から動かないこと」であって、他人が書いた速度を打ち消すことではない。
    [[nodiscard]] bool IsPolarityDriven() const;

    [[nodiscard]] bool AttackReady() const { return m_attackRemaining <= 0.0f; }

    /// 生存しているか。EnemyHealth を持たない構成では true を返す (死んだ扱いにしない)。
    [[nodiscard]] bool IsAlive() const;

    /// 攻撃の入口。振り始めた時点でクールダウンを起こし、攻撃に出たことを知らせる。
    ///
    /// WHY 当てた瞬間ではないか: モーション付きの攻撃は「振り始め → 数フレーム後に当たる」で、
    ///     当たった時点で数え始めると空振りした攻撃のクールダウンが一切進まない。
    ///     プレイヤーが避け続けている間じゅう、敵が毎フレーム振り直す。
    ///
    /// WHY 通知もここへ乗せるか: 3 種とも振り始めに必ずここを通る。当たり判定の側へ
    ///     置くと、避けられた攻撃だけ «出していないこと» になる。
    void BeginAttackCooldown();

    /// 自分の身に起きたことを CombatManager へ渡す。表情などの反応はあちらが配る。
    void Announce(CharacterEvent event) const;

    /// 水平速度だけを 0 にする。落下は殺さない。
    void StopHorizontal() const;

    /// direction の向きへ turnSpeed で滑らかに向き直る。y 成分は無視する。
    void FaceDirection(const Vector3& direction, float dt) const;

    /// プレイヤーへダメージを入れる。クールダウンには触らない。
    ///
    /// WHY 直接叩かず CombatManager を通すか: 極性衝突のダメージはあちらを通っているので、
    ///     ここだけ PlayerComponent を直接呼ぶと経路が 2 本になる。被ダメージの集計も
    ///     無敵時間の扱いも片方にしか乗らなくなる。
    bool HitPlayer(GameObject* player);

    /// 接触攻撃。クールダウンが空いていれば当て、入ったらクールダウンを開始する。
    /// モーションの途中で当てる敵は BeginAttackCooldown() + HitPlayer() を使うこと。
    bool TryDamagePlayer(GameObject* player)
    {
        if (!AttackReady() || !HitPlayer(player)) return false;
        BeginAttackCooldown();
        return true;
    }

    EntityRef m_player;
    float     m_attackRemaining = 0.0f;

private:
    void RefreshPlayer();
    /// Speed / Hit / IsDead を毎フレーム流す。Attack だけは派生が振り始めに送る。
    void DriveAnimator();
    /// 索敵範囲の出入りを見て、変わった瞬間だけ知らせる。
    void UpdateAwareness();

    bool  m_warnedNoCombat  = false;
    bool  m_wasHitReacting  = false;
    bool  m_spotted         = false;
};

FBZZ_REFLECT(EnemyAiBase)


inline void EnemyAiBase::OnStart()
{
    m_attackRemaining = 0.0f;
    m_warnedNoCombat  = false;
    m_wasHitReacting  = false;
    m_spotted         = false;
    debugSpotted      = 0;
    RefreshPlayer();

    // 極性を持てない敵は盤面のルールから外れる。7 章の衝突が一切起きないので、
    // 「なぜかこの個体だけ倒せない」という形でしか症状が出ない。
    const auto* target = scene.GetScript<PolarityTargetComponent>();
    if (!target || !scene.GetScript<EnemyHealthComponent>()) {
        debug.LogError("EnemyAiBase requires PolarityTarget and EnemyHealth "
                       "scripts on the same object.");
    }
    // WHY PolarityBody をアンカーに求めないか: 7.3 の「動くのは軽い側だけ」は
    //     «このスクリプトが付いていないこと自体が動かない側の宣言» として表現されている
    //     (PolarityBodyComponent の設計意図)。Roller は的なので付けないのが正しく、
    //     ここで一律に要求すると正しい構成が毎回エラーを吐くことになる。
    else if (!target->isAnchor && !scene.GetScript<PolarityBodyComponent>()) {
        debug.LogError("EnemyAiBase: this enemy is not an anchor but has no "
                       "PolarityBodyComponent. It can never be pulled.");
    }

    OnEnemyStart();
}

inline void EnemyAiBase::OnUpdate()
{
    m_attackRemaining = std::max(0.0f, m_attackRemaining - Time::deltaTime);
    RefreshPlayer();
    UpdateAwareness();
    DriveAnimator();
    OnEnemyUpdate();
}

inline void EnemyAiBase::Announce(CharacterEvent event) const
{
    if (auto* combat = CombatManagerComponent::Instance())
        combat->Notify(scene.Self(), event);
}

inline void EnemyAiBase::BeginAttackCooldown()
{
    m_attackRemaining = std::max(attackCooldown, 0.05f);
    Announce(CharacterEvent::Attack);
}

inline void EnemyAiBase::UpdateAwareness()
{
    GameObject* player  = Player();
    bool        spotted = false;

    if (player && IsAlive()) {
        const float distanceSq =
            (player->transform.worldPosition - transform.worldPosition).LengthSq();
        // 入るときと出るときで距離を変える。同じ距離だと、境目を歩かれるあいだ
        // 「見つけた」「見失った」が毎フレーム交互に飛ぶ。
        const float enter = std::max(spotDistance, 0.0f);
        const float leave = enter + std::max(spotRelease, 0.0f);
        const float edge  = m_spotted ? leave : enter;
        spotted = distanceSq <= edge * edge;
    }

    if (spotted == m_spotted) return;
    m_spotted    = spotted;
    debugSpotted = spotted ? 1 : 0;

    // 倒れた瞬間にも索敵は切れるが、そこから「落ち着いた」と知らせるのは嘘になる。
    // 受け手も倒れた後の Recovered は無視するが、送らないのが本筋。
    if (!IsAlive()) return;
    Announce(spotted ? CharacterEvent::Spotted : CharacterEvent::Recovered);
}

// Animator を持たない敵 (プリミティブのノーマルスライム) では proxy が全部空振りする。
// 派生ごとに «Animator が居るなら» と書き分けるより、無条件に流して黙って落とさせる方が短い。
inline void EnemyAiBase::DriveAnimator()
{
    // 引かれている / 弾かれている間の速度は PolarityBody のもので、この敵が自分で
    // 出した速さではない。そのまま流すと、飛ばされている 2 体が空中で全力疾走する。
    const Vector3 velocity = IsPolarityDriven() ? Vector3::ZERO : physics.GetVelocity();
    animator.SetFloat(enemyanim::kSpeed,
                      Vector3{ velocity.x, 0.0f, velocity.z }.Length());

    // 塗られた瞬間の 1 フレームだけ加算レイヤーへ送る。IsHitReacting は数フレーム
    // true が続くので、立ち上がりを見ないと同じ被弾で何度も撃ち直すことになる。
    const auto* target   = scene.GetScript<PolarityTargetComponent>();
    const bool  reacting = target && target->IsHitReacting();
    if (reacting && !m_wasHitReacting) animator.SetTrigger(enemyanim::kHit);
    m_wasHitReacting = reacting;

    if (const auto* health = scene.GetScript<EnemyHealthComponent>())
        animator.SetBool(enemyanim::kIsDead, !health->IsAlive());
}

inline bool EnemyAiBase::IsAlive() const
{
    const auto* health = scene.GetScript<EnemyHealthComponent>();
    return !health || health->IsAlive();
}

inline void EnemyAiBase::RefreshPlayer()
{
    // 取り直しを毎フレーム試す。プレイヤーが作り直される構成 (リスポーン・プール) でも
    // 参照が切れっぱなしにならない。
    if (m_player.Resolve(scene)) return;
    if (GameObject* player = scene.FindWithTag(playerTag))
        m_player = EntityRef{ player->GetID() };
}

inline bool EnemyAiBase::IsPolarityDriven() const
{
    const auto* body = scene.GetScript<PolarityBodyComponent>();
    return body && (body->IsBeingPulled() || body->IsStunned());
}

inline bool EnemyAiBase::IsMovementLocked() const
{
    const auto* health = scene.GetScript<EnemyHealthComponent>();
    const auto* target = scene.GetScript<PolarityTargetComponent>();
    return !health || !health->IsAlive() ||
           IsPolarityDriven() ||
           (target && target->IsHitReacting());
}

inline void EnemyAiBase::StopHorizontal() const
{
    Vector3 velocity = physics.GetVelocity();
    velocity.x = 0.0f;
    velocity.z = 0.0f;
    physics.SetVelocity(velocity);
}

inline void EnemyAiBase::FaceDirection(const Vector3& direction, float dt) const
{
    Vector3 flat = direction;
    flat.y = 0.0f;
    if (flat.LengthSq() < EPSILON) return;

    auto* rb = scene.GetComponent<RigidBodyComponent>();
    if (!rb || !rb->rigidBody) return;

    const float res = 1.0f - std::exp(-std::max(turnSpeed, 0.0f) * dt);
    rb->rigidBody->SetRotation(Quaternion::Slerp(
        rb->rigidBody->GetRotation(),
        Quaternion::LookRotation(flat.Normalized()), res).Normalized());
}

inline bool EnemyAiBase::HitPlayer(GameObject* player)
{
    if (!player) return false;

    auto* combat = CombatManagerComponent::Instance();
    if (!combat) {
        if (!m_warnedNoCombat) {
            m_warnedNoCombat = true;
            // 黙って直接叩くと経路が 2 本に戻る。落ちていることを見えるようにする。
            debug.LogError("EnemyAiBase found no CombatManagerComponent in the scene. "
                           "Contact attacks deal no damage.");
        }
        return false;
    }

    return combat->DamagePlayer(player, std::max(attackDamage, 1));
}

} // namespace sandbox
