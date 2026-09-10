/// @file    BossShockwaveComponent.hpp
/// @brief   ボスの着地から地面を走る円形衝撃波。跳ばないと越えられない
/// @author  Hasegawa Jin
/// @date    2026-08-27
///
/// WHY 大ジャンプに «外向きの波» を足すか:
///   着地の判定は落下点を中心にした 1 つの円で、遠くへ居れば何もせずに済んでしまう。
///   8 章が大ジャンプへ与えた «踏みつけの間合いから逃げた相手を追う» は、逃げた先にも
///   何かが届いて初めて成立する。落下点の外へ広がる波なら、距離を取ったこと自体は
///   正しいまま「その 1 回だけ跳ぶ」という別の答えを要求できる。
///
/// WHY «跳んで避ける» にするか (横へ逃げるではなく):
///   横へ逃げる攻撃は盤面に既に 3 つある (突進・コアビーム・踏みつけ)。同じ答えを
///   4 つ目に足しても、覚えることが増えるだけで択が増えない。波は円なので横移動では
///   絶対に外れず、走って逃げ切れる速さでもない。残る手が跳躍だけになる。
///
/// WHY 落下点の直撃と «別の当たり» にするか:
///   直撃 (BossAi の Shock Radius) は «腹の下に居た» ことへの罰で、跳んでも避けられない。
///   波はその外側だけを担当する。1 つの判定で兼ねると、跳べば真下でも助かることになり、
///   腹下へ潜る危険が消える。
///
/// WHY 高さで «越えた» を決めるか (接地フラグではなく):
///   CharacterController の isGrounded は接地の «猶予» を持っていて、跳んだ直後や
///   降り際に true と false が入れ替わる。跳んだのに当たった / 立っているのに抜けた、
///   のどちらもそこから出る。波が走っているのは地面なので、«波の面から何 m 上に居るか»
///   だけで決める方が、絵と規則が一致する。
///
/// WHY 光る輪を持たないか:
///   以前は加算の筒を 5 本 (前縁・後続・段 3 本) 走らせていた。半透明の図形は «そこに
///   何かが描いてある» としか読めず、床が砕けて空気が押し出された出来事には見えない。
///   波の姿は煙と爆発だけで作る ─ 立ち上がった土煙は床の高さに縛られるので、
///   «どこまで来たか» は輪より読みやすい。
///
/// WHY それでも判定は円のままか:
///   煙は 1 発ごとに散らして置くので、当たる線を «絵の縁» から決めると、粒の湧き方で
///   毎回変わる当たりになる。判定は半径と帯幅だけが持ち、煙はその線の上に湧かせる。
///
/// ── 誰が «前縁» を描いているか (2026-09-11 に測って組み直した) ────────────
///   «波が見えない» の原因は絵の弱さではなく、3 つとも配分だった。
///
///   1. 枠の食い潰し: 土煙 1 発は Duration 0.4 + Lifetime 0.9±0.3 で最大 1.6 秒
///      «枠を占有する»。波の全長がそれとほぼ同じなので、«1 回の波で撒いた総数» が
///      そのまま同時生存数になる。旧設定は 着地 16 + 輪 8 枚 x 14 点 + 前縁 15 で
///      143 発 ─ 枠 56 の 2.5 倍で、リングは丸ごと «最も古い枠を奪う» ので、
///      どの煙も寿命の半分も生きずに次の輪へ奪われていた。大きく育つ前に消える。
///   2. 前縁の密度: 点数は円周 / Spacing を 16 で頭打ちにしていた。r=15m では
///      円周 94m ÷ 16 = 5.9m 間隔。プレイヤーの幅 1m に対して «ほとんど穴»。
///   3. 刻みが逆: 輪は «時間» で置いていた。速さは生まれた直後が 2.4 倍なので、
///      一番速く一番危ない出だしが一番スカスカ (1 枚目と 2 枚目の間が 4m)。
///
///   組み直した後の分担:
///     煙   … «質感». 総数を枠に収めて 1 発ずつ最後まで育てる。密度は捨てる。
///     亀裂 … «前縁の線». デカールは VFX の枠を使わないので、ここだけは密に置ける。
///            プレイヤー側の狭い弧へ寄せて、置きたての熱で光る線を床に引く。
///     光   … «見える条件». 灰色の煙を暗い床の上で読ませるには 1 灯要る。
///
///   密度が要る所へ密度を置く。全周へ均等に配ると、見えない後ろ半分に半分を捨てる。
///
///   組み直した後の 1 回ぶん (既定値でシミュレートした実測値):
///     全長 1.55 秒 / 輪 9 枚 (段の間隔 2.0m 一定)
///     土煙  88 / 枠 96      爆発 14 / 枠 20      亀裂 112 / 枠 128
///     前縁の間隔 …  8m 地点: 煙 2.3m・亀裂 1.2m   15m 地点: 煙 4.4m・亀裂 2.2m
///     到達 …  5m 0.25s   8m 0.48s   12m 0.83s   16m 1.18s  (旧: 8m 0.37s)
///
///   WHY 煙の間隔が外周で 4.4m «も» 空いたままか: 枠 96 が上限で、r=15m の円周は
///       94m ある。連続した煙の壁は枠の上でどうやっても作れない ─ だから線は
///       亀裂に任せ、煙は «数» ではなく «育ちきった 1 発の大きさ» で効かせる。
///       ここを詰めようとして点を増やすと、リングが古い枠を奪って全部が小さくなる。
#pragma once

#include <Engine/Scene/Components/AudioSourceComponent.hpp>
#include <Engine/Scene/Components/DecalComponent.hpp>
#include <Engine/Scene/Components/LightComponent.hpp>
#include <Engine/Scene/EntityRef.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Scripts/Combat/BossCoreComponent.hpp>
#include <Scripts/Game/CameraShakeManagerComponent.hpp>
#include <Scripts/Game/CombatManagerComponent.hpp>
#include <Scripts/Game/RumbleManagerComponent.hpp>
#include <Scripts/Game/VfxManagerComponent.hpp>
#include <Scripts/Utils/BladeColors.hpp>
#include <Scripts/Utils/SeLibrary.hpp>
#include <algorithm>
#include <cmath>
#include <string>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

class BossShockwaveComponent : public Script {
    FBZZ_SCRIPT(BossShockwaveComponent)

    // 波の発生音はここから鳴らす。無ければ OnStart が自分で足す。
    FBZZ_OPTIONAL_COMPONENT(AudioSourceComponent)

public:
    FBZZ_GROUP("Wave")
    FBZZ_FIELD_RANGE(float, startRadius, 1.5f, "Start Radius", 0.0f, 20.0f)
    FBZZ_TOOLTIP("波が生まれる半径 [m]。ボスの胴体ぶん。0 にすると落下点で 1 フレーム "
                 "«点» が出てから広がるので、生まれた瞬間が読めない")
    FBZZ_FIELD_RANGE(float, maxRadius, 20.0f, "Max Radius", 1.0f, 60.0f)
    FBZZ_TOOLTIP("ここまで広がったら消える [m]。アリーナ半径 (実測 20m) が既定")
    FBZZ_FIELD_RANGE(float, speed, 11.0f, "速さ", 1.0f, 40.0f)
    FBZZ_TOOLTIP("広がる速さ [m/s]。プレイヤーの移動 6.0 m/s より明確に速くないと、"
                 "«走って逃げ切る» が最適解になって跳ぶ理由が消える")
    FBZZ_FIELD_RANGE(float, bandWidth, 1.0f, "Band Width", 0.1f, 5.0f)
    FBZZ_TOOLTIP("当たる帯の半幅 [m]。狭いほど «跳ぶ時刻» がシビアになる")
    FBZZ_FIELD_RANGE_INT(int, damage, 2, "ダメージ", 0, 100)
    FBZZ_TOOLTIP("直撃 (BossAi の Jump Stomp Damage) より軽く置く。"
                 "避け方が用意されている攻撃なので、当たった罰は腹下に居た罰より小さい")

