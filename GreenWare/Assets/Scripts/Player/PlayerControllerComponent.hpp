/// @file    PlayerControllerComponent.hpp
/// @brief   RigidBody ベースのプレイヤー移動・ジャンプ・回避。
/// @author  Hasegawa Jin
/// @date    2026-08-19
///
/// WHY 入力と物理操作を分離するか:
/// 可変フレームで入力を採取し、固定ステップで物理へ反映することで、
/// 短いキー入力の取りこぼしとフレームレート依存を防ぐ。
///
/// WHY 回避に無敵時間を付けないか:
/// ジャスト回避は本バージョンでは実装しない (Docs/open-questions.md)。極性回避と
/// 役割が重なるため。ここでは純粋な移動アクションとして実装し、判定と報酬は
/// 後から足せる形にしておく。
///
/// WHY 水平速度の出どころを移動入力 1 つに閉じるか:
/// 攻撃や纏いから «外からの速度» を受け取れるようにしていたが、押していない
/// フレームに体が動く原因が移動側と攻撃側のどちらにあるのか画面から切り分けられない。
/// 足を動かすのは移動入力と回避だけ、という 1 本にしておく。
#pragma once

#include <Scripts/Data/PlayerTuning.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Components/CharacterControllerComponent.hpp>
#include <Engine/Scene/Components/IKSolverComponent.hpp>
#include <Engine/Scene/Components/RigidBodyComponent.hpp>
#include <Engine/Scene/Script.hpp>
#include <Scripts/Game/CameraFollowManagerComponent.hpp>
#include <Scripts/Game/ImpactFeedbackManagerComponent.hpp>
#include <Scripts/Game/VfxManagerComponent.hpp>
#include <Scripts/Player/PlayerAimComponent.hpp>
#include <Scripts/Player/WeaponRigComponent.hpp>
#include <Scripts/Utils/InputActions.hpp>
#include <Scripts/Utils/SeLibrary.hpp>
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
    FBZZ_TOOLTIP("移動の速さ (m/s)。ロコモーションの Idle → Walk_F → Run_F がこれで決まる")
    // WHY 2 軸 (MoveX / MoveY) を持たないか:
    //   銃の頃は「体は照準を向き、脚だけが進行方向へ合う」ため、向きを 2 軸で
    //   渡して横走り・後ろ走りへブレンドしていた。双剣は体ごと進行方向を向くので、
    //   脚と体の向きが常に一致する。残る自由度は «速さ» の 1 軸だけになった。
    FBZZ_FIELD(std::string, paramVerticalSpeed, "VerticalSpeed","Vertical Speed Param")
    FBZZ_FIELD(std::string, paramIsGrounded,    "IsGrounded",   "IsGrounded Param")
    FBZZ_FIELD(std::string, paramJumpTrigger,   "Jump",         "Jump Trigger Param")
    FBZZ_FIELD(std::string, paramDodgeTrigger,  "Dodge",        "Dodge Trigger Param")

    // WHY 9 セルのエイム (AimYaw / AimPitch / Aim レイヤー) が無いか:
    //   腕で狙う姿勢そのものが双剣で無くなった (Docs/blades.md)。クリップ 10 本ごと
    //   捨てたので、パラメーターとレイヤー名だけ残すと «設定はあるのに何も起きない»
    //   になる。ロックオンした相手を向くのは、下の «立ち止まったときの旋回» だけ。
    FBZZ_GROUP("Lock-On Turn")
    FBZZ_FIELD(bool, turnToAim, true, "Turn To Aim")
    FBZZ_TOOLTIP("立ち止まっているあいだ、ロック対象の方へ体を回す (移動入力が無いときだけ)")
    FBZZ_FIELD_RANGE(float, aimTurnDeadzoneDegrees, 38.0f, "Aim Turn Deadzone", 0.0f, 90.0f)
    FBZZ_TOOLTIP("この角度までは体を回さない。0 に近づけるほど対象へ吸い付く")

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

    // WHY アニメーションイベントではなく距離で鳴らすか:
    //   足接地のタイミングはクリップが持っているので、本来は Event Track が正しい。
    //   ただしインポート後の .anim にイベントは 1 つも入っておらず、
    //   入っていないときの挙動が「無音」なので
    //   壊れていても気付けない。歩いた距離で刻めば、クリップの状態に依存しない。
    //   Event Track を打った時点でこちらを切ればよい (strideRun を 0 にする)。
    FBZZ_GROUP("Footsteps")
    FBZZ_FIELD_RANGE(float, strideWalk, 0.75f, "Walk Stride (m)", 0.0f, 4.0f)
    FBZZ_TOOLTIP("この距離だけ進むごとに歩き足音を 1 回鳴らす。0 で歩きの足音を切る")
    FBZZ_FIELD_RANGE(float, strideRun, 1.35f, "Run Stride (m)", 0.0f, 4.0f)
    FBZZ_TOOLTIP("走りの歩幅。0 で足音そのものを切る")
    FBZZ_FIELD_RANGE(float, runSpeedThreshold, 4.0f, "Run At (m/s)", 0.1f, 20.0f)
    FBZZ_TOOLTIP("この水平速度を超えたら走りの足音・歩幅へ切り替える")

    // WHY その場旋回だけ別に鳴らすか:
    //   歩いている間は接地音が «動いている» を担っているので、旋回音まで重ねると
    //   1 歩に 2 つの音が乗る。逆に立ち止まって向きだけ変えたときは、体が回っているのに
    //   音が何も出ない (足は地面を蹴っていないので足音の条件に入らない)。
    //   «歩いていない» ときだけ鳴らせば、担当が重ならずに空白も埋まる。
    FBZZ_FIELD_RANGE(float, servoTurnRate, 60.0f, "Servo Turn At (deg/s)", 5.0f, 720.0f)
    FBZZ_TOOLTIP("その場でこの角速度を超えて回ったら脚部サーボの音を鳴らす。0 に近づけると常時鳴る")
    FBZZ_FIELD_RANGE(float, servoTurnInterval, 0.35f, "Servo Turn Interval", 0.05f, 3.0f)
    FBZZ_TOOLTIP("サーボ音の最短間隔。回し続けている間はこの間隔で繰り返す")

    // WHY 足音と同じ刻みに乗せるか:
    //   土煙は «足が地面を蹴った» ことの絵で、足音はその音。別のしきい値と別のタイマーで
    //   刻むと、走り出しや坂で片方だけ出る瞬間ができ、音と絵が別々の動作に見える。
    //   歩きで出さないのも同じ理由で «走り» の判定を 1 つに保つため (量が欲しければ
    //   Run At を下げる)。濃さ・大きさは VfxManagerComponent の Run Dust が持つ。
    FBZZ_GROUP("Run Dust")
    FBZZ_FIELD(bool, runDust, true, "Run Dust")
    FBZZ_TOOLTIP("走っているあいだ、足が着くたびに足元へ土煙を出す")
    FBZZ_FIELD(std::string, footBoneLeftName,  "Foot_L", "Left Foot Bone")
    FBZZ_FIELD(std::string, footBoneRightName, "Foot_R", "Right Foot Bone")
    FBZZ_TOOLTIP("土煙を立てる位置。見つからなければプレイヤー原点から出す")

    FBZZ_GROUP("Player Hit Animation")
    FBZZ_FIELD(std::string, hitLayerName, "Add_Hit", "Hit Layer")
    FBZZ_FIELD_FILE(hitFrontClipFile,
        "guid:2c12835bcbbf3de2fd9aae7f9a21f1f0|Library/Baked/7e522cd4a98c0a3c76d1facdf7b0ce59/anims/Katana_Hit_F.anim",
        "Front Hit Clip", ".anim,.fbx")
    FBZZ_FIELD(std::string, hitFrontClipName, "Katana_Hit_F", "Front Hit Clip Name")

    // 回避中か。被弾処理や敵 AI が「今は掴めない」を判断するのに使える。
    [[nodiscard]] bool  IsDodging() const { return m_dodgeRemaining > 0.0f; }
    // UI 用。1 = 回避可能 / 0 = 使った直後。
    [[nodiscard]] float DodgeCharge() const;

    void OnStart() override;
    void OnUpdate() override;
    void OnFixedUpdate() override;
    // 被弾通知から呼ばれる標準の正面リアクション。
    void PlayHitAnimation();
    // PlayerComponent が内部モジュールとして保持するときのエイム結果の注入先。
    void SetAimComponent(PlayerAimComponent* aim) { m_aimOverride = aim; }
    // 武器の挙動そのものは WeaponRigComponent が持つ。ここは要求を渡すだけ。
    void SetWeaponRig(WeaponRigComponent* rig) { m_weaponRig = rig; }

    /// 外から掛ける移動速度の倍率。溜めている間に足を鈍らせる用途 (PolarityBladeComponent)。
    ///
    /// WHY 掛ける側が毎フレーム設定するか: «掴んでいる» を知っているのは手の側だけで、
    ///     ここが相手を名指しで問い合わせると、手を 1 つ足すたびに移動側を触ることになる。
    ///     設定しなくなれば次のフレームに 1.0 へ戻るので、解除の呼び忘れで足が遅いまま
    ///     張り付く事故も起きない。
    void RequestMoveSpeedScale(float scale)
    {
        m_requestedMoveScale = Clamp(scale, 0.05f, 1.0f);
    }

    /// この向きへ体を向けるよう 1 フレームぶん要求する (斬撃が振る向きを渡す)。
    ///
    /// WHY 攻撃側が向きまで決めるか: 斬る向きを決めているのは剣
    ///     (PolarityBladeComponent::SwingDirection) で、吸い付き補正を 0 にすると
    ///     それはカメラの正面になる。こちらが独自にロック対象へ向き続けると、
    ///     «体は敵を向いているのに刃は画面の奥へ抜ける» という、当たらない理由が
    ///     画面から読めない食い違いが生まれる。振る向きは 1 つでなければならない。
    ///
    /// WHY 移動入力より優先するか: 走りながら斬ったとき、進行方向を向いたまま
    ///     横へ斬ると «誰に向かって振ったのか» が消える。振っている間だけは
    ///     刃の向きが体の向きを持つ。
    void RequestFacing(const Vector3& direction)
    {
        Vector3 flat = direction;
        flat.y = 0.0f;
        if (flat.LengthSq() > EPSILON) m_requestedFacing = flat.Normalized();
    }

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
    // 上り / 下り / 早離しで重力を切り替える。倍率は World の重力に対する比。
    void UpdateGravityScale(const CharacterControllerComponent& cc, const RigidBody& phy);
    // 接地判定のしきい値を、今の重力と跳躍の長さから決め直す。
    void TuneGrounding(CharacterControllerComponent& cc) const;
    // 空中・回避・走行のあいだ、カメラの追従を緩めるよう要求する。
    void RequestCameraSlack(const CharacterControllerComponent& cc, float planarSpeed);
    // 空中から接地へ変わった瞬間を掴み、落下速度の分だけ手触りを鳴らす。
    void TickLanding(const CharacterControllerComponent& cc, const RigidBody& phy);
    // 接地して進んだ距離を積み、歩幅ぶん進むごとに足音と土煙を 1 回出す。
    /// その場旋回の脚部サーボ。歩かずに向きだけ変えたときの «動いている» を返す。
    void TickServoTurn(float planarSpeed, float dt);
    void TickFootsteps(const CharacterControllerComponent& cc, const Vector3& velocity,
                       float planarSpeed);
    void CacheFootBones();
    // 今まさに着いている足のワールド位置。土煙はここから立てる。
    [[nodiscard]] Vector3 PlantedFootWorld();
    // 立ち止まっているあいだ、ロック対象からずれたヨーを体の旋回で詰める。
    void TickAimTurn(RigidBody& phy, float dt);
    void UpdateSlopeLean(IKSolverComponent& ik, CharacterControllerComponent* cc,
                         RigidBodyComponent* rb);

    // 今ロックしている相手の位置。PlayerAimComponent が選んだ 1 体。
    [[nodiscard]] bool AimPointWorld(Vector3& outPoint);
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

    // 左右の足首のボーン。土煙の位置を決めるためだけに持つ。
    EntityRef m_footLeft;
    EntityRef m_footRight;

    bool m_hasSpineTargetBase = false;
    Vector3 m_spineTargetBase = Vector3::ZERO;
    Vector3 m_smoothedSpineOffset = Vector3::ZERO;
    // 回避の残り時間と、次に回避できるまでの残り時間。
    float   m_dodgeRemaining = 0.0f;
    float   m_dodgeCooldown  = 0.0f;
    Vector3 m_dodgeDirection = Vector3::ZERO;
    // 足音を鳴らしてから接地して進んだ距離 [m]。
    float   m_stepDistance   = 0.0f;
    // 前フレームの平面上の前方向と、サーボ音を鳴らしてからの間隔。
    Vector3 m_lastFacing       = Vector3::FORWARD;
    bool    m_hasLastFacing    = false;
    float   m_servoTurnCooldown = 0.0f;
    // 可変フレームで採取した入力を、次の固定ステップで一度だけ物理へ適用する。
    Vector3 m_moveDirection = Vector3::ZERO;
    // スティックの倒し量 0..1。キーボードは押していれば常に 1。
    float   m_moveMagnitude = 0.0f;
    bool    m_hasMoveInput = false;
    // 外から掛かる移動速度の倍率。要求は 1 フレーム有効で、OnUpdate が取り込んでから
    // 中立へ戻す。物理ステップが 1 フレームに複数回走っても、その全部で同じ値になる。
    float   m_moveSpeedScale     = 1.0f;
    float   m_requestedMoveScale = 1.0f;
    // 振っている間だけ «刃の向き» が体の向きになる。長さ 0 は «要求なし»。
    Vector3 m_facing          = Vector3::ZERO;
    Vector3 m_requestedFacing = Vector3::ZERO;
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
    m_jumpHeld       = false;
    m_wasGrounded    = true;
    m_peakFallSpeed  = 0.0f;
    m_stepDistance   = 0.0f;
    // 前フレームの向きは Play をまたいで持ち越さない。持ち越すと開始の 1 フレームで
    // «一気に回った» ことになり、立っているだけでサーボ音が鳴る。
    m_hasLastFacing     = false;
    m_servoTurnCooldown = 0.0f;
    m_facing            = Vector3::ZERO;
    m_requestedFacing   = Vector3::ZERO;

    // 足音・回避音の出どころ。プレイヤー本人なので減衰を掛けずに 2D で鳴らす。
    se::EnsureSource(scene);

    if (auto* cc = scene.GetComponent<CharacterControllerComponent>())
        TuneGrounding(*cc);

    CacheFootBones();
}

