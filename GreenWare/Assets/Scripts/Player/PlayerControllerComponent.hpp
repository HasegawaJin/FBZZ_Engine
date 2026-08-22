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
#include <Scripts/Game/CameraFollowManagerComponent.hpp>
#include <Scripts/Game/ImpactFeedbackManagerComponent.hpp>
#include <Scripts/Player/PlayerAimComponent.hpp>
#include <Scripts/Player/WeaponRigComponent.hpp>
#include <Scripts/Utils/InputActions.hpp>
#include <Scripts/Utils/WeaponSockets.hpp>
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

    // WHY キーバインドの項目を持たないか:
    //   物理入力を Inspector に持つと、その 1 行のためにゲームパッド対応も
    //   キーコンフィグも原理的に不可能になる (InputBinding.hpp の設計意図)。
    //   このスクリプトは論理名だけを知り、実際の割り当ては
    //   ProjectSettings/Input.inputactions が持つ。名前は InputActions.hpp を参照。

    FBZZ_GROUP("Animator Params")
    FBZZ_FIELD(std::string, paramSpeed,         "Speed",        "Speed Param")
    FBZZ_TOOLTIP("移動の速さ (m/s)。上半身の Aim Sway の強さがこれで決まる")
    // ロコモーションの 2D ブレンド。モデル前方 +Z を Y、右 +X を X に取る。
    // WHY 速さと別に持つか: 銃を抜いている間は体が照準を向くため、進行方向は
    //     体の正面と一致しない。脚だけが進行方向へ合う必要があり、それは
    //     「どの向きへ」という 2 軸でしか表せない。
    FBZZ_FIELD(std::string, paramMoveX,         "MoveX",        "Move X Param")
    FBZZ_FIELD(std::string, paramMoveY,         "MoveY",        "Move Y Param")
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
    // 9 セルの角度は UpperArm_R 起点で実測したもの。足元から測ると、同じ高さに
    // 立っている敵でもピッチが下向きに出て、腕が地面へ向く。
    FBZZ_FIELD(std::string, aimOriginBoneName, "Chest", "Aim Origin Bone")
    FBZZ_TOOLTIP("腕の付け根。見つからなければプレイヤー原点から測る")
    // Docs/conventions/minibot-animation-export.md の実測コーン。
    // ポーズを作り直したらここも入れ替える。
    FBZZ_FIELD_RANGE(float, aimYawRangeDegrees,  45.0f, "Aim Yaw Range",  5.0f, 90.0f)
    FBZZ_FIELD_RANGE(float, aimPitchUpDegrees,   39.0f, "Aim Pitch Up",   5.0f, 90.0f)
    FBZZ_FIELD_RANGE(float, aimPitchDownDegrees, 34.0f, "Aim Pitch Down", 5.0f, 90.0f)
    FBZZ_FIELD_RANGE(float, aimResponse, 14.0f, "Aim Response", 0.0f, 40.0f)
    FBZZ_TOOLTIP("ポーズが新しい角度へ追い付く速さ。0 で補間せず即座に切り替える")
    FBZZ_FIELD(bool, turnToAim, true, "Turn To Aim")
    FBZZ_TOOLTIP("コーンを超えたヨーを体の旋回で吸収する (移動入力が無いときだけ)")
    // WHY コーン (45 度) の際まで上げるか:
    //   旋回が収束した先が、そのまま「立ち止まって狙っているときの姿勢」になる。
    //   30 度だとポーズの 2/3 しか使われず、実測配分 (Chest 30 度 / UpperArm 15 度) の
    //   胴体ひねりも 2/3 に留まる。値を上げるほど腰が据わったまま上体だけがねじれ、
    //   下げるほど体ごと正対して上体のひねりが消える。狙っている姿に見せたいのは前者。
    FBZZ_FIELD_RANGE(float, aimTurnDeadzoneDegrees, 38.0f, "Aim Turn Deadzone", 0.0f, 90.0f)
    FBZZ_TOOLTIP("この角度までは体を回さず、9 セルのポーズだけで狙う。"
                 "Aim Yaw Range に近いほど上体のひねりが深くなる")

    FBZZ_GROUP("Jump (derived)")
    // 重力は ProjectSettings の [physics] gravity、到達点は PlayerTuning。
    // 導出結果をここに出さないと、重力を触った影響が Play するまで分からない。
    FBZZ_FIELD_READ_ONLY(float, debugWorldGravity, 0.0f, "World Gravity")
    FBZZ_FIELD_READ_ONLY(float, debugJumpSpeed, 0.0f, "Jump Speed")
    FBZZ_FIELD_READ_ONLY(float, debugTimeToApex, 0.0f, "Time To Apex")
    FBZZ_FIELD(bool, autoTuneGrounding, true, "Auto Tune Grounding")
    FBZZ_TOOLTIP("接地判定のしきい値を重力から決め直す。"
                 "切ると CharacterController に入っている値をそのまま使う")

    FBZZ_GROUP("Camera Feedback")
    // WHY カメラ側ではなくここに置くか: 「跳ぶとどれくらい緩めたいか」は動作を持つ
    //     こちらの意図で、「緩んだときカメラがどう動くか」はカメラの作り。
    //     混ぜると、カメラの追従方式を変えるたびに動作側の数値を入れ直すことになる。
    FBZZ_FIELD_RANGE(float, airCameraSlack, 0.85f, "Air Slack (Vertical)", 0.0f, 1.0f)
    FBZZ_TOOLTIP("空中にいる間、カメラの縦追従を緩める量。0 で従来どおり密着する")
    FBZZ_FIELD_RANGE(float, dodgeCameraSlack, 0.7f, "Dodge Slack (Horizontal)", 0.0f, 1.0f)
    FBZZ_TOOLTIP("回避中、カメラの横追従を緩める量")
    // WHY 走行中も緩めるか: 密着したままだと、走っていても画面の中でプレイヤーは
    //     止まって見え、動いているのは背景だけになる。緩めるとカメラが遅れ、
    //     進行方向へプレイヤーが流れる。クロスヘアは画面中央に固定なので、
    //     「自分は流れるが狙う点は動かない」という関係もそのまま絵に出る。
    FBZZ_FIELD_RANGE(float, moveCameraSlack, 0.45f, "Move Slack (Horizontal)", 0.0f, 1.0f)
    FBZZ_TOOLTIP("最高速で走っているとき、カメラの横追従を緩める量。"
                 "速度に比例するので歩き出しではほとんど緩まない。0 で従来どおり密着する")
    FBZZ_FIELD_RANGE(float, landFeedbackSpeed, 12.0f, "Land Feedback At", 0.0f, 60.0f)
    FBZZ_TOOLTIP("この落下速度 (m/s) で着地の反応が最大になる。0 で着地演出を切る")

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

    // 見た目の正面を +Z とする回転。transform.worldRotation はモデルのヨーオフセットを
    // 含むため、そのまま使うと「敵が正面に居るのに腕が真後ろを向く」になる。
    // 公開しているのは、狙いの向きを扱う他のモジュールが同じヨーオフセットを
    // 二重に持たないため。オフセットの正本は Model Yaw Offset ただ 1 つ。
    [[nodiscard]] Quaternion ModelRotation() const;

