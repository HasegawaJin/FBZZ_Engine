/// @file    CharacterControllerComponent.hpp
/// @brief   キャラクター移動に必要な接地検出・ジャンプ状態管理をエンジン側に集約するコンポーネント。
/// @author  Hasegawa Jin
/// @date    2026-05-31
///
/// WHY: 接地判定・速度安定化・タイマー管理はゲームロジックではなく物理システム隣接の汎用処理であり、
/// Script 内に書くと同じステートマシンを全キャラクターで重複実装することになる。
/// Unity の CharacterController に相当するが、このエンジンは RigidBody ベースの物理を使うため
/// 内部実装は velocity 操作による制御となる。
///
/// 使い方:
/// 1. GameObject に RigidBodyComponent と一緒にアタッチする
/// 2. Script の OnUpdate 先頭で Tick(rb, dt) を呼ぶ
/// 3. 物理イベントから接地通知されるため、通常は衝突コールバックを書く必要はない
/// 4. 特殊な接地を追加する場合だけ RegisterGroundContact(info) を呼ぶ
/// 5. ジャンプ時は Jump(rb, impulse) を呼ぶ
/// 6. isGrounded / verticalSpeed を読んで Animator を操作する
#pragma once

#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Script.hpp>
#include <Engine/Scene/Components/RigidBodyComponent.hpp>
#include <Math/MathUtils.hpp>
#include <Physics/RigidBody.hpp>
#include <algorithm>
#include <cmath>

namespace fbzz::scene {

// 接地判定の責務を自動判定へ固定せず、特殊移動を実装する Script が状態を
// 明示的に制御できるようにする。Forced 系は梯子・飛行・ノックバックなどで使う。
enum class CharacterGroundingMode {
    Automatic = 0,
    ForcedGrounded,
    ForcedAirborne,
};

struct CharacterControllerComponent {
    bool enabled = true;

    CharacterGroundingMode groundingMode = CharacterGroundingMode::Automatic;

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
    // 最後に検出した歩行可能面の法線。坂に応じた姿勢制御など、接地方向を必要とする処理で参照する。
    math::Vector3 groundNormal = math::Vector3::UP;

    const char* GetTypeName() const { return "Character Controller"; }

    void Reflect(IReflector& r)
    {
        // WHY BeginField で «保存キー» と «表示名» を分けるか:
        //   以前は r.Field("Jump Min Air Time", ...) のように表示名をそのまま保存キーに
        //   していた。.scene は jumpMinAirTime で書かれているので、Reflect を通る経路
        //   (AI バス・汎用 Inspector・スナップショット) だけが別のキーを見ることになり、
        //   同じ 1 つの値が経路によって «あるのに無い» ように振る舞っていた。
        //   保存キーはフィールド名に揃え、読みやすい名前は表示側だけに置く。
        r.BeginField("enabled", "Enabled");
        r.Field("enabled", enabled);
        r.EndField();
        {
            static constexpr const char* kGroundingModeLabels[] = {
                "Automatic", "Forced Grounded", "Forced Airborne"
            };
            int mode = static_cast<int>(groundingMode);
            r.BeginField("groundingMode", "Grounding Mode");
            r.Enum("Grounding Mode", mode, kGroundingModeLabels);
            r.EndField();
            mode = std::clamp(mode, 0, 2);
            groundingMode = static_cast<CharacterGroundingMode>(mode);
        }

        // 保存キー / 表示名 / 値 の 3 つ組。並べて書くと «キーだけ直し忘れる» が起きにくい。
        const auto tuned = [&r](const char* key, const char* display, float& value) {
            r.BeginField(key, display);
            r.Field(display, value);
            r.EndField();
        };
        tuned("jumpMinAirTime",         "Jump Min Air Time",        jumpMinAirTime);
        tuned("fallVelThreshold",       "Fall Vel Threshold",       fallVelThreshold);
        tuned("groundVelThreshold",     "Ground Vel Threshold",     groundVelThreshold);
        tuned("ledgeFallThreshold",     "Ledge Fall Threshold",     ledgeFallThreshold);
        tuned("minGroundNormalY",       "Min Ground Normal Y",      minGroundNormalY);
        tuned("groundContactGrace",     "Ground Contact Grace",     groundContactGrace);
        tuned("jumpGroundIgnoreTime",   "Jump Ground Ignore Time",  jumpGroundIgnoreTime);
        tuned("groundedVelSnap",        "Grounded Vel Snap",        groundedVelSnap);
        tuned("intentionalJumpMaxTime", "Intentional Jump MaxTime", intentionalJumpMaxTime);

        // 接地は実行時の状態だが .scene に保存されていた。読む側が既定 true なので
        // 落としても実害は無いが、«保存されていたものが黙って消える» を避けて残す。
        // Inspector には出さない (触れる値ではない)。
        r.BeginField("isGrounded", "Is Grounded");
        r.SetFieldHidden(true);
        r.Field("isGrounded", isGrounded);
        r.EndField();
    }

