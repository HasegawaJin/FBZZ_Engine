/// @file    PlayerPolarityComponent.hpp
/// @brief   プレイヤー自身の極。剣を振っている間だけ帯び、触れた敵を弾く
/// @author  Hasegawa Jin
/// @date    2026-08-28
///
/// WHY プレイヤーに極を持たせるか:
///   旧設計ではプレイヤーが極性を持たず、引力は敵同士にしか発生しなかった。その結果、
///   立ち位置が意味を持つのは «敵に視線が通るか» だけで、移動の用途が «視線の確保» と
///   «回避» の 2 つしか無かった。TPS を採用しているのに、攻撃行動が照準と 2 つのボタンで
///   閉じていた。極を纏えるようにすると、立ち位置と移動がそのまま戦術になる。
///
/// WHY 常時ではなく «剣を振っている間» か:
///   常に帯びていると、立っているだけで盤面の一部になる。逆に帯びる手段を
///   回避だけにすると (旧設計)、攻めと移動が別の入力になり、攻めている最中は足が止まる。
///   振っている間に限れば、手を止めた瞬間に盤面から降りられる。
///
/// WHY 入口をこのスクリプトが持たないか:
///   «いつ帯びるか» を決めるのは剣 (PolarityBladeComponent) で、こちらは
///   «帯びている間どうなるか» だけを持つ。入口をここへ書くと、斬撃の連撃状態を
///   このスクリプトが知る必要が出て、纏いの話に攻撃の状態機械が混ざる。
///
/// WHY 纏いがプレイヤーの足を動かさないか:
///   極を纏うのは «斬った» ことの副産物で、プレイヤーが移動のつもりで押した入力ではない。
///   そこへ盤面の引力・斥力を掛けると、斬るたびに体が勝手に運ばれ、立ち位置という
///   最も基本的な判断が手から離れる。動かすのは触れた相手だけにして、
///   自分の足は最後まで移動入力だけが持つ。
///
/// WHY PolarityTargetComponent を付けて盤面へ参加させないか:
///   盤面 (PolarityFieldComponent) は «どちらが飛ぶか» を決めて相手の速度を直接書く。
///   プレイヤーは CharacterController で動いていて、速度を横から書かれると接地判定も
///   坂の処理も壊れる。加えて «引かれている間 AI は停止する» という前提は、操作している
///   相手には当てはまらない。
///
/// WHY プレイヤーの体当たりにダメージが無いか:
///   「銃は敵を倒さない。倒すのは衝突である」。プレイヤーの手が直接倒せるようになると、
///   極性で盤面を組む理由が消える。当たった敵は吹き飛ぶだけで、倒れはしない。
#pragma once

#include <Engine/Scene/EntityRef.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Scripts/Data/PolarityTuning.hpp>
#include <Scripts/Game/RumbleManagerComponent.hpp>
#include <Scripts/Polarity/PolarityBodyComponent.hpp>
#include <Scripts/Polarity/PolarityRingComponent.hpp>
#include <Scripts/Polarity/PolarityTargetComponent.hpp>
#include <Scripts/Utils/GlowPartComponent.hpp>
#include <Scripts/Utils/PolarityTypes.hpp>
#include <Scripts/Utils/SeLibrary.hpp>
#include <cmath>
#include <vector>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

class PlayerPolarityComponent : public Script {
    FBZZ_SCRIPT(PlayerPolarityComponent)

public:
    // PlayerComponent が注入する。数値を Inspector 側へ複製しない。
    fbzz::Asset<PolarityTuning> tuning{};

    FBZZ_GROUP("Bump")
    // 纏っている間に触れた敵を吹き飛ばす距離。ダメージは入らない。
    //
    // WHY 衝突イベントで取らないか: PlayerComponent は内部モジュールへ
    //     OnCollisionEnter を中継していない (OnStart / OnUpdate / OnLateUpdate /
    //     OnFixedUpdate / OnDestroy だけ)。距離で取れば中継の有無に依存しない。
    FBZZ_FIELD_RANGE(float, bumpRadius, 1.8f, "Bump Radius", 0.0f, 6.0f)
    FBZZ_TOOLTIP("この距離まで近づいた帯電中の敵を弾き飛ばす。0 で体当たりを切る")

    FBZZ_GROUP("Feedback")
    FBZZ_FIELD_RANGE(float, chargeRumble, 0.55f, "Charge Rumble", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, glowIntensity, 0.9f, "Glow Intensity", 0.0f, 4.0f)
    FBZZ_TOOLTIP("纏っている間、プレイヤーの発光パーツをこの強さで極の色に置き換える。"
                 "緑 (無極) より少し強くして «今は自分も盤面のコマ» を出す")

