/// @file    PolarityTuning.hpp
/// @brief   極性システムの調整値をまとめた共有データアセット (.fzdata)
/// @author  Hasegawa Jin
/// @date    2026-08-22
///
/// WHY DataAsset にするか:
///   Docs/polarity-system.md「数値の制約」は次の 2 本を «調整項目ではなく破ってはいけない
///   設計上の制約» と書いている。
///
///       ① 反発半径 < 引力半径
///       ② 最も短い持続時間 > バッテリー連続照射時間 + 溜め時間
///
///   ① が崩れると «固めると弾け、散らすと集まる» が成立せず、距離の読みが消える。
///   ② が崩れると «塗り終える前に最初の 1 体の極が切れる» ので、複数体を 1 つの集束に
///   まとめられない。どちらもクラッシュせず «なんとなく繋がらない» としか見えない。
///   斬撃の間合いと持続時間が敵それぞれのスクリプトに散っていると目視で確かめる
///   方法が無くなるので、1 枚のアセットへ並べる。
#pragma once

#include <Engine/Asset/DataAsset.hpp>

namespace sandbox {

class PolarityTuning : public fbzz::DataAsset {
    FBZZ_DATA_ASSET(PolarityTuning)
public:
    FBZZ_GROUP("Blades")
    // 極を乗せる入口。照射から斬撃へ移った (Docs/blades.md)。
    //
    // WHY 極性の調整値と同じアセットに置くか: 剣は «極を乗せる道具» で、射程も発生も
    //     «どこまで届くか / いつ乗るか» という極性側の話でしかない。別アセットへ分けると、
    //     反発半径 6m と斬撃射程 2.6m の関係を 2 枚並べないと確かめられなくなる。
    FBZZ_FIELD_RANGE(float, bladeRange, 2.6f, "Range", 0.5f, 10.0f)
    FBZZ_TOOLTIP("斬撃が届く距離 [m]。プレイヤー全高 2.51m。自分の背丈ぶんが素直に読める")
    FBZZ_FIELD_RANGE(float, bladeAngleDegrees, 120.0f, "Angle", 20.0f, 360.0f)
    FBZZ_TOOLTIP("正面から左右へ何度まで当たるか (合計)。狭いと当たらず、"
                 "広いと向きを決める意味が消える")
    FBZZ_FIELD_RANGE_INT(int, bladeDamage, 25, "Damage", 0, 500)
    FBZZ_TOOLTIP("1 斬りのダメージ。Mite の HP 100 に対して 4 回。"
                 "上げると «斬るだけ» が最適解になり、極を乗せる理由が消える")
    // 立っているボスへの通り。転倒中は 1.0 (この倍率が掛からない)。
    //
    // WHY 0 ではなく «小さく» 通すか: 0 だと、斬っても数字が 1 つも動かない相手を
    //     何十秒も殴ることになり、当たっているかどうかが手応えでしか分からない。
    //     WHY 1 でもないか: いつでも等倍で削れるなら、部位に極を乗せて転ばせる手順が
    //     «遠回り» に落ちる。倒してから斬るのが最短であり続ける量に留める
    //     (0.15 なら 25 → 3。HP 三桁のボスを斬りだけで倒すには現実的でない回数が要る)。
    FBZZ_FIELD_RANGE(float, bladeBossStandingScale, 0.15f, "Boss Scale (standing)", 0.0f, 1.0f)
    FBZZ_TOOLTIP("立っているボスへ通るダメージの倍率。転倒中は常に等倍。"
                 "0 で «倒すまで一切通らない» という以前の挙動に戻る")
    // WHY 発生を最初に触る値と決めておくか: 近接の «当たらない» はほぼ全部ここが原因で、
    //     射程や角度を広げても直らない。振ってから判定が出るまでが遅いと、
    //     目で見えている間合いと実際に当たる間合いがずれる。
    FBZZ_FIELD_RANGE(float, bladeStartup, 0.22f, "Startup", 0.0f, 0.5f)
    FBZZ_TOOLTIP("振り始めてから判定が出るまで [秒]。手触りが決まらないときは最初にここ。"
                 "斬撃クリップの再生速度もここから導出されるので、"
                 "縮めればモーションも同じだけ速くなる")
    // WHY 最終段だけ別に持つか: 3 段目は «止めた» ことが手に返る段で、そこだけ
    //     振りかぶりが長い。1・2 段目と同じ発生にすると、最も大きい一撃が
    //     最も軽い絵になる。ノックバックを別に持っているのと同じ理由。
    FBZZ_FIELD_RANGE(float, bladeFinisherStartup, 0.32f, "Startup (finisher)", 0.0f, 0.8f)
    FBZZ_TOOLTIP("最終段の発生 [秒]。長いほど溜めて見えるが、遅すぎると差し込まれる")
    FBZZ_FIELD_RANGE(float, bladeRecovery, 0.30f, "Recovery", 0.02f, 1.0f)
    FBZZ_TOOLTIP("単発で止めたときの硬直。連撃中との «差» が繋がっている感触の正体")
    FBZZ_FIELD_RANGE(float, bladeComboRecovery, 0.12f, "Recovery (combo)", 0.02f, 1.0f)
    FBZZ_FIELD_RANGE(float, bladeComboWindow, 0.45f, "Combo Window", 0.05f, 2.0f)
    FBZZ_TOOLTIP("次の入力を受け付ける猶予。長いと押しっぱなしで繋がる")
    FBZZ_FIELD_RANGE_INT(int, bladeComboLength, 3, "Combo Length", 1, 8)
    FBZZ_TOOLTIP("連撃の段数。4 以上にすると «繋ぎ切ること» が目的になり盤面を見なくなる")
    // WHY ノックバックを持たないか:
    //   斬撃で相手を飛ばせると、盤面を動かす手が «極性で組む» と «斬って散らす» の
    //   2 本になる。散らす方が速くて確実なので、極を乗せて引き合わせるという芯が
    //   «遠回り» に落ちる。刃は極を乗せるだけで、相手を動かすのは極性
    //   (引力・斥力・衝突) の側にだけ残す。
    //
    // 剣を振っている間だけプレイヤーがその極を帯びる (Docs/blades.md)。
    //
    // WHY 硬直より長くするか: 短いと振り終わってから引かれ始めるまでに無極の空白ができ、
    //     «斬った勢いで飛ぶ» ではなく «斬ってから、なぜか飛ぶ» になる。
    FBZZ_FIELD_RANGE(float, bladeChargeSeconds, 0.9f, "Self Charge", 0.05f, 4.0f)
    FBZZ_TOOLTIP("振ってから何秒プレイヤーがその極を帯びるか。斬撃の硬直より長くすること")

