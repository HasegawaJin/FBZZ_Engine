/// @file    PlayerControllerComponent.hpp
/// @brief   RigidBody ベースのプレイヤー移動・ジャンプ・回避。
/// @author  Hasegawa Jin
/// @date    2026-08-19
///
/// @note 入力は可変フレームで採取し固定ステップで物理へ反映する (取りこぼしと
///       フレームレート依存を防ぐ)。回避の無敵判定はここでは行わない
///       («今回避中か» を答えるだけで、報酬配分は攻撃を弾く側の PlayerComponent が持つ)。
/// @note 水平速度は移動入力と回避だけに閉じる。攻撃や纏いから外部速度を受け取れると、
///       押していないフレームに体が動く原因を画面から切り分けられなくなる。
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
#include <Scripts/Game/TimeManagerComponent.hpp>
#include <Scripts/Game/ImpactFeedbackManagerComponent.hpp>
#include <Scripts/Game/VfxManagerComponent.hpp>
#include <Scripts/Player/PlayerAimComponent.hpp>
#include <Scripts/Player/PlayerBreathComponent.hpp>
#include <Scripts/Player/WeaponRigComponent.hpp>
#include <Scripts/Utils/InputActions.hpp>
#include <Scripts/Utils/SeLibrary.hpp>
#include <Scripts/Utils/LoopVoice.hpp>
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

    /// 接地判定と固定ステップの水平速度制御はこの 2 つが揃って初めて成立する。
    /// どちらかが欠けると「入力は読めているのにキャラが動かない」無言の状態になる。
    FBZZ_REQUIRE_COMPONENT(CharacterControllerComponent, RigidBodyComponent)
    /// 無くても移動そのものは成立する。Animator が無ければパラメーター送信が空振りし、
    /// IKSolver が無ければ足の接地補正と上体の傾きが省かれるだけ。
    FBZZ_OPTIONAL_COMPONENT(AnimatorComponent, IKSolverComponent)