    FBZZ_GROUP("Debug")
    FBZZ_FIELD_READ_ONLY(float, debugRemaining, 0.0f, "Charge Remaining")
    FBZZ_FIELD_READ_ONLY(std::string, debugPolarity, "None", "Polarity")

    // ── 参照する側の問い合わせ ───────────────────────────────────────────────
    [[nodiscard]] Polarity Current() const { return m_polarity; }
    [[nodiscard]] bool IsCharged() const { return m_polarity != Polarity::None; }
    /// この極を今 «自分に» 使っているか。銃はこれが true の側を撃てない。
    [[nodiscard]] bool IsChargedWith(Polarity polarity) const
    {
        return polarity != Polarity::None && m_polarity == polarity;
    }
    /// 纏いの残り [0,1]。HUD と剣の発光が読む。
    [[nodiscard]] float ChargeRatio() const;

    /// 極を纏う。剣を振った側 (PolarityBladeComponent) が呼ぶ唯一の入口。
    ///
    /// WHY 上書きを許すか: 連撃で左右を振り分けると、纏っている極が振るたびに
    ///     入れ替わる。«既に纏っているから無視» にすると、左右を斬り分けた結果が
    ///     絵にも音にも出なくなる。
    void Charge(Polarity polarity, float seconds);

    void OnStart()  override;
    void OnUpdate() override;

private:
    void EndCharge();
    /// 触れている敵を弾く。ダメージは入らない。
    void BumpNearby();
    /// 纏っている極の色を、自分の発光パーツと輪郭へ流す。
    void DriveVisual();
    /// プレイヤーの下にある発光パーツを 1 度だけ集める。
    void CacheGlowParts();

    Polarity m_polarity  = Polarity::None;
    float    m_remaining = 0.0f;
    /// 今の纏いの «長さ»。残りを割って比を出すのに要る (呼んだ側が決める値)。
    float    m_total     = 0.0f;

    // 発光パーツ。毎フレーム探すと、盤面の全 GlowPartComponent を走査することになる。
    std::vector<EntityRef> m_glowParts;
};

FBZZ_REFLECT(PlayerPolarityComponent)

// ── 実装 (inline) ─────────────────────────────────────────────────────────────

inline void PlayerPolarityComponent::OnStart()
{
    if (!tuning) {
        debug.LogError("PlayerPolarityComponent requires PolarityTuning.fzdata "
                       "(PlayerComponent injects it).");
        enabled = false;
        return;
    }

    m_polarity  = Polarity::None;
    m_remaining = 0.0f;
    m_total     = 0.0f;
    // 纏った瞬間の音はプレイヤー本人の位置で鳴る。減衰を掛ける相手が自分自身なので 2D。
    se::EnsureSource(scene);
    CacheGlowParts();
}

inline void PlayerPolarityComponent::CacheGlowParts()
{
    m_glowParts.clear();
    GameObject* self = scene.Self();
    if (!self) return;

    // WHY 祖先をたどって選り分けるか: FindObjectsOfType は盤面全体を返す。名前で
    //     探す手もあるが、発光パーツの名前はモデル側の都合で変わる。«自分の下に
    //     ぶら下がっているか» はモデルを差し替えても変わらない条件になる。
    for (GameObject* object : scene.FindObjectsOfType<GlowPartComponent>()) {
        if (!object) continue;
        for (GameObject* node = object; node; node = node->GetParent()) {
            if (node != self) continue;
            m_glowParts.push_back(EntityRef{ object->GetID() });
            break;
        }
    }
}

inline float PlayerPolarityComponent::ChargeRatio() const
{
    // WHY 長さを覚えておくか: 纏う時間を決めるのは呼んだ側 (剣は bladeChargeSeconds)。
    //     ここで調整値を引き直すと、剣が 0.9 秒で纏わせたのに 1.2 秒を基準に割ることになり、
    //     ゲージが満タンまで伸びない。
    if (m_total <= 0.0f) return 0.0f;
    return Clamp01(m_remaining / m_total);
}

inline void PlayerPolarityComponent::OnUpdate()
{
    const float dt = Max(Time::deltaTime, 0.0f);

    if (m_polarity != Polarity::None) {
        m_remaining -= dt;
        if (m_remaining <= 0.0f) EndCharge();
    }

    debugRemaining = Max(m_remaining, 0.0f);
    debugPolarity  = m_polarity == Polarity::None
        ? "None" : (m_polarity == Polarity::Plus ? "Plus" : "Minus");

    if (m_polarity == Polarity::None) return;

    BumpNearby();
    DriveVisual();
}