    // WHY 吸い付きを調整値として持つか:
    //   «斬りたい方向» を決めているのはプレイヤーのカメラで、ロック対象へ向きを寄せるのは
    //   あくまで補助。どれだけ寄せるかは手触りの好みそのものなので、0 (完全に手動) から
    //   1 (必ず相手を向く) まで連続で選べる 1 つの値にする。
    FBZZ_FIELD_RANGE(float, bladeAimAssist, 0.0f, "Aim Assist", 0.0f, 1.0f)
    FBZZ_TOOLTIP("斬る向きをロック対象へどれだけ寄せるか。0 = カメラの正面そのまま (補正なし)、"
                 "1 = 必ず相手を向く")

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
    //   周り全部に極を乗せる一撃にすると、溜めは «damage を出す手» ではなく
    //   «盤面を一度に染める手» になり、いつ溜めるかが極性の判断そのものになる。
    FBZZ_FIELD_RANGE(float, bladeChargeDelay, 0.16f, "Charge Delay", 0.0f, 1.0f)
    FBZZ_TOOLTIP("押しっぱなしがこの秒数を超えてから溜めが始まる。"
                 "0 にすると連打のたびに溜めが立ち上がり、連撃と溜めが混ざる")
    FBZZ_FIELD_RANGE(float, bladeChargeFull, 0.75f, "Charge Time", 0.1f, 3.0f)
    FBZZ_TOOLTIP("溜めが始まってから満溜めになるまで [秒]。満溜め前に離しても溜め斬りは出る "
                 "(威力と範囲が溜め比で決まる)")
    FBZZ_FIELD_RANGE(float, bladeChargedStartup, 0.30f, "Startup (charged)", 0.02f, 1.0f)
    FBZZ_TOOLTIP("溜め斬りの発生 [秒]。通常より長く、振り抜きの重さがここで決まる")
    FBZZ_FIELD_RANGE(float, bladeChargedRecovery, 0.45f, "Recovery (charged)", 0.02f, 1.5f)
    FBZZ_FIELD_RANGE(float, bladeChargedRangeScale, 1.8f, "Range x (charged)", 1.0f, 4.0f)
    FBZZ_TOOLTIP("満溜めの射程倍率。角度は溜め斬りだけ全周 (360 度) になる")
    FBZZ_FIELD_RANGE_INT(int, bladeChargedDamage, 70, "Damage (charged)", 0, 500)
    FBZZ_FIELD_RANGE(float, bladeChargedSelfCharge, 1.6f, "Self Charge (charged)", 0.05f, 5.0f)
    FBZZ_TOOLTIP("溜め斬りの後に自分が極を帯びる時間。染めた盤面をそのまま移動に使えるよう、"
                 "通常より長く取る")
    FBZZ_FIELD_RANGE(float, bladeChargeMoveScale, 0.35f, "Move Scale (charging)", 0.05f, 1.0f)
    FBZZ_TOOLTIP("溜めている間の移動速度倍率。足が止まるほど溜めは重く見えるが、"
                 "0 に近づけるほど溜め中に何もできなくなる")