public:
    /// PlayerComponent が必須参照を束ねて注入する。移動値をここへ複製しないことで、
    /// Inspector では PlayerTuning.fzdata だけが調整値の正本になる。
    fbzz::Asset<PlayerTuning> tuning{};

    FBZZ_GROUP("移動")
    FBZZ_FIELD_RANGE(float, modelYawOffsetDegrees, 180.0f, "モデルのヨー補正",  0.0f, 360.0f)
    FBZZ_FIELD(bool, useCameraForward,      true, "カメラの正面を使う")
    FBZZ_FIELD(bool, rotateToMoveDirection, true, "進行方向へ向く")
    FBZZ_FIELD(bool, useFootIK,             true, "足 IK を使う")

    FBZZ_GROUP("Dodge")
    FBZZ_TOOLTIP("速さと距離。無敵とジャスト回避は PlayerTuning の Dodge (Docs/camera-controls.md)")

    /// @note 物理入力を Inspector に持たない: 持つとゲームパッド対応もキーコンフィグも
    ///       原理的に不可能になる。このスクリプトは論理名だけを知り、実際の割り当ては
    ///       ProjectSettings/Input.inputactions が持つ (名前は InputActions.hpp)。

    FBZZ_GROUP("アニメーターのパラメーター")
    FBZZ_FIELD(std::string, paramSpeed,         "Speed",        "Speed Param")
    FBZZ_TOOLTIP("移動の速さ (m/s)。ロコモーションの Idle → Walk_F → Run_F がこれで決まる")
    /// @note 銃の頃は体が照準を向き脚だけ進行方向へ合わせるため 2 軸 (MoveX/MoveY) で
    ///       横走り・後ろ走りへブレンドしていたが、双剣は体ごと進行方向を向くため
    ///       脚と体の向きが常に一致し、残る自由度は «速さ» の 1 軸だけになった。
    FBZZ_FIELD(std::string, paramVerticalSpeed, "VerticalSpeed","Vertical Speed Param")
    FBZZ_FIELD(std::string, paramIsGrounded,    "IsGrounded",   "IsGrounded Param")
    FBZZ_FIELD(std::string, paramJumpTrigger,   "Jump",         "Jump Trigger Param")
    FBZZ_FIELD(std::string, paramDodgeTrigger,  "Dodge",        "Dodge Trigger Param")

    /// @note 回避が瞬間移動に見える原因は速さでなく、進む時間 (Dodge Duration) と
    ///       転がるクリップの長さが違うこと。直し方はクリップの再生速度を合わせること
    ///       だが、1 ステートの速度は Script から書けない (animator.SetSpeed は Animator
    ///       全体に掛かる) ため、実際の速度は Player.animcontroller の Dodge_Roll
    ///       ステートが持つ。ここではクリップ長から導出した値を Debug へ出すだけにし、
    ///       Dodge Duration を触った人が次に直す先を Inspector 上で見つけられるようにする。
    FBZZ_FIELD_RANGE(float, dodgeClipSeconds, 1.40f, "回避クリップの尺 [秒]", 0.05f, 3.0f)
    FBZZ_TOOLTIP("Dodge (回避ローリング) の尺 [秒]。クリップを差し替えたら直すこと")
    FBZZ_FIELD_READ_ONLY(float, debugDodgeClipSpeed, 0.0f, "回避クリップの再生速度")
    FBZZ_TOOLTIP("Player.animcontroller の Dodge_Roll ステートの speed に入れる値。"
                 "Dodge Duration を変えたらこの数字を写す")

    /// @note 腕で狙う姿勢は双剣で無くなった (Docs/blades.md、クリップ 10 本ごと廃止)。
    ///       9 セルのエイム (AimYaw/AimPitch/Aim レイヤー) は持たず、ロックオンした
    ///       相手を向くのは下の «立ち止まったときの旋回» だけになる。
    FBZZ_GROUP("ロックオン旋回")
    FBZZ_FIELD(bool, turnToAim, true, "狙いの方へ向く")
    FBZZ_TOOLTIP("立ち止まっているあいだ、ロック対象の方へ体を回す (移動入力が無いときだけ)")
    FBZZ_FIELD_RANGE(float, aimTurnDeadzoneDegrees, 38.0f, "旋回の不感帯", 0.0f, 90.0f)
    FBZZ_TOOLTIP("この角度までは体を回さない。0 に近づけるほど対象へ吸い付く")

    FBZZ_GROUP("跳躍 (導出値)")
    /// 重力は ProjectSettings の [physics] gravity、到達点は PlayerTuning。
    /// 導出結果をここに出さないと、重力を触った影響が Play するまで分からない。
    FBZZ_FIELD_READ_ONLY(float, debugWorldGravity, 0.0f, "ワールドの重力")
    FBZZ_FIELD_READ_ONLY(float, debugJumpSpeed, 0.0f, "跳躍の初速")
    FBZZ_FIELD_READ_ONLY(float, debugTimeToApex, 0.0f, "頂点までの時間")
    FBZZ_FIELD(bool, autoTuneGrounding, true, "接地判定を自動調整")
    FBZZ_TOOLTIP("接地判定のしきい値を重力から決め直す。"
                 "切ると CharacterController に入っている値をそのまま使う")

    /// @note 走りのクリップは 1.0 倍で 1 秒に数歩しか踏まないのに体は 10 m/s で進み、
    ///       脚の運びと進む量がずれて地面を滑って見える (足音は距離刻みなので耳と目の
    ///       歩数も食い違う)。再生を速さに比例させ、足音もその歩調から刻む。
    FBZZ_GROUP("モーションの速さ")
    FBZZ_FIELD(std::string, locomotionStateName, "Locomotion", "Locomotion State")
    FBZZ_FIELD_RANGE(float, runClipSpeed, 6.0f, "Run Clip Speed [m/s]", 0.0f, 20.0f)
    FBZZ_TOOLTIP("走りのクリップを 1.0 倍で流したときに釣り合う速さ。これより速く走ると再生が "
                 "比例して速くなる。下げるほど脚が速く回る。0 で従来 (常に 1.0 倍・足音は Run Stride)")
    FBZZ_FIELD_RANGE(float, runPlaybackMax, 1.8f, "Run Playback Max", 1.0f, 3.0f)
    FBZZ_TOOLTIP("走りの再生速度の上限。上げすぎると脚が «回転している» に見える")
    FBZZ_FIELD_RANGE(float, runClipSeconds, 0.60f, "Run Clip Length [s]", 0.1f, 3.0f)
    FBZZ_TOOLTIP("走り (Run_F) の 1 周の長さ。1 周 2 歩として足音の間隔を出す")
    /// @note 回避は出だしで弾けて尻すぼみに落ちる (Dodge End Speed) が、転がりは
    ///       等速で回っていた。出だしは脚が置いていかれ、終わりは止まりかけの体の上で
    ///       転がりだけが回り続ける。転がりの再生を移動の減速に合わせて揃える。
    FBZZ_FIELD(std::string, dodgeStateName, "Dodge_Roll", "Dodge State")
    FBZZ_FIELD_RANGE(float, dodgeAnimFollow, 0.7f, "Roll Follows Speed", 0.0f, 1.0f)
    FBZZ_TOOLTIP("転がりの再生をどれだけ移動の速さに沿わせるか。0 で等速。"
                 "平均は変えないので、転がりが回避の尺で終わるのは変わらない")
    /// @note 小さな段差でも深く膝を折る 0.23 秒が毎回入ると走りが «つっかえる» ため、
    ///       高く落ちたときだけ重く着くよう落下速度で再生を速める。
    FBZZ_FIELD(std::string, landStateName, "Player_Land", "Land State")
    FBZZ_FIELD_RANGE(float, landFastSpeed, 1.7f, "Light Land Speed", 1.0f, 3.0f)
    FBZZ_TOOLTIP("低い着地の再生速度。Land Feedback At の落下速度で 1.0 倍 (重い着地) になる")
    FBZZ_FIELD_READ_ONLY(float, debugRunPlayback, 1.0f, "Run Playback")

    FBZZ_GROUP("カメラの手応え")
    /// @note 「跳ぶとどれくらい緩めたいか」は動作側の意図、「緩んだときカメラがどう
    ///       動くか」はカメラの作りで別物。混ぜるとカメラの追従方式を変えるたびに
    ///       動作側の数値を入れ直すことになるため、ここに置く。
    FBZZ_FIELD_RANGE(float, airCameraSlack, 0.85f, "Air Slack (Vertical)", 0.0f, 1.0f)
    FBZZ_TOOLTIP("空中にいる間、カメラの縦追従を緩める量。0 で従来どおり密着する")
    /// @note 回避はクールダウン中/回避中でも押される。その瞬間に捨てると «連打している
    ///       のに出ない» になり理由も画面に出ないため、早すぎた入力を明けた 1 フレーム
    ///       目へ運ぶ。
    FBZZ_FIELD_RANGE(float, dodgeBufferSeconds, 0.15f, "Dodge Buffer", 0.0f, 0.5f)
    FBZZ_TOOLTIP("回避入力を預かる秒数。クールダウンや回避中に押しても、明けた瞬間に出る。"
                 "0 で先行入力なし (押した瞬間しか受け付けない)")
    /// @note 押した固定ステップで接地していなければ捨てていたため、着地の 1〜2 フレーム
    ///       前・回避中・縁から踏み出した直後の押下がすべて «押したのに跳ばない» に
    ///       なっていた (穴は落ちるとダメージなので、縁で跳べないのが一番理不尽に感じる)。
    FBZZ_FIELD_RANGE(float, jumpBufferSeconds, 0.12f, "Jump Buffer", 0.0f, 0.5f)
    FBZZ_TOOLTIP("跳躍入力を預かる秒数。着地の直前や回避中に押しても、跳べるようになった瞬間に出る")
    FBZZ_FIELD_RANGE(float, coyoteSeconds, 0.10f, "Coyote Time", 0.0f, 0.3f)
    FBZZ_TOOLTIP("足場を離れてからまだ跳べる秒数。跳んだ後には効かない (二段跳びにはならない)")
    FBZZ_FIELD_RANGE(float, dodgeCameraKick, 0.55f, "Dodge Camera Kick (m)", 0.0f, 2.0f)
    FBZZ_TOOLTIP("回避の出だしにカメラを後ろへ引く距離。揺れではなく 1 往復の押し込み。"
                 "画角が開くぶん、同じ速度でも速く見える")
    /// @note 回避の尺をクリップと同じ 1.4 秒にすると、抜け道が «斬撃で打ち切る»
    ///       (BladeComponent の Dodge -> Slash At) しか無く、攻撃を出さない限り
    ///       1.4 秒動けなかった。移動入力でも抜けられるようにし、無敵が切れる時刻より
    ///       早くは抜けられないようにする (早すぎると «無敵の出だしだけ使って即動く»
    ///       が最適解になる)。既定 0.25 は dodgeInvulnerableSeconds 0.35 ÷ dodgeDuration 1.4。
    FBZZ_FIELD_RANGE(float, dodgeMoveCancelAt, 0.25f, "Dodge -> Move At", 0.0f, 1.0f)
    FBZZ_TOOLTIP("回避の進み [0,1] がここを越えたら、移動入力で回避の残りを捨てる。1 で無効")
    FBZZ_FIELD_RANGE(float, dodgeCameraSlack, 0.82f, "Dodge Slack (Horizontal)", 0.0f, 1.0f)
    FBZZ_TOOLTIP("回避中、カメラの横追従を緩める量")
    /// @note 密着したままだと走っていてもプレイヤーが画面内で止まって見え、動いているのは
    ///       背景だけになる。緩めるとカメラが遅れてプレイヤーが進行方向へ流れ、画面中央
    ///       固定のクロスヘアとの「自分は流れるが狙う点は動かない」関係が絵に出る。
    FBZZ_FIELD_RANGE(float, moveCameraSlack, 0.45f, "Move Slack (Horizontal)", 0.0f, 1.0f)
    FBZZ_TOOLTIP("最高速で走っているとき、カメラの横追従を緩める量。"
                 "速度に比例するので歩き出しではほとんど緩まない。0 で従来どおり密着する")
    FBZZ_FIELD_RANGE(float, landFeedbackSpeed, 12.0f, "Land Feedback At", 0.0f, 60.0f)
    FBZZ_TOOLTIP("この落下速度 (m/s) で着地の反応が最大になる。0 で着地演出を切る")
    /// @note 揺れは向きを持たないため、着地の «重さが床へ落ちた» 向きが出ない。
    ///       カメラが一瞬下がって戻ると膝が沈んだ向きと画面の動きが揃い、体重が伝わる。
    FBZZ_FIELD_RANGE(float, landCameraDip, 0.14f, "Land Camera Dip (m)", 0.0f, 1.0f)
    FBZZ_TOOLTIP("最大の落下で着地した瞬間にカメラが下へ沈む距離。1 往復で戻る")

    /// @note 走りながら向きを変えると脚は進行方向へ回るが上体は直立のままで、氷上を
    ///       滑る台車に見える。曲がる側へ上体を倒すと遠心力に逆らう «体重» が絵に出る
    ///       (坂の前傾 UpdateSlopeLean と同じ Spine IK の目標を借用)。
    FBZZ_GROUP("Turn Lean")
    FBZZ_FIELD_RANGE(float, turnLeanPerTurn, 0.30f, "Lean Per Turn (m)", 0.0f, 1.0f)
    FBZZ_TOOLTIP("毎秒 1 回転 (360 deg/s) で回ったときに上体を曲がる側へ倒す量。"
                 "0 で切る。速さに比例して効くので、歩きではほとんど倒れない")
    FBZZ_FIELD_RANGE(float, turnLeanMax, 0.16f, "Lean Max (m)", 0.0f, 0.5f)
    FBZZ_TOOLTIP("倒す量の上限。急旋回で上体が横へ飛ぶのを止める")

    /// @note 足接地は本来 Event Track で鳴らすべきだが、インポート後の .anim にイベントは
    ///       無く、無いときの挙動が「無音」で壊れていても気付けない。歩いた距離で刻めば
    ///       クリップの状態に依存しない (Event Track を打ったら strideRun を 0 にして切る)。
    FBZZ_GROUP("Footsteps")
    FBZZ_FIELD_RANGE(float, strideWalk, 0.75f, "Walk Stride (m)", 0.0f, 4.0f)
    FBZZ_TOOLTIP("この距離だけ進むごとに歩き足音を 1 回鳴らす。0 で歩きの足音を切る")
    FBZZ_FIELD_RANGE(float, strideRun, 1.35f, "Run Stride (m)", 0.0f, 4.0f)
    FBZZ_TOOLTIP("走りの歩幅。0 で足音そのものを切る")
    FBZZ_FIELD_RANGE(float, runSpeedThreshold, 4.0f, "Run At (m/s)", 0.1f, 20.0f)
    FBZZ_TOOLTIP("この水平速度を超えたら走りの足音・歩幅へ切り替える")

    /// @note 歩いている間は接地音が «動いている» を担うため旋回音を重ねると 1 歩に 2 音
    ///       乗るが、立ち止まって向きだけ変えると足音の条件に入らず無音になる。
    ///       «歩いていない» ときだけ鳴らして担当を分ける。
    FBZZ_FIELD_RANGE(float, servoTurnRate, 60.0f, "Servo Turn At (deg/s)", 5.0f, 720.0f)
    FBZZ_TOOLTIP("その場でこの角速度を超えて回ったら脚部サーボの音を鳴らす。0 に近づけると常時鳴る")
    FBZZ_FIELD_RANGE(float, servoTurnInterval, 0.35f, "Servo Turn Interval", 0.05f, 3.0f)
    FBZZ_TOOLTIP("サーボ音の最短間隔。回し続けている間はこの間隔で繰り返す")

    /// @note 土煙は «足が地面を蹴った» ことの絵で足音はその音。別のしきい値・タイマーで
    ///       刻むと走り出しや坂で片方だけ出て音と絵が別動作に見えるため、足音と同じ
    ///       «走り» の判定に乗せる (濃さ・大きさは VfxManagerComponent の Run Dust が持つ)。
    FBZZ_GROUP("走りの砂埃")
    FBZZ_FIELD(bool, runDust, true, "走りの砂埃")
    FBZZ_TOOLTIP("走っているあいだ、足が着くたびに足元へ土煙を出す")
    /// @note 回避で床に残るのは «踏み切り» と «踏ん張り» の 2 発だけで、いちばん速く
    ///       動く 0.2 秒ほど床と関わらず跳んでいるように見える。体が最も低くなる
    ///       あたりで擦れの煙を足すと «床の上の動き» に戻る (スイッチは runDust と共有)。
    FBZZ_FIELD_RANGE(float, dodgeScuffAt, 0.42f, "回避の擦れ", 0.0f, 1.0f)
    FBZZ_TOOLTIP("回避のどこで床を擦る煙を出すか (0 = 踏み切り / 1 = 抜け際)。"
                 "1.0 で踏ん張りの煙と重なるので実質オフ")
    FBZZ_FIELD(std::string, footBoneLeftName,  "Foot_L", "Left Foot Bone")
    FBZZ_FIELD(std::string, footBoneRightName, "Foot_R", "Right Foot Bone")
    FBZZ_TOOLTIP("土煙を立てる位置。見つからなければプレイヤー原点から出す")

    FBZZ_GROUP("Player Hit Animation")
    FBZZ_FIELD(std::string, hitLayerName, "HitReaction", "Hit Layer")
    FBZZ_FIELD_FILE(hitFrontClipFile,
        "guid:a811fee1fde3556ba08c66eb375ee675|Library/Baked/a417880436274df4a656ac7cd43a45aa/anims/Hit.anim",
        "Front Hit Clip", ".anim,.fbx")
    FBZZ_FIELD(std::string, hitFrontClipName, "Hit", "Front Hit Clip Name")

    /// 回避中か。被弾処理や敵 AI が「今は掴めない」を判断するのに使える。
    [[nodiscard]] bool  IsDodging() const { return m_dodgeRemaining > 0.0f; }
    /// 回避の «出だしから seconds 秒» の内側か。無敵の窓をここ 1 つで測る。
    [[nodiscard]] bool  InDodgeIFrames(float seconds) const
    {
        if (m_dodgeRemaining <= 0.0f) return false;
        return (DodgeDuration() - m_dodgeRemaining) <= std::max(seconds, 0.0f);
    }
    /// 回避を始めた回数。ジャスト回避の報酬を «同じ 1 回の回避» で二重に配らないための札。
    [[nodiscard]] int   DodgeSerial() const { return m_dodgeSerial; }
    /// 今の回避が運んでいる向き (水平・正規化済み)。
    ///
    /// @note モデルのヨー補正が 180 度入っているため、transform.forward では代用できない
    ///       (根の «正面» は進んでいる向きの逆を指す)。
    [[nodiscard]] const Vector3& DodgeDirection() const { return m_dodgeDirection; }
    /// UI 用。1 = 回避可能 / 0 = 使った直後。
    [[nodiscard]] float DodgeCharge() const;
    /// 今の回避がどこまで進んだか。0 = 踏み切った瞬間 / 1 = 抜け切る直前。
    ///
    /// @note 回避の速さは出だしが最大で終わりに向けて落ちる (Dodge End Speed)。見せる側
    ///       (残像) がこれを知らないと、減速し切った終わり際まで出だしと同じ濃さで残り、
    ///       «最後まで同じ速さで滑った» に見えるため外へ出す。
    [[nodiscard]] float DodgeProgress01() const;
    /// 回避の残りを捨てて抜ける。抜けた瞬間の後始末 (走りの速さまで落とす) は時間切れと同じ。
    /// 回避中でなければ何もせず false。
    /// @note 転がりの後半に押した斬撃が回避が明けるまで待たされ、かわして踏み込む
    ///       1 続きの動きが切れていたため外から切れるようにする。打ち切った瞬間に
    ///       無敵も消えるので、早く斬れるぶんの代償は払われる。
    bool EndDodgeEarly();

    void OnStart() override;
    void OnUpdate() override;
    void OnFixedUpdate() override;
    void OnDisable() override { StopMotor(); }
    void OnDestroy() override { StopMotor(); }
    /// 被弾通知から呼ばれる標準の正面リアクション。
    void PlayHitAnimation();

    /// 被弾で押される。direction は水平の向き (正規化は中で行う)。
    /// @note 水平速度の出どころは `cc->Move` に渡す入力速度 1 つだけで、そこへ辿り着かない
    ///       AddImpulse は次の FixedUpdate で必ず打ち消される (物理の力積では効かない)。
    ///       押しは «一定時間、入力の代わりに置かれる速度» として扱う。押しが無いと
    ///       被弾のコストが HP だけになり間合いが変わらない。
    void Knockback(const Vector3& direction, float speed, float seconds);
    /// PlayerComponent が内部モジュールとして保持するときのエイム結果の注入先。
    void SetAimComponent(PlayerAimComponent* aim) { m_aimOverride = aim; }
    /// 武器の挙動そのものは WeaponRigComponent が持つ。ここは要求を渡すだけ。
    void SetWeaponRig(WeaponRigComponent* rig) { m_weaponRig = rig; }
    /// 回避 1 回ぶんの息を払う先。未設定なら息を見ずに転がる (息を持たない盤面)。
    void SetBreath(PlayerBreathComponent* breath) { m_breath = breath; }

    /// 外から掛ける移動速度の倍率。溜めている間に足を鈍らせる用途 (BladeComponent)。
    /// @note «掴んでいる» を知っているのは手の側だけなので、掛ける側が毎フレーム設定する。
    ///       設定しなくなれば次のフレームに 1.0 へ戻るため、解除忘れで足が遅いまま
    ///       張り付く事故が起きない。
    void RequestMoveSpeedScale(float scale)
    {
        m_requestedMoveScale = Clamp(scale, 0.05f, 1.0f);
    }

    /// この向きへ体を向けるよう 1 フレームぶん要求する (斬撃が振る向きを渡す)。
    /// @note 斬る向きを決めるのは剣 (BladeComponent::SwingDirection) で、こちらが
    ///       独自にロック対象へ向き続けると «体は敵を向いているのに刃は画面奥へ抜ける»
    ///       食い違いが生まれるため振る向きは 1 つに統一する。移動入力より優先し、
    ///       走りながら横へ斬っても «誰に向かって振ったか» が体の向きに残るようにする。
    void RequestFacing(const Vector3& direction)
    {
        Vector3 flat = direction;
        flat.y = 0.0f;
        if (flat.LengthSq() > EPSILON) m_requestedFacing = flat.Normalized();
    }

    /// 座標を外から置く演出のあいだ、移動・跳躍・回避・重力の適用を止める。
    /// RequestMoveSpeedScale と同じく、押し続けている間だけ効く。
    /// @note cutscene::HoldsPlayer は «入力» だけ止め重力と衝突解決はそのまま進むため、
    ///       3 秒以上かけて座標を置き換える演出 (登攀) では落下速度が積み上がってしまう。
    ///       向きだけは残す (登っている向きを掛ける側が決められなくなるため)。
    /// `lockInput` を立てると刀の入力 (斬撃・弾き・とどめ) まで止まる。cutscene の申告は
    /// «最後に書いた人が勝つ» 一枚のため、ボスのカメラ演出が毎フレーム holdPlayer=false を
    /// 置き直すと数秒またぐ拘束が途中で外れる。押し続ける形の錠をこちらに別に持つ。
    void RequestSuspend(bool lockInput = false)
    {
        m_requestedSuspend = true;
        m_requestedInputLock = m_requestedInputLock || lockInput;
    }

    /// 拘束中の体をこの角度だけ倒す。pitch は前へ、roll は右へ (どちらも度)。
    /// RequestFacing と同じく押し続けている間だけ効く。
    /// @note 向き (RequestFacing) は «どこを向くか»、これは «どう構えるか» で別物。
    ///       壁を登る体は脚へ正対したまま斜面へ倒れ込むため、1 つの要求にまとめると
    ///       倒す角度を変えるたびに向きが回ってしまう。拘束中だけ効かせるのは、普段の
    ///       姿勢は接地・移動・回避が奪い合っていて «誰が体を倒したか» が追えなくなるため。
    void RequestLean(float pitchDegrees, float rollDegrees)
    {
        m_requestedLeanPitch = pitchDegrees;
        m_requestedLeanRoll  = rollDegrees;
    }
    [[nodiscard]] bool IsSuspended()   const { return m_suspended; }
    [[nodiscard]] bool IsInputLocked() const { return m_inputLocked; }

    /// 見た目の正面を +Z とする回転。transform.worldRotation はモデルのヨーオフセットを
    /// 含むため、そのまま使うと「敵が正面に居るのに腕が真後ろを向く」になる。
    /// 公開しているのは、狙いの向きを扱う他のモジュールが同じヨーオフセットを
    /// 二重に持たないため。オフセットの正本は Model Yaw Offset ただ 1 つ。
    [[nodiscard]] Quaternion ModelRotation() const;

