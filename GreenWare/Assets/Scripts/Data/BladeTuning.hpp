/// @file    BladeTuning.hpp
/// @brief   双剣と盤面の調整値をまとめた共有データアセット (.fzdata)
/// @author  Hasegawa Jin
/// @date    2026-08-22
///
/// @note DataAsset にする理由: 斬撃の間合い・発生・連撃・溜めが敵それぞれのスクリプトに
///       散っていると、«届くのに当たらない» の原因を目視で確かめる方法が無くなるため。
#pragma once

#include <Engine/Asset/DataAsset.hpp>

namespace sandbox {

class BladeTuning : public fbzz::DataAsset {
    FBZZ_DATA_ASSET(BladeTuning)
public:
    FBZZ_GROUP("Blades")
    /// 斬撃が «届く» を決める 3 つ。近接の «当たらない» はほぼ全部ここから出る。
    FBZZ_FIELD_RANGE(float, bladeRange, 2.6f, "範囲", 0.5f, 10.0f)
    FBZZ_TOOLTIP("斬撃が届く距離 [m]。プレイヤー全高 2.51m。自分の背丈ぶんが素直に読める")
    FBZZ_FIELD_RANGE(float, bladeAngleDegrees, 120.0f, "Angle", 20.0f, 360.0f)
    FBZZ_TOOLTIP("正面から左右へ何度まで当たるか (合計)。狭いと当たらず、"
                 "広いと向きを決める意味が消える")
    FBZZ_FIELD_RANGE_INT(int, bladeDamage, 25, "ダメージ", 0, 500)
    FBZZ_TOOLTIP("1 斬りのダメージ。崩しゲージを持つボスには入らない (溜まるのは崩しだけ) ─ "
                 "効くのは BossBreakComponent が付いていない盤面だけ")
    /// @note 0.35 秒は 1 段目 (Slash01) の斬り抜け 0.80 秒に対する再生倍率 2.29 倍
    ///       (0.80 / 0.35) の下限。0.50 秒 (1.6 倍) では長いクリップの振り抜きが
    ///       再生の途中で次のモーションへ切り替わっていた。全段の再生速度はここから
    ///       逆算されるため (BladeComponent)、縮めると 6 段まとめて速くなる。
    ///       触ったら拍 (comboBeatSeconds) との整合も見ること ─ 硬直が下限に張り付く。
    FBZZ_FIELD_RANGE(float, bladeStartup, 0.35f, "Startup", 0.0f, 1.0f)
    FBZZ_TOOLTIP("1 段目の振り始めから判定が出るまで [秒]。手触りが決まらないときは最初にここ。"
                 "連撃全段の再生速度がここから導出されるので、"
                 "縮めれば 6 段まとめて同じだけ速くなる")
    /// @note 最終段だけ別に持つ理由: 締めは «止めた» ことが手に返る段で振りかぶりが長い。
    ///       途中の段と同じテンポだと、最も大きい一撃が最も軽い絵になる。
    FBZZ_FIELD_RANGE(float, bladeFinisherStartup, 0.708333f, "Startup (finisher)", 0.0f, 1.5f)
    FBZZ_TOOLTIP("最終段の発生 [秒]。長いほど溜めて見えるが、遅すぎると差し込まれる")
    /// @note 締めだけは拍 (comboBeatSeconds) の対象外 ─ 次の段が無いため、この値だけが
    ///       クリップ再生の尺を決める。0.26 秒では JumpAttack の再生が 7 割で
    ///       打ち切られ、振り抜きが消えていた。«重すぎる» と感じたら最初に戻す値がここ。
    FBZZ_FIELD_RANGE(float, bladeRecovery, 0.65f, "Recovery", 0.02f, 1.5f)
    FBZZ_TOOLTIP("最終段を振り切った後の硬直。締めのクリップが最後まで流れる長さが要る。"
                 "連撃中の硬直 (拍から逆算) との «差» が、繋がっている感触の正体")
    /// @note 段間の硬直は繋ぐ回数ぶん積み上がる (6 連なら 5 回)。短くしても入力は
    ///       先行入力で預かられる (m_buffered) ので «押した通りに出る» は崩れない。
    FBZZ_FIELD_RANGE(float, bladeComboRecovery, 0.08f, "Recovery (combo)", 0.02f, 1.0f)
    FBZZ_FIELD_RANGE(float, bladeComboWindow, 0.45f, "Combo Window", 0.05f, 2.0f)
    FBZZ_TOOLTIP("次の入力を受け付ける猶予。長いと押しっぱなしで繋がる")
    /// 段ごとのクリップは BladeComponent が持つ (1 袈裟 / 2 双斬り / 3 返し /
    /// 4 回転 / 5 斬り上げ / 6 回転上げ)。ここを 6 未満にすると手前から順に使われ、最後の段が
    /// 必ず締め (回転上げ) になる。7 以上にすると 4 段目の回転斬りが繰り返される。
    FBZZ_FIELD_RANGE_INT(int, bladeComboLength, 6, "Combo Length", 1, 8)
    FBZZ_TOOLTIP("左クリックだけで繋ぐ連撃の段数。6 段で袈裟 → 双斬り → 返し → 回転 → "
                 "斬り上げ → 回転上げ。7 以上は回転斬りを繋ぎに使う")
    /// @note ノックバックを持たない理由: 相手を飛ばせると間合いの管理が «斬って散らす»
    ///       一手で済んでしまい、崩して仕留める芯が遠回りに落ちるため、刃は削るだけに留める。

