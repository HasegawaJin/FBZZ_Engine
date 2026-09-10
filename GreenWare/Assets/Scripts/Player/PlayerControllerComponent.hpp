/// @file    PlayerControllerComponent.hpp
/// @brief   RigidBody ベースのプレイヤー移動・ジャンプ・回避。
/// @author  Hasegawa Jin
/// @date    2026-08-19
///
/// WHY 入力と物理操作を分離するか:
/// 可変フレームで入力を採取し、固定ステップで物理へ反映することで、
/// 短いキー入力の取りこぼしとフレームレート依存を防ぐ。
///
/// WHY 回避の無敵をここで判定しないか:
/// ここは «今回避中か» と «何回目の回避か» を答えるだけ。攻撃を弾いてジャスト回避の
/// 報酬を配るのは PlayerComponent (ダメージの入口) で、移動側は殴られた事実を知らない。
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
#include <Scripts/Utils/PlayerActionState.hpp>
#include <Scripts/Game/CameraShakeManagerComponent.hpp>
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

    FBZZ_GROUP("移動")
    FBZZ_FIELD_RANGE(float, modelYawOffsetDegrees, 180.0f, "モデルのヨー補正",  0.0f, 360.0f)
    FBZZ_FIELD(bool, useCameraForward,      true, "カメラの正面を使う")
    FBZZ_FIELD(bool, rotateToMoveDirection, true, "進行方向へ向く")
    FBZZ_FIELD(bool, useFootIK,             true, "足 IK を使う")

    FBZZ_GROUP("Dodge")
    FBZZ_TOOLTIP("速さと距離。無敵とジャスト回避は PlayerTuning の Dodge (Docs/camera-controls.md)")

    // WHY キーバインドの項目を持たないか:
    //   物理入力を Inspector に持つと、その 1 行のためにゲームパッド対応も
    //   キーコンフィグも原理的に不可能になる (InputBinding.hpp の設計意図)。
    //   このスクリプトは論理名だけを知り、実際の割り当ては
    //   ProjectSettings/Input.inputactions が持つ。名前は InputActions.hpp を参照。

    FBZZ_GROUP("アニメーターのパラメーター")
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

    // WHY 転がりの «尺» をここに持つか (2026-09-05 のテレポート修正):
    //   回避が瞬間移動に見えていた原因は速さではなく、進む時間 (Dodge Duration) と
    //   転がるクリップの長さが違っていたこと。0.24 秒で 4.5m 運びきる一方、
    //   Katana_Dodge_Roll は 0.567 秒あるので、移動が終わった時点で転がりはまだ
    //   4 割しか進んでいない ─ 踏み切りの姿勢のまま体だけが 4.5m 先に居る絵になる。
    //
    //   直し方は «クリップを再生する速さ» で長さを合わせること。ところが 1 ステートの
    //   速度は Script から書けない (animator.SetSpeed は Animator 全体に掛かる) ので、
    //   実際に速度を持つのは Player.animcontroller の Dodge_Roll ステート。
    //   ここに置いたクリップ長から «その speed に入れるべき値» を導出して Debug へ出す。
    //   Dodge Duration を触った人が «次に何を直すか» を Inspector 上で見つけられれば、
    //   離れた 2 つのファイルの数字が黙ってずれることは無い。
    FBZZ_FIELD_RANGE(float, dodgeClipSeconds, 17.0f / 30.0f, "回避クリップの尺 [秒]", 0.05f, 3.0f)
    FBZZ_TOOLTIP("Katana_Dodge_Roll の尺 [秒] (17F / 30fps)。クリップを差し替えたら直すこと")
    FBZZ_FIELD_READ_ONLY(float, debugDodgeClipSpeed, 0.0f, "回避クリップの再生速度")
    FBZZ_TOOLTIP("Player.animcontroller の Dodge_Roll ステートの speed に入れる値。"
                 "Dodge Duration を変えたらこの数字を写す")

    // WHY 9 セルのエイム (AimYaw / AimPitch / Aim レイヤー) が無いか:
    //   腕で狙う姿勢そのものが双剣で無くなった (Docs/blades.md)。クリップ 10 本ごと
    //   捨てたので、パラメーターとレイヤー名だけ残すと «設定はあるのに何も起きない»
    //   になる。ロックオンした相手を向くのは、下の «立ち止まったときの旋回» だけ。
    FBZZ_GROUP("ロックオン旋回")
    FBZZ_FIELD(bool, turnToAim, true, "狙いの方へ向く")
    FBZZ_TOOLTIP("立ち止まっているあいだ、ロック対象の方へ体を回す (移動入力が無いときだけ)")
    FBZZ_FIELD_RANGE(float, aimTurnDeadzoneDegrees, 38.0f, "旋回の不感帯", 0.0f, 90.0f)
    FBZZ_TOOLTIP("この角度までは体を回さない。0 に近づけるほど対象へ吸い付く")

    FBZZ_GROUP("跳躍 (導出値)")
    // 重力は ProjectSettings の [physics] gravity、到達点は PlayerTuning。
    // 導出結果をここに出さないと、重力を触った影響が Play するまで分からない。
    FBZZ_FIELD_READ_ONLY(float, debugWorldGravity, 0.0f, "ワールドの重力")
    FBZZ_FIELD_READ_ONLY(float, debugJumpSpeed, 0.0f, "跳躍の初速")
    FBZZ_FIELD_READ_ONLY(float, debugTimeToApex, 0.0f, "頂点までの時間")
    FBZZ_FIELD(bool, autoTuneGrounding, true, "接地判定を自動調整")
    FBZZ_TOOLTIP("接地判定のしきい値を重力から決め直す。"
                 "切ると CharacterController に入っている値をそのまま使う")

    FBZZ_GROUP("カメラの手応え")
    // WHY カメラ側ではなくここに置くか: 「跳ぶとどれくらい緩めたいか」は動作を持つ
    //     こちらの意図で、「緩んだときカメラがどう動くか」はカメラの作り。
    //     混ぜると、カメラの追従方式を変えるたびに動作側の数値を入れ直すことになる。
    FBZZ_FIELD_RANGE(float, airCameraSlack, 0.85f, "Air Slack (Vertical)", 0.0f, 1.0f)
    FBZZ_TOOLTIP("空中にいる間、カメラの縦追従を緩める量。0 で従来どおり密着する")
    // WHY 押した «事実» を預かるか: 回避はクールダウン中でも回避中でも押される。
    //     その瞬間に捨てると «連打しているのに出ない» になり、しかも出なかった理由が
    //     画面に出ない。押し忘れではなく早すぎただけの入力を、明けた 1 フレーム目へ運ぶ。
    FBZZ_FIELD_RANGE(float, dodgeBufferSeconds, 0.15f, "Dodge Buffer", 0.0f, 0.5f)
    FBZZ_TOOLTIP("回避入力を預かる秒数。クールダウンや回避中に押しても、明けた瞬間に出る。"
                 "0 で先行入力なし (押した瞬間しか受け付けない)")
    FBZZ_FIELD_RANGE(float, dodgeCameraKick, 0.4f, "Dodge Camera Kick (m)", 0.0f, 2.0f)
    FBZZ_TOOLTIP("回避の出だしにカメラを後ろへ引く距離。揺れではなく 1 往復の押し込み。"
                 "画角が開くぶん、同じ速度でも速く見える")
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
    // WHY 揺れとは別に «沈み» を持つか: 揺れは向きを持たないので、着地の «重さが
    //     床へ落ちた» 向きが出ない。カメラが一瞬下がって戻ると、膝が沈んだのと同じ
    //     方向に画面が動いて、体重が伝わる。
    FBZZ_FIELD_RANGE(float, landCameraDip, 0.14f, "Land Camera Dip (m)", 0.0f, 1.0f)
    FBZZ_TOOLTIP("最大の落下で着地した瞬間にカメラが下へ沈む距離。1 往復で戻る")

    // WHY 旋回で上体を傾けるか: 走りながら向きを変えると、脚は進行方向へ回るが
    //     上体は真っ直ぐ立ったままで、氷の上を滑る台車に見える。曲がる側へ
    //     上体を少し倒すと、遠心力に逆らっている «体重» が絵に出る。
    //     坂の前傾 (UpdateSlopeLean) と同じ Spine IK の目標を借りるので、
    //     新しいボーン操作は増えていない。
    FBZZ_GROUP("Turn Lean")
    FBZZ_FIELD_RANGE(float, turnLeanPerTurn, 0.30f, "Lean Per Turn (m)", 0.0f, 1.0f)
    FBZZ_TOOLTIP("毎秒 1 回転 (360 deg/s) で回ったときに上体を曲がる側へ倒す量。"
                 "0 で切る。速さに比例して効くので、歩きではほとんど倒れない")
    FBZZ_FIELD_RANGE(float, turnLeanMax, 0.16f, "Lean Max (m)", 0.0f, 0.5f)
    FBZZ_TOOLTIP("倒す量の上限。急旋回で上体が横へ飛ぶのを止める")

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
    FBZZ_GROUP("走りの砂埃")
    FBZZ_FIELD(bool, runDust, true, "走りの砂埃")
    FBZZ_TOOLTIP("走っているあいだ、足が着くたびに足元へ土煙を出す")
    // WHY 回避の途中にもう 1 発足すか:
    //   回避で床に残るのは «踏み切り» と «踏ん張り» の 2 発だけで、そのあいだ
    //   いちばん速く動いている 0.2 秒ほどは床に何も起きない。転がっているのに
    //   床と関わっていないので、跳んでいるように見える。体がいちばん低くなる
    //   あたりで擦れの煙を 1 発置くと、転がりが «床の上の動き» に戻る。
    //   出す / 出さないは走りの砂埃と同じ 1 つのスイッチ (上の runDust) に従う。
    FBZZ_FIELD_RANGE(float, dodgeScuffAt, 0.42f, "回避の擦れ", 0.0f, 1.0f)
    FBZZ_TOOLTIP("回避のどこで床を擦る煙を出すか (0 = 踏み切り / 1 = 抜け際)。"
                 "1.0 で踏ん張りの煙と重なるので実質オフ")
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
    /// 回避を始めた回数。ジャスト回避の報酬を «同じ 1 回の回避» で二重に配らないための札。
    [[nodiscard]] int   DodgeSerial() const { return m_dodgeSerial; }
    /// 今の回避が運んでいる向き (水平・正規化済み)。
    ///
    /// WHY 体の正面 (transform.forward) で代用できないか: モデルのヨー補正が
    ///     180 度入っているので、根の «正面» は進んでいる向きの逆を指す。
    [[nodiscard]] const Vector3& DodgeDirection() const { return m_dodgeDirection; }
    // UI 用。1 = 回避可能 / 0 = 使った直後。
    [[nodiscard]] float DodgeCharge() const;
    /// 今の回避がどこまで進んだか。0 = 踏み切った瞬間 / 1 = 抜け切る直前。
    ///
    /// WHY 外へ出すか: 回避の速さは出だしが最大で終わりに向けて落ちる
    ///     (Dodge End Speed x)。見せる側 (残像) がこれを知らないと、減速し切った
    ///     終わり際まで出だしと同じ濃さで残り、«最後まで同じ速さで滑った» に見える。
    [[nodiscard]] float DodgeProgress01() const;

    void OnStart() override;
    void OnUpdate() override;
    void OnFixedUpdate() override;
    // 被弾通知から呼ばれる標準の正面リアクション。
    void PlayHitAnimation();

    /// 被弾で押される。direction は水平の向き (正規化は中で行う)。
    ///
    /// WHY 物理の力積ではなく «移動の速度» として持つか: 水平速度の出どころは
    ///     `cc->Move` に渡す入力速度 1 つだけで、そこへ辿り着かない AddImpulse は
    ///     次の FixedUpdate で必ず打ち消される (DemoGame で踏んだ)。押しは
    ///     «一定時間、入力の代わりに置かれる速度» として扱う。
    ///
    /// WHY 押しが要るか: 押されないと被弾のコストが HP だけになり、間合いが変わらない。
    ///     斬りに戻る距離が増えて初めて «食らった» が手数の損になる。
    void Knockback(const Vector3& direction, float speed, float seconds);
    // PlayerComponent が内部モジュールとして保持するときのエイム結果の注入先。
    void SetAimComponent(PlayerAimComponent* aim) { m_aimOverride = aim; }
    // 武器の挙動そのものは WeaponRigComponent が持つ。ここは要求を渡すだけ。
    void SetWeaponRig(WeaponRigComponent* rig) { m_weaponRig = rig; }

    /// 外から掛ける移動速度の倍率。溜めている間に足を鈍らせる用途 (BladeComponent)。
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
    ///     (BladeComponent::SwingDirection) で、吸い付き補正を 0 にすると
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

    /// 座標を外から置く演出のあいだ、移動・跳躍・回避・重力の適用を止める。
    /// RequestMoveSpeedScale と同じく、押し続けている間だけ効く。
    ///
    /// WHY cutscene::HoldsPlayer では足りないか: あちらが止めるのは «入力» だけで、
    ///     重力と衝突解決はそのまま進む (「止めると演出の最中に空中で固まる」)。
    ///     登攀は 3 秒以上かけて座標を毎フレーム置き換えるので、その間ずっと
    ///     落下速度が積み上がり、甲板に着いた瞬間にその速度で落ちる。
    ///     向きだけは残す ── 登っている向きを掛ける側が決められなくなる。
    ///
    /// `lockInput` を立てると刀の入力 (斬撃・弾き・とどめ) まで止まる。
    /// cutscene の申告は «最後に書いた人が勝つ» 一枚で、ボスのカメラ演出が毎フレーム
    /// holdPlayer=false を置き直している。数秒またぐ拘束をそこへ相乗りさせると
    /// 演出の順番次第で途中から外れるので、押し続ける形の錠をこちらに持つ。
    void RequestSuspend(bool lockInput = false)
    {
        m_requestedSuspend = true;
        m_requestedInputLock = m_requestedInputLock || lockInput;
    }

    /// 拘束中の体をこの角度だけ倒す。pitch は前へ、roll は右へ (どちらも度)。
    /// RequestFacing と同じく押し続けている間だけ効く。
    ///
    /// WHY 向き (RequestFacing) と分けるか: 向きは «どこを向くか» で、これは
    ///     «どう構えるか»。壁を登る体は、脚へ正対したまま斜面へ倒れ込んでいる ──
    ///     1 つの要求にまとめると、倒す角度を変えるたびに向きが回ってしまう。
    ///
    /// WHY 拘束中だけ効かせるか: 普段の姿勢は接地・移動・回避が奪い合っていて、
    ///     そこへ外からの傾けを足すと «誰が体を倒したのか» が追えなくなる。
    ///     座標ごと外から置かれている間 (登攀) は、体の姿勢も置く側の持ち物。
    void RequestLean(float pitchDegrees, float rollDegrees)
    {
        m_requestedLeanPitch = pitchDegrees;
        m_requestedLeanRoll  = rollDegrees;
    }
    [[nodiscard]] bool IsSuspended()   const { return m_suspended; }
    [[nodiscard]] bool IsInputLocked() const { return m_inputLocked; }

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
    [[nodiscard]] float DodgeEndSpeedRatio() const
    { return Clamp(tuning->dodgeEndSpeedRatio, 0.05f, 1.0f); }

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
    int     m_dodgeSerial    = 0;
    /// この回避で擦れの煙をもう出したか。1 回の回避につき 1 発。
    bool    m_dodgeScuffed   = false;

    // 被弾の押し。残り時間で細らせながら入力へ混ぜる。
    Vector3 m_knockDir     = Vector3::ZERO;
    float   m_knockSpeed   = 0.0f;
    float   m_knockSeconds = 0.0f;
    float   m_knockLeft    = 0.0f;
    // 足音を鳴らしてから接地して進んだ距離 [m]。
    float   m_stepDistance   = 0.0f;
    // 前フレームの平面上の前方向と、サーボ音を鳴らしてからの間隔。
    Vector3 m_lastFacing       = Vector3::FORWARD;
    bool    m_hasLastFacing    = false;
    float   m_servoTurnCooldown = 0.0f;
    /// 符号付きの旋回速度 [deg/s]。正が右回り。上体の傾きが読む。
    float   m_yawRateDegrees   = 0.0f;
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
    // 拘束中の «構え»。前傾と横倒しを度で持つ。要求が来なくなれば 0 へ戻る。
    float   m_leanPitch          = 0.0f;
    float   m_leanRoll           = 0.0f;
    float   m_requestedLeanPitch = 0.0f;
    float   m_requestedLeanRoll  = 0.0f;
    // 座標を外から置かれている間。移動速度の倍率と同じ «押し続けている間だけ» の要求。
    bool    m_suspended          = false;
    bool    m_requestedSuspend   = false;
    // 刀の入力まで止めているか。剣と弾きがここを読む。
    bool    m_inputLocked        = false;
    bool    m_requestedInputLock = false;
    /// 剛体を静的にしたか。掛けた側と戻す側を 1 つの旗で対にする。
    bool    m_suspendApplied     = false;
    bool    m_jumpRequested = false;
    bool    m_dodgeRequested = false;
    /// 預かっている回避入力の残り [秒]。0 より大きい間は «押されている» として扱う。
    float   m_dodgeBuffer    = 0.0f;

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
    m_dodgeBuffer    = 0.0f;
    m_dodgeSerial    = 0;
    m_dodgeScuffed   = false;
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
    m_yawRateDegrees    = 0.0f;
    m_facing            = Vector3::ZERO;
    m_requestedFacing   = Vector3::ZERO;
    m_suspended          = false;
    m_requestedSuspend   = false;
    m_inputLocked        = false;
    m_requestedInputLock = false;
    // Play をまたぐと剛体は作り直されるので、静的の «掛けた» 記録も捨てる。
    m_suspendApplied     = false;

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
    m_leanPitch          = m_requestedLeanPitch;
    m_leanRoll           = m_requestedLeanRoll;
    m_requestedLeanPitch = 0.0f;
    m_requestedLeanRoll  = 0.0f;
    m_suspended          = m_requestedSuspend;
    m_requestedSuspend   = false;
    m_inputLocked        = m_requestedInputLock;
    m_requestedInputLock = false;

    // 転がりの尺を回避の尺へ合わせるのに要る値。Inspector で回避を詰めている最中に
    // 見えていないと «直す先がある» ことに気付けない (フィールドの WHY を参照)。
    debugDodgeClipSpeed = dodgeClipSeconds / std::max(DodgeDuration(), 0.0001f);

    const Vector3 forward = GetMoveForward();
    const Vector3 right   = GetMoveRight(forward);

    // WASD と左スティックの両方がここへ合流する。半径方向のデッドゾーンも
    // アクション層が済ませているので、斜めに倒したときの実効感度が方向で変わらない。
    // カメラ演出 (登場・撃破・とどめ) のあいだは入力を受けない。物理と重力は
    // そのまま進める ─ 止めると演出の最中に空中で固まる。
    const bool held = m_suspended || cutscene::HoldsPlayer(Time::unscaledTime);
    const Vector2 axis = held ? Vector2{ 0.0f, 0.0f } : input.GetMoveAxis();
    // WHY 倒し量を残すか: 正規化だけしてしまうと、スティックを半分倒しても
    //     全力疾走になり、パッドでの歩き / 走りの作り分けが消える。
    //     キーボードは常に 1.0 になるので、従来の挙動は変わらない。
    const float amount = Min(axis.Length(), 1.0f);

    m_hasMoveInput  = amount > EPSILON;
    m_moveMagnitude = amount;
    m_moveDirection = m_hasMoveInput
        ? (right * axis.x + forward * axis.y).Normalized()
        : Vector3::ZERO;

    m_jumpRequested  = m_jumpRequested  || (!held && input.GetActionDown(actions::kJump));
    m_dodgeRequested = m_dodgeRequested || (!held && input.GetActionDown(actions::kDodge));
    // 押しっぱなしかどうかは押した瞬間では分からない。上昇中の重力を決めるために
    // 保持状態そのものを持つ。可変フレーム側で採り、固定ステップ側で使う。
    m_jumpHeld = !held && input.GetAction(actions::kJump);
    // 入力はここまで。抜く / 納める の中身は WeaponRigComponent の担当で、
    // このスクリプトは刀の存在もソケットも知らない。
    if (m_weaponRig && !held) {
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
        // 床が押し退けられた煙。揺れと音は «画面の外» の情報で、着地した «場所»
        // には今まで何も出ていなかった。跳んだ高さで煙の大きさが変わる。
        //
        // WHY 走行の土煙ではなく Ground Dust か: 走行のそれは «足が後ろへ掻いた»
        //     跡なので 1 方向へ吹く。着地は全周へ押し退けるので、向きを持たない
        //     方の口を使う (向きは吹く先の目安としてだけ渡す)。
        if (runDust && strength > 0.05f)
            if (auto* vfx = VfxManagerComponent::Instance()) {
                Vector3 outward = phy.GetVelocity();
                outward.y = 0.0f;
                vfx->PlayGroundDust(transform.worldPosition, outward.NormalizedOr(
                                        transform.worldRotation * Vector3::FORWARD),
                                    strength, Lerp(0.35f, 0.7f, strength));
            }
        // カメラを下へ沈める。膝が沈む向きと同じで、揺れより «重さ» が出る。
        if (landCameraDip > 0.0f && strength > 0.05f)
            if (auto* shake = CameraShakeManagerComponent::Instance())
                shake->Punch(Vector3{ 0.0f, -landCameraDip * strength, 0.0f }, 0.16f);
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
        m_lastFacing     = forward;
        m_hasLastFacing  = true;
        m_yawRateDegrees = 0.0f;
        return;
    }

    // 符号付きの回転角。Cross の y 成分が回転の向きをそのまま持っている
    // (左手系 Y-up では FORWARD → RIGHT の回転で +y、つまり正が右回り)。
    const float cross   = Vector3::Cross(m_lastFacing, forward).y;
    const float dot     = std::clamp(Vector3::Dot(m_lastFacing, forward), -1.0f, 1.0f);
    m_yawRateDegrees    = ToDeg(std::atan2f(cross, dot)) / dt;
    const float yawRate = std::fabs(m_yawRateDegrees);
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