    // ── Script から呼ぶ API ───────────────────────────────────────────────────

    // OnUpdate 先頭で呼ぶ。タイマー更新・接地ステート・速度安定化・verticalSpeed 計算を行う。
    // 前フレームの m_hasGroundContact を参照してから内部でリセットするため、呼び出し順序に注意。
    void Tick(physics::RigidBody* rb, float dt)
    {
        if (!enabled) {
            isGrounded = false;
            verticalSpeed = 0.0f;
            m_hasGroundContact = false;
            m_groundContactTimer = 0.0f;
            return;
        }

        UpdateTimers(dt);
        if (!rb) return;

        const float vy = rb->GetVelocity().y;
        if (groundingMode == CharacterGroundingMode::ForcedGrounded) {
            isGrounded = true;
        } else if (groundingMode == CharacterGroundingMode::ForcedAirborne) {
            isGrounded = false;
        } else {
            UpdateGrounding(vy, dt);
        }
        StabilizeGroundedVelocity(rb, vy);
        UpdateIntentionalJump(vy, dt);
        verticalSpeed = ComputeVerticalSpeed(vy);
        m_hasGroundContact = false;
    }

    // PhysicsSystem または特殊な移動 Script から呼ぶ。法線が歩ける面なら接地として記録する。
    void RegisterGroundContact(const CollisionInfo& info)
    {
        if (!enabled || groundingMode == CharacterGroundingMode::ForcedAirborne) return;
        // 縮退した接触法線は「どちらが上か」を持たない。接地とも壁とも判定できないので、
        // 猶予タイマーに任せてこのフレームは無視する。
        if (info.contactNormal.LengthSq() < math::EPSILON * math::EPSILON) return;
        const math::Vector3 contactNormal = info.contactNormal.Normalized();
        if (contactNormal.y < minGroundNormalY)
        {
            // 歩行不可面でも、水平成分を次の入力ステップの壁制約へ引き継ぐ。
            // WHY: ここを単に接地対象外として捨てると、入力が壁方向の速度を毎回
            //      再注入し、PhysicsSolver の法線インパルスと競合して振動する。
            math::Vector3 blockingNormal = contactNormal;
            blockingNormal.y = 0.0f;
            if (blockingNormal.LengthSq() > math::EPSILON)
            {
                m_blockingContactNormal = blockingNormal.Normalized();
                m_blockingContactTimer = groundContactGrace;
            }
            return;
        }
        if (m_ignoreGroundTimer > 0.0f) return;

        m_hasGroundContact   = true;
        m_groundContactTimer = groundContactGrace;
        isGrounded           = true;
        m_wasFalling         = false;
        m_jumpTimer          = 0.0f;
        m_isIntentionalJump  = false;
        m_intentionalJumpTimer = 0.0f;
        groundNormal = contactNormal;
        RemoveVelocityIntoGround(info.contactNormal, info.self);
    }

