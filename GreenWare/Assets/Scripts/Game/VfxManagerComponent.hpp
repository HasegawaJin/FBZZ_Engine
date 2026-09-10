/// @file    VfxManagerComponent.hpp
/// @brief   単発 VFX を「借りて返す」枠へ載せ、色と対象の寸法を差し込んでから鳴らす
/// @author  Hasegawa Jin
/// @date    2026-08-24
///
/// WHY 1 箇所へ束ねるか:
///   .vfx を鳴らしたい側 (プレイヤー・ボス・床) は散っている。各自が prefab を置くと、
///   「どの演出がどれだけ画面に出ているか」を誰も知らない状態になる。衝撃波が 1 回で
///   十数発撒く以上、同時発火の抑制は必ずどこかに要る。
///   呼び出し側は「何が起きたか」だけを言い、枠の管理はここが持つ。
///
/// WHY 1 発ぶんの値をスクリプトのフィールドへ書くか (Docs/design/vfx-prefab.md §6):
///   .vfx がプレファブになり、公開パラメーターは «ルートに載せたスクリプトの公開
///   フィールド» になった。旧実装は文字列 (schemaPath) でノードのフィールドを指しており、
///   綴りを間違えても保存も検証も通っていた。型が効く形にすると、光の強さを変えるつもりで
///   パーティクルの色へ書く事故が構造上起こらなくなる。
///
/// WHY GameObject を作り捨てにしないか:
///   演出は 1 秒に何度も出る。そのたびに Create / Destroy すると、シーンの GameObject 数が
///   戦闘の激しさに比例して上下し、EntityID を握っている側の参照が揺さぶられる。
///   .vfx の «種類ごと» にリングを持ち、寝ている枠を起こして使い回す。
///
/// WHY 種類ごとにリングを分けるか:
///   1 本のリングを共有すると、4 秒残る爆発の焦げ跡が、その間に何度も鳴る斬撃の火花に
///   押し出されて途中で消える。寿命の桁が違うものを同じ順番待ちに入れてはいけない。
///
/// WHY 画面演出 (ヒットストップ・カメラ揺れ・振動・フラッシュ) を持たないか:
///   それは ImpactFeedbackManagerComponent の担当で、1 つの出来事に対する配分を
///   そこが持っている。.vfx 側にも ScreenEffect / CameraShake / TimeScale ノードは
///   置いていない (理由は FX_IMP_Explosion.vfx のヘッダー)。ここは «その場所に
///   何が見えるか» だけを受け持ち、«画面がどう反応するか» には触らない。
#pragma once

#include <Engine/Scene/Components/LightComponent.hpp>
#include <Engine/Scene/Components/VFXComponent.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
#include <Scripts/Utils/BodyBounds.hpp>
#include <Scripts/Utils/BladeColors.hpp>
#include <Scripts/Vfx/BeamScorchVfxComponent.hpp>
#include <Scripts/Vfx/ExecuteVfxComponent.hpp>
#include <Scripts/Vfx/ImpactVfxComponent.hpp>
#include <Scripts/Vfx/ParryVfxComponent.hpp>
#include <Scripts/Vfx/RunDustVfxComponent.hpp>
#include <Scripts/Vfx/SerpentBiteVfxComponent.hpp>
#include <Scripts/Vfx/SerpentGeyserVfxComponent.hpp>
#include <Scripts/Vfx/SerpentRushVfxComponent.hpp>
#include <Scripts/Vfx/SerpentSlamVfxComponent.hpp>
#include <Scripts/Vfx/SerpentSnapVfxComponent.hpp>
#include <Scripts/Vfx/SlashHitVfxComponent.hpp>
#include <Scripts/Vfx/ToppleVfxComponent.hpp>
#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

// ── 既定のアセット ───────────────────────────────────────────────────────────
// WHY 既定を持たせるか: Inspector を 1 つも触っていないシーンでも演出が出る状態にする。
//     未割り当てで «何も起きない» が既定だと、演出が消えたときに「壊れた」のか
//     「そもそも差していない」のかを絵から区別できない。差し替えは Inspector が優先する。
inline constexpr const char* kVfxImpactPath      = "Assets/VFX/Game/FX_IMP_Explosion.vfx";
inline constexpr const char* kVfxBeamScorchPath  = "Assets/VFX/Game/FX_BEAM_Scorch.vfx";
inline constexpr const char* kVfxRunDustPath     = "Assets/VFX/Game/FX_PLR_RunDust.vfx";
inline constexpr const char* kVfxGroundDustPath  = "Assets/VFX/Game/FX_BOSS_ShockDust.vfx";
inline constexpr const char* kVfxParryPath       = "Assets/VFX/Game/FX_PLR_Parry.vfx";
inline constexpr const char* kVfxParryJustPath   = "Assets/VFX/Game/FX_PLR_ParryJust.vfx";
inline constexpr const char* kVfxSlashHitPath    = "Assets/VFX/Game/FX_BLD_SlashHit.vfx";
inline constexpr const char* kVfxTopplePath      = "Assets/VFX/Game/FX_BOSS_Topple.vfx";
inline constexpr const char* kVfxExecutePath     = "Assets/VFX/Game/FX_BOSS_Execute.vfx";

// サーペント (Boss02) 専用。手ごとにシルエットを分けるためのグラフ一式。
//
// WHY 床の出来事を «爆発 + 土煙» の 2 つで賄うのをやめたか:
//   突き上げ・叩きつけ・締め上げ・走りはどれも PlayGroundBlast + PlayGroundDust で
//   出していて、特に叩きつけと締め上げは向きの意味まで含めて同じ絵だった。予兆の
//   デカールを見ていなければ «何が来たか» を区別する手掛かりが 1 つも無い状態で、
//   しかも突き上げの «真上へ» は PlayGroundDust が y 成分を捨てるため成立していなかった。
//   手ごとに 1 本ずつ持てば «縦 / 帯 / 放射 / 線» のシルエットで読み分けられる。
inline constexpr const char* kVfxSerpentGeyserPath = "Assets/VFX/Serpent/FX_SRP_Geyser.vfx";
inline constexpr const char* kVfxSerpentSlamPath   = "Assets/VFX/Serpent/FX_SRP_Slam.vfx";
inline constexpr const char* kVfxSerpentSnapPath   = "Assets/VFX/Serpent/FX_SRP_Snap.vfx";
inline constexpr const char* kVfxSerpentRushPath   = "Assets/VFX/Serpent/FX_SRP_Rush.vfx";
inline constexpr const char* kVfxSerpentBitePath   = "Assets/VFX/Serpent/FX_SRP_Bite.vfx";

// 床で起きる演出 (土煙・床を割る爆発) を回すリングの札。
// 同じ .vfx を «その場の爆発» と «床» で共有しても、枠は別々になる。
inline constexpr const char* kGroundPool = "Ground";

// 壊れた脚から立ちのぼる煙。同じ土煙のグラフを «常時鳴り続ける絵» として回す札。
inline constexpr const char* kLegSmokePool = "LegSmoke";

// 噴き上がり (FX_SRP_Geyser) を «手» と «出入り» で分けて回すための札。
//
// WHY 分けるか: 突き上げは 1 回に 3 口ぶん撒く «手» で、口の通過は 7 秒ごとに
//     必ず起きる «常時の絵»。1 本のリングにすると、攻撃のたびに出入りの土煙が
//     頭出しで消え、«胴が口を通っているのに何も出ないフレーム» ができる。
inline constexpr const char* kGeyserThrustPool = "Thrust";
inline constexpr const char* kGeyserMouthPool  = "Mouth";

// FX_IMP_Explosion の Ground Mark。柱・壁・床を割ったときだけ «ひび» へ差し替える。
inline constexpr const char* kMarkScorch = "Assets/VFX/Textures/T_Scorch_Decal.png";
inline constexpr const char* kMarkCrack  = "Assets/VFX/Textures/T_Crack_Decal.png";

