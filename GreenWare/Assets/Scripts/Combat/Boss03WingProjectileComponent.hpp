/// @file    Boss03WingProjectileComponent.hpp
/// @brief   投げられた翼の «一生»。飛ぶ → 床へ落ちる → 残る → ボスへ戻る
/// @author  Hasegawa Jin
/// @date    2026-09-15
///
/// 生成するのは `Boss03AiComponent::ThrowWing`。こちらが持つのは翼 1 枚の一生だけで、
/// «どの翼を抜いたか» と «戻ってきたら畳む» はボスの側が持つ (OnWingReturned)。
///
/// 飛ぶのは **翼オブジェクトそのもの**。投げる側がボスから切り離し、既存の
/// SkinnedMeshRendererを有効にしたまま、このスクリプトを翼へ載せる。
/// こちらは翼を運ぶだけで、見た目の組み立てには触らない。
///
/// WHY 剛体を使うか:
///   移動そのものはこのスクリプトが決めるが、TerrainColliderとの接触検知は物理へ
///   任せる。Gravityまで物理へ任せると翼ごとの演出を細かく制御できないため、重力は使わない。
///
/// WHY 当たりを剛体の接触で取らないか:
///   行きの当たりは «弾ける一撃» で、プレイヤーの弾き窓と噛み合う必要がある。
///   接触に任せると翼の形と速さで当たる瞬間がぶれる。当たりは距離で 1 回だけ見て
///   (BossPartDebrisComponent と同じ)、剛体には **運ぶことと止まること**だけを任せる。
///
/// WHY 飛んでいる間と戻る間はすり抜けるか:
///   ボスのすぐ横で押し合うと翼もボスも小刻みに震える。**床に落ちている間だけ固い**
///   ＝ そのときだけ遮蔽になる、という切り分けにする。
///
/// WHY 帰り道では当たらないか:
///   1 回の投擲で «行き» と «帰り» の 2 回当たると、どちらを弾けばよいのかが割れる。
///   このボスの読みは «連撃の何拍目か» なので、1 投 = 1 拍に保つ。
///   帰りは «翼が戻る ＝ 次の連撃が長くなる» という**盤面の情報**として見せる。
#pragma once

#include <Engine/Scene/Components/ColliderComponent.hpp>
#include <Engine/Scene/Components/RigidBodyComponent.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/EntityRef.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Math/Segment.hpp>
#include <Scripts/Combat/EnemyHealthComponent.hpp>
#include <Scripts/Utils/WeaponSockets.hpp>
#include <Scripts/Combat/PlayerHit.hpp>
#include <Scripts/Combat/BossPartComponent.hpp>
#include <Scripts/Game/CombatManagerComponent.hpp>
#include <Scripts/Game/VfxManagerComponent.hpp>
#include <Scripts/Utils/BladeColors.hpp>
#include <Scripts/Utils/SeLibrary.hpp>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <limits>
#include <memory>
#include <string>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

class Boss03WingProjectileComponent : public Script {
    FBZZ_SCRIPT(Boss03WingProjectileComponent)

public:
    FBZZ_GROUP("デバッグ")
    FBZZ_FIELD_READ_ONLY(std::string, debugState, "Idle", "状態")
    FBZZ_FIELD_READ_ONLY(int, debugWing, -1, "翼")
    FBZZ_FIELD_READ_ONLY(float, debugRest, 0.0f, "残り [秒]")

    /// 投げた側が 1 度だけ呼ぶ。ここから先はこのスクリプトが運ぶ。
    ///
    /// ⚠ ここで剛体へ触ってはいけない。呼ばれるのは `AddScript` の直後で context は
    ///   まだ結ばれていない (`SetContext` は `ScriptSystem` が後から呼ぶ)。
    ///   **同じ翼は何度でも投げ直されるので `OnStart` も当てにできない** ─ 2 回目は
    ///   もう始まっている。頼みを立てるだけにして、最初の `OnUpdate` で組む。
    ///
    /// @param owner    ボスのルート。戻り先であり、戻ったことを告げる相手
    /// @param wing     翼の番号 (Boss03Wing)
    /// @param target   狙う点 (ワールド)。ここを通り過ぎたら落下へ移る
    void Setup(const EntityRef& owner, int wing, const Vector3& target);

