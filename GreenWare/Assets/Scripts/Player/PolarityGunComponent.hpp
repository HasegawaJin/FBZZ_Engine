/// @file PolarityGunComponent.hpp
/// @brief 二丁の Polarity Emitter。左右独立の照射バッテリーで極性だけを塗る
/// @author Hasegawa Jin
/// @date 2026-08-22
///
/// WHY ここにダメージが 1 行も無いか:
///   企画書 2 章の前提「銃は敵を倒さない。倒すのは衝突である」。
///   ここに 1 行でもダメージ処理を足すと、企画そのものが別のゲームになる。
///
/// WHY 弾ではなく照射か (6 章):
///   「単発では 5 体溜めるのに 4 秒かかっていた。なぞりなら 1 回のジェスチャーで終わる」。
///   飛翔体は 1 発 1 体しか運べないため、6.1 の「敵 A → B → C をなぞって 3 体同時に
///   帯電させる」が原理的に作れない。線を引く操作にするには、線そのものが判定でなければ
///   ならない。貫通・塗り時間・太さの 3 つは 6.2 の仕様表がそのまま実装になる。
///
/// WHY 同じボタンで長押しとタップを分けるか (6.2):
///   長押し = なぞって塗る / タップ = 一瞬の点付与 = 起爆。7.9 の起爆は
///   「温存した無極の 1 体へ逆極を当てる」1 手なので、なぞりと同じ操作では出せない。
///   押した瞬間からビームは出る (どこを指しているかは常に見えていないと線が引けない)。
///   離すのが速ければ、塗り切っていなくてもその 1 体へ点付与を確定させる。
///
/// WHY ビームの GameObject を先に作って寝かせるか:
///   照射のたびに GameObject を作って捨てると、押すたびに生成・破棄が走り、
///   シーンの GameObject 数が入力に合わせて上下する。左右 2 本ぶんを OnStart で
///   確保しておけば、戦闘中は「寝ているビームを起こして伸ばす」だけになる。
///
/// WHY 帯そのものはここが描かないか:
///   照射に必要なのは «押されているか / 何秒引けるか / 誰が線上に居るか» で、
///   «帯の頂点をどう曲げるか» はそのどれでもない。両方をここへ置いていた間、
///   このファイルは «撃つ規則» と «光り方» が交互に並ぶ読みにくい状態になっていた。
///   帯は BeamTrailRendererComponent が持ち、ここは BeamTrailStyle を組んで渡すだけ。
///
///   帯の実体はビルボードのリボンで、どの角度から見ても太さが変わらない。
///   円柱メッシュにしないのは、MeshRenderer::mesh を meshPath から解決しているのが
///   SceneSerializer だけで、ランタイムに meshPath を書いても mesh は nullptr のまま
///   = 一切描画されないため。
#pragma once

#include <Engine/Scene/Components/AudioSourceComponent.hpp>
#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <Engine/Scene/Components/PresentationComponents.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Scripts/Combat/CharacterEvent.hpp>
#include <Scripts/Data/PolarityTuning.hpp>
#include <Scripts/Game/CameraFollowManagerComponent.hpp>
#include <Scripts/Game/CombatManagerComponent.hpp>
#include <Scripts/Game/ScreenEffectManagerComponent.hpp>
#include <Scripts/Player/PlayerAimComponent.hpp>
#include <Scripts/Player/PlayerControllerComponent.hpp>
#include <Scripts/Player/WeaponAnimatorComponent.hpp>
#include <Scripts/Player/WeaponRigComponent.hpp>
#include <Scripts/Polarity/PolarityTargetComponent.hpp>
#include <Scripts/Utils/BeamTrailRendererComponent.hpp>
#include <Scripts/Utils/ElectricArc.hpp>
#include <Scripts/Utils/EmitterBattery.hpp>
#include <Scripts/Utils/InputActions.hpp>
#include <Scripts/Utils/PolarityTypes.hpp>
#include <Scripts/Utils/SeLibrary.hpp>
#include <Scripts/Utils/WeaponSockets.hpp>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

// 充填音を鳴らす閾値。Refill は cap へ漸近するので、厳密な一致では鳴らないことがある。
inline constexpr float kBatteryFullRatio = 0.999f;

class PolarityGunComponent : public Script {
    FBZZ_SCRIPT(PolarityGunComponent)

    // SE はここから鳴らす。無くても照射できるが 12.1 の手応えが丸ごと消える。
    FBZZ_OPTIONAL_COMPONENT(AudioSourceComponent)

public:
    // PlayerComponent が必須 PolarityTuning を注入する。数値を Inspector 側へ複製しない。
    fbzz::Asset<PolarityTuning> tuning{};

    FBZZ_GROUP("Weapons")
    // 銃本体。ここから WeaponAnimatorComponent を引いて照射モーションを再生する。
    // 未設定でも WPN_Pistol_R / WPN_Pistol_L を名前で復旧する。
    FBZZ_REF(GameObject, weaponPlus,  "Weapon (+) / Right")
    FBZZ_REF(GameObject, weaponMinus, "Weapon (-) / Left")

    FBZZ_GROUP("Muzzles (override)")
    // 通常は空でよい。銃側の SOCKET_Muzzle が自動で使われる。
    // WHY それでも残すか: マズルを意図的にずらしたい演出用の逃げ道。
    FBZZ_REF(GameObject, muzzleRight, "Muzzle Right (+)")
    FBZZ_REF(GameObject, muzzleLeft,  "Muzzle Left (-)")

    // WHY 入力の項目を持たないか:
    //   11 章の操作表は「左入力 = 左銃 (−) / 右入力 = 右銃 (＋)」という対応だけを
    //   決めており、それが右クリックなのか RT なのかは別の話。物理入力を Inspector に
    //   持つと、その 1 行のためにゲームパッド対応が原理的に不可能になる。
    //   実際の割り当ては ProjectSettings/Input.inputactions が持つ
    //   (マウス左右 / LT・RT が同じアクションへ束ねてある)。

    FBZZ_GROUP("Feedback (12.1)")
    // 12.1 は「4 週目の調整項目ではなく 1 週目から入れる」と名指ししている。
    // 極性にダメージが無い以上、塗った手応えが無いと開始 1 分で投げられる。
    FBZZ_FIELD_AUDIO(sfxBeamStart,  "", "SFX Beam Start")
    FBZZ_FIELD_AUDIO(sfxPaint,      "", "SFX Paint")
    FBZZ_TOOLTIP("1 体を塗り切った瞬間。なぞりのリズムはこの音で数える")
    FBZZ_FIELD_AUDIO(sfxNeutralize, "", "SFX Neutralize")
    FBZZ_FIELD_AUDIO(sfxEmpty,      "", "SFX Empty")
    FBZZ_TOOLTIP("バッテリーが尽きた瞬間と、空のまま撃とうとしたときの音")
    // WHY 点火«だけ» に画面効果を割り当てるか: 長押し照射は出っぱなしなので、線そのものは
    //     «今まさに撃った» を伝えられない。押した 1 瞬に画面が反応して初めて、
    //     押し始めが出来事になる。照射中ずっと掛けると、それは «状態» の表示になり、
    //     押した手応えは消える (掛けっぱなしにしたければ SetSustainedDistortion がある)。
    FBZZ_FIELD_RANGE(float, ignitionSurge, 1.0f, "Ignition Surge", 0.0f, 1.0f)
    FBZZ_TOOLTIP("点火した瞬間、画面の縁を極の色で走らせる強さ。0 で出さない。"
                 "濃さと歪みの配分は ScreenEffectManager の Surge が持つ")
    FBZZ_FIELD_RANGE(float, ignitionSurgeSeconds, 0.16f, "Ignition Surge Seconds", 0.0f, 1.0f)
    // WHY 起爆 (7.9) と同じ器を使ってよいか: 画角の張り出しは «こちらが解き放った» を
    //     表す語で、点火も起爆もその側の出来事なので語彙は同じでよい。役割は量で分ける。
    //     PunchFov は最大値で合成するため、点火の小さな張り出しは起爆の大きな張り出しを
    //     潰さない。逆に点火を 1.0 まで上げると、起爆の一手が «いつもの絵» に埋もれる。
    FBZZ_FIELD_RANGE(float, ignitionFovPunch, 0.35f, "Ignition FOV Punch", 0.0f, 1.0f)
    FBZZ_TOOLTIP("点火した瞬間に画角を張り出す強さ。起爆は 1.0 なので、それより小さく保つ。"
                 "広がる度数と立ち上がり / 戻りは CameraFollowManager の FOV Burst が持つ")