// コライダーを持たない対象に使う体の寸法。Mite 相当の大きさ。
inline constexpr float kFallbackBodyTop    = 1.2f;
inline constexpr float kFallbackBodyRadius = 0.6f;

class VfxManagerComponent : public Script {
    FBZZ_SCRIPT(VfxManagerComponent)

public:
    FBZZ_GROUP("Graphs")
    FBZZ_ASSET_FIELD(VFXRef, impactVfx, "着弾")
    FBZZ_TOOLTIP("爆発。未割り当てなら FX_IMP_Explosion.vfx を使う")
    FBZZ_ASSET_FIELD(VFXRef, beamScorchVfx, "Beam Scorch (6.2)")
    FBZZ_TOOLTIP("ビームが地形を焼いた点。未割り当てなら FX_BEAM_Scorch.vfx を使う")
    FBZZ_ASSET_FIELD(VFXRef, runDustVfx, "走りの砂埃")
    FBZZ_TOOLTIP("走っている足が着いた点。未割り当てなら FX_PLR_RunDust.vfx を使う")
    FBZZ_ASSET_FIELD(VFXRef, groundDustVfx, "Ground Dust")
    FBZZ_TOOLTIP("ボスの重量が床へ掛かった点。未割り当てなら FX_BOSS_ShockDust.vfx を使う")
    FBZZ_ASSET_FIELD(VFXRef, parryVfx, "弾き")
    FBZZ_TOOLTIP("刀で弾いた瞬間。未割り当てなら FX_PLR_Parry.vfx を使う")
    FBZZ_ASSET_FIELD(VFXRef, parryJustVfx, "Parry (Just)")
    FBZZ_TOOLTIP("窓の頭で受けた弾き。未割り当てなら FX_PLR_ParryJust.vfx を使う")
    FBZZ_ASSET_FIELD(VFXRef, slashHitVfx, "Slash Hit")
    FBZZ_TOOLTIP("刀が当たった瞬間。未割り当てなら FX_BLD_SlashHit.vfx を使う")
    FBZZ_ASSET_FIELD(VFXRef, toppleVfx, "Topple")
    FBZZ_TOOLTIP("崩しが満ちてボスが倒れた瞬間。未割り当てなら FX_BOSS_Topple.vfx を使う")
    FBZZ_ASSET_FIELD(VFXRef, executeVfx, "とどめ")
    FBZZ_TOOLTIP("とどめで脚 / 節がもげた瞬間。未割り当てなら FX_BOSS_Execute.vfx を使う")

    FBZZ_GROUP("Graphs (Serpent)")
    FBZZ_ASSET_FIELD(VFXRef, serpentGeyserVfx, "Geyser")
    FBZZ_TOOLTIP("床の口から噴き上がる縦の柱。突き上げと «胴が口を通る» の両方で使う。"
                 "未割り当てなら FX_SRP_Geyser.vfx")
    FBZZ_ASSET_FIELD(VFXRef, serpentSlamVfx, "叩きつけ")
    FBZZ_TOOLTIP("11 m の胴が落ちて床を叩いた帯。未割り当てなら FX_SRP_Slam.vfx")
    FBZZ_ASSET_FIELD(VFXRef, serpentSnapVfx, "Snap")
    FBZZ_TOOLTIP("檻が角へ寄って砕けた。未割り当てなら FX_SRP_Snap.vfx")
    FBZZ_ASSET_FIELD(VFXRef, serpentRushVfx, "Rush")
    FBZZ_TOOLTIP("走る胴が床を削った 1 点。未割り当てなら FX_SRP_Rush.vfx")
    FBZZ_ASSET_FIELD(VFXRef, serpentBiteVfx, "Bite")
    FBZZ_TOOLTIP("噛みつきの溜めと、外した着弾。未割り当てなら FX_SRP_Bite.vfx")

    FBZZ_GROUP("Serpent")
    FBZZ_FIELD_RANGE_INT(int, geyserSlots, 14, "Geyser Slots", 1, 32)
    FBZZ_TOOLTIP("噴き上がりの枠数 («手» と «出入り» でそれぞれこの数)。"
                 "«撒く頻度 ≦ 枠数 ÷ 寿命» を割ると輪が虫食いになる ─ 1 発は 1.05 秒 "
                 "なので 14 で 13 発/秒まで。出入りは口 1 つにつき 5 発/秒で、"
                 "渡っている最中は «出る口» と «入る口» の 2 つが同時に鳴る")
    FBZZ_FIELD_RANGE_INT(int, rushSlots, 20, "Rush Slots", 1, 48)
    FBZZ_TOOLTIP("走りの削り跡の枠数。0.12 秒ごとに 1 発・寿命 1.6 秒なので "
                 "«寿命 ÷ 間隔» = 14 を下回ると、走り終える前に先頭の跡が消える")

    // 弾きは «硬い物どうしが一瞬触れた» 出来事。煙も焦げも残さず、閃光と破片だけで
    // 0.4 秒に閉じる。重い手 (突進) を弾いたときだけ破片と輪を大きくする。
    FBZZ_GROUP("弾き")
    FBZZ_FIELD_RANGE(float, parrySparkMin, 11.0f, "火花 (ライト)", 0.0f, 40.0f)
    FBZZ_FIELD_RANGE(float, parrySparkMax, 24.0f, "Spark (heavy)", 0.0f, 40.0f)
    FBZZ_FIELD_RANGE(float, parryLightMin, 12.0f, "ライト", 0.0f, 60.0f)
    FBZZ_FIELD_RANGE(float, parryLightMax, 28.0f, "Light (heavy)", 0.0f, 60.0f)
    FBZZ_FIELD_RANGE(float, parryRingMin, 2.4f, "Ring (light)", 0.5f, 10.0f)
    FBZZ_FIELD_RANGE(float, parryRingMax, 4.4f, "Ring (heavy)", 0.5f, 10.0f)

    FBZZ_GROUP("Slash Hit")
    FBZZ_FIELD_RANGE(float, slashSparkMin, 6.0f, "火花 (ライト)", 0.0f, 40.0f)
    FBZZ_FIELD_RANGE(float, slashSparkMax, 16.0f, "Spark (finisher)", 0.0f, 40.0f)
    FBZZ_FIELD_RANGE(float, slashLightMin, 4.0f, "ライト", 0.0f, 60.0f)
    FBZZ_FIELD_RANGE(float, slashLightMax, 14.0f, "Light (finisher)", 0.0f, 60.0f)
    FBZZ_FIELD_RANGE(float, slashArcMin, 1.1f, "Arc (light)", 0.3f, 5.0f)
    FBZZ_FIELD_RANGE(float, slashArcMax, 2.2f, "Arc (finisher)", 0.3f, 5.0f)
    FBZZ_TOOLTIP("刃が通った弧の大きさ。3 段目だけ太くしてノックバックの大きさを絵で予告する")

    // 転倒は «床の側» の出来事。土煙の色は走行と同じ床の色を使う (runDustColor)。
    FBZZ_GROUP("Topple")
    FBZZ_FIELD_RANGE(float, toppleCrack, 9.0f, "亀裂の大きさ", 2.0f, 20.0f)
    FBZZ_TOOLTIP("床のひびの直径 [m]。Scale 1.0 のときの値")
    FBZZ_FIELD_RANGE(float, toppleLight, 8.0f, "着地音のライト", 0.0f, 30.0f)

    FBZZ_GROUP("とどめ")
    FBZZ_FIELD_RANGE(float, executeSpark, 18.0f, "火花の勢い", 2.0f, 40.0f)
    FBZZ_FIELD_RANGE(float, executeLight, 30.0f, "切り口のライト", 0.0f, 60.0f)
    FBZZ_FIELD_RANGE(float, executeCut, 2.6f, "切り口の大きさ", 0.5f, 8.0f)
    FBZZ_TOOLTIP("弧の大きさ [m]。Scale 1.0 のときの値 (コアの脚)。蛇の節は呼ぶ側が Scale で縮める")