    FBZZ_GROUP("Clearance")
    FBZZ_FIELD_TAG(playerTag, "Player", "プレイヤーのタグ")
    FBZZ_FIELD_RANGE(float, clearHeight, 0.90f, "クリアランス高さ", 0.0f, 4.0f)
    FBZZ_TOOLTIP("波の面からこの高さ以上に足が有れば越えられる [m]。"
                 "PlayerTuning の Apex Height (既定 4.0m) より十分低く保つこと。"
                 "近づけるほど «跳躍の頂点でしか抜けられない» になり、超えると理不尽になる")
    FBZZ_FIELD_RANGE(float, playerRadius, 0.45f, "プレイヤーの半径", 0.0f, 3.0f)
    FBZZ_TOOLTIP("プレイヤーの当たり半径 [m]。帯の半幅へ足して判定する")

    // WHY 等速をやめるか:
    //   等速の波は «円が大きくなっている» ようにしか見えず、叩きつけられた空気が
    //   走っている感じにならない。実際に跳ぶかどうかを決めるのは生まれた直後の
    //   1 秒未満なので、そこだけ速くすると «来る» の圧が出て、判断の時刻も前へ寄る。
    //
    // WHY 加速側にしか振らないか:
    //   Speed は «走って逃げ切れない» を保証している下限 (上の Speed の WHY)。
    //   減速側へ振ると外周で速度が落ち、逃げ切りが最適解に戻る。倍率は必ず 1 以上。
    FBZZ_GROUP("バースト")
    // WHY 2.4 → 1.5 か (2026-09-11): 2.4 だと 8m 先のプレイヤーへ 0.37 秒で届く。
    //     人の反応 (約 0.25 秒) に跳躍の発生を足すと、近距離では «見えても跳べない» ─
    //     «見えない» の一部は絵ではなく時間の問題だった。1.5 で 0.48 秒になり、
    //     Speed 11 が保証している «走って逃げ切れない» はそのまま残る。
    FBZZ_FIELD_RANGE(float, burstMultiplier, 1.5f, "Initial Speed x", 1.0f, 6.0f)
    FBZZ_TOOLTIP("生まれた瞬間の速さ倍率。1 で等速。上げるほど近距離が跳べなくなる")
    FBZZ_FIELD_RANGE(float, burstFalloff, 5.0f, "Falloff", 0.5f, 20.0f)
    FBZZ_TOOLTIP("倍率が Speed へ落ち着く速さ。大きいほど早く終端速度になる")

    // WHY 着地そのものを別に鳴らすか:
    //   走り出した波を «たどって» 出す煙は、1 発ずつが小さくないと輪にならない。
    //   だが着地の瞬間だけは 1 箇所で全部が起きるので、同じ強さで撒くと «跳んだ質量が
    //   床にぶつかった» が波の途中と区別できなくなる。ここだけ数と大きさを別に持つ。
    FBZZ_GROUP("Landing")
    FBZZ_FIELD_RANGE(float, landingBlast, 1.0f, "Blast", 0.0f, 1.0f)
    FBZZ_TOOLTIP("落下点の真ん中で出す爆発の強さ。0 で出さない")
    FBZZ_FIELD_RANGE_INT(int, landingBlastCount, 4, "Blast Ring", 0, 8)
    FBZZ_TOOLTIP("中心の爆発を囲んで足す数。1 発だけだと «点» で終わるので、"
                 "腹の下いっぱいが割れた大きさを、囲む数で作る")
    FBZZ_FIELD_RANGE(float, landingBlastSpread, 3.2f, "Blast Ring Radius", 0.5f, 12.0f)
    FBZZ_TOOLTIP("囲む爆発を置く半径 [m]。ボスの腹の差し渡しに合わせる")
    // WHY 16 → 12 か: 枠は 1 回の波ぶんの総数で決まる (ファイル頭の «枠の食い潰し»)。
    //     着地で 16 も使うと、走り出した後の輪へ回す枠が残らない。
    FBZZ_FIELD_RANGE_INT(int, landingSmokeCount, 10, "Smoke Points", 0, 32)
    FBZZ_TOOLTIP("落下点を囲む土煙の数。輪の «生まれた瞬間» を作る")
    FBZZ_FIELD_RANGE(float, landingSmokeScale, 1.9f, "煙のスケール", 0.2f, 4.0f)
    FBZZ_TOOLTIP("VfxManager の Ground Dust に掛ける倍率。波の途中より大きく置く")

    // WHY 着地の煙を腹の «縁» へ出すか (2026-09-11):
    //   以前は Start Radius (1.5m) と その 0.45 倍に置いていた ─ つまりボスの腹の
    //   真下で、カメラからは 6m 級の胴体が丸ごと隠す位置。爆発だけが Blast Ring
    //   Radius (3.2m) で外へ出ていて、煙は «出ているのに一度も見えない» 状態だった。
    //   シルエットの外へ出して初めて «着いた» が絵になる。

