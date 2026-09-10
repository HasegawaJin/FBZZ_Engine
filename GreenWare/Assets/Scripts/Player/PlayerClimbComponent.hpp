/// @file    PlayerClimbComponent.hpp
/// @brief   倒れたボスの脚を登って甲板へ上がる。刀は登るあいだ背中へ納める
/// @author  Hasegawa Jin
/// @date    2026-09-08
///
/// WHY 転倒中しか登れないようにするか:
///   歩いているボスへ取り付くと «動く床の上でプレイヤーを追従させる» ことになり、
///   毎フレーム骨の世界姿勢を引いて座標を合わせ続ける必要が出る。転倒中に限れば
///   ボスはほぼ静止していて、この問題が丸ごと消える (Docs/climb-core.md)。
///
/// WHY 経路を骨から引くか (座標を直書きせず):
///   脚は 3 リンクで、転倒すると畳まれる。足・踵関節・膝の «今の» 世界位置を
///   毎回引けば、どんな姿勢で倒れていても経路がその形に沿う。
///   直書きした座標は «立っているボス» にしか合わない。
///
/// WHY 全身レイヤー (FullBody) を新しく持つか:
///   Player の既存レイヤーは Attack / UpperBody / Add_Hit の 3 枚とも M_UpperBody.mask で、
///   脚には 1 コマも届かない。登攀は «四肢で壁に掴まる» 全身の動作なので、上半身へ
///   流すと «直立で腕だけ動かしながら昇る» になる。Player_Base.mask (全身) の
///   レイヤーを Player.animcontroller へ 1 枚足し、そこへ流す。
///   レイヤーの被せ量は掛ける側が Slot のフェードから毎フレーム決める
///   (BladeComponent / WeaponRigComponent と同じ契約。DriveLayerWeight の WHY)。
///
/// WHY 納刀を挟むか:
///   両手が塞がったまま登る絵は «刀が壁を貫く» か «手が離れている» のどちらかにしかならない。
///   抜刀・納刀のイベント (SOCKET_BackSword_*) は既にあるので、挟むだけで済む。
#pragma once

#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Components/ColliderComponent.hpp>
#include <Engine/Scene/Components/IKSolverComponent.hpp>
#include <Engine/Scene/EntityRef.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
#include <Scripts/Combat/BossAiComponent.hpp>
#include <Scripts/Combat/IBoss.hpp>
#include <Scripts/Game/RumbleManagerComponent.hpp>
#include <Scripts/Player/PlayerComponent.hpp>
#include <Scripts/Utils/InputActions.hpp>
#include <Scripts/Utils/PlayerActionState.hpp>
#include <Scripts/Utils/SeLibrary.hpp>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

class PlayerClimbComponent : public Script {
    FBZZ_SCRIPT(PlayerClimbComponent)

public:
    FBZZ_GROUP("取り付き")
    FBZZ_FIELD_RANGE(float, mountRange, 3.6f, "取り付ける距離", 0.5f, 12.0f)
    FBZZ_TOOLTIP("脚の «足» からこの距離まで寄ると登れる。とどめ の 3.4m と揃えてある")
    FBZZ_FIELD(std::string, mountAction, actions::kClimb, "入力")
    FBZZ_TOOLTIP("取り付く入力。既定は専用の «登る» (E / パッド Y)。"
                 "ジャンプと兼ねると «跳ぼうとして登る» が起きる")

    // WHY «一定速度で経路を滑る» をやめたか (2026-09-10):
    //   距離で進めると、脚のどこに居ても同じ速さで上がる ── 手を掛ける音だけが
    //   別の時計 (クリップの尺) で鳴るので、画面の中に «掴んだ» 瞬間が 1 つも無い。
    //   関節を 1 つずつ «掴んで引き上げる» 単位で進めると、拍が体・音・揺れの
    //   3 つに同時に乗る。段が動いても «次はあの関節» が変わらないので、
    //   ボスが揺れているあいだも狙いが崩れない。
    FBZZ_GROUP("拍")
    FBZZ_FIELD_RANGE(float, stepSeconds, 0.34f, "1 関節あたり [s]", 0.08f, 2.0f)
    FBZZ_TOOLTIP("関節 1 つを掴んで引き上げるまで。経路は 4〜5 段なので、"
                 "0.34 で登り全体が 1.4 秒前後になる。上げるほど «重い» が、"
                 "0.5 を超えると倒れている 9 秒に対して長すぎる")
    FBZZ_FIELD_RANGE(float, stepSettle, 0.20f, "溜め", 0.0f, 0.6f)
    FBZZ_TOOLTIP("1 拍のうち «掴んで止まっている» 割合。0 で滑らかに繋がって拍が消え、"
                 "大きいほど 1 手ずつが立つ。ここがリズムの正体")
    FBZZ_FIELD_RANGE(float, stepBounce, 0.07f, "引き上げの伸び [m]", 0.0f, 0.5f)
    FBZZ_TOOLTIP("引く途中でどれだけ行き過ぎるか。体が «伸び上がって掴む» に見える。"
                 "大きいと弾んで登る")
    FBZZ_FIELD_RANGE(float, lengthInfluence, 0.35f, "段の長さを効かせる", 0.0f, 1.0f)
    FBZZ_TOOLTIP("0 で «どの段も同じ拍» (完全なリズム)、1 で «長い段ほど時間を掛ける» "
                 "(物理的な速さ一定)。既定は拍を主役にしつつ、甲板への長い一伸びだけ"
                 "少し粘らせる配分")
    FBZZ_FIELD_RANGE(float, stepReference, 1.10f, "基準の段の長さ [m]", 0.2f, 4.0f)
    FBZZ_TOOLTIP("«段の長さを効かせる» が見る基準。この長さの段がちょうど 1 拍になる")
    FBZZ_FIELD_RANGE(float, grabRumble, 0.16f, "掴む手応え", 0.0f, 1.0f)
    FBZZ_TOOLTIP("1 手ごとの振動。音と同じ拍で来ると «掴んだ» が手に返る。0 で無し")

    FBZZ_GROUP("速さ")
    FBZZ_FIELD_RANGE(float, sheatheSeconds, 0.76f, "納刀", 0.0f, 3.0f)
    FBZZ_TOOLTIP("納刀を頼んでから登り始めるまで。刀が背中へ «移る» のは左が f22 "
                 "(0.73s、WeaponSockets の kKatanaSheatheTransferL) なので、"
                 "そこを下回ると刀を握ったまま壁を掴み始める。クリップ全体 (40F/1.33s) は待たない")
    FBZZ_FIELD_RANGE(float, drawSeconds, 0.60f, "抜刀", 0.0f, 3.0f)
    FBZZ_TOOLTIP("Katana_Draw は 34F/1.13s、刀が手へ移るのは f18 (0.60s)")

    FBZZ_GROUP("甲板")
    FBZZ_FIELD(std::string, deckAnchorBone, "Body", "基準の骨")
    FBZZ_TOOLTIP("甲板の高さを測る基準。ボスの胴の骨から真上へ deckRise 上げた点に立たせる")
    FBZZ_FIELD_RANGE(float, deckRise, 1.33f, "甲板の高さ", 0.0f, 6.0f)
    FBZZ_TOOLTIP("胴の骨から甲板までの高さ [m]。Blender 実測で 5.83 − 4.50 = 1.33")
    FBZZ_FIELD_RANGE(float, deckSideOffset, 1.55f, "蓋からの距離", 0.0f, 4.0f)
    FBZZ_FIELD_RANGE(float, holdSeconds, 3.2f, "乗っている間の猶予 [s]", 0.0f, 10.0f)
    FBZZ_TOOLTIP("背に乗っているあいだ «あと何秒» 倒れたままにするか。降りるとここから数えて起き上がる。ボス側の上限 (9 秒) を超えては延ばせない")
    FBZZ_TOOLTIP("着地点を蓋の中心からどれだけ横へずらすか [m]。0 だとコアに埋まる")

    FBZZ_GROUP("当たり")
    // WHY 登っている間だけ当たりを外すか (2026-09-10):
    //   経路はボスの脚と胴の «中» を通る。そこへプレイヤーを置くと、体どうしが
    //   毎フレーム深く重なる。しかも拘束中のプレイヤーの剛体は静的 (＝無限の質量) に
    //   なるので、めり込みを解く力は全部 **ボスの側** へ行く ── 400kg の胴が
    //   毎ステップ弾き出されて «登り始めるとボスが吹き飛ぶ»。
    //   BossHitboxRigComponent が «自分の当たりで自分を押すな» と言っているのと
    //   同じ事故で、向きが «プレイヤー → ボス» になっただけ。
    //
    //   拘束中のプレイヤーは座標も向きも登攀が毎フレーム書いていて、重力も接地判定も
    //   止まっている。当たりが要る相手が 1 つも無いので、外してしまうのが素直。
    //   甲板に着いたら戻す ── そこからは HB_Deck (ボスの剛体へ属した足場) の上に
    //   «普通に立つ» ので、当たりが要る。
    FBZZ_FIELD(bool, detachCollider, true, "登っている間は当たりを外す")
    FBZZ_TOOLTIP("経路の上に居る間だけプレイヤーの当たりを切る。off にすると"
                 "めり込みを解く力がボスへ行き、登り始めた瞬間にボスが吹き飛ぶ")