    FBZZ_GROUP("Pool")
    FBZZ_FIELD_RANGE_INT(int, slotsPerEffect, 8, "Slots Per Effect", 1, 32)
    FBZZ_TOOLTIP("1 種類あたりの同時再生数。超えたら最も古い 1 発を頭出しし直して奪う")

    FBZZ_GROUP("着弾")
    // 同じフレームに何発も重なるほど 1 発ずつを弱めないと、光源が重なって画面が
    // 白へ抜け、どこで何が起きたのか読めなくなる。
    FBZZ_FIELD_RANGE(float, blastLightMin, 11.0f, "Blast Light (weak)", 0.0f, 60.0f)
    FBZZ_FIELD_RANGE(float, blastLightMax, 30.0f, "Blast Light (strong)", 0.0f, 60.0f)
    FBZZ_FIELD_RANGE(float, sparkPowerMin, 5.0f, "Spark Power (weak)", 0.0f, 30.0f)
    FBZZ_FIELD_RANGE(float, sparkPowerMax, 20.0f, "Spark Power (strong)", 0.0f, 30.0f)
    FBZZ_FIELD_RANGE_INT(int, lightFalloffCount, 3, "Light Falloff Count", 1, 16)
    FBZZ_TOOLTIP("同フレームに何発目までを明るく出すか。これを超えた分は光源を落とす")
    FBZZ_FIELD_RANGE_INT(int, groundBlastSlots, 14, "Ground Blast Slots", 1, 32)
    FBZZ_TOOLTIP("床を割る爆発 (衝撃波) 専用の枠数。衝突の枠とは別に持つ。"
                 "1 発が焦げ跡まで含めて 4 秒あるので、上げるほど «同時に燃えている» "
                 "数がそのまま増える ─ 撒く間隔より先にここを疑うこと")

    FBZZ_GROUP("Beam Scorch (6.2)")
    // なぞりは 1 本の線を引く操作なので、焼け跡は «列» で置かれる。1 点ぶんを
    // 強くすると線全体が壁を白く塗る。1 点は控えめにして、量で見せる。
    FBZZ_FIELD_RANGE(float, emberRate, 26.0f, "火の粉のレート", 0.0f, 60.0f)
    FBZZ_TOOLTIP("焼けた点 1 つから出る火の粉の量 [個/秒]")

    FBZZ_GROUP("走りの砂埃")
    // 最高速では 1 秒に 7 回近く鳴る。1 発を強くすると足元が煙で埋まって
    // プレイヤー自身が見えなくなるので、量ではなく «速さで変わる» ことで見せる。
    FBZZ_FIELD_COLOR(runDustColor, (Vector4{ 0.66f, 0.63f, 0.58f, 0.4f }), "Dust Color")
    FBZZ_TOOLTIP("床の色。刀の赤青は乗せない (土は誰が立てても同じ色)。アルファが煙の濃さ")
    FBZZ_FIELD_RANGE(float, runDustKickMin, 1.0f, "Kick (walk pace)", 0.0f, 8.0f)
    FBZZ_FIELD_RANGE(float, runDustKickMax, 2.6f, "Kick (top speed)", 0.0f, 8.0f)
    FBZZ_FIELD_RANGE(float, runDustSizeMin, 0.45f, "Puff Size (walk pace)", 0.1f, 3.0f)
    FBZZ_FIELD_RANGE(float, runDustSizeMax, 0.9f, "Puff Size (top speed)", 0.1f, 3.0f)

    // WHY 走行の土煙と数値を分けるか:
    //   同じ .vfx を大きくして共用すると、ボスの重さに合わせた瞬間にプレイヤーの足元まで
    //   煙まみれになる。鳴る頻度も «1 秒に 7 回» と «衝撃波で数十発» で桁が違うので、
    //   枠の数もここだけ別に持つ (種類が違えばリングも別になる)。
    FBZZ_GROUP("Ground Dust")
    FBZZ_FIELD_RANGE_INT(int, groundDustSlots, 56, "スロット", 1, 96)
    FBZZ_TOOLTIP("同時に生きていられる土煙の数。衝撃波の輪はこの数の煙で描かれる。"
                 "«1 枚の点数 x (1 発の寿命 ÷ 輪の間隔)» を下回ると、外周へ届く前に"
                 "内側の輪が欠け始める ─ 撒く側を密にしたら必ずここも上げること")
    FBZZ_FIELD_RANGE(float, groundDustSize, 2.4f, "煙の大きさ", 0.2f, 8.0f)
    FBZZ_TOOLTIP("1 発が最後に広がる大きさ [m]。Scale 1.0 のときの値")
    FBZZ_FIELD_RANGE(float, groundDustKick, 4.2f, "蹴り上げ", 0.0f, 16.0f)
    FBZZ_TOOLTIP("煙と砂粒を押し出す速さ [m/s]。衝撃波では波の速さに近づけると、"
                 "煙が前縁に貼り付いて «押し寄せている» に見える")
    FBZZ_FIELD_RANGE(float, groundDustGrit, 1.4f, "Grit Spread", 0.0f, 4.0f)

    // もげずに残った脚の «壊れている» を言い続ける煙。土煙のグラフを流用し、
    // 色と向きだけを差し替える。
    //
    // WHY 土煙と枠を分けるか: これは «出来事» ではなく、脚が壊れているあいだ
    //     ずっと鳴り続ける常時の絵。床の枠へ相乗りさせると、脚 2 本ぶんの煙が
    //     衝撃波の土煙を毎回押し出して «踏まれたのに床が鳴らない» になる。
    FBZZ_GROUP("壊れた脚の煙")
    FBZZ_FIELD_COLOR(legSmokeColor, (Vector4{ 0.24f, 0.23f, 0.22f, 0.5f }), "煙の色")
    FBZZ_TOOLTIP("焼けた機械から出る煙なので、床の土煙より暗く。アルファが濃さ")
    FBZZ_FIELD_RANGE(float, legSmokeSize, 1.1f, "煙の大きさ", 0.2f, 6.0f)
    FBZZ_TOOLTIP("1 発が最後に広がる大きさ [m]。Scale 1.0 のときの値")
    FBZZ_FIELD_RANGE(float, legSmokeRise, 1.6f, "立ち上がり", 0.0f, 8.0f)
    FBZZ_TOOLTIP("真上へ押し出す速さ [m/s]。0 だと切断面に溜まって «湧いている» に見える")
    FBZZ_FIELD_RANGE_INT(int, legSmokeSlots, 24, "スロット", 1, 64)
    FBZZ_TOOLTIP("同時に生きていられる煙の数。1 発は 1.6 秒あるので、"
                 "«脚の本数 ÷ 間隔 x 1.6» を下回ると古い煙から消えて筋が途切れる")

    FBZZ_GROUP("デバッグ")
    FBZZ_FIELD_READ_ONLY(int, debugSlots, 0, "Live Slots")
    FBZZ_FIELD_READ_ONLY(std::string, debugLast, "", "Last Effect")

    // 盤面に «点いている光» が何個あるか。
    //
    // WHY VFX の管理者が数えるか: 光を実行時に増やしうるのは «演出» だけで、
    //     その大半 (爆発・弾き・斬撃・とどめ) の実体はここが回している枠。
    //     どこかが片付け損ねたときに最初に膨らむのもここなので、数える場所も同じにする。
    //
    // WHY 上限を «256» ではなく警告のしきい値で見るか: 統合配列の上限は 256 だが、
    //     クラスタ 1 つに入るのは 64。密集して 64 を越えた時点で «そこだけ暗くなる»
    //     という、原因の見えない壊れ方をする。膨らみ始めを名指しで拾う。
    FBZZ_GROUP("Light Census")
    FBZZ_FIELD(bool, censusLights, true, "Count Lights")
    FBZZ_FIELD_RANGE_INT(int, censusWarnAt, 48, "Warn At", 8, 256)
    FBZZ_TOOLTIP("点いている光がこの数を越えたら、内訳を 1 度だけ名指しで報告する")
    FBZZ_FIELD_READ_ONLY(int, debugLightsLit, 0, "Lights (lit)")
    FBZZ_FIELD_READ_ONLY(int, debugLightsTotal, 0, "Lights (total)")

