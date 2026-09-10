/// @file    BladeTuning.hpp
/// @brief   双剣と盤面の調整値をまとめた共有データアセット (.fzdata)
/// @author  Hasegawa Jin
/// @date    2026-08-22
///
/// WHY DataAsset にするか:
///   斬撃の間合い・発生・連撃・溜めが敵それぞれのスクリプトに散っていると、
///   «届くのに当たらない» の原因を目視で確かめる方法が無くなる。1 枚のアセットへ並べる。
#pragma once

#include <Engine/Asset/DataAsset.hpp>

namespace sandbox {

class BladeTuning : public fbzz::DataAsset {
    FBZZ_DATA_ASSET(BladeTuning)
public:
    FBZZ_GROUP("Blades")
    // 斬撃が «届く» を決める 3 つ。近接の «当たらない» はほぼ全部ここから出る。
    FBZZ_FIELD_RANGE(float, bladeRange, 2.6f, "範囲", 0.5f, 10.0f)
    FBZZ_TOOLTIP("斬撃が届く距離 [m]。プレイヤー全高 2.51m。自分の背丈ぶんが素直に読める")
    FBZZ_FIELD_RANGE(float, bladeAngleDegrees, 120.0f, "Angle", 20.0f, 360.0f)
    FBZZ_TOOLTIP("正面から左右へ何度まで当たるか (合計)。狭いと当たらず、"
                 "広いと向きを決める意味が消える")
    FBZZ_FIELD_RANGE_INT(int, bladeDamage, 25, "ダメージ", 0, 500)
    FBZZ_TOOLTIP("1 斬りのダメージ。崩しゲージを持つボスには入らない (溜まるのは崩しだけ) ─ "
                 "効くのは BossBreakComponent が付いていない盤面だけ")
    // WHY 発生を最初に触る値と決めておくか: 近接の «当たらない» はほぼ全部ここが原因で、
    //     射程や角度を広げても直らない。振ってから判定が出るまでが遅いと、
    //     目で見えている間合いと実際に当たる間合いがずれる。
    // WHY 0.17 か: Katana_Slash_R (17F) の斬り抜けは f8 (0.267 秒) なので再生は 1.57 倍。
    //     0.08 まで縮めると 3.3 倍になり、振りかぶりが 1 コマも見えない
    //     «腕が切り替わるだけ» の絵になる。1.6 倍が «振りが読めて、なお軽い» 上限。
    //
    // WHY 1 段目の発生が連撃全段のテンポになるか: 5 連のクリップは 17F から 33F まで
    //     長さが倍近く違う。全段を同じ発生で出すと長いクリップだけ 2 倍を超えて
    //     再生される。1 段目が決めた «振りの速さ» を全段で共有し、各段の発生は
    //     そのクリップの斬り抜けの時刻から逆算する (BladeComponent)。
    //     結果、ここを縮めれば 5 段まとめて速くなる。
    FBZZ_FIELD_RANGE(float, bladeStartup, 0.17f, "Startup", 0.0f, 0.5f)
    FBZZ_TOOLTIP("1 段目の振り始めから判定が出るまで [秒]。手触りが決まらないときは最初にここ。"
                 "連撃全段の再生速度がここから導出されるので、"
                 "縮めれば 5 段まとめて同じだけ速くなる")
    // WHY 最終段だけ別に持つか: 締めは «止めた» ことが手に返る段で、そこだけ
    //     振りかぶりが長い。途中の段と同じテンポにすると、最も大きい一撃が
    //     最も軽い絵になる。ノックバックを別に持っているのと同じ理由。
    FBZZ_FIELD_RANGE(float, bladeFinisherStartup, 0.38f, "Startup (finisher)", 0.0f, 0.8f)
    FBZZ_TOOLTIP("最終段の発生 [秒]。長いほど溜めて見えるが、遅すぎると差し込まれる")
    FBZZ_FIELD_RANGE(float, bladeRecovery, 0.26f, "Recovery", 0.02f, 1.0f)
    FBZZ_TOOLTIP("単発で止めたときの硬直。連撃中との «差» が繋がっている感触の正体")
    // WHY 5 連にして縮めたか: 段の間の硬直は 1 回ぶんの «待ち» ではなく、繋ぐ回数だけ
    //     積み上がる。3 連の 0.14 秒は 2 回で 0.28 秒だったが、5 連では 4 回になり、
    //     そのままでは振っている時間より «次を待っている時間» の方が伸びる。
    //     入力は先行入力で預かられる (m_buffered) ので、短くしても «押した通りに出る»
    //     ことは崩れない。
    FBZZ_FIELD_RANGE(float, bladeComboRecovery, 0.08f, "Recovery (combo)", 0.02f, 1.0f)
    FBZZ_FIELD_RANGE(float, bladeComboWindow, 0.45f, "Combo Window", 0.05f, 2.0f)
    FBZZ_TOOLTIP("次の入力を受け付ける猶予。長いと押しっぱなしで繋がる")
    // 段ごとのクリップは BladeComponent が持つ (1 袈裟 / 2 双斬り / 3 返し /
    // 4 回転 / 5 回転上げ)。ここを 5 未満にすると手前から順に使われ、最後の段が
    // 必ず締め (回転上げ) になる。6 以上にすると 4 段目の回転斬りが繰り返される。
    FBZZ_FIELD_RANGE_INT(int, bladeComboLength, 5, "Combo Length", 1, 8)
    FBZZ_TOOLTIP("連撃の段数。クリップは 5 段ぶん用意してある。"
                 "6 以上にすると絵が足りず 4 段目が繰り返される")
    // WHY ノックバックを持たないか:
    //   斬撃で相手を飛ばせると、間合いの管理が «斬って散らす» 一手で済んでしまう。
    //   崩して仕留めるという芯が «遠回り» に落ちるので、刃は削るだけに留める。