    // WHY 手足を IK で «関節へ» 置くか (2026-09-10):
    //   経路の上を体だけ動かすと、腕も脚もクリップの通りに空を掻く。登っている絵の
    //   説得力は «掴んでいる所が動かないこと» で決まるので、掴む先を決めているのは
    //   経路そのもの ── 拍 1 つで 1 関節ぶん進むのだから、掴む所も関節でよい。
    //   四肢の目標を «脚の関節 ± 太さ» に置けば、脚が揺れても手足は脚に貼り付く。
    FBZZ_GROUP("掴む (IK)")
    FBZZ_FIELD(bool, gripIk, true, "手足を脚へ掴ませる")
    FBZZ_TOOLTIP("登っている間だけ、両手・両足を IK でボスの脚へ置く。"
                 "切るとクリップのまま空を掻く")
    FBZZ_FIELD_RANGE(float, gripRadius, 0.42f, "脚の太さ [m]", 0.05f, 2.0f)
    FBZZ_TOOLTIP("経路 (脚の芯) から手足を左右へ張り出す距離。ボスの脚の «掴める» 半径で、"
                 "PlayerBossBlockComponent の脚の半径と揃えてある")
    FBZZ_FIELD_RANGE(float, handLead, 0.25f, "手を掛ける先 (関節の先)", 0.0f, 1.0f)
    FBZZ_TOOLTIP("掴んだ関節からどれだけ先を握るか (段の長さに対する比)。"
                 "0 で関節ちょうど、大きいほど «上へ伸ばして» 掴む")
    FBZZ_FIELD_RANGE(float, footTrail, 0.30f, "足を置く所 (関節の手前)", 0.0f, 1.0f)
    FBZZ_TOOLTIP("体が離れる関節からどれだけ手前に足を残すか。0 で関節ちょうど")
    FBZZ_FIELD_RANGE(float, gripWeight, 1.0f, "IK Weight", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, gripBlend, 0.18f, "効き始めるまで [s]", 0.01f, 1.0f)
    FBZZ_TOOLTIP("登り始めと降りたときに IK を出し入れする時間。0 に近いと手足が飛ぶ")
    FBZZ_FIELD(bool, drawGrip, false, "掴む所を描く")

    FBZZ_GROUP("姿勢")
    // WHY 角度を «経路から» 出すか: 登る絵の説得力は、脚の斜面と体の傾きが揃っているかで
    //     決まる。斜度を数値で置くと、脚が畳まれて経路が立った瞬間に体だけ直立して
    //     «壁を素通りして昇っている» になる。今いる段の接線から毎フレーム引けば、
    //     どんな倒れ方をしていても体が斜面へ貼り付く。
    FBZZ_FIELD_RANGE(float, leanScale, 0.42f, "前傾の比", 0.0f, 1.5f)
    FBZZ_TOOLTIP("経路の «立ち上がり角» をどれだけ体の前傾へ写すか。"
                 "1.0 で斜面と平行 ── 人の体は脚で支えるぶん寝かせすぎない")
    FBZZ_FIELD_RANGE(float, leanMax, 34.0f, "前傾の上限 [度]", 0.0f, 80.0f)
    FBZZ_TOOLTIP("垂直な段でもここまでしか倒さない。超えると頭から壁へ刺さって見える")
    FBZZ_FIELD_RANGE(float, swayDegrees, 6.0f, "左右の揺れ [度]", 0.0f, 30.0f)
    FBZZ_TOOLTIP("1 手ごとに掴んだ側へ体を振る量。引いている間だけ振れて、掴んで"
                 "止まっている間に戻る ── 揺れそのものが拍を数える。"
                 "0 で «真っ直ぐ昇るエレベーター»")
    FBZZ_FIELD_RANGE(float, leanSmooth, 9.0f, "追従の速さ", 0.5f, 30.0f)
    FBZZ_TOOLTIP("傾きが目標へ寄る速さ [1/s]。低いほど段の切り替わりで体が遅れて付いてくる")
    FBZZ_FIELD_RANGE(float, faceTangentMin, 0.12f, "接線で向きを決める下限", 0.0f, 1.0f)
    FBZZ_TOOLTIP("接線の水平成分がこれを下回ったら «脚の方を向く» へ切り替える。"
                 "垂直な段では接線の水平成分がほぼ 0 で、そのまま使うと体が振り回される")

    FBZZ_GROUP("クリップ")
    // WHY 上半身レイヤー (Attack) ではないか (2026-09-10): Player の Attack / UpperBody /
    //     Add_Hit はどれも M_UpperBody.mask で、脚には 1 コマも届かない。登攀は全身の
    //     動作なので、Player_Base.mask (全身) の «FullBody» レイヤーへ流す。
    //     レイヤーは Assets/Animation/Player/Player.animcontroller が正本。
    FBZZ_FIELD(std::string, layerName, "FullBody", "Layer")
    FBZZ_TOOLTIP("登攀・納刀・抜刀を流す全身レイヤー。上半身マスクのレイヤーを指すと"
                 "脚がロコモーションのまま «立ったまま昇る» になる")
    // WHY .fbx でも «guid: 付きの Library パス» でもなく «パッケージの論理パス» か
    //     (2026-09-10): クリップの実体は .anim で、FBX には入っていない
    //     (「FBX インポート時のクリップは .anim に分離されており .fzasset には
    //     clips が含まれない」)。.fbx を指すと LoadClips は «clips が 0 本のモデル» を
    //     読んで終わり、Slot は無音で畳まれる ── **これが «モーションが出ない» の正体**。
    //     `guid:` を前に付けると guid が勝ってパスのヒントは捨てられるので、
    //     FBX の guid を書いた瞬間また FBX へ戻る。
    //     `Assets/<dir>/<Name>/anims/<Clip>.anim` の形なら AssetManager が
    //     «隣の <Name>.fbx の guid» から Library/Baked/<guid>/anims/ を引く
    //     (AssetManager::LibraryBakedPath)。Inspector で .fbx を選び直さないこと。
    FBZZ_FIELD_FILE(climbClipFile,
        "Assets/Models/Player/Climb/anims/Climb.anim",
        "Climb Clip", ".anim,.fbx")
    FBZZ_FIELD(std::string, climbClipName, "Climb", "Climb Clip Name")
    // WHY マスクを実行時に外すか (2026-09-10): 流し先のレイヤーが上半身マスクだと
    //     脚が 1 コマも動かない。レイヤーの指定はシーンに保存されていて、
    //     コード側の既定値を上書きし続ける («Attack» のまま残っていた) ので、
    //     «全身で鳴らす» を鳴らす側が握る。掛けている間だけ外し、終わったら戻す。
    FBZZ_FIELD(bool, forceFullBody, true, "鳴らす間はマスクを外す")
    FBZZ_TOOLTIP("登攀の間だけレイヤーのマスクを解除して全身へ効かせる。"
                 "off にすると、上半身マスクのレイヤーでは «立ったまま腕だけ動く» になる")
    // 納刀 / 抜刀のクリップはここには無い。WeaponRigComponent が «刀の付け替え» と
    // 一緒に上半身レイヤーへ流す ── クリップだけこちらから流すと、芝居はするのに
    // 刀は手に握られたまま、という食い違いが生まれる (PlayerComponent の
    // RequestSheatheWeapons の WHY)。

    FBZZ_GROUP("デバッグ")
    FBZZ_GROUP("音")
    FBZZ_FIELD_RANGE(float, climbVolume, 0.95f, "登攀の音量", 0.0f, 2.0f)
    FBZZ_FIELD_RANGE(float, grabInterval, 0.4165f, "クリップの 1 掴み [s]", 0.05f, 2.0f)
    FBZZ_TOOLTIP("Climb クリップ (25F/0.83s) の半分 ── 左右 1 回ずつ手が掛かるので、"
                 "«クリップ側の 1 手» の尺。拍 (1 関節あたり) と割り算して再生速度を出すので、"
                 "クリップを差し替えたらここを合わせる。鳴らす間隔ではない")