    [[nodiscard]] static VfxManagerComponent* Instance() { return s_instance; }

    /// 対象を包んでいた光が内へ畳まれて消える。吸い込む半径を体の寸法へ合わせる。
    /// 爆発。strength01 は当たりの強さ、againstAnchor は柱・壁へ叩きつけたか。
    /// side は色を選ぶだけで、None なら無彩色。
    void PlayImpact(const Vector3& point, BladeSide side, float strength01,
                    bool againstAnchor);
    /// 床が砕けた爆発。見た目は PlayImpact と同じで、枠のリングだけが別。
    ///
    /// WHY 分けるか: 衝撃波は 1 回で十数発撒く。同じリングに入れると、波が
    ///     走っているあいだ «その場で起きた» 爆発が全部押し出される。
    void PlayGroundBlast(const Vector3& point, BladeSide side, float strength01);
    /// ビームが地形を焼いた点 (6.2)。normal は焼けた面の法線、searSize は焦げの直径。
    ///
    /// WHY 爆発と分けるか: 焼け跡は 1 本の線として «列» で置かれ、なぞっている間ずっと
    ///     鳴り続ける。同じ枠に入れると、壁をなぞっただけで爆発の枠が押し出され、
    ///     本当にぶつかった爆発が途中で消える。
    void PlayBeamScorch(const Vector3& point, const Vector3& normal, BladeSide side,
                        float searSize);
    /// 走っている足が床に着いた点。moveDirection は進んでいる向き、
    /// strength01 は最高速に対する今の速さ。
    ///
    /// WHY 他の 5 つと違い «盤面の出来事» ではないのにここへ置くか:
    ///     同時発火の抑制が要る理由は同じで、むしろ足元が最も頻繁に鳴る。呼ぶ側 (移動) に
    ///     枠を持たせると、走っているあいだじゅう自前のリングを回すことになる。
    void PlayRunDust(const Vector3& footPoint, const Vector3& moveDirection, float strength01);
    /// 重いものが床へ掛かって舞う土煙。outward は煙が流れていく «向き»、
    /// scale は 1.0 で Ground Dust の既定の大きさ。
    ///
    /// WHY 走行の土煙と別の口にするか:
    ///   走行のそれは «足が後ろへ掻いた» 跡なので、渡した進行方向の逆へ吹く。こちらは
    ///   «床が押し退けられた» 側なので、渡した向きそのものへ吹く。同じ関数で兼ねると、
    ///   呼ぶ側が «どちらの意味で渡すのか» を毎回思い出さないといけなくなる。
    void PlayGroundDust(const Vector3& point, const Vector3& outward, float strength01,
                        float scale = 1.0f);
    /// 刀で弾いた瞬間。point は刃と攻撃が触れた所、away は破片が逃げる向き
    /// (攻撃してきた側から離れる向き)。strength01 は弾いた手の重さ。
    /// just は窓の頭で受けた «読み切った» 弾き ─ 別の .vfx (放射の光条と二重の輪) を使う。
    void PlayParry(const Vector3& point, const Vector3& away, float strength01, bool just = false);
    /// 刀が当たった瞬間。away は刃が抜けていく向き。side は振った刀 (弧と光の色)。
    ///
    /// WHY 弾きの流用をやめたか: 輪と閃光は «弾いた» の印で、当たりのたびに輪が出ると
    ///     1 秒に何度も弾いているように読める。当たりは «刃が通った弧» と火花だけで言う。
    void PlaySlashHit(const Vector3& point, const Vector3& away, float strength01,
                      BladeSide side = BladeSide::None);
    /// 崩しが満ちてボスが倒れた瞬間。point は体が床に着いた所、scale は 1.0 でコア (6m 級)。
    ///
    /// WHY 床の爆発 (PlayGroundBlast) を使わないか: 爆発は閃光と熱が主役で、転倒は床が主役。
    ///     倒れた瞬間に画面が白く光ると «撃破» と読まれ、5 秒の転倒が «終わった» に見える。
    void PlayTopple(const Vector3& point, float scale = 1.0f);
    /// 壊れたまま本体に残っている脚から立ちのぼる煙。point は煙の出どころ (膝の破断面)、
    /// drift は横へ流れる向き (水平)。scale は 1.0 で Leg Smoke の既定の大きさ。
    ///
    /// WHY 床の土煙と別の口にするか: あちらは «床が押し退けられた» で水平に吹くが、
    ///     こちらは «壊れた物から立ちのぼる» で主役は上。同じ関数で兼ねると、
    ///     呼ぶ側が渡す向きの意味が 2 通りになる (PlayGroundDust の WHY と同じ)。
    void PlayLegSmoke(const Vector3& point, const Vector3& drift, float strength01,
                      float scale = 1.0f);
    /// とどめで脚 / 節がもげた瞬間。point は切断面、away は刃が抜けた向き (斬った側から離れる向き)。
    /// scale は 1.0 でコアの脚。蛇の節は 0.7 程度。
    void PlayExecute(const Vector3& point, const Vector3& away, float scale = 1.0f);

    // ── サーペント (Boss02) の手ごとの絵 ────────────────────────────────
    //
    // WHY «床の出来事» を 1 つの口で兼ねないか (PlayGroundBlast / PlayGroundDust):
    //   兼ねていた間、10 ある手のうち柱と槍以外はすべて同じ絵で出ていた。しかも
    //   «向きで分ける» という唯一の区別は PlayGroundDust が y 成分を捨てるせいで
    //   縦を表せず、実際には機能していなかった。呼ぶ側が «どの手か» を言えば、
    //   シルエット (縦 / 帯 / 放射 / 線 / 光) がそのまま読み分けになる。

    /// 床の口から噴き上がる縦の柱。突き上げの着弾で使う。
    /// @param center 口の中心 (床面)。scale は 1.0 で既定の太さ
    void PlaySerpentGeyser(const Vector3& center, float strength01, float scale = 1.0f);
    /// 胴が口を出入りしている。突き上げより小さく、別のリングで回す。
    void PlaySerpentMouth(const Vector3& center, float strength01);
    /// 11 m の胴が落ちて床を叩いた。from / to は弧の 2 つの足元 (床面)。
    void PlaySerpentSlam(const Vector3& from, const Vector3& to, float strength01);
    /// 檻が角へ寄って砕けた。corner は 2 枚の壁が共有する口 (床面)。
    void PlaySerpentSnap(const Vector3& corner, float strength01);
    /// 走る胴が床を削った 1 点。forward は走っている向き (水平)。
    void PlaySerpentRush(const Vector3& point, const Vector3& forward, float strength01);
    /// 噛みつき。charge=true で喉の溜め、false で外した着弾。forward は噛んだ向き。
    void PlaySerpentBite(const Vector3& point, const Vector3& forward, bool charge);

    void OnStart()   override;
    void OnUpdate()  override;
    void OnDestroy() override;

private:
    /// .vfx 1 種類ぶんの枠のリング。枠の実体はその .vfx プレファブのインスタンス。
    struct Pool {
        std::string vfxPath;
        /// 同じ .vfx を «別の用途» として分けて回すための札。空なら本来の使い道。
        ///
        /// WHY 要るか: 衝撃波は 1 回で十数発の爆発を撒く。当たりの爆発と同じリングを
        ///     使うと、波が走っているあいだ本当にぶつかった爆発がすべて押し出される。
        ///     出どころが違えば «込み合い» も別に数えるべきで、それは枠を分けること。
        std::string tag;
        std::vector<EntityRef> slots;
        int next = 0;
        /// この種類だけの枠数。0 なら slotsPerEffect に従う。
        ///
        /// WHY 種類ごとに変えられるようにするか: 枠の数は «1 発の寿命 × 鳴る頻度» で
        ///     決まる量で、衝突の爆発 (数発) と衝撃波の土煙 (数十発) では桁が違う。
        ///     全体を土煙に合わせて増やすと、一度も込み合わない演出まで枠を抱える。
        int capacity = 0;
    };

