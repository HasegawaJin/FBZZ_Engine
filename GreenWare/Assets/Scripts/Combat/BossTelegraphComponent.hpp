/// @file    BossTelegraphComponent.hpp
/// @brief   BossAiComponent が出す予兆を、地面のデカール 1 枚として描く
/// @author  Hasegawa Jin
/// @date    2026-08-30
///
/// WHY 板ではなくデカールか:
///   板を 1 枚置くと、床の起伏・瓦礫・ボスの足元でめり込んで切れる。デカールは
///   深度から受け面を復元して投影するので、範囲が地形に沿って «敷かれた» 形になる
///   (手続きデカールと同じ理由)。
///
/// WHY 1 枚だけ持つか:
///   ボスは 1 度に 1 つの行動しか出さない (Act は排他)。枚数を増やしても同時に
///   2 つ出る経路が無く、使われない枠の «消し忘れ» を疑う手間だけが増える。
///
/// WHY AI に形を作らせて、こちらは描くだけにするか:
///   着弾点も射程も攻撃の進行が持つ値。ここで同じ計算を持つと、AI の数値を触るたびに
///   «予兆だけ古い場所に出る» が起きる (BossTelegraph.hpp の WHY)。
#pragma once

#include <Engine/Scene/Components/DecalComponent.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Scripts/Combat/BossAiComponent.hpp>
#include <Scripts/Combat/BossTelegraph.hpp>
#include <Scripts/Combat/DangerWallComponent.hpp>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

class BossTelegraphComponent : public Script {
    FBZZ_SCRIPT(BossTelegraphComponent)

public:
    FBZZ_GROUP("Decal")
    FBZZ_FIELD_FILE(telegraphMaterial, "Assets/Materials/Decal/DecalBossTelegraph.mat",
                    "Material", ".mat")
    FBZZ_FIELD_RANGE(float, projectionDepth, 6.0f, "奥行き", 0.2f, 30.0f)
    FBZZ_TOOLTIP("投影の厚み [m]。薄いと坂で切れ、厚いと段差の裏にも回り込む")

    FBZZ_GROUP("手触り")
    // WHY 出だしを薄くするか: 予兆が最初から満濃度で出ると «もう来た» に見える。
    //     立ち上がりに一拍あると «来る» と «来た» が分かれる。
    FBZZ_FIELD_RANGE(float, fadeInSeconds, 0.12f, "フェードイン", 0.0f, 1.0f)
    // 拍と回避窓。
    //
    // WHY 明滅 (Blink Hz) を置き換えたか (2026-09-07): 旧実装は
    //     `sin(Time::time * hz)` の絶対時刻で、予兆が出た瞬間の位相が毎回違った。
    //     «何回光ったら来る» が成立しないので «そろそろ» としか言えず、しかも
    //     既定が 0.0 だったのでボス 1 では明滅そのものが出ていなかった。
    FBZZ_FIELD_RANGE_INT(int, pips, 3, "ピップの数", 0, 8)
    FBZZ_TOOLTIP("枠に刻む拍の数。明るい弧が目盛りを 1 つ越えるたびに 1 拍光る。"
                 "進みを等分するので、予兆の尺が手ごとに違っても拍の数は変わらない")
    FBZZ_FIELD_RANGE(float, strikeFrom, 0.82f, "打撃の起点", 0.0f, 1.0f)
    FBZZ_TOOLTIP("回避窓の入口。ここから枠が白へ寄って太り、斜線の流れが止まる。"
                 "«量» ではなく «質» が変わることで «今» が読める")
    FBZZ_FIELD_RANGE(float, pipGain, 2.1f, "ピップの明滅", 1.0f, 4.0f)
    FBZZ_TOOLTIP("拍の瞬間の明るさの跳ね。1.0 で跳ねなし")
    FBZZ_FIELD_RANGE(float, burstSeconds, 0.12f, "バースト", 0.0f, 0.5f)
    FBZZ_TOOLTIP("着弾の瞬間に枠が太って薄れる尺 [秒]。0 で即座に消える ─ "
                 "«避けきったのか当たったのか» が絵に残らなくなる")

