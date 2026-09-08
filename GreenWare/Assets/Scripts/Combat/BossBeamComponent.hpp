/// @file    BossBeamComponent.hpp
/// @brief   ボスのコアビーム。線の «見え» と «当たり» の両方をここ 1 つが持つ
/// @author  Hasegawa Jin
/// @date    2026-08-26
///
/// WHY 見えと当たりを 1 つに持たせるか:
///   企画書 8 章はコアビームの役割を «移動を強制する» と定義している。避けられたかどうかが
///   絵と一致していなければ、その役割そのものが成立しない。線を引く側と判定する側を
///   別のスクリプトに分けると、片方が «アパーチャから接地点» でもう片方が «ボスの前方» を
///   見ている、という食い違いが黙って成立してしまう。端点を 1 か所で決めて、
///   そこから «描く» と «測る» の両方を出す。
///
/// WHY 帯を張るのは BeamTrailRendererComponent へ任せるか:
///   折れ線を組んで LineRenderer へ流し込む仕事は、プレイヤーのビームで既に解いてある。
///   «帯をどう曲げるか» に武器ごとの違いは無く、違うのは断面の絵 (シェーダー) と
///   曲げ幅の数値だけ。組み立てを複製すると、片方だけ直した歪みが残る。
///
/// WHY 接地点を毎フレーム地面へ落とすか (前方の固定距離で済ませないか):
///   8 章は «地面へ照射し、旋回して薙ぐ» と書いている。段差やスロープで線が地面へ
///   刺さらないと、薙いでいるのに焦げが空中に浮く。線の終端は必ず «何かに当たった点»
///   でなければ、接地の VFX が置き場所を失う。
///
/// WHY 点火 (charge) を線の側で持つか:
///   Beam_Start の 30F は «構え» で、その間ビームが有る / 無いの 2 値だと 1 フレームで
///   全開の線が生える。針から本径まで太らせる値を線が持てば、AI は撃つ / 止めるだけを
///   言えばよく、予兆の作り方はビームの中に閉じる。
#pragma once

#include <Engine/Scene/Components/LightComponent.hpp>
#include <Engine/Scene/EntityRef.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Scripts/Combat/BossCoreComponent.hpp>
#include <Scripts/Game/CameraShakeManagerComponent.hpp>
#include <Scripts/Game/CombatManagerComponent.hpp>
#include <Scripts/Game/RumbleManagerComponent.hpp>
#include <Scripts/Game/ScreenEffectManagerComponent.hpp>
#include <Scripts/Game/VfxManagerComponent.hpp>
#include <Scripts/Utils/BeamLook.hpp>
#include <Scripts/Utils/BeamTrailRendererComponent.hpp>
#include <Scripts/Utils/ElectricArc.hpp>
#include <Scripts/Utils/BeamGeometry.hpp>
#include <Scripts/Utils/BladeColors.hpp>
#include <Scripts/Utils/ShockFalloff.hpp>
#include <Scripts/Utils/WeaponSockets.hpp>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

/// BossBeam.hlsl のボス固有パラメーター。
/// WHY 定数にするか: MaterialInstance はシェーダーリフレクションで名前を検証し、
///     違えば黙って捨てる。綴りの突き合わせ先を 1 箇所へ閉じる。
inline constexpr MaterialPropertyId kBossBeamChargeId{ "charge" };

/// 線と焦げの色。ボスは色を切り替えないので 1 本に固定する。
inline constexpr BladeSide kBeamSide = BladeSide::Right;

/// 薙ぎの «下地» で低周波モーターを高周波の何割で回すか。
/// WHY 高周波を主にするか: 低周波は «一撃» の器で、数秒回し続けると手が痺れて
///     接地点が寄ってきたことが読めなくなる。薙ぎは打撃ではなく «焼いている音»。
inline constexpr float kBossBeamSweepLowRatio = 0.45f;

class BossBeamComponent : public Script {
    FBZZ_SCRIPT(BossBeamComponent)

public:
    FBZZ_GROUP("Aperture")
    FBZZ_FIELD(std::string, apertureBone, "Muzzle", "Aperture Bone")
    FBZZ_TOOLTIP("下面アパーチャのボーン (Docs/boss.md)。見つからなければ胴体の下から撃つ")
    FBZZ_FIELD_RANGE(float, apertureFallbackHeight, 3.6f, "Fallback Height", 0.0f, 10.0f)
    FBZZ_TOOLTIP("ボーンが引けなかったときに使う、ボス原点からの高さ [m]")

    FBZZ_GROUP("Beam")
    FBZZ_FIELD_FILE(beamMaterial, "Assets/Materials/Effects/FX_BOSS_Beam.mat",
                    "Material", ".mat")
    FBZZ_FIELD_RANGE(float, coreWidth, 0.62f, "Core Width", 0.02f, 4.0f)
    FBZZ_TOOLTIP("芯層の帯の太さ [m]。当たり判定の太さとは別 (Hit Radius が正本)")
    FBZZ_FIELD_RANGE(float, glowWidth, 1.90f, "Glow Width", 0.0f, 8.0f)
    FBZZ_TOOLTIP("裾層の太さ [m]。0 で裾を出さない")
    FBZZ_FIELD_RANGE_INT(int, tubeSegments, 12, "筒の分割数", 3, 32)
    FBZZ_TOOLTIP("筒の円周分割数。少ないと近寄ったとき角が見え、増やしても遠目には変わらない")
    FBZZ_FIELD_RANGE(float, intensity, 1.6f, "強さ", 0.0f, 8.0f)
    FBZZ_FIELD_RANGE(float, wobble, 0.05f, "Wobble", 0.0f, 1.0f)
    FBZZ_TOOLTIP("10m 先での帯の振れ幅 [m]。ボスの線は «変わらない» のが読みなので浅く")
    FBZZ_FIELD_RANGE(float, scrollSpeed, 2.2f, "Scroll Speed", -20.0f, 20.0f)
    FBZZ_FIELD_RANGE(float, churnRate, 1.4f, "Churn Rate", 0.0f, 20.0f)
    FBZZ_TOOLTIP("乱れの位相が進む速さ。段が落ちる速さもこれに乗る")