    /// 飛ぶ速さ [m/s]。プレイヤーの走り (10 m/s) より速くしないと、
    /// «走って避ける» が常に正解になって弾く理由が消える。
    float flightSpeed = 22.0f;
    /// 飛べる長さの上限 [秒]。狙いを外しても、これで必ず落下へ移る。
    float flightSeconds = 1.6f;
    /// 戻り切るときの速さ [m/s]。磁力で吸われるので、**そこまで加速して**到達する。
    float returnSpeed = 18.0f;
    /// 吸い寄せの加速度 [m/s^2]。大きいほど «引っ張られている» が強く出る。
    float recallAccel = 26.0f;
    /// 吸い寄せの «溜め» [秒]。抜ける前に震えて浮く。
    ///
    /// WHY 溜めを置くか: 床の翼がいきなり動くと «消えて湧いた» に見える。
    ///     震えて浮いてから引かれると、動かしているのがボスだと読める。
    float recallWindup = 0.45f;
    /// 溜めのあいだに浮く高さ [m]。
    float recallRise = 0.9f;
    /// 床に落ち着いてから残る尺 [秒]。
    float restSeconds = 6.0f;
    /// 当たり半径 [m]。行きだけ。
    float hitRadius = 1.4f;
    /// 当たったときのダメージ。弾ける一撃として出す。
    int damage = 3;
    /// 長軸まわりの回転 [度/秒]。
    float spinRate = 720.0f;
    std::string playerTag = "Player";

    /// これ未満の速さ [m/s] が `settleSeconds` 続いたら «床に落ち着いた»。
    float settleSpeed = 0.5f;
    float settleSeconds = 0.3f;
    FBZZ_FIELD_RANGE(float, fallAcceleration, 18.0f, "落下加速度 [m/s²]", 0.0f, 80.0f)
    FBZZ_FIELD_RANGE(float, maxFallSpeed, 9.0f, "落下速度上限 [m/s]", 0.0f, 40.0f)
    /// 坂や隙間で転がり続けても、この時間で落ち着いた扱いにする。
    ///
    /// WHY 要るか: これが無いと、傾いた所へ落ちた 1 枚が永久に «まだ落下中» のままで、
    ///     ボスの連撃が二度と元の長さへ戻らない。
    float settleTimeout = 4.0f;
    /// 翼の重さ [kg] と空気抵抗。
    float mass = 14.0f;
    float linearDrag = 0.15f;

    /// 戻り切ったときに自分を消すか。
    ///
    /// WHY 既定が false か: 運んでいるのは **ボスの翼そのもの**で、戻ったら繋ぎ直して
    ///     また使う。消してよいのは «持ち主が居なくなった» ときだけ。
    bool destroyOnReturn = false;

    /// 戻り切った瞬間。«翼を畳む» を結ぶのはボスの側
    /// (BossBreakComponent::onBreak と同じ形)。
    ///
    /// WHY ボスのスクリプトを名指しで引かないか: こちらがボスのヘッダーを include すると、
    ///     ボス側もこちらを include している以上、循環参照になる。
    ///     «戻った» は 1 つの出来事なので、口を 1 本渡せば足りる。
    std::function<void(int wing)> onReturned;
    std::function<void(int wing)> onParried;
    std::function<void(int wing)> onBroken;

    void OnUpdate() override;
    /// 崩し・繭の間は飛行中の攻撃を落とす。retire は撃破時の完全停止。
    void CancelAttack(bool retire = false);
    [[nodiscard]] bool IsAttacking() const { return m_pending || m_state == State::Fly; }
    [[nodiscard]] bool CanExecute() const
    {
        return m_state == State::Fall || m_state == State::Rest || m_state == State::Recall;
    }
    void OnCollisionEnter(const CollisionInfo& info) override;
    void OnTriggerEnter(const CollisionInfo& info) override;

private:
    /// 翼 1 枚の一生。投げ直されるたびに Fly から回り直す。
    enum class State { Idle, Fly, Fall, Rest, Recall, Return, Done };