    // WHY «少しずつ撒き続ける» をやめて «輪を 1 枚ずつ置く» にするか:
    //   撒く点が 1 回 2〜3 個だと、半径は連続して伸びているので点は «回りながら外へ»
    //   並ぶ。1 枚の絵として見たときに残るのはその軌跡で、渦にしか見えない。
    //   同じ半径へ一斉に、円周へ均等に撒けば、1 回ぶんがそのまま «円» になる。
    //   段が外へ置かれていくので «段々と広がる» も同じ形から出る。
    //
    // WHY 点の数を半径で変えるか:
    //   固定数だと、生まれたての輪は点が密集して塊になり、広がりきった輪は 1 区間が
    //   10m を超えて «点が 8 個置いてある» にしか見えない。円周を一定の間隔で割れば、
    //   どの大きさでも同じ «輪» として読める。
    //
    // WHY 上限が要るか:
    //   円周は半径に比例して伸びるので、間隔を保つと外周では際限なく増える。枠の数
    //   (VfxManager の Ground Dust Slots) を «1 枚の点数 x 生きている輪の枚数» が
    //   超えた瞬間、古い輪が途中で消えて «内側から欠けていく円» になる。
    // WHY 輪を «時間» でなく «進んだ距離» で置くか (2026-09-11):
    //   時間で刻むと、速さが 2 倍違う出だしと外周で段の間隔も 2 倍変わる。旧設定では
    //   1 枚目 (r=1.5) と 2 枚目 (r=5.5) の間が 4m 空き、外周では 2.1m ─ 一番速くて
    //   一番危ない出だしが一番スカスカという逆転が起きていた。距離で刻めば段の間隔は
    //   どこでも一定で、速い所ほど «立て続けに» 置かれる。
    FBZZ_GROUP("Ring")
    FBZZ_FIELD_RANGE(float, ringStep, 2.0f, "段の間隔 [m]", 0.3f, 6.0f)
    FBZZ_TOOLTIP("波がこれだけ進むごとに輪を 1 枚置く。詰めるほど連続した壁に近づくが、"
                 "1 回の波で撒く総数が増えるので Ground Dust Slots と釣り合わせること")
    FBZZ_FIELD_RANGE(float, ringInterval, 0.10f, "最短間隔 [秒]", 0.02f, 1.0f)
    FBZZ_TOOLTIP("段の間隔がどれだけ詰まっても、これより短い周期では置かない。"
                 "出だしの速さで枠を一気に食い潰さないための床")
    FBZZ_FIELD_RANGE(float, ringSpacing, 4.0f, "Point Spacing", 1.0f, 12.0f)
    FBZZ_TOOLTIP("輪の上に置く点の間隔 [m]。土煙 1 発の広がり (Ground Dust の Puff Size) "
                 "より詰めると隙間が埋まる")
    FBZZ_FIELD_RANGE_INT(int, ringMinPoints, 5, "Min Points", 3, 24)
    // WHY 上限を 16 → 8 へ «下げる» か:
    //   増やしても密にはならない。土煙 1 発は最大 1.6 秒枠を占有し、波の全長とほぼ
    //   同じなので «1 回の波で撒いた総数 = 同時生存数»。枠 (96) を超えた瞬間、
    //   リングが最も古い枠を奪って作り直すので、どの煙も育ちきる前に消える ─
    //   点を増やすほど 1 発ずつが小さくなるという逆立ちが起きる。
    //   前縁の «線» は亀裂 (デカール) が引く。煙は数ではなく «育った大きさ» で効かせる。
    FBZZ_FIELD_RANGE_INT(int, ringMaxPoints, 8, "Max Points", 3, 32)
    FBZZ_TOOLTIP("1 枚あたりの上限。«Ground Dust Slots ÷ 1 回の波の輪の枚数» を"
                 "超えると、外周へ届く前に内側の輪が欠け始める")
    // WHY 0.4 → 0.25 か: 点が 16 個あった頃は散らしても «輪» が残ったが、8 個に
    //     絞った今は 1 点が前縁から 0.4m 引っ込むだけで線が読めなくなる。
    FBZZ_FIELD_RANGE(float, ringJitter, 0.25f, "Jitter", 0.0f, 3.0f)
    FBZZ_TOOLTIP("点を内側へ散らす幅 [m]。0 だと真円すぎて «描いた図形» に見える。"
                 "外側へは散らさない ─ 当たらない場所に絵が出ないため")
    FBZZ_FIELD_RANGE(float, smokeScale, 1.3f, "煙のスケール", 0.2f, 4.0f)
    FBZZ_TOOLTIP("点を減らしたぶん 1 発を大きくする。数ではなく大きさで輪を作る")
    FBZZ_FIELD_RANGE(float, smokeStrength, 1.0f, "Smoke Strength", 0.0f, 1.0f)
    FBZZ_TOOLTIP("蹴り出しの強さ。1 で Ground Dust の既定どおり押し出す")

    // WHY 全周へ均等に配らないか (2026-09-11):
    //   円周は半径に比例して伸びるので、均等に配ると r=15m では点の間隔が 5.9m に
    //   なる ─ プレイヤーの幅 1m に対してほぼ穴で、しかもその半分は «背後の、
    //   カメラに映らない側» に捨てている。跳ぶ時刻を読ませたいのは自分へ向かって
    //   くる縁だけなので、同じ枠をそこへ寄せる。Front Marker が 1 点だけでやっている
    //   判断 (当たる線にだけ絵を置く) を、輪と亀裂へ広げたもの。
    //
    //   WHY 残りを 0 にしないか: 全部を弧へ入れると «円» が消えて、自分の正面にだけ
    //       壁が湧く絵になる。«円が広がっている» は盤面の形そのものなので、
    //       薄くても残す ─ 遠くの縁は «形» が判ればよく、密度は要らない。
    FBZZ_GROUP("Front Arc")
    FBZZ_FIELD_RANGE(float, frontArcDegrees, 100.0f, "弧の幅 [度]", 20.0f, 360.0f)
    FBZZ_TOOLTIP("プレイヤーの方角を中心にこの角度ぶんへ点を寄せる。360 で全周均等")
    FBZZ_FIELD_RANGE(float, frontArcShare, 0.70f, "寄せる割合", 0.0f, 1.0f)
    FBZZ_TOOLTIP("輪の点のうち弧へ入れる割合。残りは «円の形» のために全周へ薄く配る")
    FBZZ_FIELD_RANGE(float, crackArcDegrees, 50.0f, "亀裂の弧 [度]", 10.0f, 360.0f)
    FBZZ_TOOLTIP("亀裂を寄せる角度。煙の弧より狭くする ─ 亀裂は «当たる線» そのものを"
                 "引く役で、幅を持たせると線ではなく面になる")
    FBZZ_FIELD_RANGE(float, crackArcShare, 0.75f, "亀裂を寄せる割合", 0.0f, 1.0f)

    // WHY 爆発を輪と同じ拍で出すか:
    //   煙だけだと «埃が舞っている» で、床が砕けた出来事にならない。ただし爆発を別の
    //   間隔で出すと、煙の輪とずれた半径で弾けて «輪の外側でも何か起きている» になり、
    //   円が 2 重にぼやける。同じ瞬間・同じ半径に置けば、爆発は輪の一部として読める。
    //
    // WHY 数を絞るか:
    //   1 発が焦げ跡まで含めて 4 秒あり、光源も伴う。撒きすぎると光が重なって
    //   赤青の区別が消え、Ground Blast Slots を使い切って途中で消える。
    FBZZ_GROUP("Blast")
    // WHY 2 → 1 か: 段を距離で刻むと 1 回の波の輪は 11 枚前後になる。2 発ずつだと
    //     22 発で、1 発 4 秒の爆発を Ground Blast Slots (20) で回しきれない ─
    //     煙と同じ «奪い合って全部が育たない» が爆発側でも起きる。
    FBZZ_FIELD_RANGE_INT(int, blastsPerRing, 1, "Per Ring", 0, 6)
    FBZZ_TOOLTIP("輪 1 枚に混ぜる爆発の数。0 で煙だけになる。点と点の «間» へ置く")
    FBZZ_FIELD_RANGE(float, blastStrength, 0.42f, "強度", 0.0f, 1.0f)
    FBZZ_TOOLTIP("上げると «激突» と同じ大きさで弾けるので、波の 1 点で起きる出来事"
                 "としては強すぎる。着地の Blast より必ず小さく置くこと")

    // WHY 輪と別に «前縁の目印» を出すか:
    //   輪は 0.2 秒ごとに置かれるので、輪と輪の間では当たる線より最大 2.2m 内側までしか
    //   絵が無い。跳ぶ時刻をそこから読むと «煙が来る前に食らった» が起きる。
    //   自分へ向かってくる縁にだけ、輪の合間も点を置き続けて線の «今» を示す。
    FBZZ_GROUP("Front Marker")
    FBZZ_FIELD_RANGE(float, frontInterval, 0.12f, "間隔", 0.0f, 1.0f)
    FBZZ_TOOLTIP("プレイヤーの方角へ点を置く間隔 [秒]。0 で出さない (輪だけになる)")
    // WHY 0.8 → 1.5 へ «上げる» か (2026-09-11): 一番読ませたい点 ─ 自分へ
    //     向かってくる縁 ─ が輪の点より小さいという逆立ちになっていた。«追ってくる塊»
    //     に見える心配は、置く角度を左右へ振ることで既に外してある (下の EmitFrontMarker)。
    FBZZ_FIELD_RANGE(float, frontScale, 1.5f, "スケール", 0.2f, 4.0f)
    FBZZ_TOOLTIP("輪の点より大きく置く。跳ぶ時刻はこの点から読まれる")