    FBZZ_GROUP("Ignition")
    FBZZ_FIELD_RANGE(float, chargeTime, 1.00f, "チャージ", 0.0f, 4.0f)
    FBZZ_TOOLTIP("Beam_Start の 30F = 1.0 秒。針から本径まで太る時間")
    FBZZ_FIELD_RANGE(float, dischargeTime, 0.80f, "Discharge", 0.0f, 4.0f)
    FBZZ_TOOLTIP("Beam_End の 24F = 0.8 秒。細って消えるまで")

    FBZZ_GROUP("Hit")
    FBZZ_FIELD_TAG(playerTag, "Player", "プレイヤーのタグ")
    FBZZ_FIELD_RANGE(float, hitRadius, 1.15f, "当たり半径", 0.1f, 6.0f)
    FBZZ_TOOLTIP("線の «当たりの太さ» [m]。見た目の帯より少し細くして、"
                 "«掠って見えたのに食らった» を避ける")
    FBZZ_FIELD_RANGE(float, playerRadius, 0.45f, "プレイヤーの半径", 0.0f, 3.0f)
    FBZZ_FIELD_RANGE(float, playerCenterHeight, 1.25f, "Player Center", 0.0f, 4.0f)
    FBZZ_TOOLTIP("プレイヤー原点 (足元) から胴体中心までの高さ [m]。"
                 "線を持ち上げたとき、足元との距離で測ると «胸を貫いているのに当たらない» になる")
    FBZZ_FIELD_RANGE(float, tickInterval, 0.35f, "Tick", 0.05f, 3.0f)
    FBZZ_TOOLTIP("照射中にダメージが入る間隔。毎フレームだと一瞬触れただけで溶ける")
    FBZZ_FIELD_RANGE_INT(int, damage, 1, "ダメージ", 0, 100)
    FBZZ_FIELD_RANGE(float, traceRange, 30.0f, "Trace Range", 1.0f, 80.0f)
    FBZZ_TOOLTIP("射線を伸ばす上限 [m]。何にも当たらなければここで線を切る。"
                 "振り上げたビームが壁へ届くよう、アリーナの差し渡しより長く取る")

    FBZZ_GROUP("Arcs")
    FBZZ_FIELD_RANGE_INT(int, apertureArcs, 3, "Aperture", 0, 8)
    FBZZ_TOOLTIP("点火中にアパーチャの周りで暴れる筋。«来る» を読ませる予兆そのもの")
    FBZZ_FIELD_RANGE_INT(int, beamArcs, 3, "ビームに沿って", 0, 8)
    FBZZ_TOOLTIP("筒の外側を這う筋。筒だけだと表面が硬いので、輪郭を崩す役")
    FBZZ_FIELD_RANGE_INT(int, groundArcs, 5, "At Contact", 0, 12)
    FBZZ_TOOLTIP("接地点から面を這って逃げる筋。焼いている «場所» を広く見せる")
    FBZZ_FIELD_RANGE_INT(int, arcStrands, 2, "筋の数", 1, 6)
    FBZZ_TOOLTIP("1 束あたりの筋の数。束の数 × これが実際の本数になる")
    FBZZ_FIELD_RANGE(float, arcWidth, 0.085f, "幅", 0.005f, 0.6f)
    FBZZ_FIELD_RANGE(float, arcRate, 26.0f, "打撃の頻度", 1.0f, 60.0f)
    FBZZ_TOOLTIP("形を組み替える頻度 [Hz]。上げるほど «ビリビリ» が細かくなる")
    FBZZ_FIELD_RANGE(float, arcIntensity, 2.0f, "強さ", 0.0f, 10.0f)
    FBZZ_FIELD_RANGE(float, arcBow, 1.1f, "たわみ", 0.0f, 6.0f)
    FBZZ_TOOLTIP("線に沿う筋が筒からどれだけ外へ膨らむか [m]")
    FBZZ_FIELD_RANGE(float, groundArcReach, 4.5f, "Contact Reach", 0.2f, 20.0f)
    FBZZ_FIELD_RANGE(float, apertureArcReach, 1.7f, "Aperture Reach", 0.1f, 8.0f)

    FBZZ_GROUP("エフェクト")
    FBZZ_FIELD_RANGE(float, scorchRate, 16.0f, "Scorch Rate", 0.0f, 60.0f)
    FBZZ_TOOLTIP("接地点へ焦げと火花を置く頻度 [回/秒]。0 で出さない")
    FBZZ_FIELD_RANGE(float, scorchSize, 1.6f, "Scorch Size", 0.05f, 2.0f)
    FBZZ_FIELD(bool, contactLight, true, "Contact Light")
    FBZZ_TOOLTIP("接地点に点光源を置く。線そのものは地面を照らさないので、"
                 "これが無いと «焼いている» のに床が暗いままになる")
    FBZZ_FIELD_RANGE(float, lightIntensity, 24.0f, "Light Intensity", 0.0f, 200.0f)
    FBZZ_FIELD_RANGE(float, lightRange, 9.0f, "ライトの範囲", 0.5f, 40.0f)