    [[nodiscard]] GameObject* Player() const { return scene.FindWithTag(playerTag); }
    /// 戻り先。ボスが畳まれていれば nullptr。
    [[nodiscard]] GameObject* Owner() const { return m_owner.Resolve(scene); }
    void Enter(State next);
    /// 投げるたびに剛体とコライダーを起こす。2 回目以降は作らず起こし直すだけ。
    void EnsurePhysics();
    /// 翼のメッシュから箱の中心と大きさを解く。引けなければ false。
    ///
    /// WHY メッシュから採るか: 翼オブジェクトの原点はモデルの原点で、翼の実体は
    ///     そこから離れた所にある。原点に箱を置くと **翼から離れた空中で止まる**。
    [[nodiscard]] bool MeshBox(Vector3& center, Vector3& size) const;
    /// 床に落ちる物にするか、すり抜ける物にするか。
    void SetSolid(bool solid) const
    {
        if (auto* box = scene.GetComponent<BoxColliderComponent>()) box->SetTrigger(!solid);
    }
    /// 物理そのものを止める / 動かす。戻して繋ぎ直すときは止める ──
    /// ボスの子に戻った翼の姿勢を、物理が毎フレーム上書きしてしまう。
    void SetPhysicsActive(bool active) const
    {
        if (auto* rb = scene.GetComponent<RigidBodyComponent>()) rb->enabled = active;
        if (auto* box = scene.GetComponent<BoxColliderComponent>()) box->enabled = active;
    }
    [[nodiscard]] float Speed() const { return physics.GetVelocity().Length(); }
    /// 行きの当たりを 1 度だけ返す。弾かれたらその場で落とす。
    void ResolveHit();
    /// 落下へ移る。スクリプトで下降速度を作り、接地だけ物理へ任せる。
    void BeginFall();
    void HandleTerrainContact(const CollisionInfo& info);

    State     m_state  = State::Idle;
    EntityRef m_owner;
    int       m_wing   = -1;
    Vector3   m_target = {};
    Vector3   m_previousPosition = {};
    float     m_rest   = 0.0f;
    float     m_age    = 0.0f;
    /// 遅い状態が続いている長さ [秒]。跳ねる頂点の «一瞬遅い» を数えないための溜め。
    float     m_slowFor = 0.0f;
    bool      m_dealt  = false;
    bool      m_pending = false;
    bool      m_cancelPending = false;
    int       m_savedHealth = -1;
    Vector3 m_returnPrevious{};
    Vector3 m_returnPreviousGoal{};
    bool m_hasReturnPrevious = false;
    bool      m_terrainContacted = false;
    /// 吸い寄せの経過 [秒]。
    float     m_recall     = 0.0f;
    /// 戻りの今の速さ [m/s]。0 から returnSpeed まで上がる。
    float     m_returnSpeed = 0.0f;
    float     m_fallSpeed = 0.0f;
    bool      m_parried = false;
    bool      m_breakReported = false;
    /// 行きの向き。押し戻しの向きもこれで決める。
    Vector3   m_heading = Vector3::FORWARD;
};

FBZZ_REFLECT(Boss03WingProjectileComponent)


inline void Boss03WingProjectileComponent::Setup(const EntityRef& owner, int wing,
                                                 const Vector3& target)
{
    // 同じ翼を何度でも投げ直せるよう、state はここで全部初期化する
    // (翼そのものを運ぶので、このスクリプトは戻った後も付いたまま残る)。
    m_owner       = owner;
    m_wing        = wing;
    m_target      = target;
    m_dealt       = false;
    m_rest        = 0.0f;
    m_age         = 0.0f;
    m_slowFor     = 0.0f;
    m_recall      = 0.0f;
    m_returnSpeed = 0.0f;
    m_fallSpeed   = 0.0f;
    m_parried     = false;
    m_hasReturnPrevious = false;
    m_breakReported = false;
    m_terrainContacted = false;
    m_pending     = true;
    m_cancelPending = false;
    debugRest     = 0.0f;
    debugWing     = wing;
}