    static inline VfxManagerComponent* s_instance = nullptr;

    [[nodiscard]] std::string PathOf(const VFXRef& reference, const char* fallback) const;
    [[nodiscard]] Pool& PoolFor(const std::string& vfxPath, const char* poolTag, int capacity);
    /// 爆発 1 発。PlayImpact と PlayGroundBlast の違いは «どのリングで回すか» だけ。
    void Blast(const Vector3& point, BladeSide side, float strength01, bool againstAnchor,
               const char* poolTag, int capacity, const char* debugName);
    [[nodiscard]] GameObject* AcquireSlot(Pool& pool);
    /// 枠を 1 つ確保し、位置と回転を合わせて返す。まだ鳴らさない。
    ///
    /// WHY 鳴らす前に返すか: 1 発ぶんの値 (色・体の寸法・当たりの強さ) は
    ///     呼び出し側にしか無く、種類ごとに型が違う。値を書き込んでから Restart する
    ///     必要があるので、«確保» と «発火» を分ける。
    /// @param poolTag 空でなければ、同じ .vfx を別のリングで回す。
    /// @param capacity 0 でこの種類の枠数を slotsPerEffect に任せる。
    [[nodiscard]] GameObject* Prepare(const std::string& vfxPath, const Vector3& position,
                                      const Quaternion& rotation, const char* debugName,
                                      const char* poolTag = "", int capacity = 0);
    /// 頭出しして鳴らす。Prepare で得た枠へ値を書いた後に呼ぶ。
    static void Fire(GameObject& root);
    void ReleaseSlots();
    /// 同フレーム何発目か。重なった 2 発目以降を減衰させるために数える。
    [[nodiscard]] int NextImpactIndexThisFrame();

    std::vector<Pool> m_pools;
    std::uint64_t m_impactFrame = 0;
    int           m_impactsThisFrame = 0;
    /// 光が膨らんだことを 1 度だけ言うための札。
    bool          m_warnedLights = false;
};

FBZZ_REFLECT(VfxManagerComponent)

// ── 実装 (inline) ─────────────────────────────────────────────────────────────

inline void VfxManagerComponent::OnUpdate()
{
    if (!censusLights) return;

    // 30 フレームに 1 回で足りる。光が «徐々に増える» 不具合を見つけるのが目的で、
    // 1 フレームの正確さは要らない (毎フレーム全走査すると数える側が重い)。
    if ((Time::frameCount % 30) != 0) return;

    // WHY LightComponent を型で引けるか: FindObjectsOfType<T> は Script 派生でない型を
    //     ECS の GetEntities<T> へ流す (Script.hpp の分岐)。IBoss のような
    //     «横断インターフェース» とは経路が違うので、こちらは素直に引ける。
    const std::vector<GameObject*> lights = scene.FindObjectsOfType<LightComponent>();
    debugLightsTotal = static_cast<int>(lights.size());

    int lit = 0;
    for (GameObject* object : lights) {
        if (!object || !object->activeInHierarchy()) continue;
        const auto* light = object->GetComponent<LightComponent>();
        if (light && light->enabled) ++lit;
    }
    debugLightsLit = lit;

    if (m_warnedLights || lit <= std::max(censusWarnAt, 8)) return;
    m_warnedLights = true;

    // 内訳は «名前の頭» で束ねる。実行時に増える光はどれも «元の名前 + 連番» で
    // 作られるので、頭を見れば «どのスクリプトが» まで一息で分かる。
    std::vector<std::pair<std::string, int>> byPrefix;
    for (GameObject* object : lights) {
        if (!object || !object->activeInHierarchy()) continue;
        const auto* light = object->GetComponent<LightComponent>();
        if (!light || !light->enabled) continue;

        std::string prefix = object->name;
        const std::size_t cut = prefix.find_last_of("_0123456789");
        if (cut != std::string::npos && cut > 0) prefix = prefix.substr(0, cut);

        auto found = std::find_if(byPrefix.begin(), byPrefix.end(),
                                  [&](const auto& e) { return e.first == prefix; });
        if (found == byPrefix.end()) byPrefix.emplace_back(prefix, 1);
        else ++found->second;
    }
    std::sort(byPrefix.begin(), byPrefix.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });

    std::string report;
    for (std::size_t i = 0; i < byPrefix.size() && i < 6; ++i)
        report += (i ? ", " : "") + byPrefix[i].first + " x" +
                  std::to_string(byPrefix[i].second);

    debug.LogWarning("VfxManagerComponent: " + std::to_string(lit) +
                     " lights are lit (clusters hold 64). Breakdown: " + report +
                     ". Anything that grows run after run is not being released.");
}

inline void VfxManagerComponent::OnStart()
{
    if (s_instance && s_instance != this) {
        debug.LogWarning("VfxManagerComponent: another instance is already active. "
                         "The most recently started one takes over.");
    }
    s_instance = this;
    // 前回 Play / DLL リロードの枠はここへ来る前に OnDestroy で畳まれている。
    // 畳めていない経路 (Play 中の再読込) が残っても、AcquireSlot が名前で拾い直す。
    m_pools.clear();
    m_impactFrame      = 0;
    m_impactsThisFrame = 0;
    debugSlots = 0;
    m_warnedLights = false;
    debugLast.clear();
}

inline void VfxManagerComponent::OnDestroy()
{
    ReleaseSlots();
    if (s_instance == this) s_instance = nullptr;
}

inline void VfxManagerComponent::ReleaseSlots()
{
    // 枠はルートに置いてあるので、このスクリプトが消えても一緒には消えない。持ち主が畳む。
    for (Pool& pool : m_pools) {
        for (EntityRef& slot : pool.slots)
            if (GameObject* object = slot.Resolve(scene)) scene.Destroy(*object);
        pool.slots.clear();
    }
    m_pools.clear();
    debugSlots = 0;
    m_warnedLights = false;
}

inline std::string VfxManagerComponent::PathOf(const VFXRef& reference, const char* fallback) const
{
    std::string path = reference.ResolvePath();
    return path.empty() ? std::string(fallback) : path;
}

inline VfxManagerComponent::Pool& VfxManagerComponent::PoolFor(const std::string& vfxPath,
                                                               const char* poolTag, int capacity)
{
    const std::string tag = poolTag ? poolTag : "";
    const auto found = std::find_if(m_pools.begin(), m_pools.end(),
        [&](const Pool& pool) { return pool.vfxPath == vfxPath && pool.tag == tag; });
    if (found != m_pools.end()) {
        // 枠数は «この種類が何発同時に鳴るか» なので、Inspector で動かせば次の 1 発から効く。
        found->capacity = capacity;
        return *found;
    }

    m_pools.push_back(Pool{ vfxPath, tag, {}, 0, capacity });
    return m_pools.back();
}