    // WHY 床へ跡を残すか:
    //   煙も爆発も «通り過ぎたら何も無かったことになる» 表現で、波が通った後の床は
    //   攻撃の前と同じ。跡が残って初めて «床を割って走った» になり、次に同じ攻撃が
    //   来たときも «さっきここを通った» が盤面から読める。
    //
    // WHY 消えるか (永久に残さないか):
    //   ボスは何度も跳ぶ。残し続けると床が数分で亀裂だらけになり、«今の 1 回» が
    //   どれか読めなくなる。跡は «直前に起きたこと» の表示なので、数秒で薄れる。
    FBZZ_GROUP("Cracks")
    FBZZ_FIELD_FILE(crackMaterial, "Assets/Materials/Decal/DecalCrack.mat",
                    "Material", ".mat")
    FBZZ_TOOLTIP("render_path = \"decal\" の .mat。手続きで割れを描く DecalCrack が既定")
    // WHY 亀裂が «前縁の線» を引く役になったか (2026-09-11):
    //   煙で連続した壁を作ることは枠の上できない ─ 1 発が 1.6 秒枠を占有し、波の
    //   全長がそれとほぼ同じなので、r=15m の円周 94m を埋める数はどうやっても入らない。
    //   デカールは VFX のリングを使わず、自前の 128 枠で回る。しかも置きたての 0.45 秒は
    //   溝の底が光る (Heat) ので、«今まさに割れている線» がそのまま前縁の表示になり、
    //   冷えた後は «通った跡» として残る。1 つの絵が予告と記録を兼ねる。
    FBZZ_FIELD_RANGE(float, crackStep, 1.25f, "段の間隔 [m]", 0.2f, 6.0f)
    FBZZ_TOOLTIP("波がこれだけ進むごとに亀裂を落とす。輪より細かく刻んで線を繋ぐ")
    FBZZ_FIELD_RANGE(float, crackInterval, 0.03f, "最短間隔 [秒]", 0.0f, 1.0f)
    FBZZ_TOOLTIP("段の間隔がどれだけ詰まっても、これより短い周期では落とさない。"
                 "跡ごと消したいときは Life を 0 にする")
    FBZZ_FIELD_RANGE_INT(int, crackPerBurst, 8, "Points / Burst", 1, 16)
    FBZZ_TOOLTIP("1 回に落とす枚数。Crack Arc へ寄せた上で «1 枚の差し渡し» より"
                 "間隔が狭くなる数にすると、床に 1 本の線として繋がる")
    FBZZ_FIELD_RANGE(float, crackSize, 2.2f, "サイズ", 0.2f, 10.0f)
    FBZZ_TOOLTIP("1 枚の差し渡し [m]。波の帯幅より少し大きいと «縁が割れた» に見える")
    FBZZ_FIELD_RANGE(float, crackGrowSeconds, 0.12f, "成長", 0.01f, 1.0f)
    FBZZ_TOOLTIP("中心から先端まで伸びきる秒数。長いと «描かれていく» に見える")
    FBZZ_FIELD_RANGE(float, crackLife, 3.5f, "Life", 0.2f, 20.0f)
    FBZZ_TOOLTIP("消えるまでの秒数。跡が «直前に起きたこと» で居られる長さ")
    FBZZ_FIELD_RANGE(float, crackHeatSeconds, 0.45f, "Heat", 0.0f, 3.0f)
    FBZZ_TOOLTIP("溝の底が光っている秒数。0 で最初から黒い溝だけになる")
    FBZZ_FIELD_RANGE(float, crackProjectionDepth, 1.2f, "Projection Depth", 0.2f, 8.0f)
    FBZZ_TOOLTIP("投影する厚み [m]。薄いと段差で跡が途切れ、厚いと壁にも回り込む")

    // WHY 越えた側にも返すか: 跳んで抜けた «成功» に何も返らないと、当たらなかったのが
    //     読み勝ちなのか、そもそも判定が無かったのかプレイヤーには区別できない。
    //     足の下を通ったことを手で返して初めて、跳ぶ操作が答えとして確定する。
    FBZZ_GROUP("手応え")
    FBZZ_FIELD_RANGE(float, passRumble, 0.45f, "Pass Rumble", 0.0f, 1.0f)
    FBZZ_TOOLTIP("波がプレイヤーの位置を通り過ぎた瞬間。当たった側は被弾側が返すので、"
                 "ここは «越えた» の合図として軽く置く")
    FBZZ_FIELD_RANGE(float, passShakeRatio, 0.5f, "揺れの比率", 0.0f, 1.0f)

    // WHY 光を 1 灯だけ足すか (2026-09-11):
    //   土煙は無彩色で、アリーナの床も暗い (取り込んだ .mat は emissiveScale = 0)。
    //   «煙が出ているのに見えない» の最後の一枚がこれで、絵を増やすより先に
    //   «見える条件» を作る方が効く。全周へ置くと光が重なって白く飛ぶうえ枠も食うので、
    //   プレイヤーへ向かってくる縁の 1 点だけを照らす ─ 読ませたいのはそこだけ。
    FBZZ_GROUP("Front Light")
    FBZZ_FIELD(bool, frontLight, true, "有効にする")
    FBZZ_FIELD_COLOR(frontLightColor, (Vector4{ 1.00f, 0.55f, 0.22f, 1.0f }), "色")
    FBZZ_TOOLTIP("割れた床の熱。刀の赤青を使うと «帯電している» と読み違えるので"
                 "亀裂の Heat と同じ暖色へ置く")
    FBZZ_FIELD_RANGE(float, frontLightIntensity, 16.0f, "強さ", 0.0f, 200.0f)
    FBZZ_FIELD_RANGE(float, frontLightRange, 9.0f, "範囲", 0.5f, 40.0f)
    FBZZ_FIELD_RANGE(float, frontLightHeight, 1.1f, "高さ [m]", 0.0f, 6.0f)
    FBZZ_TOOLTIP("床から浮かせる高さ。0 だと床面と同一平面で、立ち上がった煙に光が届かない")

    FBZZ_GROUP("デバッグ")
    FBZZ_FIELD_READ_ONLY(float, debugRadius, 0.0f, "半径")
    FBZZ_FIELD_READ_ONLY(bool, debugDealt, false, "Dealt")
    FBZZ_FIELD(bool, drawDebugBand, false, "Draw Band")
    FBZZ_TOOLTIP("当たる帯の内外を線で引く。煙は散らして置くので、絵と当たりが"
                 "どれだけずれているかはこれでしか確かめられない")

    /// 着地の瞬間に 1 度だけ呼ぶ。groundPoint は波が走る «床» の高さを兼ねる。
    void Emit(const Vector3& groundPoint);
    /// 走っている波を畳む。倒された・シーンを抜けた、など «無かったことにする» 側。
    void Cancel();

    [[nodiscard]] bool  IsActive() const { return m_active; }
    [[nodiscard]] float Radius()   const { return m_radius; }

