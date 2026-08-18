// FBZZ Engine
// PlayerControllerComponent.hpp | sandbox
// RigidBody ベースのプレイヤー移動・ジャンプ・回避。
//
// WHY 入力と物理操作を分離するか:
//   可変フレームで入力を採取し、固定ステップで物理へ反映することで、
//   短いキー入力の取りこぼしとフレームレート依存を防ぐ。
//
// WHY 回避に無敵時間を付けないか:
//   11 章「ジャスト回避の判定と報酬は本バージョンでは実装しない」、19 章でも
//   追加候補として明示的に外している。ここでは純粋な移動アクションとして実装し、
//   判定と報酬は後から足せる形にしておく。
#pragma once

#include <Scripts/Data/PlayerTuning.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Components/CharacterControllerComponent.hpp>
#include <Engine/Scene/Components/IKSolverComponent.hpp>
#include <Engine/Scene/Components/RigidBodyComponent.hpp>
#include <Engine/Scene/Script.hpp>
#include <algorithm>
#include <cmath>
#include <string_view>

using namespace fbzz::scene;
using namespace fbzz::math;
using namespace fbzz::input;
using namespace fbzz::physics;
using fbzz::Time;

namespace sandbox {

// 親を差し替えても、その瞬間のワールド姿勢を維持する。
// WHY: SetParent だけではローカル座標がそのまま残り、銃が原点へ跳ぶため、Draw/Holster の
//      アニメーション途中で「現在位置から取り出す」という操作にならない。
inline bool ReparentKeepingWorld(GameObject& child, GameObject& parent)
{
    const Vector3 worldPosition = child.transform.worldPosition;
    const Quaternion worldRotation = child.transform.worldRotation;
    const Vector3 worldScale = child.transform.worldScale;
    const Quaternion inverseParentRotation = parent.transform.worldRotation.Inverse();
    const Vector3 localPosition = inverseParentRotation *
        (worldPosition - parent.transform.worldPosition);
    const Vector3 parentScale = parent.transform.worldScale;
    const auto divideByParentScale = [](float value, float scale) {
        return std::abs(scale) > 0.000001f ? value / scale : value;
    };

    if (!child.SetParent(&parent))
        return false;

    child.transform.position = {
        divideByParentScale(localPosition.x, parentScale.x),
        divideByParentScale(localPosition.y, parentScale.y),
        divideByParentScale(localPosition.z, parentScale.z),
    };
    child.transform.rotation =
        (inverseParentRotation * worldRotation).Normalized();
    child.transform.scale = {
        divideByParentScale(worldScale.x, parentScale.x),
        divideByParentScale(worldScale.y, parentScale.y),
        divideByParentScale(worldScale.z, parentScale.z),
    };
    return true;
}

class PlayerControllerComponent : public Script {
    FBZZ_SCRIPT(PlayerControllerComponent)

    // 接地判定と固定ステップの水平速度制御はこの 2 つが揃って初めて成立する。
    // どちらかが欠けると「入力は読めているのにキャラが動かない」無言の状態になる。
    FBZZ_REQUIRE_COMPONENT(CharacterControllerComponent, RigidBodyComponent)
    // 無くても移動そのものは成立する。Animator が無ければパラメーター送信が空振りし、
    // IKSolver が無ければ足の接地補正と上体の傾きが省かれるだけ。
    FBZZ_OPTIONAL_COMPONENT(AnimatorComponent, IKSolverComponent)

public:
    FBZZ_ASSET(PlayerTuning, tuning, "Tuning")
    FBZZ_TOOLTIP("移動・ジャンプ・回避の共有値。未割り当てなら下の既定値で動く")

    FBZZ_GROUP("Movement")
    FBZZ_FIELD_RANGE(float, moveSpeed,              6.0f,  "Move Speed",        0.1f, 20.0f)
    FBZZ_FIELD_RANGE(float, modelYawOffsetDegrees, 180.0f, "Model Yaw Offset",  0.0f, 360.0f)
    FBZZ_FIELD_RANGE(float, groundAccel,            18.0f, "Ground Accel",       1.0f, 100.0f)
    FBZZ_FIELD_RANGE(float, groundDecel,            22.0f, "Ground Decel",       1.0f, 100.0f)
    FBZZ_FIELD_RANGE(float, airAccel,                3.0f, "Air Accel",          0.0f,  50.0f)
    FBZZ_FIELD_RANGE(float, turnSpeed,              14.0f, "Turn Speed",         0.1f,  30.0f)
    FBZZ_FIELD_RANGE(float, jumpSpeed,               7.0f, "Jump Speed",          0.1f,  30.0f)
    FBZZ_FIELD(bool, useCameraForward,      true, "Use Camera Forward")
    FBZZ_FIELD(bool, rotateToMoveDirection, true, "Rotate To Move Dir")
    FBZZ_FIELD(bool, useFootIK,             true, "Use Foot IK")

