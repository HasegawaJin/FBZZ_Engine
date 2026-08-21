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
#include <Scripts/Player/PlayerAimComponent.hpp>
#include <Scripts/Player/WeaponRigComponent.hpp>
#include <algorithm>
#include <cmath>
#include <string_view>

using namespace fbzz::scene;
using namespace fbzz::math;
using namespace fbzz::input;
using namespace fbzz::physics;
using fbzz::Time;

namespace sandbox {

class PlayerControllerComponent : public Script {
    FBZZ_SCRIPT(PlayerControllerComponent)

    // 接地判定と固定ステップの水平速度制御はこの 2 つが揃って初めて成立する。
    // どちらかが欠けると「入力は読めているのにキャラが動かない」無言の状態になる。
    FBZZ_REQUIRE_COMPONENT(CharacterControllerComponent, RigidBodyComponent)
    // 無くても移動そのものは成立する。Animator が無ければパラメーター送信が空振りし、
    // IKSolver が無ければ足の接地補正と上体の傾きが省かれるだけ。
    FBZZ_OPTIONAL_COMPONENT(AnimatorComponent, IKSolverComponent)

public:
    // PlayerComponent が必須参照を束ねて注入する。移動値をここへ複製しないことで、
    // Inspector では PlayerTuning.fzdata だけが調整値の正本になる。
    fbzz::Asset<PlayerTuning> tuning{};

    FBZZ_GROUP("Movement")
    FBZZ_FIELD_RANGE(float, modelYawOffsetDegrees, 180.0f, "Model Yaw Offset",  0.0f, 360.0f)
    FBZZ_FIELD(bool, useCameraForward,      true, "Use Camera Forward")
    FBZZ_FIELD(bool, rotateToMoveDirection, true, "Rotate To Move Dir")
    FBZZ_FIELD(bool, useFootIK,             true, "Use Foot IK")

    FBZZ_GROUP("Dodge")
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

    FBZZ_GROUP("Aim Animation")
    // AimOffset は現在のソフトエイム対象をプレイヤー基準の角度へ変換して駆動する。
    FBZZ_FIELD(std::string, paramAimYaw,   "AimYaw",   "Aim Yaw Param")
    FBZZ_FIELD(std::string, paramAimPitch, "AimPitch", "Aim Pitch Param")
    FBZZ_FIELD(std::string, aimLayerName,  "Aim",      "Aim Layer")
    FBZZ_FIELD(std::string, aimSwayLayerName, "Add_AimSway", "Aim Sway Layer")

    FBZZ_GROUP("Weapon Keys")
    // ここは「抜きたい / 収めたい」という意思の入口だけ。何が起きるかは
    // WeaponRigComponent が持つ。キーバインドの変更で銃の挙動を読まなくて済む。
    FBZZ_FIELD(KeyCode, keyDraw,    KeyCode::G, "Draw Weapons")
    FBZZ_FIELD(KeyCode, keyHolster, KeyCode::H, "Holster Weapons")

    FBZZ_GROUP("Player Fire Animation")
    // 発砲リコイルは銃のスライドとは別に Player の Add_Fire へ加算する。
    FBZZ_FIELD(std::string, fireLayerName, "Add_Fire", "Fire Layer")
    FBZZ_FIELD_FILE(fireLeftClipFile,  "guid:bd25df58d95f4297f208069c014514f5", "Left Fire Clip",  ".anim,.fbx")
    FBZZ_FIELD_FILE(fireRightClipFile, "guid:48b26f14e4c8e89df23b7012356ea83f", "Right Fire Clip", ".anim,.fbx")
    FBZZ_FIELD(std::string, fireLeftClipName,  "Fire_Pistol_L", "Left Fire Clip Name")
    FBZZ_FIELD(std::string, fireRightClipName, "Fire_Pistol_R", "Right Fire Clip Name")
    // Fire_Pistol_L/R は手首 26 度・前腕 7 度しか動かず、肩と上体は静止している。
    // クリップを作り直すまでの暫定として、加算レイヤーの倍率で振れ幅を稼ぐ。
    FBZZ_FIELD_RANGE(float, fireLayerGain, 2.0f, "Fire Recoil Gain", 0.0f, 4.0f)
    FBZZ_TOOLTIP("Add_Fire の加算倍率。1.0 = クリップそのまま")