    void OnStart()      override;
    void OnLateUpdate() override;
    void OnDisable()    override { Cancel(); }
    void OnDestroy()    override;

private:
    /// 落下点で 1 度だけ出す爆発と、それを囲む土煙。
    void EmitLanding();
    /// 今の半径へ «輪を 1 枚» 置く。煙も爆発も同じ 1 枚に乗る。
    void EmitRing(float dt);
    /// 輪の合間、プレイヤーへ向かってくる縁にだけ点を置く。
    void EmitFrontMarker(float dt);
    /// 前縁の床へ亀裂を落とす。
    void EmitCracks(float dt);
    /// 前縁のプレイヤー寄りへ点光源を置く。波が消えたら消灯する。
    void DriveFrontLight(bool lit);
    /// プレイヤーの居る方角 [rad]。居なければ false。
    [[nodiscard]] bool PlayerAngle(float& outAngle) const;
    /// 円周へ置く count 点の角度を out へ書く。プレイヤー側の弧へ share だけ寄せる。
    ///
    /// WHY 弧の «中» も等間隔にするか: 中心 (プレイヤーの真正面) を濃くすると、
    ///     どの角度から見ても «自分を狙って湧いている» になり、円が広がっている
    ///     出来事から «自分を追う何か» へ読み替わる。密度は弧の中では一定にする。
    /// @return 実際に書いた数。
    [[nodiscard]] int SpreadAngles(int count, float share, float arcDegrees, float phase,
                                   float* out, int capacity) const;
    /// 落とした亀裂を伸ばし、冷まし、薄れさせる。波が消えた後も続ける。
    void UpdateCracks(float dt);
    /// 帯に触れているプレイヤーへ 1 度だけ当てる。跳んで越えていれば当てない。
    void ResolveHit(GameObject* player);
    /// 波がプレイヤーの立っている半径を追い越した瞬間を 1 度だけ返す。
    void NotifyPass(const Vector3& playerPoint);
    /// ボスの極性。消灯中 (激突スタン) でも無極の爆発にはしない。
    [[nodiscard]] BladeSide WaveSide() const;
    /// 波の上の 1 点。angle は中心から見た方角、inset は前縁から内側へ引く距離 [m]。
    [[nodiscard]] Vector3 PointOnFront(float angle, float inset) const;
    /// 水平距離。高さは «越えたか» の判定にしか使わないので、帯の測りには入れない。
    [[nodiscard]] static float PlanarDistance(const Vector3& a, const Vector3& b);

    /// 床に残す亀裂 1 枚。
    ///
    /// WHY 使い回すか (毎回作って消さないか): 1 回の波で数十枚落ちる。作っては
    ///     消すと GameObject 配列が波のたびに伸び縮みし、握っているポインタが
    ///     無効になる箇所が増える。枠を先に決めて一番古いものから上書きする。
    struct Crack {
        EntityRef ref;
        float     age = -1.0f;   ///< 負なら未使用
    };
    /// 同時に床へ残せる枚数。0.09 秒 x 3 枚で 1 秒あたり約 33 枚、寿命 3.5 秒だと
    /// 波 1 回で 60 枚前後になる ─ そこで頭打ちにして古い側から消える。
    /// WHY 64 → 128 か (2026-09-11): 亀裂が前縁の線を引く役になり、1 回の波で
    ///     落とす枚数が 15 段 x 8 枚 = 120 前後になった。64 のままだと波の途中で
    ///     内側から消え始め、«割れて走った» が «割れているのは今の所だけ» になる。
    static constexpr int kMaxCracks = 128;
    Crack m_cracks[kMaxCracks];
    int   m_crackNext  = 0;
    float m_crackTimer = 0.0f;
    float m_crackPhase = 0.0f;

    /// 前縁の光。波と一緒に生まれず、初回に作って以後は消灯で寝かせる。
    EntityRef m_frontLightRef;

    Vector3 m_center = Vector3::ZERO;
    /// 最短間隔の残り [秒]。距離の刻みと «どちらも満ちたら» 置く。
    float   m_ringTimer = 0.0f;
    /// 最後に輪 / 亀裂を置いた半径 [m]。段の間隔は時間ではなくここからの差で決める。
    float   m_ringRadius  = 0.0f;
    float   m_crackRadius = 0.0f;
    /// 輪の «0 番目の点» を置く角度。1 枚ごとに回さないと、輪と輪の点が同じ方角へ
    /// 揃って «円周に並んだ何本かの筋» に見える。
    float   m_ringPhase = 0.0f;
    float   m_frontTimer = 0.0f;
    float   m_radius = 0.0f;
    bool    m_active = false;
    /// この波で 1 度でも当てたか。1 回の着地でダメージは 1 回だけ。
    bool    m_dealt  = false;
    /// プレイヤーの位置を追い越したか。越えた合図は 1 度だけ返す。
    bool    m_passed = false;
    bool    m_warnedNoCombat = false;
};

FBZZ_REFLECT(BossShockwaveComponent)


inline float BossShockwaveComponent::PlanarDistance(const Vector3& a, const Vector3& b)
{
    const float dx = a.x - b.x;
    const float dz = a.z - b.z;
    return std::sqrt(dx * dx + dz * dz);
}

inline Vector3 BossShockwaveComponent::PointOnFront(float angle, float inset) const
{
    const float radius = std::max(m_radius - inset, 0.05f);
    return { m_center.x + std::cos(angle) * radius,
             m_center.y,
             m_center.z + std::sin(angle) * radius };
}

inline bool BossShockwaveComponent::PlayerAngle(float& outAngle) const
{
    GameObject* player = scene.FindWithTag(playerTag);
    if (!player) return false;
    const Vector3 to = player->transform.worldPosition - m_center;
    if (std::fabs(to.x) <= EPSILON && std::fabs(to.z) <= EPSILON) return false;
    outAngle = std::atan2(to.z, to.x);
    return true;
}

inline int BossShockwaveComponent::SpreadAngles(int count, float share, float arcDegrees,
                                                float phase, float* out, int capacity) const
{
    const int total = std::clamp(count, 0, capacity);
    if (total <= 0) return 0;

    const float arc = ToRad(std::clamp(arcDegrees, 10.0f, 360.0f));
    float centre = 0.0f;
    // プレイヤーが居ない (死亡・シーン切替) 間は寄せる先が無い。全周等間隔へ戻す。
    const bool aimed = Clamp01(share) > 0.0f && arc < TWO_PI - EPSILON && PlayerAngle(centre);
    if (!aimed) {
        const float step = TWO_PI / static_cast<float>(total);
        for (int i = 0; i < total; ++i) out[i] = phase + step * static_cast<float>(i);
        return total;
    }

    // WHY 変数名に near / far を使わないか: Windows SDK (minwindef.h) が両方を
    //     空のマクロとして定義しているため、宣言がそのまま消えてコンパイルが通らない。
    const int inArc = std::clamp(
        static_cast<int>(std::lround(static_cast<float>(total) * Clamp01(share))), 1, total);
    const int outside = total - inArc;

    // 弧の中。両端に点を置くと «弧の切れ目» が縦線として見えるので半区間ぶん内側から。
    const float arcStep = arc / static_cast<float>(inArc);
    // WHY 送りを区間の中へ折り返すか: 段どうしで点の角度が揃うと、輪をまたいで
    //     «円周に並んだ何本かの筋» に見える。かといって送りをそのまま足すと弧ごと
    //     回ってしまい、寄せた先がプレイヤーから外れる。中でだけ回す。
    const float arcPhase = std::fmod(std::fabs(phase), arcStep);
    for (int i = 0; i < inArc; ++i)
        out[i] = centre - arc * 0.5f + arcPhase + arcStep * (static_cast<float>(i) + 0.5f);

    // 弧の外。«円が広がっている» の形だけを保てばよいので、薄く均等に。
    if (outside > 0) {
        const float step = (TWO_PI - arc) / static_cast<float>(outside);
        for (int j = 0; j < outside; ++j)
            out[inArc + j] = centre + arc * 0.5f + step * (static_cast<float>(j) + 0.5f);
    }
    return total;
}