    // WHY 模様を流すか: 止まった斜線は «床に描いてある柄» と区別が付かない。
    //     流れていると «今それが起きつつある» になり、帯では «どちらから来るか»
    //     まで同じ模様が言う。デカールの cbuffer に時刻が無いので、位相は
    //     こちらが毎フレーム進めて渡す (手続きデカールの spin と同じ形)。
    FBZZ_FIELD_RANGE(float, stripeScrollHz, 0.45f, "縞のスクロール", -4.0f, 4.0f)
    FBZZ_TOOLTIP("斜線と矢羽根が流れる速さ [周/秒]。帯では正で «ボスから前へ» 流れる。"
                 "0 で止まる")

    FBZZ_GROUP("デバッグ")
    FBZZ_FIELD_READ_ONLY(std::string, debugShape, "None", "Shape")
    FBZZ_FIELD_READ_ONLY(float, debugProgress, 0.0f, "Progress")

    void OnStart() override;
    void OnLateUpdate() override;
    void OnDestroy() override;

private:
    [[nodiscard]] std::string DecalName(int index) const;
    /// index 枚目のデカールを拾い直す。無ければ作る。
    [[nodiscard]] GameObject* EnsureDecal(int index);
    void Hide(GameObject& object);
    void Place(GameObject& object, const BossTelegraph& telegraph);

    /// 出している枚数ぶん。コアビームは «撃つ線» の本数だけ帯が要る。
    ///
    /// WHY 1 枚を使い回さないか: 帯は DecalComponent の transform そのものなので、
    ///     1 フレームに 2 本描くことができない。本数ぶん持って、余ったぶんを
    ///     伏せるのが «出す側が本数を決める» に対する素直な受け方。
    std::vector<EntityRef> m_decals;
    /// 予兆が出てからの秒数。立ち上がりのフェードに使う。
    float m_shownFor = 0.0f;
    /// 拍・回避窓・着弾の弾け。蛇と部位発光も同じ型を通る (BossTelegraph.hpp)。
    BossTelegraphCue m_cue;
};

FBZZ_REFLECT(BossTelegraphComponent)

inline std::string BossTelegraphComponent::DecalName(int index) const
{
    GameObject* owner = scene.Self();
    std::string name = "BossTelegraph_" + (owner ? owner->instanceId : std::string("orphan"));
    // 1 枚目だけは名前を変えない。既にシーンへ残っている帯を拾い直せなくなると、
    // DLL リロードのたびに古い 1 枚が伏せられないまま床に残る。
    if (index > 0) name += "_" + std::to_string(index);
    return name;
}

inline GameObject* BossTelegraphComponent::EnsureDecal(int index)
{
    if (index < 0) return nullptr;
    if (m_decals.size() <= static_cast<std::size_t>(index))
        m_decals.resize(static_cast<std::size_t>(index) + 1);

    // WHY 先に拾い直すか: スクリプト DLL をリロードすると Script は作り直され
    //     EntityRef は空に戻るが、デカールの GameObject は Scene に残る。
    //     無条件に作るとリロードのたびに 1 枚ずつ増えていく。
    EntityRef& slot = m_decals[static_cast<std::size_t>(index)];
    if (GameObject* existing = slot.Resolve(scene)) return existing;

    GameObject* object = scene.Find(DecalName(index));
    if (!object) {
        // WHY ボスの子にしないか: 子にすると位置と «向き» がボスの回転を引き継ぐ。
        //     予兆はワールドの形なので、ボスが旋回しただけで帯が振り回される。
        GameObject& created = scene.Create(DecalName(index));
        created.runtimeGenerated = true;
        object = &created;
    }

    DecalComponent* decal = object->GetComponent<DecalComponent>();
    if (!decal) decal = &object->AddComponent<DecalComponent>();
    if (!telegraphMaterial.empty()) decal->materialPath = telegraphMaterial;

    slot = EntityRef{ object->GetID() };
    return object;
}

