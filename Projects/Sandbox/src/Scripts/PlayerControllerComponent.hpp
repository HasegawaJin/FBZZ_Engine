// FBZZ Engine
// PlayerControllerComponent.hpp | sandbox
// WASD + Space ジャンプによるプレイヤー操作スクリプト
#pragma once

#include <Engine/Input/Input.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Components/CameraComponent.hpp>
#include <Engine/Scene/Components/IKSolverComponent.hpp>
#include <Engine/Scene/Components/RigidBodyComponent.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>
#include <cmath>

namespace sandbox {

struct PlayerControllerComponent : fbzz::scene::Script {
    static constexpr const char* TYPE_NAME = "PlayerControllerComponent";

    const char* GetTypeName() const override { return TYPE_NAME; }

    float moveSpeed             = 4.0f;
    float sprintMultiplier      = 1.8f;
    float jumpForce             = 5.0f;
    float modelYawOffsetDegrees = 180.0f;
    bool  useCameraForward      = true;
    bool  rotateToMoveDirection = true;

    void Reflect(fbzz::scene::IReflector& reflector) override
    {
        reflector.Field("Move Speed",               moveSpeed);
        reflector.Field("Sprint Multiplier",        sprintMultiplier);
        reflector.Field("Jump Force",               jumpForce);
        reflector.Field("Use Camera Forward",       useCameraForward);
        reflector.Field("Rotate To Move Direction", rotateToMoveDirection);
    }

    void OnUpdate(float dt) override
    {
        if (!m_gameObject) return;

        auto* rigidBody       = m_gameObject->GetComponent<fbzz::scene::RigidBodyComponent>();
        const bool usePhysics = rigidBody && rigidBody->enabled && rigidBody->rigidBody;

        // 接地・空中状態の更新とアニメーターへの反映を移動入力より先に行う。
        // WHY: 移動なし時の早期 return の前に処理しないとジャンプ・落下が検出されない。
        if (usePhysics) {
            const float vy = rigidBody->rigidBody->GetVelocity().y;
            UpdateGrounding(vy, dt);
            SetAnimatorFloat("VerticalSpeed", vy);
            SetAnimatorBool("IsGrounded", m_isGrounded);
        }
        UpdateIK();
        HandleJump(rigidBody, usePhysics);

        const fbzz::math::Vector3 forward = GetMoveForward();
        const fbzz::math::Vector3 right   = GetMoveRight(forward);

        fbzz::math::Vector3 move = fbzz::math::Vector3::ZERO;
        if (fbzz::input::Input::KeyHeld(fbzz::input::KeyCode::W)) move += forward;
        if (fbzz::input::Input::KeyHeld(fbzz::input::KeyCode::S)) move -= forward;
        if (fbzz::input::Input::KeyHeld(fbzz::input::KeyCode::D)) move += right;
        if (fbzz::input::Input::KeyHeld(fbzz::input::KeyCode::A)) move -= right;

        if (move.LengthSq() <= fbzz::math::EPSILON) {
            StopHorizontalPhysicsVelocity(rigidBody);
            SetAnimatorFloat("Speed", 0.0f);
            return;
        }

        const float speed = fbzz::input::Input::KeyHeld(fbzz::input::KeyCode::SHIFT)
            ? moveSpeed * sprintMultiplier
            : moveSpeed;
        const fbzz::math::Vector3 direction = move.Normalized();
        SetAnimatorFloat("Speed", speed);

        if (usePhysics) {
            // WHY: カプセルを Transform で直接ワープさせると、坂の接触法線による押し上げを
            // physics::World が速度として解決できない。水平速度だけを入力で上書きし、Y 速度は
            // 重力・接触解決に任せることで、斜面上では通常の衝突解決で登れるようにする。
            fbzz::math::Vector3 vel = rigidBody->rigidBody->GetVelocity();
            vel.x = direction.x * speed;
            vel.z = direction.z * speed;
            rigidBody->rigidBody->SetVelocity(vel);
        } else {
            // RigidBody を持たないテスト用 GameObject では Transform 移動にフォールバックする。
            m_gameObject->transform.localPosition += direction * (speed * dt);
            m_gameObject->transform.position = m_gameObject->transform.localPosition;
        }

        if (rotateToMoveDirection) {
            const fbzz::math::Quaternion moveRotation = fbzz::math::Quaternion::LookRotation(direction);
            const fbzz::math::Quaternion modelOffset =
                fbzz::math::Quaternion::FromAxisAngle(fbzz::math::Vector3::UP,
                                                      fbzz::math::ToRad(modelYawOffsetDegrees));
            m_gameObject->transform.localRotation = (moveRotation * modelOffset).Normalized();
            m_gameObject->transform.rotation      = m_gameObject->transform.localRotation;
        }
    }

private:
    // インパルス直後の誤判定防止用の最低待機時間 (秒)。
    // WHY: ApplyImpulse 直後は velocity.y がまだ物理ステップに反映されていない場合があり、
    //      瞬間的に 0 のままに見えることがあるため、短いガード時間を設ける。
    static constexpr float JUMP_MIN_AIR_TIME    = 0.2f;
    // 「落下中」とみなす velocity.y の閾値 (m/s)。着地検出の前提条件。
    static constexpr float FALL_VEL_THRESHOLD   = -0.5f;
    // 着地とみなす velocity.y の絶対値閾値 (m/s)。落下後にこの値以下になったとき着地と判定。
    static constexpr float GROUND_VEL_THRESHOLD = 0.3f;
    // 崖落ちとみなす velocity.y の閾値 (m/s)。接地中にこれを下回ると空中扱いに切り替える。
    static constexpr float LEDGE_FALL_THRESHOLD = -1.0f;

