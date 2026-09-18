/// @file    BossShockwaveComponent.hpp
/// @brief   ボスの着地から地面を走る円形衝撃波。跳ばないと越えられない
/// @author  Hasegawa Jin
/// @date    2026-08-27
///
/// @note 波は落下点の直撃 (BossAi の Shock Radius) とは別の当たりで外側だけを担当する。
///       横移動する攻撃は既に3つあるため円形にして跳躍でしか避けられないようにし、
///       «越えた» は接地フラグ (猶予があり跳んだ直後や降り際に入れ替わる) でなく
///       波面からの高さで決める。
/// @note 姿は煙と爆発だけで作り光る輪は持たない (半透明の筒は «描いてある» としか
///       読めない)。判定は半径と帯幅だけが持ち、粒がばらける煙の絵には依らない。
/// @note 密度は VfxManager の枠 (スロット) の奪い合いで決まり、増やすと逆にリングが
///       古い枠を奪って1発ずつ育つ前に消える。前縁の線は枠を食わない亀裂 (デカール)
///       に任せ、煙は数でなく «育ちきった1発の大きさ» で効かせる。点は自分へ向かって
///       くる弧だけへ寄せ、背後は薄くても «円の形» のために残す。
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

    /// 波の発生音はここから鳴らす。無ければ OnStart が自分で足す。
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

    /// @note 等速をやめる。生まれた直後の1秒未満だけ速くすると «来る» の圧が出て
    ///       判断の時刻が前へ寄る。倍率は必ず1以上 (Speed が保証する «走って
    ///       逃げ切れない» という下限を、減速側へ振ると崩してしまうため)。
    FBZZ_GROUP("バースト")
    /// @note 8m先のプレイヤーへの到達が0.37秒だと人の反応(約0.25秒)+跳躍発生を
    ///       足すと近距離で «見えても跳べない»。0.48秒まで緩めても Speed が保証する
    ///       «走って逃げ切れない» は残る。
    FBZZ_FIELD_RANGE(float, burstMultiplier, 1.5f, "Initial Speed x", 1.0f, 6.0f)
    FBZZ_TOOLTIP("生まれた瞬間の速さ倍率。1 で等速。上げるほど近距離が跳べなくなる")
    FBZZ_FIELD_RANGE(float, burstFalloff, 5.0f, "Falloff", 0.5f, 20.0f)
    FBZZ_TOOLTIP("倍率が Speed へ落ち着く速さ。大きいほど早く終端速度になる")

    /// @note 着地は別に鳴らす。波を «たどって» 出す煙は1発ずつ小さくないと輪にならないが、
    ///       着地の瞬間は1箇所で全部が起きるため、同じ強さだと波の途中と区別できない。
    FBZZ_GROUP("Landing")
    FBZZ_FIELD_RANGE(float, landingBlast, 1.0f, "Blast", 0.0f, 1.0f)
    FBZZ_TOOLTIP("落下点の真ん中で出す爆発の強さ。0 で出さない")
    FBZZ_FIELD_RANGE_INT(int, landingBlastCount, 4, "Blast Ring", 0, 8)
    FBZZ_TOOLTIP("中心の爆発を囲んで足す数。1 発だけだと «点» で終わるので、"
                 "腹の下いっぱいが割れた大きさを、囲む数で作る")
    FBZZ_FIELD_RANGE(float, landingBlastSpread, 3.2f, "Blast Ring Radius", 0.5f, 12.0f)
    FBZZ_TOOLTIP("囲む爆発を置く半径 [m]。ボスの腹の差し渡しに合わせる")
    /// @note 枠は1回の波ぶんの総数で決まるため、着地で使いすぎると走り出した後の
    ///       輪へ回す枠が残らない。
    FBZZ_FIELD_RANGE_INT(int, landingSmokeCount, 10, "Smoke Points", 0, 32)
    FBZZ_TOOLTIP("落下点を囲む土煙の数。輪の «生まれた瞬間» を作る")
    FBZZ_FIELD_RANGE(float, landingSmokeScale, 1.9f, "煙のスケール", 0.2f, 4.0f)
    FBZZ_TOOLTIP("VfxManager の Ground Dust に掛ける倍率。波の途中より大きく置く")

    /// @note 着地の煙はボスのシルエット (胴体) の外、腹の «縁» へ出す。真下に置くと
    ///       6m級の胴体に隠れて «出ているのに一度も見えない» 状態になる。

    /// @note 輪は «少しずつ撒き続ける» でなく «1 枚ずつ置く»。半径が連続して伸びる間に
    ///       少数ずつ撒くと点が «回りながら外へ» 並び渦にしか見えないため、同じ半径へ
    ///       一斉に円周を均等分割して撒く。点数は固定でなく円周に比例させる ─ 固定だと
    ///       生まれたては密集し広がった輪はスカスカになるため。
    /// @note 点数に上限が要る。円周は半径に比例して伸びるため、間隔を保つと外周では
    ///       際限なく増え、VfxManager の枠 (Ground Dust Slots) を超えた瞬間に古い輪が
    ///       欠け始める。輪は «時間» でなく «進んだ距離» で置く。出だしは速さが2倍近く
    ///       あるため、時間刻みだと一番危ない出だしの段間隔が一番スカスカになる。
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
    /// @note 増やしても密にはならない。土煙1発は波の全長とほぼ同じ1.6秒枠を占有するため
    ///       «1回の波で撒いた総数 = 同時生存数»。枠を超えるとリングが最も古い枠を
    ///       奪って作り直すので、点を増やすほど1発ずつ育ちきる前に消える逆効果になる。
    ///       前縁の «線» は枠を食わない亀裂 (デカール) に任せる。
    FBZZ_FIELD_RANGE_INT(int, ringMaxPoints, 8, "Max Points", 3, 32)
    FBZZ_TOOLTIP("1 枚あたりの上限。«Ground Dust Slots ÷ 1 回の波の輪の枚数» を"
                 "超えると、外周へ届く前に内側の輪が欠け始める")
    /// @note 点数を絞った (16→8) ため、散らす幅も狭くしないと前縁から引っ込むだけで
    ///       線が読めなくなる。
    FBZZ_FIELD_RANGE(float, ringJitter, 0.25f, "Jitter", 0.0f, 3.0f)
    FBZZ_TOOLTIP("点を内側へ散らす幅 [m]。0 だと真円すぎて «描いた図形» に見える。"
                 "外側へは散らさない ─ 当たらない場所に絵が出ないため")
    FBZZ_FIELD_RANGE(float, smokeScale, 1.3f, "煙のスケール", 0.2f, 4.0f)
    FBZZ_TOOLTIP("点を減らしたぶん 1 発を大きくする。数ではなく大きさで輪を作る")
    FBZZ_FIELD_RANGE(float, smokeStrength, 1.0f, "Smoke Strength", 0.0f, 1.0f)
    FBZZ_TOOLTIP("蹴り出しの強さ。1 で Ground Dust の既定どおり押し出す")

    /// @note 全周へ均等には配らない。円周が伸びる外周では均等配置だと点の間隔が
    ///       穴だらけになり、しかも半分は «背後の、カメラに映らない側» に捨てている
    ///       ため、跳ぶ時刻を読ませたい自分へ向かってくる縁だけへ寄せる。残りを 0 に
    ///       せず薄く残すのは、«円が広がっている» という盤面の形自体は保つため。
    FBZZ_GROUP("Front Arc")
    FBZZ_FIELD_RANGE(float, frontArcDegrees, 100.0f, "弧の幅 [度]", 20.0f, 360.0f)
    FBZZ_TOOLTIP("プレイヤーの方角を中心にこの角度ぶんへ点を寄せる。360 で全周均等")
    FBZZ_FIELD_RANGE(float, frontArcShare, 0.70f, "寄せる割合", 0.0f, 1.0f)
    FBZZ_TOOLTIP("輪の点のうち弧へ入れる割合。残りは «円の形» のために全周へ薄く配る")
    FBZZ_FIELD_RANGE(float, crackArcDegrees, 50.0f, "亀裂の弧 [度]", 10.0f, 360.0f)
    FBZZ_TOOLTIP("亀裂を寄せる角度。煙の弧より狭くする ─ 亀裂は «当たる線» そのものを"
                 "引く役で、幅を持たせると線ではなく面になる")
    FBZZ_FIELD_RANGE(float, crackArcShare, 0.75f, "亀裂を寄せる割合", 0.0f, 1.0f)

    /// @note 爆発は輪と同じ瞬間・同じ半径に出す。ずらすと «輪の外側でも何か起きている»
    ///       になり円が2重にぼやける。1発が焦げ跡込みで4秒と長く光源も伴うため、
    ///       数を絞らないと光が重なり Ground Blast Slots を使い切って途中で消える。
    FBZZ_GROUP("Blast")
    /// @note 1回の波の輪は11枚前後になるため、2発ずつだと22発で Ground Blast Slots
    ///       (20) を回しきれず、煙と同じ «奪い合って全部が育たない» が起きる。
    FBZZ_FIELD_RANGE_INT(int, blastsPerRing, 1, "Per Ring", 0, 6)
    FBZZ_TOOLTIP("輪 1 枚に混ぜる爆発の数。0 で煙だけになる。点と点の «間» へ置く")
    FBZZ_FIELD_RANGE(float, blastStrength, 0.42f, "強度", 0.0f, 1.0f)
    FBZZ_TOOLTIP("上げると «激突» と同じ大きさで弾けるので、波の 1 点で起きる出来事"
                 "としては強すぎる。着地の Blast より必ず小さく置くこと")

    /// @note 輪と別に前縁の目印を出す。輪は間隔があるため輪と輪の間は当たる線より
    ///       最大2.2m内側までしか絵が無く、それだけで読むと «来る前に食らった» が起きる。
    ///       自分へ向かってくる縁にだけ、輪の合間も点を置き続けて線の «今» を示す。
    FBZZ_GROUP("Front Marker")
    FBZZ_FIELD_RANGE(float, frontInterval, 0.12f, "間隔", 0.0f, 1.0f)
    FBZZ_TOOLTIP("プレイヤーの方角へ点を置く間隔 [秒]。0 で出さない (輪だけになる)")
    /// @note 一番読ませたい点 (自分へ向かってくる縁) が輪の点より小さくならないよう
    ///       輪より大きく置く。«追ってくる塊» に見える心配は角度を左右に振って外してある
    ///       (EmitFrontMarker)。
    FBZZ_FIELD_RANGE(float, frontScale, 1.5f, "スケール", 0.2f, 4.0f)
    FBZZ_TOOLTIP("輪の点より大きく置く。跳ぶ時刻はこの点から読まれる")

    /// @note 床へ跡を残す。煙も爆発も «通り過ぎたら何も無かったことになる» ため、
    ///       跡が残って初めて «床を割って走った» になる。ただし何度も跳ぶボスなので、
    ///       残し続けると «今の1回» が読めなくなり、数秒で薄れて消す。
    FBZZ_GROUP("Cracks")
    FBZZ_FIELD_FILE(crackMaterial, "Assets/Materials/Decal/DecalCrack.mat",
                    "Material", ".mat")
    FBZZ_TOOLTIP("render_path = \"decal\" の .mat。手続きで割れを描く DecalCrack が既定")
    /// @note 亀裂が前縁の線を引く役を兼ねる。煙は枠の上で連続した壁を作れないが、
    ///       デカールは VFX のリングと別枠 (128) で回るため密に置ける。置きたて0.45秒は
    ///       溝の底が光る (Heat) ので «今割れている線» がそのまま前縁表示になり、
    ///       冷えた後は «通った跡» として残る ─ 1つの絵が予告と記録を兼ねる。
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

    /// @note 越えた側にも返す。跳んで抜けた «成功» に何も返らないと、当たらなかったのが
    ///       読み勝ちなのか判定が無かったのか区別できず、跳ぶ操作が答えとして確定しない。
    FBZZ_GROUP("手応え")
    FBZZ_FIELD_RANGE(float, passRumble, 0.45f, "Pass Rumble", 0.0f, 1.0f)
    FBZZ_TOOLTIP("波がプレイヤーの位置を通り過ぎた瞬間。当たった側は被弾側が返すので、"
                 "ここは «越えた» の合図として軽く置く")
    FBZZ_FIELD_RANGE(float, passShakeRatio, 0.5f, "揺れの比率", 0.0f, 1.0f)

    /// @note 光は1灯だけ足す。土煙は無彩色でアリーナの床も暗いため、絵を増やすより
    ///       «見える条件» を作る方が効く。全周に置くと光が重なって白飛びし枠も食うので、
    ///       プレイヤーへ向かってくる縁の1点だけを照らす。
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
    /// @note 弧の «中» も等間隔にする。中心 (プレイヤーの真正面) を濃くすると
    ///       «自分を狙って湧いている» に読み替わってしまうため、密度は弧の中で一定にする。
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
    /// @note 使い回す。1回の波で数十枚落ちるため、作っては消すと GameObject 配列が
    ///       伸び縮みしポインタが無効になる箇所が増える。枠を先に決め古い側から上書きする。
    struct Crack {
        EntityRef ref;
        float     age = -1.0f;   ///< 負なら未使用
    };
    /// 同時に床へ残せる枚数。1回の波で15段 x 8枚 = 120枚前後落ちるため、それを
    /// 下回ると波の途中で内側から消え始め «割れて走った» が «今の所だけ» になる。
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
    /// @note プレイヤーが居ない (死亡・シーン切替) 間は寄せる先が無い。全周等間隔へ戻す。
    const bool aimed = Clamp01(share) > 0.0f && arc < TWO_PI - EPSILON && PlayerAngle(centre);
    if (!aimed) {
        const float step = TWO_PI / static_cast<float>(total);
        for (int i = 0; i < total; ++i) out[i] = phase + step * static_cast<float>(i);
        return total;
    }

    /// @note 変数名に near / far は使わない。Windows SDK (minwindef.h) が両方を
    ///       空のマクロとして定義しているため、宣言がそのまま消えてコンパイルが通らない。
    const int inArc = std::clamp(
        static_cast<int>(std::lround(static_cast<float>(total) * Clamp01(share))), 1, total);
    const int outside = total - inArc;

    /// @note 弧の中。両端に点を置くと «弧の切れ目» が縦線として見えるので半区間ぶん内側から。
    const float arcStep = arc / static_cast<float>(inArc);
    /// @note 送りは区間の中へ折り返す。段どうしで点の角度が揃うと «筋» に見えるが、
    ///       送りをそのまま足すと弧ごと回って寄せた先がプレイヤーから外れるため。
    const float arcPhase = std::fmod(std::fabs(phase), arcStep);
    for (int i = 0; i < inArc; ++i)
        out[i] = centre - arc * 0.5f + arcPhase + arcStep * (static_cast<float>(i) + 0.5f);

    /// @note 弧の外。«円が広がっている» の形だけを保てばよいので、薄く均等に。
    if (outside > 0) {
        const float step = (TWO_PI - arc) / static_cast<float>(outside);
        for (int j = 0; j < outside; ++j)
            out[inArc + j] = centre + arc * 0.5f + step * (static_cast<float>(j) + 0.5f);
    }
    return total;
}