    FBZZ_GROUP("Aim")
    FBZZ_FIELD_RANGE(float, beamRange, 40.0f, "Beam Range", 5.0f, 120.0f)
    FBZZ_TOOLTIP("照射が届く距離 (m)。線上の敵は貫通して全員が判定に乗る")
    FBZZ_FIELD_RANGE(float, beamRadius, 0.6f, "Beam Radius", 0.05f, 3.0f)
    FBZZ_TOOLTIP("ロックオンを廃した代わりのエイム補助。太いほど楽になるが、"
                 "線を意図して引く感覚が薄れる")
    FBZZ_FIELD_RANGE(float, paintSeconds, 0.15f, "Paint Seconds", 0.02f, 1.0f)
    FBZZ_TOOLTIP("1 体あたりの塗り時間。長いほど『丁寧になぞる』ゲームになり、"
                 "短いほど大味になる。素早く振ると塗り残す")
    // WHY 塗り残しを即座に捨てないか: ビームの縁を掠めた 1 フレームの取りこぼしで
    //     進捗が全部消えると、原因がプレイヤーには「たまに塗れない」としか見えない。
    //     塗るのと同じ速さで戻すことで、往復してなぞれば追いつく形に収める。
    FBZZ_FIELD_RANGE(float, paintDecayScale, 1.0f, "Paint Decay", 0.0f, 8.0f)
    FBZZ_TOOLTIP("ビームが外れたときに塗り進捗が戻る速さ (塗り速度に対する倍率)。0 で戻さない")
    FBZZ_FIELD_RANGE(float, tapSeconds, 0.18f, "Tap Seconds", 0.02f, 0.6f)
    FBZZ_TOOLTIP("これ以下で離すとタップ = 1 体だけへの短い点付与。長押しはなぞり塗りになる。"
                 "起爆という工程は無いので、タップと長押しの差は «1 体か、なぞりか» だけ")

    // ダメージが無い照射でも命中したと分かる短い硬直。敵 AI だけを止め、物理は止めない。
    FBZZ_FIELD_RANGE(float, hitReactSeconds, 0.08f, "Hit React", 0.0f, 0.5f)
    FBZZ_TOOLTIP("極性付与時の短い硬直。«命中時にビクッとする» ための時間")

    // 逆極を当てて中和した対象は、しばらくどの極も受け付けなくなる。
    //
    // WHY 罰を用意するか: 中和の «結果» を塗る前に見せる警告表示 (旧 6.5) は廃止した。
    //     結果が全部見えていると判断する必要が無くなり、失敗そのものが起きなくなる。
    //     見せるのをやめる代わりに、間違えたら一手ぶん無駄になるようにする。
    FBZZ_FIELD_RANGE(float, neutralizeLockSeconds, 0.8f, "Neutralize Lock", 0.0f, 4.0f)
    FBZZ_TOOLTIP("中和した対象がどの極も受け付けなくなる時間。0 で罰なし")