private:
    // 回避の開始判定と、回避中の速度上書き。
    void TickDodge(const Vector3& moveDirection, bool hasInput,
                   bool dodgeRequested, RigidBody& phy, float dt);
    void UpdateIK();
    // 進行方向をモデルローカルの 2 軸へ落とし、脚の 2D ブレンドへ渡す。
    void UpdateLocomotionParameters();
    void UpdateAimParameters();
    // 上り / 下り / 早離しで重力を切り替える。倍率は World の重力に対する比。
    void UpdateGravityScale(const CharacterControllerComponent& cc, const RigidBody& phy);
    // 接地判定のしきい値を、今の重力と跳躍の長さから決め直す。
    void TuneGrounding(CharacterControllerComponent& cc) const;
    // 空中・回避・走行のあいだ、カメラの追従を緩めるよう要求する。
    void RequestCameraSlack(const CharacterControllerComponent& cc, float planarSpeed);
    // 空中から接地へ変わった瞬間を掴み、落下速度の分だけ手触りを鳴らす。
    void TickLanding(const CharacterControllerComponent& cc, const RigidBody& phy);
    // コーンを超えたヨーを体の旋回で吸収する。ポーズだけでは ±45 度しか向けない。
    void TickAimTurn(RigidBody& phy, float dt);
    void UpdateSlopeLean(IKSolverComponent& ik, CharacterControllerComponent* cc,
                         RigidBodyComponent* rb);

    void CacheAimOrigin();
    // 照準が今どこを指しているか。6.4 でロックオンを廃したため、腕が向く先は
    // 「選ばれた敵」ではなく「線の先」になる。敵が居ない方向を狙っていても腕は付いてくる。
    [[nodiscard]] bool AimPointWorld(Vector3& outPoint);
    // 狙点を、モデルの正面 (+Z) から見た角度 (度) へ落とす。
    [[nodiscard]] bool ResolveAimAngles(float& outYawDegrees, float& outPitchDegrees);
    [[nodiscard]] Vector3 AimOriginWorld();
    Vector3 GetMoveForward() const;
    Vector3 GetMoveRight(const Vector3& forward) const;

    // 移動・回避の数値は必須 PlayerTuning からのみ読む。フォールバックを持たせないことで、
    // fzdata の未設定を「たまたま動く」状態にしない。
    [[nodiscard]] float MoveSpeed()     const { return tuning->moveSpeed; }
    [[nodiscard]] float GroundAccel()   const { return tuning->groundAccel; }
    [[nodiscard]] float GroundDecel()   const { return tuning->groundDecel; }
    [[nodiscard]] float AirAccel()      const { return tuning->airAccel; }
    [[nodiscard]] float TurnSpeed()     const { return tuning->turnSpeed; }
    // ProjectSettings の [physics] gravity が重力の正本。毎回読み直すので、
    // 設定を変えたぶんがそのまま跳躍の導出へ効く。
    [[nodiscard]] float WorldGravity() const
    {
        const float gravity = physics.GetWorldGravity().Length();
        return gravity > EPSILON ? gravity : 9.81f;
    }
    // 到達点 h を重力 g のもとで満たす初速。v0 = sqrt(2gh)。
    [[nodiscard]] float JumpSpeed() const
    {
        return std::sqrtf(2.0f * WorldGravity() * std::max(tuning->jumpApexHeight, 0.0f));
    }
    [[nodiscard]] float DodgeSpeed()    const { return tuning->dodgeSpeed; }
    [[nodiscard]] float DodgeDuration() const { return tuning->dodgeDuration; }
    [[nodiscard]] float DodgeCooldown() const { return tuning->dodgeCooldown; }

    PlayerAimComponent* m_aimOverride = nullptr;
    WeaponRigComponent* m_weaponRig   = nullptr;

    // 腕の付け根のボーン。毎フレーム名前で探すと 58 ボーンを走査することになる。
    EntityRef m_aimOrigin;
    // Animator へ送っている今の値 (-1..1)。角度そのものではなくセル座標で持つ。
    float m_aimYaw   = 0.0f;
    float m_aimPitch = 0.0f;

    bool m_hasSpineTargetBase = false;
    Vector3 m_spineTargetBase = Vector3::ZERO;
    Vector3 m_smoothedSpineOffset = Vector3::ZERO;
    // 回避の残り時間と、次に回避できるまでの残り時間。
    float   m_dodgeRemaining = 0.0f;
    float   m_dodgeCooldown  = 0.0f;
    Vector3 m_dodgeDirection = Vector3::ZERO;
    // 可変フレームで採取した入力を、次の固定ステップで一度だけ物理へ適用する。
    Vector3 m_moveDirection = Vector3::ZERO;
    // スティックの倒し量 0..1。キーボードは押していれば常に 1。
    float   m_moveMagnitude = 0.0f;
    bool    m_hasMoveInput = false;
    bool    m_jumpRequested = false;
    bool    m_dodgeRequested = false;

    // ジャンプキーを押している間だけ上りの重力を軽いままにする。
    bool  m_jumpHeld = false;
    // 前フレームの接地。着地の「瞬間」は状態そのものではなく変化でしか掴めない。
    bool  m_wasGrounded = true;
    // 接地判定が立つ前に速度は 0 へ寄せられるため、着地の強さは
    // 空中で見ていた最大の落下速度から測る。
    float m_peakFallSpeed = 0.0f;
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
    m_moveMagnitude  = 0.0f;
    m_hasMoveInput   = false;
    m_jumpRequested  = false;
    m_dodgeRequested = false;
    m_aimYaw         = 0.0f;
    m_aimPitch       = 0.0f;
    m_jumpHeld       = false;
    m_wasGrounded    = true;
    m_peakFallSpeed  = 0.0f;

    if (auto* cc = scene.GetComponent<CharacterControllerComponent>())
        TuneGrounding(*cc);

    CacheAimOrigin();
}