inline void PlayerControllerComponent::Knockback(const Vector3& direction, float speed,
                                                 float seconds)
{
    const Vector3 flat{ direction.x, 0.0f, direction.z };
    if (flat.LengthSq() < EPSILON || speed <= 0.0f || seconds <= 0.0f) return;

    m_knockDir     = flat.NormalizedOr(Vector3::FORWARD);
    m_knockSpeed   = speed;
    m_knockSeconds = seconds;
    m_knockLeft    = seconds;
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

    // 座標を外から置かれている間 (登攀)。速度を毎ステップ 0 に落として重力も切る。
    //
    // WHY 速度を «0 にし続ける» 必要があるか: テレポート扱いの位置書き込みは
    //     PhysicsSystem が受け付けるが、速度は誰も触らない。切らずに置くと落下速度が
    //     登っている 3 秒ぶん積み上がり、操作を返した瞬間にその速さで甲板から落ちる。
    if (m_suspended) {
        // WHY 静的にするか: 剛体は物理世界に残ったままなので、脚や胴の当たりへ
        //     座標を置くたびにソルバーが押し戻す。静的な剛体は積分も衝突解決も
        //     飛ばされる (RigidBody::m_isStatic) ので、置いた座標がそのまま残る。
        if (!m_suspendApplied) {
            m_suspendApplied = true;
            physics.SetStatic(true);
        }
        phy.SetVelocity(Vector3::ZERO);
        phy.SetAngularVelocity(Vector3::ZERO);
        physics.SetGravityScale(0.0f);
        // 接地の «モード» は変えない (ForceGrounded は Automatic へ戻すまで latch する)。
        // Tick を回していない間はこの値がそのまま残るので、旗を直に立てれば足りる。
        cc->isGrounded  = true;
        cc->verticalSpeed = 0.0f;
        // 向きだけは掛ける側 (登攀) が決められるよう残す。脚へ正対しないと、
        // 横を向いたまま経路を滑り上がる絵になる。
        if (m_facing.LengthSq() > EPSILON) {
            Quaternion targetRotation =
                (Quaternion::LookRotation(m_facing) *
                 Quaternion::FromAxisAngle(Vector3::UP, ToRad(modelYawOffsetDegrees))).Normalized();

            // 傾けは «世界の軸» で外から掛ける。向いている先へ倒す軸は Cross(UP, facing)
            // ── 前後左右のどちらへ倒すかを、ヨーオフセットの有無に関わらず同じ式で書ける
            //    (BossCollapsePostureComponent の傾けと同じ規則)。
            if (std::abs(m_leanPitch) > EPSILON || std::abs(m_leanRoll) > EPSILON) {
                const Vector3 right = Vector3::Cross(Vector3::UP, m_facing).NormalizedOr(Vector3::ZERO);
                if (right.LengthSq() > EPSILON) {
                    targetRotation = (Quaternion::FromAxisAngle(right, ToRad(m_leanPitch)) *
                                      Quaternion::FromAxisAngle(Vector3::Cross(Vector3::UP, right),
                                                                ToRad(m_leanRoll)) *
                                      targetRotation).Normalized();
                }
            }

            // WHY 剛体ではなく Transform へ書くか (2026-09-10):
            //   静的な剛体の姿勢は «毎ステップ Transform から上書きされる»
            //   (PhysicsSystem::SyncRigidBodies は IsStatic() を無条件の
            //   テレポート扱いにする)。ここで phy.SetRotation() を呼んでも、
            //   同じフレームのうちに古い Transform の値で塗り直され、
            //   ステップ後の書き戻しでその古い値が Transform へ返る ──
            //   **向きも傾きも 1 度も変わらない**。座標を Place() で
            //   Transform へ書いているのと同じ理由で、姿勢も Transform が正本。
            //
            //   ワールド値も一緒に書くのは、この固定ステップが PrePhysics
            //   (local → world の組み直し) より後に走るため。ローカルだけ書くと、
            //   同じフレームの同期には «組み直す前の world» が渡る。
            GameObject* self = scene.Self();
            const Quaternion current = self ? self->transform.worldRotation : phy.GetRotation();
            const Quaternion next = Quaternion::Slerp(
                current, targetRotation,
                1.0f - std::exp(-std::max(TurnSpeed(), 0.0f) * 3.0f * dt)).Normalized();
            if (self) {
                // プレイヤーはシーンのルートなので local = world。
                self->transform.rotation      = next;
                self->transform.worldRotation = next;
            }
            phy.SetRotation(next);
        }
        return;
    }

    // 拘束が解けた。静的と重力は必ず戻す ─ 戻し忘れると «立ったまま動かない
    // プレイヤー» になり、原因が操作にも物理にも見えない止まり方をする。
    if (m_suspendApplied) {
        m_suspendApplied = false;
        physics.SetStatic(false);
        physics.SetGravityScale(1.0f);
    }

    cc->Tick(&phy, dt);

    const bool jumpRequested = m_jumpRequested;
    m_jumpRequested = false;

    // 押した瞬間を «預かり» へ移し替える。TickDodge が実際に出したときだけ空にする。
    if (m_dodgeRequested) m_dodgeBuffer = std::max(dodgeBufferSeconds, 0.0f);
    m_dodgeRequested = false;
    const bool dodgeRequested = m_dodgeBuffer > 0.0f;
    m_dodgeBuffer = std::max(0.0f, m_dodgeBuffer - dt);

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
            // 踏み切りの煙。足が床を蹴った «場所» に出る唯一の印で、これが無いと
            // 跳んだ瞬間が «浮き始めた» にしか見えない。走行の土煙を後ろへ吹かせる。
            if (runDust)
                if (auto* vfx = VfxManagerComponent::Instance()) {
                    Vector3 dir = phy.GetVelocity();
                    dir.y = 0.0f;
                    vfx->PlayRunDust(PlantedFootWorld(),
                                     dir.NormalizedOr(transform.worldRotation * Vector3::FORWARD),
                                     0.8f);
                }
        }

        // WHY 抜刀していても進行方向を向くか:
        //   銃の頃は «体を照準へ向けて、脚を 2D ブレンドで進行方向へ合わせる» 形だった。
        //   これは横走り・後ろ走りのクリップが揃っていて初めて成立する。双剣では
        //   ロコモーションを Katana_Idle / Walk_F / Run_F の前進 3 本だけにしたので、
        //   体を照準へ固定すると «正面を向いたまま横へ滑る» になる。
        //   斬る向きは体の向きではなく BladeComponent::SwingDirection() が
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
        Vector3 inputVelocity = m_hasMoveInput
            ? m_moveDirection * (MoveSpeed() * m_moveMagnitude * m_moveSpeedScale)
            : Vector3::ZERO;
        // 入力が無いときの -1 は減速の合図。水平速度の出どころは移動入力だけなので、
        // 押していないフレームは必ずブレーキが掛かる。
        float acceleration = m_hasMoveInput
            ? (cc->isGrounded ? GroundAccel() : AirAccel())
            : -1.0f;

        // 被弾の押し。残り時間で細らせながら «入力の代わり» に置く。
        //
        // WHY 入力へ足さずに混ぜるか: 足すと押されている間にスティックを倒せば
        //     ほぼ打ち消せてしまい、押した意味が消える。逆に入力を完全に殺すと
        //     «操作を取り上げられた» になる。残り時間で比率を渡す。
        if (m_knockLeft > 0.0f) {
            m_knockLeft = std::max(0.0f, m_knockLeft - dt);
            const float ratio = m_knockSeconds > EPSILON ? m_knockLeft / m_knockSeconds : 0.0f;
            inputVelocity = m_knockDir * (m_knockSpeed * ratio) + inputVelocity * (1.0f - ratio);
            acceleration  = std::max(GroundAccel(), 1.0f);
        }
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