    FBZZ_GROUP("Battery")
    // 「時間が資源になる」。塗れる体数の天井をここで決める。
    // 塗り放題にすると「全部＋にして最後に−」が毎回の正解になり、思考が消える。
    FBZZ_FIELD_RANGE(float, batterySeconds, 1.2f, "Battery Seconds", 0.2f, 10.0f)
    FBZZ_TOOLTIP("連続照射できる時間。左右で独立。制約②: 最短の持続時間 > これ + 溜め時間。"
                 "1.2 秒は塗り時間 0.15 秒の 8 体ぶんだが、視線移動が入るので実際は 3〜4 体")
    FBZZ_FIELD_RANGE(float, batteryRefillSeconds, 3.0f, "Refill Seconds", 0.1f, 10.0f)
    FBZZ_TOOLTIP("空から全快までの時間。非照射時にだけ回復する")
    // WHY 空になったら一定量まで再点火させないか: 空のまま押しっぱなしにすると
    //     「1 フレーム回復 → 1 フレーム照射」で毎フレーム点滅し、線も引けないまま
    //     バッテリーだけが空で張り付く。撃てる状態と撃てない状態を明確に分ける。
    FBZZ_FIELD_RANGE(float, batteryRearmRatio, 0.25f, "Rearm Ratio", 0.0f, 1.0f)
    FBZZ_TOOLTIP("空にした後、この割合まで回復するまで再点火できない。0 で即座に撃ち直せる")

    FBZZ_GROUP("Duration")
    // 重い対象ほど長く帯電する。ただし «あと何秒か» を読ませるための時間ではなく、
    // «今撃たなければ消える» と急かすための時間なので、旧値の 1/3 以下まで詰めてある。
    FBZZ_FIELD_RANGE(float, durationNormal,  2.5f, "Mite (light)",   0.5f, 60.0f)
    FBZZ_TOOLTIP("最も短い。撃ったら使う、というリズムをこの敵が教える")
    FBZZ_FIELD_RANGE(float, durationShooter, 3.0f, "Serpent (mid)",  0.5f, 60.0f)
    FBZZ_FIELD_RANGE(float, durationHeavy,   5.0f, "Roller (heavy)", 0.5f, 60.0f)
    FBZZ_TOOLTIP("的として置いておける唯一の敵。長いのはそのため")
    FBZZ_FIELD_RANGE(float, durationPillar,  4.0f, "Kill Core",      0.5f, 60.0f)
    FBZZ_TOOLTIP("倒した敵がその場に残す一時アンカーの寿命")
    // WHY 同極を «延長» ではなく «上書き» にするか:
    //   延長に上限を設けても、1 体を撫で続けるだけで盤面の 1 か所を固定できてしまう。
    //   持続そのものが 2.5 秒まで詰まった今は、撃ち直せば満タンに戻るだけで足りる。
    //   ratio を 1.0 にすると «上書き» と同義になる (既定)。
    FBZZ_FIELD_RANGE(float, extendRatio, 1.0f, "Extend Ratio", 0.0f, 1.0f)
    FBZZ_TOOLTIP("同極を重ねたとき、基準持続時間のこの割合ぶん残り時間を戻す。1 で満タンへ上書き")
    FBZZ_FIELD_RANGE(float, extendCapRatio, 1.0f, "Extend Cap", 1.0f, 4.0f)
    FBZZ_TOOLTIP("延長の上限。基準持続時間の何倍まで伸ばせるか。1 で «満タン以上にはならない»")