inline void PlayerPolarityComponent::Charge(Polarity polarity, float seconds)
{
    if (polarity == Polarity::None || seconds <= 0.0f) return;

    // WHY 極が «変わったとき» だけ画面へ返すか: 連撃で同じ剣を続けて振ると
    //     毎回ここへ来る。そのたびに画面の縁を走らせると、振っている間じゅう
    //     縁が光り続けて «極が切り替わった» という出来事が読めなくなる。
    const bool switched = (m_polarity != polarity);

    m_polarity  = polarity;
    m_remaining = seconds;
    m_total     = seconds;

    if (!switched) return;

    // «この個体が帯びた» の音。敵に極が乗るのとまったく同じ事象なので同じ音を使う
    // (相手が自分になっただけ)。
    se::Play(audio, se::kPolarityInfect);

    if (auto* pad = RumbleManagerComponent::Instance())
        pad->Rumble(chargeRumble);
}

inline void PlayerPolarityComponent::EndCharge()
{
    m_polarity  = Polarity::None;
    m_remaining = 0.0f;
    m_total     = 0.0f;
}

inline void PlayerPolarityComponent::BumpNearby()
{
    if (bumpRadius <= 0.0f) return;

    const Vector3 self     = transform.worldPosition;
    const float   radiusSq = bumpRadius * bumpRadius;
    const float   speed    = Max(tuning->playerBumpSpeed, 0.0f);
    if (speed <= 0.0f) return;

    for (GameObject* object : scene.FindObjectsOfType<PolarityTargetComponent>()) {
        if (!object || !object->activeInHierarchy()) continue;
        auto* target = scene.GetScript<PolarityTargetComponent>(object);
        if (!target || !target->IsCharged()) continue;
        // 弾かれた直後の相手を続けて弾かない。盤面の反発と同じ間隔を共有する。
        if (!target->CanBeRepulsed()) continue;

        Vector3 delta = object->transform.worldPosition - self;
        delta.y = 0.0f;
        const float distanceSq = delta.LengthSq();
        if (distanceSq > radiusSq || distanceSq < EPSILON) continue;

        const Vector3 direction = delta / std::sqrt(distanceSq);

        // 弾かれ方を本人が持っている相手 (Roller は転がり出す) はそちらへ渡す。
        if (target->onRepulse) {
            target->onRepulse(direction, speed);
        } else {
            auto* body = scene.GetScript<PolarityBodyComponent>(object);
            if (!body || target->isAnchor) continue;   // 動かない相手は押せない
            if (!body->IsAvailableForRepulse()) continue;
            body->ApplyRepulse(direction, speed, tuning->repulseLift, 1.0f);
        }
        target->NotifyRepulsed();

        // 盤面の反発と同じ絵と音にする。«自分が弾いた» と «盤面が弾いた» で
        // 見え方が違うと、同じ規則が働いていることが伝わらない。
        //
        // WHY 環を反発半径ではなく体当たりの距離で出すか: 環は «届いた範囲» の表示。
        //     6m の環を出すと、実際には 1.8m しか届いていないのに «押しのけた範囲»
        //     を嘘で広げることになる。
        if (auto* rings = PolarityRingComponent::Instance())
            rings->Burst(self + delta * 0.5f, bumpRadius, m_polarity);
        // 体当たりは必ず自分のすぐ横で起きる。定位させる意味が無いので 2D のまま。
        se::Play(audio, se::kPolarityRepulse);
    }
}

inline void PlayerPolarityComponent::DriveVisual()
{
    const Vector4 color = PolarityColor(m_polarity);

    // 纏っている間だけ、緑をその極の色へ置き換える。1.2 秒しか続かないので、
    // «プレイヤー = 緑» という配色の約束は壊れない。
    for (const EntityRef& ref : m_glowParts) {
        GameObject* object = ref.Resolve(scene);
        if (!object) continue;
        if (auto* glow = scene.GetScript<GlowPartComponent>(object))
            glow->RequestColor(color, glowIntensity);
    }

    // WHY 自分には輪郭を掛けないか:
    //   輪郭は «視界の端に居るものの極を数える» ための記号で、プレイヤーは常に
    //   画面の中央に居るので端で数える必要がない。それどころか、TPS では自機が
    //   画面で一番大きく映るぶん、輪郭を掛けると盤面のどれよりも目立ってしまい、
    //   «どの脚が何極か» を読ませたい相手の輪郭が沈む。纏いは発光パーツの色と
    //   画面の縁のサージが受け持つ。
}

} // namespace sandbox