inline BladeSide BossShockwaveComponent::WaveSide() const
{
    // ボスは色を切り替えない。波の色は 1 本に固定する。
    return BladeSide::Right;
}

inline void BossShockwaveComponent::OnStart()
{
    m_active  = false;
    m_dealt   = false;
    m_passed  = false;
    m_radius  = 0.0f;
    m_warnedNoCombat = false;
    debugRadius = 0.0f;
    debugDealt  = false;

    // 波は盤面の «場所» で起きる出来事なので 3D。2D にすると、アリーナの反対側で
    // 起きた着地も自分の足元と同じ音量で鳴り、どこへ跳べばよいのか判らなくなる。
    se::EnsureSource(scene, "SE", 1.0f);
}

inline void BossShockwaveComponent::OnDestroy()
{
    // 跡は波より長く残る作りなので、畳み忘れると床に焼き付く。ルートに置いた以上、
    // ボスと一緒には消えない。持ち主が畳む。
    for (Crack& crack : m_cracks) {
        if (GameObject* object = crack.ref.Resolve(scene)) scene.Destroy(*object);
        crack = {};
    }
    // 破棄では OnDisable が呼ばれない (Scene::Destroy は OnDestroy だけを回す) ので、
    // 波の途中で消えると前縁の光が点いたまま床に残る。
    if (GameObject* light = m_frontLightRef.Resolve(scene)) scene.Destroy(*light);
    m_frontLightRef = {};
}

inline void BossShockwaveComponent::Emit(const Vector3& groundPoint)
{
    m_center  = groundPoint;
    m_radius  = std::max(startRadius, 0.0f);
    m_active  = true;
    m_dealt   = false;
    m_passed  = false;
    debugDealt = false;
    // 1 枚目は着地の輪そのもの。遅らせると «着いてから少しして床が広がる» になる。
    m_ringTimer  = 0.0f;
    m_frontTimer = 0.0f;
    m_crackTimer = 0.0f;
    // 段の起点を 1 段ぶん手前へ置く。«生まれた半径から進んだら» にすると、着地の
    // 1 枚目が 1 段ぶん遅れて出て «着いてから少しして床が広がる» に戻ってしまう。
    m_ringRadius  = m_radius - std::max(ringStep, 0.1f);
    m_crackRadius = m_radius - std::max(crackStep, 0.1f);

    EmitLanding();

    // WHY ボス本体ではなく着地点で鳴らすか: 大ジャンプの着地点は «プレイヤーの近く»
    //     なので、胴体の原点で鳴らしても大差ないように見える。だが波が走り出す場所は
    //     床であって腹ではない。床で鳴らしておけば、跳ぶかどうかを決める耳の手がかりと
    //     煙の出どころが一致する。
    se::PlayAt(audio, se::kImpactHeavy, groundPoint);
}

inline void BossShockwaveComponent::Cancel()
{
    m_active = false;
    m_radius = 0.0f;
    debugRadius = 0.0f;
    // 光は «作り直さず消灯»。破棄すると次の着地で作り直しになり、跳ぶたびに
    // ライトの実体がシーンから出入りする (斉射の線と同じ扱い)。
    DriveFrontLight(false);
}

inline void BossShockwaveComponent::EmitLanding()
{
    auto* vfx = VfxManagerComponent::Instance();
    if (!vfx) return;

    const BladeSide side = WaveSide();

    if (landingBlast > 0.0f) {
        // 爆発は落下点そのもの。少しでも浮かせると床から切り離されて «空中で爆ぜた»
        // ように見え、この後に走り出す煙と繋がらない。
        vfx->PlayGroundBlast(m_center, side, Clamp01(landingBlast));

        // 中心の 1 発を囲む。腹の下いっぱいが割れた «広さ» は、1 発を大きくしても
        // 出ない ─ 離れた場所で同時に起きて初めて、割れた面積として読める。
        const int ring = std::clamp(landingBlastCount, 0, 8);
        if (ring > 0) {
            const float step = TWO_PI / static_cast<float>(ring);
            m_ringPhase += 0.618f * step + 0.73f;
            for (int i = 0; i < ring; ++i) {
                const float angle = m_ringPhase + step * static_cast<float>(i);
                const float radius = std::max(landingBlastSpread, 0.5f);
                vfx->PlayGroundBlast(Vector3{ m_center.x + std::cos(angle) * radius, m_center.y,
                                              m_center.z + std::sin(angle) * radius },
                                     side, Clamp01(landingBlast) * 0.7f);
            }
        }
    }

    const int count = std::clamp(landingSmokeCount, 0, 32);
    if (count <= 0) return;

    const float step = TWO_PI / static_cast<float>(count);
    // 起点をずらしておく。0 度から並べると、着地のたびに同じ方角へ同じ煙が立つ。
    m_ringPhase += 0.618f * step + 0.41f;

    for (int i = 0; i < count; ++i) {
        const float angle = m_ringPhase + step * static_cast<float>(i);
        const float cx = std::cos(angle);
        const float sz = std::sin(angle);
        // 腹の «縁» へ置く。基準は爆発と同じ Blast Ring Radius ─ 落下点の真上には
        // 6m 級の胴体が載っているので、Start Radius (腹の下) に置いた煙は 1 度も
        // 画面に出ない。1 つおきに内側へ落とすのは «円周に置いた印» にしないため。
        const float belly  = std::max(landingBlastSpread, std::max(startRadius, 0.1f));
        const float radius = belly * ((i % 2 == 0) ? 1.0f : 0.62f);
        const Vector3 point{ m_center.x + cx * radius, m_center.y, m_center.z + sz * radius };
        vfx->PlayGroundDust(point, Vector3{ cx, 0.0f, sz }, 1.0f,
                            std::max(landingSmokeScale, 0.2f) * ((i % 2 == 0) ? 1.0f : 1.25f));
    }
}