inline float PlayerControllerComponent::DodgeProgress01() const
{
    if (m_dodgeRemaining <= 0.0f) return 1.0f;
    return Clamp01(1.0f - m_dodgeRemaining / std::max(DodgeDuration(), 0.0001f));
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

        // 入力方向へ跳ぶ。無入力なら «前» へ踏み込む。
        //
        // WHY 後退ではなく前進か (2026-09-05 に反転):
        //   1. 回避クリップは前方への踏み切り 1 本しか無い。後ろへ運ぶと、
        //      横回避と同じ «クリップと進行方向が食い違う» 滑りが残る。
        //   2. ボス 1 体との近接戦では、離れることは «次の一手が遠くなる» でしかない。
        //      とっさに押した回避が毎回間合いを空けると、詰め直しに戦闘時間を使う。
        //   3. ジャスト回避の報酬は «次の一振りが満溜め» (Flux)。射程 2.6m の技なので、
        //      4.5m 下がると報酬を使う前に走って戻ることになる。
        //   無敵は全区間にあるので、前進はそのまま «攻撃を潜り抜ける» になる。
        Vector3 direction = hasInput ? moveDirection : GetMoveForward();
        direction.y = 0.0f;
        if (direction.LengthSq() < EPSILON) return;

        m_dodgeDirection = direction.Normalized();
        m_dodgeRemaining = DodgeDuration();
        m_dodgeBuffer    = 0.0f;   // 出した。預かりはここで空にする
        m_dodgeScuffed   = false;
        ++m_dodgeSerial;

        // 体を回避方向へ向ける。
        //
        // WHY 入力があるときだけか: 無入力の «前» はカメラ前方で、体はロック対象へ
        //     向いている (TickAimTurn)。両者は Aim Turn Deadzone のぶんまでずれるので、
        //     ここで回すと狙っている相手から最大 38 度だけ体が跳ねる。
        //     どちらもほぼ同じ向きなので、回さない方が絵が落ち着く。
        // WHY 補間せず即入れるか: 0.24 秒しかない動きを普段の追従で回すと «跳び終わって
        //     から向き終わる»。回避クリップは前方への踏み切り 1 本しか無いので、
        //     向かないまま横へ運ぶと «正面を向いたまま滑る» になる。
        if (hasInput) {
            phy.SetRotation(
                (Quaternion::LookRotation(m_dodgeDirection) *
                 Quaternion::FromAxisAngle(Vector3::UP, ToRad(modelYawOffsetDegrees)))
                    .Normalized());
        }
        // クールダウンは回避の開始から数える。実質的な待ち時間は
        // DodgeCooldown - DodgeDuration になる。
        m_dodgeCooldown  = DodgeCooldown();
        animator.SetTrigger(paramDodgeTrigger);
        se::Play(audio, se::kPlayerDodge);

        // 出だしを «弾けた» と読ませる。走りと回避の違いは速さだけなので、
        // 蹴り出しの煙とカメラの引きが無いと «少し速く走った» にしか見えない。
        if (runDust)
            if (auto* vfx = VfxManagerComponent::Instance())
                vfx->PlayRunDust(PlantedFootWorld(), m_dodgeDirection, 1.0f);
        // カメラを 1 往復ぶん後ろへ引く。画角が開いて、同じ速度でも速く見える。
        if (dodgeCameraKick > 0.0f)
            if (auto* shake = CameraShakeManagerComponent::Instance())
                shake->Punch(Vector3{ 0.0f, 0.0f, -dodgeCameraKick }, 0.18f);
        // 開始フレームからそのまま下の速度上書きへ進む。
        // ここで return すると 1 フレームだけ慣性で滑り、出だしが鈍る。
    } else {
        m_dodgeRemaining = std::max(0.0f, m_dodgeRemaining - dt);
        if (m_dodgeRemaining <= 0.0f) {
            // 終わり際の踏ん張り。開始音だけだと、回避がいつ終わって
            // 次の入力を受け付けるのかが耳から分からない。
            se::Play(audio, se::kPlayerDodgeEnd);
            // 抜けた足元の «踏ん張り» の煙。出だしの煙だけだと、回避が «どこで
            // 終わったか» が床に残らず、転がりの距離が絵から読めない。
            if (runDust)
                if (auto* vfx = VfxManagerComponent::Instance())
                    vfx->PlayRunDust(PlantedFootWorld(), m_dodgeDirection, 0.6f);

            // 抜けた «瞬間» に走りの速さまで落とす。
            //
            // WHY 通常移動の減速に任せないか: この後を引き継ぐのは Ground Decel で、
            //     回避速度 (走りの 2.8 倍) から落とし切るまで時間が要る。100 m/s² でも
            //     28 → 10 に 0.18 秒かかり、そのあいだ入力と無関係に滑り続ける。
            //     回避の距離を伸ばすほどこの尾も伸びるので、速さで距離を作る設計とは
            //     両立しない。向きは保ったまま «走っている状態» へ直接繋ぐ。
            Vector3 vel = phy.GetVelocity();
            const float planar = std::sqrt(vel.x * vel.x + vel.z * vel.z);
            const float cap    = MoveSpeed();
            if (planar > cap && planar > EPSILON) {
                const float scale = cap / planar;
                vel.x *= scale;
                vel.z *= scale;
                phy.SetVelocity(vel);
            }
            return;  // 今フレームで終了
        }
    }

    // 回避中は水平速度を上書きし続ける。
    // WHY 開始時に一度入れて終わりにしないか: 敵や壁に接触した瞬間にソルバーが
    //     速度を削るため、途中で失速して「跳んだのに動かない」になる。持続時間の
    //     あいだ毎フレーム入れ直すことで、移動距離が入力に対して安定する。
    //
    // 速さは出だしが最大で、終わりに向けて Dodge End Speed x まで線形に落ちる。
    // 等速で運ぶと «床を一定速度で滑っている» 絵になり、距離を伸ばすほど氷に見える。
    const float total    = std::max(DodgeDuration(), 0.0001f);
    const float progress = Clamp01(1.0f - m_dodgeRemaining / total);
    const float speed    = DodgeSpeed() * Lerp(1.0f, DodgeEndSpeedRatio(), progress);

    // 転がっている最中に床を擦る 1 発。踏み切りと踏ん張りの «あいだ» を埋める。
    // 煙は体が通り過ぎた側へ流れるので、進行方向の逆へ吹かせる。
    if (!m_dodgeScuffed && runDust && dodgeScuffAt > 0.0f && progress >= dodgeScuffAt) {
        m_dodgeScuffed = true;
        if (auto* vfx = VfxManagerComponent::Instance())
            vfx->PlayGroundDust(PlantedFootWorld(), -m_dodgeDirection, 0.35f, 0.7f);
    }

    Vector3 vel = phy.GetVelocity();
    vel.x = m_dodgeDirection.x * speed;
    vel.z = m_dodgeDirection.z * speed;
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
        // 旋回の傾き。曲がっている側へ上体を倒す (Turn Lean の WHY)。
        // 回避中は転がりのクリップが体の向きを持っているので触らない。
        Vector3 planar = rb->rigidBody->GetVelocity();
        planar.y = 0.0f;
        const float speed = planar.Length();
        if (turnLeanPerTurn > 0.0f && speed > 0.5f && !IsDodging()) {
            // 正の旋回速度は右回り。右回りなら右へ倒す = 進行方向の右手側。
            const Vector3 right = Vector3::Cross(Vector3::UP, planar / speed);
            const float turns   = m_yawRateDegrees / 360.0f;
            const float amount  = Clamp(turns * turnLeanPerTurn,
                                        -std::max(turnLeanMax, 0.0f),
                                         std::max(turnLeanMax, 0.0f))
                                * Clamp01(speed / std::max(MoveSpeed(), EPSILON));
            const Vector3 localRight =
                (transform.worldRotation.Inverse() * right).NormalizedOr(Vector3::RIGHT);
            desiredOffset += localRight * amount;
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
