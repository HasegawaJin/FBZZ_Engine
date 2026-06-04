// FBZZ Engine
// PlayerControllerComponent.hpp | sandbox
// RigidBody ベースの汎用プレイヤーコントローラースクリプト
// キーバインド・アニメーター連携・IK 連携をすべて Inspector から設定可能にする。
// 接地検出・ジャンプ状態管理は CharacterControllerComponent に委譲する。
#pragma once

// WHY: Sandbox スクリプトは engine 層からインクルードされない末端ヘッダのため、
//      using namespace を許可する。詳細は AGENTS.md を参照。
#include <Engine/Core/Logger.hpp>
#include <Engine/Input/Input.hpp>
#include <Engine/Scene/Components/CharacterControllerComponent.hpp>
#include <Engine/Scene/Components/IKSolverComponent.hpp>
#include <Engine/Scene/Components/RigidBodyComponent.hpp>
#include <Engine/Scene/Script.hpp>

using namespace fbzz::scene;
using namespace fbzz::math;
using namespace fbzz::input;
using namespace fbzz::physics;

namespace sandbox {

class PlayerControllerComponent : public Script {
public:
    static constexpr const char* TYPE_NAME = "PlayerControllerComponent";
    const char* GetTypeName() const override { return TYPE_NAME; }

    // ── 移動設定 ──────────────────────────────────────────────────────────
    float moveSpeed             = 4.0f;
    float sprintMultiplier      = 1.8f;
    float jumpForce             = 5.0f;
    float modelYawOffsetDegrees = 180.0f;
    bool  useCameraForward      = true;
    bool  rotateToMoveDirection = true;

    // ── キーバインド ───────────────────────────────────────────────────────
    // WHY: int で保持し (KeyCode)keyXxx でキャストする。
    //      IReflector が int をサポートするため Inspector・シリアライザ両対応できる。
    int keyForward  = (int)KeyCode::W;
    int keyBackward = (int)KeyCode::S;
    int keyLeft     = (int)KeyCode::A;
    int keyRight    = (int)KeyCode::D;
    int keyJump     = (int)KeyCode::SPACE;
    int keySprint   = (int)KeyCode::SHIFT;

    // ── Animator パラメーター名 ────────────────────────────────────────────
    std::string paramSpeed         = "Speed";
    std::string paramVerticalSpeed = "VerticalSpeed";
    std::string paramIsGrounded    = "IsGrounded";

    // ── Animator 空中ステート名 (IK コンポーネント自体を無効化する対象) ──────
    // WHY: IK Weight は AnimationState.ikWeight で制御する。
    //      ここでは IKSolverComponent.enabled を完全に切るステートのみ指定する。
    //      (例: 高速パーティクル演出中など IK 計算コスト自体を省きたい場合)
    std::string stateJumpUp  = "JumpUp";
    std::string stateFall    = "Fall";
    std::string stateLanding = "Landing";

    void Reflect(IReflector& r) override
    {
        r.Field("Move Speed",               moveSpeed);
        r.Field("Sprint Multiplier",        sprintMultiplier);
        r.Field("Jump Force",               jumpForce);
        r.Field("Model Yaw Offset",         modelYawOffsetDegrees);
        r.Field("Use Camera Forward",       useCameraForward);
        r.Field("Rotate To Move Direction", rotateToMoveDirection);
        r.Field("Key Forward",              keyForward);
        r.Field("Key Backward",             keyBackward);
        r.Field("Key Left",                 keyLeft);
        r.Field("Key Right",                keyRight);
        r.Field("Key Jump",                 keyJump);
        r.Field("Key Sprint",               keySprint);
        r.Field("Param Speed",              paramSpeed);
        r.Field("Param Vertical Speed",     paramVerticalSpeed);
        r.Field("Param Is Grounded",        paramIsGrounded);
        r.Field("State Jump Up",            stateJumpUp);
        r.Field("State Fall",               stateFall);
        r.Field("State Landing",            stateLanding);
    }

    void OnStart() override
    {
        auto* rb = scene.GetComponent<RigidBodyComponent>();
        if (!rb || !rb->rigidBody) return;
        // WHY: 接触摩擦トルクでカプセルが傾くと接触法線が変化し、Baumgarte 補正が
        //      水平成分を持って前後ジッターを引き起こす。全軸 freeze でこれを防ぐ。
        rb->rigidBody->SetFreezeRotation({ true, true, true });
    }