inline void BossShockwaveComponent::EmitRing(float dt)
{
    // 距離と時間の «どちらも» 満ちたら 1 枚。距離だけだと出だしの 16m/s で
    // 1 フレームに 2 枚置く回ができ、時間だけだと段の間隔が速さで伸び縮みする。
    m_ringTimer = std::max(m_ringTimer - dt, 0.0f);
    if (m_ringTimer > 0.0f) return;
    if (m_radius - m_ringRadius < std::max(ringStep, 0.1f)) return;
    m_ringTimer  = std::max(ringInterval, 0.02f);
    m_ringRadius = m_radius;

    auto* vfx = VfxManagerComponent::Instance();
    if (!vfx) return;

    // 円周を Point Spacing で割った数。半径が伸びれば点も増えるので、どの大きさでも
    // 同じ «輪» の見え方になる。上限の意味は Max Points の WHY を参照 (枠の総量)。
    const int lower = std::clamp(ringMinPoints, 3, 32);
    const int upper = std::clamp(ringMaxPoints, lower, 32);
    const int count = std::clamp(
        static_cast<int>(TWO_PI * std::max(m_radius, 0.1f) / std::max(ringSpacing, 0.5f)),
        lower, upper);

    // 1 枚ごとに起点を回す。同じ角度から並べると、輪をまたいで点が放射状に揃い、
    // 円ではなく «車輪のスポーク» として読まれる。
    m_ringPhase += 0.618f * (TWO_PI / static_cast<float>(count)) + 0.21f;

    float angles[32];
    const int placed = SpreadAngles(count, frontArcShare, frontArcDegrees, m_ringPhase,
                                    angles, 32);

    for (int i = 0; i < placed; ++i) {
        const float angle = angles[i];
        // 散らしは «通り過ぎた側» にしか出さない。外側へ散らすと、当たらない場所に
        // 煙が立って «見た目どおりに当たる» が破れる。
        const float jitter = std::max(ringJitter, 0.0f) *
                             std::fabs(std::sin(angle * 4.7f + m_ringPhase * 2.3f));
        const Vector3 point = PointOnFront(angle, std::max(bandWidth, 0.0f) * 0.5f + jitter);
        // 煙が流れる向きは波の進む向き = 外向き。
        vfx->PlayGroundDust(point, Vector3{ std::cos(angle), 0.0f, std::sin(angle) },
                            Clamp01(smokeStrength), std::max(smokeScale, 0.2f));
    }

    const int blasts = std::clamp(blastsPerRing, 0, 6);
    if (blasts <= 0 || blastStrength <= 0.0f || placed <= 0) return;

    // 爆発も «寄せた側» へ置く。全周へ均等に置くと、煙が濃い正面と光る場所がずれて
    // 「どこで何が起きたか」が 2 か所に割れる。弧の点の «間» を借りる。
    const BladeSide side = WaveSide();
    const int step = std::max(placed / blasts, 1);
    for (int i = 0; i < blasts; ++i) {
        const int index = std::min(i * step, placed - 1);
        const float angle = angles[index] + 0.5f * (TWO_PI / static_cast<float>(count));
        vfx->PlayGroundBlast(PointOnFront(angle, 0.0f), side, Clamp01(blastStrength));
    }
}

inline void BossShockwaveComponent::EmitFrontMarker(float dt)
{
    if (frontInterval <= 0.0f) return;

    m_frontTimer -= dt;
    if (m_frontTimer > 0.0f) return;
    m_frontTimer = std::max(frontInterval, 0.02f);

    GameObject* player = scene.FindWithTag(playerTag);
    if (!player) return;

    const Vector3 to = player->transform.worldPosition - m_center;
    if (std::fabs(to.x) <= EPSILON && std::fabs(to.z) <= EPSILON) return;

    auto* vfx = VfxManagerComponent::Instance();
    if (!vfx) return;

    // 真正面へ置き続けると «自分を追ってくる 1 つの煙» に見える。左右へ振って、
    // 向かってくる縁の «一部» として読ませる。振り幅の種を半径から取るのは、
    // 半径だけが 1 発ごとに必ず変わっているため。輪の点も同じ弧へ寄せてあるので、
    // この 1 点はその弧のうち «今いちばん近い所» として読まれる。
    const float angle = std::atan2(to.z, to.x) + std::sin(m_radius * 5.3f) * 0.22f;
    const Vector3 point = PointOnFront(angle, std::max(bandWidth, 0.0f) * 0.5f);
    vfx->PlayGroundDust(point, Vector3{ std::cos(angle), 0.0f, std::sin(angle) },
                        Clamp01(smokeStrength), std::max(frontScale, 0.2f));
}

inline void BossShockwaveComponent::EmitCracks(float dt)
{
    if (crackInterval < 0.0f || crackLife <= 0.0f) return;

    m_crackTimer = std::max(m_crackTimer - dt, 0.0f);
    if (m_crackTimer > 0.0f) return;
    if (m_radius - m_crackRadius < std::max(crackStep, 0.1f)) return;
    m_crackTimer  = std::max(crackInterval, 0.0f);
    m_crackRadius = m_radius;

    const int count = std::clamp(crackPerBurst, 1, 16);
    m_crackPhase += 0.618f * (TWO_PI / static_cast<float>(count)) + 0.37f;

    float angles[16];
    const int placed = SpreadAngles(count, crackArcShare, crackArcDegrees, m_crackPhase,
                                    angles, 16);

    for (int i = 0; i < placed; ++i) {
        const float angle = angles[i];
        // 亀裂は «割れた床» なので前縁のわずかに内側。前縁ちょうどに置くと、
        // まだ何も通っていない床が先に割れている絵になる。
        const Vector3 point = PointOnFront(angle, std::max(bandWidth, 0.0f) * 0.35f);

        const int slot = m_crackNext;
        Crack&    crack = m_cracks[slot];
        m_crackNext = (m_crackNext + 1) % kMaxCracks;

        // WHY 名前で拾い直すか: スクリプト DLL をリロードするとこの Script は
        //     作り直されて EntityRef が空に戻るが、跡の GameObject は Scene 側に残る。
        GameObject* object = crack.ref.Resolve(scene);
        if (!object) {
            GameObject* owner = scene.Self();
            const std::string name = "FX_BossCrack_" +
                                     (owner ? owner->instanceId : std::string{}) + "_" +
                                     std::to_string(slot);
            object = scene.Find(name);
            if (!object) {
                GameObject& created = scene.Create(name);
                created.runtimeGenerated = true;
                object = &created;
            }
            crack.ref = EntityRef{ object->GetID() };
            object = crack.ref.Resolve(scene);
            if (!object) continue;
        }

        auto* decal = object->GetComponent<DecalComponent>();
        if (!decal) decal = &object->AddComponent<DecalComponent>();
        decal->materialPath = crackMaterial;

        // 投影軸はローカル +Y。床へ落とすだけなので姿勢は無回転でよい。
        // 大きさだけ 1 枚ごとに散らす ─ 同じ差し渡しが並ぶと «同じ判子» に見える。
        const float size = std::max(crackSize, 0.1f) *
                           (0.75f + 0.5f * std::fabs(std::sin(angle * 5.7f + m_crackPhase)));
        object->transform.position      = point;
        object->transform.worldPosition = point;
        object->transform.rotation      = Quaternion::Identity();
        object->transform.worldRotation = Quaternion::Identity();
        object->transform.scale = { size, std::max(crackProjectionDepth, 0.2f), size };

        // 割れ方の種。角度から作るので、同じ場所を 2 回叩けば同じ割れ方になる。
        decal->materialParamOverrides["seed"] =
            { std::fabs(std::sin(angle * 12.9898f + m_crackPhase)) };
        crack.age = 0.0f;
        object->SetActive(true);
    }
}