    FBZZ_GROUP("デバッグ")
    // WHY «転倒中だけ» へ戻したか (2026-09-10): 窓が短いからと «いつでも» にしていたが、
    //     経路は脚の骨から引いた 4 点の固定経路で、歩いているボスでは 1 歩ごとに
    //     踊り場が数 m 動く。進捗が飛ぶか、経路の外へ置き去りになるかのどちらかで、
    //     «映っている経路に沿って登る» が成立しない。窓の短さは秒数の側で直す
    //     (転倒 9 秒 ＋ 乗っている間の延長)。
    FBZZ_FIELD(bool, requireToppled, true, "転倒中だけ登れる")
    FBZZ_TOOLTIP("on にすると倒れているボスにしか取り付けない。off なら歩いていても飛びつく")
    FBZZ_FIELD(bool, drawPath, true, "経路を描く")
    FBZZ_FIELD_READ_ONLY(std::string, debugPhase, "None", "状態")
    FBZZ_FIELD_READ_ONLY(float, debugProgress, 0.0f, "進捗 [m]")
    FBZZ_FIELD_READ_ONLY(int, debugStep, 0, "今の段")
    // «モーションが出ない» の切り分け。0 のままなら鳴っていない (クリップが
    // 解決できていないか、そもそも Slot を差していない)。上がっているのに画面が
    // 変わらないなら、レイヤーのマスクか被せ量の側。
    FBZZ_FIELD_READ_ONLY(float, debugSlot, 0.0f, "Slot の被せ量")
    FBZZ_FIELD_READ_ONLY(float, debugLean, 0.0f, "前傾 [度]")
    FBZZ_FIELD_READ_ONLY(std::string, debugLeg, "", "登っている脚")

    /// 登っている / 甲板に居る。移動と攻撃を止める側が読む。
    [[nodiscard]] bool IsClimbing() const { return m_phase != Phase::None && m_phase != Phase::Deck; }
    [[nodiscard]] bool IsOnDeck()   const { return m_phase == Phase::Deck; }
    /// 甲板から降ろす。ボスが起き上がったとき・撃破されたときに呼ぶ。
    void Dismount();

    /// 今このフレーム «登る» が通るか。画面の案内 (ClimbPromptComponent) が読む。
    ///
    /// WHY 案内の側で条件を組み直させないか: 通る条件は «転倒中か» と «脚のそばか» の
    ///     2 つで、どちらもここが既に持っている。案内が自前で測ると、間合いを
    ///     変えたときに «出ているのに登れない» が生まれる。押せるかどうかを
    ///     決めている場所が、そのまま «出すかどうか» を答える。
    [[nodiscard]] bool CanMount();

    void OnStart() override;
    void OnUpdate() override;
    void OnDestroy() override;
    void OnDrawGizmos() override;

private:
    /// 今このボスに取り付けるか。既定では転倒中だけ。
    [[nodiscard]] bool Climbable(const IBoss* boss) const;
    /// 甲板に立っている間、ボスが動いたぶんだけプレイヤーを運ぶ。
    void CarryWithBoss();
    /// プレイヤーをこのワールド座標へ置く。
    ///
    /// WHY worldPosition だけでは «全く動かない» か (2026-09-10): スクリプトの次に走る
    ///     TransformSystem (PrePhysics) は、ルートから階層を辿って **ローカル値から
    ///     ワールドを組み直す**。ワールドだけ書いても、物理へ渡る前にその 1 行が
    ///     捨てられる ── 経路も進捗も正しいのに絵が 1mm も動かない、という
    ///     どこを見ても原因の出ない止まり方になる。ローカルまで書いて初めて残る。
    ///     プレイヤーはシーンのルートなので local = world (盤面の約束)。
    void Place(const Vector3& world);
    /// 重力と移動を止め、この向きへ体を向けるよう毎フレーム要求する。
    /// 呼ばなくなれば次のフレームで自然に元へ戻る。
    ///
    /// `holdInput` が true の間は刀の入力ごと止める。
    /// 経路の上に居る間だけ掛かり、甲板に着いたら呼ばなくなる (物理へ返す)。
    void HoldPlayer(const Vector3& facing, bool holdInput);
    /// レイヤーの被せ量を Slot のフェードへ追従させる。
    ///
    /// WHY 要るか: Override レイヤーの被せ量は layer.weight × ボーンの mask weight で、
    ///     Slot 自身のフェードはそこへ掛からない (AnimatorSystem)。weight を 0 のまま
    ///     Slot を鳴らしても **1 コマも出ない** ── クリップも尺も正しいのに
    ///     «モーションが切り替わらない» に見えるのはここ。1 に固定すれば出るが、
    ///     今度は立ち上がりと戻りが両方 1 フレームで飛ぶ (BladeComponent と同じ形)。
    void DriveLayerWeight();
    /// 登っている間、クリップが鳴り続けていることを毎フレーム確かめ直す。
    ///
    /// WHY 1 度 PlaySlot して終わりにできないか (2026-09-10):
    ///   Slot はレイヤーが持つ状態で、`ApplyAnimatorControllerAsset` が
    ///   `animator.layers = asset.layers` でレイヤーごと差し替えると **消える**
    ///   (引き継がれるのは同名レイヤーの runtime だけで、slot も weight も mask も
    ///   アセットの値へ戻る)。これはアセットの世代が変わるたび ── エディターが
    ///   何かを保存し直すたび ── に起きるので、「さっきまで出ていたモーションが
    ///   途中で消える」という形の壊れ方になる。
    ///   クリップが未ロードのまま鳴らした 1 回目も `UpdateLayerSlot` が
    ///   `slot.active = false` にして畳む。どちらも «鳴っていなければ鳴らし直す»
    ///   で回復する ── 登っている間はこのレイヤーの持ち主はこちらしか居ない。
    void KeepClimbClip();
    /// 登攀で鳴らす 3 本を Animator の «読み込む対象» へ宣言する。
    ///
    /// WHY 要るか (2026-09-10): `AnimatorSystem::LoadClips` が読むのは
    ///     **ステートから辿れるクリップだけ**。Climb はどのステートからも参照されて
    ///     いないので Animator の clips に載らず、Slot は «参照が解決できない» として
    ///     鳴った瞬間に畳まれる (`UpdateLayerSlot` が slot.active を false にする)。
    ///     警告もログも出ないので «クリップも尺もレイヤーも正しいのに無音» になる。
    ///     `externalClipSources` がそのための宣言先で、SequenceSystem も同じ手を使う。
    void RegisterClips();
    /// プレイヤーの当たりを入れる / 外す。外している間は物理に居ないのと同じ。
    void SetBodyCollision(bool on);
    /// 経路の接線から «向き・前傾・横倒し» を決めて溜める。sway は掴んだ手の側 [度]。
    void DrivePosture(const Vector3& tangent, float dt, bool onPath, float sway);
    /// 段 (関節から関節) 1 つぶんの拍の長さ [s]。
    [[nodiscard]] float StepDuration(std::size_t step) const;
    /// 今の経路で 1 掴みに掛かる平均の拍。クリップの再生速度を合わせるのに使う。
    [[nodiscard]] float AverageStep() const;
    /// 次の関節へ手を掛けた。音・振動・手の左右・掴み替えをここで 1 つにまとめる。
    void OnGrab();

    /// 四肢 (右手 / 左手 / 右足 / 左足)。掴む所と IK チェーンをこの順で持つ。
    enum Limb { HandR = 0, HandL, FootR, FootL, LimbCount };
    /// 四肢の IK 的を作る。DLL リロードで作り直されても名前で拾い直す。
    void EnsureGripTargets();
    /// 四肢 1 本の TwoBone チェーンを引く (無ければ足す)。
    [[nodiscard]] IKChain* EnsureGripChain(int limb);
    /// 掴む所を毎フレーム置き直し、IK の効きを出し入れする。
    void DriveGrip(float dt, const Vector3& facing);
    /// IK を手放す。降りたら必ず通す ── 残すと «歩いていても手が空を掴む»。
    void ReleaseGrip();
    /// 経路の «関節 p 番目» の点。p は小数で、関節と関節の間を指せる。
    [[nodiscard]] Vector3 PointAtJoint(float joint) const;
    /// 四肢の IK チェーンの order 起点。頭 (Look-At) より後に解いて、腕を勝たせる。
    static constexpr int kGripOrder = 900;
    /// 経路の上での体の向き。接線が立ちすぎている段では脚の方を向く。
    [[nodiscard]] Vector3 FacingFor(const Vector3& tangent) const;
    /// プレイヤーからコアへの向き。コアが見つからなければ長さ 0。
    [[nodiscard]] Vector3 CoreFacing() const;
    /// 段 (m_path[step] → m_path[step + 1]) の向き。体の向きと前傾をここから決める。
    [[nodiscard]] Vector3 StepDirection(std::size_t step) const;
    enum class Phase { None, Sheathe, Climb, Draw, Deck };