    FBZZ_GROUP("Repulsion (same pole)")
    // 本作の即応性を担う。トリガーを引いた瞬間に盤面が動くのは、ほぼこれによる。
    //
    // WHY 引力の半分にするか: «近いと弾ける、遠いと溜まる» を距離だけで覚えさせる。
    //     ここが引力半径以上になると «固めると弾け、散らすと集まる» が成立せず、
    //     同極を並べて集束させる手そのものが消える (制約①)。
    FBZZ_FIELD_RANGE(float, repulsionRadius, 6.0f, "Repulsion Radius", 0.0f, 30.0f)
    FBZZ_TOOLTIP("同極どうしが弾き合う距離。0 で反発を切る (旧仕様の挙動)。"
                 "制約①: 必ず Attraction Radius より小さいこと")
    // WHY 溜めを持たないか: 引力 (溜め 0.45 秒) と役割を分ける。押しは軽く即時、
    //     引きは重く溜めがある。同じ立ち上がりにすると 2 つの手が同じ 1 つに見える。
    FBZZ_FIELD_RANGE(float, repulseSpeed, 12.0f, "Repulse Speed", 0.0f, 40.0f)
    FBZZ_TOOLTIP("弾かれる初速 [m/s]。中心で最大、縁で 0 まで落ちる。"
                 "遅いと «押した気がしない»、速いと盤面が壊れて組めなくなる")
    FBZZ_FIELD_RANGE(float, repulseLift, 2.4f, "Repulse Lift", 0.0f, 20.0f)
    FBZZ_TOOLTIP("弾かれる瞬間に上へ乗せる初速。0 だと床を滑るだけで «弾けた» に見えない")
    FBZZ_FIELD_RANGE(float, repulsePlateau, 1.5f, "Repulse Plateau", 0.0f, 10.0f)
    FBZZ_TOOLTIP("この距離までは減衰させず最大で弾く。密着した 2 体が «押されない» のを防ぐ")
    // WHY 間隔を空けるか: 弾かれた先でまだ半径内に居ると、次のフレームでまた弾かれる。
    //     押し合いが毎フレーム反転して、盤面が痙攣しているようにしか見えなくなる。
    FBZZ_FIELD_RANGE(float, repulseCooldown, 0.5f, "Repulse Cooldown", 0.0f, 3.0f)
    FBZZ_TOOLTIP("一度弾かれた対象が次に弾かれるまでの最短間隔 [秒]")

    FBZZ_GROUP("Attraction (opposite pole)")
    FBZZ_FIELD_RANGE(float, attractionRadius, 12.0f, "Attraction Radius", 1.0f, 30.0f)
    FBZZ_TOOLTIP("異極が引き合う距離。アリーナの実効半径 20m に合わせた値。"
                 "帯電数で最大 20m まで広がる")
    // 異極が揃った 2 体は、まず重力が抜けたように浮きながら互いと逆へ離れ、
    // 終盤で震え、そこから一気に撃ち出される。
    //
    // WHY 助走と滞空を作るか: 溜めが「その場で震える」だけだと、飛び出しの直前と直後で
    //     絵がほとんど変わらず、衝突の重さが「速いから」ではなく「急に始まったから」に
    //     見える。一度引き離して足を地面から離しておくと、同じ速度でも移動距離と
    //     落差が付き、ぶつかる瞬間の速さが目で読める。
    FBZZ_FIELD_RANGE(float, windupSeconds, 0.45f, "Windup Seconds", 0.0f, 3.0f)
    FBZZ_TOOLTIP("リンク成立から撃ち出しまでの時間。衝突までの合計は これ + Impact Seconds")
    FBZZ_FIELD_RANGE(float, windupGravityScale, 0.15f, "Windup Gravity", -1.0f, 1.0f)
    FBZZ_TOOLTIP("溜め中の重力倍率。0 で完全な無重力、負値で浮き上がり続ける")
    FBZZ_FIELD_RANGE(float, windupLiftSpeed, 1.8f, "Windup Lift", 0.0f, 20.0f)
    FBZZ_TOOLTIP("溜めに入った瞬間に上へ与える初速。重力倍率と合わせて滞空の高さが決まる")
    FBZZ_FIELD_RANGE(float, recoilSpeed, 4.5f, "Recoil Speed", 0.0f, 20.0f)
    FBZZ_TOOLTIP("相手と逆へ離れる速さ。溜めの序盤で強く、終盤へ向けて 0 まで落ちる")
    // WHY 速度で表現するか: 剛体の Transform を直接ずらしても物理同期で戻されるため、
    //     震えは高周波で向きが変わる速度として与える。純移動量はほぼゼロになる。
    FBZZ_FIELD_RANGE(float, chargeJitterSpeed, 0.9f, "Charge Jitter", 0.0f, 5.0f)
    FBZZ_TOOLTIP("溜め終盤の震えの速さ。離れ切ったあとに強まり、撃ち出しの直前が最大になる")