inline void BossShockwaveComponent::DriveFrontLight(bool lit)
{
    GameObject* object = m_frontLightRef.Resolve(scene);
    if (!object) {
        if (!lit || !frontLight) return;
        // WHY 名前で拾い直すか: 亀裂と同じ ─ DLL リロードで EntityRef だけが空に戻る。
        GameObject* owner = scene.Self();
        const std::string name = "FX_BossShockLight_" +
                                 (owner ? owner->instanceId : std::string{});
        object = scene.Find(name);
        if (!object) {
            GameObject& created = scene.Create(name);
            created.runtimeGenerated = true;
            object = &created;
        }
        m_frontLightRef = EntityRef{ object->GetID() };
        object = m_frontLightRef.Resolve(scene);
        if (!object) return;
    }

    auto* light = object->GetComponent<LightComponent>();
    if (!light) light = &object->AddComponent<LightComponent>();

    float angle = 0.0f;
    light->enabled = lit && frontLight && PlayerAngle(angle);
    if (!light->enabled) return;

    // 床から少し浮かせる。床面と同一平面に置くと、立ち上がった煙の «腹» にしか
    // 光が当たらず、輪郭を出すという目的に届かない。
    Vector3 point = PointOnFront(angle, 0.0f);
    point.y += std::max(frontLightHeight, 0.0f);
    object->transform.position      = point;
    object->transform.worldPosition = point;

    // 生まれた瞬間と消えぎわは絞る。全区間で同じ明るさだと «ずっと点いている光» で、
    // 波が走っていることの説明にならない。
    const float travelled = std::max(maxRadius - startRadius, EPSILON);
    const float progress  = Clamp01((m_radius - startRadius) / travelled);
    const float fade      = std::min(1.0f, (1.0f - progress) * 3.0f);

    light->type        = LightComponent::Type::Point;
    light->color       = { frontLightColor.x, frontLightColor.y, frontLightColor.z };
    light->intensity   = std::max(frontLightIntensity, 0.0f) * fade;
    light->range       = std::max(frontLightRange, 0.1f);
    light->castShadows = false;
}

inline void BossShockwaveComponent::UpdateCracks(float dt)
{
    for (Crack& crack : m_cracks) {
        if (crack.age < 0.0f) continue;

        crack.age += dt;
        GameObject* object = crack.ref.Resolve(scene);
        if (!object) { crack.age = -1.0f; continue; }

        const float life = std::max(crackLife, 0.05f);
        if (crack.age >= life) {
            crack.age = -1.0f;
            object->SetActive(false);
            continue;
        }

        auto* decal = object->GetComponent<DecalComponent>();
        if (!decal) continue;

        // 伸びる → 光が引く → 薄れて消える、の 3 つを別々の時定数で持つ。
        // 1 つの進みで兼ねると «伸びながら薄れる» になって、割れた瞬間が出ない。
        const float grow = Clamp01(crack.age / std::max(crackGrowSeconds, 0.01f));
        const float heat = crackHeatSeconds <= 0.0f
                         ? 0.0f
                         : Clamp01(1.0f - crack.age / crackHeatSeconds);
        // 消えは最後の 1/3 だけ。ずっと薄れ続けると «最初から薄い» に見える。
        const float left = Clamp01((life - crack.age) / std::max(life * 0.33f, 0.05f));

        decal->materialParamOverrides["growth"] = { grow };
        decal->materialParamOverrides["heat"]   = { heat * heat };
        // WHY 色ではなく opacity を書くか: crackColor は .mat が持つ «この亀裂は
        //     どういう色か» の正本で、濃さを毎フレーム書き潰すと .mat 側で色を
        //     変えても消える。投影側の倍率なら材質の中身を知らずに掛けられる。
        decal->opacity = left;
    }
}

inline void BossShockwaveComponent::ResolveHit(GameObject* player)
{
    if (m_dealt || damage <= 0) return;

    const float distance = PlanarDistance(player->transform.worldPosition, m_center);
    const float reach    = std::max(bandWidth, 0.0f) + std::max(playerRadius, 0.0f);
    if (std::fabs(distance - m_radius) > reach) return;

    // 跳んで越えているか。足元の高さだけで決める (ヘッダー冒頭の WHY)。
    const float height = player->transform.worldPosition.y - m_center.y;
    if (height >= std::max(clearHeight, 0.0f)) return;

    m_dealt = true;
    debugDealt = true;

    auto* combat = CombatManagerComponent::Instance();
    if (!combat) {
        if (!m_warnedNoCombat) {
            m_warnedNoCombat = true;
            debug.LogError("BossShockwaveComponent found no CombatManagerComponent in the scene. "
                           "The landing shockwave deals no damage.");
        }
        return;
    }
    // 押しは波の中心から外へ。輪から «押し出された» が読める向き。
    (void)combat->DamagePlayer(player, damage, &m_center);
}

inline void BossShockwaveComponent::NotifyPass(const Vector3& playerPoint)
{
    if (m_passed) return;
    if (m_radius < PlanarDistance(playerPoint, m_center)) return;
    m_passed = true;

    // 通り過ぎた «場所» はプレイヤーの足元そのもの。減衰の中心を着地点にすると、
    // 遠くで跳んで越えたときだけ何も返らず、成功の合図が距離で消える。
    const float strength = Clamp01(passRumble);
    if (strength <= 0.0f) return;
    if (auto* pad = RumbleManagerComponent::Instance()) pad->Rumble(strength);
    if (auto* shake = CameraShakeManagerComponent::Instance())
        shake->Shake(strength * Clamp01(passShakeRatio));
}

inline void BossShockwaveComponent::OnLateUpdate()
{
    // WHY 実時間ではなくゲーム時間で広げるか: 波は盤面の出来事で、避けるための
    //     «時刻» がヒットストップ中も進むと、止まっている画面の中で判定だけが動く。
    const float dt = std::max(Time::deltaTime, 0.0f);

    // 亀裂は波より長く残る。«波が終わったら跡も消える» では跡にならないので、
    // ここだけは m_active に関わらず進める。
    UpdateCracks(dt);

    if (!m_active) return;

    // 生まれた直後だけ速い。倍率は 1 以上にしかならないので、Speed が保証している
    // «走って逃げ切れない» 下限は崩れない。
    const float travelled = std::max(maxRadius - startRadius, EPSILON);
    const float progress  = Clamp01((m_radius - startRadius) / travelled);
    const float boost     = 1.0f + std::max(burstMultiplier - 1.0f, 0.0f) *
                                   std::exp(-progress * std::max(burstFalloff, 0.01f));

    m_radius += std::max(speed, 0.0f) * boost * dt;
    debugRadius = m_radius;

    if (m_radius >= std::max(maxRadius, startRadius)) {
        Cancel();
        return;
    }

    EmitRing(dt);
    EmitFrontMarker(dt);
    EmitCracks(dt);
    DriveFrontLight(true);

    if (GameObject* player = scene.FindWithTag(playerTag)) {
        ResolveHit(player);
        NotifyPass(player->transform.worldPosition);
    }

    // WHY 帯を別に引くか: 煙は 1 発ごとに散らして置くので、見えている煙の縁と当たる
    //     線は一致しない。重ねて初めて «どれだけ外へ出て見えているか» が判る。
    if (drawDebugBand) {
        const Vector4 edge{ 1.0f, 0.85f, 0.2f, 1.0f };
        constexpr int kBandSegments = 64;
        constexpr float kStep = TWO_PI / static_cast<float>(kBandSegments);
        for (const float radius : { std::max(m_radius - bandWidth, 0.05f), m_radius + bandWidth }) {
            for (int i = 0; i < kBandSegments; ++i) {
                const float a = kStep * static_cast<float>(i);
                const float b = kStep * static_cast<float>(i + 1);
                debug.DrawLine({ m_center.x + std::cos(a) * radius, m_center.y + 0.05f,
                                 m_center.z + std::sin(a) * radius },
                               { m_center.x + std::cos(b) * radius, m_center.y + 0.05f,
                                 m_center.z + std::sin(b) * radius }, edge);
            }
        }
    }
}

} // namespace sandbox