inline void BossTelegraphComponent::Hide(GameObject& object)
{
    object.SetActive(false);
}

inline void BossTelegraphComponent::Place(GameObject& object, const BossTelegraph& telegraph)
{
    auto* decal = object.GetComponent<DecalComponent>();
    if (!decal) return;

    const bool  line     = telegraph.shape == BossTelegraphShape::Line;
    const float depth    = std::max(projectionDepth, 0.2f);
    const float progress = Clamp01(telegraph.progress);

    // デカールの投影軸はローカル +Y。
    //
    // 円 … 直径 × 直径の箱。回転は要らない
    // 帯 … 幅 (半幅の 2 倍) × 長さの箱。始点ではなく «中点» へ置く
    if (line) {
        const Vector3 dir = telegraph.direction.NormalizedOr(Vector3::FORWARD);
        const Vector3 mid = telegraph.origin + dir * (telegraph.length * 0.5f);
        object.transform.position      = mid;
        object.transform.worldPosition = mid;
        // +Z を進行方向へ向ける。投影軸 (+Y) は起こしたまま。
        object.transform.rotation =
            Quaternion::FromAxisAngle(Vector3::UP, std::atan2(dir.x, dir.z));
        object.transform.scale = { std::max(telegraph.radius, 0.05f) * 2.0f,
                                   depth,
                                   std::max(telegraph.length, 0.1f) };
    } else {
        object.transform.position      = telegraph.origin;
        object.transform.worldPosition = telegraph.origin;
        object.transform.rotation      = Quaternion::Identity();
        const float diameter = std::max(telegraph.radius, 0.05f) * 2.0f;
        object.transform.scale = { diameter, depth, diameter };
    }

    // 立ち上がりの薄さ。予兆が最初から満濃度で出ると «もう来た» に見える。
    // 拍・回避窓・着弾の弾けは共有の BossTelegraphCue が持つ。
    const float fadeIn = fadeInSeconds > 0.0f ? Clamp01(m_shownFor / fadeInSeconds) : 1.0f;

    decal->materialParamOverrides["shape"]    = { line ? 1.0f : 0.0f };
    decal->materialParamOverrides["progress"] = { progress };
    // «上から来る» は落下リングで言う。形 (円 / 帯) と直交する軸なので、
    // 攻撃の種類から引く (BossTelegraph.hpp の BossThreatOriginOf)。
    decal->materialParamOverrides["threatAbove"] =
        { BossThreatOriginOf(telegraph.kind) == BossThreatOrigin::Above ? 1.0f : 0.0f };
    decal->materialParamOverrides["pulse"]    = { std::max(m_cue.pulse * fadeIn, 0.0f) };
    // 位相は cue が進める。回避窓へ入ると止まる ─ 動いていたものが止まるのは
    // «構え終わった» の合図で、明るさの変化より視界の端でも拾いやすい。
    decal->materialParamOverrides["stripeScroll"] = { m_cue.scroll };
    // 突進は帯に沿って «走ってくる» 手。踏みつけ・着地・パルスは円、ビームは
    // 撃った瞬間に線が通るので、走る絵になるのは突進だけ。
    decal->materialParamOverrides["travel"] =
        { (line && telegraph.kind == BossAttackKind::Charge) ? 1.0f : 0.0f };
    decal->materialParamOverrides["strikeWindow"] = { Clamp01(strikeFrom) };
    decal->materialParamOverrides["countPips"]    = { static_cast<float>(std::max(pips, 0)) };
    decal->materialParamOverrides["burstFade"]    = { m_cue.burstFade };
}

inline void BossTelegraphComponent::OnStart()
{
    if (!scene.GetScript<BossAiComponent>()) {
        debug.LogError("BossTelegraphComponent requires BossAiComponent on the same object "
                       "(the attack progress is what decides the telegraph).");
        enabled = false;
        return;
    }

    if (GameObject* object = EnsureDecal(0)) Hide(*object);
}