inline void Boss03WingProjectileComponent::Enter(State next)
{
    m_state = next;
    m_age   = 0.0f;
    switch (next) {
    case State::Idle:   debugState = "Idle";   break;
    case State::Fly:    debugState = "Fly";    break;
    case State::Fall:   debugState = "Fall";   break;
    case State::Rest:   debugState = "Rest";   break;
    case State::Recall: debugState = "Recall"; break;
    case State::Return: debugState = "Return"; break;
    case State::Done:   debugState = "Done";   break;
    }
}

inline bool Boss03WingProjectileComponent::MeshBox(Vector3& center, Vector3& size) const
{
    const auto* skin = scene.GetComponent<SkinnedMeshRenderer>();
    if (!skin || !skin->model) return false;

    Vector3 lo{ std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                std::numeric_limits<float>::max() };
    Vector3 hi{ -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(),
                -std::numeric_limits<float>::max() };

    bool any = false;
    for (std::size_t i = 0, count = skin->SubmeshCount(); i < count; ++i) {
        const auto* mesh = skin->SubmeshMesh(i);
        if (!mesh || mesh->boundsRadius <= 0.0f) continue;
        any = true;
        lo.x = std::min(lo.x, mesh->boundsCenter.x - mesh->boundsExtents.x);
        lo.y = std::min(lo.y, mesh->boundsCenter.y - mesh->boundsExtents.y);
        lo.z = std::min(lo.z, mesh->boundsCenter.z - mesh->boundsExtents.z);
        hi.x = std::max(hi.x, mesh->boundsCenter.x + mesh->boundsExtents.x);
        hi.y = std::max(hi.y, mesh->boundsCenter.y + mesh->boundsExtents.y);
        hi.z = std::max(hi.z, mesh->boundsCenter.z + mesh->boundsExtents.z);
    }
    if (!any) return false;

    center = (lo + hi) * 0.5f;
    size   = Vector3{ std::max(hi.x - lo.x, 0.2f),
                      std::max(hi.y - lo.y, 0.2f),
                      std::max(hi.z - lo.z, 0.2f) };
    return true;
}

inline void Boss03WingProjectileComponent::EnsurePhysics()
{
    GameObject* self = scene.Self();
    if (!self) return;

    // 引けないときの控え。翼 1 枚ぶんのおおよその大きさで、少なくとも床には当たる。
    Vector3 center = Vector3::ZERO;
    Vector3 size{ 1.0f, 0.4f, 2.4f };
    if (!MeshBox(center, size))
        debug.LogWarning("Boss03WingProjectileComponent: 翼のメッシュから箱が引けない。"
                         "仮の大きさで落とす。");

    auto* box = self->GetComponent<BoxColliderComponent>();
    if (!box) box = &self->AddComponent<BoxColliderComponent>();
    box->SetSize(size);
    box->center  = center;
    box->enabled = true;
    box->SetTrigger(true);   // 飛んでいる間はすり抜ける。固くなるのは落下から

    const Vector3    pos = self->transform.worldPosition;
    const Quaternion rot = self->transform.worldRotation;

    auto* rb = self->GetComponent<RigidBodyComponent>();
    if (!rb) {
        RigidBodyComponent created{};
        created.rigidBody = std::make_unique<fbzz::physics::RigidBody>();
        self->AddComponent<RigidBodyComponent>(std::move(created));
        rb = self->GetComponent<RigidBodyComponent>();
    }
    if (!rb || !rb->rigidBody) return;

    rb->enabled = true;
    rb->rigidBody->SetMass(std::max(mass, 1.0f));
    rb->rigidBody->SetPosition(pos);
    rb->rigidBody->SetRotation(rot);
    rb->rigidBody->m_linearDrag  = std::max(linearDrag, 0.0f);
    rb->rigidBody->m_angularDrag = std::max(linearDrag, 0.0f) * 1.5f;
    // WHY CCD か: 22 m/s で飛ぶ翼は 1 step で 0.3m 進む。薄い側から当たると
    //     テレインを抜けて «床の下へ落ちていく» ことがある。
    rb->rigidBody->m_useCCD    = true;
    rb->rigidBody->m_ccdRadius = std::max(std::min(size.x, std::min(size.y, size.z)) * 0.5f, 0.2f);
    rb->ResetPhysicsSyncState(pos, rot);
}