inline void PlayerControllerComponent::OnUpdate()
{
    if (!enabled || !transform || !tuning) return;
    const Vector3 forward = GetMoveForward();
    const Vector3 right   = GetMoveRight(forward);

    // WASD と左スティックの両方がここへ合流する。半径方向のデッドゾーンも
    // アクション層が済ませているので、斜めに倒したときの実効感度が方向で変わらない。
    const Vector2 axis = input.GetMoveAxis();
    // WHY 倒し量を残すか: 正規化だけしてしまうと、スティックを半分倒しても
    //     全力疾走になり、パッドでの歩き / 走りの作り分けが消える。
    //     キーボードは常に 1.0 になるので、従来の挙動は変わらない。
    const float amount = Min(axis.Length(), 1.0f);

    m_hasMoveInput  = amount > EPSILON;
    m_moveMagnitude = amount;
    m_moveDirection = m_hasMoveInput
        ? (right * axis.x + forward * axis.y).Normalized()
        : Vector3::ZERO;

    m_jumpRequested  = m_jumpRequested  || input.GetActionDown(actions::kJump);
    m_dodgeRequested = m_dodgeRequested || input.GetActionDown(actions::kDodge);
    // 押しっぱなしかどうかは押した瞬間では分からない。上昇中の重力を決めるために
    // 保持状態そのものを持つ。可変フレーム側で採り、固定ステップ側で使う。
    m_jumpHeld = input.GetAction(actions::kJump);
    // 入力はここまで。抜く / 収める の中身は WeaponRigComponent の担当で、
    // このスクリプトは銃の存在もソケットも知らない。
    if (m_weaponRig) {
        if (input.GetActionDown(actions::kDrawWeapons))    m_weaponRig->RequestDraw();
        if (input.GetActionDown(actions::kHolsterWeapons)) m_weaponRig->RequestHolster();
        // パッドはボタンが足りないので 1 つで往復させる。どちらへ動かすかは
        // 押した側ではなく今の状態が決める。
        if (input.GetActionDown(actions::kToggleWeapons)) {
            if (m_weaponRig->IsDrawn()) m_weaponRig->RequestHolster();
            else                        m_weaponRig->RequestDraw();
        }
    }

    auto* cc = scene.GetComponent<CharacterControllerComponent>();
    auto* rb = scene.GetComponent<RigidBodyComponent>();
    auto* phy = rb && rb->enabled && rb->rigidBody ? rb->rigidBody.get() : nullptr;
    if (cc) {
        animator.SetFloat(paramVerticalSpeed, cc->verticalSpeed);
        animator.SetBool(paramIsGrounded, cc->isGrounded);
    }
    const Vector3 velocity = phy ? phy->GetVelocity() : Vector3::ZERO;
    // 水平の速さ。歩行アニメーションの再生速度と、カメラをどれだけ緩めるかの
    // 両方がこれで決まる。2 箇所で別々に測ると、走っている判定が食い違う。
    const float planarSpeed = IsDodging()
        ? DodgeSpeed()
        : std::sqrtf(velocity.x * velocity.x + velocity.z * velocity.z);
    animator.SetFloat(paramSpeed, planarSpeed);
    UpdateLocomotionParameters();
    UpdateAimParameters();
    UpdateIK();

    // 手触りの出力は可変フレーム側で回す。固定ステップは 1 フレームに 0 回にも
    // 複数回にもなるため、揺れや緩みの要求をそこへ置くと回数が絵に依存する。
    if (cc && phy) {
        RequestCameraSlack(*cc, planarSpeed);
        TickLanding(*cc, *phy);
    }

    // 重力と到達点から導かれる値を Inspector へ返す。ProjectSettings を触ったとき、
    // 跳躍がどう変わったかをここで読めるようにしておく。
    const float gravity = WorldGravity();
    debugWorldGravity = gravity;
    debugJumpSpeed    = JumpSpeed();
    debugTimeToApex   = debugJumpSpeed / gravity;
}