inline BladeSide BossShockwaveComponent::WaveSide() const
{
    /// @note ボスは色を切り替えない。波の色は 1 本に固定する。
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

    /// @note 波は盤面の «場所» で起きる出来事なので 3D。2D にすると、アリーナの反対側で
    ///       起きた着地も自分の足元と同じ音量で鳴り、どこへ跳べばよいのか判らなくなる。
    se::EnsureSource(scene, "SE", 1.0f);
}

inline void BossShockwaveComponent::OnDestroy()
{
    /// @note 跡は波より長く残る作りなので、畳み忘れると床に焼き付く。ルートに置いた以上、
    ///       ボスと一緒には消えない。持ち主が畳む。
    for (Crack& crack : m_cracks) {
        if (GameObject* object = crack.ref.Resolve(scene)) scene.Destroy(*object);
        crack = {};
    }
    /// @note 破棄では OnDisable が呼ばれない (Scene::Destroy は OnDestroy だけを回す) ので、
    ///       波の途中で消えると前縁の光が点いたまま床に残る。
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
    /// @note 1 枚目は着地の輪そのもの。遅らせると «着いてから少しして床が広がる» になる。
    m_ringTimer  = 0.0f;
    m_frontTimer = 0.0f;
    m_crackTimer = 0.0f;
    /// @note 段の起点を 1 段ぶん手前へ置く。«生まれた半径から進んだら» にすると、着地の
    ///       1 枚目が 1 段ぶん遅れて出て «着いてから少しして床が広がる» に戻ってしまう。
    m_ringRadius  = m_radius - std::max(ringStep, 0.1f);
    m_crackRadius = m_radius - std::max(crackStep, 0.1f);

    EmitLanding();

    /// @note ボス本体ではなく着地点で鳴らす。波が走り出す場所は床であって腹ではないため、
    ///       床で鳴らすことで跳ぶかどうかを決める耳の手がかりと煙の出どころが一致する。
    se::PlayAt(audio, se::kImpactHeavy, groundPoint);
}