inline GameObject* VfxManagerComponent::AcquireSlot(Pool& pool)
{
    const int capacity = pool.capacity > 0 ? std::clamp(pool.capacity, 1, 96)
                                           : std::clamp(slotsPerEffect, 1, 32);

    // 上限に届くまでは «使うときに 1 つ足す»。開幕に capacity 個並べると、一度も
    // 鳴らない演出のぶんまで Hierarchy に空の枠が残り、シーンを覗いたときに邪魔になる。
    if (static_cast<int>(pool.slots.size()) < capacity) {
        // WHY 名前で拾い直すか: スクリプト DLL をリロードするとこの Script は作り直され、
        //     EntityRef は空に戻る。一方で枠の GameObject は Scene 側に残っているため、
        //     拾わずに作ると、リロードのたびに枠が capacity 個ずつ増えていく。
        const std::size_t stemStart = pool.vfxPath.find_last_of("/\\") + 1;
        const std::size_t stemEnd   = pool.vfxPath.find_last_of('.');
        const std::string stem = pool.vfxPath.substr(
            stemStart, stemEnd == std::string::npos ? std::string::npos : stemEnd - stemStart);
        // 札の付いた枠は別の名前で置く。同名だと «同じ .vfx を 2 つのリングで回す»
        // 構成で、片方が作った枠をもう片方が拾い上げて共有してしまう。
        const std::string suffix = pool.tag.empty() ? std::string{} : "_" + pool.tag;
        const std::string name = "VFX_" + stem + suffix + "_" +
                                 std::to_string(pool.slots.size());

        GameObject* object = scene.Find(name);
        if (!object) {
            // .vfx はプレファブなので «展開して置く» のが生成そのもの。
            // scene.Spawn は PrefabPool 経由なので、2 度目以降はファイル読み込みを通らない。
            object = scene.Spawn(pool.vfxPath, Vector3::ZERO, Quaternion::Identity());
            if (!object) return nullptr;
            object->name = name;
            object->runtimeGenerated = true;
        }

        // WHY autoDestroy を降ろすか: 枠の寿命はこのマネージャーが持つ。
        //     VFXSystem に畳ませると、鳴り終わった枠がプールへ返ってしまい、
        //     こちらが握っている EntityRef が «別の演出として貸し出された実体» を指す。
        if (auto* vfx = object->GetComponent<VFXComponent>()) {
            vfx->autoDestroy = false;
            vfx->playOnAwake = false;
            vfx->loop        = false;
        }

        pool.slots.push_back(EntityRef{ object->GetID() });
        ++debugSlots;
        return object;
    }

    // 埋まっているので最も古い枠を奪う。next は常に «次に使う = 最も古い» を指す。
    EntityRef& slot = pool.slots[static_cast<std::size_t>(pool.next)];
    pool.next = (pool.next + 1) % static_cast<int>(pool.slots.size());
    return slot.Resolve(scene);
}

inline GameObject* VfxManagerComponent::Prepare(const std::string& vfxPath,
                                                const Vector3& position,
                                                const Quaternion& rotation,
                                                const char* debugName,
                                                const char* poolTag, int capacity)
{
    if (!enabled || vfxPath.empty()) return nullptr;

    GameObject* object = AcquireSlot(PoolFor(vfxPath, poolTag, capacity));
    if (!object) return nullptr;

    // 枠はルートに置いてある (親が居ないのでローカル = ワールド)。
    // 親に付けると、持ち主が動いた瞬間に既に出ている粒ごと引きずられる。
    object->transform.position      = position;
    object->transform.worldPosition = position;
    object->transform.rotation      = rotation;
    object->transform.worldRotation = rotation;
    object->SetActive(true);

    debugLast = debugName;
    return object;
}

inline void VfxManagerComponent::Fire(GameObject& root)
{
    // WHY 作り直しが要らなくなったか: 旧実装はパラメーターがノード生成時に 1 度だけ
    //     焼き込まれる作りだったため、値を変えるには毎回 reload が必要だった。
    //     いまは値の行き先が実体のコンポーネントそのものなので、書いて頭出しすれば済む。
    if (auto* vfx = root.GetComponent<VFXComponent>()) vfx->Restart();
}

inline int VfxManagerComponent::NextImpactIndexThisFrame()
{
    if (m_impactFrame != Time::frameCount) {
        m_impactFrame      = Time::frameCount;
        m_impactsThisFrame = 0;
    }
    return m_impactsThisFrame++;
}

inline void VfxManagerComponent::PlayImpact(const Vector3& point, BladeSide side,
                                            float strength01, bool againstAnchor)
{
    Blast(point, side, strength01, againstAnchor, /*poolTag=*/"", /*capacity=*/0,
          againstAnchor ? "AnchorImpact" : "BodyImpact");
}

inline void VfxManagerComponent::PlayGroundBlast(const Vector3& point, BladeSide side,
                                                 float strength01)
{
    // 床が砕けた爆発なので跡は «ひび»。焦げにすると、波が通った床が焼けたことになる。
    Blast(point, side, strength01, /*againstAnchor=*/true, kGroundPool,
          groundBlastSlots, "GroundBlast");
}

inline void VfxManagerComponent::Blast(const Vector3& point, BladeSide side, float strength01,
                                       bool againstAnchor, const char* poolTag, int capacity,
                                       const char* debugName)
{
    const float strength = Clamp01(strength01);
    const int   index    = NextImpactIndexThisFrame();

    // 衝撃波は 1 回で十数発撒く。近い場所で光源が重なると画面が白へ抜けて、
    // «どこで何が起きたか» がその瞬間だけ読めなくなる。
    //
    // WHY «最も強い 1 発» ではなく «先着» を明るくするか: 演出は出来事を受け取ったその場で
    //     鳴らすので、このフレームにあと何発来るかを知る術が無い。1 フレーム貯めてから
    //     並べ替えれば «最も強い 1 発» を選べるが、そのぶん爆発が 1 フレーム遅れて、
    //     音とヒットストップだけが先に来る。手触りは遅延の方に強く出る。
    const float crowdFade = index < std::max(lightFalloffCount, 1)
        ? 1.0f : 1.0f / static_cast<float>(index - lightFalloffCount + 2);

    GameObject* root = Prepare(PathOf(impactVfx, kVfxImpactPath), point, Quaternion::Identity(),
                               debugName, poolTag, capacity);
    if (!root) return;

    if (auto* params = root->GetScript<ImpactVfxComponent>()) {
        // 刀と結び付かない爆発 (床を割る衝撃波・流れ弾) は BladeSide::None で無彩色になる。
        params->tintColor  = BladeColor(side);
        params->sparkPower = Lerp(sparkPowerMin, sparkPowerMax, strength);
        params->blastLight = Lerp(blastLightMin, blastLightMax, strength) * crowdFade;
        // 柱・壁・床を割った跡は焦げではなく «ひび»。
        params->groundMark = againstAnchor ? kMarkCrack : kMarkScorch;
        params->Apply();
    }
    Fire(*root);
}

inline void VfxManagerComponent::PlayBeamScorch(const Vector3& point, const Vector3& normal,
                                                BladeSide side, float searSize)
{
    // 火花も煙も面の «外» へ吹かせる。Cone はローカル +Z へ吹くので、法線を向く回転を渡す。
    const Vector3 axis = normal.NormalizedOr(Vector3::UP);
    // LookRotation は forward と up が平行だと基底を作れない。床と天井でだけ姿勢が
    // 飛ぶので、そこだけ up を替える。
    const Vector3 up = Abs(Vector3::Dot(axis, Vector3::UP)) > 0.99f
        ? Vector3::FORWARD : Vector3::UP;

    // 面から少しだけ浮かせる。面上ちょうどに置くと、粒の半分が受け面へ潜って
    // 焼け跡が «欠けた円» になる。
    GameObject* root = Prepare(PathOf(beamScorchVfx, kVfxBeamScorchPath), point + axis * 0.03f,
                               Quaternion::LookRotation(axis, up), "BeamScorch");
    if (!root) return;

    if (auto* params = root->GetScript<BeamScorchVfxComponent>()) {
        // 色が乗るのは電弧の層だけ (VFX/Game/README.md)。火花と煙は熱の色のまま。
        params->tintColor = BladeColor(side);
        params->emberRate = std::max(emberRate, 0.0f);
        // 光る範囲を焦げの大きさへ合わせる。ずれると «焦げの外側が光っている» ように見える。
        params->searSize = std::clamp(searSize, 0.05f, 2.0f);
        params->Apply();
    }
    Fire(*root);
}