    FBZZ_GROUP("Beam")
    FBZZ_FIELD_FILE(beamMaterial, "Assets/Materials/Effects/FX_WPN_Beam.mat",
                    "Beam Material", ".mat")
    FBZZ_TOOLTIP("ビームに割り当てる .mat。極の色は albedo へ毎フレーム上書きされる。"
                 "既定は事前乗算の Beam.hlsl。断面と流れの値を持たない .mat を差した場合、"
                 "下の Shape / Flow は黙って無視される (帯電の乱れも同じく無視される)")
    // 12.3「ビームは細い芯 ＋ 淡いグローの 2 層」。芯だけだと線が硬く、
    // グローだけだとどこが中心か読めない。当たり判定の太さとは無関係の見た目の値。
    FBZZ_FIELD_RANGE(float, coreWidth, 0.09f, "Core Width", 0.01f, 1.0f)
    FBZZ_FIELD_RANGE(float, glowWidth, 0.34f, "Glow Width", 0.0f,  3.0f)
    FBZZ_TOOLTIP("0 でグロー層を出さない")
    // 1 を超えた色がブルームに乗る。12.2 の注記どおり、上げすぎると白飛びして
    // 赤と青の区別がつかなくなるので、明るさより彩度を優先する。
    FBZZ_FIELD_RANGE(float, coreBrightness, 2.4f, "Core Brightness", 0.1f, 8.0f)
    FBZZ_FIELD_RANGE(float, glowBrightness, 1.0f, "Glow Brightness", 0.1f, 8.0f)
    FBZZ_FIELD_RANGE(float, glowOpacity, 0.35f, "Glow Opacity", 0.0f, 1.0f)
    FBZZ_TOOLTIP("グロー層が背景を隠す量。0 に近いほど純粋な加算グローになる")

    FBZZ_GROUP("Beam Shape")
    // WHY 太さ (Core Width) と別に持つか: 上の 2 つは «帯を何メートルで組むか»、
    //     ここは «その帯の中で芯がどこまでか»。帯を太くしても芯の割合は変えたくない
    //     (太くした分だけ芯まで太ると、ただの明るい板になる)。
    FBZZ_FIELD_RANGE(float, coreSharpness, 0.45f, "Core Sharpness", 0.01f, 1.0f)
    FBZZ_TOOLTIP("芯層の断面で、芯が帯の半幅の何割を占めるか")
    FBZZ_FIELD_RANGE(float, glowSoftness, 3.2f, "Glow Softness", 0.5f, 8.0f)
    FBZZ_TOOLTIP("グロー層の縁の減衰指数。大きいほど中心へ寄って細く見える")
    FBZZ_FIELD_RANGE(float, muzzleFade, 0.04f, "Muzzle Fade", 0.0f, 0.5f)
    FBZZ_TOOLTIP("銃口側の立ち上がり。0 だと帯の四角い切り口が発射口から生えて見える")
    FBZZ_FIELD_RANGE(float, tipFade, 0.08f, "Tip Fade", 0.0f, 0.5f)
    FBZZ_TOOLTIP("着弾側の先細り。当たった点は着弾エフェクトが担うので線は手前で譲る")

    FBZZ_GROUP("Beam Flow")
    // 6.1 の「なぞって塗る」は線を引く操作なので、線が流れていないと
    // 「当てている最中」なのか「止まっている」のかが絵から読めない。
    FBZZ_FIELD_RANGE(float, stripeDensity, 1.2f, "Stripe Density", 0.0f, 8.0f)
    FBZZ_TOOLTIP("素材を 1m あたり何回繰り返すか。0 で 1 枚を全長へ引き伸ばす")
    FBZZ_FIELD_RANGE(float, scrollSpeed, 3.0f, "Scroll Speed", -20.0f, 20.0f)
    FBZZ_TOOLTIP("模様が流れる速さ [周/秒]。負で銃口へ向かって流れる")

    FBZZ_GROUP("Beam Crackle")
    // WHY 帯の «中» を暴れさせるか: 折れ線にすると当たり判定 (PolarityBeam の線分) と
    //     芯の位置が食い違い、6.4 が守ろうとしている «見た目どおりに当たる» が崩れる。
    //     蛇行も途切れも断面の中で作れば、判定は直線のまま絵だけが生きる。
    //     詳細は Beam.hlsl のヘッダー。
    FBZZ_FIELD_RANGE(float, crackle, 0.45f, "Crackle", 0.0f, 1.0f)
    FBZZ_TOOLTIP("芯の途切れ。0 で滑らかな線、1 で焼き切れかけたフィラメント")
    FBZZ_FIELD_RANGE(float, snake, 0.35f, "Snake", 0.0f, 1.0f)
    FBZZ_TOOLTIP("芯が帯の中で蛇行する幅。銃口側は自動で絞られ、着弾側ほど大きく振れる")
    FBZZ_FIELD_RANGE(float, snakeFrequency, 9.0f, "Snake Frequency", 0.0f, 40.0f)
    FBZZ_FIELD_RANGE(float, flicker, 0.22f, "Flicker", 0.0f, 1.0f)
    FBZZ_TOOLTIP("帯全体の明滅の深さ")
    FBZZ_FIELD_RANGE(float, chargeBeads, 4.0f, "Charge Beads", 0.0f, 24.0f)
    FBZZ_TOOLTIP("銃口から着弾点へ流れる電荷の粒の数。0 で出さない")
    FBZZ_FIELD_RANGE(float, churnRate, 9.0f, "Churn Rate", 0.0f, 40.0f)
    FBZZ_TOOLTIP("乱れが組み替わる速さ。上げるほど «高い電圧» に見える")
    // 6.3 の「時間が資源」を絵でも読ませる。ゲージを見なくても線が荒れてくる。
    FBZZ_FIELD_RANGE(float, lowBatteryUnrest, 0.5f, "Low Battery Unrest", 0.0f, 1.0f)
    FBZZ_TOOLTIP("バッテリーが減るほど乱れを増やす量。0 で残量に関わらず同じ線")
    // 12.1 の「当たっている手応え」。塗れているフレームだけ線が張る。
    FBZZ_FIELD_RANGE(float, contactBoost, 0.7f, "Contact Boost", 0.0f, 2.0f)
    FBZZ_TOOLTIP("線上に対象が居るあいだ、明るさと放電を増す量")

    FBZZ_GROUP("Beam Wobble")
    // WHY 断面の蛇行 (Snake) と別に持つか:
    //   Snake は «帯の中で芯がどこを通るか» で、帯の輪郭そのものは直線のまま。どれだけ
    //   強く振っても «硬い板の上で光が泳いでいる» にしかならない。ここは帯の頂点を
    //   動かすので、シルエットごとうねる。2 つは «中の電流» と «管そのもの» の違いで、
    //   どちらか一方だけだと «中身のない管» か «動かない管» になる。
    //   実際に頂点を曲げるのは BeamTrailRendererComponent。
    FBZZ_FIELD_RANGE(float, wobble, 0.10f, "Wobble", 0.0f, 1.0f)
    FBZZ_TOOLTIP("10m 先を撃ったときに帯そのものが振れる幅 [m]。0 で直線に戻る。"
                 "実際の振れ幅は線の長さに比例する (遠いほど画面上で小さくなるため)")
    FBZZ_FIELD_RANGE(float, wobbleFrequency, 3.0f, "Wobble Frequency", 0.0f, 12.0f)
    FBZZ_TOOLTIP("帯 1 本あたりの波の数。上げすぎると «震え» になって線に見えなくなる")
    FBZZ_FIELD_RANGE(float, wobbleTravel, 1.6f, "Wobble Travel", -8.0f, 8.0f)
    FBZZ_TOOLTIP("波が銃口から着弾点へ抜ける速さ [周/秒]。負で銃口へ向かって戻る。"
                 "0 にすると形が固まり «たわんだ棒» に見える")
    FBZZ_FIELD_RANGE(float, wobbleBias, 0.68f, "Wobble Bias", 0.0f, 1.0f)
    FBZZ_TOOLTIP("振れが最大になる位置。両端は必ず 0 なので、銃口と着弾点は動かない "
                 "(見た目どおりに当たるという 6.4 の前提を崩さない)")
    FBZZ_FIELD_RANGE(float, wobbleGlow, 0.55f, "Wobble Glow Follow", 0.0f, 1.0f)
    FBZZ_TOOLTIP("裾が芯の揺れをどれだけ追うか。1 で 2 層が一緒に泳ぐ")
    FBZZ_FIELD_RANGE(float, wobbleSegments, 2.0f, "Wobble Segments / m", 0.5f, 6.0f)
    FBZZ_TOOLTIP("1m あたりの折れ点数。少ないと波が «角» に割れる")