    FBZZ_GROUP("Dodge")
    FBZZ_FIELD_RANGE(float, dodgeSpeed,    16.0f, "Dodge Speed",    1.0f, 60.0f)
    FBZZ_FIELD_RANGE(float, dodgeDuration, 0.22f, "Dodge Duration", 0.05f, 1.0f)
    FBZZ_FIELD_RANGE(float, dodgeCooldown,  0.8f, "Dodge Cooldown", 0.0f,  5.0f)
    FBZZ_TOOLTIP("無敵時間は持たない (11 章 / 19 章がジャスト回避を本バージョンから外しているため)")

    FBZZ_GROUP("Key Bindings")
    FBZZ_FIELD(KeyCode, keyForward,  KeyCode::W,     "Forward")
    FBZZ_FIELD(KeyCode, keyBackward, KeyCode::S,     "Backward")
    FBZZ_FIELD(KeyCode, keyLeft,     KeyCode::A,     "Left")
    FBZZ_FIELD(KeyCode, keyRight,    KeyCode::D,     "Right")
    FBZZ_FIELD(KeyCode, keyJump,     KeyCode::SPACE, "Jump")
    FBZZ_FIELD(KeyCode, keyDodge,    KeyCode::SHIFT, "Dodge")

    FBZZ_GROUP("Animator Params")
    FBZZ_FIELD(std::string, paramSpeed,         "Speed",        "Speed Param")
    FBZZ_FIELD(std::string, paramVerticalSpeed, "VerticalSpeed","Vertical Speed Param")
    FBZZ_FIELD(std::string, paramIsGrounded,    "IsGrounded",   "IsGrounded Param")
    FBZZ_FIELD(std::string, paramJumpTrigger,   "Jump",         "Jump Trigger Param")
    FBZZ_FIELD(std::string, paramDodgeTrigger,  "Dodge",        "Dodge Trigger Param")

    FBZZ_GROUP("Weapon Animation")
    // Draw / Holster は入力とアニメーションの責務を分け、銃の親だけをこのスクリプトで切り替える。
    FBZZ_FIELD(KeyCode, keyDraw,    KeyCode::G, "Draw Weapons")
    FBZZ_FIELD(KeyCode, keyHolster, KeyCode::H, "Holster Weapons")
    FBZZ_FIELD(std::string, paramDrawTrigger,    "Draw",          "Draw Trigger Param")
    FBZZ_FIELD(std::string, paramHolsterTrigger, "Holster",       "Holster Trigger Param")
    FBZZ_FIELD(std::string, weaponLayerName,      "Layer 1",       "Weapon Layer")
    FBZZ_FIELD(std::string, drawStateName,        "DrawPistols",   "Draw State")
    FBZZ_FIELD(std::string, holsterStateName,     "HolsterPistols","Holster State")
    FBZZ_FIELD_RANGE(float, drawAttachNormalizedTime,    0.60f, "Draw Attach Time",    0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, holsterDetachNormalizedTime, 0.60f, "Holster Attach Time", 0.0f, 1.0f)

    // 参照が保存されていない旧シーンでも名前で復旧できるよう、GUID参照と既定名を併用する。
    FBZZ_REF(GameObject, weaponLeft,     "Left Pistol")
    FBZZ_REF(GameObject, weaponRight,    "Right Pistol")
    FBZZ_REF(GameObject, holsterLeft,    "Left Holster Socket")
    FBZZ_REF(GameObject, holsterRight,   "Right Holster Socket")
    FBZZ_REF(GameObject, handSocketLeft, "Left Hand Socket")
    FBZZ_REF(GameObject, handSocketRight,"Right Hand Socket")

    // 回避中か。被弾処理や敵 AI が「今は掴めない」を判断するのに使える。
    [[nodiscard]] bool  IsDodging() const { return m_dodgeRemaining > 0.0f; }
    // UI 用。1 = 回避可能 / 0 = 使った直後。
    [[nodiscard]] float DodgeCharge() const;