    void OnUpdate(float dt) override
    {
        if (!transform) return;
        auto* cc  = scene.GetComponent<CharacterControllerComponent>();
        auto* rb  = scene.GetComponent<RigidBodyComponent>();
        auto* phy = rb && rb->enabled && rb->rigidBody ? rb->rigidBody.get() : nullptr;

        if (cc) {
            cc->Tick(phy, dt);
            animator.SetFloat(paramVerticalSpeed, cc->verticalSpeed);
            animator.SetBool(paramIsGrounded,     cc->isGrounded);
        }
        UpdateIK();
        HandleJump(cc, phy);

        const Vector3 forward = GetMoveForward();
        const Vector3 right   = GetMoveRight(forward);
        Vector3 move = Vector3::ZERO;
        if (input.GetKey((KeyCode)keyForward))  move += forward;
        if (input.GetKey((KeyCode)keyBackward)) move -= forward;
        if (input.GetKey((KeyCode)keyRight))    move += right;
        if (input.GetKey((KeyCode)keyLeft))     move -= right;

        if (move.LengthSq() <= EPSILON) {
            if (phy) {
                // WHAT: 入力がないフレームでは XZ 速度だけを止め、Y 速度は落下・接地判定に残す。
                Vector3 vel = phy->GetVelocity();
                vel.x = vel.z = 0.0f;
                phy->SetVelocity(vel);
            }
            animator.SetFloat(paramSpeed, 0.0f);
            return;
        }

        const float speed = input.GetKey((KeyCode)keySprint) ? moveSpeed * sprintMultiplier : moveSpeed;
        const Vector3 direction = move.Normalized();
        animator.SetFloat(paramSpeed, speed);

        if (phy) {
            // WHY: カプセルを Transform で直接ワープさせると、坂の接触法線による押し上げを
            //      physics::World が速度として解決できない。水平速度だけを入力で上書きし、Y 速度は
            //      重力・接触解決に任せることで、斜面上では通常の衝突解決で登れるようにする。
            Vector3 vel = phy->GetVelocity();
            vel.x = direction.x * speed;
            vel.z = direction.z * speed;
            phy->SetVelocity(vel);
        } else {
            // RigidBody を持たないテスト用 GameObject では Transform 移動にフォールバックする。
            transform->localPosition += direction * (speed * dt);
            transform->position       = transform->localPosition;
        }

        if (rotateToMoveDirection) {
            const Quaternion rot = (Quaternion::LookRotation(direction) *
                Quaternion::FromAxisAngle(Vector3::UP, ToRad(modelYawOffsetDegrees))).Normalized();
            transform->localRotation = transform->rotation = rot;
        }
    }

    void OnCollisionEnter(const CollisionInfo& info) override
    {
        auto* cc = scene.GetComponent<CharacterControllerComponent>();
        if (cc) cc->RegisterGroundContact(info);
    }

    void OnCollisionStay(const CollisionInfo& info) override
    {
        auto* cc = scene.GetComponent<CharacterControllerComponent>();
        if (cc) cc->RegisterGroundContact(info);
    }

private:
    void HandleJump(CharacterControllerComponent* cc, RigidBody* phy)
    {
        if (!cc || !cc->isGrounded || !phy) return;
        if (!input.GetKeyDown((KeyCode)keyJump)) return;

        // WHY: インパルス = jumpForce * mass とすることで、質量に関わらず同じ跳躍高さを保つ。
        phy->ApplyImpulse({ 0.0f, jumpForce * phy->GetMass(), 0.0f });
        cc->Jump();
        animator.SetBool(paramIsGrounded, false);
        animator.SetFloat(paramVerticalSpeed, jumpForce);
    }

    // IK は地上ステート (Idle/Walk/Run) のときのみ有効にする。
    // WHY: JumpUp / Fall / Landing 中は足 IK のグラウンドスナップが無意味になり、
    //      アニメーションが破綻するため。
    void UpdateIK()
    {
        const bool ikOff = animator.IsInState(stateJumpUp)  ||
                           animator.IsInState(stateFall)    ||
                           animator.IsInState(stateLanding);
        auto* ik = scene.GetComponent<IKSolverComponent>();
        if (ik) ik->enabled = !ikOff;
    }

    Vector3 GetMoveForward() const
    {
        if (!useCameraForward) return Vector3::FORWARD;
        auto* camGO = scene.GetMainCameraObject();
        if (!camGO) return Vector3::FORWARD;
        Vector3 fwd = camGO->transform.Forward();
        fwd.y = 0.0f;
        return fwd.LengthSq() > EPSILON ? fwd.Normalized() : Vector3::FORWARD;
    }

    Vector3 GetMoveRight(const Vector3& forward) const
    {
        Vector3 right = Vector3::Cross(Vector3::UP, forward);
        return right.LengthSq() > EPSILON ? right.Normalized() : Vector3::RIGHT;
    }
};

} // namespace sandbox