    // WHY 予兆と薙ぎで測る場所を分けるか:
    //   アパーチャは常にボスの腹下にあり、接地点は薙ぎに連れて盤面を走る。1 点で
    //   兼ねると «ボスが唸り始めた» と «焼く線が自分へ寄ってきた» が同じ強さで返り、
    //   どちらも «ビームが出ている» としか読めなくなる。8 章がこの攻撃に与えた
    //   «移動を強制する» は、線が寄ってきたことが判って初めて成立する。
    FBZZ_GROUP("手応え")
    FBZZ_FIELD_RANGE(float, chargeRumble, 0.45f, "Charge Rumble", 0.0f, 1.0f)
    FBZZ_TOOLTIP("点火の予兆。太りきるまで上がり続ける低周波の唸り。0 で出さない")
    FBZZ_FIELD_RANGE(float, chargeRange, 30.0f, "Charge Range", 1.0f, 80.0f)
    FBZZ_TOOLTIP("アパーチャからこの距離まで離れると予兆が届かなくなる。"
                 "ビームは 8〜18m から撃たれるので、そこを覆う値でないと予兆にならない")
    FBZZ_FIELD_RANGE(float, sweepRumble, 0.55f, "Sweep Rumble", 0.0f, 1.0f)
    FBZZ_TOOLTIP("照射中の下地。接地点の近さで決まるので、薙ぎが寄るほど強くなる")
    FBZZ_FIELD_RANGE(float, burnRumble, 1.00f, "Burn Rumble", 0.0f, 1.0f)
    FBZZ_TOOLTIP("線に触れているあいだ。焼かれていることは距離に関わらず振り切る")
    FBZZ_FIELD_RANGE(float, contactRange, 14.0f, "Contact Range", 1.0f, 60.0f)
    FBZZ_TOOLTIP("接地点からこの距離まで離れると、薙ぎの揺れも振動も 0 になる")
    FBZZ_FIELD_RANGE(float, contactNear, 2.0f, "全開になる距離", 0.0f, 20.0f)
    FBZZ_TOOLTIP("この距離までは減衰させない。足元を舐めた線が薄まらないための床")
    FBZZ_FIELD_RANGE(float, sweepShake, 0.35f, "Sweep Shake", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, shakeInterval, 0.12f, "Shake Interval", 0.02f, 1.0f)
    FBZZ_TOOLTIP("照射中に揺れを継ぎ足す間隔。短くしすぎると要求が上限に達して"
                 "同じ照射の揺れが互いを押し出す")
    FBZZ_FIELD_RANGE(float, ignitionShake, 0.50f, "Ignition Shake", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, burnDistortion, 0.70f, "Burn Distortion", 0.0f, 1.0f)
    FBZZ_TOOLTIP("線に触れているあいだ掛け続ける画面の歪み。抜けた瞬間に 0 へ戻る")

    FBZZ_GROUP("デバッグ")
    FBZZ_FIELD_READ_ONLY(float, debugCharge, 0.0f, "チャージ")
    FBZZ_FIELD_READ_ONLY(float, debugLength, 0.0f, "長さ")
    FBZZ_FIELD(bool, drawDebugHit, false, "Draw Hit Line")

    void OnStart()      override;
    void OnLateUpdate() override;
    void OnDisable()    override { Hide(); }
    // 放電の筋はルートに置いた GameObject なので、このスクリプトと一緒には消えない。
    void OnDestroy()    override
    {
        // 照射の途中でシーンが切り替わっても、パッドと画面の歪みを置き去りにしない。
        StopFeedback();
        for (ElectricArcBundle& arc : m_apertureArcs) arc.Detach(*this);
        for (ElectricArcBundle& arc : m_beamArcs)     arc.Detach(*this);
        for (ElectricArcBundle& arc : m_groundArcs)   arc.Detach(*this);
        m_apertureArcs.clear();
        m_beamArcs.clear();
        m_groundArcs.clear();
        // 着弾点の光も筋と同じくルートに置いた実体。破棄では OnDisable が呼ばれない
        // (Scene::Destroy は OnDestroy だけを回す) ので、照射中に消えると点いたまま残る。
        if (GameObject* light = m_light.Resolve(scene)) scene.Destroy(*light);
        m_light = {};
    }

    // ── AI からの入口 ───────────────────────────────────────────────────────

    /// 狙う点 (ワールド)。毎フレーム呼ぶ。終端は worldTarget の «真下の地面» から
    /// heightAboveGround だけ持ち上げた位置になる。
    ///
    /// WHY 高さを «地面からの相対» で受けるか: 呼ぶ側 (AI) が知っているのは «どれくらい
    ///     上へ向けたいか» だけで、その先の地面が何 m にあるかは知らない。絶対座標で
    ///     受けると、段差の上を薙いだ瞬間に線が地面へ潜る。
    void Aim(const Vector3& worldTarget, float heightAboveGround = 0.0f)
    {
        m_target  = worldTarget;
        m_aimLift = heightAboveGround;
        m_aimed   = true;
    }
    /// 照射の開始 / 停止。点火と消灯は線の側が時間をかけて処理する。
    void SetFiring(bool firing) { m_firing = firing; }

