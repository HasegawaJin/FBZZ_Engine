// FBZZ Engine
// CharacterControllerComponent.hpp | fbzz::scene
// キャラクター移動に必要な接地検出・ジャンプ状態管理をエンジン側に集約するコンポーネント
//
// WHY: 接地判定・速度安定化・タイマー管理はゲームロジックではなく物理システム隣接の汎用処理であり、
//      Script 内に書くと同じステートマシンを全キャラクターで重複実装することになる。
//      Unity の CharacterController に相当するが、このエンジンは RigidBody ベースの物理を使うため
//      内部実装は velocity 操作による制御となる。
//
// 使い方:
//   1. GameObject に RigidBodyComponent と一緒にアタッチする
//   2. Script の OnUpdate 先頭で Tick(rb, dt) を呼ぶ
//   3. Script の OnCollisionEnter/Stay で RegisterGroundContact(info) を呼ぶ
//   4. ジャンプ時は Jump(rb, impulse) を呼ぶ
//   5. isGrounded / verticalSpeed を読んで Animator を操作する
#pragma once

#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Script.hpp>
#include <Engine/Scene/Components/RigidBodyComponent.hpp>
#include <Math/MathUtils.hpp>
#include <Physics/RigidBody.hpp>
#include <cmath>

namespace fbzz::scene {

struct CharacterControllerComponent {
    // ── Inspector / Serializer 公開フィールド ───────────────────────────────
    // WHY: ゲームデザイナーが Inspector から調整できるよう Reflect で公開する。

    // ApplyImpulse 直後は velocity.y がまだ物理ステップに反映されていない場合があるため、
    // ジャンプ後この時間は着地判定を無視する。
    float jumpMinAirTime       = 0.2f;
    // 「落下中」とみなす velocity.y の閾値 (m/s)
    float fallVelThreshold     = -0.5f;
    // 着地とみなす |velocity.y| の閾値 (m/s)。落下フェーズ後にこれ以下になったとき着地と判定する。
    float groundVelThreshold   = 0.3f;
    // 崖落ちとみなす velocity.y の閾値 (m/s)。接地中にこれを下回ると空中扱いへ移行する。
    float ledgeFallThreshold   = -1.0f;
    // 歩ける面の法線 Y 成分の最小値。急斜面・壁への接触を接地から除外する。
    float minGroundNormalY     = 0.5f;
    // 物理固定ステップと描画フレームのズレで 1 フレーム接触が欠けても空中扱いにしない猶予時間 (秒)
    float groundContactGrace   = 0.12f;
    // ジャンプ直後は床接触が残るため、その接触で即着地へ戻るのを防ぐ無視時間 (秒)
    float jumpGroundIgnoreTime = 0.12f;
    // 接地中の微小な Y 速度をゼロへ寄せる閾値 (m/s)。着地後の細かい Jump/Fall 遷移を抑える。
    float groundedVelSnap      = 0.35f;
    // 意図的ジャンプ状態の最大継続時間 (秒)。接触補正の微小速度を JumpUp と誤認しないための上限。
    float intentionalJumpMaxTime = 1.0f;

    // ── 読み取り専用状態 (Script から参照する) ──────────────────────────────
    bool  isGrounded        = true;
    // Animator の VerticalSpeed パラメーターへ渡す用途に計算済みの縦速度
    // 接地中・意図的ジャンプでない上昇は 0 に丸められる
    float verticalSpeed     = 0.0f;

    const char* GetTypeName() const { return "Character Controller"; }

    void Reflect(IReflector& r)
    {
        r.Field("Jump Min Air Time",        jumpMinAirTime);
        r.Field("Fall Vel Threshold",       fallVelThreshold);
        r.Field("Ground Vel Threshold",     groundVelThreshold);
        r.Field("Ledge Fall Threshold",     ledgeFallThreshold);
        r.Field("Min Ground Normal Y",      minGroundNormalY);
        r.Field("Ground Contact Grace",     groundContactGrace);
        r.Field("Jump Ground Ignore Time",  jumpGroundIgnoreTime);
        r.Field("Grounded Vel Snap",        groundedVelSnap);
        r.Field("Intentional Jump MaxTime", intentionalJumpMaxTime);
    }

    // ── Script から呼ぶ API ───────────────────────────────────────────────────

    // OnUpdate 先頭で呼ぶ。タイマー更新・接地ステート・速度安定化・verticalSpeed 計算を行う。
    // 前フレームの m_hasGroundContact を参照してから内部でリセットするため、呼び出し順序に注意。
    void Tick(physics::RigidBody* rb, float dt)
    {
        UpdateTimers(dt);
        if (!rb) return;

        const float vy = rb->GetVelocity().y;
        UpdateGrounding(vy, dt);
        StabilizeGroundedVelocity(rb, vy);
        UpdateIntentionalJump(vy, dt);
        verticalSpeed = ComputeVerticalSpeed(vy);
        m_hasGroundContact = false;
    }

    // OnCollisionEnter / OnCollisionStay から呼ぶ。法線が歩ける面なら接地として記録する。
    void RegisterGroundContact(const CollisionInfo& info)
    {
        if (info.contactNormal.y < minGroundNormalY) return;
        if (m_ignoreGroundTimer > 0.0f) return;

        m_hasGroundContact   = true;
        m_groundContactTimer = groundContactGrace;
        isGrounded           = true;
        m_wasFalling         = false;
        m_jumpTimer          = 0.0f;
        m_isIntentionalJump  = false;
        m_intentionalJumpTimer = 0.0f;
        RemoveVelocityIntoGround(info.contactNormal, info.self);
    }