    FBZZ_GROUP("Player Hit Animation")
    FBZZ_FIELD(std::string, hitLayerName, "Add_Hit", "Hit Layer")
    FBZZ_FIELD_FILE(hitFrontClipFile, "guid:399f84087c05a267c373d75c4a778b11", "Front Hit Clip", ".anim,.fbx")
    FBZZ_FIELD(std::string, hitFrontClipName, "Hit_F", "Front Hit Clip Name")

    // 回避中か。被弾処理や敵 AI が「今は掴めない」を判断するのに使える。
    [[nodiscard]] bool  IsDodging() const { return m_dodgeRemaining > 0.0f; }
    // UI 用。1 = 回避可能 / 0 = 使った直後。
    [[nodiscard]] float DodgeCharge() const;

    void OnStart() override;
    void OnUpdate() override;
    void OnFixedUpdate() override;
    // PolarityGunComponent から呼ばれる Player 側の発砲リコイル。
    void PlayFireAnimation(bool rightHand);
    // 被弾通知から呼ばれる標準の正面リアクション。
    void PlayHitAnimation();
    // PlayerComponent が内部モジュールとして保持するときのエイム結果の注入先。
    void SetAimComponent(PlayerAimComponent* aim) { m_aimOverride = aim; }
    // 武器の挙動そのものは WeaponRigComponent が持つ。ここは要求を渡すだけ。
    void SetWeaponRig(WeaponRigComponent* rig) { m_weaponRig = rig; }

private:
    // 回避の開始判定と、回避中の速度上書き。
    void TickDodge(const Vector3& moveDirection, bool hasInput,
                   bool dodgeRequested, RigidBody& phy, float dt);
    void UpdateIK();
    void UpdateAimParameters();
    void UpdateSlopeLean(IKSolverComponent& ik, CharacterControllerComponent* cc,
                         RigidBodyComponent* rb);
    Vector3 GetMoveForward() const;
    Vector3 GetMoveRight(const Vector3& forward) const;

    // 移動・回避の数値は必須 PlayerTuning からのみ読む。フォールバックを持たせないことで、
    // fzdata の未設定を「たまたま動く」状態にしない。
    [[nodiscard]] float MoveSpeed()     const { return tuning->moveSpeed; }
    [[nodiscard]] float GroundAccel()   const { return tuning->groundAccel; }
    [[nodiscard]] float GroundDecel()   const { return tuning->groundDecel; }
    [[nodiscard]] float AirAccel()      const { return tuning->airAccel; }
    [[nodiscard]] float TurnSpeed()     const { return tuning->turnSpeed; }
    [[nodiscard]] float JumpSpeed()     const { return tuning->jumpSpeed; }
    [[nodiscard]] float DodgeSpeed()    const { return tuning->dodgeSpeed; }
    [[nodiscard]] float DodgeDuration() const { return tuning->dodgeDuration; }
    [[nodiscard]] float DodgeCooldown() const { return tuning->dodgeCooldown; }