    /// 一番近い脚の «足» を探す。見つからなければ false。
    [[nodiscard]] bool FindNearestLeg(std::string& outSuffix, Vector3& outFoot);
    /// 足の骨 4 本を掴み直す。
    ///
    /// WHY 覚えるか (2026-09-10): 名前引きはボスの全サブツリー (100 ノード超) を
    ///     再帰で歩く。取り付きの判定だけなら押した瞬間の 4 回で済んでいたが、
    ///     画面の案内が «今登れるか» を毎フレーム聞くようになったので、
    ///     覚えずにいると毎フレーム 4 回歩くことになる。
    ///     骨の «並び» は転倒しても変わらないので、覚えたまま位置だけ読めばいい。
    void RefreshFeet();
    /// 足から親をたどって胴の手前まで、最後に甲板の着地点。今の姿勢から引き直す。
    void BuildPath(const std::string& suffix);
    /// 踊り場として数える最小の間隔 [m]。これより近い骨は同じ段として畳む。
    static constexpr float       kMinStep   = 0.25f;
    /// たどる関節の上限。リグを組み替えて環ができても «永遠に登る» にしない。
    static constexpr std::size_t kMaxJoints = 12;
    /// 経路の総距離 [m]。進捗の表示にだけ使う (進行そのものは段の «拍» で決まる)。
    [[nodiscard]] float   PathLength() const;

    [[nodiscard]] GameObject* Boss() const;
    [[nodiscard]] static GameObject* FindInSubtree(GameObject& root, const std::string& name);

    Phase                m_phase   = Phase::None;
    float                m_timer   = 0.0f;
    /// 登り切るまでの割合 [0,1]。段の番号と拍から出す表示用の値で、
    /// 進行そのものを決めているのは m_step と m_stepTime。
    float                m_travel  = 0.0f;
    /// 今つかみに行っている段 (m_path[m_step] → m_path[m_step + 1])。
    std::size_t m_step     = 0;
    /// その段に入ってからの秒数。
    float       m_stepTime = 0.0f;
    /// 今どちらの手で掴んでいるか。体を振る側と、次に振る側を決める。
    bool        m_rightHand = true;
    /// 今フレーム体へ掛けている傾き [度]。目標へ指数で寄せる。
    float m_leanPitch  = 0.0f;
    float m_leanRoll   = 0.0f;
    /// «レイヤーが無い» «クリップが鳴らない» を 1 度だけ言うための札。
    bool  m_warnedLayer = false;
    bool  m_warnedClip  = false;
    /// 何フレーム続けて «鳴らしたのに畳まれた» か。1 回きりは回復、続くなら参照の問題。
    int   m_clipRetries = 0;
    /// 今プレイヤーの当たりを外しているか。掛けた側と戻す側を 1 つの旗で対にする。
    bool  m_colliderOff = false;
    /// マスクを外している間、元のマスクを預かる。
    std::string m_savedMask;
    bool        m_maskForced = false;
    /// 四肢が掴んでいる «関節の番号» (小数)。経路は毎フレーム引き直すので、
    /// 世界座標ではなく «経路上のどこか» で覚える ── 脚が揺れても掴んだ所は動かない。
    float     m_gripJoint[LimbCount] = { 1.0f, 0.6f, 0.0f, 0.0f };
    EntityRef m_gripTarget[LimbCount];
    /// IK の効き [0,1]。掴み始めと放しで出し入れする。
    float     m_gripFade  = 0.0f;
    bool      m_gripBuilt = false;
    /// 前フレームの «甲板の基準骨» の姿勢。差分を渡すために持つ。
    Vector3    m_carryPos;
    Quaternion m_carryRot;
    bool       m_carryValid = false;
    /// 取り付いた直後の «離す» 誤爆よけ [秒]。
    float      m_mountGrace = 0.0f;
    std::string          m_suffix;
    std::vector<Vector3> m_path;
    EntityRef            m_boss;
    /// 覚えている足の骨 (_FR / _FL / _BR / _BL の順)。
    EntityRef            m_feet[4];
    /// 掴み直すまでの残り [秒]。ボスが入れ替わっても数百 ms で追いつく。
    float                m_footProbe = 0.0f;
    static constexpr float kFootProbeInterval = 0.5f;
};

FBZZ_REFLECT(PlayerClimbComponent)

inline GameObject* PlayerClimbComponent::Boss() const
{
    if (GameObject* cached = m_boss.Resolve(scene)) return cached;
    return FindBossOnBoard(scene);
}

inline GameObject* PlayerClimbComponent::FindInSubtree(GameObject& root, const std::string& name)
{
    if (root.name == name) return &root;
    const int count = root.GetChildCount();
    for (int i = 0; i < count; ++i)
        if (GameObject* child = root.GetChild(i))
            if (GameObject* found = FindInSubtree(*child, name)) return found;
    return nullptr;
}

inline void PlayerClimbComponent::RefreshFeet()
{
    for (EntityRef& foot : m_feet) foot = {};
    GameObject* boss = Boss();
    if (!boss) return;

    static constexpr const char* kSuffix[4] = { "_FR", "_FL", "_BR", "_BL" };
    for (int i = 0; i < 4; ++i)
        if (GameObject* foot = FindInSubtree(*boss, std::string("Foot") + kSuffix[i]))
            m_feet[i] = EntityRef{ foot->GetID() };
}

inline bool PlayerClimbComponent::FindNearestLeg(std::string& outSuffix, Vector3& outFoot)
{
    if (!Boss()) return false;

    // 骨の探索は高いが、位置を読むのは安い。掴み直しは間隔を空ける。
    m_footProbe -= std::max(Time::deltaTime, 0.0f);
    bool missing = false;
    for (const EntityRef& foot : m_feet)
        if (!foot.Resolve(scene)) { missing = true; break; }
    if (missing || m_footProbe <= 0.0f) {
        m_footProbe = kFootProbeInterval;
        RefreshFeet();
    }

    static constexpr const char* kSuffix[4] = { "_FR", "_FL", "_BR", "_BL" };
    const Vector3 origin = transform.worldPosition;

    bool  found = false;
    float best  = 0.0f;
    for (int i = 0; i < 4; ++i) {
        GameObject* foot = m_feet[i].Resolve(scene);
        if (!foot) continue;
        Vector3 delta = foot->transform.worldPosition - origin;
        delta.y = 0.0f;
        const float distance = delta.Length();
        if (!found || distance < best) {
            found = true; best = distance;
            outSuffix = kSuffix[i]; outFoot = foot->transform.worldPosition;
        }
    }
    return found && best <= std::max(mountRange, 0.1f);
}

inline bool PlayerClimbComponent::CanMount()
{
    // 既に取り付いている間は案内を出さない。«押せる» のは離すときで、意味が違う。
    if (m_phase != Phase::None) return false;
    if (!Climbable(IBoss::Of(Boss()))) return false;
    std::string suffix; Vector3 foot;
    return FindNearestLeg(suffix, foot);
}

inline void PlayerClimbComponent::BuildPath(const std::string& suffix)
{
    m_path.clear();
    GameObject* boss = Boss();
    if (!boss) return;

    // 足を起点に «親» を順にたどる。骨の繋がりそのものが経路になる。
    //
    // WHY 名前を並べないか (2026-09-10): 以前は Foot / Hock / Thigh の 3 つを名指しで
    //     拾っていて、**Shin を飛ばしていた** (リグは Thigh → Shin → Hock → Foot の
    //     4 本)。脛の関節を跳ばすと、脚が畳まれるほど膝で角を切って、当たりの
    //     カプセル (Thigh→Shin / Shin→Hock / Hock→Foot の 3 本) から経路が浮く
    //     ── «映っている脚に沿って登る» が崩れるのはここ。
    //     親をたどれば、間の骨も、腿の付け根のソケット (Yaw_*) も自然に入る。
    //
    // WHY 体の骨 (deckAnchorBone) を経路へ入れないか: 胴の «中心» なので、
    //     踊り場にすると甲羅の内側を通ってから甲板へ出ることになる。
    //     手前で止めて、最後の関節から着地点へ直接つなぐ。
    GameObject* foot = FindInSubtree(*boss, std::string("Foot") + suffix);
    for (GameObject* node = foot; node && node != boss; node = node->GetParent()) {
        // 胴に着いたら脚は終わり。ここから先は甲板の話になる。
        //
        // Root / RootNode も終端に含める。腿の付け根が胴ではなく骨格のルートに
        // 繋がっているリグだと、そこを抜けて **モデル原点** を踊り場にしてしまう
        // (骨より上のノードは姿勢が恒等で原点に居る)。抜けた瞬間に経路が
        // 足元へ落ちるので、絵で気付くのが難しい壊れ方になる。
        if (node->name == deckAnchorBone || node->name == "Root" || node->name == "RootNode")
            break;

        const Vector3 at = node->transform.worldPosition;
        // 長さ 0 の骨は踊り場にならない。段が «その場で 1 拍» になり、手を掛ける音だけが
        // 増えて拍が崩れる。
        if (!m_path.empty() && (m_path.back() - at).LengthSq() < kMinStep * kMinStep) continue;
        m_path.push_back(at);
        // 骨の環はありえないが、リグを組み替えたときに «永遠に登る» で気付くのは辛い。
        if (m_path.size() >= kMaxJoints) break;
    }
    // 並べ替えは要らない。足から親へ上がる順が、そのまま登る順になっている。

    // 甲板の着地点。胴の骨から真上へ上げ、蓋の中心から横へずらす。
    if (GameObject* body = FindInSubtree(*boss, deckAnchorBone)) {
        const Vector3 up   = boss->transform.up;
        Vector3       side = boss->transform.right;
        // 登ってきた脚の側へ降ろす。反対側へ出すと甲板を横断してから戦うことになる。
        if (!m_path.empty()) {
            Vector3 lean = m_path.front() - body->transform.worldPosition;
            lean.y = 0.0f;
            if (lean.LengthSq() > EPSILON) side = lean.Normalized();
        }
        const Vector3 deck = body->transform.worldPosition
                           + up * std::max(deckRise, 0.0f)
                           + side * std::max(deckSideOffset, 0.0f);
        m_path.push_back(deck);
    }
}