inline void PlayerControllerComponent::RequestCameraSlack(
    const CharacterControllerComponent& cc, float planarSpeed)
{
    auto* follow = CameraFollowManagerComponent::Instance();
    if (!follow) return;

    // WHY 速さに比例させるか: 二値で切り替えると、歩き出した瞬間にカメラが
    //     どさっと置いていかれる。最高速で最大になる比例なら、加速がそのまま
    //     画面内のずれの増え方になり、走り出しの重さとして読める。
    const float moveSlack = moveCameraSlack * Clamp01(planarSpeed / Max(MoveSpeed(), EPSILON));

    const float vertical = cc.isGrounded ? 0.0f : airCameraSlack;
    // 回避と走行はどちらも横。強いほうがその軸を決める (合成規則は Loosen と同じ)。
    const float horizontal = Max(IsDodging() ? dodgeCameraSlack : 0.0f, moveSlack);
    if (vertical <= 0.0f && horizontal <= 0.0f) return;

    // WHY 状態が続く間ずっと短い要求を出し直すか: 「解除」を別に呼ぶ作りにすると、
    //     着地を取りこぼした 1 回でカメラが緩んだまま戻らなくなる。
    //     期限付きを毎フレーム更新すれば、呼ばなくなった時点が解除になる。
    constexpr float kSustainSeconds = 0.12f;
    follow->Loosen(horizontal, vertical, kSustainSeconds);
}