    // ジャンプを実行し、インパルス適用とステート遷移をまとめて行う。
    // WHY: 旧 Jump() は呼び出し元が rb->ApplyImpulse() と Jump() の順序を保証する必要があり、
    //      順序を誤ると jumpGroundIgnoreTime タイマーがずれて着地判定が即発動した。
    //      rb と impulse をここに渡すことで正しい順序をこのメソッドが保証する。
    void Jump(physics::RigidBody* rb, const math::Vector3& impulse)
    {
        if (!enabled) return;

        isGrounded           = false;
        m_wasFalling         = false;
        m_hasGroundContact   = false;
        m_jumpTimer          = 0.0f;
        m_groundContactTimer = 0.0f;
        m_ignoreGroundTimer  = jumpGroundIgnoreTime;
        m_isIntentionalJump  = true;
        m_intentionalJumpTimer = 0.0f;
        groundNormal         = math::Vector3::UP;
        // ステート更新後にインパルスを適用する。逆順だと velocity.y が古い値のまま
        // m_ignoreGroundTimer セット前の Contact 判定に入る余地が生まれる。
        if (rb) rb->ApplyImpulse(impulse);
    }

    // 質量やインパルス単位を意識せず、目標の上向き速度 (m/s) でジャンプする。
    // 既存の Jump() は爆発など任意方向の物理インパルス用として残す。
    void JumpAtVelocity(physics::RigidBody* rb, float verticalSpeed)
    {
        const float currentVerticalSpeed = rb ? rb->GetVelocity().y : 0.0f;
        const float mass = rb ? rb->GetMass() : 0.0f;
        Jump(rb, math::Vector3::UP * ((verticalSpeed - currentVerticalSpeed) * mass));
    }

    // 接地モードを明示的に切り替える。Automatic に戻すと通常の接触判定へ復帰する。
    void SetGroundingMode(CharacterGroundingMode mode)
    {
        groundingMode = mode;
        if (mode == CharacterGroundingMode::ForcedGrounded) {
            isGrounded = true;
        } else if (mode == CharacterGroundingMode::ForcedAirborne) {
            isGrounded = false;
            m_hasGroundContact = false;
            m_groundContactTimer = 0.0f;
        }
    }

    void ForceGrounded(const math::Vector3& normal = math::Vector3::UP)
    {
        groundingMode = CharacterGroundingMode::ForcedGrounded;
        isGrounded = true;
        groundNormal = normal.LengthSq() > math::EPSILON
            ? normal.Normalized() : math::Vector3::UP;
    }

    void ForceAirborne()
    {
        SetGroundingMode(CharacterGroundingMode::ForcedAirborne);
        groundNormal = math::Vector3::UP;
    }

    void UseAutomaticGrounding()
    {
        groundingMode = CharacterGroundingMode::Automatic;
    }

    // 水平移動の最小プリミティブ。Y 速度は重力・ジャンプ・物理解決へ残す。
    void SetHorizontalVelocity(physics::RigidBody* rb, const math::Vector3& velocity) const
    {
        if (!enabled || !rb || rb->IsStatic()) return;
        math::Vector3 current = rb->GetVelocity();
        current.x = velocity.x;
        current.z = velocity.z;
        rb->SetVelocity(current);
    }

    void AddHorizontalVelocity(physics::RigidBody* rb, const math::Vector3& velocity) const
    {
        if (!enabled || !rb || rb->IsStatic()) return;
        math::Vector3 current = rb->GetVelocity();
        current.x += velocity.x;
        current.z += velocity.z;
        rb->SetVelocity(current);
    }