inline void VfxManagerComponent::PlayRunDust(const Vector3& footPoint,
                                             const Vector3& moveDirection, float strength01)
{
    const float strength = Clamp01(strength01);

    // 土煙は «足が後ろへ掻いた» 側へ残る。層の Cone はローカル +Z へ吹くので、
    // 進行方向の逆を向く回転を渡す。水平へ倒してから正規化するので、
    // LookRotation の基底が上向きと平行になる経路は無い。
    Vector3 wake = -moveDirection;
    wake.y = 0.0f;
    wake = wake.NormalizedOr(Vector3::FORWARD);

    GameObject* root = Prepare(PathOf(runDustVfx, kVfxRunDustPath), footPoint,
                               Quaternion::LookRotation(wake, Vector3::UP), "RunDust");
    if (!root) return;

    if (auto* params = root->GetScript<RunDustVfxComponent>()) {
        params->dustColor = runDustColor;
        params->kickSpeed = Lerp(runDustKickMin, runDustKickMax, strength);
        params->puffSize  = Lerp(runDustSizeMin, runDustSizeMax, strength);
        params->Apply();
    }
    Fire(*root);
}

inline void VfxManagerComponent::PlayParry(const Vector3& point, const Vector3& away,
                                           float strength01, bool just)
{
    const float strength = Clamp01(strength01);

    // 破片は攻撃してきた側から «逃げる» 向きへ。層は球で撒くので向きは薄く効くだけだが、
    // 光条 (Cross) の向きがこれで決まり、«どちらから来た一撃を弾いたか» が残る。
    Vector3 flow{ away.x, away.y * 0.3f, away.z };
    flow = flow.NormalizedOr(Vector3::FORWARD);
    const Vector3 up = Abs(Vector3::Dot(flow, Vector3::UP)) > 0.99f ? Vector3::FORWARD : Vector3::UP;

    // Just は別の .vfx (層が多い) を別のリングで回す。同じリングに入れると、Just の
    // 直後に軽い弾きが鳴ったとき 0.45 秒残る光条が頭出しで消される。
    const std::string path = just ? PathOf(parryJustVfx, kVfxParryJustPath)
                                  : PathOf(parryVfx, kVfxParryPath);
    GameObject* root = Prepare(path, point, Quaternion::LookRotation(flow, up),
                               just ? "ParryJust" : "Parry");
    if (!root) return;

    // Just は «読み切った» ぶん一段強く。通常の最大 (heavy) と同じ値では区別が付かない。
    const float boost = just ? 1.35f : 1.0f;
    if (auto* params = root->GetScript<ParryVfxComponent>()) {
        params->sparkPower = Lerp(parrySparkMin, parrySparkMax, strength) * boost;
        params->flashLight = Lerp(parryLightMin, parryLightMax, strength) * boost;
        params->ringSize   = Lerp(parryRingMin, parryRingMax, strength) * boost;
        params->Apply();
    }
    Fire(*root);
}

inline void VfxManagerComponent::PlaySlashHit(const Vector3& point, const Vector3& away,
                                              float strength01, BladeSide side)
{
    const float strength = Clamp01(strength01);

    Vector3 flow{ away.x, away.y * 0.3f, away.z };
    flow = flow.NormalizedOr(Vector3::FORWARD);
    const Vector3 up = Abs(Vector3::Dot(flow, Vector3::UP)) > 0.99f ? Vector3::FORWARD : Vector3::UP;

    GameObject* root = Prepare(PathOf(slashHitVfx, kVfxSlashHitPath), point,
                               Quaternion::LookRotation(flow, up), "SlashHit");
    if (!root) return;

    if (auto* params = root->GetScript<SlashHitVfxComponent>()) {
        // 弧と光が振った刀の色になり、«どちらの刀で斬ったか» が跡に残る。
        params->tint       = side == BladeSide::None ? Vector4{ 1.0f, 1.0f, 1.0f, 1.0f }
                                                     : BladeColor(side);
        params->sparkPower = Lerp(slashSparkMin, slashSparkMax, strength);
        params->flashLight = Lerp(slashLightMin, slashLightMax, strength);
        params->arcSize    = Lerp(slashArcMin, slashArcMax, strength);
        params->Apply();
    }
    Fire(*root);
}

inline void VfxManagerComponent::PlayTopple(const Vector3& point, float scale)
{
    const float size = std::max(scale, 0.05f);

    // 転倒は床の出来事なので、枠は床のリング (kGroundPool) と同じ数え方で 1 種類だけ持つ。
    GameObject* root = Prepare(PathOf(toppleVfx, kVfxTopplePath), point,
                               Quaternion::Identity(), "Topple", "", 2);
    if (!root) return;

    if (auto* params = root->GetScript<ToppleVfxComponent>()) {
        params->dustColor = runDustColor;
        params->dustColor.w = std::max(runDustColor.w, 0.45f);
        params->scale     = size;
        params->crackSize = std::max(toppleCrack, 0.5f) * size;
        params->thudLight = std::max(toppleLight, 0.0f);
        params->Apply();
    }
    Fire(*root);
}

inline void VfxManagerComponent::PlayExecute(const Vector3& point, const Vector3& away, float scale)
{
    const float size = std::max(scale, 0.05f);

    Vector3 flow{ away.x, away.y * 0.3f, away.z };
    flow = flow.NormalizedOr(Vector3::FORWARD);
    const Vector3 up = Abs(Vector3::Dot(flow, Vector3::UP)) > 0.99f ? Vector3::FORWARD : Vector3::UP;

    GameObject* root = Prepare(PathOf(executeVfx, kVfxExecutePath), point,
                               Quaternion::LookRotation(flow, up), "Execute", "", 2);
    if (!root) return;

    if (auto* params = root->GetScript<ExecuteVfxComponent>()) {
        params->sparkPower = std::max(executeSpark, 0.0f) * Lerp(0.8f, 1.0f, Clamp01(size));
        params->cutLight   = std::max(executeLight, 0.0f);
        params->cutSize    = std::max(executeCut, 0.1f) * size;
        params->Apply();
    }
    Fire(*root);
}

inline void VfxManagerComponent::PlaySerpentGeyser(const Vector3& center, float strength01,
                                                   float scale)
{
    const float strength = Clamp01(strength01);
    const float size     = std::max(scale, 0.05f);

    // 姿勢は素のまま。噴き上がりは «真上» だけが向きなので、渡す回転は恒等でよい
    // ─ ここが LookRotation を使わない唯一の口で、その理由がそのまま
    //   PlayGroundDust で縦が出せなかった理由でもある。
    GameObject* root = Prepare(PathOf(serpentGeyserVfx, kVfxSerpentGeyserPath), center,
                               Quaternion::Identity(), "SerpentGeyser", kGeyserThrustPool,
                               geyserSlots);
    if (!root) return;

    if (auto* params = root->GetScript<SerpentGeyserVfxComponent>()) {
        params->dustColor   = runDustColor;
        params->riseSpeed   = 11.0f * size * Lerp(0.7f, 1.0f, strength);
        params->columnWidth = 1.6f * size;
        params->skirtSpread = 2.2f * size;
        params->emberPower  = 8.0f * Lerp(0.5f, 1.0f, strength);
        params->rimLight    = 7.0f * Lerp(0.4f, 1.0f, strength);
        params->Apply();
    }
    Fire(*root);
}

inline void VfxManagerComponent::PlaySerpentMouth(const Vector3& center, float strength01)
{
    const float strength = Clamp01(strength01);

    GameObject* root = Prepare(PathOf(serpentGeyserVfx, kVfxSerpentGeyserPath), center,
                               Quaternion::Identity(), "SerpentMouth", kGeyserMouthPool,
                               geyserSlots);
    if (!root) return;

    if (auto* params = root->GetScript<SerpentGeyserVfxComponent>()) {
        params->dustColor = runDustColor;
        // 出入りは «押し退けられた床» で、噴き上がった手ではない。低く・広く・暗く。
        params->riseSpeed   = 4.2f * Lerp(0.6f, 1.0f, strength);
        params->columnWidth = 1.1f;
        params->skirtSpread = 2.6f;
        params->emberPower  = 3.0f * strength;
        params->rimLight    = 2.0f * strength;
        params->Apply();
    }
    Fire(*root);
}