inline void PlayerControllerComponent::TickLanding(const CharacterControllerComponent& cc,
                                                   const RigidBody& phy)
{
    if (!cc.isGrounded) {
        m_peakFallSpeed = std::max(m_peakFallSpeed, -phy.GetVelocity().y);
        m_wasGrounded   = false;
        return;
    }

    // 空中から接地へ変わったフレームだけ鳴らす。接地している間ずっと鳴らすと、
    // 坂を降りるだけで揺れ続ける。
    if (!m_wasGrounded && landFeedbackSpeed > 0.0f) {
        const float strength = Clamp01(m_peakFallSpeed / landFeedbackSpeed);
        if (auto* feedback = ImpactFeedbackManagerComponent::Instance())
            feedback->Play(FeedbackEvent::PlayerLand, strength);
    }
    m_wasGrounded   = true;
    m_peakFallSpeed = 0.0f;
}

inline void PlayerControllerComponent::UpdateLocomotionParameters()
{
    if (!m_hasMoveInput) {
        // 原点へ倒せば 2D ブレンドは Idle になる。方向は残さない
        // (残すと止まった瞬間に最後の向きの歩きが薄く残る)。
        animator.SetFloat(paramMoveX, 0.0f);
        animator.SetFloat(paramMoveY, 0.0f);
        return;
    }

    // モデルの正面 (+Z) から見た進行方向。ResolveAimAngles と同じ基準を使う。
    // WHY worldRotation ではなく ModelRotation() か: モデルは 180 度回してあり、
    //     生の回転で割ると前後左右が入れ替わる。
    const Vector3 local = ModelRotation().Inverse() * m_moveDirection;

    // 半径が速さの輪を選ぶ。半分倒せば Walk の輪、いっぱいで Run の輪。
    // 6.1 のなぞりは走りながら行うので、キーボード (常に 1.0) は Run のままになる。
    animator.SetFloat(paramMoveX, local.x * m_moveMagnitude);
    animator.SetFloat(paramMoveY, local.z * m_moveMagnitude);
}