    [[nodiscard]] bool  IsFiring() const { return m_firing; }
    /// 点火 0..1。AI が «撃ち始めた» 予兆の進みを見るために使う。
    [[nodiscard]] float Charge01() const { return m_charge; }
    /// 今フレームの接地点。VFX や SE の置き場所として使う。
    [[nodiscard]] Vector3 ContactPoint() const { return m_contact; }
    /// 今フレーム、線がプレイヤーへ触れているか。
    [[nodiscard]] bool IsTouchingPlayer() const { return m_touching; }

private:
    /// 帯 1 層ぶんの GameObject を用意する。DLL リロードをまたいでも増えない。
    [[nodiscard]] GameObject* BuildLayer(const std::string& name);
    [[nodiscard]] std::string LayerName(const char* layer) const;
    [[nodiscard]] BeamTrailRendererComponent* TrailOf(const EntityRef& ref) const;
    /// 下面アパーチャのワールド位置。
    [[nodiscard]] Vector3 AperturePoint() const;
    /// アパーチャから狙点へ向けて射線を伸ばし、最初に当たった面で止める。
    /// 何にも当たらなければ最大距離まで伸ばす。outNormal には当たった面の法線。
    [[nodiscard]] Vector3 TraceContact(const Vector3& from, const Vector3& aimPoint,
                                       Vector3& outNormal) const;
    [[nodiscard]] Vector4 BeamColor() const;
    /// 明るさを 1 に正規化した極性色。«色が意味を持つ» 側だけが要る値。
    [[nodiscard]] Vector4 BeamHue() const;

    void Show(const Vector3& from, const Vector3& to);
    void Hide();
    /// 線分とプレイヤーの距離を測り、間隔を空けて当てる。
    void ResolveHit(const Vector3& from, const Vector3& to, float dt);
    /// 揺れ・振動・画面の歪みを今フレームの «点火と接地点» から出す。
    void DriveFeedback(const Vector3& from, float dt);
    /// 掛けっぱなしの手触りを畳む。線を消すときに必ず通す。
    void StopFeedback();
    void DriveEffects(float dt);
    /// 接地点の光。無ければ作り、照射していなければ消す。
    void DriveLight(bool lit);
    /// 放電 3 系統を今フレームの端点へ張り直す。
    void DriveArcs(const Vector3& from, const Vector3& to, float dt);
    /// 全部の束を消灯する。線を畳むときに必ず通す。
    void ExtinguishArcs();
    /// 束の数を揃え、鍵を配る。鍵が衝突すると束どうしが筋を奪い合う。
    void EnsureArcs(std::vector<ElectricArcBundle>& bundles, int count, const char* tag);
    /// 3 系統が共有する基本の見た目。膨らみ方と長さだけ呼び出し側が変える。
    [[nodiscard]] ElectricArcStyle ArcStyleBase(float brightness) const;

    /// 今フレームの «線の質»。斉射 (LaserVolleyComponent) と同じ器へ詰めて渡す。
    [[nodiscard]] beamlook::Look LookOf() const;
    /// 層 1 枚ぶんの見た目を組む。
    [[nodiscard]] BeamTrailStyle StyleOf(bool isCore) const
    {
        return beamlook::Style(LookOf(), isCore);
    }

    EntityRef m_core;
    EntityRef m_glow;
    EntityRef m_light;

    // 放電は «同じ電気の別の出方» なので 3 つに分ける。1 つの束で兼ねると、
    // 膨らむ場所 (taperBias) が 1 通りしか選べず、どれかが必ず嘘になる。
    std::vector<ElectricArcBundle> m_apertureArcs;
    std::vector<ElectricArcBundle> m_beamArcs;
    std::vector<ElectricArcBundle> m_groundArcs;
    /// 接地点の放電を回す角度。止めると «同じ形が明滅している» に見える。
    float m_arcSpin = 0.0f;

    Vector3 m_target  = Vector3::ZERO;
    Vector3 m_contact = Vector3::ZERO;
    /// 焼いている面の法線。床なら上、壁なら横を向く。焦げの向きに使う。
    Vector3 m_contactNormal = Vector3::UP;
    /// 狙点を床から持ち上げる量 [m]。AI が薙ぎの進みに合わせて上げる。
    float   m_aimLift = 0.0f;
    bool    m_aimed   = false;
    bool    m_firing  = false;
    bool    m_visible = false;
    bool    m_touching = false;

    float m_charge   = 0.0f;
    float m_hitTimer = 0.0f;
    float m_scorchTimer = 0.0f;
    /// 揺れを継ぎ足すまでの残り。毎フレーム積むと要求どうしが押し出し合う。
    float m_shakeTimer = 0.0f;
    /// この 1 射で点火しきった合図を返したか。撃つたびに 1 度だけ。
    bool  m_ignited = false;
    /// 掛けっぱなしの手触りを持っているか。持っていないのに畳むと、
    /// 他所が設定した歪みまで巻き添えで 0 にしてしまう。
    bool  m_feedbackActive = false;
    bool  m_warnedNoCombat = false;
};

FBZZ_REFLECT(BossBeamComponent)


inline std::string BossBeamComponent::LayerName(const char* layer) const
{
    // WHY 持ち主ごとに名前を変えるか: 帯はルートに置くため、名前で拾い直すときに
    //     同名だと 2 体目のボス (デバッグ用の複製を含む) が 1 体目の帯を奪う。
    GameObject* owner = scene.Self();
    return std::string("BossBeam_") + layer + "_" + (owner ? owner->instanceId : std::string{});
}

inline GameObject* BossBeamComponent::BuildLayer(const std::string& name)
{
    // WHY 先に拾い直すか: スクリプト DLL をリロードするとこの Script は作り直され、
    //     EntityRef は空に戻る。一方 帯の GameObject は Scene 側に残っているため、
    //     拾わずに作り直すとリロードのたびに 2 本ずつ増えていく。
    GameObject* existing = scene.Find(name);
    if (!existing) {
        // WHY ボスの子にしないか: 子にすると帯がボスの移動・回転を引き継ぎ、
        //     ワールド座標で指定した両端がそのぶん歪む。ルートへ原点で置く。
        GameObject& object = scene.Create(name);
        object.runtimeGenerated = true;
        existing = &object;
    }
    if (!scene.GetScript<BeamTrailRendererComponent>(existing))
        existing->AddScript<BeamTrailRendererComponent>();
    return existing;
}