    FBZZ_GROUP("Beam Arc")
    // 帯の «外» を走る放電。断面の乱れだけでは «明るい線» の域を出ないので、
    // 帯からはみ出す筋を重ねて «電気が漏れている» ところまで持っていく。
    // 12.5 の敵どうしを結ぶエネルギーラインと同じ ElectricArcBundle で組む
    // (同じ «電気» が銃から出ているように見せるため、実装を分けない)。
    FBZZ_FIELD(bool, drawArc, true, "Draw Arc")
    FBZZ_FIELD_RANGE_INT(int, arcStrands, 3, "Arc Strands", 1, 6)
    FBZZ_FIELD_RANGE(float, arcAmplitude, 0.45f, "Arc Amplitude", 0.0f, 2.0f)
    FBZZ_TOOLTIP("10m 先を撃ったときに放電が帯から外れる幅 [m]。"
                 "実際の振れ幅は線の長さに比例する (遠いほど画面上で小さくなるため)")
    FBZZ_FIELD_RANGE(float, arcWidth, 0.05f, "Arc Width", 0.005f, 0.5f)
    FBZZ_FIELD_RANGE(float, arcRate, 28.0f, "Arc Rate", 1.0f, 60.0f)
    FBZZ_TOOLTIP("放電が組み替わる頻度 [Hz]。上げすぎると雑音の帯になる")
    FBZZ_FIELD_RANGE(float, arcIntensity, 1.6f, "Arc Intensity", 0.0f, 8.0f)
    // 帯へ «戻ってこない» 枝。稲妻が稲妻に見えるのは、本線から外れて途中で
    // 消える筋があるからで、本線を何本束ねてもこれは出ない。
    FBZZ_FIELD_RANGE_INT(int, forkCount, 2, "Forks", 0, 4)
    FBZZ_TOOLTIP("線の途中から外れて消える枝の本数。0 で出さない")
    FBZZ_FIELD_RANGE(float, forkLength, 0.9f, "Fork Length", 0.1f, 4.0f)
    FBZZ_TOOLTIP("枝が本線から離れる長さ [m]")
    FBZZ_FIELD(bool, drawImpactArc, true, "Draw Impact Arc")
    FBZZ_TOOLTIP("着弾点で這う放電。地形に当たっているときだけ出る")
    FBZZ_FIELD_RANGE(float, impactArcRadius, 0.55f, "Impact Arc Radius", 0.05f, 3.0f)

    FBZZ_GROUP("Debug")
    FBZZ_FIELD(bool, drawDebugBeam, false, "Draw Debug Beam")

    // ── HUD が読む窓口 ──────────────────────────────────────────────────
    /// 照射バッテリーの充填率。1 = 満タン / 0 = 空。
    [[nodiscard]] float BatteryOf(Polarity polarity) const;
    /// 今この極で照射を始められるか (6.3 の再点火待ちを含む)。
    [[nodiscard]] bool  CanEmit(Polarity polarity) const;
    /// 今まさに照射しているか。HUD が「減っている最中」を出し分けるために読む。
    [[nodiscard]] bool  IsEmitting(Polarity polarity) const;

    void SetAimComponent(PlayerAimComponent* aim) { m_aimOverride = aim; }
    void SetController(PlayerControllerComponent* controller) { m_controllerOverride = controller; }
    void SetWeaponRig(WeaponRigComponent* rig) { m_weaponRig = rig; }

    void OnStart()      override;
    // WHY Script ではなく LateScript か: ビームの終点は PlayerAimComponent が引いた線の
    //     先端で、その線はカメラの向きから決まる。照射と同じフェーズに揃えておかないと、
    //     触れて見えた敵と極性が乗る敵が 1 フレームずれる。
    void OnLateUpdate() override;
    void OnDestroy()    override;

private:
    /// 片側のエミッター 1 基。左右で同じ処理を 2 度書かないためにまとめる。
    struct Emitter {
        Polarity       polarity = Polarity::Plus;
        EmitterBattery battery;
        /// 12.3 の 2 層。芯とグローで GameObject を分ける。
        EntityRef core;
        EntityRef glow;
        /// 帯の外を走る放電と、着弾点で這う放電。
        ElectricArcBundle arc;
        ElectricArcBundle impactArc;
        /// 本線から外れて消える枝。1 本ごとに根元と行き先が違うので束を分ける。
        std::vector<ElectricArcBundle> forks;
        /// 線上に対象が居た直後を 1 として減衰する。命中の «張り» を絵へ乗せる。
        float contactPulse = 0.0f;
        /// 押している間の経過秒。tapSeconds 以下で離せばタップ = 起爆 (6.2)。
        float heldSeconds = 0.0f;
        bool  held        = false;
        bool  emitting    = false;
        /// この 1 押しで 1 度でも照射できたか。空のまま連打して起爆だけ通ると、
        /// 6.3 の「時間が資源になる」が起爆に対して効かなくなる。
        bool  pressEmitted = false;
        /// 空になった瞬間だけ音を出すための立ち上がり検出。
        bool  wasDepleted = false;
        /// 満タンに戻った瞬間だけ音を出すための立ち上がり検出。
        bool  wasFull = true;
        /// 照射ループ音を銃へ流しているか。切り替えたフレームだけ銃へ伝える。
        bool  loopPlaying = false;
        /// 今ビームを出しているか。消すのは状態が変わったフレームだけでよい。
        bool  beamVisible = false;
    };

    [[nodiscard]] Emitter&       Side(Polarity polarity)
    { return polarity == Polarity::Plus ? m_plus : m_minus; }
    [[nodiscard]] const Emitter& Side(Polarity polarity) const
    { return polarity == Polarity::Plus ? m_plus : m_minus; }

    void TickEmitter(Emitter& emitter, float dt);
    /// 照射 1 フレームぶん。線に触れている対象すべてを塗る (6.2 の貫通)。
    /// contactSeconds はバッテリーから実際に引けた秒数。尽きかけたフレームは
    /// dt より短くなるので、残量がそのまま塗れた量になる。
    void Sweep(Emitter& emitter, float contactSeconds, float dt);
    /// タップの一瞬の点付与 = 起爆 (6.2 / 7.9)。
    void Detonate(Emitter& emitter);
    /// emitted は今その音を出した側の極。塗り結果と極の両方で音を選ぶ。
    void PlayPaintFeedback(const PolarityResult& result, Polarity emitted);
    /// 照射中だけ鳴るループ音を、状態が変わったフレームだけ銃へ伝える。
    ///
    /// WHY プレイヤーではなく銃で鳴らすか: ループは主 voice を 1 本占有するため、
    ///     左右同時に照射するとプレイヤーの AudioSource 1 つには載らない。
    ///     銃はもともと左右に 1 つずつあるので、そこが正しい持ち主になる。
    void UpdateBeamLoop(Emitter& emitter);

    [[nodiscard]] bool InputHeld(Polarity polarity) const;
    /// 銃が手にあって、抜き / 収めも終わっているか。
    ///
    /// WHY IsDrawn() だけでは足りないか: WeaponRigComponent の m_drawn は
    ///     「動作が終わったか」ではなく「銃の親を移し終えたか」で立つ。抜きの後半は
    ///     まだ展開クリップの途中なのに IsDrawn() は true になっているため、
    ///     発射口が畳まれたままレーザーだけが伸びる。IsBusy() はそのために在る。
    [[nodiscard]] bool WeaponsReady() const;
    [[nodiscard]] PlayerAimComponent* Aim() const;