inline float PlayerClimbComponent::PathLength() const
{
    float total = 0.0f;
    for (std::size_t i = 1; i < m_path.size(); ++i)
        total += (m_path[i] - m_path[i - 1]).Length();
    return total;
}

inline Vector3 PlayerClimbComponent::StepDirection(std::size_t step) const
{
    if (step + 1 >= m_path.size()) return Vector3::ZERO;
    return (m_path[step + 1] - m_path[step]).NormalizedOr(Vector3::ZERO);
}

inline Vector3 PlayerClimbComponent::CoreFacing() const
{
    GameObject* boss = Boss();
    if (!boss) return Vector3::ZERO;
    GameObject* core = FindInSubtree(*boss, "HB_Core");
    if (!core) core = FindInSubtree(*boss, "Core");
    if (!core) return Vector3::ZERO;

    Vector3 to = core->transform.worldPosition - transform.worldPosition;
    to.y = 0.0f;
    return to;
}

inline void PlayerClimbComponent::SetBodyCollision(bool on)
{
    // プレイヤーの体はカプセル 1 つ。形を差し替えたときに黙って効かなくならないよう、
    // 素直な 3 種類は全部見る (無い型は nullptr で素通りする)。
    if (auto* capsule = scene.GetComponent<CapsuleColliderComponent>()) capsule->enabled = on;
    if (auto* box     = scene.GetComponent<BoxColliderComponent>())     box->enabled     = on;
    if (auto* sphere  = scene.GetComponent<SphereColliderComponent>())  sphere->enabled  = on;
}

inline void PlayerClimbComponent::RegisterClips()
{
    auto* component = scene.GetComponent<AnimatorComponent>();
    if (!component) return;

    bool added = false;
    const auto declare = [&](const std::string& path, const char* label) {
        if (path.empty()) return;

        // .fbx を指した参照は «clips を持たないモデル» を読んで終わる。Slot は
        // 無音で畳まれ、クリップ名も尺も正しく見えるので原因に辿り着けない。
        // 黙って失敗させず、直し方まで名指しで言う。
        if (path.size() >= 4 &&
            path.compare(path.size() - 4, 4, ".fbx") == 0) {
            debug.LogError("PlayerClimbComponent: " + std::string(label) +
                           " points at an .fbx (" + path + "). Clips live in the baked "
                           ".anim, so the slot plays nothing. Use "
                           "'Assets/<dir>/<Name>/anims/<Clip>.anim' (no 'guid:' prefix "
                           "— the guid wins and resolves back to the .fbx).");
            return;
        }

        auto& sources = component->externalClipSources;
        if (std::find(sources.begin(), sources.end(), path) != sources.end()) return;
        sources.push_back(path);
        added = true;
    };
    declare(climbClipFile, "Climb Clip");

    // 宣言しただけでは読み直されない。読み込み済みの札を落として作り直させる。
    if (added) component->clipsLoaded = false;
}

inline void PlayerClimbComponent::KeepClimbClip()
{
    if (m_phase != Phase::Climb || layerName.empty() || climbClipFile.empty()) return;
    if (animator.IsSlotPlaying(layerName)) { m_clipRetries = 0; return; }

    // 鳴っていない。クリップが積まれているかを確かめ直してから鳴らし直す。
    RegisterClips();
    const float speed = Clamp(std::max(grabInterval, 0.05f) /
                              std::max(AverageStep(), 0.05f), 0.25f, 4.0f);
    animator.PlaySlot(layerName, climbClipFile, climbClipName, 0.10f, 0.12f, speed, true);

    // 1 回きりなら «レイヤーが差し替わった» の回復で、これは正常。何フレーム続けても
    // 畳まれるなら原因はクリップの参照側にしか無い ── そこで初めて名指しで言う
    // (PlaySlot 直後は必ず «鳴っている» ので、その場では判定できない)。
    if (++m_clipRetries >= 3 && !m_warnedClip) {
        m_warnedClip = true;
        debug.LogError("PlayerClimbComponent: clip '" + climbClipName + "' (" +
                       climbClipFile + ") did not start on layer '" + layerName +
                       "'. The animator drops slots whose clip is not loaded — the "
                       "path must resolve to a baked .anim (see Climb Clip の WHY).");
    }
}

inline void PlayerClimbComponent::DriveLayerWeight()
{
    if (layerName.empty()) return;

    const float slot = Clamp01(animator.GetSlotWeight(layerName));
    animator.SetLayerWeight(layerName, slot);
    debugSlot = slot;

    // マスクを外すのは «自分が鳴らしている間» だけ。フェードアウトが終わるまでは
    // 外したままにする ── 途中で戻すと、消えかけの姿勢から脚だけ 1 フレームで
    // ロコモーションへ跳ねる。
    const bool want = forceFullBody && (m_phase != Phase::None || slot > EPSILON);

    if (want) {
        // 毎フレーム «外れているか» を確かめる。Controller が読み直されるとレイヤーは
        // アセットの値ごと差し替わり、外したはずのマスクが黙って戻る
        // (KeepClimbClip の WHY と同じ経路)。
        const std::string current = animator.GetLayerMask(layerName);
        if (!current.empty()) {
            if (!m_maskForced) m_savedMask = current;
            animator.SetLayerMask(layerName, "");
        }
        m_maskForced = true;
        return;
    }

    if (m_maskForced) {
        m_maskForced = false;
        animator.SetLayerMask(layerName, m_savedMask);
    }
}

inline Vector3 PlayerClimbComponent::FacingFor(const Vector3& tangent) const
{
    // 段が寝ているうちは «進んでいる向き» が体の正面。
    Vector3 flat{ tangent.x, 0.0f, tangent.z };
    if (flat.Length() >= std::max(faceTangentMin, 0.0f)) return flat;

    // 立った段では接線の水平成分がほぼ 0 で、残っているのは丸め誤差だけ ──
    // そのまま向きにすると体が毎フレーム別の方角を向く。掴んでいる脚の方を向く。
    if (GameObject* boss = Boss())
        if (GameObject* body = FindInSubtree(*boss, deckAnchorBone)) {
            Vector3 to = body->transform.worldPosition - transform.worldPosition;
            to.y = 0.0f;
            if (to.LengthSq() > EPSILON) return to;
        }
    return Vector3::ZERO;
}

inline float PlayerClimbComponent::StepDuration(std::size_t step) const
{
    const float beat = std::max(stepSeconds, 0.08f);
    if (step + 1 >= m_path.size()) return beat;

    // 段の長さで «どれだけ» 拍を伸ばすか。0 なら段の長さに関わらず同じ拍で刻む。
    const float length = (m_path[step + 1] - m_path[step]).Length();
    const float ratio  = length / std::max(stepReference, 0.2f);
    return std::max(beat * Lerp(1.0f, ratio, Clamp01(lengthInfluence)), 0.06f);
}

inline float PlayerClimbComponent::AverageStep() const
{
    if (m_path.size() < 2) return std::max(stepSeconds, 0.08f);
    float total = 0.0f;
    for (std::size_t i = 0; i + 1 < m_path.size(); ++i) total += StepDuration(i);
    return total / static_cast<float>(m_path.size() - 1);
}

inline Vector3 PlayerClimbComponent::PointAtJoint(float joint) const
{
    if (m_path.empty())   return transform.worldPosition;
    if (m_path.size() < 2) return m_path.front();

    const float  last  = static_cast<float>(m_path.size() - 1);
    const float  where = Clamp(joint, 0.0f, last);
    const auto   index = std::min(static_cast<std::size_t>(where), m_path.size() - 2);
    return Vector3::Lerp(m_path[index], m_path[index + 1], where - static_cast<float>(index));
}