inline BeamTrailRendererComponent* BossBeamComponent::TrailOf(const EntityRef& ref) const
{
    GameObject* object = ref.Resolve(scene);
    return object ? scene.GetScript<BeamTrailRendererComponent>(object) : nullptr;
}

inline void BossBeamComponent::OnStart()
{
    m_firing   = false;
    m_visible  = false;
    m_touching = false;
    m_aimed    = false;
    m_charge   = 0.0f;
    m_hitTimer = 0.0f;
    m_scorchTimer = 0.0f;
    m_shakeTimer = 0.0f;
    m_ignited = false;
    m_feedbackActive = false;
    m_warnedNoCombat = false;

    m_core = EntityRef{ BuildLayer(LayerName("Core"))->GetID() };
    m_glow = glowWidth > 0.0f ? EntityRef{ BuildLayer(LayerName("Glow"))->GetID() }
                              : EntityRef{};
    m_visible = true; // 直後の Hide に確実に畳ませる
    Hide();
}

inline Vector3 BossBeamComponent::AperturePoint() const
{
    if (GameObject* self = scene.Self()) {
        if (GameObject* bone = FindInSubtree(*self, apertureBone))
            return bone->transform.worldPosition;

        // ボーンが引けない構成でも «下から出ている» ことだけは守る。原点から出すと
        // 足元から生えて、腹下のアパーチャという設計がまるごと消える。
        Vector3 point = self->transform.worldPosition;
        point.y += std::max(apertureFallbackHeight, 0.0f);
        return point;
    }
    return transform.worldPosition;
}

inline Vector3 BossBeamComponent::TraceContact(const Vector3& from, const Vector3& aimPoint,
                                               Vector3& outNormal) const
{
    outNormal = Vector3::UP;

    const float   range = std::max(traceRange, 0.5f);
    const Vector3 dir   = (aimPoint - from).NormalizedOr(Vector3{ 0.0f, -1.0f, 0.0f });

    // WHY 真下ではなく射線に沿って探すか:
    //   薙ぎ終わりに線を振り上げると、狙点の «真下» には何も無い。真下を探す実装だと
    //   終端が空中に取り残され、焦げと火花が宙に湧く。射線を伸ばせば、下を向いている間は
    //   床に、振り上げた後はアリーナの壁に当たる。どちらでも «何かを焼いている» になる。
    //
    // WHY 最初の 1 件ではなく全件か:
    //   間にプレイヤーが立っていると、Raycast はそこで止まる。線がプレイヤーの手前で
    //   途切れると «当たっているのに刺さっていない» 絵になる。当たり判定は線分の側で
    //   別に測っているので、線そのものは «地形» だけで止める。
    GameObject* self = scene.Self();
    const RaycastHit* nearest = nullptr;
    const std::vector<RaycastHit> hits = physics.RaycastAll(from, dir, range);
    for (const RaycastHit& hit : hits) {
        GameObject* object = hit.gameObject;
        if (!object) continue;
        // 自分の部位 (脚のヒットボックス) で止まると、線が脚の上で切れる。
        if (object == self || (self && object->IsDescendantOf(*self))) continue;
        if (object->tag == playerTag) continue;
        if (!nearest || hit.distance < nearest->distance) nearest = &hit;
    }

    if (nearest) {
        outNormal = nearest->normal.NormalizedOr(Vector3::UP);
        return nearest->point;
    }
    // 何にも当たらないまま抜けた。線は最大距離で切る (空へ伸ばしっぱなしにしない)。
    return from + dir * range;
}

inline Vector4 BossBeamComponent::BeamColor() const
{
    // ボスは色を切り替えない。線の色は 1 本に固定する。
    const Vector4 tint = BladeColor(kBeamSide);
    const float   gain = std::max(intensity, 0.0f);
    return { tint.x * gain, tint.y * gain, tint.z * gain, 1.0f };
}

inline Vector4 BossBeamComponent::BeamHue() const
{
    // 明るさは各所が自分の倍率で決めるので、色として使いたい側は色相だけを取り出す。
    return beamlook::Hue(BeamColor());
}

inline beamlook::Look BossBeamComponent::LookOf() const
{
    beamlook::Look look;
    look.materialPath = beamMaterial;
    look.color        = BeamColor();
    look.coreWidth    = std::max(coreWidth, 0.01f);
    look.glowWidth    = std::max(glowWidth, 0.01f);
    look.tubeSegments = tubeSegments;
    look.charge       = m_charge;
    look.wobble       = wobble;
    look.scrollSpeed  = scrollSpeed;
    look.churnRate    = churnRate;
    return look;
}

inline void BossBeamComponent::Show(const Vector3& from, const Vector3& to)
{
    if (auto* core = TrailOf(m_core)) {
        core->Show(from, to, StyleOf(true));
        // 点火はシェーダーの «太さと明るさの元» なので、帯を張った後に毎フレーム押す。
        // BeamTrailRendererComponent は共通名しか書かないため、ここが受け持つ。
        const MaterialInstance instance = material.Instance(m_core, 0u);
        if (instance.HasProperty(kBossBeamChargeId))
            instance.SetFloat(kBossBeamChargeId, Clamp01(m_charge));
    }
    if (auto* glow = TrailOf(m_glow)) {
        glow->Show(from, to, StyleOf(false));
        const MaterialInstance instance = material.Instance(m_glow, 0u);
        if (instance.HasProperty(kBossBeamChargeId))
            instance.SetFloat(kBossBeamChargeId, Clamp01(m_charge));
    }
    m_visible = true;
    debugLength = (to - from).Length();
}