    // ── ビームの見た目 ──────────────────────────────────────────────────
    /// 今フレームの «帯電の激しさ»。残量・命中・調整値を 1 度だけ束ねる。
    ///
    /// WHY 束ねるか: 芯・グロー・放電の 3 か所が同じ量を必要とする。各所で組むと、
    ///     残量の効き方を変えたときに 1 か所だけ直し忘れて «芯だけ荒れる» が起きる。
    struct Unrest {
        float crackle = 0.0f;
        float snake   = 0.0f;
        float flicker = 0.0f;
        float beads   = 0.0f;
        /// 明るさと放電の強さの倍率。命中しているあいだ 1 を超える。
        float energy  = 1.0f;
        /// 命中の «張り» そのもの [0,1]。energy と違い調整値が乗っていないので、
        /// 明るさではなく «出来事» として使う側 (帯を走るリングと端の閃光) が読む。
        float surge   = 0.0f;
    };

    void BuildBeam(Emitter& emitter);
    /// 銃口から照準の先までへ 2 層の線と放電を張る。
    void ShowBeam(Emitter& emitter, const Vector3& from, const Vector3& to, float dt);
    void HideBeam(Emitter& emitter);
    void ReleaseBeam(Emitter& emitter);
    [[nodiscard]] GameObject* BuildBeamPart(const std::string& name);
    [[nodiscard]] Unrest UnrestOf(const Emitter& emitter) const;
    /// 1 層ぶんの «どんな線か»。帯をどう曲げるかは受け取った側が決める。
    ///
    /// WHY 共有 .mat ではなく per-instance へ流すことを前提にするか: 左右のビームは
    ///     同じ .mat を指しているので、共有アセットへ書くと ＋ の値が − のビームにも乗る。
    ///     長さから決まる tiling は左右で必ず違うため、per-instance でなければ成立しない。
    [[nodiscard]] BeamTrailStyle StyleOf(const Emitter& emitter, bool isCore,
                                         const Unrest& unrest) const;
    /// 帯を描く側。まだ OnStart を抜けていなければ nullptr。
    [[nodiscard]] BeamTrailRendererComponent* TrailOf(const EntityRef& ref) const;
    /// 帯に沿って走る放電を今フレームの両端へ張り直す。
    void UpdateBeamArc(Emitter& emitter, const Vector3& from, const Vector3& to,
                       const Unrest& unrest, float dt);
    /// 本線から外れて消える枝を張り直す。
    void UpdateForks(Emitter& emitter, const Vector3& from, const Vector3& to,
                     const Unrest& unrest, float dt);
    /// 着弾点で這う放電。地形に当たっているあいだだけ出す。
    void UpdateImpactArc(Emitter& emitter, const Unrest& unrest, float dt);
    [[nodiscard]] std::string BeamName(Polarity polarity, const char* layer) const;

    // ── 銃 ──────────────────────────────────────────────────────────────
    [[nodiscard]] Vector3 MuzzlePosition(Polarity polarity) const;
    /// 銃 GameObject と、その上の WeaponAnimatorComponent を引く。
    /// 銃は Draw/Holster で親が張り替わるが GameObject 自体は生き続けるため、
    /// 参照ではなく毎回引き直す。
    [[nodiscard]] GameObject* WeaponObject(Polarity polarity) const;
    [[nodiscard]] WeaponAnimatorComponent* WeaponAnim(Polarity polarity) const;

    Emitter m_plus;
    Emitter m_minus;

    PlayerAimComponent* m_aimOverride = nullptr;
    PlayerControllerComponent* m_controllerOverride = nullptr;
    WeaponRigComponent* m_weaponRig = nullptr;
    // 照準が居ない構成は組み間違いなので、1 度だけ言えば足りる。押している間ずっと
    // 出すと、同じ行で Console が埋まって他の警告が流れる。
    bool m_warnedNoAim = false;
};

FBZZ_REFLECT(PolarityGunComponent)

// ── 実装 (inline) ─────────────────────────────────────────────────────────────

inline float PolarityGunComponent::BatteryOf(Polarity polarity) const
{
    return Side(polarity).battery.Ratio(tuning->batterySeconds);
}

inline bool PolarityGunComponent::CanEmit(Polarity polarity) const
{
    return Side(polarity).battery.CanEmit();
}

inline bool PolarityGunComponent::IsEmitting(Polarity polarity) const
{
    return Side(polarity).emitting;
}

inline PlayerAimComponent* PolarityGunComponent::Aim() const
{
    return m_aimOverride ? m_aimOverride : scene.GetScript<PlayerAimComponent>();
}

inline bool PolarityGunComponent::WeaponsReady() const
{
    // WHY 無いときは撃てる扱いにするか: WeaponRigComponent を置かない構成
    //     (デバッグシーンや、抜き差しを作る前の検証) で撃てなくなると、
    //     照射そのものを確かめられなくなる。抑止は「収納機構がある」ときだけ効かせる。
    auto* rig = m_weaponRig ? m_weaponRig : scene.GetScript<WeaponRigComponent>();
    return !rig || (rig->IsDrawn() && !rig->IsBusy());
}

inline void PolarityGunComponent::OnStart()
{
    if (!tuning) {
        debug.LogError("PolarityGunComponent requires PolarityTuning.fzdata "
                       "(PlayerComponent injects it).");
        enabled = false;
        return;
    }

    m_plus  = {};
    m_minus = {};
    m_warnedNoAim = false;
    m_plus.polarity  = Polarity::Plus;
    m_minus.polarity = Polarity::Minus;
    m_plus.battery.Reset(tuning->batterySeconds);
    m_minus.battery.Reset(tuning->batterySeconds);
    // 空撃ち・尽きた音はプレイヤー本人の位置で鳴る。減衰を掛ける相手が自分自身なので 2D。
    se::EnsureSource(scene);

    BuildBeam(m_plus);
    BuildBeam(m_minus);

    // 7.7 の「破ってはいけない設計上の制約」をここで検算する。
    //     敵の極性持続時間 ＞ 1 本の線を引き切る時間 ＋ 起爆までの間
    // 破っていると起爆する前に最初に塗った敵の極が切れ、ゲームが成立しない。
    // 破綻の仕方が「なんとなく繋がらない」なので、明示的に言わないと気付けない。
    constexpr float kDetonateSecondsEstimate = 0.8f; // 線を引き終えて起爆点へ振り向くまで
    const float shortest = tuning->ShortestDuration();
    if (!tuning->SatisfiesTimingConstraint(shortest, kDetonateSecondsEstimate)) {
        debug.LogError(
            "PolarityTuning breaks the 7.7 timing constraint: shortest duration must "
            "exceed battery seconds + the time it takes to swing onto the detonator. "
            "Combos will not connect.");
    }
}

inline void PolarityGunComponent::OnDestroy()
{
    ReleaseBeam(m_plus);
    ReleaseBeam(m_minus);
}

inline bool PolarityGunComponent::InputHeld(Polarity polarity) const
{
    // 右 = ＋ / 左 = −。左右の入力と左右の銃を一致させる (11 章)。
    // マウス左右と LT / RT はアクション層で同じ名前へ束ねてあるので、
    // ここではどのデバイスで押されたかを知らなくてよい。
    return input.GetAction(polarity == Polarity::Plus ? actions::kEmitPlus
                                                      : actions::kEmitMinus);
}

inline void PolarityGunComponent::OnLateUpdate()
{
    const float dt = Time::deltaTime;
    TickEmitter(m_plus, dt);
    TickEmitter(m_minus, dt);
}