    void OnStart() override;
    void OnUpdate() override;
    void OnFixedUpdate() override;

private:
    // 回避の開始判定と、回避中の速度上書き。
    void TickDodge(const Vector3& moveDirection, bool hasInput,
                   bool dodgeRequested, RigidBody& phy, float dt);
    void StartWeaponAction(bool draw);
    void UpdateWeaponAction(float dt);
    void AttachWeaponsToHands();
    void AttachWeaponsToHolsters();
    GameObject* ResolveObject(const Ref<GameObject>& reference,
                              std::string_view fallbackName) const;
    void UpdateIK();
    void UpdateSlopeLean(IKSolverComponent& ik, CharacterControllerComponent* cc,
                         RigidBodyComponent* rb);
    Vector3 GetMoveForward() const;
    Vector3 GetMoveRight(const Vector3& forward) const;

    // 値は DataAsset があればそちらを、無ければ自分のフィールドを使う。
    // WHY 両方持つか: アセットを作る前でも触って動かせる状態を保ちたい。
    //      DataAsset を割り当てた瞬間に共有値へ切り替わる。
    [[nodiscard]] float MoveSpeed()     const { return tuning ? tuning->moveSpeed     : moveSpeed; }
    [[nodiscard]] float GroundAccel()   const { return tuning ? tuning->groundAccel   : groundAccel; }
    [[nodiscard]] float GroundDecel()   const { return tuning ? tuning->groundDecel   : groundDecel; }
    [[nodiscard]] float TurnSpeed()     const { return tuning ? tuning->turnSpeed     : turnSpeed; }
    [[nodiscard]] float JumpSpeed()     const { return tuning ? tuning->jumpSpeed     : jumpSpeed; }
    [[nodiscard]] float DodgeSpeed()    const { return tuning ? tuning->dodgeSpeed    : dodgeSpeed; }
    [[nodiscard]] float DodgeDuration() const { return tuning ? tuning->dodgeDuration : dodgeDuration; }
    [[nodiscard]] float DodgeCooldown() const { return tuning ? tuning->dodgeCooldown : dodgeCooldown; }

    bool m_hasSpineTargetBase = false;
    Vector3 m_spineTargetBase = Vector3::ZERO;
    Vector3 m_smoothedSpineOffset = Vector3::ZERO;
    // 回避の残り時間と、次に回避できるまでの残り時間。
    float   m_dodgeRemaining = 0.0f;
    float   m_dodgeCooldown  = 0.0f;
    Vector3 m_dodgeDirection = Vector3::ZERO;
    // 可変フレームで採取した入力を、次の固定ステップで一度だけ物理へ適用する。
    Vector3 m_moveDirection = Vector3::ZERO;
    bool    m_hasMoveInput = false;
    bool    m_jumpRequested = false;
    bool    m_dodgeRequested = false;