    // WHY 吸い付きを調整値として持つか:
    //   «斬りたい方向» を決めているのはプレイヤーのカメラで、ロック対象へ向きを寄せるのは
    //   あくまで補助。どれだけ寄せるかは手触りの好みそのものなので、0 (完全に手動) から
    //   1 (必ず相手を向く) まで連続で選べる 1 つの値にする。
    FBZZ_FIELD_RANGE(float, bladeAimAssist, 0.45f, "Aim Assist", 0.0f, 1.0f)
    FBZZ_TOOLTIP("斬る向きを当て先へどれだけ寄せるか。0 = カメラの正面そのまま (補正なし)、"
                 "1 = 必ず当て先を向く")

    // WHY «中心» ではなく部位を当て先にするか:
    //   吸い付きと踏み込みが寄せていたのはロック対象の worldPosition ─ ボスの腹の
    //   中心だった。射程 2.6m に対して中心はそこから 3m 以上先にあるので、脚の横に
    //   立って振っても向きが腹へ引っぱられ、踏み込みは «届いている» と判断されて
    //   一度も出なかった。倒し方が «脚を 4 本もぐ» ことである以上、寄せる相手は
    //   «今そこに立っている脚» でなければ、吸い付きは補助ではなく妨害になる。
    //
    // WHY 距離ではなく «画面の奥» で選ぶか: 近さだけで選ぶと腹の下で見ていない側の
    //   脚へ吸い付く。どの脚を削るかはこのゲームの判断そのものなので、
    //   選ぶ主体はカメラ (プレイヤー) 側に置き、近さは同点のときの決め手に留める。
    FBZZ_FIELD_RANGE(float, bladeAimPartRange, 7.0f, "Aim Part Range", 0.0f, 20.0f)
    FBZZ_TOOLTIP("吸い付きと踏み込みが «脚・コア» を当て先にする捕捉距離。"
                 "0 にすると従来どおり相手の中心へ寄せる。もいだ脚・削り切った部位は候補から外れる")