inline void PolarityGunComponent::TickEmitter(Emitter& emitter, float dt)
{
    // 銃を収めている間と、抜き / 収めの最中は照射できない。
    //
    // WHY 空撃ちの音も出さないか: バッテリー切れは「撃とうとしたが出なかった」なので
    //     反応を返す必要があるが、収納中は銃自体が手に無い。音を返すと畳まれた銃が
    //     応答したことになる。収納は HUD のゲージとクロスヘアが消えることで既に
    //     見えているので、ここは黙って何もしないのが正しい。
    if (!WeaponsReady()) {
        // 押していた事実ごと捨てる。残したまま抜き直すと、その最初のフレームが
        // 「離した」と解釈されてタップの起爆が 1 回暴発する。
        emitter.held         = false;
        emitter.heldSeconds  = 0.0f;
        emitter.emitting     = false;
        emitter.pressEmitted = false;
        // 6.3 の回復は非照射時に進む。収めている間も溜まる。
        emitter.battery.Refill(dt, tuning->batterySeconds, tuning->batteryRefillSeconds,
                               tuning->batteryRearmRatio);
        // 立ち上がり検出も追従させる。ここを止めると、収納中に満タンへ戻ったのに
        // 抜いた瞬間だけ「尽きた音」が鳴る。
        emitter.wasDepleted = emitter.battery.depleted;
        emitter.wasFull     = emitter.battery.Ratio(tuning->batterySeconds) >= kBatteryFullRatio;
        // 収めた瞬間に照射が切れる。ループを止めないと畳んだ銃が鳴り続ける。
        UpdateBeamLoop(emitter);
        HideBeam(emitter);
        return;
    }

    const bool held    = InputHeld(emitter.polarity);
    const bool pressed = held && !emitter.held;
    // 離したフレームに、それがタップだったかを決める。押していた時間しか手がかりが無い。
    const bool tapped  = !held && emitter.held && emitter.heldSeconds <= tuning->tapSeconds;

    emitter.heldSeconds = held ? emitter.heldSeconds + dt : 0.0f;
    emitter.held        = held;
    emitter.emitting    = false;

    if (pressed) {
        emitter.pressEmitted = false;
        if (emitter.battery.CanEmit()) {
            // 12.2 が「赤と青を耳でも区別する」を求めているので、点火音は極ごとに分ける。
            se::Play(audio, sfxBeamStart, se::BeamStart(emitter.polarity));
            // 画面の縁を極の色で走らせる。耳と銃のモーションだけだと、点火が
            // «自分の手元» で完結して盤面の側では何も起きていないように見える。
            // WHY 被弾の Flash を流用しないか: あれは画面全体を塗る器なので、＋ (赤) を
            //     撃つたびに被弾と同じ絵が出る。縁だけを明るくする Surge と役割を分ける。
            if (auto* screen = ScreenEffectManagerComponent::Instance())
                screen->Surge(PolarityColor(emitter.polarity), ignitionSurge,
                              ignitionSurgeSeconds);
            // 画角も一緒に張り出す。縁の光だけだと «画面の表面» で起きたことになり、
            // 銃から出たエネルギーが盤面を押した感じにならない。
            if (auto* follow = CameraFollowManagerComponent::Instance())
                follow->PunchFov(ignitionFovPunch);
            // 銃側にも点火を伝える。スライドが動く代わりに発射口が開く。
            if (auto* weapon = WeaponAnim(emitter.polarity)) weapon->PlayFire();
            if (auto* player = m_controllerOverride
                ? m_controllerOverride : scene.GetScript<PlayerControllerComponent>())
                player->PlayFireAnimation(emitter.polarity == Polarity::Plus);
            // WHY 空撃ちでは知らせないか: 出来事としての「攻撃を出した」は線が出たときだけ。
            //     バッテリー切れの空撃ちまで含めると、撃てない間じゅう攻撃の顔になる。
            if (auto* combat = CombatManagerComponent::Instance())
                combat->Notify(scene.Self(), CharacterEvent::Attack);
        } else {
            // 空撃ちにも音を返す。無反応だと「入力が拾われていない」のか
            // 「バッテリーが空」なのか区別できず、6.3 のリズムを覚えられない。
            se::Play(audio, sfxEmpty, se::BatteryEmpty(emitter.polarity));
            if (auto* weapon = WeaponAnim(emitter.polarity)) weapon->PlayDry();
        }
    }

    // 命中の «張り» は照射をやめた後も一瞬だけ残す。フレーム単位で立ち下げると、
    // 縁を掠めた 1 フレームの取りこぼしのたびに線の明るさが痙攣する。
    emitter.contactPulse = Max(emitter.contactPulse - dt * 5.0f, 0.0f);

    if (held) {
        // 6.3「非照射時に回復」。押している間は回復しないので、消費できた秒数が
        // そのまま線を引ける長さになる。
        const float spent = emitter.battery.Drain(dt);
        if (spent > 0.0f) {
            emitter.emitting     = true;
            emitter.pressEmitted = true;
            Sweep(emitter, spent, dt);
        } else {
            HideBeam(emitter);
        }
    } else {
        emitter.battery.Refill(dt, tuning->batterySeconds, tuning->batteryRefillSeconds,
                               tuning->batteryRearmRatio);
        HideBeam(emitter);
    }

    UpdateBeamLoop(emitter);

    // 尽きた瞬間だけ鳴らす。空の間ずっと鳴らすと、音が状態ではなく背景になる。
    if (emitter.battery.depleted && !emitter.wasDepleted)
        se::Play(audio, sfxEmpty, se::BatteryEmpty(emitter.polarity));
    emitter.wasDepleted = emitter.battery.depleted;

    // 満タンに戻った瞬間。6.3 は「時間が資源」だと言っているので、次の 1 本を
    // 引き切れるようになったことは、ゲージを見ていなくても分かる必要がある。
    const bool full = emitter.battery.Ratio(tuning->batterySeconds) >= kBatteryFullRatio;
    if (full && !emitter.wasFull) se::Play(audio, se::BatteryFull(emitter.polarity));
    emitter.wasFull = full;

    // タップは塗り時間に届かなくても点付与を確定させる (6.2)。
    // WHY 長押しの塗りと重ねて構わないか: 塗り切っていれば 7 章のルール 2 で
    //     延長になるだけで、7.9 の起爆点としての意味は変わらない。
    if (tapped && emitter.pressEmitted) Detonate(emitter);
}

inline void PolarityGunComponent::Sweep(Emitter& emitter, float contactSeconds, float dt)
{
    auto* aim = Aim();
    if (!aim || !aim->HasAim()) {
        // 6 章のなぞりはこのゲームの入力そのもの。線が引けない状態で照射できると、
        // 「塗れない」原因がプレイヤーの操作に見えてしまう。
        if (!aim && !m_warnedNoAim) {
            m_warnedNoAim = true;
            debug.LogError("PolarityGunComponent requires PlayerAimComponent "
                           "on the same object.");
        }
        HideBeam(emitter);
        return;
    }

    // 塗る前に立てる。線の «張り» は塗り切ったかどうかではなく、対象が線上に
    // 居るかどうかで決まる (塗り切った後も当て続けている間は張っていてほしい)。
    if (!aim->Contacts().empty()) emitter.contactPulse = 1.0f;

    ShowBeam(emitter, MuzzlePosition(emitter.polarity), aim->AimPoint(), dt);

    // 6.2「貫通する。線上の敵すべてに判定が乗る」。手前で止めない。
    for (const BeamContact& contact : aim->Contacts()) {
        PolarityResult result{};
        if (contact.target->Paint(emitter.polarity, contactSeconds, result))
            PlayPaintFeedback(result, emitter.polarity);
    }
}

inline void PolarityGunComponent::Detonate(Emitter& emitter)
{
    auto* aim = Aim();
    if (!aim) return;

    // 起爆点は線のいちばん手前。7.9 の「線から外して温存した 1 体」がそこに来る。
    auto* target = aim->CurrentPolarityTarget();
    if (!target) return;

    // 起爆は「なぞり」ではなく 1 手の操作なので、塗り確定音とは別に打点の音を返す。
    // これが無いと、7.9 の温存 → 起爆が長押しの一部に聞こえてしまう。
    se::Play(audio, se::Tap(emitter.polarity));

    // 画角を一瞬だけ広げる。7.9 の起爆はプレイヤーが仕込んだ結果が動き出す瞬間で、
    // 音と塗り色だけでは「今それが起きた」ことが線の見た目に埋もれる。
    // WHY カメラ揺れを使わないか: 揺れは受けた衝撃の表現で、盤面が動き出すのは
    //     こちらが仕掛けた側の出来事。広がる画角の方が「解き放った」に近い。
    if (auto* follow = CameraFollowManagerComponent::Instance())
        follow->PunchFov(1.0f);

    PlayPaintFeedback(target->Apply(emitter.polarity), emitter.polarity);
}