    enum class WeaponAction { None, Draw, Holster };
    WeaponAction m_weaponAction = WeaponAction::None;
    float m_weaponActionTime = 0.0f;
    bool m_weaponActionSwitched = false;
    bool m_gunsDrawn = false;
};

// Reflect() をフィールド宣言から自動生成する (旧 .generated.hpp は廃止)。
FBZZ_REFLECT(PlayerControllerComponent)

// ── 実装 (inline) ─────────────────────────────────────────────────────────────
inline void PlayerControllerComponent::OnStart()
{
    // WHY: 接触摩擦トルクによるカプセル傾きで水平ジッターが発生するため全軸フリーズ。
    physics.SetFreezeRotation(true, true, true);
    m_dodgeRemaining = 0.0f;
    m_dodgeCooldown  = 0.0f;
    m_moveDirection  = Vector3::ZERO;
    m_hasMoveInput   = false;
    m_jumpRequested  = false;
    m_dodgeRequested = false;
    m_weaponAction = WeaponAction::None;
    m_weaponActionTime = 0.0f;
    m_weaponActionSwitched = false;
    m_gunsDrawn = false;
    AttachWeaponsToHolsters();
}

inline void PlayerControllerComponent::OnUpdate()
{
    if (!transform) return;
    const Vector3 forward = GetMoveForward();
    const Vector3 right   = GetMoveRight(forward);
    Vector3 move = Vector3::ZERO;
    if (input.GetKey(keyForward))  move += forward;
    if (input.GetKey(keyBackward)) move -= forward;
    if (input.GetKey(keyRight))    move += right;
    if (input.GetKey(keyLeft))     move -= right;

    m_hasMoveInput = move.LengthSq() > EPSILON;
    m_moveDirection = m_hasMoveInput ? move.Normalized() : Vector3::ZERO;
    m_jumpRequested = m_jumpRequested || input.GetKeyDown(keyJump);
    m_dodgeRequested = m_dodgeRequested || input.GetKeyDown(keyDodge);
    if (input.GetKeyDown(keyDraw))    StartWeaponAction(true);
    if (input.GetKeyDown(keyHolster)) StartWeaponAction(false);

    auto* cc = scene.GetComponent<CharacterControllerComponent>();
    auto* rb = scene.GetComponent<RigidBodyComponent>();
    auto* phy = rb && rb->enabled && rb->rigidBody ? rb->rigidBody.get() : nullptr;
    if (cc) {
        animator.SetFloat(paramVerticalSpeed, cc->verticalSpeed);
        animator.SetBool(paramIsGrounded, cc->isGrounded);
    }
    const Vector3 velocity = phy ? phy->GetVelocity() : Vector3::ZERO;
    animator.SetFloat(paramSpeed, IsDodging()
        ? DodgeSpeed()
        : std::sqrtf(velocity.x * velocity.x + velocity.z * velocity.z));
    UpdateWeaponAction(std::max(Time::deltaTime, 0.0f));
    UpdateIK();
}

inline GameObject* PlayerControllerComponent::ResolveObject(
    const Ref<GameObject>& reference, std::string_view fallbackName) const
{
    if (GameObject* object = reference.Get())
        return object;
    return scene.Find(fallbackName);
}

inline void PlayerControllerComponent::StartWeaponAction(bool draw)
{
    auto* animatorComponent = scene.GetComponent<AnimatorComponent>();
    if (!animatorComponent)
        return;

    const WeaponAction requested = draw ? WeaponAction::Draw : WeaponAction::Holster;
    if (m_weaponAction == WeaponAction::None &&
        ((draw && m_gunsDrawn) || (!draw && !m_gunsDrawn)))
        return;
    if (m_weaponAction == requested)
        return;

    m_weaponAction = requested;
    m_weaponActionTime = 0.0f;
    m_weaponActionSwitched = false;
    animator.SetTrigger(draw ? paramDrawTrigger : paramHolsterTrigger);
}

inline void PlayerControllerComponent::UpdateWeaponAction(float dt)
{
    if (m_weaponAction == WeaponAction::None)
        return;

    auto* animatorComponent = scene.GetComponent<AnimatorComponent>();
    if (!animatorComponent) {
        m_weaponAction = WeaponAction::None;
        return;
    }

    m_weaponActionTime += dt;
    const bool draw = m_weaponAction == WeaponAction::Draw;
    const std::string& stateName = draw ? drawStateName : holsterStateName;
    const bool stateActive = animatorComponent->GetLayerState(weaponLayerName) == stateName;
    const float normalizedTime = animatorComponent->GetLayerNormalizedTime(weaponLayerName);
    const float attachTime = draw ? drawAttachNormalizedTime : holsterDetachNormalizedTime;

    // クリップがまだロードされていない場合も、入力処理を永久にロックしない。
    // 通常は正規化時間で同期し、Controller不整合時だけ短い時間のフォールバックを使う。
    if (!m_weaponActionSwitched &&
        ((stateActive && normalizedTime >= attachTime) || m_weaponActionTime >= 0.75f)) {
        if (draw)
            AttachWeaponsToHands();
        else
            AttachWeaponsToHolsters();
        m_weaponActionSwitched = true;
        m_gunsDrawn = draw;
    }

    // ステートが終わったら待機し、壊れた遷移定義でも2秒以内に入力を受け付ける。
    if (m_weaponActionSwitched &&
        ((!stateActive && m_weaponActionTime >= 0.20f) || m_weaponActionTime >= 2.0f))
        m_weaponAction = WeaponAction::None;
}

inline void PlayerControllerComponent::AttachWeaponsToHands()
{
    GameObject* leftWeapon = ResolveObject(weaponLeft, "WPN_Pistol_L");
    GameObject* rightWeapon = ResolveObject(weaponRight, "WPN_Pistol_R");
    GameObject* leftSocket = ResolveObject(handSocketLeft, "GunSocket_Hand_L");
    GameObject* rightSocket = ResolveObject(handSocketRight, "GunSocket_Hand_R");
    if (leftWeapon && leftSocket)
        ReparentKeepingWorld(*leftWeapon, *leftSocket);
    if (rightWeapon && rightSocket)
        ReparentKeepingWorld(*rightWeapon, *rightSocket);
}

inline void PlayerControllerComponent::AttachWeaponsToHolsters()
{
    GameObject* leftWeapon = ResolveObject(weaponLeft, "WPN_Pistol_L");
    GameObject* rightWeapon = ResolveObject(weaponRight, "WPN_Pistol_R");
    GameObject* leftSocket = ResolveObject(holsterLeft, "Sock_Holster_L");
    GameObject* rightSocket = ResolveObject(holsterRight, "Sock_Holster_R");
    if (leftWeapon && leftSocket)
        ReparentKeepingWorld(*leftWeapon, *leftSocket);
    if (rightWeapon && rightSocket)
        ReparentKeepingWorld(*rightWeapon, *rightSocket);
}

inline void PlayerControllerComponent::OnFixedUpdate()
{
    auto* cc = scene.GetComponent<CharacterControllerComponent>();
    auto* rb = scene.GetComponent<RigidBodyComponent>();
    if (!cc || !rb || !rb->enabled || !rb->rigidBody)
        return;

    RigidBody& phy = *rb->rigidBody;
    const float dt = std::max(Time::fixedDeltaTime, 0.0f);
    cc->Tick(&phy, dt);

    const bool jumpRequested = m_jumpRequested;
    const bool dodgeRequested = m_dodgeRequested;
    m_jumpRequested = false;
    m_dodgeRequested = false;
    TickDodge(m_moveDirection, m_hasMoveInput, dodgeRequested, phy, dt);

    // 回避中は開始方向を維持し、通常移動の旋回・速度制御を適用しない。
    if (IsDodging())
        return;

    // 接地中だけ目標上向き速度を与える。質量差と現在の落下速度は
    // CharacterController が吸収するため、調整値を m/s に統一できる。
    if (jumpRequested && cc->enabled && cc->isGrounded) {
        cc->JumpAtVelocity(&phy, std::max(JumpSpeed(), 0.0f));
        animator.SetBool(paramIsGrounded, false);
        animator.SetTrigger(paramJumpTrigger);
    }

    if (m_hasMoveInput && rotateToMoveDirection) {
        const Quaternion targetRotation =
            (Quaternion::LookRotation(m_moveDirection) *
             Quaternion::FromAxisAngle(Vector3::UP, ToRad(modelYawOffsetDegrees))).Normalized();
        const float turnResponse = 1.0f - std::exp(
            -std::max(TurnSpeed(), 0.0f) * dt);
        phy.SetRotation(Quaternion::Slerp(
            phy.GetRotation(), targetRotation, turnResponse).Normalized());
    }

    const Vector3 desiredVelocity = m_hasMoveInput
        ? m_moveDirection * MoveSpeed()
        : Vector3::ZERO;
    const float acceleration = m_hasMoveInput
        ? (cc->isGrounded ? GroundAccel() : airAccel)
        : -1.0f;
    cc->Move(&phy, desiredVelocity, dt, acceleration, GroundDecel());
}

inline float PlayerControllerComponent::DodgeCharge() const
{
    const float cooldown = DodgeCooldown();
    if (cooldown <= 0.0f) return 1.0f;
    return Clamp01(1.0f - m_dodgeCooldown / cooldown);
}

inline void PlayerControllerComponent::TickDodge(const Vector3& moveDirection,
                                                 bool hasInput,
                                                 bool dodgeRequested,
                                                 RigidBody& phy,
                                                 float dt)
{
    if (m_dodgeCooldown > 0.0f)
        m_dodgeCooldown = std::max(0.0f, m_dodgeCooldown - dt);

    // まだ回避していないなら、開始できるかを見る。
    if (m_dodgeRemaining <= 0.0f) {
        if (!dodgeRequested) return;
        if (m_dodgeCooldown > 0.0f) return;

        // 入力方向へ跳ぶ。無入力なら後方へ下がる (敵から離れるのが自然な既定動作)。
        Vector3 direction = hasInput ? moveDirection : -GetMoveForward();
        direction.y = 0.0f;
        if (direction.LengthSq() < EPSILON) return;

        m_dodgeDirection = direction.Normalized();
        m_dodgeRemaining = DodgeDuration();
        // クールダウンは回避の開始から数える。実質的な待ち時間は
        // DodgeCooldown - DodgeDuration になる。
        m_dodgeCooldown  = DodgeCooldown();
        animator.SetTrigger(paramDodgeTrigger);
        // 開始フレームからそのまま下の速度上書きへ進む。
        // ここで return すると 1 フレームだけ慣性で滑り、出だしが鈍る。
    } else {
        m_dodgeRemaining = std::max(0.0f, m_dodgeRemaining - dt);
        if (m_dodgeRemaining <= 0.0f) return;  // 今フレームで終了
    }

    // 回避中は水平速度を上書きし続ける。
    // WHY 開始時に一度入れて終わりにしないか: 敵や壁に接触した瞬間にソルバーが
    //     速度を削るため、途中で失速して「跳んだのに動かない」になる。持続時間の
    //     あいだ毎フレーム入れ直すことで、移動距離が入力に対して安定する。
    Vector3 vel = phy.GetVelocity();
    vel.x = m_dodgeDirection.x * DodgeSpeed();
    vel.z = m_dodgeDirection.z * DodgeSpeed();
    phy.SetVelocity(vel);
}

inline void PlayerControllerComponent::UpdateIK()
{
    auto* ik = scene.GetComponent<IKSolverComponent>();
    if (!ik) return;

    // Use Foot IK は足チェーンだけを制御する。Solver 全体を切ると Spine と LookAt まで停止してしまう。
    for (auto& chain : ik->chains) {
        if (chain.type == IKSolverType::FootPlace)
            chain.enabled = useFootIK;
    }

    UpdateSlopeLean(*ik,
                    scene.GetComponent<CharacterControllerComponent>(),
                    scene.GetComponent<RigidBodyComponent>());
}

inline void PlayerControllerComponent::UpdateSlopeLean(
    IKSolverComponent& ik, CharacterControllerComponent* cc, RigidBodyComponent* rb)
{
    IKChain* spine = nullptr;
    for (auto& chain : ik.chains) {
        if (chain.type == IKSolverType::FABRIK) {
            spine = &chain;
            break;
        }
    }
    if (!spine) return;

    auto* target = scene.GetGameObject(spine->targetEntity);
    if (!target) return;
    if (!m_hasSpineTargetBase) {
        m_spineTargetBase = target->transform.position;
        m_hasSpineTargetBase = true;
    }

    Vector3 desiredOffset = Vector3::ZERO;
    if (cc && cc->isGrounded && rb && rb->enabled && rb->rigidBody) {
        Vector3 moveDirection = rb->rigidBody->GetVelocity();
        moveDirection.y = 0.0f;
        if (moveDirection.LengthSq() > 0.01f && cc->groundNormal.y > 0.1f) {
            moveDirection = moveDirection.Normalized();
            const Vector3 normal = cc->groundNormal.Normalized();
            // 地面法線から移動方向の上り勾配 tan(theta) を求め、上り坂だけ上体を進行方向へ倒す。
            const float uphillGrade = std::max(
                0.0f, -Vector3::Dot(normal, moveDirection) / normal.y);
            constexpr float LEAN_PER_GRADE = 0.35f;
            constexpr float MAX_LEAN_OFFSET = 0.20f;
            const float lean = std::min(MAX_LEAN_OFFSET, uphillGrade * LEAN_PER_GRADE);
            const Vector3 localMoveDirection =
                (transform.worldRotation.Inverse() * moveDirection).Normalized();
            desiredOffset = localMoveDirection * lean;
        }
    }

    // 接触法線は物理ステップごとに微動するため、指数応答でターゲットの揺れを抑える。
    constexpr float LEAN_RESPONSE = 8.0f;
    const float response = 1.0f - std::exp(-LEAN_RESPONSE * std::max(Time::deltaTime, 0.0f));
    m_smoothedSpineOffset = Vector3::Lerp(m_smoothedSpineOffset, desiredOffset, response);
    target->transform.position = m_spineTargetBase + m_smoothedSpineOffset;
}

inline Vector3 PlayerControllerComponent::GetMoveForward() const
{
    if (!useCameraForward) return Vector3::FORWARD;
    auto* camGO = scene.GetMainCameraObject();
    if (!camGO) return Vector3::FORWARD;
    Vector3 fwd = camGO->transform.forward;
    fwd.y = 0.0f;
    return fwd.LengthSq() > EPSILON ? fwd.Normalized() : Vector3::FORWARD;
}

inline Vector3 PlayerControllerComponent::GetMoveRight(const Vector3& forward) const
{
    Vector3 right = Vector3::Cross(Vector3::UP, forward);
    return right.LengthSq() > EPSILON ? right.Normalized() : Vector3::RIGHT;
}

} // namespace sandbox