    // desiredVelocity へ向けて水平速度だけを補間する。
    // acceleration / deceleration が負なら、その側は即時設定になるため、
    // プレイヤー・敵 AI・回避で異なる応答を呼び出し側が選べる。
    void Move(physics::RigidBody* rb,
              const math::Vector3& desiredVelocity,
              float dt,
              float acceleration = -1.0f,
              float deceleration = -1.0f) const
    {
        if (!enabled || !rb || rb->IsStatic()) return;

        math::Vector3 constrainedVelocity = desiredVelocity;
        if (isGrounded && m_groundContactTimer > 0.0f)
            RemoveVelocityIntoSurface(constrainedVelocity, groundNormal);
        if (m_blockingContactTimer > 0.0f)
            RemoveVelocityIntoSurface(constrainedVelocity, m_blockingContactNormal);

        math::Vector3 current = rb->GetVelocity();
        const float currentSpeed = std::sqrtf(current.x * current.x + current.z * current.z);
        const float desiredSpeed = std::sqrtf(
            constrainedVelocity.x * constrainedVelocity.x +
            constrainedVelocity.z * constrainedVelocity.z);
        const float rate = desiredSpeed > currentSpeed ? acceleration : deceleration;

        if (rate < 0.0f || dt <= 0.0f) {
            current.x = constrainedVelocity.x;
            current.z = constrainedVelocity.z;
            rb->SetVelocity(current);
            return;
        }

        const float maxDelta = rate * dt;
        const float dx = constrainedVelocity.x - current.x;
        const float dz = constrainedVelocity.z - current.z;
        const float deltaLength = std::sqrtf(dx * dx + dz * dz);
        if (deltaLength <= maxDelta || deltaLength <= math::EPSILON) {
            current.x = constrainedVelocity.x;
            current.z = constrainedVelocity.z;
        } else {
            const float scale = maxDelta / deltaLength;
            current.x += dx * scale;
            current.z += dz * scale;
        }
        rb->SetVelocity(current);
    }

    [[nodiscard]] math::Vector3 GetVelocity(const physics::RigidBody* rb) const
    {
        return rb ? rb->GetVelocity() : math::Vector3::ZERO;
    }

    [[nodiscard]] math::Vector3 GetHorizontalVelocity(const physics::RigidBody* rb) const
    {
        math::Vector3 velocity = GetVelocity(rb);
        velocity.y = 0.0f;
        return velocity;
    }

private:
    bool  m_wasFalling           = false;
    bool  m_hasGroundContact     = false;
    bool  m_isIntentionalJump    = false;
    float m_jumpTimer            = 0.0f;
    float m_groundContactTimer   = 0.0f;
    float m_blockingContactTimer = 0.0f;
    float m_ignoreGroundTimer    = 0.0f;
    float m_intentionalJumpTimer = 0.0f;
    math::Vector3 m_blockingContactNormal = math::Vector3::ZERO;

    bool HasGroundContact() const
    {
        return m_hasGroundContact && m_ignoreGroundTimer <= 0.0f;
    }

    void UpdateTimers(float dt)
    {
        m_groundContactTimer -= dt;
        if (m_groundContactTimer < 0.0f) m_groundContactTimer = 0.0f;
        m_blockingContactTimer -= dt;
        if (m_blockingContactTimer < 0.0f) m_blockingContactTimer = 0.0f;
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

    // 入力目標から接触面へ食い込む速度成分だけを除去する。
    // WHY: FixedScript は毎ステップ入力速度を再設定するため、物理解決が除去した
    //      壁・斜面方向の速度をそのまま渡すと、入力と衝突解決が交互に同じ成分を
    //      注入・除去し、前後方向のジッターになる。
    void RemoveVelocityIntoSurface(math::Vector3& velocity,
                                   const math::Vector3& surfaceNormal) const
    {
        if (surfaceNormal.LengthSq() <= math::EPSILON) return;
        const math::Vector3 normal = surfaceNormal.Normalized();
        const float intoSurface = math::Vector3::Dot(velocity, normal);
        if (intoSurface < 0.0f)
            velocity -= normal * intoSurface;
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