inline void PlayerClimbComponent::EnsureGripTargets()
{
    // WHY 毎回名前で拾い直すか: スクリプト DLL をリロードすると Script は作り直され、
    //     EntityRef は空へ戻る。作った GameObject はシーンに残るので、«作った» を
    //     覚えたままだと的が 1 組ずつ増え続ける (ボスの IK と同じ理由)。
    if (m_gripBuilt && m_gripTarget[0].Resolve(scene)) return;

    static constexpr const char* kName[LimbCount] = {
        "PlayerGrip_HandR", "PlayerGrip_HandL", "PlayerGrip_FootR", "PlayerGrip_FootL"
    };
    for (int limb = 0; limb < LimbCount; ++limb) {
        GameObject* target = scene.Find(kName[limb]);
        if (!target) {
            // scene.Create は GameObject 配列を再確保する。作って即しまうだけに留める。
            GameObject& created      = scene.Create(kName[limb]);
            created.runtimeGenerated = true;
            target = &created;
        }
        m_gripTarget[limb] = EntityRef{ target->GetID() };
    }
    m_gripBuilt = true;
}

inline IKChain* PlayerClimbComponent::EnsureGripChain(int limb)
{
    GameObject* self   = scene.Self();
    GameObject* target = m_gripTarget[limb].Resolve(scene);
    if (!self || !target) return nullptr;

    auto* ik = self->GetComponent<IKSolverComponent>();
    if (!ik) ik = &self->AddComponent<IKSolverComponent>();
    ik->enabled = true;

    const int order = kGripOrder + limb;
    for (IKChain& chain : ik->chains)
        if (chain.order == order) {
            chain.targetEntity = target->GetID();
            return &chain;
        }

    // 骨の並びは Player.skel のとおり。腕は 3 節・脚も 3 節なので TwoBone で解ける。
    static constexpr const char* kBones[LimbCount][3] = {
        { "UpperArm_R", "Forearm_R", "Hand_R" },
        { "UpperArm_L", "Forearm_L", "Hand_L" },
        { "Thigh_R",    "Shin_R",    "Foot_R" },
        { "Thigh_L",    "Shin_L",    "Foot_L" },
    };

    IKChain chain{};
    chain.type         = IKSolverType::TwoBone;
    chain.boneNames    = { kBones[limb][0], kBones[limb][1], kBones[limb][2] };
    chain.order        = order;
    chain.enabled      = false;
    chain.weight       = 0.0f;
    chain.targetEntity = target->GetID();
    // 伸びきる手前で止める。腕を «突っ張った棒» にすると、届かない所を指した瞬間に
    // 肩ごと持っていかれる。
    chain.maxExtension = 0.96f;
    ik->chains.push_back(std::move(chain));
    return &ik->chains.back();
}

inline void PlayerClimbComponent::ReleaseGrip()
{
    GameObject* self = scene.Self();
    if (!self) return;
    if (auto* ik = self->GetComponent<IKSolverComponent>())
        for (IKChain& chain : ik->chains)
            if (chain.order >= kGripOrder && chain.order < kGripOrder + LimbCount) {
                chain.enabled = false;
                chain.weight  = 0.0f;
            }
    m_gripFade = 0.0f;
}

inline void PlayerClimbComponent::DriveGrip(float dt, const Vector3& facing)
{
    // 掴むのは «経路の上を登っている間» だけ。納刀中は刀を背へ回している最中で、
    // 甲板では普通に立っている ── どちらも手は空いていなければならない。
    const bool want = gripIk && m_phase == Phase::Climb && m_path.size() >= 2;
    const float step = dt / std::max(gripBlend, 0.01f);
    m_gripFade = Clamp01(m_gripFade + (want ? step : -step));
    if (m_gripFade <= EPSILON) { ReleaseGrip(); return; }

    EnsureGripTargets();

    // 脚をまたいで掴む。左右へ張り出す向きは «体の右» ── 経路の接線から作ると、
    // 段が垂直なところで外積が潰れて左右が入れ替わる。
    const Vector3 side = Vector3::Cross(Vector3::UP, facing).NormalizedOr(Vector3::ZERO);
    if (side.LengthSq() <= EPSILON) return;

    for (int limb = 0; limb < LimbCount; ++limb) {
        IKChain*    chain  = EnsureGripChain(limb);
        GameObject* target = m_gripTarget[limb].Resolve(scene);
        if (!chain || !target) continue;

        const float sign = (limb == HandR || limb == FootR) ? 1.0f : -1.0f;
        const Vector3 at = PointAtJoint(m_gripJoint[limb]) + side * (gripRadius * sign);

        // 的はルート直下の空オブジェクト。ローカルまで書かないと、次の
        // TransformSystem が local から world を組み直して掴む所が原点へ落ちる。
        target->transform.position      = at;
        target->transform.worldPosition = at;

        chain->enabled = true;
        chain->weight  = Clamp01(m_gripFade * Clamp01(gripWeight));
    }
}

inline void PlayerClimbComponent::OnGrab()
{
    // 手を左右で入れ替える。体の振り (DrivePosture の sway) と音が同じ拍で来るので、
    // «右手で掴んで体が右へ寄る» が 1 つの出来事として読める。
    m_rightHand = !m_rightHand;

    // 掴み替え。掴んだ手は «これから登る関節» を握り、対角の足が «体が離れる関節» へ
    // 乗る ── 人も四足も、手と足は対角で入れ替わる。残る 2 本は前の掴みのまま
    // 動かないので、画面には «3 点で支えて 1 本ずつ掛け替える» が出る。
    const int hand = m_rightHand ? HandR : HandL;
    const int foot = m_rightHand ? FootL : FootR;
    m_gripJoint[hand] = static_cast<float>(m_step) + 1.0f + Clamp01(handLead);
    m_gripJoint[foot] = static_cast<float>(m_step) - Clamp01(footTrail);

    se::Play(audio, se::kPlayerClimbGrab, climbVolume);
    if (grabRumble > 0.0f)
        if (auto* rumble = RumbleManagerComponent::Instance())
            rumble->Rumble(Clamp01(grabRumble));
}

inline void PlayerClimbComponent::DrivePosture(const Vector3& tangent, float dt, bool onPath,
                                               float sway)
{
    float pitch = 0.0f;
    float roll  = sway;

    if (onPath) {
        // 段の «立ち上がり角»。垂直な段で 90 度、水平な渡りで 0 度。
        const Vector3 t = tangent.NormalizedOr(Vector3::ZERO);
        if (t.LengthSq() > EPSILON) {
            const float elevation = ToDeg(std::asin(Clamp(t.y, -1.0f, 1.0f)));
            pitch = Clamp(elevation * std::max(leanScale, 0.0f), 0.0f, std::max(leanMax, 0.0f));
        }
    }

    // 段が切り替わる所で角度は階段状に変わる。そのまま渡すと «カクッと» 折れるので、
    // 体の側で寄せる (向きは PlayerController が Slerp するので二重に鈍らせない)。
    const float k = 1.0f - std::exp(-std::max(leanSmooth, 0.1f) * dt);
    m_leanPitch += (pitch - m_leanPitch) * k;
    m_leanRoll  += (roll  - m_leanRoll)  * k;
    debugLean = m_leanPitch;
}

inline void PlayerClimbComponent::HoldPlayer(const Vector3& facing, bool holdInput)
{
    // WHY 毎フレーム置き直すか: cutscene の申告は 0.25 秒で古びる (maxAge)。
    //     取り付いた 1 回だけ置いていたので、登り始めて 0.25 秒後には操作が戻り、
    //     経路の書き込みと移動入力が同じ座標を奪い合っていた。
    //
    // WHY unscaledTime か: 読む側 (PlayerController / Blade / Parry / BossAi) が
    //     全部 unscaledTime で古さを測る。ヒットストップで止めた秒数のぶん
    //     Time::time は遅れるので、scaled で置くと最初から «古い申告» になる。
    if (holdInput)
        cutscene::Publish(/*holdBoss=*/false, /*holdPlayer=*/true, Time::unscaledTime);

    // 入力を止めるだけでは重力と衝突解決が進む。座標を置き換えている間は
    // 移動そのものを止めないと、落下速度が登攀の 3 秒ぶん積み上がる。
    // 操作は PlayerComponent の内部モジュールなので、窓口は Player 本体が持っている。
    if (auto* player = scene.GetScript<PlayerComponent>()) {
        player->RequestSuspend(holdInput);
        if (facing.LengthSq() > EPSILON) player->RequestFacing(facing);
        // 傾きは «向きが決まっている» ときだけ意味を持つ。倒す軸を向きから作るので、
        // 向きが無いフレームに掛けると前後左右の定まらない回転になる。
        if (facing.LengthSq() > EPSILON) player->RequestLean(m_leanPitch, m_leanRoll);
    }
}

inline void PlayerClimbComponent::Place(const Vector3& world)
{
    transform.position      = world;
    transform.worldPosition = world;
}

