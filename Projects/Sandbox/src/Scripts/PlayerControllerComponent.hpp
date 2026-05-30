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

    void OnStart() override
    {
        if (!m_gameObject) return;
        auto* rb = m_gameObject->GetComponent<fbzz::scene::RigidBodyComponent>();
        if (!rb || !rb->rigidBody) return;
        // WHY: 接触摩擦トルクでカプセルが傾くと接触法線が変化し、Baumgarte 補正が
        //      水平成分を持って前後ジッターを引き起こす。全軸 freeze でこれを防ぐ。
        rb->rigidBody->SetFreezeRotation({ true, true, true });
    }

    void OnUpdate(float dt) override
    {
        if (!m_gameObject) return;

        auto* rigidBody       = m_gameObject->GetComponent<fbzz::scene::RigidBodyComponent>();
        const bool usePhysics = rigidBody && rigidBody->enabled && rigidBody->rigidBody;

        // 接地・空中状態の更新とアニメーターへの反映を移動入力より先に行う。
        // WHY: 移動なし時の早期 return の前に処理しないとジャンプ・落下が検出されない。
        if (usePhysics) {
            UpdateGroundContactTimer(dt);
            float vy = rigidBody->rigidBody->GetVelocity().y;
            UpdateGrounding(vy, dt);
            StabilizeGroundedVerticalVelocity(rigidBody, vy);
            UpdateIntentionalJumpState(vy, dt);
            SetAnimatorFloat("VerticalSpeed", ComputeAnimatorVerticalSpeed(vy));
            SetAnimatorBool("IsGrounded", m_isGrounded);
        }
        UpdateIK();
        HandleJump(rigidBody, usePhysics);
        if (usePhysics)
            m_hasGroundContact = false;

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

    void OnCollisionEnter(const fbzz::scene::CollisionInfo& info) override
    {
        RegisterGroundContact(info);
    }

    void OnCollisionStay(const fbzz::scene::CollisionInfo& info) override
    {
        RegisterGroundContact(info);
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
    // WHY: 斜面下降時は Y 速度が負になり続けるため、歩ける面との接触を接地の主判定にする。
    static constexpr float MIN_GROUND_NORMAL_Y = 0.5f;
    // WHY: 物理の固定ステップと ScriptSystem の更新差で 1 フレームだけ接触が欠けても空中扱いにしない。
    static constexpr float GROUND_CONTACT_GRACE_TIME = 0.12f;
    // WHY: ジャンプ直後は床接触が残るため、その接触で即座に着地へ戻るのを防ぐ。
    static constexpr float JUMP_GROUND_IGNORE_TIME = 0.12f;
    // WHAT: 接地中に残る微小な Y 速度をゼロへ寄せ、着地後の小刻みな Jump/Fall 遷移を抑える。
    static constexpr float GROUNDED_Y_VELOCITY_SNAP = 0.35f;
    // WHY: JumpUp は「上向き速度」ではなく「プレイヤーがジャンプ入力を出した事実」で駆動する。
    //      着地補正や接触キャッシュの微小な上向き速度を JumpUp と誤認しないための上限時間。
    static constexpr float INTENTIONAL_JUMP_MAX_TIME = 1.0f;

    bool  m_isGrounded = true;   // 地面に接触しているか
    bool  m_wasFalling = false;  // 落下フェーズ (vy < FALL_VEL_THRESHOLD) を経験したか
    bool  m_hasGroundContact = false;
    bool  m_isIntentionalJump = false;
    float m_jumpTimer  = 0.0f;   // ジャンプ後の滞空経過秒数
    float m_groundContactTimer = GROUND_CONTACT_GRACE_TIME;
    float m_ignoreGroundTimer = 0.0f;
    float m_intentionalJumpTimer = 0.0f;

    // 接地状態の更新。接地・空中どちらの方向にも遷移する。
    // WHY: VerticalSpeed float でステートマシンの JumpUp/Fall 遷移を駆動するため、
    //      スクリプト側の IsGrounded は「実際に地面と接触しているか」の判定のみに専念する。
    //      ジャンプ弧の頂点誤判定は m_wasFalling フラグで防ぐ。
    void UpdateGrounding(float vy, float dt)
    {
        const bool hasGroundContact = HasGroundContact();
        const bool hasRecentGroundContact =
            m_groundContactTimer > 0.0f && m_ignoreGroundTimer <= 0.0f;

        if (m_isGrounded) {
            if (hasRecentGroundContact) {
                m_wasFalling = false;
                m_jumpTimer  = 0.0f;
                m_isIntentionalJump = false;
                m_intentionalJumpTimer = 0.0f;
                return;
            }

            // 崖落ち検出: 接地中に velocity.y が閾値を下回ったら空中へ移行する。
            if (vy < LEDGE_FALL_THRESHOLD) {
                m_isGrounded = false;
                m_wasFalling = false;
                m_jumpTimer  = 0.0f;
            }
        } else {
            // 着地検出: ジャンプ直後の誤判定を避けるため、いったん落下フェーズを経由してから判定する。
            m_jumpTimer += dt;
            if (hasGroundContact && m_jumpTimer >= JUMP_MIN_AIR_TIME) {
                m_isGrounded = true;
                m_wasFalling = false;
                m_jumpTimer  = 0.0f;
                m_isIntentionalJump = false;
                m_intentionalJumpTimer = 0.0f;
                return;
            }
            if (m_jumpTimer >= JUMP_MIN_AIR_TIME && vy < FALL_VEL_THRESHOLD)
                m_wasFalling = true;
            if (m_wasFalling && std::fabsf(vy) < GROUND_VEL_THRESHOLD) {
                m_isGrounded = true;
                m_wasFalling = false;
                m_jumpTimer  = 0.0f;
                m_isIntentionalJump = false;
                m_intentionalJumpTimer = 0.0f;
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

    // 接地接触の猶予時間を更新する。固定物理ステップと描画フレームのズレをここで吸収する。
    void UpdateGroundContactTimer(float dt)
    {
        if (m_groundContactTimer > 0.0f) {
            m_groundContactTimer -= dt;
            if (m_groundContactTimer < 0.0f) m_groundContactTimer = 0.0f;
        }

        if (m_ignoreGroundTimer > 0.0f) {
            m_ignoreGroundTimer -= dt;
            if (m_ignoreGroundTimer < 0.0f) m_ignoreGroundTimer = 0.0f;
        }
    }

    // 歩ける面との接触を記録する。壁や急斜面は contactNormal.y で除外する。
    void RegisterGroundContact(const fbzz::scene::CollisionInfo& info)
    {
        if (info.contactNormal.y < MIN_GROUND_NORMAL_Y) return;
        if (m_ignoreGroundTimer > 0.0f) return;

        m_hasGroundContact = true;
        m_groundContactTimer = GROUND_CONTACT_GRACE_TIME;
        m_isGrounded = true;
        m_wasFalling = false;
        m_jumpTimer = 0.0f;
        m_isIntentionalJump = false;
        m_intentionalJumpTimer = 0.0f;
        RemoveVelocityIntoGround(info.contactNormal);
        SetAnimatorBool("IsGrounded", true);
        SetAnimatorFloat("VerticalSpeed", 0.0f);
    }

    // 接地中の微小な上下速度だけを抑える。大きい下り速度は斜面追従のため物理に残す。
    void StabilizeGroundedVerticalVelocity(fbzz::scene::RigidBodyComponent* rigidBody, float& vy) const
    {
        if (!m_isGrounded || !rigidBody || !rigidBody->rigidBody) return;
        if (!HasGroundContact()) return;
        if (std::fabsf(vy) > GROUNDED_Y_VELOCITY_SNAP) return;

        fbzz::math::Vector3 vel = rigidBody->rigidBody->GetVelocity();
        vel.y = 0.0f;
        rigidBody->rigidBody->SetVelocity(vel);
        vy = 0.0f;
    }

    // ジャンプ入力から発生した上昇だけを JumpUp 用の速度として残す。
    // WHY: 物理ソルバーの接触補正は速度を小さく揺らすため、raw velocity.y を Animator に直結すると
    //      着地直後の押し戻しが「もう一度ジャンプした」と誤解される。
    void UpdateIntentionalJumpState(float vy, float dt)
    {
        if (!m_isIntentionalJump) return;

        m_intentionalJumpTimer += dt;
        if (m_isGrounded || vy < FALL_VEL_THRESHOLD || m_intentionalJumpTimer >= INTENTIONAL_JUMP_MAX_TIME) {
            m_isIntentionalJump = false;
            m_intentionalJumpTimer = 0.0f;
        }
    }

    // Animator に渡す縦速度を、物理速度からアニメーション意味へ変換する。
    // WHAT: 接地中は 0、入力由来ではない上向き速度も 0 に丸め、落下速度だけは Fall 判定へ残す。
    float ComputeAnimatorVerticalSpeed(float vy) const
    {
        if (m_isGrounded) return 0.0f;
        if (vy > 0.0f && !m_isIntentionalJump) return 0.0f;
        return vy;
    }

    // 接地面に垂直な速度成分を両方向とも除去する。斜面に沿う速度は残すため、下り坂で接着を壊さない。
    // WHY: Resolve() でインパルス適用後は vy が正（上向きバウンス）に反転していることがある。
    //      大きいコライダーほど貫通深度が大きく Baumgarte 補正も強いため、このバウンスが顕著になる。
    //      負方向（地面へ向かう）だけでなく正方向（バウンス）も除去することで Landing 直後の再浮遊を防ぐ。
    void RemoveVelocityIntoGround(const fbzz::math::Vector3& groundNormal) const
    {
        if (!m_gameObject) return;
        auto* rigidBody = m_gameObject->GetComponent<fbzz::scene::RigidBodyComponent>();
        if (!rigidBody || !rigidBody->enabled || !rigidBody->rigidBody) return;

        fbzz::math::Vector3 vel = rigidBody->rigidBody->GetVelocity();
        if (groundNormal.LengthSq() <= fbzz::math::EPSILON) return;

        const fbzz::math::Vector3 normal = groundNormal.Normalized();
        const float normalSpeed = fbzz::math::Vector3::Dot(vel, normal);
        if (std::fabsf(normalSpeed) < fbzz::math::EPSILON) return;

        vel -= normal * normalSpeed;
        rigidBody->rigidBody->SetVelocity(vel);
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
        m_hasGroundContact = false;
        m_jumpTimer  = 0.0f;
        m_groundContactTimer = 0.0f;
        m_ignoreGroundTimer = JUMP_GROUND_IGNORE_TIME;
        m_isIntentionalJump = true;
        m_intentionalJumpTimer = 0.0f;
        SetAnimatorBool("IsGrounded", false);
        SetAnimatorFloat("VerticalSpeed", jumpForce);
    }

    bool HasGroundContact() const
    {
        return m_hasGroundContact && m_ignoreGroundTimer <= 0.0f;
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
