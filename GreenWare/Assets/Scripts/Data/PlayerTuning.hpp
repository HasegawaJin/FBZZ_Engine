/// @file    PlayerTuning.hpp
/// @brief   プレイヤーの移動・ジャンプ・回避・息・体力をまとめた共有データアセット (.fzdata)。
/// @author  Hasegawa Jin
/// @date    2026-08-19
///
/// @note BladeTuning と分ける理由: あちらは敵・柱・銃が共通で参照する「盤面のルール」で
///       触ると成立条件 (7.7) が動く。こちらはプレイヤーの手触りだけに閉じ、ゲームの
///       成立には影響しない。波及範囲が違うものを 1 枚にすると判断が鈍る。
#pragma once

#include <Engine/Asset/DataAsset.hpp>

namespace sandbox {

class PlayerTuning : public fbzz::DataAsset {
    FBZZ_DATA_ASSET(PlayerTuning)
public:
    FBZZ_GROUP("Move")
    FBZZ_FIELD_RANGE(float, moveSpeed,   6.0f,  "Move Speed",   0.1f, 20.0f)
    FBZZ_FIELD_RANGE(float, groundAccel, 18.0f, "Ground Accel", 1.0f, 100.0f)
    FBZZ_FIELD_RANGE(float, groundDecel, 22.0f, "Ground Decel", 1.0f, 100.0f)
    FBZZ_FIELD_RANGE(float, airAccel,     3.0f, "Air Accel",     0.0f,  50.0f)
    FBZZ_FIELD_RANGE(float, turnSpeed,   14.0f, "旋回の速さ",   0.1f, 30.0f)

    FBZZ_GROUP("Jump")
    /// @note 初速ではなく到達点で持つ理由: 重力は ProjectSettings の [physics] gravity が
    ///       正本で全剛体が共有する。初速で持つと重力を触るたびに跳べる高さが黙って変わり、
    ///       レベルデザインの前提が崩れる。到達点なら初速は重力から導かれ
    ///       (v0 = sqrt(2gh))、重力を強くしても高さは保たれたまま滞空だけが縮む。
    /// @note 4.0m の根拠 (2026-09-05): 全高 2.51m に対し 2.8m では «少し浮いた» にしか
    ///       見えなかった。背丈の 1.6 倍でボスの胴 (肩 3m 前後) を見下ろす高さに届く。
    ///       滞空が伸びるぶんは Fall Gravity x が頂点を尖らせて受け止める。
    FBZZ_FIELD_RANGE(float, jumpApexHeight, 4.0f, "Apex Height", 0.2f, 8.0f)
    FBZZ_TOOLTIP("キーを押し続けたときの最高到達点 (m)。キャラの背丈 (2.51m) と比べて決める。"
                 "滞空の長さは ProjectSettings の重力で決まる。"
                 "ボスの衝撃波を跳んで越える必要があるので、Clear Height より十分高く保つこと")
    /// @note 落ちを速くする理由: 上りと下りが同じ加速度だと頂点が長く「浮いている」と
    ///       読まれる。落ちだけ強くすると頂点が尖って重さが出る。世界の重力に対する
    ///       倍率なので、重力を触っても比は保たれる。
    FBZZ_FIELD_RANGE(float, fallGravityMultiplier, 1.8f, "Fall Gravity x", 1.0f, 5.0f)
    FBZZ_TOOLTIP("下降中の重力倍率。上げるほど頂点で粘らず落ちる")
    /// @note 離したら重くする理由: 押しっぱなしと軽く叩くで高さが変わらないと跳躍が
    ///       1 種類の動作になる。段差を越えるだけの小さい跳びを操作で作れるようにする。
    FBZZ_FIELD_RANGE(float, lowJumpGravityMultiplier, 2.6f, "Low Jump Gravity x", 1.0f, 8.0f)
    FBZZ_TOOLTIP("上昇中にジャンプキーを離している間の重力倍率。1 で高さ固定の跳躍になる")