inline void BossBeamComponent::Hide()
{
    // 手触りは m_visible より先に畳む。線を 1 度も出さずに撃ち止めた経路 (狙いが
    // 付く前に AI が止めた) でも、点火の唸りだけは既に鳴っているため。
    StopFeedback();
    if (!m_visible) return;
    m_visible  = false;
    m_touching = false;
    if (auto* core = TrailOf(m_core)) core->Hide();
    if (auto* glow = TrailOf(m_glow)) glow->Hide();
    ExtinguishArcs();
    DriveLight(false);
    debugLength = 0.0f;
}

inline void BossBeamComponent::ResolveHit(const Vector3& from, const Vector3& to, float dt)
{
    m_touching = false;
    m_hitTimer = std::max(0.0f, m_hitTimer - dt);

    GameObject* player = scene.FindWithTag(playerTag);
    if (!player) return;

    // WHY 物理の掃引を使わないか: 判定したい相手は
    //     プレイヤー 1 体だけで、線分との距離を直接測る方が単純で速く、
    //     «見た目の線と同じ線» で測っていることがコードから読める。
    // 胴体中心で測る。プレイヤーの原点は足元にあるので、線を持ち上げた途端に
    // «胸を貫いているのに足元からは遠い» が起きる。
    Vector3 body = player->transform.worldPosition;
    body.y += std::max(playerCenterHeight, 0.0f);

    float along = 0.0f;
    const float distance = beamgeom::DistanceToSegment(body, from, to, along);

    // 点火しきる前は当たらない。針の段階で削られると «避けようがない» になる。
    const float reach = std::max(hitRadius, 0.0f) * Clamp01(m_charge);
    if (!beamgeom::Touches(distance, reach, playerRadius)) return;

    m_touching = true;
    if (m_hitTimer > 0.0f) return;
    m_hitTimer = std::max(tickInterval, 0.05f);

    auto* combat = CombatManagerComponent::Instance();
    if (!combat) {
        if (!m_warnedNoCombat) {
            m_warnedNoCombat = true;
            debug.LogError("BossBeamComponent found no CombatManagerComponent in the scene. "
                           "The core beam deals no damage.");
        }
        return;
    }
    (void)combat->DamagePlayer(player, std::max(damage, 0));
}

inline void BossBeamComponent::EnsureArcs(std::vector<ElectricArcBundle>& bundles,
                                          int count, const char* tag)
{
    const std::size_t wanted = static_cast<std::size_t>(std::max(count, 0));
    while (bundles.size() < wanted) {
        bundles.emplace_back();
        // 鍵は «持ち主 + 系統 + 番号»。同じ鍵の束が 2 つあると筋を奪い合い、
        // どちらも 1 本ぶんしか出なくなる。
        bundles.back().SetKey(LayerName(tag) + "_" + std::to_string(bundles.size() - 1));
    }
    // 減らされた枠は消灯だけして寝かせる。作り直すと GameObject 数が毎フレーム動く。
    for (std::size_t i = wanted; i < bundles.size(); ++i)
        bundles[i].Extinguish(*this);
}

inline ElectricArcStyle BossBeamComponent::ArcStyleBase(float brightness) const
{
    return beamlook::ArcStyle(LookOf(), brightness, arcStrands, arcWidth, arcRate,
                              arcIntensity);
}