    FBZZ_FIELD_RANGE(float, impactSeconds, 0.18f, "Impact Seconds", 0.02f, 2.0f)
    FBZZ_TOOLTIP("撃ち出しから衝突までの目標時間。距離から速度を逆算するので、"
                 "離れていても近くても同じ間合いでぶつかる")
    FBZZ_FIELD_RANGE(float, attractSpeed, 18.0f, "Attract Speed Floor", 1.0f, 120.0f)
    FBZZ_TOOLTIP("撃ち出し速度の下限。近距離で Impact Seconds どおりにすると遅くなりすぎるため、"
                 "この値を下回らないようにする (下限に当たった分だけ早く着く)")
    FBZZ_FIELD_RANGE(float, minImpactSpeed, 6.0f, "Damage Speed Threshold", 0.0f, 40.0f)
    FBZZ_TOOLTIP("これ未満の相対速度で当たってもダメージにしない")
    // 飛行が終わらないまま残り続けるのを防ぐ保険。
    // WHY 要るか: 相手が別の何かに阻まれて永久に届かない配置は必ず作れてしまう。
    //     その場合に敵が延々と壁へ突き刺さり続けると、AI 停止も解けず盤面が死ぬ。
    FBZZ_FIELD_RANGE(float, maxFlightSeconds, 3.0f, "Max Flight Seconds", 0.2f, 10.0f)
    FBZZ_TOOLTIP("この秒数を超えても衝突しなければ引き寄せを打ち切る (詰み防止の保険)")

    FBZZ_GROUP("Player Polarity")
    // 回避と同時にトリガーを引くと、プレイヤー自身が短時間だけ極を帯びる。
    //
    // WHY 常時ではなく回避と同時か: 常に帯びられると «盤面に使うか自分に使うか» の
    //     選択が «いつでも両方» に緩む。回避と束ねると、移動する意思と極を纏う意思が
    //     1 つの入力になり、代償 (その銃が撃てない) が必ず付いてくる。
    FBZZ_FIELD_RANGE(float, playerPolaritySeconds, 1.2f, "Charge Seconds", 0.1f, 5.0f)
    FBZZ_TOOLTIP("プレイヤーが極を纏っている時間。この間その側の銃は撃てない。"
                 "長いと銃が使えない時間が痛すぎ、短いと移動しきれない")
    FBZZ_FIELD_RANGE(float, playerDashCooldown, 1.5f, "Dash Cooldown", 0.0f, 6.0f)
    FBZZ_TOOLTIP("極性回避の再使用までの時間。通常回避と共有する")
    // WHY 引力・斥力でプレイヤー自身を動かさないか:
    //   極を纏うのは攻撃の副産物なので、盤面の力で足を取ると «斬るたびに自分が
    //   どこかへ運ばれる» ことになり、立ち位置がプレイヤーの判断でなくなる。
    //   纏いが動かすのは相手だけ (体当たり) で、自分の足は最後まで入力が持つ。
    // プレイヤーの体当たりでは倒せない (Docs/polarity-system.md「衝突」)。
    // 当たった敵を吹き飛ばすだけで、ダメージは一切入らない。
    FBZZ_FIELD_RANGE(float, playerBumpSpeed, 9.0f, "Bump Speed", 0.0f, 40.0f)
    FBZZ_TOOLTIP("極を纏ったプレイヤーがぶつかった敵を弾く速さ。ダメージは入らない")

    FBZZ_GROUP("Impact")
    // 衝突後の「短時間スタン」。この間は再び引き寄せの対象にならない。
    // WHY 引力側の除外まで兼ねるか: 除外しないと、ぶつかって重なった 2 体が
    //     その場で再リンクし、密着したまま延々と引き合い続ける (見た目は震えるだけ)。
    FBZZ_FIELD_RANGE(float, impactStunSeconds, 0.45f, "Impact Stun", 0.0f, 3.0f)
    FBZZ_TOOLTIP("衝突後の硬直。この間は引力の対象から外れる")
    // 衝突時に跳ね返る速さ。物理ソルバーの反発だけだと、正面衝突で両者が
    // その場に止まってしまい「ドンッ」の手応えが出ない。
    FBZZ_FIELD_RANGE(float, impactKnockbackSpeed, 6.0f, "Impact Knockback", 0.0f, 30.0f)
    FBZZ_TOOLTIP("衝突した瞬間に接触法線方向へ弾き返す速さ")