inline void BossTelegraphComponent::OnLateUpdate()
{
    const auto* ai = scene.GetScript<BossAiComponent>();
    if (!ai) return;

    const BossTelegraph&              primary = ai->CurrentTelegraph();
    const std::vector<BossTelegraph>& extras  = ai->ExtraTelegraphs();
    const bool shown = primary.shape != BossTelegraphShape::None;

    // 出ていない間は «出てからの秒数» を進めない。1 枚目の状態で代表させるのは、
    // 全部が同じ予兆の一部で、同時に出て同時に消えるため。
    if (!shown) m_shownFor = 0.0f;
    else        m_shownFor += std::max(Time::deltaTime, 0.0f);

    // 時刻の言葉は共有の 1 つを通す。枚数が変わっても «同じ予兆の一部» なので、
    // 拍も回避窓も 1 つで足りる ─ 枚ごとに持つと同じ攻撃の中で拍がずれる。
    m_cue.pips         = std::max(pips, 0);
    m_cue.pipGain      = pipGain;
    m_cue.strikeFrom   = Clamp01(strikeFrom);
    m_cue.scrollHz     = stripeScrollHz;
    m_cue.burstSeconds = std::max(burstSeconds, 0.0f);
    m_cue.Tick(shown ? primary.progress : 1.0f, std::max(Time::deltaTime, 0.0f), shown);

    // 着弾の «弾け» が残っている間は、消えた予兆でも 1 コマ描き続ける。
    // 即座に消すと «避けきったのか当たったのか» が絵に残らない。
    if (!shown && m_cue.visible) {
        for (std::size_t i = 0; i < m_decals.size(); ++i)
            if (GameObject* object = EnsureDecal(static_cast<int>(i)))
                if (object->activeSelf())
                    if (auto* decal = object->GetComponent<DecalComponent>()) {
                        decal->materialParamOverrides["burstFade"] = { m_cue.burstFade };
                        decal->materialParamOverrides["progress"]  = { 1.0f };
                    }
        debugShape    = "Burst";
        debugProgress = 1.0f;
        return;
    }

    // 1 枚目 + 続き。余った枚数は伏せる (本数は攻撃ごとに変わる)。
    const std::size_t want = shown ? extras.size() + 1 : 0;
    const std::size_t have = std::max(m_decals.size(), want);
    for (std::size_t i = 0; i < have; ++i) {
        GameObject* object = EnsureDecal(static_cast<int>(i));
        if (!object) continue;

        if (i >= want) {
            if (object->activeSelf()) Hide(*object);
            continue;
        }
        if (!object->activeSelf()) object->SetActive(true);
        Place(*object, i == 0 ? primary : extras[i - 1]);
    }

    debugShape    = shown ? (primary.shape == BossTelegraphShape::Line ? "Line" : "Circle")
                          : "None";
    debugProgress = shown ? Clamp01(primary.progress) : 0.0f;

    // 帯の予兆は «立てて» も見せる。ボスの体で床が隠れる距離ほど、
    // 通り道の高さが読めないと避けようがない (DangerWallComponent の WHY)。
    //
    // WHY 1 枚目だけ渡すか: 壁は 1 枚しか立たない (Submit は最後の 1 つが勝つ)。
    //     全部渡すと «最後に渡した線» の壁だけが立ち、どれが立っているのかが
    //     出す側から見えなくなる。狙われている中心の線を代表させる。
    if (auto* wall = scene.GetScript<DangerWallComponent>())
        if (shown) wall->Submit(primary);
}

inline void BossTelegraphComponent::OnDestroy()
{
    // ルートに置いた以上、ボスと一緒には消えない。持ち主が畳む。
    for (EntityRef& ref : m_decals)
        if (GameObject* object = ref.Resolve(scene)) scene.Destroy(*object);
    m_decals.clear();
}

} // namespace sandbox