inline void PlayerClimbComponent::Dismount()
{
    if (m_phase == Phase::None) return;
    m_phase = Phase::None;
    m_timer = 0.0f;
    m_travel = 0.0f;
    m_step = 0; m_stepTime = 0.0f;
    debugStep = 0;
    ReleaseGrip();
    m_path.clear();
    m_carryValid = false;
    animator.StopSlot(layerName);
    cutscene::Publish(/*holdBoss=*/false, /*holdPlayer=*/false, Time::unscaledTime);
}

inline void PlayerClimbComponent::OnStart()
{
    m_phase = Phase::None; m_timer = 0.0f; m_travel = 0.0f;
    m_step = 0; m_stepTime = 0.0f; m_rightHand = true;
    m_leanPitch = 0.0f; m_leanRoll = 0.0f;
    m_warnedLayer = false; m_warnedClip = false; m_clipRetries = 0;
    // 前回の Play / DLL リロードが登攀の途中で終わっていると、当たりを外したまま
    // シーンに残る。旗は作り直しで false へ戻るので、外れっぱなしに気付けない。
    m_colliderOff = false;
    SetBodyCollision(true);
    m_maskForced = false; m_savedMask.clear();
    // 鳴らすクリップは «鳴らす側» が宣言する。ここで積んでおかないと Slot が空振りする。
    RegisterClips();
    m_carryValid = false; m_mountGrace = 0.0f;
    m_path.clear(); m_boss = {}; m_suffix.clear();
    for (EntityRef& foot : m_feet) foot = {};
    m_footProbe = 0.0f;
    debugPhase = "None"; debugProgress = 0.0f; debugStep = 0; debugLeg.clear();
}

inline void PlayerClimbComponent::OnDestroy()
{
    // 破棄では OnDisable が来ない。ここで戻さないと «当たりの無いプレイヤー» と
    // «マスクの外れたレイヤー» (＝斬撃まで全身で出る) が残る。
    if (m_colliderOff) {
        m_colliderOff = false;
        SetBodyCollision(true);
    }
    if (m_maskForced) {
        m_maskForced = false;
        animator.SetLayerMask(layerName, m_savedMask);
    }
    ReleaseGrip();
}

inline bool PlayerClimbComponent::Climbable(const IBoss* boss) const
{
    return boss && (!requireToppled || boss->IsToppled());
}