    PlayerAimComponent* m_aimOverride = nullptr;
    WeaponRigComponent* m_weaponRig   = nullptr;

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
};

// Reflect() をフィールド宣言から自動生成する (旧 .generated.hpp は廃止)。
FBZZ_REFLECT(PlayerControllerComponent)

// ── 実装 (inline) ─────────────────────────────────────────────────────────────
inline void PlayerControllerComponent::OnStart()
{
    // PlayerComponent から注入される共有調整値が無い場合は、ヘルパーの
    // tuning->参照を実行しない。単体配置の設定漏れを SEH へ進ませず、
    // スクリプトを安全に停止する。
    if (!tuning) {
        debug.LogError(
            "PlayerControllerComponent requires PlayerTuning .fzdata asset. "
            "Attach it through PlayerComponent before entering Play mode.");
        enabled = false;
        return;
    }

    // WHY: 接触摩擦トルクによるカプセル傾きで水平ジッターが発生するため全軸フリーズ。
    physics.SetFreezeRotation(true, true, true);
    m_dodgeRemaining = 0.0f;
    m_dodgeCooldown  = 0.0f;
    m_moveDirection  = Vector3::ZERO;
    m_hasMoveInput   = false;
    m_jumpRequested  = false;
    m_dodgeRequested = false;
}

inline void PlayerControllerComponent::OnUpdate()
{
    if (!enabled || !transform || !tuning) return;
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
    // 入力はここまで。抜く / 収める の中身は WeaponRigComponent の担当で、
    // このスクリプトは銃の存在もソケットも知らない。
    if (m_weaponRig) {
        if (input.GetKeyDown(keyDraw))    m_weaponRig->RequestDraw();
        if (input.GetKeyDown(keyHolster)) m_weaponRig->RequestHolster();
    }

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
    UpdateAimParameters();
    UpdateIK();
}

inline void PlayerControllerComponent::UpdateAimParameters()
{
    // 銃を収納している間は、エイム差分と歩行中の上半身揺れを無効にする。
    // レイヤー自体は Controller に常駐させ、再び抜いたときの再構築コストを避ける。
    // 「抜いているか」は銃の側の事実なので、WeaponRigComponent に聞く。
    const float aimWeight = (m_weaponRig && m_weaponRig->IsDrawn()) ? 1.0f : 0.0f;
    animator.SetLayerWeight(aimLayerName, aimWeight);
    animator.SetLayerWeight(aimSwayLayerName, aimWeight);

    auto* aim = m_aimOverride ? m_aimOverride : scene.GetScript<PlayerAimComponent>();
    GameObject* target = aim ? aim->CurrentTarget() : nullptr;
    if (!target) {
        animator.SetFloat(paramAimYaw, 0.0f);
        animator.SetFloat(paramAimPitch, 0.0f);
        return;
    }

    // Animator の入力は設計書のエイムコーン (-1..1) に正規化する。
    // ワールド角ではなくプレイヤーのローカル角を使うため、キャラクターの旋回と
    // カメラの向きが変わっても 9 セルの意味が変わらない。
    const Vector3 localTarget = transform.worldRotation.Inverse() *
        (target->transform.worldPosition - transform.worldPosition);
    const float horizontalLength =
        std::sqrtf(localTarget.x * localTarget.x + localTarget.z * localTarget.z);
    constexpr float RAD_TO_DEGREES = 57.29577951308232f;
    const float yawDegrees = std::atan2f(localTarget.x, localTarget.z) * RAD_TO_DEGREES;
    const float pitchDegrees = std::atan2f(localTarget.y,
                                           std::max(horizontalLength, 0.0001f)) * RAD_TO_DEGREES;

    animator.SetFloat(paramAimYaw,
                      std::clamp(yawDegrees / 45.0f, -1.0f, 1.0f));
    animator.SetFloat(paramAimPitch,
                      std::clamp(pitchDegrees / (pitchDegrees >= 0.0f ? 39.0f : 34.0f),
                                 -1.0f, 1.0f));
}

inline void PlayerControllerComponent::PlayFireAnimation(bool rightHand)
{
    const std::string& clipFile = rightHand ? fireRightClipFile : fireLeftClipFile;
    const std::string& clipName = rightHand ? fireRightClipName : fireLeftClipName;
    if (clipFile.empty()) return;

    // Fire は Additive Reference Pose = Pose_FireZero のレイヤーでのみ再生する。
    // Aim / Hit と基準ポーズが異なるため、別レイヤーへ分けて混線を防ぐ。
    // 倍率は撃つたびに入れ直す。Inspector で触った値がその場の 1 発から効く。
    animator.SetLayerWeight(fireLayerName, fireLayerGain);
    animator.PlaySlot(fireLayerName, clipFile, clipName, 0.0f, 0.08f, 1.0f, false);
}

inline void PlayerControllerComponent::PlayHitAnimation()
{
    if (hitFrontClipFile.empty()) return;
    animator.PlaySlot(hitLayerName, hitFrontClipFile, hitFrontClipName,
                      0.03f, 0.10f, 1.0f, false);
}

inline void PlayerControllerComponent::OnFixedUpdate()
{
    if (!enabled || !tuning) return;

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
        ? (cc->isGrounded ? GroundAccel() : AirAccel())
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