inline void BossBeamComponent::DriveArcs(const Vector3& from, const Vector3& to, float dt)
{
    EnsureArcs(m_apertureArcs, apertureArcs, "ApArc");
    EnsureArcs(m_beamArcs,     beamArcs,     "BmArc");
    EnsureArcs(m_groundArcs,   groundArcs,   "GdArc");

    const float ignite = Clamp01(m_charge);
    const Vector3 delta  = to - from;
    const float   length = delta.Length();
    if (length <= EPSILON) { ExtinguishArcs(); return; }

    const Vector3 axis = delta / length;
    Vector3 side, up;
    beamlook::PerpendicularBasis(axis, side, up);

    // 回し続ける。止めると同じ形が明滅するだけの «静止した飾り» になる。
    m_arcSpin = std::fmod(m_arcSpin + dt * 1.7f, TWO_PI);

    // ── アパーチャ ──────────────────────────────────────────────────────────
    // WHY 点火中ほど強くするか: 8 章はすべての攻撃に予兆を求めている。ビーム本体は
    //     点火で細いまま出てくるので、来ることを伝えるのは «砲口が暴れている» 側の役。
    //     照射が始まったら本体が主役なので、逆に落とす。
    const float apertureGain = ignite * (1.35f - 0.75f * ignite);
    for (std::size_t i = 0; i < m_apertureArcs.size(); ++i) {
        const float phase = m_arcSpin * 1.9f + static_cast<float>(i) * TWO_PI
                          / static_cast<float>(std::max<std::size_t>(m_apertureArcs.size(), 1));
        const float reach = std::max(apertureArcReach, 0.05f);
        // 砲口から «斜め前» へ吹き出す。真横だと筒の中へ潜って見えない。
        const Vector3 tip = from
                          + (side * std::cos(phase) + up * std::sin(phase)) * reach
                          + axis * (reach * 0.45f);

        ElectricArcStyle style = ArcStyleBase(apertureGain);
        style.segments   = 18;
        style.amplitude  = reach * 0.55f;
        style.taperBias  = 0.55f;
        style.beadDensity = 3.0f;
        m_apertureArcs[i].Update(*this, from, tip, style, dt);
    }

    // ── 線に沿う ────────────────────────────────────────────────────────────
    // 筒は表面が硬い。輪郭を跨いで這う筋があると、«帯電した塊» として読めるようになる。
    for (std::size_t i = 0; i < m_beamArcs.size(); ++i) {
        const float phase = m_arcSpin + static_cast<float>(i) * TWO_PI
                          / static_cast<float>(std::max<std::size_t>(m_beamArcs.size(), 1));
        // 両端は筒に触れたまま、途中だけ外へ膨らませる。端を離すと «別の線» に見える。
        const Vector3 offset = (side * std::cos(phase) + up * std::sin(phase))
                             * (coreWidth * 0.5f);

        ElectricArcStyle style = ArcStyleBase(ignite);
        // 折れ点は長さで決める。20m を 24 点で折ると 1 区間 0.8m の «稲妻» になる。
        style.segments  = std::clamp(static_cast<int>(length * 2.5f), 16, 56);
        style.amplitude = std::max(arcBow, 0.0f) * ignite;
        // 接地側で暴れさせる。焼いている所がいちばん荒れている、という当たり前。
        style.taperBias = 0.72f;
        style.width     = std::max(arcWidth, 0.001f) * 0.85f;
        m_beamArcs[i].Update(*this, from + offset, to + offset, style, dt);
    }

    // ── 接地点 ──────────────────────────────────────────────────────────────
    // 焼いている面に沿って外へ逃がす。法線を使うので、床でも壁でも面へ寝る。
    Vector3 floorSide, floorUp;
    beamlook::PerpendicularBasis(m_contactNormal.NormalizedOr(Vector3::UP), floorSide, floorUp);
    for (std::size_t i = 0; i < m_groundArcs.size(); ++i) {
        const float phase = -m_arcSpin * 2.3f + static_cast<float>(i) * TWO_PI
                          / static_cast<float>(std::max<std::size_t>(m_groundArcs.size(), 1));
        // 長さを筋ごとに散らす。揃えると «車輪» に見えて放電に見えない。
        const float reach = std::max(groundArcReach, 0.05f)
                          * (0.55f + 0.45f * ArcHash01(static_cast<uint32_t>(i) * 7919u
                                                       + static_cast<uint32_t>(m_arcSpin * 3.0f)));
        const Vector3 tip = to + (floorSide * std::cos(phase) + floorUp * std::sin(phase)) * reach
                          + m_contactNormal.NormalizedOr(Vector3::UP) * 0.06f;

        ElectricArcStyle style = ArcStyleBase(ignite * 1.15f);
        style.segments  = 20;
        style.amplitude = reach * 0.42f;
        // 先端で暴れさせる。根元が暴れると «接地点がどこか» が読めなくなる。
        style.taperBias = 0.8f;
        m_groundArcs[i].Update(*this, to, tip, style, dt);
    }
}

inline void BossBeamComponent::ExtinguishArcs()
{
    for (ElectricArcBundle& arc : m_apertureArcs) arc.Extinguish(*this);
    for (ElectricArcBundle& arc : m_beamArcs)     arc.Extinguish(*this);
    for (ElectricArcBundle& arc : m_groundArcs)   arc.Extinguish(*this);
}

inline void BossBeamComponent::DriveEffects(float dt)
{
    if (scorchRate <= 0.0f) return;

    m_scorchTimer -= dt;
    if (m_scorchTimer > 0.0f) return;
    m_scorchTimer = 1.0f / std::max(scorchRate, 0.01f);

    auto* vfx = VfxManagerComponent::Instance();
    if (!vfx) return;

    // 焦げは当たった面の法線へ向けて置く。床を焼いている間は上向き、振り上げて
    // 壁へ移ったら壁の法線になるので、火花が面へ潜らない。薙いでいる間は終端が
    // 毎フレーム動くので、置いた点の列がそのまま «焼き払った跡» になる。
    vfx->PlayBeamScorch(m_contact, m_contactNormal, kBeamSide,
                        std::clamp(scorchSize * Clamp01(m_charge), 0.05f, 2.0f));
}

inline void BossBeamComponent::DriveFeedback(const Vector3& from, float dt)
{
    m_feedbackActive = true;

    const float ignite = Clamp01(m_charge);
    const GameObject* player = scene.FindWithTag(playerTag);

    // 予兆はアパーチャから、薙ぎは接地点から測る (Feedback グループの冒頭を参照)。
    const float chargeNear = shock::NearnessTo(player, from, std::max(chargeRange, 1.0f));
    const float burnNear   = shock::NearnessTo(player, m_contact,
                                                std::max(contactRange, 1.0f),
                                                std::max(contactNear, 0.0f));
    // 点火は低周波だけで返す。«近づいてくる質量» は重い唸りでしか表せない。
    float low  = Clamp01(chargeRumble) * ignite * chargeNear;
    float high = 0.0f;

    // 薙いでいるあいだの下地。接地点が寄るほど強くなるので、線が来ることが手で判る。
    const float sweeping = m_firing ? ignite : 0.0f;
    const float sweep    = Clamp01(sweepRumble) * sweeping * burnNear;
    low  = std::max(low,  sweep * kBossBeamSweepLowRatio);
    high = std::max(high, sweep);

    // 焼かれているあいだは距離を見ない。線の中に居ることは «近い» ではなく «当たっている»。
    if (m_touching) {
        low  = std::max(low,  Clamp01(burnRumble));
        high = std::max(high, Clamp01(burnRumble));
    }
    if (auto* pad = RumbleManagerComponent::Instance())
        pad->Sustain(RumbleChannel::BossBeam, low, high);

    // 点火しきった 1 瞬。唸りが最大まで上がったのに何も起きないと、予兆が
    // «ずっと鳴っている音» に化けて、いつ来たのかが判らない。ここで段を付ける。
    if (!m_ignited && m_firing && ignite >= 1.0f) {
        m_ignited = true;
        if (auto* shake = CameraShakeManagerComponent::Instance())
            shake->Shake(Clamp01(ignitionShake) * chargeNear);
    }
    if (!m_firing) m_ignited = false;

    // WHY 揺れだけ間隔を空けて継ぎ足すか: CameraShakeManager は寿命付きの要求を溜める
    //     器で、上限に達すると «最も弱いものを捨てる» が働く。毎フレーム積むと同じ
    //     1 回の照射が自分の揺れを押し出し合い、強さが本数の運で決まってしまう。
    //     振動が持続の器 (Sustain) を持つのに対して、こちらは持っていない。
    m_shakeTimer -= dt;
    if (sweep > 0.0f && m_shakeTimer <= 0.0f) {
        m_shakeTimer = std::max(shakeInterval, 0.02f);
        if (auto* shake = CameraShakeManagerComponent::Instance())
            shake->Shake(Clamp01(sweepShake) * sweeping * burnNear);
    }

    // WHY Distort ではなく掛けっぱなしの器か: 線に触れているのは «状態» であって
    //     当たった «瞬間» ではない。寿命付きの Distort を毎フレーム積むと、線から
    //     抜けた後も溜まったぶんが残り、避けきったのに画面が歪んだままになる。
    if (auto* screen = ScreenEffectManagerComponent::Instance())
        screen->SetSustainedDistortion(m_touching ? Clamp01(burnDistortion) : 0.0f);
}