inline void PolarityGunComponent::PlayPaintFeedback(const PolarityResult& result,
                                                    Polarity emitted)
{
    // 12.4 は「中和・延長した瞬間にも明確なフィードバックを返す」を仕様として要求する。
    // 同じ音で済ませると、狙って中和したのか事故だったのかが耳で判別できない。
    //
    // WHY 付与と延長も分けるか: 素材が Paint_Confirm と Paint_Extend で別に入っている。
    //     7 章のルールでは延長は「既に持っている極を伸ばした」= 新しく塗れてはいない。
    //     同じ音にすると、なぞりで拾えたつもりの 1 体が実は延長だったことに気付けない。
    switch (result.change) {
    case PolarityChange::Neutralized:
        se::Play(audio, sfxNeutralize, se::kNeutralize);
        break;
    case PolarityChange::Applied:
        se::Play(audio, sfxPaint, se::PaintConfirm(emitted));
        break;
    case PolarityChange::Extended:
        // 上書きが空なら延長専用の音へ。Inspector で指定されていればそれを優先する。
        se::Play(audio, sfxPaint, se::kPaintExtend);
        break;
    }
}

inline void PolarityGunComponent::UpdateBeamLoop(Emitter& emitter)
{
    if (emitter.emitting == emitter.loopPlaying) return;
    emitter.loopPlaying = emitter.emitting;

    auto* weapon = WeaponAnim(emitter.polarity);
    if (!weapon) return;

    if (emitter.loopPlaying) {
        weapon->SetBeamLoop(se::BeamLoop(emitter.polarity).First());
    } else {
        weapon->SetBeamLoop({});
        // 止めた瞬間の減衰音。ループが無音になるだけだと、照射をやめたのか
        // バッテリーが尽きたのか耳で区別できない。
        se::Play(audio, se::BeamEnd(emitter.polarity));
    }
}

// ── ビームの見た目 ────────────────────────────────────────────────────────────

inline std::string PolarityGunComponent::BeamName(Polarity polarity, const char* layer) const
{
    // WHY 持ち主ごとに名前を変えるか: ビームはルートに置くため、名前で拾い直すときに
    //     同名だと 2 人目のプレイヤー (デバッグ用の複製を含む) が 1 人目のビームを奪う。
    GameObject* owner = scene.Self();
    return std::string("PolarityBeam_") + (polarity == Polarity::Plus ? "Plus" : "Minus")
         + "_" + layer + "_" + (owner ? owner->instanceId : std::string{});
}

inline void PolarityGunComponent::BuildBeam(Emitter& emitter)
{
    emitter.core = EntityRef{ BuildBeamPart(BeamName(emitter.polarity, "Core"))->GetID() };
    emitter.glow = glowWidth > 0.0f
        ? EntityRef{ BuildBeamPart(BeamName(emitter.polarity, "Glow"))->GetID() }
        : EntityRef{};
    // 放電の筋も BeamName と同じ «持ち主 + 極» の鍵で拾い直させる。鍵が無いと
    // DLL リロードのたびに筋が増え、2 人目のプレイヤーが 1 人目の筋を奪う。
    emitter.arc.SetKey(BeamName(emitter.polarity, "Arc"));
    emitter.impactArc.SetKey(BeamName(emitter.polarity, "Tip"));
    emitter.beamVisible = true; // 直後の HideBeam に確実に畳ませる
    HideBeam(emitter);
}

inline GameObject* PolarityGunComponent::BuildBeamPart(const std::string& name)
{
    // WHY 先に拾い直すか: スクリプト DLL をリロードするとこの Script は作り直され、
    //     EntityRef は空に戻る。一方 ビームの GameObject は Scene 側に残っているため、
    //     拾わずに作り直すとリロードのたびに 2 本ずつ増えていく。
    GameObject* existing = scene.Find(name);
    if (!existing) {
        // WHY プレイヤーの子にしないか: 子にするとビームがプレイヤーの移動・回転・スケールを
        //     引き継ぎ、ワールド座標で指定した両端がそのぶん歪む。ルートへ原点で置く。
        GameObject& object = scene.Create(name);
        object.runtimeGenerated = true;
        existing = &object;
    }

    // 帯の形と .mat は BeamTrailRendererComponent が毎フレーム入れ直す。ここは
    // «誰が描く帯なのか» を決めるところまでで、線の設定は持たない。
    //
    // WHY GetScript してから足すか: 拾い直した個体には前回のコンポーネントが
    //     残っている。無条件に足すと、リロードのたびに同じ帯を 2 つ 3 つと
    //     描く Script が積み上がる。
    if (!scene.GetScript<BeamTrailRendererComponent>(existing))
        existing->AddScript<BeamTrailRendererComponent>();
    return existing;
}

// 芯層の断面。Inspector へ出していないのは、この 2 つを触ると «芯とグローの 2 層» という
// 12.3 の構成そのものが崩れるため (芯を鈍らせるとグロー層と区別が付かなくなる)。
// 線の太さと明るさは Beam / Beam Shape グループの側で振れる。
inline constexpr float kBeamCoreEdgeFalloff = 2.0f;
inline constexpr float kBeamCoreBoost       = 2.2f;
inline constexpr float kBeamBeadFalloff     = 12.0f;

// 位相と模様の送りを巻き取る周期。シェーダーの frac(sin(x * 12.9898)) は x が
// 大きくなるほど精度を失い、放置すると乱れが縞へ潰れる (ElectricArc と同じ理由)。
//
// WHY 整数で巻くか: scroll は素材の uv と粒の位相へそのまま足される。どちらも
//     周期 1 で繰り返すので、整数で巻き戻せば絵は 1 ドットも動かない。
inline constexpr float kBeamPhaseWrap  = 128.0f;
inline constexpr float kBeamScrollWrap = 1024.0f;

inline PolarityGunComponent::Unrest
PolarityGunComponent::UnrestOf(const Emitter& emitter) const
{
    // 残量が減るほど荒れる。1 = 満タン / 0 = 空。
    const float drained = 1.0f - Clamp01(emitter.battery.Ratio(tuning->batterySeconds));
    const float unrest  = drained * Clamp01(lowBatteryUnrest);
    const float contact = Clamp01(emitter.contactPulse);

    Unrest out;
    // WHY 命中でも荒れを増やすか: 明るさだけを上げると «ビームが太くなった» に見えて、
    //     何かに当たったのか自分が動いたのか区別が付かない。乱れが増えれば、
    //     線の向こうで «噛んでいる» ことが線そのものから読める。
    out.crackle = Clamp01(crackle + unrest * 0.45f + contact * 0.15f);
    out.snake   = Clamp01(snake   + unrest * 0.35f + contact * 0.20f);
    out.flicker = Clamp01(flicker + unrest * 0.55f);
    out.beads   = Max(chargeBeads, 0.0f);
    out.energy  = 1.0f + Max(contactBoost, 0.0f) * contact;
    out.surge   = contact;
    return out;
}