private:
    /// 回避の開始判定と、回避中の速度上書き。
    void TickDodge(const Vector3& moveDirection, bool hasInput,
                   bool dodgeRequested, RigidBody& phy, float dt);
    /// 回避を抜けた瞬間の後始末。時間切れと打ち切り (EndDodgeEarly) が同じ 1 本を通る。
    void FinishDodge(RigidBody& phy);
    /// 走り・転がりのステートの再生速度を今の速さから決める。毎フレーム呼ぶ。
    void DriveMotionSpeeds(float planarSpeed);
    void UpdateIK();
    /// 上り / 下り / 早離しで重力を切り替える。倍率は World の重力に対する比。
    void UpdateGravityScale(const CharacterControllerComponent& cc, const RigidBody& phy);
    /// 接地判定のしきい値を、今の重力と跳躍の長さから決め直す。
    void TuneGrounding(CharacterControllerComponent& cc) const;
    /// 空中・回避・走行のあいだ、カメラの追従を緩めるよう要求する。
    void RequestCameraSlack(const CharacterControllerComponent& cc, float planarSpeed);
    /// 空中から接地へ変わった瞬間を掴み、落下速度の分だけ手触りを鳴らす。
    void TickLanding(const CharacterControllerComponent& cc, const RigidBody& phy);
    /// 接地して進んだ距離を積み、歩幅ぶん進むごとに足音と土煙を 1 回出す。
    /// その場旋回の脚部サーボ。歩かずに向きだけ変えたときの «動いている» を返す。
    void TickServoTurn(float planarSpeed, float dt);
    void TickFootsteps(const CharacterControllerComponent& cc, const Vector3& velocity,
                       float planarSpeed);
    void CacheFootBones();
    /// 今まさに着いている足のワールド位置。土煙はここから立てる。
    [[nodiscard]] Vector3 PlantedFootWorld();
    /// 立ち止まっているあいだ、ロック対象からずれたヨーを体の旋回で詰める。
    void TickAimTurn(RigidBody& phy, float dt);
    void UpdateSlopeLean(IKSolverComponent& ik, CharacterControllerComponent* cc,
                         RigidBodyComponent* rb);

    /// 今ロックしている相手の位置。PlayerAimComponent が選んだ 1 体。
    [[nodiscard]] bool AimPointWorld(Vector3& outPoint);
    Vector3 GetMoveForward() const;
    Vector3 GetMoveRight(const Vector3& forward) const;

    /// 移動・回避の数値は必須 PlayerTuning からのみ読む。フォールバックを持たせないことで、
    /// fzdata の未設定を「たまたま動く」状態にしない。
    [[nodiscard]] float MoveSpeed()     const { return tuning->moveSpeed; }
    [[nodiscard]] float GroundAccel()   const { return tuning->groundAccel; }
    [[nodiscard]] float GroundDecel()   const { return tuning->groundDecel; }
    [[nodiscard]] float AirAccel()      const { return tuning->airAccel; }
    [[nodiscard]] float TurnSpeed()     const { return tuning->turnSpeed; }
    /// ProjectSettings の [physics] gravity が重力の正本。毎回読み直すので、
    /// 設定を変えたぶんがそのまま跳躍の導出へ効く。
    [[nodiscard]] float WorldGravity() const
    {
        const float gravity = physics.GetWorldGravity().Length();
        return gravity > EPSILON ? gravity : 9.81f;
    }
    /// 到達点 h を重力 g のもとで満たす初速。v0 = sqrt(2gh)。
    [[nodiscard]] float JumpSpeed() const
    {
        return std::sqrtf(2.0f * WorldGravity() * std::max(tuning->jumpApexHeight, 0.0f));
    }
    [[nodiscard]] float DodgeSpeed()    const { return tuning->dodgeSpeed; }
    [[nodiscard]] float DodgeDuration() const { return tuning->dodgeDuration; }
    [[nodiscard]] float DodgeCooldown() const { return tuning->dodgeCooldown; }
    [[nodiscard]] float DodgeEndSpeedRatio() const
    { return Clamp(tuning->dodgeEndSpeedRatio, 0.05f, 1.0f); }

    PlayerAimComponent*    m_aimOverride = nullptr;
    WeaponRigComponent*    m_weaponRig   = nullptr;
    PlayerBreathComponent* m_breath      = nullptr;

    /// 左右の足首のボーン。土煙の位置を決めるためだけに持つ。
    EntityRef m_footLeft;
    EntityRef m_footRight;

    bool m_hasSpineTargetBase = false;
    Vector3 m_spineTargetBase = Vector3::ZERO;
    Vector3 m_smoothedSpineOffset = Vector3::ZERO;
    /// 回避の残り時間と、次に回避できるまでの残り時間。
    float   m_dodgeRemaining = 0.0f;
    float   m_dodgeCooldown  = 0.0f;
    Vector3 m_dodgeDirection = Vector3::ZERO;
    int     m_dodgeSerial    = 0;
    /// この回避で擦れの煙をもう出したか。1 回の回避につき 1 発。
    bool    m_dodgeScuffed   = false;

    /// 被弾の押し。残り時間で細らせながら入力へ混ぜる。
    Vector3 m_knockDir     = Vector3::ZERO;
    float   m_knockSpeed   = 0.0f;
    float   m_knockSeconds = 0.0f;
    float   m_knockLeft    = 0.0f;
    /// 走行VFXの接地周期を決める距離 [m]。
    float   m_stepDistance   = 0.0f;
    bool m_jumpSoundPending = false;
    se::LoopVoice m_motor;
    float m_motorGain = 0.0f;
    float m_motorPitch = 0.85f;

    void StopMotor()
    {
        m_motor.Stop(*this);
        m_motorGain = 0.0f;
        m_motorPitch = 0.85f;
    }

    void TickMotor(float speed, bool active, float dt)
    {
        if (dt <= 0.0f) return;
        const float ratio = Clamp01(speed / Max(MoveSpeed(), 0.1f));
        const float target = active && speed > 0.1f ? Lerp(0.35f, 0.75f, ratio) : 0.0f;
        const float step = dt / (target > m_motorGain ? 0.08f : 0.30f);
        m_motorGain += std::clamp(target - m_motorGain, -step, step);
        const float pitchTarget = target > 0.0f ? Lerp(0.95f, 1.35f, ratio) : 0.85f;
        m_motorPitch += (pitchTarget - m_motorPitch) * (1.0f - std::exp(-dt / 0.10f));
        m_motor.Update(*this, se::kPlayerMotorLoop.First(), m_motorGain, m_motorPitch);
    }
    /// 前フレームの平面上の前方向と、サーボ音を鳴らしてからの間隔。
    Vector3 m_lastFacing       = Vector3::FORWARD;
    bool    m_hasLastFacing    = false;
    float   m_servoTurnCooldown = 0.0f;
    /// 符号付きの旋回速度 [deg/s]。正が右回り。上体の傾きが読む。
    float   m_yawRateDegrees   = 0.0f;
    /// 可変フレームで採取した入力を、次の固定ステップで一度だけ物理へ適用する。
    Vector3 m_moveDirection = Vector3::ZERO;
    /// スティックの倒し量 0..1。キーボードは押していれば常に 1。
    float   m_moveMagnitude = 0.0f;
    bool    m_hasMoveInput = false;
    /// 外から掛かる移動速度の倍率。要求は 1 フレーム有効で、OnUpdate が取り込んでから
    /// 中立へ戻す。物理ステップが 1 フレームに複数回走っても、その全部で同じ値になる。
    float   m_moveSpeedScale     = 1.0f;
    float   m_requestedMoveScale = 1.0f;
    /// 振っている間だけ «刃の向き» が体の向きになる。長さ 0 は «要求なし»。
    Vector3 m_facing          = Vector3::ZERO;
    Vector3 m_requestedFacing = Vector3::ZERO;
    /// 拘束中の «構え»。前傾と横倒しを度で持つ。要求が来なくなれば 0 へ戻る。
    float   m_leanPitch          = 0.0f;
    float   m_leanRoll           = 0.0f;
    float   m_requestedLeanPitch = 0.0f;
    float   m_requestedLeanRoll  = 0.0f;
    /// 座標を外から置かれている間。移動速度の倍率と同じ «押し続けている間だけ» の要求。
    bool    m_suspended          = false;
    bool    m_requestedSuspend   = false;
    /// 刀の入力まで止めているか。剣と弾きがここを読む。
    bool    m_inputLocked        = false;
    bool    m_requestedInputLock = false;
    /// 剛体を静的にしたか。掛けた側と戻す側を 1 つの旗で対にする。
    bool    m_suspendApplied     = false;
    bool    m_jumpRequested = false;
    bool    m_dodgeRequested = false;
    /// 預かっている回避入力の残り [秒]。0 より大きい間は «押されている» として扱う。
    float   m_dodgeBuffer    = 0.0f;
    /// 預かっている跳躍入力の残り [秒] と、足場を離れてからまだ跳べる残り [秒]。
    float   m_jumpBuffer     = 0.0f;
    float   m_coyote         = 0.0f;
    /// 回避を外から打ち切った。速度の後始末は固定ステップ側でしか書けないので預ける。
    bool    m_dodgeEndPending = false;

    /// ジャンプキーを押している間だけ上りの重力を軽いままにする。
    bool  m_jumpHeld = false;
    /// 前フレームの接地。着地の「瞬間」は状態そのものではなく変化でしか掴めない。
    bool  m_wasGrounded = true;
    /// 接地判定が立つ前に速度は 0 へ寄せられるため、着地の強さは
    /// 空中で見ていた最大の落下速度から測る。
    float m_peakFallSpeed = 0.0f;
    /// 走りの再生速度。足音の歩幅はここから出す (目と耳の歩調を揃える)。
    float m_runPlayback = 1.0f;
};