    /// @note 吸い付きを調整値として持つ理由: 斬る向きはカメラが決め、吸い付きは
    ///       ロック対象へ向きを寄せる補助に留める。0 (手動) から 1 (必ず相手を向く) まで
    ///       連続で選べる値にする。
    FBZZ_FIELD_RANGE(float, bladeAimAssist, 0.45f, "Aim Assist", 0.0f, 1.0f)
    FBZZ_TOOLTIP("斬る向きを当て先へどれだけ寄せるか。0 = カメラの正面そのまま (補正なし)、"
                 "1 = 必ず当て先を向く")

    /// @note 中心 (worldPosition) ではなく部位を当て先にする理由: 射程 2.6m に対し
    ///       ボスの中心は 3m 以上先にあり、脚の横で振っても «届いている» 判定にならなかった。
    ///       選ぶ主体はカメラ (プレイヤー) 側に置き、近さは同点時の決め手に留める。
    FBZZ_FIELD_RANGE(float, bladeAimPartRange, 7.0f, "Aim Part Range", 0.0f, 20.0f)
    FBZZ_TOOLTIP("吸い付きと踏み込みが «脚・コア» を当て先にする捕捉距離。"
                 "0 にすると従来どおり相手の中心へ寄せる。もいだ脚・削り切った部位は候補から外れる")

    /// 踏み込み ─ ロックしている相手が射程の外なら、そこまで詰めながら斬る。
    /// @note 上限を «届く距離» にする理由: 速さだけ決めると遠い相手ほど長く滑って
    ///       «吸い付く» になる。範囲を先に決めれば、外の相手には «届かない» で済む。
    FBZZ_FIELD_RANGE(float, bladeDashRange, 6.0f, "Dash Range", 0.0f, 20.0f)
    FBZZ_TOOLTIP("射程の外に居るロック対象へ、この距離まで踏み込む。0 で踏み込まない")
    FBZZ_FIELD_RANGE(float, bladeDashSpeed, 16.0f, "Dash Speed", 1.0f, 40.0f)
    FBZZ_TOOLTIP("踏み込む速さ [m/s]。発生のあいだに届き切る速さでないと «振ってから寄る» になる")

    FBZZ_FIELD_RANGE(float, bladeDashDepth, 1.0f, "Dash Depth", 0.0f, 5.0f)
    FBZZ_TOOLTIP("射程の縁ちょうどで止めず、このぶん内側まで踏み込む。"
                 "0 だと «ぎりぎり届く» 位置で止まり、当たったり当たらなかったりする")

    /// @name 溜め斬り
    /// @{
    /// @note 押した «瞬間» は通常斬りのままにする理由: 溜めを長押しの成果にすると、
    ///       押した瞬間に何も起きない待ち時間が生まれ «押したら斬れる» が死ぬ。
    ///       押した瞬間は必ず斬り、押し続けている間だけ次の一撃を溜める。
    /// @note 溜め斬りを «全周» にする理由: 威力だけの溜めは «強い通常斬り» でしかなく
    ///       盤面の読みが増えない。全周に届く一撃なら «囲まれた状況を一度に薙ぐ手» になり、
    ///       いつ溜めるかが判断そのものになる。
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
    /// @}

};

FBZZ_REFLECT(BladeTuning)

} // namespace sandbox