inline BeamTrailStyle PolarityGunComponent::StyleOf(const Emitter& emitter, bool isCore,
                                                   const Unrest& unrest) const
{
    const Vector4 base = PolarityColor(emitter.polarity);
    // 12.2 の «明るさより彩度» は保つ。命中で増やすのは倍率だけで、白は混ぜない。
    const float brightness = (isCore ? coreBrightness : glowBrightness) * unrest.energy;
    // グロー層は «淡い裾» なので、同じ量で振ると画面全体が明滅する。
    // 芯が暴れて裾がゆっくり呼吸する、という差が «芯とグローの 2 層» を保つ。
    const float layer = isCore ? 1.0f : 0.35f;

    BeamTrailStyle style;
    style.materialPath = beamMaterial;
    style.isCore       = isCore;
    // 芯を後ろ (order 1) に置いてグロー (order 0) の上へ重ねる。順番が逆だと
    // 淡い層が芯を覆い、どこが線の中心なのか読めなくなる。
    style.orderInLayer = isCore ? 1 : 0;
    style.width        = isCore ? coreWidth : glowWidth;
    style.color        = { base.x * brightness, base.y * brightness, base.z * brightness,
                           isCore ? 1.0f : glowOpacity };

    // 左右で違う形にする。同じ鍵だと 2 本のビームが完全に同じうねり方をして、
    // «1 本を鏡写しにした絵» に見える。芯と裾には同じ鍵を渡す (同じ波を共有させる)。
    style.seed             = emitter.polarity == Polarity::Plus ? 17u : 8191u;
    // 残量が減るほど帯そのものが暴れる。6.3 の «時間が資源» をゲージを見ずに読ませる。
    style.wobble           = Max(wobble, 0.0f) * (1.0f + unrest.crackle * 0.4f);
    style.wobbleScale      = isCore ? 1.0f : Clamp01(wobbleGlow);
    style.wobbleFrequency  = Max(wobbleFrequency, 0.0f);
    style.wobbleTravel     = wobbleTravel;
    style.wobbleBias       = Clamp01(wobbleBias);
    style.segmentsPerMeter = Max(wobbleSegments, 0.1f);

    style.coreWidth   = coreSharpness;
    style.edgeFalloff = isCore ? kBeamCoreEdgeFalloff : glowSoftness;
    style.coreBoost   = kBeamCoreBoost;
    style.tiling      = Max(stripeDensity, 0.0f);
    // WHY 位相を自前で積まないか: Time::time はヒットストップで止まる。止まった画面で
    //     ビームだけ流れ続けると、時間が止まったことの方が嘘に見える。
    style.scroll      = std::fmod(-Time::time * scrollSpeed, kBeamScrollWrap);
    style.muzzleFade  = muzzleFade;
    style.tipFade     = tipFade;
    style.phase       = std::fmod(Time::time * Max(churnRate, 0.0f), kBeamPhaseWrap);
    style.arcAmp      = unrest.snake * layer;
    style.arcFreq     = Max(snakeFrequency, 0.0f);
    style.crackle     = unrest.crackle * layer;
    style.flicker     = unrest.flicker * layer;
    style.beadDensity = unrest.beads;
    style.beadFalloff = kBeamBeadFalloff;
    style.surge       = unrest.surge;
    return style;
}

inline BeamTrailRendererComponent* PolarityGunComponent::TrailOf(const EntityRef& ref) const
{
    GameObject* object = ref.Resolve(scene);
    if (!object) return nullptr;
    auto* trail = scene.GetScript<BeamTrailRendererComponent>(object);
    // 実行時に足した Script は、ScriptSystem が SetContext を通すまでプロキシが
    // 繋がっていない。張らせるのは OnStart を抜けてからにする。
    return (trail && trail->IsReady()) ? trail : nullptr;
}

inline void PolarityGunComponent::UpdateBeamArc(Emitter& emitter, const Vector3& from,
                                                const Vector3& to, const Unrest& unrest,
                                                float dt)
{
    if (!drawArc || arcStrands <= 0) {
        emitter.arc.Extinguish(*this);
        return;
    }

    const float   length = (to - from).Length();
    const Vector4 rail   = PolarityColor(emitter.polarity);

    ElectricArcStyle style;
    style.strandCount = arcStrands;
    // 折れ点は長さから決める。40m の線を 24 点で折ると 1 区間 1.7m の «稲妻» になり、
    // 近距離と遠距離で放電の細かさが別物に見える。
    style.segments    = static_cast<int>(Clamp(length * 2.0f, 10.0f, 48.0f));
    // WHY 振れ幅を長さに «比例» させるか:
    //   放電が電気に見えるかは «線の長さに対して何割はみ出すか» で決まる。固定幅にすると、
    //   12.5 のエネルギーライン (敵どうしは 5m で 0.5m = 1 割) と同じ値でも、40m 先を
    //   撃ったビームでは 1% しか外れず «少しぼやけた直線» にしかならない。
    //   10m を基準にして比例させれば、どの距離でも同じ «電気» に見える。
    style.amplitude   = Max(arcAmplitude, 0.0f) * Clamp(length / 10.0f, 0.35f, 3.0f)
                      * (0.7f + 0.3f * unrest.energy);
    // 銃口は押さえが効き、着弾側で暴れる。Beam.hlsl の蛇行と同じ配分に揃える。
    style.taperBias   = 0.75f;
    style.width       = Max(arcWidth, 0.001f);
    style.strikeRate  = Max(arcRate, 1.0f);
    // 距離で消さない。6.2 の射程 (40m) まで届く線なので、ここで減衰させると
    // 遠くを撃ったときだけ放電が消えて «別の武器» に見える。
    style.strikeRange = 0.0f;
    // 芯 (order 1) の上へ。放電が帯に隠れると重ねた意味が無い。
    style.orderInLayer = 2;

    style.intensity   = Max(arcIntensity, 0.0f) * unrest.energy;
    style.breakup     = Clamp01(0.35f + unrest.crackle * 0.5f);
    style.travel      = 9.0f;
    style.coreTint    = 0.6f;
    style.beadDensity = unrest.beads * 0.5f;
    style.fromColor   = rail;
    style.toColor     = rail;

    emitter.arc.Update(*this, from, to, style, dt);
}

inline void PolarityGunComponent::UpdateForks(Emitter& emitter, const Vector3& from,
                                              const Vector3& to, const Unrest& unrest,
                                              float dt)
{
    // math::Clamp は float 版しか無い。本数を float 経由で丸めると境界で 1 本ぶれる。
    const int wanted = drawArc ? std::clamp(forkCount, 0, 4) : 0;

    // 減らしたぶんは畳む。消灯だけだと Inspector で 0 にしても筋が残り続ける。
    while (static_cast<int>(emitter.forks.size()) > wanted) {
        emitter.forks.back().Detach(*this);
        emitter.forks.pop_back();
    }
    if (wanted <= 0) return;

    const Vector3 delta  = to - from;
    const float   length = delta.Length();
    if (length <= EPSILON) return;
    const Vector3 axis = delta * (1.0f / length);

    // 軸に垂直な 2 軸。軸が真上に近いときだけ基準を前方へ倒す (外積が縮退するため)。
    const Vector3 reference = Abs(axis.y) > 0.9f ? Vector3::FORWARD : Vector3::UP;
    const Vector3 side = Vector3::Cross(axis, reference).Normalized();
    const Vector3 up   = Vector3::Cross(side, axis);

    const Vector4 rail   = PolarityColor(emitter.polarity);
    const uint32_t stem  = emitter.polarity == Polarity::Plus ? 17u : 8191u;

    for (int i = 0; i < wanted; ++i) {
        const auto slot = static_cast<std::size_t>(i);
        if (slot >= emitter.forks.size()) {
            emitter.forks.emplace_back();
            emitter.forks.back().SetKey(BeamName(emitter.polarity, "Fork")
                                        + "_" + std::to_string(slot));
        }

        const uint32_t seed = stem + static_cast<uint32_t>(i) * 7919u;
        // 根元は線の上を «滑る»。毎フレーム別の位置へ飛ぶと、枝ではなく
        // «線のまわりで点滅する棒» に見える。
        const float where = 0.15f + 0.75f * (ArcNoise(seed, Time::time * 1.7f) * 0.5f + 0.5f);
        const Vector3 root = from + delta * where;

        // 行き先は根元から外へ。線に沿う成分を少し混ぜて、進行方向へ寝かせる
        // (垂直に生やすと «線から生えたトゲ» になり、電気に見えない)。
        const float spin = Time::time * 4.3f + static_cast<float>(i) * 2.4f;
        const Vector3 outward = (side * std::cos(spin) + up * std::sin(spin)).Normalized();
        const float   reach   = Max(forkLength, 0.0f)
                              * (0.45f + 0.55f * (ArcNoise(seed + 31u, Time::time * 3.1f)
                                                  * 0.5f + 0.5f));
        const Vector3 tip = root + (outward * 0.85f + axis * 0.35f) * reach;

        ElectricArcStyle style;
        style.strandCount = 1;
        style.segments    = 10;
        style.amplitude   = reach * 0.35f;
        // 根元は本線に刺さっていてほしいので、暴れるのは先端側。
        style.taperBias   = 0.7f;
        style.width       = Max(arcWidth, 0.001f) * 0.7f;
        // 本線より速く組み替える。枝は «一瞬走って消える» ものなので、本線と同じ
        // 頻度だと «常時生えている» ように見える。
        style.strikeRate  = Max(arcRate, 1.0f) * 1.6f;
        style.strikeRange = 0.0f;
        style.orderInLayer = 2;
        style.intensity   = Max(arcIntensity, 0.0f) * 0.75f * unrest.energy;
        style.breakup     = 0.8f;
        style.travel      = 14.0f;
        style.coreTint    = 0.7f;
        style.fromColor   = rail;
        style.toColor     = rail;

        emitter.forks[slot].Update(*this, root, tip, style, dt);
    }
}