inline void Boss03WingProjectileComponent::BeginFall()
{
    Enter(State::Fall);
    m_slowFor = 0.0f;
    m_fallSpeed = 0.0f;
    SetSolid(true);
    physics.SetGravityScale(0.0f);
    if (auto* part = scene.GetScript<BossPartComponent>()) {
        part->Restore();
        if (m_savedHealth > 0) part->Damage(std::max(part->maxHealth - m_savedHealth, 0));
    }
}

inline void Boss03WingProjectileComponent::HandleTerrainContact(const CollisionInfo& info)
{
    // 飛行中は翼の軌道が地形の上端へ触れても落とさない。ここで落とすと
    // Telegraph の手前で地面へ吸われ、攻撃の到達点が読めなくなる。
    // Fly から Fall への遷移は OnUpdate の到達判定だけが行う。
    if (m_state != State::Fall || !info.other ||
        !info.other->GetComponent<TerrainColliderComponent>()) return;

    m_terrainContacted = true;
    physics.SetVelocity(Vector3::ZERO);
    physics.SetAngularVelocity(Vector3::ZERO);
}

inline void Boss03WingProjectileComponent::OnCollisionEnter(const CollisionInfo& info)
{
    HandleTerrainContact(info);
}

inline void Boss03WingProjectileComponent::OnTriggerEnter(const CollisionInfo& info)
{
    HandleTerrainContact(info);
}

inline void Boss03WingProjectileComponent::ResolveHit()
{
    if (m_dealt) return;

    GameObject* player = Player();
    if (!player) return;

    const Vector3 at = transform.worldPosition;
    const Vector3 chest = player->transform.worldPosition + Vector3{ 0.0f, 1.2f, 0.0f };
    const Vector3 closest = ClosestPointOnSegment(chest, m_previousPosition, at);
    const float radius = std::max(hitRadius, 0.1f);
    if ((chest - closest).LengthSq() > radius * radius) return;

    m_dealt = true;

    auto* combat = CombatManagerComponent::Instance();
    if (!combat) return;

    // 押しは «翼が来た向き»。飛んできた物に弾かれる形が正しい。
    const Vector3 source = closest - m_heading;
    const PlayerHitResult result =
        combat->HitPlayer(player, std::max(damage, 0), &source, PlayerHitKind::Parryable);

    if (result == PlayerHitResult::Parried) {
        m_parried = true;
        SetSolid(false);
        physics.SetGravityScale(0.0f);
        m_returnSpeed = std::max(flightSpeed * 0.65f, 8.0f);
        Enter(State::Return);
        physics.SetVelocity(-m_heading * m_returnSpeed);
        return;
    }

    if (auto* vfx = VfxManagerComponent::Instance())
        vfx->PlayImpact(at, BladeSide::None, 0.7f, false);
    se::Play(audio, se::kImpactMid, 0.8f);
}

inline void Boss03WingProjectileComponent::CancelAttack(bool retire)
{
    if (retire) {
        m_pending = false;
        onReturned = {};
        onParried = {};
        onBroken = {};
        physics.SetVelocity(Vector3::ZERO);
        physics.SetAngularVelocity(Vector3::ZERO);
        SetPhysicsActive(false);
        Enter(State::Done);
        return;
    }
    if (m_pending) {
        m_cancelPending = true;
        return;
    }
    if (m_state == State::Fly) {
        m_dealt = true;
        BeginFall();
    }
}