    // ジャンプを実行し内部状態を更新する。rb->ApplyImpulse は呼び出し元で行う。
    // WHY: impulse 値は Script 側が決定するが、ジャンプ後のステート遷移はここで一元管理する。
    void Jump()
    {
        isGrounded           = false;
        m_wasFalling         = false;
        m_hasGroundContact   = false;
        m_jumpTimer          = 0.0f;
        m_groundContactTimer = 0.0f;
        m_ignoreGroundTimer  = jumpGroundIgnoreTime;
        m_isIntentionalJump  = true;
        m_intentionalJumpTimer = 0.0f;
    }

private:
    bool  m_wasFalling           = false;
    bool  m_hasGroundContact     = false;
    bool  m_isIntentionalJump    = false;
    float m_jumpTimer            = 0.0f;
    float m_groundContactTimer   = 0.0f;
    float m_ignoreGroundTimer    = 0.0f;
    float m_intentionalJumpTimer = 0.0f;

    bool HasGroundContact() const
    {
        return m_hasGroundContact && m_ignoreGroundTimer <= 0.0f;
    }

    void UpdateTimers(float dt)
    {
        m_groundContactTimer -= dt;
        if (m_groundContactTimer < 0.0f) m_groundContactTimer = 0.0f;
        m_ignoreGroundTimer  -= dt;
        if (m_ignoreGroundTimer  < 0.0f) m_ignoreGroundTimer  = 0.0f;
    }

    // 接地ステートマシン。接触タイマーと velocity.y を組み合わせて isGrounded を更新する。
    // WHY: 斜面降下時は vy が負になり続けるため、接触タイマーを接地の主判定にする。
    //      ジャンプ弧の頂点誤判定は m_wasFalling フラグで防ぐ。
    void UpdateGrounding(float vy, float dt)
    {
        const bool hasRecentContact = m_groundContactTimer > 0.0f && m_ignoreGroundTimer <= 0.0f;

        if (isGrounded) {
            if (hasRecentContact) {
                m_wasFalling = false;
                m_jumpTimer  = 0.0f;
                m_isIntentionalJump    = false;
                m_intentionalJumpTimer = 0.0f;
                return;
            }
            if (vy < ledgeFallThreshold) {
                isGrounded   = false;
                m_wasFalling = false;
                m_jumpTimer  = 0.0f;
            }
        } else {
            m_jumpTimer += dt;
            if (HasGroundContact() && m_jumpTimer >= jumpMinAirTime) {
                isGrounded   = true;
                m_wasFalling = false;
                m_jumpTimer  = 0.0f;
                m_isIntentionalJump    = false;
                m_intentionalJumpTimer = 0.0f;
                return;
            }
            if (m_jumpTimer >= jumpMinAirTime && vy < fallVelThreshold)
                m_wasFalling = true;
            if (m_wasFalling && std::fabsf(vy) < groundVelThreshold) {
                isGrounded   = true;
                m_wasFalling = false;
                m_jumpTimer  = 0.0f;
                m_isIntentionalJump    = false;
                m_intentionalJumpTimer = 0.0f;
            }
        }
    }

    // 接地中の微小な Y 速度をゼロへ寄せる。大きい降下速度は斜面追従のために残す。
    void StabilizeGroundedVelocity(physics::RigidBody* rb, float vy) const
    {
        if (!isGrounded || !HasGroundContact()) return;
        if (std::fabsf(vy) > groundedVelSnap) return;

        math::Vector3 vel = rb->GetVelocity();
        vel.y = 0.0f;
        rb->SetVelocity(vel);
    }

    // 意図的ジャンプ状態の更新。接触補正の微小速度を JumpUp と誤認しないためのフラグ管理。
    void UpdateIntentionalJump(float vy, float dt)
    {
        if (!m_isIntentionalJump) return;
        m_intentionalJumpTimer += dt;
        if (isGrounded || vy < fallVelThreshold || m_intentionalJumpTimer >= intentionalJumpMaxTime) {
            m_isIntentionalJump    = false;
            m_intentionalJumpTimer = 0.0f;
        }
    }

    // Animator の VerticalSpeed へ渡す値を計算する。
    // WHAT: 接地中は 0。入力由来でない上昇も 0 に丸める。落下速度は Fall 判定へそのまま渡す。
    float ComputeVerticalSpeed(float vy) const
    {
        if (isGrounded) return 0.0f;
        if (vy > 0.0f && !m_isIntentionalJump) return 0.0f;
        return vy;
    }

    // 接地面に垂直な速度成分 (正負両方向) を除去する。
    // WHY: Baumgarte 補正が貫通深度に応じて上向きバウンスを生じさせるため、
    //      負方向だけでなく正方向も除去して Landing 直後の再浮遊を防ぐ。
    void RemoveVelocityIntoGround(const math::Vector3& normal, GameObject* go) const
    {
        if (!go) return;
        auto* rbc = go->GetComponent<RigidBodyComponent>();
        if (!rbc || !rbc->enabled || !rbc->rigidBody) return;

        math::Vector3 vel = rbc->rigidBody->GetVelocity();
        if (normal.LengthSq() <= math::EPSILON) return;

        const math::Vector3 n = normal.Normalized();
        const float dot = math::Vector3::Dot(vel, n);
        if (std::fabsf(dot) < math::EPSILON) return;

        rbc->rigidBody->SetVelocity(vel - n * dot);
    }
};

} // namespace fbzz::scene