inline void PolarityGunComponent::UpdateImpactArc(Emitter& emitter, const Unrest& unrest,
                                                  float dt)
{
    auto* aim = Aim();
    // 敵に当たっている間は出さない。敵は動き続けるので、体の «表面» に沿わせられず
    // 放電が体を突き抜ける。敵側の反応は 12.4 の明滅と付与の演出が既に持っている。
    if (!drawImpactArc || !aim || !aim->HitGeometry()) {
        emitter.impactArc.Extinguish(*this);
        return;
    }

    const Vector3 point  = aim->AimPoint();
    const Vector3 normal = aim->SurfaceNormal().NormalizedOr(Vector3::UP);
    // 面に沿う 2 軸。法線が真上に近いときだけ基準を前方へ倒す (外積が縮退するため)。
    const Vector3 reference = Abs(normal.y) > 0.9f ? Vector3::FORWARD : Vector3::UP;
    const Vector3 tangent   = Vector3::Cross(normal, reference).Normalized();
    const Vector3 bitangent = Vector3::Cross(normal, tangent);

    // 這う先を «ゆっくり回しながら伸び縮みさせる»。毎フレーム別の方向へ飛ばすと
    // 放電ではなく点滅する星に見える。回転と長さを連続にすると、面の上を
    // 探るように這う。極ごとに位相をずらして左右の放電を別物にする。
    const bool     plus  = emitter.polarity == Polarity::Plus;
    const uint32_t seed  = plus ? 1u : 977u;
    const float    spin  = Time::time * 6.7f + (plus ? 0.0f : 2.1f);
    const float    wave  = ArcNoise(seed, Time::time * 5.0f) * 0.5f + 0.5f; // [0,1]
    const float    reach = 0.45f + 0.55f * wave;
    const Vector3 spoke = (tangent * std::cos(spin) + bitangent * std::sin(spin))
                        * (Max(impactArcRadius, 0.0f) * reach);
    // 面から少しだけ浮かせる。真上に置くと Z ファイトで放電が縞に割れる。
    const Vector3 lift = normal * 0.04f;

    const Vector4 rail = PolarityColor(emitter.polarity);

    ElectricArcStyle style;
    style.strandCount  = 2;
    style.segments     = 12;
    style.amplitude    = Max(impactArcRadius, 0.0f) * 0.35f;
    // 着弾点は «根» なので振れを 0 に保ち、逃げていく先で暴れさせる。
    style.taperBias    = 0.85f;
    style.width        = Max(arcWidth, 0.001f) * 0.8f;
    style.strikeRate   = Max(arcRate, 1.0f) * 1.4f;
    style.strikeRange  = 0.0f;
    style.orderInLayer = 2;
    style.intensity    = Max(arcIntensity, 0.0f) * 1.2f * unrest.energy;
    style.breakup      = 0.7f;
    style.travel       = 12.0f;
    style.coreTint     = 0.5f;
    style.fromColor    = rail;
    style.toColor      = rail;
    // 帯の放電より芯を落ち着かせる。着弾点は爆発の «核» ではなく «焦げる» 側なので、
    // ここまで白熱させると当たった点が線より明るくなり、線の先が読めなくなる。
    style.coreColor    = { 5.0f, 4.6f, 4.4f, 1.0f };

    emitter.impactArc.Update(*this, point + lift, point + spoke + lift, style, dt);
}

inline void PolarityGunComponent::ShowBeam(Emitter& emitter, const Vector3& from,
                                           const Vector3& to, float dt)
{
    if ((to - from).LengthSq() <= EPSILON) {
        HideBeam(emitter);
        return;
    }
    emitter.beamVisible = true;

    const Unrest unrest = UnrestOf(emitter);

    // 帯をどう曲げるかは BeamTrailRendererComponent が持つ。ここは «どんな線か» を
    // 組んで渡すだけで、点列も .mat への流し込みも向こう側の仕事。
    if (auto* core = TrailOf(emitter.core))
        core->Show(from, to, StyleOf(emitter, true, unrest));
    if (glowWidth > 0.0f)
        if (auto* glow = TrailOf(emitter.glow))
            glow->Show(from, to, StyleOf(emitter, false, unrest));

    UpdateBeamArc(emitter, from, to, unrest, dt);
    UpdateForks(emitter, from, to, unrest, dt);
    UpdateImpactArc(emitter, unrest, dt);

    // WHY 判定線をデバッグで «別に» 引くか: 帯は頂点ごとに振れているので、塗る相手を
    //     決めている直線とは形が違う。重ねて初めて «どれだけ外れているか» が見える。
    if (drawDebugBeam)
        debug.DrawLine(from, to, PolarityColor(emitter.polarity));
}

inline void PolarityGunComponent::HideBeam(Emitter& emitter)
{
    if (!emitter.beamVisible) return;
    emitter.beamVisible = false;

    if (auto* core = TrailOf(emitter.core)) core->Hide();
    if (auto* glow = TrailOf(emitter.glow)) glow->Hide();
    emitter.arc.Extinguish(*this);
    emitter.impactArc.Extinguish(*this);
    for (ElectricArcBundle& fork : emitter.forks) fork.Extinguish(*this);
}

inline void PolarityGunComponent::ReleaseBeam(Emitter& emitter)
{
    // ルートに置いた以上、プレイヤーと一緒には消えない。持ち主が畳む。
    if (GameObject* core = emitter.core.Resolve(scene)) scene.Destroy(*core);
    if (GameObject* glow = emitter.glow.Resolve(scene)) scene.Destroy(*glow);
    emitter.arc.Detach(*this);
    emitter.impactArc.Detach(*this);
    for (ElectricArcBundle& fork : emitter.forks) fork.Detach(*this);
    emitter.forks.clear();
    emitter.core = {};
    emitter.glow = {};
}

// ── 銃 ────────────────────────────────────────────────────────────────────────

inline GameObject* PolarityGunComponent::WeaponObject(Polarity polarity) const
{
    const HandSide hand = HandOf(polarity);
    const auto& ref = (polarity == Polarity::Plus) ? weaponPlus : weaponMinus;

    // 参照先が本当にその手の銃かを名前で確かめる (理由は WeaponSockets.hpp の IsWeaponObject)。
    if (GameObject* object = ref.Get(); IsWeaponObject(object, hand))
        return object;
    return scene.Find(WeaponObjectName(hand));
}

inline WeaponAnimatorComponent* PolarityGunComponent::WeaponAnim(Polarity polarity) const
{
    GameObject* weapon = WeaponObject(polarity);
    return weapon ? scene.GetScript<WeaponAnimatorComponent>(weapon) : nullptr;
}

inline Vector3 PolarityGunComponent::MuzzlePosition(Polarity polarity) const
{
    // 1) Inspector で明示的に指定されたマズル (演出上ずらしたい場合)
    const auto& muzzle = (polarity == Polarity::Plus) ? muzzleRight : muzzleLeft;
    if (GameObject* object = muzzle.Get())
        return object->transform.worldPosition;

    // 2) 銃側が解決済みの SOCKET_Muzzle
    // WHY scene.Find("SOCKET_Muzzle") としないか: 同名ソケットが左右の銃に 1 本ずつ
    //     存在するため、グローバル検索ではどちらが返るか GameObject の生成順に依存する。
    //     症状が日替わりになる種類のバグなので、必ず銃の部分木から引く。
    if (auto* weapon = WeaponAnim(polarity); weapon && weapon->HasMuzzleSocket())
        return weapon->MuzzlePosition();

    // 3) 旧シーン救済 (FBX 再インポート前)
    if (GameObject* weapon = WeaponObject(polarity)) {
        const bool right = polarity == Polarity::Plus;
        if (GameObject* socket = FindSocketInSubtree(
                *weapon, kSocketMuzzle,
                right ? kLegacySocketMuzzleR : kLegacySocketMuzzleL))
            return socket->transform.worldPosition;
    }

    // WHY Player 原点へ落とすのを最後にするか: ここへ来ると銃を手へ移した後も
    //     ビームだけが足元から伸びる。見た目が微妙にズレるだけなので気付きにくい。
    return transform.worldPosition;
}

} // namespace sandbox