    FBZZ_GROUP("Dodge")
    /// @note ボスの脅威半径 (踏みつけ 3.2m・着地 5.0m) を抜けるには 1 回で 6m 台が要る。
    /// @note 進む時間 (Dodge Duration) は転がりクリップ (0.567 秒) に合わせた 0.36 秒。
    ///       ここより速いと踏み切りの絵の途中で移動が終わり «瞬間移動» に見える
    ///       (2026-09-05)。距離 4.5m を保つよう速さはクリップの尺から逆算してある。
    FBZZ_FIELD_RANGE(float, dodgeSpeed,    20.0f, "Dodge Speed",    1.0f, 60.0f)
    FBZZ_TOOLTIP("回避の «出だし» の水平速度 [m/s]。Move Speed との比が鋭さを決める "
                 "(2 倍を下回ると走りに埋もれる)。終わりまでにここから "
                 "Dodge End Speed x 倍まで落ちる")
    /// ここを触ったら Player.animcontroller の Dodge_Roll の speed も直すこと。
    /// 必要な値は PlayerControllerComponent の Debug > Dodge Clip Speed に出ている。
    FBZZ_FIELD_RANGE(float, dodgeDuration, 0.36f, "Dodge Duration", 0.05f, 1.0f)
    FBZZ_TOOLTIP("この秒数だけ水平速度を回避方向で上書きする。無敵時間でもある。"
                 "**転がりクリップの長さと一致させること** ─ ずれた分だけ "
                 "«体だけ先に着く / 着いた後も転がっている» になる")
    /// @note 等速にしない理由 (2026-09-05): Dodge Speed を最後まで保つと «床を滑っている»
    ///       絵になる。頭で弾けて尻すぼみに減速させると同じ距離でも «跳んだ» に読める。
    ///       移動距離 = Dodge Speed × Duration × (1 + ここ) / 2。終速は Move Speed を
    ///       下回らせること ─ 上回ると回避明けに «滑る尾» が付く。
    FBZZ_FIELD_RANGE(float, dodgeEndSpeedRatio, 0.25f, "Dodge End Speed x", 0.05f, 1.0f)
    FBZZ_TOOLTIP("回避の終わり際の速さ (Dodge Speed に対する比)。1 で等速 (滑って見える)。"
                 "下げるほど出だしだけ鋭い «跳び» になる")
    /// @note 実質の待ちは Cooldown - Duration = 0.21 秒 (連続で 2 回跳べる長さ)。
    ///       Duration を触ったらここも同じだけ動かすこと ─ 差の方が待ち時間なので、
    ///       Duration だけ伸ばすと «押した瞬間にもう次が出る» 側へ倒れる。
    FBZZ_FIELD_RANGE(float, dodgeCooldown, 0.57f, "Dodge Cooldown", 0.0f, 5.0f)
    FBZZ_FIELD(bool, dodgeInvulnerable, true, "Invulnerable")
    FBZZ_TOOLTIP("回避中は攻撃を受けない。切ると純粋な移動に戻り、ジャスト回避も起きない")
    /// @note 出だしだけ無敵にする理由 (2026-09-14): 回避クリップが 1.4 秒あり、全区間を
    ///       無敵にすると «転がっている間は当たらない» になって読みの遊びが消える。
    ///       出だしだけに限ると «踏み切りを合わせる» が技として戻る。
    FBZZ_FIELD_RANGE(float, dodgeInvulnerableSeconds, 0.35f, "無敵の長さ [秒]", 0.0f, 2.0f)
    FBZZ_TOOLTIP("回避の出だしから数えた無敵の長さ。Dodge Duration より長い値を入れると全区間になる")
    /// @note 深さより «戻りの長さ» が «時間が伸びた» を作る (2026-09-07)。1 コマで
    ///       落として即戻す形は «止まった» に見えた。回避はヒットストップではなく
    ///       スローの語で言う。見た目は SlowMotion.hlsl が Slow01 を読んで自動で乗る。
    FBZZ_FIELD_RANGE(float, perfectDodgeSlowScale, 0.40f, "Just Dodge Slow", 0.0f, 1.0f)
    FBZZ_TOOLTIP("ジャスト回避の瞬間に世界を落とす速さ。1 でスローしない。"
                 "0.3 未満は «止まった» に見えるので、深くするより長くすること")
    FBZZ_FIELD_RANGE(float, perfectDodgeSlowSeconds, 0.42f, "Just Dodge Slow Seconds", 0.0f, 2.0f)
    FBZZ_FIELD_RANGE(float, perfectDodgeSlowIn, 0.06f, "Just Dodge Slow In", 0.0f, 0.5f)
    FBZZ_TOOLTIP("掛け始めの秒数。0 だと 1 コマで落ちて «止まった» に見える")
    FBZZ_FIELD_RANGE(float, perfectDodgeSlowOut, 0.28f, "Just Dodge Slow Out", 0.0f, 1.0f)
    FBZZ_TOOLTIP("戻しの秒数。長いほど «世界が動き出す» が見える。ここが手触りの正体")
    FBZZ_FIELD_RANGE(float, perfectDodgeFluxSeconds, 2.5f, "Flux Window", 0.0f, 10.0f)
    FBZZ_TOOLTIP("ジャスト回避後、次の一振りが溜め無しで満溜めの全周斬りになる猶予 [秒]。"
                 "0 で報酬なし (無敵だけ)")