    // 踏み込み ─ ロックしている相手が射程の外なら、そこまで詰めながら斬る。
    //
    // WHY 要るか: 射程は 2.6m しかないので、相手が大きいほど «斬る → 離れる →
    //     詰め直す» の «詰め直す» が毎回手作業になる。連撃が繋がる時間 (0.45 秒) は
    //     詰め直すには短く、テンポはここで削られる。
    //
    // WHY 速度ではなく «届く距離» を上限に持つか: 速さだけを決めると、遠い相手ほど
    //     長く滑って «吸い付く» になる。届く範囲を先に決めておけば、
    //     外へ出た相手には «届かないものは届かない» で済む。
    FBZZ_FIELD_RANGE(float, bladeDashRange, 6.0f, "Dash Range", 0.0f, 20.0f)
    FBZZ_TOOLTIP("射程の外に居るロック対象へ、この距離まで踏み込む。0 で踏み込まない")
    FBZZ_FIELD_RANGE(float, bladeDashSpeed, 16.0f, "Dash Speed", 1.0f, 40.0f)
    FBZZ_TOOLTIP("踏み込む速さ [m/s]。発生のあいだに届き切る速さでないと «振ってから寄る» になる")

    FBZZ_FIELD_RANGE(float, bladeDashDepth, 1.0f, "Dash Depth", 0.0f, 5.0f)
    FBZZ_TOOLTIP("射程の縁ちょうどで止めず、このぶん内側まで踏み込む。"
                 "0 だと «ぎりぎり届く» 位置で止まり、当たったり当たらなかったりする")

    // ── 溜め斬り ────────────────────────────────────────────────────────────
    //
    // WHY 押した «瞬間» は今までどおり通常斬りのままにするか:
    //   溜めを «長押しの成果» にすると、押した瞬間に何も起きない待ち時間が生まれ、
    //   近接で最も大事な «押したら斬れる» が死ぬ。押した瞬間は必ず斬り、
    //   そのまま押し続けている間だけ次の一撃を溜める形にすると、
    //   «斬ってから、続けて力を溜める» という 1 続きの動作になる。
    //
    // WHY 溜め斬りを «全周» にするか:
    //   威力を上げるだけの溜めは «強い通常斬り» でしかなく、盤面の読みが増えない。
    //   周り全部へ届く一撃にすると、溜めは «damage を出す手» ではなく
    //   «囲まれた状況を一度に薙ぐ手» になり、いつ溜めるかが判断そのものになる。
    FBZZ_FIELD_RANGE(float, bladeChargeDelay, 0.16f, "Charge Delay", 0.0f, 1.0f)
    FBZZ_TOOLTIP("押しっぱなしがこの秒数を超えてから溜めが始まる。"
                 "0 にすると連打のたびに溜めが立ち上がり、連撃と溜めが混ざる")
    FBZZ_FIELD_RANGE(float, bladeChargeFull, 0.75f, "Charge Time", 0.1f, 3.0f)
    FBZZ_TOOLTIP("溜めが始まってから満溜めになるまで [秒]。満溜め前に離しても溜め斬りは出る "
                 "(威力と範囲が溜め比で決まる)")
    FBZZ_FIELD_RANGE(float, bladeChargedStartup, 0.32f, "Startup (charged)", 0.02f, 1.0f)
    FBZZ_TOOLTIP("溜め斬りの発生 [秒]。通常より長く、振り抜きの重さがここで決まる")
    FBZZ_FIELD_RANGE(float, bladeChargedRecovery, 0.45f, "Recovery (charged)", 0.02f, 1.5f)
    FBZZ_FIELD_RANGE(float, bladeChargedRangeScale, 1.8f, "Range x (charged)", 1.0f, 4.0f)
    FBZZ_TOOLTIP("満溜めの射程倍率。角度は溜め斬りだけ全周 (360 度) になる")
    FBZZ_FIELD_RANGE_INT(int, bladeChargedDamage, 70, "Damage (charged)", 0, 500)
    FBZZ_FIELD_RANGE(float, bladeChargeMoveScale, 0.35f, "Move Scale (charging)", 0.05f, 1.0f)
    FBZZ_TOOLTIP("溜めている間の移動速度倍率。足が止まるほど溜めは重く見えるが、"
                 "0 に近づけるほど溜め中に何もできなくなる")

};

FBZZ_REFLECT(BladeTuning)

} // namespace sandbox