/// Reflect() をフィールド宣言から自動生成する (旧 .generated.hpp は廃止)。
FBZZ_REFLECT(PlayerControllerComponent)

inline void PlayerControllerComponent::OnStart()
{
    /// @note PlayerComponent から注入される共有調整値が無い場合は、ヘルパーの
    ///       tuning->参照を実行しない。単体配置の設定漏れを SEH へ進ませず、
    ///       スクリプトを安全に停止する。
    if (!tuning) {
        debug.LogError(
            "PlayerControllerComponent requires PlayerTuning .fzdata asset. "
            "Attach it through PlayerComponent before entering Play mode.");
        enabled = false;
        return;
    }

    /// @note 接触摩擦トルクによるカプセル傾きで水平ジッターが発生するため全軸フリーズ。
    physics.SetFreezeRotation(true, true, true);
    m_dodgeRemaining = 0.0f;
    m_dodgeCooldown  = 0.0f;
    m_dodgeBuffer    = 0.0f;
    m_jumpBuffer     = 0.0f;
    m_coyote         = 0.0f;
    m_dodgeEndPending = false;
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
    m_runPlayback    = 1.0f;
    m_stepDistance   = 0.0f;
    m_jumpSoundPending = false;
    m_motor.SetKey("PlayerMovement");
    m_motor.SetOutput("SE", 0.0f);
    StopMotor();
    /// @note 前フレームの向きは Play をまたいで持ち越さない。持ち越すと開始の 1 フレームで
    ///       «一気に回った» ことになり、立っているだけでサーボ音が鳴る。
    m_hasLastFacing     = false;
    m_servoTurnCooldown = 0.0f;
    m_yawRateDegrees    = 0.0f;
    m_facing            = Vector3::ZERO;
    m_requestedFacing   = Vector3::ZERO;
    m_suspended          = false;
    m_requestedSuspend   = false;
    m_inputLocked        = false;
    m_requestedInputLock = false;
    /// @note Play をまたぐと剛体は作り直されるので、静的の «掛けた» 記録も捨てる。
    m_suspendApplied     = false;

    /// @note 足音・回避音の出どころ。プレイヤー本人なので減衰を掛けずに 2D で鳴らす。
    se::EnsureSource(scene);

    if (auto* cc = scene.GetComponent<CharacterControllerComponent>())
        TuneGrounding(*cc);

    CacheFootBones();
}