inline void BossShockwaveComponent::Cancel()
{
    m_active = false;
    m_radius = 0.0f;
    debugRadius = 0.0f;
    /// @note 光は «作り直さず消灯»。破棄すると次の着地で作り直しになり、跳ぶたびに
    ///       ライトの実体がシーンから出入りする (斉射の線と同じ扱い)。
    DriveFrontLight(false);
}

inline void BossShockwaveComponent::EmitLanding()
{
    auto* vfx = VfxManagerComponent::Instance();
    if (!vfx) return;

    const BladeSide side = WaveSide();

    if (landingBlast > 0.0f) {
        /// @note 爆発は落下点そのもの。少しでも浮かせると床から切り離されて «空中で爆ぜた»
        ///       ように見え、この後に走り出す煙と繋がらない。
        vfx->PlayGroundBlast(m_center, side, Clamp01(landingBlast));

        /// @note 中心の 1 発を囲む。腹の下いっぱいが割れた «広さ» は、1 発を大きくしても
        ///       出ない ─ 離れた場所で同時に起きて初めて、割れた面積として読める。
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
    /// @note 起点をずらしておく。0 度から並べると、着地のたびに同じ方角へ同じ煙が立つ。
    m_ringPhase += 0.618f * step + 0.41f;

    for (int i = 0; i < count; ++i) {
        const float angle = m_ringPhase + step * static_cast<float>(i);
        const float cx = std::cos(angle);
        const float sz = std::sin(angle);
        /// @note 腹の «縁» へ置く。基準は爆発と同じ Blast Ring Radius ─ 落下点の真上には
        ///       6m 級の胴体が載っているので、Start Radius (腹の下) に置いた煙は 1 度も
        ///       画面に出ない。1 つおきに内側へ落とすのは «円周に置いた印» にしないため。
        const float belly  = std::max(landingBlastSpread, std::max(startRadius, 0.1f));
        const float radius = belly * ((i % 2 == 0) ? 1.0f : 0.62f);
        const Vector3 point{ m_center.x + cx * radius, m_center.y, m_center.z + sz * radius };
        vfx->PlayGroundDust(point, Vector3{ cx, 0.0f, sz }, 1.0f,
                            std::max(landingSmokeScale, 0.2f) * ((i % 2 == 0) ? 1.0f : 1.25f));
    }
}

inline void BossShockwaveComponent::EmitRing(float dt)
{
    /// @note 距離と時間の «どちらも» 満ちたら 1 枚。距離だけだと出だしの 16m/s で
    ///       1 フレームに 2 枚置く回ができ、時間だけだと段の間隔が速さで伸び縮みする。
    m_ringTimer = std::max(m_ringTimer - dt, 0.0f);
    if (m_ringTimer > 0.0f) return;
    if (m_radius - m_ringRadius < std::max(ringStep, 0.1f)) return;
    m_ringTimer  = std::max(ringInterval, 0.02f);
    m_ringRadius = m_radius;

    auto* vfx = VfxManagerComponent::Instance();
    if (!vfx) return;

    /// @note 円周を Point Spacing で割った数。半径が伸びれば点も増えるので、どの大きさでも
    ///       同じ «輪» の見え方になる。上限の意味 (枠の総量) は Max Points を参照。
    const int lower = std::clamp(ringMinPoints, 3, 32);
    const int upper = std::clamp(ringMaxPoints, lower, 32);
    const int count = std::clamp(
        static_cast<int>(TWO_PI * std::max(m_radius, 0.1f) / std::max(ringSpacing, 0.5f)),
        lower, upper);

    /// @note 1 枚ごとに起点を回す。同じ角度から並べると、輪をまたいで点が放射状に揃い、
    ///       円ではなく «車輪のスポーク» として読まれる。
    m_ringPhase += 0.618f * (TWO_PI / static_cast<float>(count)) + 0.21f;

    float angles[32];
    const int placed = SpreadAngles(count, frontArcShare, frontArcDegrees, m_ringPhase,
                                    angles, 32);

    for (int i = 0; i < placed; ++i) {
        const float angle = angles[i];
        /// @note 散らしは «通り過ぎた側» にしか出さない。外側へ散らすと、当たらない場所に
        ///       煙が立って «見た目どおりに当たる» が破れる。
        const float jitter = std::max(ringJitter, 0.0f) *
                             std::fabs(std::sin(angle * 4.7f + m_ringPhase * 2.3f));
        const Vector3 point = PointOnFront(angle, std::max(bandWidth, 0.0f) * 0.5f + jitter);
        /// @note 煙が流れる向きは波の進む向き = 外向き。
        vfx->PlayGroundDust(point, Vector3{ std::cos(angle), 0.0f, std::sin(angle) },
                            Clamp01(smokeStrength), std::max(smokeScale, 0.2f));
    }

    const int blasts = std::clamp(blastsPerRing, 0, 6);
    if (blasts <= 0 || blastStrength <= 0.0f || placed <= 0) return;

    /// @note 爆発も «寄せた側» へ置く。全周へ均等に置くと、煙が濃い正面と光る場所がずれて
    ///       「どこで何が起きたか」が 2 か所に割れる。弧の点の «間» を借りる。
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

    /// @note 真正面へ置き続けると «自分を追ってくる 1 つの煙» に見える。左右へ振って、
    ///       向かってくる縁の «一部» として読ませる。振り幅の種を半径から取るのは、
    ///       半径だけが 1 発ごとに必ず変わっているため。輪の点も同じ弧へ寄せてあるので、
    ///       この 1 点はその弧のうち «今いちばん近い所» として読まれる。
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
        /// @note 亀裂は «割れた床» なので前縁のわずかに内側。前縁ちょうどに置くと、
        ///       まだ何も通っていない床が先に割れている絵になる。
        const Vector3 point = PointOnFront(angle, std::max(bandWidth, 0.0f) * 0.35f);

        const int slot = m_crackNext;
        Crack&    crack = m_cracks[slot];
        m_crackNext = (m_crackNext + 1) % kMaxCracks;

        /// @note 名前で拾い直す。DLL リロードでこの Script は作り直され EntityRef は
        ///       空に戻るが、跡の GameObject は Scene 側に残るため。
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

        /// @note 投影軸はローカル +Y。床へ落とすだけなので姿勢は無回転でよい。
        ///       大きさだけ 1 枚ごとに散らす ─ 同じ差し渡しが並ぶと «同じ判子» に見える。
        const float size = std::max(crackSize, 0.1f) *
                           (0.75f + 0.5f * std::fabs(std::sin(angle * 5.7f + m_crackPhase)));
        object->transform.position      = point;
        object->transform.worldPosition = point;
        object->transform.rotation      = Quaternion::Identity();
        object->transform.worldRotation = Quaternion::Identity();
        object->transform.scale = { size, std::max(crackProjectionDepth, 0.2f), size };

        /// @note 割れ方の種。角度から作るので、同じ場所を 2 回叩けば同じ割れ方になる。
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
        /// @note 名前で拾い直す。亀裂と同じで DLL リロードで EntityRef だけが空に戻るため。
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

    /// @note 床から少し浮かせる。床面と同一平面に置くと、立ち上がった煙の «腹» にしか
    ///       光が当たらず、輪郭を出すという目的に届かない。
    Vector3 point = PointOnFront(angle, 0.0f);
    point.y += std::max(frontLightHeight, 0.0f);
    object->transform.position      = point;
    object->transform.worldPosition = point;

    /// @note 生まれた瞬間と消えぎわは絞る。全区間で同じ明るさだと «ずっと点いている光» で、
    ///       波が走っていることの説明にならない。
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

        /// @note 伸びる → 光が引く → 薄れて消える、の 3 つを別々の時定数で持つ。
        ///       1 つの進みで兼ねると «伸びながら薄れる» になって、割れた瞬間が出ない。
        const float grow = Clamp01(crack.age / std::max(crackGrowSeconds, 0.01f));
        const float heat = crackHeatSeconds <= 0.0f
                         ? 0.0f
                         : Clamp01(1.0f - crack.age / crackHeatSeconds);
        /// @note 消えは最後の 1/3 だけ。ずっと薄れ続けると «最初から薄い» に見える。
        const float left = Clamp01((life - crack.age) / std::max(life * 0.33f, 0.05f));

        decal->materialParamOverrides["growth"] = { grow };
        decal->materialParamOverrides["heat"]   = { heat * heat };
        /// @note 色でなく opacity を書く。crackColor は .mat が持つ «この亀裂はどういう
        ///       色か» の正本なので、濃さを毎フレーム書き潰すと .mat 側の変更が消える。
        decal->opacity = left;
    }
}