inline void PlayerControllerComponent::UpdateAimParameters()
{
    // 銃を収納している間は、エイム差分と歩行中の上半身揺れを無効にする。
    // レイヤー自体は Controller に常駐させ、再び抜いたときの再構築コストを避ける。
    // 「抜いているか」は銃の側の事実なので、WeaponRigComponent に聞く。
    const float aimWeight = (m_weaponRig && m_weaponRig->IsDrawn()) ? 1.0f : 0.0f;
    animator.SetLayerWeight(aimLayerName, aimWeight);
    animator.SetLayerWeight(aimSwayLayerName, aimWeight);

    float yawDegrees   = 0.0f;
    float pitchDegrees = 0.0f;
    const bool hasAim = ResolveAimAngles(yawDegrees, pitchDegrees);

    // 9 セルは -1..1 の格子。実測コーンで割り、外側はコーンの縁のセルへ張り付かせる。
    // ワールド角ではなくモデルのローカル角なので、キャラクターの旋回とカメラの向きが
    // 変わっても 9 セルの意味は変わらない。
    const float desiredYaw = hasAim
        ? std::clamp(yawDegrees / std::max(aimYawRangeDegrees, 1.0f), -1.0f, 1.0f)
        : 0.0f;
    const float pitchRange = std::max(
        pitchDegrees >= 0.0f ? aimPitchUpDegrees : aimPitchDownDegrees, 1.0f);
    const float desiredPitch = hasAim
        ? std::clamp(pitchDegrees / pitchRange, -1.0f, 1.0f)
        : 0.0f;

    // WHY 補間するか: 照準はマウスの生の動きで飛ぶ。9 セルの格子へそのまま入れると、
    //     隣のセルへ跨ぐたびに腕が 1 フレームで切り替わり、ポーズの差し替えが見える。
    const float response = aimResponse > 0.0f
        ? 1.0f - std::exp(-aimResponse * std::max(Time::deltaTime, 0.0f))
        : 1.0f;
    m_aimYaw   += (desiredYaw   - m_aimYaw)   * response;
    m_aimPitch += (desiredPitch - m_aimPitch) * response;

    animator.SetFloat(paramAimYaw, m_aimYaw);
    animator.SetFloat(paramAimPitch, m_aimPitch);
}

inline void PlayerControllerComponent::CacheAimOrigin()
{
    m_aimOrigin = {};
    GameObject* self = scene.Self();
    if (!self || aimOriginBoneName.empty())
        return;

    if (GameObject* bone = FindInSubtree(*self, aimOriginBoneName))
        m_aimOrigin = EntityRef{ bone->GetID() };
    else
        debug.LogWarning("PlayerControllerComponent: aim origin bone '" + aimOriginBoneName
                         + "' not found; aiming from the player origin "
                           "(pitch will read flat for targets at eye level).");
}

inline bool PlayerControllerComponent::AimPointWorld(Vector3& outPoint)
{
    auto* aim = m_aimOverride ? m_aimOverride : scene.GetScript<PlayerAimComponent>();
    if (!aim || !aim->HasAim())
        return false;

    outPoint = aim->AimPoint();
    return true;
}

inline Quaternion PlayerControllerComponent::ModelRotation() const
{
    return (transform.worldRotation *
            Quaternion::FromAxisAngle(Vector3::UP, ToRad(-modelYawOffsetDegrees))).Normalized();
}