inline void PlayerControllerComponent::OnUpdate()
{
    if (!enabled || !transform || !tuning) { StopMotor(); return; }

    /// @note 前フレームに来ていた要求を取り込み、要求側を中立へ戻す。掛ける側が押し続けて
    ///       いる間だけ鈍り、呼ばれなくなれば次のフレームで自然に元へ戻る。
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

    /// @note 転がりの尺を回避の尺へ合わせるのに要る値。Inspector で回避を詰めている最中に
    ///       見えていないと «直す先がある» ことに気付けない (dodgeClipSeconds 参照)。
    debugDodgeClipSpeed = dodgeClipSeconds / std::max(DodgeDuration(), 0.0001f);

    const Vector3 forward = GetMoveForward();
    const Vector3 right   = GetMoveRight(forward);

    /// @note WASD と左スティックの両方がここへ合流する。半径方向のデッドゾーンも
    ///       アクション層が済ませているので、斜めに倒したときの実効感度が方向で変わらない。
    ///       カメラ演出 (登場・撃破・とどめ) のあいだは入力を受けない。物理と重力は
    ///       そのまま進める ─ 止めると演出の最中に空中で固まる。
    const bool held = m_suspended || cutscene::HoldsPlayer(Time::unscaledTime);
    const Vector2 axis = held ? Vector2{ 0.0f, 0.0f } : input.GetMoveAxis();
    /// @note 正規化だけすると半分倒しても全力疾走になり、パッドでの歩き/走りの作り分けが
    ///       消えるため倒し量を残す (キーボードは常に 1.0 になるので挙動は変わらない)。
    const float amount = Min(axis.Length(), 1.0f);

    m_hasMoveInput  = amount > EPSILON;
    m_moveMagnitude = amount;
    m_moveDirection = m_hasMoveInput
        ? (right * axis.x + forward * axis.y).Normalized()
        : Vector3::ZERO;

    m_jumpRequested  = m_jumpRequested  || (!held && input.GetActionDown(actions::kJump));
    m_dodgeRequested = m_dodgeRequested || (!held && input.GetActionDown(actions::kDodge));
    /// @note 押しっぱなしかどうかは押した瞬間では分からない。上昇中の重力を決めるために
    ///       保持状態そのものを持つ。可変フレーム側で採り、固定ステップ側で使う。
    m_jumpHeld = !held && input.GetAction(actions::kJump);

    auto* cc = scene.GetComponent<CharacterControllerComponent>();
    auto* rb = scene.GetComponent<RigidBodyComponent>();
    auto* phy = rb && rb->enabled && rb->rigidBody ? rb->rigidBody.get() : nullptr;
    if (cc) {
        animator.SetFloat(paramVerticalSpeed, cc->verticalSpeed);
        animator.SetBool(paramIsGrounded, cc->isGrounded);
    }
    const Vector3 velocity = phy ? phy->GetVelocity() : Vector3::ZERO;
    /// @note 水平の速さ。歩行アニメーションの再生速度と、カメラをどれだけ緩めるかの
    ///       両方がこれで決まる。2 箇所で別々に測ると、走っている判定が食い違う。
    const float planarSpeed = IsDodging()
        ? DodgeSpeed()
        : std::sqrtf(velocity.x * velocity.x + velocity.z * velocity.z);
    animator.SetFloat(paramSpeed, planarSpeed);
    DriveMotionSpeeds(planarSpeed);
    UpdateIK();

    /// @note 手触りの出力は可変フレーム側で回す。固定ステップは 1 フレームに 0 回にも
    ///       複数回にもなるため、揺れや緩みの要求をそこへ置くと回数が絵に依存する。
    if (cc && phy) {
        if (m_jumpSoundPending) {
            m_jumpSoundPending = false;
            se::Play(audio, se::kPlayerJump);
        }
        RequestCameraSlack(*cc, planarSpeed);
        TickLanding(*cc, *phy);
        TickFootsteps(*cc, velocity, planarSpeed);
        TickServoTurn(planarSpeed, TimeManagerComponent::PlayerDeltaTime());
    }

    /// @note 重力と到達点から導かれる値を Inspector へ返す。ProjectSettings を触ったとき、
    ///       跳躍がどう変わったかをここで読めるようにしておく。
    const float gravity = WorldGravity();
    debugWorldGravity = gravity;
    debugJumpSpeed    = JumpSpeed();
    debugTimeToApex   = debugJumpSpeed / gravity;
    /// @note 音源の生成がコンポーネント配列を伸ばすため、cc/phyを使い終えてから呼ぶ。
    if (cc && phy) TickMotor(planarSpeed, cc->isGrounded && !held, TimeManagerComponent::PlayerDeltaTime());
    else StopMotor();
}

inline void PlayerControllerComponent::RequestCameraSlack(
    const CharacterControllerComponent& cc, float planarSpeed)
{
    auto* follow = CameraFollowManagerComponent::Instance();
    if (!follow) return;

    /// @note 二値で切り替えると歩き出した瞬間にカメラがどさっと置いていかれるため、
    ///       最高速で最大になる比例にする (加速がそのまま画面内のずれの増え方になる)。
    const float moveSlack = moveCameraSlack * Clamp01(planarSpeed / Max(MoveSpeed(), EPSILON));

    const float vertical = cc.isGrounded ? 0.0f : airCameraSlack;
    /// @note 回避と走行はどちらも横。強いほうがその軸を決める (合成規則は Loosen と同じ)。
    const float horizontal = Max(IsDodging() ? dodgeCameraSlack : 0.0f, moveSlack);
    if (vertical <= 0.0f && horizontal <= 0.0f) return;

    /// @note 「解除」を別に呼ぶ作りだと着地を取りこぼした 1 回でカメラが緩んだまま戻らなく
    ///       なるため、期限付きの要求を毎フレーム更新し呼ばなくなった時点を解除とする。
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

    /// @note 空中から接地へ変わったフレームだけ鳴らす。接地している間ずっと鳴らすと、
    ///       坂を降りるだけで揺れ続ける。着地の芝居は落ちた高さで速める (landFastSpeed 参照)。
    if (!m_wasGrounded && !landStateName.empty()) {
        const float weight = landFeedbackSpeed > 0.0f
            ? Clamp01(m_peakFallSpeed / landFeedbackSpeed) : 1.0f;
        animator.SetStateSpeed(landStateName, Lerp(std::max(landFastSpeed, 1.0f), 1.0f, weight));
    }

    if (!m_wasGrounded && landFeedbackSpeed > 0.0f) {
        const float strength = Clamp01(m_peakFallSpeed / landFeedbackSpeed);
        if (auto* feedback = ImpactFeedbackManagerComponent::Instance())
            feedback->Play(FeedbackEvent::PlayerLand, strength);
        /// @note 床が押し退けられた煙。揺れと音は «画面の外» の情報で、着地した «場所»
        ///       には今まで何も出ていなかった。走行の土煙は «足が後ろへ掻いた» 跡なので
        ///       1 方向へ吹くが、着地は全周へ押し退けるため向きを持たない Ground Dust を使う
        ///       (向きは吹く先の目安としてだけ渡す。跳んだ高さで煙の大きさが変わる)。
        if (runDust && strength > 0.05f)
            if (auto* vfx = VfxManagerComponent::Instance()) {
                Vector3 outward = phy.GetVelocity();
                outward.y = 0.0f;
                vfx->PlayGroundDust(transform.worldPosition, outward.NormalizedOr(
                                        transform.worldRotation * Vector3::FORWARD),
                                    strength, Lerp(0.35f, 0.7f, strength));
            }
        /// @note カメラを下へ沈める。膝が沈む向きと同じで、揺れより «重さ» が出る。
        if (landCameraDip > 0.0f && strength > 0.05f)
            if (auto* shake = CameraShakeManagerComponent::Instance())
                shake->Punch(Vector3{ 0.0f, -landCameraDip * strength, 0.0f }, 0.16f);
    }
    m_wasGrounded   = true;
    m_peakFallSpeed = 0.0f;
}