inline void BossBeamComponent::StopFeedback()
{
    m_ignited    = false;
    m_shakeTimer = 0.0f;
    // 持っていないのに畳まない。掛けっぱなしの歪みは盤面で 1 つしか無いので、
    // 撃っていない間も 0 を書き続けると、他所が掛けた歪みを毎フレーム消してしまう。
    if (!m_feedbackActive) return;
    m_feedbackActive = false;

    if (auto* pad = RumbleManagerComponent::Instance())
        pad->StopSustain(RumbleChannel::BossBeam);
    if (auto* screen = ScreenEffectManagerComponent::Instance())
        screen->SetSustainedDistortion(0.0f);
}

inline void BossBeamComponent::DriveLight(bool lit)
{
    if (!contactLight) {
        // Inspector で切られた後も点きっぱなしにしない。
        if (GameObject* object = m_light.Resolve(scene))
            if (auto* light = object->GetComponent<LightComponent>())
                light->enabled = false;
        return;
    }

    GameObject* object = m_light.Resolve(scene);
    if (!object) {
        if (!lit) return;
        const std::string name = LayerName("Light");
        object = scene.Find(name);
        if (!object) {
            GameObject& created = scene.Create(name);
            created.runtimeGenerated = true;
            object = &created;
        }
        m_light = EntityRef{ object->GetID() };
    }

    auto* light = object->GetComponent<LightComponent>();
    if (!light) light = &object->AddComponent<LightComponent>();

    light->enabled = lit;
    if (!lit) return;

    // WHY worldPosition へ直接置くか: ルートに置いた GameObject なので local = world。
    //     親に付けるとボスの回転がそのまま光の位置に乗り、薙いだときに光だけが
    //     別の弧を描く (描画は worldPosition を見る)。
    object->transform.position = m_contact;
    object->transform.worldPosition = m_contact;

    const Vector4 hue = BeamHue();
    light->type      = LightComponent::Type::Point;
    light->color     = { hue.x, hue.y, hue.z };
    light->intensity = std::max(lightIntensity, 0.0f) * Clamp01(m_charge);
    light->range     = std::max(lightRange, 0.1f);
    light->castShadows = false;
}

inline void BossBeamComponent::OnLateUpdate()
{
    const float dt = std::max(Time::deltaTime, 0.0f);

    // 点火は撃っている間に太り、止めた後に細って消える。AI は撃つ / 止めるだけを言えばよい。
    const float rate = m_firing ? (chargeTime    > 0.0f ? dt / chargeTime    : 1.0f)
                                : (dischargeTime > 0.0f ? dt / dischargeTime : 1.0f);
    m_charge = Clamp01(m_charge + (m_firing ? rate : -rate));
    debugCharge = m_charge;

    if (m_charge <= 0.0f) {
        Hide();
        m_aimed = false;
        return;
    }

    // 狙いを更新されていないフレームは、最後に狙った点をそのまま使う。消灯中に
    // 端点が原点へ落ちると、消えかけの線が一瞬だけ盤面を横切る。
    const Vector3 from = AperturePoint();
    if (m_aimed) {
        // 狙点は «正面の床» を基準に、そこから持ち上げた点。射線はそこへ向けて伸ばす。
        Vector3 aimPoint = m_target;
        aimPoint.y += std::max(m_aimLift, 0.0f);
        m_contact = TraceContact(from, aimPoint, m_contactNormal);
    }
    if (!m_aimed && m_contact == Vector3::ZERO) { Hide(); return; }

    Show(from, m_contact);
    ResolveHit(from, m_contact, dt);
    DriveArcs(from, m_contact, dt);
    DriveLight(true);
    // 手触りは ResolveHit の後。線に触れているかは «今フレームの答え» で、
    // 焼かれている間だけ振り切る配分がそれを待っている。
    DriveFeedback(from, dt);
    if (m_firing) DriveEffects(dt);

    if (drawDebugHit)
        debug.DrawLine(from, m_contact, { 0.2f, 1.0f, 0.4f, 1.0f });
}

} // namespace sandbox