    FBZZ_GROUP("Visual")
    // 残り時間が短くなるほど明滅が速くなる。可視化は演出ではなく仕様。
    //
    // WHY 下限を上げたか: 持続が 2.5〜5.0 秒まで詰まったので、明滅はすぐ速い側へ寄る。
    //     «点滅している = 消えかけ» ではなく «点滅している = 帯電している» という
    //     読み方になるため、点きはじめをもう少しゆっくりにしておく。
    FBZZ_FIELD_RANGE(float, blinkHzMin, 1.6f, "Blink Hz (fresh)", 0.0f, 10.0f)
    FBZZ_FIELD_RANGE(float, blinkHzMax, 7.0f, "Blink Hz (expiring)", 0.0f, 30.0f)
    FBZZ_FIELD_RANGE(float, blinkDepth, 0.35f, "Blink Depth", 0.0f, 1.0f)
    FBZZ_TOOLTIP("明滅の深さ。0 で明滅しない")

    FBZZ_GROUP("Audio")
    // WHY 明滅と同じグループに置かないか: 明滅は 1 体を見たときの残り時間の読みで、
    //     こちらは «盤面にいくつ帯電しているか» の読み。帯電した対象の数だけ同時に
    //     鳴るので、1 体ぶんの値を上げると盤面全体が一気に濁る。1 体の見た目を
    //     調整するときに触る値と、重なりを調整するときに触る値は分けておく。
    FBZZ_FIELD_RANGE(float, chargedLoopVolume, 0.35f, "Charged Loop", 0.0f, 1.0f)
    FBZZ_TOOLTIP("帯電中ずっと鳴る音の 1 体あたりの音量。0 で鳴らさない")

    /// 制約① 反発半径 < 引力半径。
    /// 崩れると «固めると弾け、散らすと集まる» が成立せず、距離の読みが消える。
    /// 反発を切っている (0) 構成は制約の対象外。
    [[nodiscard]] bool SatisfiesRadiusConstraint() const
    {
        return repulsionRadius <= 0.0f || repulsionRadius < attractionRadius;
    }

    /// 連撃を 1 セット振り切るのにかかる時間 [秒]。
    ///
    /// WHY 制約の基準がこれになったか: 照射時代は「1 本の線を引き切る時間」を
    ///     バッテリーの連続照射時間で代表させていた。斬撃には塗り時間もバッテリーも
    ///     無く、代わりに「連撃を振り切るのに何秒かかるか」がその役を引き継いだ。
    ///     2 体目を斬り終えるまでに 1 体目の極が切れると、2 体を 1 つの衝突に
    ///     まとめられない。
    [[nodiscard]] float ComboSetSeconds() const
    {
        const int  length  = bladeComboLength > 0 ? bladeComboLength : 1;
        const auto leading = static_cast<float>(length - 1);
        return (bladeStartup + bladeComboRecovery) * leading
             + bladeFinisherStartup + bladeRecovery;
    }

    /// 制約② 最も短い持続時間 > 連撃 1 セットの長さ + 溜め時間。
    ///
    /// WHY 溜めを足すか: 最後の 1 体に極が乗ってから実際にぶつかるまでに要るのは、
    ///     引力の溜め (windupSeconds) だけ。ここまで持てば必ず衝突が成立する。
    [[nodiscard]] bool SatisfiesTimingConstraint(float shortestDuration) const
    {
        return shortestDuration > ComboSetSeconds() + windupSeconds;
    }

    /// 制約②の余裕 [秒]。負なら破っている。ログに数字を出すために公開する。
    [[nodiscard]] float TimingMargin(float shortestDuration) const
    {
        return shortestDuration - (ComboSetSeconds() + windupSeconds);
    }

    // 最も短い持続時間。制約の検算に使う (ここが破れたら他は全部通る)。
    [[nodiscard]] float ShortestDuration() const
    {
        float shortest = durationNormal;
        if (durationShooter < shortest) shortest = durationShooter;
        if (durationHeavy   < shortest) shortest = durationHeavy;
        if (durationPillar  < shortest) shortest = durationPillar;
        return shortest;
    }
};

FBZZ_REFLECT(PolarityTuning)

} // namespace sandbox