    /// @note 攻撃で減らない理由 (2026-09-14): 斬撃の見返りは崩し +3.5 のみで、コストを
    ///       付けると斬るほど損になり «斬撃 → 刃の熱 → 次の弾き» の輪が止まる。
    ///       守りにだけ効かせ、押しっぱなしのガードを内側から咎める歯止めにする。
    /// @name 守り続けられないから攻める、攻めるから息が戻る。
    /// @{
    /// @note 尽きても HP は削らない。HP は 5 しかなく、息切れに痛みを付けると
    ///       覚える前の段階でボタンに触らなくなる。罰は «その手が出せない» に閉じる。
    FBZZ_GROUP("Breath")
    FBZZ_FIELD_RANGE(float, breathMax, 100.0f, "息の満タン", 10.0f, 300.0f)
    FBZZ_TOOLTIP("他の値はすべてこれに対する量。満タンを触ると回避の連続回数もガードの秒数も一緒に動く")
    /// @note 25 は満タンから 4 回ぶん。回避は 1.15 秒間隔 (Dodge Cooldown) なので、
    ///       4 回続けると 3.4 秒ぶん逃げ続けられて、そこで打ち止めになる。
    FBZZ_FIELD_RANGE(float, breathDodgeCost, 25.0f, "回避の消費", 0.0f, 100.0f)
    /// @note 毎秒 22 で満タンから 4.5 秒。ボスの 1 セット (2〜3 手) は受け切れて、
    ///       居座るには足りない長さ。
    FBZZ_FIELD_RANGE(float, breathGuardDrain, 22.0f, "ガードの消費 [毎秒]", 0.0f, 100.0f)
    FBZZ_TOOLTIP("押しっぱなしで構えている間だけ減る。弾きの窓 (頭の 0.22 秒) では減らない")
    /// @note 空振りにだけ払わせる理由: 弾けた構えは見返りの側なので取らない。
    ///       外した構えには 0.40 秒の硬直しか無く、連打が «とりあえず押す» で成立していた。
    FBZZ_FIELD_RANGE(float, breathParryWhiffCost, 12.0f, "空振りの消費", 0.0f, 100.0f)
    /// @note 息を返す唯一の能動的な手。連撃 5 段を当て切ると +50 で回避 2 回ぶんが戻る
    ///       ─ «攻めた分だけ逃げられる» が数字になる。
    FBZZ_FIELD_RANGE(float, breathSlashGain, 10.0f, "斬撃が当たった回復", 0.0f, 100.0f)
    FBZZ_TOOLTIP("1 振りにつき 1 回。多段 (刻み) では重ねて入らない")
    FBZZ_FIELD_RANGE(float, breathParryGain, 30.0f, "弾きの回復", 0.0f, 100.0f)
    FBZZ_FIELD_RANGE(float, breathJustParryGain, 50.0f, "Just 弾きの回復", 0.0f, 100.0f)
    FBZZ_TOOLTIP("読み切った弾きほど息が続く。ジャスト回避は別枠で満タンまで戻る")
    /// @note 1.2 秒は回避の間隔 (1.15 秒) より少しだけ長い。続けて転がっている間は
    ///       1 滴も戻らず、手を止めた瞬間から戻り始める。
    FBZZ_FIELD_RANGE(float, breathRegenDelay, 1.2f, "回復が始まるまで [秒]", 0.0f, 5.0f)
    FBZZ_FIELD_RANGE(float, breathRegenRate, 35.0f, "回復 [毎秒]", 0.0f, 200.0f)
    FBZZ_TOOLTIP("空から満タンまで約 2.9 秒。崩しの減衰 (毎秒 -8) より速い ─ "
                 "«待てば息は戻るが、その間に崩しは落ちる»")
    /// @note 0 で明けない理由: 回復した 1 滴で転がってまた尽きる往復になり、
    ///       «切れている» こと自体が画面にも手にも出ない。
    FBZZ_FIELD_RANGE(float, breathExhaustRecover, 0.35f, "息切れから戻る割合", 0.05f, 1.0f)
    FBZZ_TOOLTIP("息切れ中はここまで戻るまで回避もガードも出せない。実質 2 秒ほどの空白になる")