inline void PlayerControllerComponent::TickServoTurn(float planarSpeed, float dt)
{
    /// @note 平面上の前方向。真上を向いた姿勢では成分が消えるので、前フレームへ落とす
    ///       (Normalized() は長さ 0 で assert する)。
    const Vector3 facing  = transform.worldRotation * Vector3::FORWARD;
    const Vector3 forward = Vector3{ facing.x, 0.0f, facing.z }.NormalizedOr(m_lastFacing);

    if (!m_hasLastFacing || dt <= 0.0f) {
        m_lastFacing     = forward;
        m_hasLastFacing  = true;
        m_yawRateDegrees = 0.0f;
        return;
    }

    /// @note 符号付きの回転角。Cross の y 成分が回転の向きをそのまま持っている
    ///       (左手系 Y-up では FORWARD → RIGHT の回転で +y、つまり正が右回り)。
    const float cross   = Vector3::Cross(m_lastFacing, forward).y;
    const float dot     = std::clamp(Vector3::Dot(m_lastFacing, forward), -1.0f, 1.0f);
    m_yawRateDegrees    = ToDeg(std::atan2f(cross, dot)) / dt;
    const float yawRate = std::fabs(m_yawRateDegrees);
    m_lastFacing = forward;

    m_servoTurnCooldown = Max(0.0f, m_servoTurnCooldown - dt);

    /// @note 歩いている間は足音が «動いている» を担っている。回避中も専用の音がある。
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
    /// @note 空中と回避中は足が地面を蹴っていない。回避には専用の音があるので、
    ///       ここで足音まで鳴らすと 1 回の回避で 2 種類の音が重なる。
    if (!cc.isGrounded || IsDodging() || planarSpeed <= EPSILON) {
        /// @note 止まっている間に貯めた距離は捨てる。残すと、止まって歩き出した 1 歩目が
        ///       歩幅を待たずに鳴り、歩き始めだけリズムが崩れる。
        m_stepDistance = 0.0f;
        return;
    }

    const bool  running = planarSpeed >= runSpeedThreshold;
    /// @note 走りは «今の再生の歩調» から歩幅を出す (1 周 2 歩)。目に見える脚と耳の足音が揃う。
    const bool  cadence = running && runClipSpeed > 0.0f && runClipSeconds > 0.0f;
    const float stride  = cadence
        ? planarSpeed * runClipSeconds / (2.0f * std::max(m_runPlayback, 0.1f))
        : (running ? strideRun : strideWalk);
    if (stride <= 0.0f) {
        m_stepDistance = 0.0f;
        return;
    }

    /// @note 歩容に同期した駆動音にするため、再生間隔にランダムな揺らぎを加えない。
    const float threshold = stride;

    m_stepDistance += planarSpeed * TimeManagerComponent::PlayerDeltaTime();
    if (m_stepDistance < threshold) return;

    /// @note 剰余で戻す。引き切ると、1 フレームで歩幅を大きく超えたとき (低フレームレートや
    ///       回避の直後) に余りが積み残り、次の 1 歩が早まる。
    m_stepDistance = std::fmod(m_stepDistance, threshold);

    if (!running || !runDust) return;
    if (auto* vfx = VfxManagerComponent::Instance()) {
        /// @note 蹴り出しの強さは «最高速に対する今の速さ»。しきい値からの比で取ると、
        ///       Run At を下げただけで走り出しの 1 歩目が最大の土煙になる。
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

    /// @note 歩幅は距離で刻むためクリップの再生位相とは独立に進む。左右を交互に配ると
    ///       走行速度が変わった瞬間に左右が入れ替わり、宙に浮いている方の足から土煙が
    ///       立つため、着いている足は «低い方» で判定する。
    GameObject* planted = left;
    if (!planted || (right && right->transform.worldPosition.y < left->transform.worldPosition.y))
        planted = right;
    if (!planted) return transform.worldPosition;

    /// @note 足首のボーンは床から浮いている。左右と前後の位置だけ足から借り、
    ///       高さは接地しているプレイヤー原点に合わせる。
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
    const float dt = std::max(Time::fixedDeltaTime, 0.0f) * TimeManagerComponent::PlayerTimeScale();

    /// @note 座標を外から置かれている間 (登攀)。速度を毎ステップ 0 に落として重力も切る。
    ///       テレポート扱いの位置書き込みは PhysicsSystem が受け付けるが速度は誰も触らない
    ///       ため、切らずに置くと落下速度が登っている間ぶん積み上がり、操作を返した瞬間に
    ///       その速さで落ちてしまう。
    if (m_suspended) {
        /// @note 剛体は物理世界に残ったままだと脚や胴の当たりへ座標を置くたびにソルバーが
        ///       押し戻すため、静的にする (RigidBody::m_isStatic は積分も衝突解決も飛ばし
        ///       置いた座標がそのまま残る)。
        if (!m_suspendApplied) {
            m_suspendApplied = true;
            physics.SetStatic(true);
        }
        phy.SetVelocity(Vector3::ZERO);
        phy.SetAngularVelocity(Vector3::ZERO);
        physics.SetGravityScale(0.0f);
        /// @note 接地の «モード» は変えない (ForceGrounded は Automatic へ戻すまで latch する)。
        ///       Tick を回していない間はこの値がそのまま残るので、旗を直に立てれば足りる。
        cc->isGrounded  = true;
        cc->verticalSpeed = 0.0f;
        /// @note 向きだけは掛ける側 (登攀) が決められるよう残す。脚へ正対しないと、
        ///       横を向いたまま経路を滑り上がる絵になる。
        if (m_facing.LengthSq() > EPSILON) {
            Quaternion targetRotation =
                (Quaternion::LookRotation(m_facing) *
                 Quaternion::FromAxisAngle(Vector3::UP, ToRad(modelYawOffsetDegrees))).Normalized();

            /// @note 傾けは «世界の軸» で外から掛ける。向いている先へ倒す軸は Cross(UP, facing)
            /// @name 前後左右のどちらへ倒すかを、ヨーオフセットの有無に関わらず同じ式で書ける
            /// @note (BossCollapsePostureComponent の傾けと同じ規則)。
            if (std::abs(m_leanPitch) > EPSILON || std::abs(m_leanRoll) > EPSILON) {
                const Vector3 right = Vector3::Cross(Vector3::UP, m_facing).NormalizedOr(Vector3::ZERO);
                if (right.LengthSq() > EPSILON) {
                    targetRotation = (Quaternion::FromAxisAngle(right, ToRad(m_leanPitch)) *
                                      Quaternion::FromAxisAngle(Vector3::Cross(Vector3::UP, right),
                                                                ToRad(m_leanRoll)) *
                                      targetRotation).Normalized();
                }
            }

            /// @note 静的な剛体の姿勢は毎ステップ Transform から上書きされる
            ///       (PhysicsSystem::SyncRigidBodies が IsStatic() を無条件のテレポート
            ///       扱いにするため)。phy.SetRotation() を呼んでも古い Transform の値で
            ///       塗り直され向きも傾きも変わらないため、座標と同じく Transform が正本。
            ///       ワールド値も一緒に書くのは、この固定ステップが PrePhysics
            ///       (local → world の組み直し) より後に走り、ローカルだけでは同じフレームの
            ///       同期に «組み直す前の world» が渡ってしまうため。
            GameObject* self = scene.Self();
            const Quaternion current = self ? self->transform.worldRotation : phy.GetRotation();
            const Quaternion next = Quaternion::Slerp(
                current, targetRotation,
                1.0f - std::exp(-std::max(TurnSpeed(), 0.0f) * 3.0f * dt)).Normalized();
            if (self) {
                /// @note プレイヤーはシーンのルートなので local = world。
                self->transform.rotation      = next;
                self->transform.worldRotation = next;
            }
            phy.SetRotation(next);
        }
        return;
    }

    /// @note 拘束が解けた。静的と重力は必ず戻す ─ 戻し忘れると «立ったまま動かない
    ///       プレイヤー» になり、原因が操作にも物理にも見えない止まり方をする。
    if (m_suspendApplied) {
        m_suspendApplied = false;
        physics.SetStatic(false);
        physics.SetGravityScale(1.0f);
    }

    cc->Tick(&phy, dt);

    /// @note 跳躍も回避と同じく «押した事実» を預かる。0 秒にしても押したステップでは出るよう、
    ///       預ける長さの下限はほぼ 0 の正の値にする。
    if (m_jumpRequested) m_jumpBuffer = std::max(jumpBufferSeconds, 1.0e-4f);
    m_jumpRequested = false;
    const bool jumpRequested = m_jumpBuffer > 0.0f;
    m_jumpBuffer = std::max(0.0f, m_jumpBuffer - dt);
    m_coyote = cc->isGrounded ? std::max(coyoteSeconds, 0.0f)
                              : std::max(0.0f, m_coyote - dt);

    /// @note 押した瞬間を «預かり» へ移し替える。TickDodge が実際に出したときだけ空にする。
    if (m_dodgeRequested) m_dodgeBuffer = std::max(dodgeBufferSeconds, 0.0f);
    m_dodgeRequested = false;
    const bool dodgeRequested = m_dodgeBuffer > 0.0f;
    m_dodgeBuffer = std::max(0.0f, m_dodgeBuffer - dt);

    TickDodge(m_moveDirection, m_hasMoveInput, dodgeRequested, phy, dt);

    /// @note 回避中は開始方向を維持し、通常移動の旋回・速度制御を適用しない。
    ///       縦は回避が触らないので、重力だけはこの下で回避中も更新する。
    if (!IsDodging()) {
        /// @note 接地中だけ目標上向き速度を与える。質量差と現在の落下速度は
        ///       CharacterController が吸収するため、調整値を m/s に統一できる。
        if (jumpRequested && cc->enabled && (cc->isGrounded || m_coyote > 0.0f)) {
            /// @note 猶予も空にする。残すと跳んだ直後の空中でもう 1 回跳べる。
            m_jumpBuffer = 0.0f;
            m_coyote     = 0.0f;
            cc->JumpAtVelocity(&phy, std::max(JumpSpeed(), 0.0f));
            m_jumpSoundPending = true;
            animator.SetBool(paramIsGrounded, false);
            animator.SetTrigger(paramJumpTrigger);
            /// @note 踏み切りの煙。足が床を蹴った «場所» に出る唯一の印で、これが無いと
            ///       跳んだ瞬間が «浮き始めた» にしか見えない。走行の土煙を後ろへ吹かせる。
            if (runDust)
                if (auto* vfx = VfxManagerComponent::Instance()) {
                    Vector3 dir = phy.GetVelocity();
                    dir.y = 0.0f;
                    vfx->PlayRunDust(PlantedFootWorld(),
                                     dir.NormalizedOr(transform.worldRotation * Vector3::FORWARD),
                                     0.8f);
                }
        }

        /// @note 銃の頃は体を照準へ向け脚を 2D ブレンドで進行方向へ合わせていたが、両手剣は
        ///       ロコモーションが前進 3 本 (Idle/SwordWalk/Run_F) だけなので体を照準へ
        ///       固定すると «正面を向いたまま横へ滑る» になる。斬る向きは体でなく
        ///       BladeComponent::SwingDirection() が決めるため、抜刀中も進行方向を向き、
        ///       立ち止まったときだけ TickAimTurn が対象へ向ける (RequestFacing 参照)。
        ///       振っている間は通常の旋回より速く回す。斬るのは 0.2 秒ほどの出来事で、
        ///       いつもの追従では «振り終わってから向き終わる» ため振り出しで合わせる。
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

        /// @note 倒し量をそのまま速さへ掛ける。スティックを半分倒せば半分の速さで歩く。
        ///       MoveSpeed() 自体には掛けない。あちらは走りのブレンドとカメラの追従遅れを
        ///       正規化する基準にも使われ、基準ごと縮めると «歩いているのに走りモーション»
        ///       になる。
        Vector3 inputVelocity = m_hasMoveInput
            ? m_moveDirection * (MoveSpeed() * m_moveMagnitude * m_moveSpeedScale)
            : Vector3::ZERO;
        /// @note 入力が無いときの -1 は減速の合図。水平速度の出どころは移動入力だけなので、
        ///       押していないフレームは必ずブレーキが掛かる。
        float acceleration = m_hasMoveInput
            ? (cc->isGrounded ? GroundAccel() : AirAccel())
            : -1.0f;

        /// @note 被弾の押し。残り時間で細らせながら «入力の代わり» に置く (足すと押されている
        ///       間にスティックでほぼ打ち消せてしまい、完全に殺すと «操作を取り上げられた»
        ///       になるため、残り時間で比率を渡して混ぜる)。
        if (m_knockLeft > 0.0f) {
            m_knockLeft = std::max(0.0f, m_knockLeft - dt);
            const float ratio = m_knockSeconds > EPSILON ? m_knockLeft / m_knockSeconds : 0.0f;
            inputVelocity = m_knockDir * (m_knockSpeed * ratio) + inputVelocity * (1.0f - ratio);
            acceleration  = std::max(GroundAccel(), 1.0f);
        }
        cc->Move(&phy, inputVelocity, dt, acceleration, GroundDecel());
    }

    /// @note 上りか下りかはこの step で与えたジャンプ初速まで含めた速度で判定するため
    ///       最後に決める。踏み切りの前に決めると、跳んだ最初の 1 step だけ下り扱いの
    ///       重い重力が掛かり、到達点が調整値より低くなる。
    UpdateGravityScale(*cc, phy);
}

inline void PlayerControllerComponent::UpdateGravityScale(
    const CharacterControllerComponent& cc, const RigidBody& phy)
{
    /// @note 基準は 1.0 = ProjectSettings の重力そのまま。ここで持つのは状況ごとの比だけで、
    ///       重力の絶対値には触らない。触ると ProjectSettings が調整値でなくなる。
    ///       接地中も 1.0 のまま。下りの倍率を残すと、坂や段差でソルバーが毎ステップ
    ///       強い押し戻しを解くことになる。
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

    /// @note CharacterController のしきい値はすべて速度 (m/s) と秒で書かれているため、
    ///       ProjectSettings で重力を 3 倍にすると 1 物理ステップで積む速度も滞空時間も
    ///       3 倍/1/3 になり同じ数値が別の意味になる。とくに ledgeFallThreshold は
    ///       接地中の誤判定側へ倒れやすく、重力を上げた瞬間に接地が点滅するため
    ///       重力から決め直す。
    const float gravity  = WorldGravity();
    const float stepTime = std::max(Time::fixedDeltaTime, 1.0f / 240.0f);
    /// @note 1 物理ステップで重力が積む速度。速度系のしきい値はすべてこれの倍数で決まる。
    const float step = gravity * stepTime;
    cc.groundedVelSnap    =  std::max(step * 4.0f, 0.2f);
    cc.groundVelThreshold =  std::max(step * 4.0f, 0.2f);
    cc.fallVelThreshold   = -std::max(step * 6.0f, 0.3f);
    cc.ledgeFallThreshold = -std::max(step * 10.0f, 0.5f);

    /// @note 時間系は跳躍の長さから。最短の跳び (すぐキーを離した場合) の滞空より
    ///       確実に短くしないと、着地が受け付けられずに空中扱いのまま滑る。
    const float riseSeconds = JumpSpeed() / std::max(gravity, EPSILON);
    cc.jumpMinAirTime         = Clamp(riseSeconds * 0.25f, 0.02f, 0.2f);
    cc.jumpGroundIgnoreTime   = cc.jumpMinAirTime * 0.75f;
    cc.intentionalJumpMaxTime = std::max(riseSeconds * 2.5f, 0.3f);
}

inline void PlayerControllerComponent::TickAimTurn(RigidBody& phy, float dt)
{
    if (!turnToAim)
        return;
    /// @note 銃を収めている間は狙っていない。突っ立ったまま体だけ敵を追うのは不気味に見える。
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

    /// @note 立ち止まっているだけで体がぬるぬる追尾すると «吸い付いている» 絵になるため、
    ///       対象の方向へぴったりは向かせず、少し外れているぶんを残してはみ出した角度だけ回す。
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

    if (m_dodgeEndPending) {
        m_dodgeEndPending = false;
        FinishDodge(phy);
        return;
    }

    /// @note まだ回避していないなら、開始できるかを見る。
    if (m_dodgeRemaining <= 0.0f) {
        if (!dodgeRequested) return;
        if (m_dodgeCooldown > 0.0f) return;

        /// @note 入力方向へ跳ぶ。無入力なら «前» へ踏み込む (後退ではない)。回避クリップは
        ///       前方への踏み切り 1 本しか無く後ろへ運ぶと滑りが残ること、ボス 1 体との
        ///       近接戦では離れると詰め直しに時間を使うこと、ジャスト回避の報酬 (Flux) は
        ///       射程 2.6m の技なので下がると使う前に走って戻ることになるのが理由。
        ///       無敵は全区間にあるので、前進はそのまま «攻撃を潜り抜ける» になる。
        Vector3 direction = hasInput ? moveDirection : GetMoveForward();
        direction.y = 0.0f;
        if (direction.LengthSq() < EPSILON) return;

        /// @note 息を払う。切れているなら転がれない ─ 回避は «逃げ» の手で、ガードと同じ
        ///       息を食う (PlayerTuning の Breath)。息が戻るのは早くても 1 秒以上先で、
        ///       預かりは 0.15 秒しか持たないため、失敗したら預かりを捨てる
        ///       (残すと «押した覚えの無い回避» が出る余地だけが残る)。
        if (m_breath && !m_breath->SpendDodge()) {
            m_dodgeBuffer = 0.0f;
            return;
        }

        m_dodgeDirection = direction.Normalized();
        m_dodgeRemaining = DodgeDuration();
        /// @note 出した。預かりはここで空にする
        m_dodgeBuffer    = 0.0f;
        m_dodgeScuffed   = false;
        ++m_dodgeSerial;

        /// @note 体を回避方向へ向ける。入力があるときだけ回すのは、無入力の «前» はカメラ
        ///       前方で体は既にロック対象へ向いており (TickAimTurn)、ここで回すと Aim Turn
        ///       Deadzone ぶん最大 38 度体が跳ねるため。補間せず即入れるのは、0.24 秒しか
        ///       ない動きを普段の追従で回すと «跳び終わってから向き終わる» ため。
        if (hasInput) {
            phy.SetRotation(
                (Quaternion::LookRotation(m_dodgeDirection) *
                 Quaternion::FromAxisAngle(Vector3::UP, ToRad(modelYawOffsetDegrees)))
                    .Normalized());
        }
        /// @note クールダウンは回避の開始から数える。実質的な待ち時間は
        ///       DodgeCooldown - DodgeDuration になる。
        m_dodgeCooldown  = DodgeCooldown();
        animator.SetTrigger(paramDodgeTrigger);
        se::Play(audio, se::kPlayerDodge);

        /// @note 出だしを «弾けた» と読ませる。走りと回避の違いは速さだけなので、
        ///       蹴り出しの煙とカメラの引きが無いと «少し速く走った» にしか見えない。
        if (runDust)
            if (auto* vfx = VfxManagerComponent::Instance())
                vfx->PlayRunDust(PlantedFootWorld(), m_dodgeDirection, 1.0f);
        /// @note カメラを 1 往復ぶん後ろへ引く。画角が開いて、同じ速度でも速く見える。
        if (dodgeCameraKick > 0.0f)
            if (auto* shake = CameraShakeManagerComponent::Instance())
                shake->Punch(Vector3{ 0.0f, 0.0f, -dodgeCameraKick }, 0.18f);
        /// @note 開始フレームからそのまま下の速度上書きへ進む。
        ///       ここで return すると 1 フレームだけ慣性で滑り、出だしが鈍る。
    } else {
        m_dodgeRemaining = std::max(0.0f, m_dodgeRemaining - dt);
        if (m_dodgeRemaining <= 0.0f) {
            FinishDodge(phy);
            /// @note 今フレームで終了
            return;
        }
        /// @note 無敵を使い切った後は移動入力で転がりを抜けられる (dodgeMoveCancelAt 参照)。
        ///       FinishDodge が水平速度を走りの速さまで落とし向きも置き直すため、時間切れで
        ///       抜けたときと同じ状態から通常移動が続く (抜け方が 2 つでも後の 1 本は共通)。
        if (m_hasMoveInput && dodgeMoveCancelAt < 1.0f
            && DodgeProgress01() >= Clamp01(dodgeMoveCancelAt)) {
            /// @note 残りを先に捨てる。FinishDodge は後始末だけで «回避中» を降ろさないので、
            ///       ここで 0 にしないと次のフレームも IsDodging() が真のままになる。
            m_dodgeRemaining = 0.0f;
            FinishDodge(phy);
            /// @note 今フレームで終了
            return;
        }
    }

    /// @note 回避中は水平速度を毎フレーム上書きし続ける (開始時に一度だけ入れると、敵や壁に
    ///       接触した瞬間にソルバーが速度を削り「跳んだのに動かない」失速が起きるため)。
    ///       速さは出だしが最大で、後半から Dodge End Speed x へ落ちる。
    ///       直線で落とすと入力直後から減速が見えて踏み切りが弱くなるため、進行度を 2 乗し、
    ///       前半の速度を長く保ってから抜け際だけ着地速度へ繋ぐ。
    const float total    = std::max(DodgeDuration(), 0.0001f);
    const float progress = Clamp01(1.0f - m_dodgeRemaining / total);
    const float speed    = DodgeSpeed() * Lerp(
        1.0f, DodgeEndSpeedRatio(), progress * progress);

    /// @note 転がっている最中に床を擦る 1 発。踏み切りと踏ん張りの «あいだ» を埋める。
    ///       煙は体が通り過ぎた側へ流れるので、進行方向の逆へ吹かせる。
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

inline void PlayerControllerComponent::DriveMotionSpeeds(float planarSpeed)
{
    /// @note 走り: 釣り合う速さを越えたぶんだけ再生を速める。回避中の planarSpeed は
    ///       転がりの速さ (DodgeSpeed) なので触らない。
    if (!IsDodging()) {
        m_runPlayback = runClipSpeed > 0.0f
            ? std::clamp(planarSpeed / runClipSpeed, 1.0f, std::max(runPlaybackMax, 1.0f))
            : 1.0f;
        debugRunPlayback = m_runPlayback;
        if (!locomotionStateName.empty())
            animator.SetStateSpeed(locomotionStateName, m_runPlayback);
    }

    /// @note 転がり: 平均は «クリップの尺 ÷ 回避の尺» のまま、形だけ移動の速さへ沿わせる。
    ///       平均は尺から毎フレーム計算で出す (以前は .animcontroller の speed を手で合わせて
    ///       おり、回避の尺を触るたびに 2 つのファイルがずれていた)。
    if (dodgeStateName.empty()) return;
    const float base = dodgeClipSeconds / std::max(DodgeDuration(), 1.0e-4f);
    float shape = 1.0f;
    if (IsDodging()) {
        const float follow = Clamp01(dodgeAnimFollow);
        const float end    = DodgeEndSpeedRatio();
        const float motion = Lerp(1.0f, end, DodgeProgress01());
        /// @note ∫(1 − f(1 − m)) du = 1 − f(1 − end)/2 で割って、平均を 1 に保つ。
        shape = (1.0f - follow * (1.0f - motion))
              / std::max(1.0f - follow * (1.0f - end) * 0.5f, 0.05f);
    }
    animator.SetStateSpeed(dodgeStateName, base * shape);
}

inline bool PlayerControllerComponent::EndDodgeEarly()
{
    if (!IsDodging()) return false;
    /// @note 無敵と «回避中» はこの瞬間に落とす。速度の後始末は次の固定ステップ (TickDodge)。
    m_dodgeRemaining  = 0.0f;
    m_dodgeEndPending = true;
    return true;
}

inline void PlayerControllerComponent::FinishDodge(RigidBody& phy)
{
    /// @note 抜けた向きへ体を置き直す。回避は «向きを変えずに» 転がる (速度だけ上書きする)
    ///       ため、放置すると次の一手 (斬撃・弾き) が «後ろを向いたまま振る» ことになる。
    ///       Slerp せず直接置くのは、抜けるのは 1 フレームの出来事で補間を始めると
    ///       «転がり終わってから向き直る» 尾が残るため。
    if (m_dodgeDirection.LengthSq() > EPSILON) {
        phy.SetRotation((Quaternion::LookRotation(m_dodgeDirection) *
            Quaternion::FromAxisAngle(Vector3::UP, ToRad(modelYawOffsetDegrees))).Normalized());
    }

    /// @note 終わり際の踏ん張り。開始音だけだと、回避がいつ終わって
    ///       次の入力を受け付けるのかが耳から分からない。
    se::Play(audio, se::kPlayerDodgeEnd);
    /// @note 抜けた足元の «踏ん張り» の煙。出だしの煙だけだと、回避が «どこで
    ///       終わったか» が床に残らず、転がりの距離が絵から読めない。
    if (runDust)
        if (auto* vfx = VfxManagerComponent::Instance())
            vfx->PlayRunDust(PlantedFootWorld(), m_dodgeDirection, 0.6f);

    /// @note 抜けた «瞬間» に走りの速さまで落とす。通常移動の減速 (Ground Decel) に任せると、
    ///       回避速度 (走りの 2.8 倍) から落とし切るまで 100 m/s² でも 0.18 秒かかり、その
    ///       あいだ入力と無関係に滑り続けて速さで距離を作る設計と両立しない。向きは保つ。
    Vector3 vel = phy.GetVelocity();
    const float planar = std::sqrt(vel.x * vel.x + vel.z * vel.z);
    const float cap    = MoveSpeed();
    if (planar > cap && planar > EPSILON) {
        const float scale = cap / planar;
        vel.x *= scale;
        vel.z *= scale;
        phy.SetVelocity(vel);
    }
}

inline void PlayerControllerComponent::UpdateIK()
{
    auto* ik = scene.GetComponent<IKSolverComponent>();
    if (!ik) return;

    /// @note Use Foot IK は足チェーンだけを制御する。Solver 全体を切ると Spine と LookAt まで停止してしまう。
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
            /// @note 地面法線から移動方向の上り勾配 tan(theta) を求め、上り坂だけ上体を進行方向へ倒す。
            const float uphillGrade = std::max(
                0.0f, -Vector3::Dot(normal, moveDirection) / normal.y);
            constexpr float LEAN_PER_GRADE = 0.35f;
            constexpr float MAX_LEAN_OFFSET = 0.20f;
            const float lean = std::min(MAX_LEAN_OFFSET, uphillGrade * LEAN_PER_GRADE);
            const Vector3 localMoveDirection =
                (transform.worldRotation.Inverse() * moveDirection).Normalized();
            desiredOffset = localMoveDirection * lean;
        }
        /// @note 旋回の傾き。曲がっている側へ上体を倒す (turnLeanPerTurn 参照)。
        ///       回避中は転がりのクリップが体の向きを持っているので触らない。
        Vector3 planar = rb->rigidBody->GetVelocity();
        planar.y = 0.0f;
        const float speed = planar.Length();
        if (turnLeanPerTurn > 0.0f && speed > 0.5f && !IsDodging()) {
            /// @note 正の旋回速度は右回り。右回りなら右へ倒す = 進行方向の右手側。
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

    /// @note 接触法線は物理ステップごとに微動するため、指数応答でターゲットの揺れを抑える。
    constexpr float LEAN_RESPONSE = 8.0f;
    const float response = 1.0f - std::exp(-LEAN_RESPONSE * std::max(TimeManagerComponent::PlayerDeltaTime(), 0.0f));
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