inline void BossShockwaveComponent::ResolveHit(GameObject* player)
{
    if (m_dealt || damage <= 0) return;

    const float distance = PlanarDistance(player->transform.worldPosition, m_center);
    const float reach    = std::max(bandWidth, 0.0f) + std::max(playerRadius, 0.0f);
    if (std::fabs(distance - m_radius) > reach) return;

    /// @note 跳んで越えているか。足元の高さだけで決める (理由はヘッダー冒頭を参照)。
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
    /// @note 押しは波の中心から外へ。輪から «押し出された» が読める向き。
    (void)combat->DamagePlayer(player, damage, &m_center);
}

inline void BossShockwaveComponent::NotifyPass(const Vector3& playerPoint)
{
    if (m_passed) return;
    if (m_radius < PlanarDistance(playerPoint, m_center)) return;
    m_passed = true;

    /// @note 通り過ぎた «場所» はプレイヤーの足元そのもの。減衰の中心を着地点にすると、
    ///       遠くで跳んで越えたときだけ何も返らず、成功の合図が距離で消える。
    const float strength = Clamp01(passRumble);
    if (strength <= 0.0f) return;
    if (auto* pad = RumbleManagerComponent::Instance()) pad->Rumble(strength);
    if (auto* shake = CameraShakeManagerComponent::Instance())
        shake->Shake(strength * Clamp01(passShakeRatio));
}

inline void BossShockwaveComponent::OnLateUpdate()
{
    /// @note 実時間でなくゲーム時間で広げる。ヒットストップ中も進むと、止まっている
    ///       画面の中で判定だけが動いてしまう。
    const float dt = std::max(Time::deltaTime, 0.0f);

    /// @note 亀裂は波より長く残る。«波が終わったら跡も消える» では跡にならないので、
    ///       ここだけは m_active に関わらず進める。
    UpdateCracks(dt);

    if (!m_active) return;

    /// @note 生まれた直後だけ速い。倍率は 1 以上にしかならないので、Speed が保証している
    ///       «走って逃げ切れない» 下限は崩れない。
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

    /// @note 帯は別に引く。煙は1発ごとに散らして置くので見えている縁と当たる線は
    ///       一致せず、重ねて初めて «どれだけ外へ出て見えているか» が判る。
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