inline void PlayerClimbComponent::OnUpdate()
{
    const float dt = std::max(Time::deltaTime, 0.0f);

    // 鳴っていなければ鳴らし直す (KeepClimbClip の WHY)。重みはその後で流す ──
    // 順番が逆だと、鳴らし直した最初の 1 フレームだけ重み 0 で出ない。
    KeepClimbClip();
    // 降りた後もフェードアウトが終わるまで面倒を見る。降りた瞬間に手を離すと、
    // 被せ量が残ったまま固まって «降りたのに登る姿勢のまま» になる。
    DriveLayerWeight();

    // 登っていないフレームでも IK は戻しに行く。Climb を抜けた瞬間に呼ばなくなると、
    // 最後の掴みのまま効きが残って «歩きながら空を掴む» になる。
    if (m_phase != Phase::Climb) DriveGrip(dt, Vector3::ZERO);

    // 当たりは «経路の上に居る間» だけ外す (フィールドの WHY)。状態から毎フレーム
    // 決めるので、どの経路で降りても ── 撃破・振り落とし・DLL リロード ── 戻る。
    if (const bool detach = detachCollider && IsClimbing(); detach != m_colliderOff) {
        m_colliderOff = detach;
        SetBodyCollision(!detach);
    }

    GameObject* boss = Boss();
    const IBoss* iboss = IBoss::Of(boss);

    // 乗っているあいだは倒れたままにしてもらう。
    // WHY 毎フレーム «あと holdSeconds 秒» と言い続けるか: 降りた瞬間に言うのをやめれば、
    //     ボスは holdSeconds 後に起き上がる。«いつ降りたか» を別に伝える必要がない。
    if (m_phase != Phase::None && boss)
        if (auto* ai = scene.GetScript<BossAiComponent>(boss))
            ai->HoldTopple(std::max(holdSeconds, 0.0f));

    // 降りるのは «ボスが居なくなった» か «自分で離した» ときだけ。
    //
    // WHY 起き上がりで降ろさないか (2026-09-08): 以前は転倒が解けた瞬間に落としていたが、
    //     それだと «登り切る前に必ず落ちる»。掴まっている以上、ボスが立ち上がっても
    //     振り落とされるまでは付いていくのが自然で、そのほうが絵としても強い。
    if (m_phase != Phase::None && !boss) Dismount();
    // 掴んでいる最中にもう一度押したら手を離す。甲板に着くまでは離せない
    // ── 経路の途中で操作を返すと、壁の中に置き去りになる。
    if (m_phase == Phase::Deck && m_mountGrace <= 0.0f && input.GetActionDown(mountAction))
        Dismount();
    if (m_mountGrace > 0.0f) m_mountGrace -= dt;

    switch (m_phase) {
    case Phase::None: {
        if (!Climbable(iboss)) break;
        if (!input.GetActionDown(mountAction)) break;
        std::string suffix; Vector3 foot;
        if (!FindNearestLeg(suffix, foot)) break;

        m_suffix = suffix;
        m_boss   = boss ? EntityRef{ boss->GetID() } : EntityRef{};
        BuildPath(suffix);
        if (m_path.size() < 2) break;

        m_phase = Phase::Sheathe; m_timer = 0.0f; m_travel = 0.0f;
        // 取り付いた同じフレームの入力で «すぐ離す» にならないように 1 拍置く。
        m_mountGrace = 0.20f;
        m_carryValid = false;
        // 取り付いた足元へ吸い付ける。走り込んだ勢いのまま脚の «横» から登り始めると、
        // 1 段目の踊り場まで斜めに滑って上がる絵になる。
        Place(m_path.front());
        // 刀を背へ回す音。二刀なので «必ず 2 回鳴る» のが素材側の前提
        // (Assets/Sound/SE/README_v3_Blades_Arena.md)。ここは 1 本目。
        // 刀を «実際に» 背へ移す。クリップを流すだけでは刀は手に握られたままで、
        // 両手で掴むはずの登攀で刀が壁を貫く。芝居・音・付け替えの時刻は
        // すべて WeaponRigComponent が持っているので、頼むだけにする。
        if (auto* player = scene.GetScript<PlayerComponent>())
            if (player->AreWeaponsDrawn()) player->RequestSheatheWeapons();
        m_step = 0; m_stepTime = 0.0f; m_rightHand = true;
        // OnStart がアニメーターより先に走ることがある。取り付く瞬間に積み直す
        // (既にあれば何もしない)。
        RegisterClips();
        m_leanPitch = 0.0f;
        m_leanRoll  = 0.0f;
        // 納刀の «絵» は WeaponRig が上半身レイヤーへ流す。ここで全身レイヤーへ
        // 二重に流すと、脚まで納刀の立ち姿へ固まってから登り始める。

        // 名指しのレイヤーが Controller に無いと PlaySlot も SetLayerWeight も
        // 黙って何もしない。症状は «登るが姿勢が変わらない» だけで、クリップにも
        // 尺にも異常が出ない ── 出どころが分からない壊れ方なので、1 度だけ言う。
        if (!m_warnedLayer) {
            m_warnedLayer = true;
            animator.SetLayerWeight(layerName, 1.0f);
            if (animator.GetLayerWeight(layerName) <= EPSILON)
                debug.LogError("PlayerClimbComponent: animator layer '" + layerName +
                               "' not found. Add a full-body (Player_Base.mask) layer to "
                               "Assets/Animation/Player/Player.animcontroller, or point "
                               "Layer at one that exists.");
        }
        debugLeg = suffix;
        break;
    }
    case Phase::Sheathe:
        BuildPath(m_suffix);
        // 足元へ貼り付けたまま納刀する。ここで手を離すと脚から落ちる。
        if (!m_path.empty()) Place(m_path.front());
        // 納刀の間は «脚を見上げて構える»。ここで傾けると、刀を背へ回す動作が
        // 斜めになって «倒れながら納刀している» になる。
        DrivePosture(Vector3::ZERO, dt, /*onPath=*/false, /*sway=*/0.0f);
        HoldPlayer(FacingFor(StepDirection(0)), /*holdInput=*/true);
        m_timer += dt;
        if (m_timer >= std::max(sheatheSeconds, 0.0f)) {
            m_phase = Phase::Climb; m_timer = 0.0f;
            m_step = 0; m_stepTime = 0.0f;
            // 登り始めの «構え»。右手が 1 つ上の関節、左手はその手前、両足は足元。
            // ここから 1 拍ごとに 1 本ずつ掛け替わる。
            m_gripJoint[HandR] = 1.0f;
            m_gripJoint[HandL] = 0.55f;
            m_gripJoint[FootR] = 0.0f;
            m_gripJoint[FootL] = 0.0f;
            // 1 段目にも手を掛ける。«登り始めの 1 手» が無いと、最初の関節だけ
            // 無音で上がって拍が 1 つ足りなく聞こえる。
            OnGrab();
            if (!climbClipFile.empty()) {
                // クリップの «1 掴み» を拍の長さへ合わせる。速さを合わせないと、
                // 体が段を跨いでいる最中にクリップだけ 2 回手を伸ばす。
                const float speed = Clamp(std::max(grabInterval, 0.05f) /
                                          std::max(AverageStep(), 0.05f), 0.25f, 4.0f);
                animator.PlaySlot(layerName, climbClipFile, climbClipName,
                                  0.10f, 0.12f, speed, true);
            }
        }
        break;
    case Phase::Climb: {
        // 経路は毎フレーム引き直す。転倒の揺り戻しで脚が動いても付いていける。
        // 段は «番号» で持っているので、踊り場が動いても «次はあの関節» は変わらない。
        BuildPath(m_suffix);
        if (m_path.size() < 2) break;
        const std::size_t last = m_path.size() - 2;
        m_step = std::min(m_step, last);

        m_stepTime += dt;
        float duration = StepDuration(m_step);
        // 1 フレームで複数の段を跨ぐこともある (拍が短い / フレームが飛んだ)。
        // 掴んだ数だけ音と手の入れ替えを通す ─ 飛ばすと «無音で 2 段上がる» になる。
        while (m_stepTime >= duration && m_step < last) {
            m_stepTime -= duration;
            ++m_step;
            duration = StepDuration(m_step);
            OnGrab();
        }

        const float t = Clamp01(m_stepTime / std::max(duration, 0.01f));
        // 掴んで «引く» のが前半、«止まって» いるのが後半。この止めが拍を作る。
        const float pull = Clamp01(t / std::max(1.0f - Clamp01(stepSettle), 0.05f));
        // 引きは減速して着く (ease-out)。等速だと拍の頭が立たない。
        const float ease = 1.0f - (1.0f - pull) * (1.0f - pull) * (1.0f - pull);

        const Vector3 from = m_path[m_step];
        const Vector3 to   = m_path[m_step + 1];
        Vector3       at   = Vector3::Lerp(from, to, ease);
        // 引く途中でわずかに行き過ぎて戻る。«伸び上がって掴む» が出る。
        at.y += std::sin(PI * pull) * std::max(stepBounce, 0.0f);
        Place(at);

        // 向きは «脚へ正対»、傾きは «段の斜度»。接線をそのまま向きにすると、垂直な段で
        // 水平成分が消えて体が振り回される (FacingFor の WHY)。
        const Vector3 tangent = StepDirection(m_step);
        // 体が振れるのは «引いている間» だけ。掴んで止まっている間に戻るので、
        // 揺れそのものが拍を数えている絵になる。
        const float sway = std::sin(PI * pull) * swayDegrees * (m_rightHand ? 1.0f : -1.0f);
        DrivePosture(tangent, dt, /*onPath=*/true, sway);
        const Vector3 facing = FacingFor(tangent);
        HoldPlayer(facing, /*holdInput=*/true);
        // 掴む所は経路の «関節» なので、体が動いても脚が揺れても手足は貼り付いたまま。
        DriveGrip(dt, facing);

        m_travel = (static_cast<float>(m_step) + t) / static_cast<float>(last + 1);
        debugProgress = m_travel * PathLength();
        debugStep     = static_cast<int>(m_step) + 1;

        // 最後の段を掴み切ったら甲板。ここだけは «次の段» が無いので while を抜ける。
        if (m_step >= last && t >= 1.0f) {
            m_phase = Phase::Draw; m_timer = 0.0f;
            // 登攀のクリップはここで終わり。抜刀は WeaponRig が上半身へ流すので、
            // 全身レイヤーは手放して甲板の立ち姿 (ロコモーション) へ戻す。
            animator.StopSlot(layerName, 0.08f);
            if (auto* player = scene.GetScript<PlayerComponent>())
                if (!player->AreWeaponsDrawn()) player->RequestDrawWeapons();
        }
        break;
    }
    case Phase::Draw:
        // 抜刀の間も甲板の着地点へ置き続ける。ここで座標を手放すと、
        // 抜いている 0.6 秒のあいだにボスの揺り戻しで甲板が下から抜ける。
        BuildPath(m_suffix);
        if (!m_path.empty()) Place(m_path.back());
        // 抜いたら正面にコアが居てほしい。とどめ (Execute) は正面へ振り下ろすので、
        // 向いていないと «着いたのに何も起きない» になる。
        // 甲板は平らなので傾きは 0 へ戻す ── 登り切った前傾のまま抜刀すると、
        // 操作が返った瞬間に体が直立へ跳ねる。
        DrivePosture(Vector3::ZERO, dt, /*onPath=*/false, /*sway=*/0.0f);
        HoldPlayer(CoreFacing(), /*holdInput=*/true);
        m_timer += dt;
        if (m_timer >= std::max(drawSeconds, 0.0f)) {
            m_phase = Phase::Deck; m_timer = 0.0f;
            m_carryValid = false;
            // 甲板に着いたら操作を返す。ここから先は普通の戦闘 ── コアが
            // とどめ の的になっているので、Parry で居合が出る。
            cutscene::Publish(false, false, Time::unscaledTime);
            // 着地音を «甲板に乗った» に流用する。専用の音を足さないのは、
            // 高さから落ちた着地と «乗り移った» が体の側では同じ出来事だから。
            se::Play(audio, se::kPlayerLanding, climbVolume * 0.85f);
        }
        break;
    case Phase::Deck:
        // 起き上がられたら振り落とされる。
        //
        // WHY 登っている最中は落とさないのに甲板では落とすか: 途中で落とすと
        //     «登り切る前に必ず落ちる» になるが (2026-09-08 の判断)、立ち上がった
        //     重機の背に乗り続けられると «登った先が一番安全» になる。
        //     Docs/climb-core.md の契約 3「経路に乗られたら振り落とす手を持つ」。
        if (!iboss || !iboss->IsToppled()) { Dismount(); break; }

        // 甲板の上では普通に歩ける。座標は握らず、**ボスが動いたぶんだけ運ぶ**。
        //
        // WHY 座標を貼り直さないか: 貼り直すと甲板の上を歩けなくなる。足場そのものは
        //     PlayerBossBlockComponent の «背に乗る» が受け持つ (面の円の中に居れば
        //     面へ上げる)。物理の当たりを背に置く道もあるが、それはボス自身を
        //     押す危険と引き換えになる ── あちらの WHY を参照。
        // WHY それでも運ぶ必要があるか: 転倒中でも体は揺り戻しで動く。
        //     箱に乗っているだけでは «床が横へ滑って足が置いていかれる» が起きる。
        CarryWithBoss();
        break;
    }

    static constexpr const char* kName[] = { "None", "Sheathe", "Climb", "Draw", "Deck" };
    debugPhase = kName[static_cast<int>(m_phase)];
}

inline void PlayerClimbComponent::CarryWithBoss()
{
    GameObject* boss = Boss();
    GameObject* anchor = boss ? FindInSubtree(*boss, deckAnchorBone) : nullptr;
    if (!anchor) { m_carryValid = false; return; }

    const Vector3    now    = anchor->transform.worldPosition;
    const Quaternion nowRot = anchor->transform.worldRotation;

    if (m_carryValid) {
        // 平行移動と «向きの変化» の両方を渡す。
        // WHY 回転も要るか: ボスがその場で向きを変えると、甲板の上の点は円を描いて動く。
        //     平行移動だけ渡すと、旋回した瞬間に甲板から滑り落ちる。
        const Quaternion delta = nowRot * m_carryRot.Inverse();
        const Vector3    local = transform.worldPosition - m_carryPos;
        Place(now + delta * local);
    }
    m_carryPos   = now;
    m_carryRot   = nowRot;
    m_carryValid = true;
}

inline void PlayerClimbComponent::OnDrawGizmos()
{
    if (m_path.size() < 2) return;

    if (drawPath)
        for (std::size_t i = 1; i < m_path.size(); ++i)
            debug.DrawLine(m_path[i - 1], m_path[i], { 0.2f, 1.0f, 0.45f, 1.0f });

    // 掴んでいる 4 点。手が青・足が橙で、掛け替わる 1 本だけが拍ごとに飛ぶ。
    if (!drawGrip || m_gripFade <= EPSILON) return;
    for (int limb = 0; limb < LimbCount; ++limb)
        if (GameObject* target = m_gripTarget[limb].Resolve(scene)) {
            const bool hand = limb == HandR || limb == HandL;
            const Vector4 color = hand ? Vector4{ 0.35f, 0.7f, 1.0f, 1.0f }
                                       : Vector4{ 1.0f, 0.62f, 0.2f, 1.0f };
            debug.DrawSphere(target->transform.worldPosition, 0.09f, color);
        }
}

} // namespace sandbox