    bool  m_isGrounded = true;   // 地面に接触しているか
    bool  m_wasFalling = false;  // 落下フェーズ (vy < FALL_VEL_THRESHOLD) を経験したか
    float m_jumpTimer  = 0.0f;   // ジャンプ後の滞空経過秒数

    // 接地状態の更新。接地・空中どちらの方向にも遷移する。
    // WHY: VerticalSpeed float でステートマシンの JumpUp/Fall 遷移を駆動するため、
    //      スクリプト側の IsGrounded は「実際に地面と接触しているか」の判定のみに専念する。
    //      ジャンプ弧の頂点誤判定は m_wasFalling フラグで防ぐ。
    void UpdateGrounding(float vy, float dt)
    {
        if (m_isGrounded) {
            // 崖落ち検出: 接地中に velocity.y が閾値を下回ったら空中へ移行する。
            if (vy < LEDGE_FALL_THRESHOLD) {
                m_isGrounded = false;
                m_wasFalling = false;
                m_jumpTimer  = 0.0f;
            }
        } else {
            // 着地検出: ジャンプ直後の誤判定を避けるため、いったん落下フェーズを経由してから判定する。
            m_jumpTimer += dt;
            if (m_jumpTimer >= JUMP_MIN_AIR_TIME && vy < FALL_VEL_THRESHOLD)
                m_wasFalling = true;
            if (m_wasFalling && std::fabsf(vy) < GROUND_VEL_THRESHOLD) {
                m_isGrounded = true;
                m_wasFalling = false;
                m_jumpTimer  = 0.0f;
            }
        }
    }

    // IK は地上ステート (Idle/Walk/Run) のときのみ有効にする。
    // WHY: JumpUp / Fall / Landing 中は足 IK のグラウンドスナップが無意味になり、
    //      不正なターゲット追従でアニメーションが破綻するため。
    //      Landing はステートマシンが hasExitTime で自動完了するため、
    //      IsInState で抜けた瞬間に IK を戻せばアニメーションと同期できる。
    void UpdateIK()
    {
        if (!m_gameObject) return;
        auto* anim = m_gameObject->GetComponent<fbzz::scene::AnimatorComponent>();
        const bool inAirState = !anim ||
            anim->IsInState("JumpUp") ||
            anim->IsInState("Fall")   ||
            anim->IsInState("Landing");
        SetIKEnabled(!inAirState);
    }

    // Space キー押下時、接地中であれば Y 方向インパルスでジャンプさせる。
    void HandleJump(fbzz::scene::RigidBodyComponent* rigidBody, bool usePhysics)
    {
        if (!usePhysics || !m_isGrounded) return;
        if (!fbzz::input::Input::KeyDown(fbzz::input::KeyCode::SPACE)) return;

        // WHY: インパルス = jumpForce * mass とすることで、質量に関わらず同じ跳躍高さを保つ。
        const float impulse = jumpForce * rigidBody->rigidBody->GetMass();
        rigidBody->rigidBody->ApplyImpulse({ 0.0f, impulse, 0.0f });
        m_isGrounded = false;
        m_wasFalling = false;
        m_jumpTimer  = 0.0f;
    }

    fbzz::math::Vector3 GetMoveForward() const
    {
        if (!useCameraForward || !m_scene) return fbzz::math::Vector3::FORWARD;

        for (auto& go : m_scene->GameObjects()) {
            auto* camera = go.GetComponent<fbzz::scene::CameraComponent>();
            if (!camera || !camera->enabled || !camera->isMain) continue;

            fbzz::math::Vector3 forward = go.transform.Forward();
            forward.y = 0.0f;
            if (forward.LengthSq() > fbzz::math::EPSILON) return forward.Normalized();
        }

        return fbzz::math::Vector3::FORWARD;
    }

    fbzz::math::Vector3 GetMoveRight(const fbzz::math::Vector3& forward) const
    {
        fbzz::math::Vector3 right = fbzz::math::Vector3::Cross(fbzz::math::Vector3::UP, forward);
        if (right.LengthSq() <= fbzz::math::EPSILON) return fbzz::math::Vector3::RIGHT;
        return right.Normalized();
    }

    void StopHorizontalPhysicsVelocity(fbzz::scene::RigidBodyComponent* rigidBody) const
    {
        if (!rigidBody || !rigidBody->enabled || !rigidBody->rigidBody) return;

        // WHAT: 入力がないフレームでは XZ 速度だけを止め、Y 速度は落下・接地判定に残す。
        fbzz::math::Vector3 vel = rigidBody->rigidBody->GetVelocity();
        vel.x = 0.0f;
        vel.z = 0.0f;
        rigidBody->rigidBody->SetVelocity(vel);
    }

    void SetAnimatorFloat(std::string_view name, float value) const
    {
        if (!m_gameObject) return;
        auto* anim = m_gameObject->GetComponent<fbzz::scene::AnimatorComponent>();
        if (anim) anim->SetFloat(name, value);
    }

    void SetAnimatorBool(std::string_view name, bool value) const
    {
        if (!m_gameObject) return;
        auto* anim = m_gameObject->GetComponent<fbzz::scene::AnimatorComponent>();
        if (anim) anim->SetBool(name, value);
    }

    void SetIKEnabled(bool enabled) const
    {
        if (!m_gameObject) return;
        auto* ik = m_gameObject->GetComponent<fbzz::scene::IKSolverComponent>();
        if (ik) ik->enabled = enabled;
    }
};

} // namespace sandbox