inline void Boss03WingProjectileComponent::OnUpdate()
{
    const float dt = std::max(time.DeltaTime(), 0.0f);
    if (m_pending || (m_state != State::Idle && m_state != State::Done)) {
        GameObject* owner = Owner();
        const auto* health = owner ? owner->GetScript<EnemyHealthComponent>() : nullptr;
        if (!owner || (health && !health->IsAlive())) {
            CancelAttack(true);
            if (!owner && scene.Self()) scene.Destroy(*scene.Self());
            return;
        }
    }

    // 投げの頼みは最初の OnUpdate で叶える (Setup の ⚠ を参照)。
    if (m_pending) {
        m_pending = false;
        EnsurePhysics();
        m_previousPosition = transform.worldPosition;

        Vector3 heading = m_target - transform.worldPosition;
        m_heading = heading.NormalizedOr(Vector3::FORWARD);

        physics.SetGravityScale(0.0f);
        physics.SetVelocity(m_heading * std::max(flightSpeed, 1.0f));
        // 回転は進行方向まわりに。縦に回すと «飛んでいる» が輪郭で読めなくなる。
        physics.SetAngularVelocity(m_heading * ToRad(std::max(spinRate, 0.0f)));
        Enter(State::Fly);
        if (m_cancelPending) {
            m_cancelPending = false;
            m_dealt = true;
            BeginFall();
        }
    }

    if (dt <= 0.0f || m_state == State::Idle || m_state == State::Done) return;
    m_age += dt;

    if (CanExecute() && !m_breakReported) {
        if (auto* part = scene.GetScript<BossPartComponent>(); part && part->IsDepleted()) {
            m_breakReported = true;
            SetPhysicsActive(false);
            Enter(State::Done);
            const auto reportBroken = onBroken;
            if (reportBroken) reportBroken(m_wing);
            return;
        }
    }

    switch (m_state) {
    case State::Fly: {
        // 速さは毎フレーム押し直す。何かを擦って落ちた速度のまま «漂う» にしない。
        physics.SetVelocity(m_heading * std::max(flightSpeed, 1.0f));
        ResolveHit();
        m_previousPosition = transform.worldPosition;
        if (m_state != State::Fly) break;   // 弾かれて落下へ移った

        // 狙いを通り過ぎたら落とす。当たっても外しても、行き着く先は床。
        Vector3 toTarget = m_target - transform.worldPosition;
        toTarget.y = 0.0f;
        if (Vector3::Dot(toTarget, Vector3{ m_heading.x, 0.0f, m_heading.z }) <= 0.0f ||
            m_age >= std::max(flightSeconds, 0.1f))
            BeginFall();
        break;
    }

    case State::Fall: {
        if (m_terrainContacted) {
            physics.SetVelocity(Vector3::ZERO);
            physics.SetAngularVelocity(Vector3::ZERO);
        } else {
            m_fallSpeed = std::min(m_fallSpeed + std::max(fallAcceleration, 0.0f) * dt,
                                   std::max(maxFallSpeed, 0.0f));
            physics.SetVelocity(Vector3{ 0.0f, -m_fallSpeed, 0.0f });
        }
        // 跳ねている途中の «頂点で一瞬遅くなる» は数えない。
        m_slowFor = Speed() < std::max(settleSpeed, 0.0f) ? m_slowFor + dt : 0.0f;
        if (m_slowFor < std::max(settleSeconds, 0.0f) &&
            m_age < std::max(settleTimeout, 1.0f))
            break;

        const Vector3 at = transform.worldPosition;
        if (auto* vfx = VfxManagerComponent::Instance())
            vfx->PlayGroundDust(at, -m_heading, 0.6f, 0.9f);
        se::PlayAt(audio, se::kImpactDebris, at);
        se::PlayAt(audio, se::kBossStompSettle, at, 0.8f);
        m_rest = std::max(restSeconds, 0.0f);
        physics.SetVelocity(Vector3::ZERO);
        physics.SetAngularVelocity(Vector3::ZERO);
        Enter(State::Rest);
        break;
    }

    case State::Rest:
        m_rest -= dt;
        debugRest = std::max(m_rest, 0.0f);
        if (m_rest <= 0.0f) {
            // 吸い寄せの合図。盤面では «連撃がまた長くなる» の予告になる。
            se::PlayAt(audio, se::kAttractWindup, transform.worldPosition, 0.8f);
            if (auto* effects = VfxManagerComponent::Instance()) {
                const GameObject* owner = Owner();
                const Vector3 toward = owner
                    ? owner->transform.worldPosition + Vector3{ 0.0f, 2.0f, 0.0f } - transform.worldPosition
                    : Vector3::UP;
                effects->PlayWingRecall(transform.worldPosition, toward);
            }
            m_recall = 0.0f;
            SetSolid(false);
            physics.SetGravityScale(0.0f);
            Enter(State::Recall);
        }
        break;

    case State::Recall: {
        // 磁力に引かれて震え、浮き上がる。まだ進まない ─ «持っていかれる直前» を見せる。
        m_recall += dt;
        const float windup = std::max(recallWindup, 0.01f);
        const float t      = Clamp01(m_recall / windup);
        // 震えは高い周波数で、浮きは滑らかに。抵抗が抜けていく形にする。
        const float shake  = (1.0f - t) * 3.0f;
        physics.SetVelocity(
            Vector3{ std::sin(m_recall * 47.0f) * shake,
                     std::max(recallRise, 0.0f) / windup,
                     std::cos(m_recall * 39.0f) * shake });
        physics.SetAngularVelocity(Vector3::UP * ToRad(std::max(spinRate, 0.0f)) * t);

        if (m_recall >= windup) {
            if (auto* part = scene.GetScript<BossPartComponent>()) {
                m_savedHealth = part->Health();
                part->Break();
            }
            se::PlayAt(audio, se::kAttractLaunch, transform.worldPosition, 0.9f);
            m_returnSpeed = 0.0f;
            Enter(State::Return);
        }
        break;
    }

    case State::Return: {
        GameObject* owner = Owner();
        if (!owner) {
            // 戻り先が畳まれた (撃破・シーン遷移)。床に残しても誰も片付けない。
            Enter(State::Done);
            if (scene.Self()) scene.Destroy(*scene.Self());
            return;
        }

        Vector3 goal = owner->transform.worldPosition;
        goal.y += 2.0f;
        if (m_parried)
            if (GameObject* body = FindInSubtree(*owner, "Body")) goal = body->transform.worldPosition;
        const Vector3 current = transform.worldPosition;
        const Vector3 relativeFrom = m_hasReturnPrevious ? m_returnPrevious - m_returnPreviousGoal : current - goal;
        const bool arrived = ClosestPointOnSegment(Vector3::ZERO, relativeFrom, current - goal).LengthSq()
                           <= 1.5f * 1.5f;
        m_returnPrevious = current;
        m_returnPreviousGoal = goal;
        m_hasReturnPrevious = true;

        const Vector3 to       = goal - transform.worldPosition;
        const Vector3 dir      = to.NormalizedOr(m_heading);

        // 加速して吸い込まれる。等速で帰ると «飛んで戻った» で、磁力に見えない。
        m_returnSpeed = std::min(m_returnSpeed + std::max(recallAccel, 0.1f) * dt,
                                 std::max(returnSpeed, 0.5f));
        // 成功報酬の帰還まで遅くすると、スローを眺めるだけで反撃時間を失う。
        const float counterScale = m_parried && dt > 0.0f
            ? std::clamp(time.UnscaledDeltaTime() / dt, 1.0f, 8.0f) : 1.0f;
        const float speed = m_parried ? std::max(m_returnSpeed, 32.0f) * counterScale : m_returnSpeed;
        physics.SetVelocity(dir * speed);

        // 届かないまま時間切れでも繋ぎ直す。追いつけない翼を 1 枚でも残すと、
        // 連撃の長さが戻らないまま最後まで進む。
        if (arrived || m_age >= 8.0f) {
            Enter(State::Done);
            // 繋ぎ直す前に物理を止める。生かしたままだと、ボスの子へ戻した翼の姿勢を
            // WriteBackTransforms が毎フレーム床の座標へ引き戻す。
            physics.SetVelocity(Vector3::ZERO);
            physics.SetAngularVelocity(Vector3::ZERO);
            physics.SetGravityScale(0.0f);
            SetPhysicsActive(false);
            const auto reportParried = onParried;
            if (m_parried && arrived && reportParried) reportParried(m_wing);
            // 繋ぎ直すのはボスの仕事 ─ «翼が戻った» は進行の状態なので、持ち主が握る。
            const auto reportReturned = onReturned;
            if (reportReturned) reportReturned(m_wing);
            if (destroyOnReturn && scene.Self()) scene.Destroy(*scene.Self());
        }
        break;
    }

    case State::Idle:
    case State::Done:
        break;
    }
}

} // namespace sandbox