inline Vector3 PlayerControllerComponent::AimOriginWorld()
{
    if (GameObject* bone = m_aimOrigin.Resolve(scene))
        return bone->transform.worldPosition;
    return transform.worldPosition;
}

inline bool PlayerControllerComponent::ResolveAimAngles(float& outYawDegrees,
                                                        float& outPitchDegrees)
{
    Vector3 aimPoint;
    if (!AimPointWorld(aimPoint))
        return false;

    const Vector3 local = ModelRotation().Inverse() * (aimPoint - AimOriginWorld());
    const float horizontal = std::sqrtf(local.x * local.x + local.z * local.z);
    if (local.LengthSq() < EPSILON)
        return false;

    outYawDegrees   = ToDeg(std::atan2f(local.x, local.z));
    outPitchDegrees = ToDeg(std::atan2f(local.y, std::max(horizontal, 0.0001f)));
    return true;
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
    // 縦は回避が触らないので、重力だけはこの下で回避中も更新する。
    if (!IsDodging()) {
        // 接地中だけ目標上向き速度を与える。質量差と現在の落下速度は
        // CharacterController が吸収するため、調整値を m/s に統一できる。
        if (jumpRequested && cc->enabled && cc->isGrounded) {
            cc->JumpAtVelocity(&phy, std::max(JumpSpeed(), 0.0f));
            animator.SetBool(paramIsGrounded, false);
            animator.SetTrigger(paramJumpTrigger);
        }

        // WHY 銃を抜いている間は進行方向を向かないか:
        //   進行方向を向くと、横へ走りながら正面を狙ったときに体が 90 度ずれる。
        //   エイムオフセットは ±aimYawRangeDegrees (9 セルの実測 45 度) しか無いので、
        //   腕がそこで頭打ちになり、銃口とレーザーが別々の方向を向く。
        //   横走りのクリップが揃った今は、体を照準へ向けて脚を 2D ブレンドで
        //   合わせるのが正しい。TickAimTurn がコーンを超えた分だけ体を回す。
        const bool faceAim = m_weaponRig && m_weaponRig->IsDrawn();
        if (m_hasMoveInput && rotateToMoveDirection && !faceAim) {
            const Quaternion targetRotation =
                (Quaternion::LookRotation(m_moveDirection) *
                 Quaternion::FromAxisAngle(Vector3::UP, ToRad(modelYawOffsetDegrees))).Normalized();
            const float turnResponse = 1.0f - std::exp(
                -std::max(TurnSpeed(), 0.0f) * dt);
            phy.SetRotation(Quaternion::Slerp(
                phy.GetRotation(), targetRotation, turnResponse).Normalized());
        } else {
            TickAimTurn(phy, dt);
        }

        // 倒し量をそのまま速さへ掛ける。スティックを半分倒せば半分の速さで歩く。
        const Vector3 desiredVelocity = m_hasMoveInput
            ? m_moveDirection * (MoveSpeed() * m_moveMagnitude)
            : Vector3::ZERO;
        const float acceleration = m_hasMoveInput
            ? (cc->isGrounded ? GroundAccel() : AirAccel())
            : -1.0f;
        cc->Move(&phy, desiredVelocity, dt, acceleration, GroundDecel());
    }

    // WHY 最後に決めるか: 上りか下りかは、この step で与えたジャンプ初速まで
    //     含めた速度で判定したい。踏み切りの前に決めると、跳んだ最初の 1 step だけ
    //     下り扱いの重い重力が掛かり、到達点が調整値より低くなる。
    UpdateGravityScale(*cc, phy);
}

inline void PlayerControllerComponent::UpdateGravityScale(
    const CharacterControllerComponent& cc, const RigidBody& phy)
{
    // 基準は 1.0 = ProjectSettings の重力そのまま。ここで持つのは状況ごとの比だけで、
    // 重力の絶対値には触らない。触ると ProjectSettings が調整値でなくなる。
    // 接地中も 1.0 のまま。下りの倍率を残すと、坂や段差でソルバーが毎ステップ
    // 強い押し戻しを解くことになる。
    float multiplier = 1.0f;
    if (!cc.isGrounded) {
        multiplier = phy.GetVelocity().y > 0.0f
            ? (m_jumpHeld ? 1.0f : std::max(tuning->lowJumpGravityMultiplier, 1.0f))
            : std::max(tuning->fallGravityMultiplier, 1.0f);
    }
    physics.SetGravityScale(multiplier);
}