inline void VfxManagerComponent::PlaySerpentSlam(const Vector3& from, const Vector3& to,
                                                 float strength01)
{
    const float strength = Clamp01(strength01);

    const Vector3 flat{ to.x - from.x, 0.0f, to.z - from.z };
    const float   length = flat.Length();
    // 層は «ローカル +Z が帯の向き»。2 つの足元を結ぶ線を向かせれば、あとは
    // 長さを Z へ伸ばすだけで帯になる。
    const Vector3 axis = length > EPSILON ? flat / length : Vector3::FORWARD;
    const Vector3 mid{ (from.x + to.x) * 0.5f, (from.y + to.y) * 0.5f, (from.z + to.z) * 0.5f };

    GameObject* root = Prepare(PathOf(serpentSlamVfx, kVfxSerpentSlamPath), mid,
                               Quaternion::LookRotation(axis, Vector3::UP), "SerpentSlam",
                               "", 3);
    if (!root) return;

    if (auto* params = root->GetScript<SerpentSlamVfxComponent>()) {
        params->dustColor    = runDustColor;
        params->dustColor.w  = std::max(runDustColor.w, 0.5f);
        params->bandLength   = std::max(length, 1.0f);
        params->scorchLength = std::max(length, 1.0f);
        params->kickSpeed    = 5.5f * Lerp(0.65f, 1.0f, strength);
        params->thudLight    = 6.0f * Lerp(0.5f, 1.0f, strength);
        params->Apply();
    }
    Fire(*root);
}

inline void VfxManagerComponent::PlaySerpentSnap(const Vector3& corner, float strength01)
{
    const float strength = Clamp01(strength01);

    GameObject* root = Prepare(PathOf(serpentSnapVfx, kVfxSerpentSnapPath), corner,
                               Quaternion::Identity(), "SerpentSnap", "", 2);
    if (!root) return;

    if (auto* params = root->GetScript<SerpentSnapVfxComponent>()) {
        params->dustColor    = runDustColor;
        params->dustColor.w  = std::max(runDustColor.w, 0.55f);
        params->outwardSpeed = 9.0f * Lerp(0.7f, 1.0f, strength);
        params->sparkPower   = 12.0f * Lerp(0.6f, 1.0f, strength);
        params->snapLight    = 9.0f * Lerp(0.5f, 1.0f, strength);
        params->Apply();
    }
    Fire(*root);
}

inline void VfxManagerComponent::PlaySerpentRush(const Vector3& point, const Vector3& forward,
                                                 float strength01)
{
    const float strength = Clamp01(strength01);

    // 層は «ローカル +Z が走っている向き»。水平へ倒してから正規化するので、
    // LookRotation の基底が上向きと平行になる経路は無い。
    Vector3 flow{ forward.x, 0.0f, forward.z };
    flow = flow.NormalizedOr(Vector3::FORWARD);

    GameObject* root = Prepare(PathOf(serpentRushVfx, kVfxSerpentRushPath), point,
                               Quaternion::LookRotation(flow, Vector3::UP), "SerpentRush",
                               "", rushSlots);
    if (!root) return;

    if (auto* params = root->GetScript<SerpentRushVfxComponent>()) {
        params->dustColor  = runDustColor;
        params->wakeSpeed  = 7.0f * Lerp(0.6f, 1.0f, strength);
        params->puffSize   = 1.5f * Lerp(0.75f, 1.0f, strength);
        params->sparkPower = 14.0f * strength;
        params->Apply();
    }
    Fire(*root);
}

inline void VfxManagerComponent::PlaySerpentBite(const Vector3& point, const Vector3& forward,
                                                 bool charge)
{
    Vector3 flow{ forward.x, forward.y * 0.4f, forward.z };
    flow = flow.NormalizedOr(Vector3::FORWARD);
    const Vector3 up = Abs(Vector3::Dot(flow, Vector3::UP)) > 0.99f
        ? Vector3::FORWARD : Vector3::UP;

    // WHY 溜めと着弾でリングを分けるか: 溜め (0.7 秒) が終わる前に着弾が鳴ることは
    //     無いが、外して硬直している間に次の噛みつきの溜めが始まることはある。
    //     同じリングだと着弾の煙が溜めに押し出されて «外したのに跡が残らない» になる。
    GameObject* root = Prepare(PathOf(serpentBiteVfx, kVfxSerpentBitePath), point,
                               Quaternion::LookRotation(flow, up),
                               charge ? "SerpentBiteCharge" : "SerpentBiteImpact",
                               charge ? "Charge" : "Impact", 2);
    if (!root) return;

    if (auto* params = root->GetScript<SerpentBiteVfxComponent>()) {
        params->dustColor  = runDustColor;
        params->impactPose = !charge;
        params->Apply();
    }
    Fire(*root);
}

inline void VfxManagerComponent::PlayLegSmoke(const Vector3& point, const Vector3& drift,
                                              float strength01, float scale)
{
    const float strength = Clamp01(strength01);
    const float size     = std::max(scale, 0.05f);

    // 層の Cone はローカル +Z へ吹く。煙の主役は «上» なので、+Z を真上へ向けて
    // 流れる向きは «枠の上» として渡す ── こうすると横のばらけが drift 側へ寄る。
    Vector3 side{ drift.x, 0.0f, drift.z };
    side = side.NormalizedOr(Vector3::FORWARD);

    GameObject* root = Prepare(PathOf(groundDustVfx, kVfxGroundDustPath), point,
                               Quaternion::LookRotation(Vector3::UP, side), "LegSmoke",
                               kLegSmokePool, legSmokeSlots);
    if (!root) return;

    if (auto* params = root->GetScript<RunDustVfxComponent>()) {
        params->dustColor = legSmokeColor;
        params->puffSize  = std::max(legSmokeSize, 0.05f) * size * Lerp(0.7f, 1.0f, strength);
        params->kickSpeed = std::max(legSmokeRise, 0.0f) * Lerp(0.6f, 1.0f, strength);
        // 砂粒は床の話。機械の破断面から «土» が飛ぶと、壊れたのが脚だと読めなくなる。
        params->gritPower = 0.0f;
        params->Apply();
    }
    Fire(*root);
}

inline void VfxManagerComponent::PlayGroundDust(const Vector3& point, const Vector3& outward,
                                                float strength01, float scale)
{
    const float strength = Clamp01(strength01);
    const float size     = std::max(scale, 0.05f);

    // 層の Cone はローカル +Z へ吹く。押し退けられた側 (outward) をそのまま向かせる。
    Vector3 flow{ outward.x, 0.0f, outward.z };
    flow = flow.NormalizedOr(Vector3::FORWARD);

    GameObject* root = Prepare(PathOf(groundDustVfx, kVfxGroundDustPath), point,
                               Quaternion::LookRotation(flow, Vector3::UP), "GroundDust",
                               kGroundPool, groundDustSlots);
    if (!root) return;

    if (auto* params = root->GetScript<RunDustVfxComponent>()) {
        params->dustColor = runDustColor;
        // WHY 強さで «0 まで» 落とさないか: 弱く踏んだ 1 歩でも床は鳴っている。
        //     0 に近づけると «歩いているのに何も出ない» フレームができ、
        //     出たり出なかったりする方が抜けとして目立つ。
        params->puffSize  = std::max(groundDustSize, 0.05f) * size * Lerp(0.62f, 1.0f, strength);
        params->kickSpeed = std::max(groundDustKick, 0.0f) * size * Lerp(0.45f, 1.0f, strength);
        params->gritPower = std::max(groundDustGrit, 0.0f);
        params->Apply();
    }
    Fire(*root);
}

} // namespace sandbox