inline void PlayerControllerComponent::OnUpdate()
{
    if (!enabled || !transform || !tuning) return;

    // 前フレームに来ていた要求を取り込み、要求側を中立へ戻す。掛ける側が押し続けて
    // いる間だけ鈍り、呼ばれなくなれば次のフレームで自然に元へ戻る。
    m_moveSpeedScale     = m_requestedMoveScale;
    m_requestedMoveScale = 1.0f;
    m_facing          = m_requestedFacing;
    m_requestedFacing = Vector3::ZERO;

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
    // 入力はここまで。抜く / 納める の中身は WeaponRigComponent の担当で、
    // このスクリプトは刀の存在もソケットも知らない。
    if (m_weaponRig) {
        if (input.GetActionDown(actions::kDrawWeapons))    m_weaponRig->RequestDraw();
        if (input.GetActionDown(actions::kHolsterWeapons)) m_weaponRig->RequestSheathe();
        // パッドはボタンが足りないので 1 つで往復させる。どちらへ動かすかは
        // 押した側ではなく今の状態が決める。
        if (input.GetActionDown(actions::kToggleWeapons))
            m_weaponRig->RequestToggle();
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
    UpdateIK();

    // 手触りの出力は可変フレーム側で回す。固定ステップは 1 フレームに 0 回にも
    // 複数回にもなるため、揺れや緩みの要求をそこへ置くと回数が絵に依存する。
    if (cc && phy) {
        RequestCameraSlack(*cc, planarSpeed);
        TickLanding(*cc, *phy);
        TickFootsteps(*cc, velocity, planarSpeed);
        TickServoTurn(planarSpeed, Time::deltaTime);
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

inline void PlayerControllerComponent::TickServoTurn(float planarSpeed, float dt)
{
    // 平面上の前方向。真上を向いた姿勢では成分が消えるので、前フレームへ落とす
    // (Normalized() は長さ 0 で assert する)。
    const Vector3 facing  = transform.worldRotation * Vector3::FORWARD;
    const Vector3 forward = Vector3{ facing.x, 0.0f, facing.z }.NormalizedOr(m_lastFacing);

    if (!m_hasLastFacing || dt <= 0.0f) {
        m_lastFacing    = forward;
        m_hasLastFacing = true;
        return;
    }

    // 符号付きの回転角。Cross の y 成分が回転の向きをそのまま持っている。
    const float cross   = Vector3::Cross(m_lastFacing, forward).y;
    const float dot     = std::clamp(Vector3::Dot(m_lastFacing, forward), -1.0f, 1.0f);
    const float yawRate = std::fabs(ToDeg(std::atan2f(cross, dot))) / dt;
    m_lastFacing = forward;

    m_servoTurnCooldown = Max(0.0f, m_servoTurnCooldown - dt);

    // 歩いている間は足音が «動いている» を担っている。回避中も専用の音がある。
    if (planarSpeed > EPSILON || IsDodging()) return;
    if (yawRate < servoTurnRate) return;
    if (m_servoTurnCooldown > 0.0f) return;

    se::Play(audio, se::kPlayerServoTurn);
    m_servoTurnCooldown = servoTurnInterval;
}

inline void PlayerControllerComponent::TickFootsteps(const CharacterControllerComponent& cc,
                                                     const Vector3& velocity,
                                                     float planarSpeed)
{
    // 空中と回避中は足が地面を蹴っていない。回避には専用の音があるので、
    // ここで足音まで鳴らすと 1 回の回避で 2 種類の音が重なる。
    if (!cc.isGrounded || IsDodging() || planarSpeed <= EPSILON) {
        // 止まっている間に貯めた距離は捨てる。残すと、止まって歩き出した 1 歩目が
        // 歩幅を待たずに鳴り、歩き始めだけリズムが崩れる。
        m_stepDistance = 0.0f;
        return;
    }

    const bool  running = planarSpeed >= runSpeedThreshold;
    const float stride  = running ? strideRun : strideWalk;
    if (stride <= 0.0f) {
        m_stepDistance = 0.0f;
        return;
    }

    // WHY 歩幅そのものを毎歩振るか:
    //   素材の README は «走り 0.355 秒 / 歩き 0.58 秒間隔を ±3% ほど揺らす» と
    //   書いている (人の歩容は等間隔にならない)。歩幅で刻んでいるこちらでは、
    //   一定の歩幅を一定の速さで踏むと間隔もぴたりと等間隔になるので、同じ揺れは
    //   歩幅の側へ入れる。歩幅 1.35m / 走り 4.0m/s は 0.34 秒で、素材が想定している
    //   歩調とほぼ重なっている。
    //
    // WHY 音量を振らないか:
    //   1 歩ごとの音量差 (−2.4〜+1.8dB) は既にファイルへ焼き込んである。ここで
    //   さらに掛けると二重になり、逆に揃えると «機械の足音» に戻る。触らないのが正解。
    constexpr float kStrideJitter = 0.03f;
    const float threshold = stride * random.Range(1.0f - kStrideJitter, 1.0f + kStrideJitter);

    m_stepDistance += planarSpeed * Time::deltaTime;
    if (m_stepDistance < threshold) return;

    // 剰余で戻す。引き切ると、1 フレームで歩幅を大きく超えたとき (低フレームレートや
    // 回避の直後) に余りが積み残り、次の 1 歩が早まる。
    m_stepDistance = std::fmod(m_stepDistance, threshold);
    se::Play(audio, running ? se::kPlayerFootstepRun : se::kPlayerFootstepWalk);

    if (!running || !runDust) return;
    if (auto* vfx = VfxManagerComponent::Instance()) {
        // 蹴り出しの強さは «最高速に対する今の速さ»。しきい値からの比で取ると、
        // Run At を下げただけで走り出しの 1 歩目が最大の土煙になる。
        vfx->PlayRunDust(PlantedFootWorld(), velocity,
                         Clamp01(planarSpeed / Max(MoveSpeed(), EPSILON)));
    }
}

inline void PlayerControllerComponent::CacheFootBones()
{
    m_footLeft  = {};
    m_footRight = {};
    GameObject* self = scene.Self();
    if (!self) return;

    if (GameObject* bone = FindInSubtree(*self, footBoneLeftName))
        m_footLeft = EntityRef{ bone->GetID() };
    if (GameObject* bone = FindInSubtree(*self, footBoneRightName))
        m_footRight = EntityRef{ bone->GetID() };
}

inline Vector3 PlayerControllerComponent::PlantedFootWorld()
{
    GameObject* left  = m_footLeft.Resolve(scene);
    GameObject* right = m_footRight.Resolve(scene);

    // WHY 左右を交互に数えないか: 歩幅は距離で刻んでいるので、クリップの再生位相とは
    //     独立に進む。交互に配ると走行速度が変わった瞬間から左右が入れ替わり、
    //     宙に浮いている方の足から土煙が立つ。着いている足は «低い方» でしか分からない。
    GameObject* planted = left;
    if (!planted || (right && right->transform.worldPosition.y < left->transform.worldPosition.y))
        planted = right;
    if (!planted) return transform.worldPosition;

    // 足首のボーンは床から浮いている。左右と前後の位置だけ足から借り、
    // 高さは接地しているプレイヤー原点に合わせる。
    Vector3 point = planted->transform.worldPosition;
    point.y = transform.worldPosition.y;
    return point;
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

        // WHY 抜刀していても進行方向を向くか:
        //   銃の頃は «体を照準へ向けて、脚を 2D ブレンドで進行方向へ合わせる» 形だった。
        //   これは横走り・後ろ走りのクリップが揃っていて初めて成立する。双剣では
        //   ロコモーションを Katana_Idle / Walk_F / Run_F の前進 3 本だけにしたので、
        //   体を照準へ固定すると «正面を向いたまま横へ滑る» になる。
        //   斬る向きは体の向きではなく PolarityBladeComponent::SwingDirection() が
        //   ロックオン対象から決めるので、体まで対象へ縛る理由はもう無い。
        //   立ち止まっているときだけ TickAimTurn が体を対象へ向ける。
        // 振っている間の «刃の向き» が最優先。走りの向きにもロック対象にも譲らない
        // (RequestFacing の WHY を参照)。
        //
        // WHY 通常の旋回より速く回すか: 斬るのは 0.2 秒ほどの出来事で、いつもの
        //     追従で回すと «振り終わってから向き終わる» ことになる。振り出しの
        //     1 フレーム目で概ね向いていないと、刃と体が別々に動いて見える。
        if (m_facing.LengthSq() > EPSILON) {
            const Quaternion targetRotation =
                (Quaternion::LookRotation(m_facing) *
                 Quaternion::FromAxisAngle(Vector3::UP, ToRad(modelYawOffsetDegrees))).Normalized();
            const float turnResponse = 1.0f - std::exp(
                -std::max(TurnSpeed(), 0.0f) * 3.0f * dt);
            phy.SetRotation(Quaternion::Slerp(
                phy.GetRotation(), targetRotation, turnResponse).Normalized());
        } else if (m_hasMoveInput && rotateToMoveDirection) {
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
        //
        // WHY MoveSpeed() 自体には掛けないか: あちらは走りのブレンドとカメラの
        //     追従遅れを正規化する基準にも使われている。基準ごと縮めると、
        //     鈍らせている間だけ «歩いているのに走りモーション» になる。
        const Vector3 inputVelocity = m_hasMoveInput
            ? m_moveDirection * (MoveSpeed() * m_moveMagnitude * m_moveSpeedScale)
            : Vector3::ZERO;
        // 入力が無いときの -1 は減速の合図。水平速度の出どころは移動入力だけなので、
        // 押していないフレームは必ずブレーキが掛かる。
        const float acceleration = m_hasMoveInput
            ? (cc->isGrounded ? GroundAccel() : AirAccel())
            : -1.0f;
        cc->Move(&phy, inputVelocity, dt, acceleration, GroundDecel());
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

    // WHY 対象の方向へ «ぴったり» 向かせないか: 立ち止まっているだけで体がぬるぬる
    //     追尾すると、狙っているというより «吸い付いている» 絵になる。少し外れている
    //     ぶんは残し、はみ出した角度だけ回す。
    const float deadzone = std::max(aimTurnDeadzoneDegrees, 0.0f);
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
        se::Play(audio, se::kPlayerDodge);
        // 開始フレームからそのまま下の速度上書きへ進む。
        // ここで return すると 1 フレームだけ慣性で滑り、出だしが鈍る。
    } else {
        m_dodgeRemaining = std::max(0.0f, m_dodgeRemaining - dt);
        if (m_dodgeRemaining <= 0.0f) {
            // 終わり際の踏ん張り。開始音だけだと、回避がいつ終わって
            // 次の入力を受け付けるのかが耳から分からない。
            se::Play(audio, se::kPlayerDodgeEnd);
            return;  // 今フレームで終了
        }
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