inline void PlayerControllerComponent::TuneGrounding(CharacterControllerComponent& cc) const
{
    if (!autoTuneGrounding)
        return;

    // WHY 重力から決め直すか: CharacterController のしきい値はすべて速度 (m/s) と
    //     秒で書かれている。ProjectSettings で重力を 3 倍にすると、1 物理ステップで
    //     積む速度も滞空時間も 3 倍 / 1/3 になり、同じ数値が別の意味になる。
    //     とくに ledgeFallThreshold は、接地しているのに「崖から落ちた」と誤判定する
    //     側へ倒れるため、重力を上げた瞬間に接地が点滅する。
    const float gravity  = WorldGravity();
    const float stepTime = std::max(Time::fixedDeltaTime, 1.0f / 240.0f);
    // 1 物理ステップで重力が積む速度。速度系のしきい値はすべてこれの倍数で決まる。
    const float step = gravity * stepTime;
    cc.groundedVelSnap    =  std::max(step * 4.0f, 0.2f);
    cc.groundVelThreshold =  std::max(step * 4.0f, 0.2f);
    cc.fallVelThreshold   = -std::max(step * 6.0f, 0.3f);
    cc.ledgeFallThreshold = -std::max(step * 10.0f, 0.5f);

    // 時間系は跳躍の長さから。最短の跳び (すぐキーを離した場合) の滞空より
    // 確実に短くしないと、着地が受け付けられずに空中扱いのまま滑る。
    const float riseSeconds = JumpSpeed() / std::max(gravity, EPSILON);
    cc.jumpMinAirTime         = Clamp(riseSeconds * 0.25f, 0.02f, 0.2f);
    cc.jumpGroundIgnoreTime   = cc.jumpMinAirTime * 0.75f;
    cc.intentionalJumpMaxTime = std::max(riseSeconds * 2.5f, 0.3f);
}

inline void PlayerControllerComponent::TickAimTurn(RigidBody& phy, float dt)
{
    if (!turnToAim)
        return;
    // 銃を収めている間は狙っていない。突っ立ったまま体だけ敵を追うのは不気味に見える。
    if (!m_weaponRig || !m_weaponRig->IsDrawn())
        return;

    Vector3 aimPoint;
    if (!AimPointWorld(aimPoint))
        return;

    Vector3 toTarget = aimPoint - transform.worldPosition;
    toTarget.y = 0.0f;
    if (toTarget.LengthSq() < EPSILON)
        return;

    const Vector3 local = ModelRotation().Inverse() * toTarget.Normalized();
    const float yawDegrees = ToDeg(std::atan2f(local.x, local.z));

    // WHY 対象の方向をそのまま向かせないか: 体が対象へ正対すると AimYaw が常に 0 になり、
    //     8 方向ポーズが中央セルから動かなくなる。コーンの内側は腕に任せ、
    //     はみ出したぶんだけ体を回す (Docs/conventions/minibot-animation-export.md 6 節)。
    const float deadzone = std::min(aimTurnDeadzoneDegrees, aimYawRangeDegrees);
    if (std::abs(yawDegrees) <= deadzone)
        return;

    const float excess = yawDegrees - std::copysign(deadzone, yawDegrees);
    const Quaternion targetRotation =
        (Quaternion::FromAxisAngle(Vector3::UP, ToRad(excess)) * phy.GetRotation()).Normalized();
    const float turnResponse = 1.0f - std::exp(-std::max(TurnSpeed(), 0.0f) * dt);
    phy.SetRotation(Quaternion::Slerp(
        phy.GetRotation(), targetRotation, turnResponse).Normalized());
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