    FBZZ_GROUP("HP")
    /// @note _INT 版を使う理由: FBZZ_FIELD_RANGE は IReflector::FloatRange へ流すため
    ///       float 専用。int を渡すと float& へバインドできずコンパイルエラーになる。
    FBZZ_FIELD_RANGE_INT(int, maxHealth, 5, "最大 HP", 1, 50)
    /// 被弾直後の無敵。連続ヒットで一瞬に溶けるのを防ぐためのもので、
    /// 回避の無敵 (Dodge > Invulnerable) とは別物。
    FBZZ_FIELD_RANGE(float, hitInvulnerable, 0.6f, "Hit Invulnerable", 0.0f, 3.0f)

    /// @note 土壇場を持つ理由 (2026-09-06): HP 5 の最後の 1 は «あと 1 発で終わり» で
    ///       逆転する道が無かった。追い詰められたときだけ報酬が厚くなると、最後の 1 が
    ///       «ここからが本番» になる。画面は鼓動する縁 (GameVignette) で同じ状態を言う。
    FBZZ_GROUP("Last Stand")
    FBZZ_FIELD_RANGE_INT(int, lastStandHealth, 1, "Last Stand At", 0, 50)
    FBZZ_TOOLTIP("残り HP がこれ以下で土壇場。0 で無効")
    FBZZ_FIELD_RANGE(float, lastStandFluxScale, 1.5f, "Flux Window x", 1.0f, 4.0f)
    FBZZ_TOOLTIP("土壇場でのジャスト回避の猶予 (Flux Window) の倍率")
    FBZZ_FIELD_RANGE(float, lastStandBreakScale, 1.25f, "Break Gain x", 1.0f, 3.0f)
    FBZZ_TOOLTIP("土壇場で崩しゲージが溜まる量の倍率。弾き・見切り・斬撃すべてに掛かる")
    /// @}
};

FBZZ_REFLECT(PlayerTuning)

} // namespace sandbox
