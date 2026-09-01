/// @file    SerpentPolarityRigComponent.hpp
/// @brief   節に乗った極を輪郭で見せ、逆極の 2 節が揃ったら胴を折る。付ける先は Boss02
/// @author  Hasegawa Jin
/// @date    2026-08-31
///
/// WHY 発光帯ではなく輪郭で出すか (boss-serpent.md「極の表示」):
///   装甲が暗く、面積のわりに画面では細い。自発光を上げても «光っている» と気づく前に
///   極の色が飽和する。しかも `M_E_RingGlow` は «電気が通っている» を示す中立の琥珀で、
///   赤で灯しっぱなしにすると極を乗せていない節まで ＋ に見える。
///   形の外側へ出る輪郭なら、視界の端の節まで «何極か» を数えられる ─
///   選ぶのが «離れた 2 節» である以上、そこが読めなければ選択にならない。
///
/// WHY 節ごとに絶縁されているか:
///   雑魚の Serpent は導体で 1 発当てれば全節が帯電するが、ボスは伝わらない。
///   1 節ずつ灼して並べることそのものが手数になる。
///
/// WHY «一番離れた組» を採るか:
///   削れる長さは 2 節の間隔で決まる。近い組と遠い組が同時に成立しているとき、
///   プレイヤーが狙って作ったのは «遠い方» ─ 近い組は遠い組を作る途中で
///   たまたま並んだ副産物になる。近い方を先に消費すると、狙って作った盤面が
///   毎回横取りされる。
#pragma once

#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Scripts/Combat/BossPartPolarityComponent.hpp>
#include <Scripts/Combat/SerpentBodyComponent.hpp>
#include <Scripts/Combat/SerpentBones.hpp>
#include <Scripts/Combat/SerpentHitboxRigComponent.hpp>
#include <Scripts/Combat/SerpentSpineComponent.hpp>
#include <Scripts/Game/CameraShakeManagerComponent.hpp>
#include <Scripts/Game/CombatManagerComponent.hpp>
#include <Scripts/Game/RumbleManagerComponent.hpp>
#include <Scripts/Game/ScreenEffectManagerComponent.hpp>
#include <Scripts/Polarity/PolarityRingComponent.hpp>
#include <Scripts/Utils/PolarityTypes.hpp>
#include <Scripts/Utils/SeLibrary.hpp>
#include <algorithm>
#include <string>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

class SerpentPolarityRigComponent : public Script {
    FBZZ_SCRIPT(SerpentPolarityRigComponent)

public:
    FBZZ_GROUP("Fold")
    FBZZ_FIELD_RANGE(float, pullSeconds, 0.85f, "Windup", 0.1f, 5.0f)
    FBZZ_TOOLTIP("逆極の 2 節が揃ってから折れるまで。ここが «見せ場» の長さで、"
                 "短いと «斬った瞬間に勝手に折れた» になり、長いと待たされる")
    FBZZ_FIELD_RANGE(float, pullDecay, 2.5f, "Decay", 0.5f, 20.0f)
    FBZZ_TOOLTIP("対が崩れたとき溜めが戻る速さ。実時間の倍率")
    FBZZ_FIELD_RANGE_INT(int, minGap, 2, "Min Gap", 1, 27)
    FBZZ_TOOLTIP("対として成立する節の間隔。隣どうしでは輪が閉じない")

    FBZZ_GROUP("Head")
    FBZZ_FIELD_RANGE_INT(int, headDamage, 90, "Head Damage", 0, 2000)
    FBZZ_TOOLTIP("終盤に頭を斬ったときに通るダメージ。胴には通らない "
                 "(斬るのは極を乗せるため)")

    FBZZ_GROUP("Outline")
    FBZZ_FIELD(bool, outlineSegments, true, "Outline Segments")
    FBZZ_FIELD_RANGE(float, outlineWidth, 0.85f, "Width", 0.1f, 1.0f)
    FBZZ_FIELD_RANGE(float, outlinePullWidth, 1.00f, "Width (pulling)", 0.1f, 1.0f)
    // WHY 既定で遮蔽を無視するか: 胴は自分自身でとぐろを巻く。手前の節が奥の節を
    //     隠すのが常態なので、遮蔽で捨てるとマスクがほとんど残らない ─
    //     «どの節が何極か» は隠れていても読めなければ意味が無い。
    FBZZ_FIELD(bool, outlineThroughWalls, true, "Through Walls")

    FBZZ_GROUP("Feel")
    FBZZ_FIELD(bool, drawPullLink, true, "Draw Link")
    FBZZ_TOOLTIP("引き合っている 2 節を線で結ぶ。どこが対になったかの唯一の表示")
    FBZZ_FIELD_RANGE(float, ringIntervalFar, 0.30f, "Ring Interval (start)", 0.02f, 2.0f)
    FBZZ_FIELD_RANGE(float, ringIntervalNear, 0.07f, "Ring Interval (end)", 0.02f, 2.0f)
    FBZZ_FIELD_RANGE(float, ringRadius, 1.4f, "Ring Radius", 0.2f, 10.0f)

    FBZZ_GROUP("Debug")
    FBZZ_FIELD_READ_ONLY(int, debugCharged, 0, "Charged")
    FBZZ_FIELD_READ_ONLY(std::string, debugPair, "-", "Pair")
    FBZZ_FIELD_READ_ONLY(float, debugPull, 0.0f, "Pull")

    /// 引き合いの進み [0,1]。
    [[nodiscard]] float PullRatio() const { return Clamp01(m_pull / Max(pullSeconds, 0.01f)); }
    [[nodiscard]] bool  IsPulling() const { return m_pull > 0.0f; }
    /// 地上に出ていて極が乗っている節の数。仕込みがどれだけ進んだかそのもの。
    ///
    /// WHY AI がこれを読むか: ボスは極を塗り替えられないので、妨害は «届かなくする»
    ///     しかない (boss-serpent.md)。1 節だけ乗っている ＝ 対を組む途中なので、
    ///     そこが潜って逃げる判断の材料になる (SerpentAiComponent の Evade)。
    [[nodiscard]] int ChargedCount() const { return debugCharged; }

    void OnUpdate() override;

private:
    [[nodiscard]] SerpentBodyComponent*      Body()  const { return scene.GetScript<SerpentBodyComponent>(); }
    [[nodiscard]] SerpentSpineComponent*     Spine() const { return scene.GetScript<SerpentSpineComponent>(); }
    [[nodiscard]] SerpentHitboxRigComponent* Rig()   const { return scene.GetScript<SerpentHitboxRigComponent>(); }

    /// 極を乗せられない節に乗った極を落とす。床下へ潜った節がそのまま
    /// «盤面に居る» ことになると、見えていない所で対が成立する。
    void ClearUnreachable();
    /// 一番離れた逆極の組。見つからなければ false。
    [[nodiscard]] bool PickPair(int& a, int& b) const;
    void DriveOutline(bool pulling);
    void TickPullFeel(const Vector3& a, Polarity pa, const Vector3& b, Polarity pb, float dt);
    void Fire(int a, int b);
    /// 終盤だけ頭に斬撃を通す。
    void ResolveHeadHit();

    float m_pull      = 0.0f;
    float m_ringTimer = 0.0f;
};

FBZZ_REFLECT(SerpentPolarityRigComponent)

inline void SerpentPolarityRigComponent::ClearUnreachable()
{
    auto* rig   = Rig();
    auto* spine = Spine();
    auto* body  = Body();
    if (!rig) return;

    debugCharged = 0;
    for (int i = 1; i <= serpent::kSegmentCount; ++i) {
        auto* part = rig->SegmentPart(i);
        if (!part || !part->IsCharged()) continue;

        const bool reachable = (!body || body->IsAlive(i)) && (!spine || spine->IsExposed(i));
        if (!reachable) part->Clear();
        else            ++debugCharged;
    }
}

inline bool SerpentPolarityRigComponent::PickPair(int& a, int& b) const
{
    auto* rig   = Rig();
    auto* spine = Spine();
    auto* body  = Body();
    if (!rig) return false;

    int  bestGap = 0;
    bool found   = false;

    for (int i = 1; i <= serpent::kSegmentCount; ++i) {
        const auto* pi = rig->SegmentPart(i);
        if (!pi || !pi->IsCharged()) continue;
        if (body && !body->IsAlive(i)) continue;
        if (spine && !spine->IsExposed(i)) continue;

        for (int j = i + std::max(minGap, 1); j <= serpent::kSegmentCount; ++j) {
            const auto* pj = rig->SegmentPart(j);
            if (!pj || !pj->IsCharged()) continue;
            if (body && !body->IsAlive(j)) continue;
            if (spine && !spine->IsExposed(j)) continue;
            if (!IsAttracting(pi->Current(), pj->Current())) continue;

            const int gap = j - i;
            if (!found || gap > bestGap) {
                bestGap = gap;
                a       = i;
                b       = j;
                found   = true;
            }
        }
    }
    return found;
}

inline void SerpentPolarityRigComponent::DriveOutline(bool pulling)
{
    if (!outlineSegments) return;

    auto* rig   = Rig();
    auto* body  = Body();
    auto* spine = Spine();
    if (!rig || !body) return;

    bool any = false;
    for (int i = 1; i <= serpent::kSegmentCount; ++i) {
        const auto* part = rig->SegmentPart(i);
        if (!part || !part->IsCharged()) continue;
        if (!body->IsAlive(i)) continue;
        if (spine && !spine->IsExposed(i)) continue;

        // マスクの意味は読む側 (PolarityOutline.hlsl) との取り決め:
        // RGB = 極の色 / A = 太さ。PolarityTargetComponent と同じ約束で書く。
        Vector4 color = PolarityColor(part->Current());
        // 切れる直前は薄くする。«まだ乗っている» と «もう切れる» が同じ濃さだと、
        // もう 1 節を塗りに行くか諦めるかの判断ができない。
        color.w = FadeFromRemaining(part->RemainingNormalized());

        float width = Clamp01(outlineWidth);
        if (pulling) width = Lerp(width, Clamp01(outlinePullWidth), PullRatio());

        for (const EntityRef& ref : body->Meshes(i))
            if (GameObject* piece = ref.Resolve(scene)) {
                objectMask.Set(*piece, color, width, true, !outlineThroughWalls);
                any = true;
            }
    }

    // 申告はそのフレームだけ有効なので、出したいフレームは毎回パスも要求する。
    if (any)
        if (auto* screen = ScreenEffectManagerComponent::Instance()) screen->KeepOutline();
}

inline void SerpentPolarityRigComponent::TickPullFeel(const Vector3& a, Polarity pa,
                                                      const Vector3& b, Polarity pb, float dt)
{
    m_ringTimer -= dt;
    if (m_ringTimer > 0.0f) return;

    // 間隔が詰まっていくことで «来る» を出す。同じ間隔で刻むと、溜まっているのか
    // ただ乗っているだけなのかが音からも絵からも読めない。
    const float ratio = PullRatio();
    m_ringTimer = Lerp(Max(ringIntervalFar, 0.02f), Max(ringIntervalNear, 0.02f), ratio);

    if (auto* rings = PolarityRingComponent::Instance()) {
        rings->Burst(a, ringRadius, pa);
        rings->Burst(b, ringRadius, pb);
    }
    if (auto* pad = RumbleManagerComponent::Instance())
        pad->Rumble(0.10f + 0.35f * ratio, 0.05f + 0.20f * ratio, 0.06f);
    se::Play(audio, se::kAttractWindup, 0.35f + 0.65f * ratio);
}

inline void SerpentPolarityRigComponent::Fire(int a, int b)
{
    auto* body = Body();
    auto* rig  = Rig();
    if (!body || !rig) return;

    Vector3 midpoint = Vector3::ZERO;
    if (auto* spine = Spine())
        midpoint = (spine->JointPosition(a) + spine->JointPosition(b)) * 0.5f;

    // 使った極は落とす。残したままだと折れた直後に同じ対が再成立して、
    // プレイヤーが何もしていないのに 2 度目が始まる。
    if (auto* pa = rig->SegmentPart(a)) pa->Clear();
    if (auto* pb = rig->SegmentPart(b)) pb->Clear();
    m_pull      = 0.0f;
    m_ringTimer = 0.0f;

    const int crushed = body->TryFold(a, b);
    if (crushed <= 0) return;

    if (auto* rings = PolarityRingComponent::Instance())
        rings->Burst(midpoint, ringRadius * 3.0f, Polarity::None);
}

inline void SerpentPolarityRigComponent::ResolveHeadHit()
{
    auto* rig  = Rig();
    auto* body = Body();
    if (!rig || !body) return;

    auto* head = rig->SegmentPart(0);
    if (!head || !head->IsCharged()) return;

    // 極が乗ったこと自体が «斬った» の証拠になる。乗ったら必ず落とす ─
    // 残すと胴の対探しに頭が混ざり、頭と胴で輪が閉じてしまう。
    head->Clear();

    // 中盤まで頭は床下にいて触れない。«頭を斬りたい» という動機だけ先に置いて、
    // 通る瞬間を終盤に絞ると、頭が出っぱなしになったときの解放感が効く。
    if (!body->HeadIsVulnerable() || headDamage <= 0) return;

    if (auto* combat = CombatManagerComponent::Instance())
        if (GameObject* self = scene.Self())
            (void)combat->DamageEnemyDirect(self, headDamage);

    se::Play(audio, se::kImpactCoreHit);
    if (auto* shake = CameraShakeManagerComponent::Instance()) shake->Shake(0.35f);
}

inline void SerpentPolarityRigComponent::OnUpdate()
{
    const float dt = Max(Time::deltaTime, 0.0f);

    ClearUnreachable();
    ResolveHeadHit();

    int a = 0;
    int b = 0;
    const bool paired = PickPair(a, b);

    DriveOutline(paired);

    if (paired) {
        m_pull += dt;

        Vector3 pa = Vector3::ZERO;
        Vector3 pb = Vector3::ZERO;
        if (auto* spine = Spine()) {
            pa = spine->JointPosition(a);
            pb = spine->JointPosition(b);
        }
        Polarity qa = Polarity::None;
        Polarity qb = Polarity::None;
        if (auto* rig = Rig()) {
            if (const auto* part = rig->SegmentPart(a)) qa = part->Current();
            if (const auto* part = rig->SegmentPart(b)) qb = part->Current();
        }
        TickPullFeel(pa, qa, pb, qb, dt);

        if (drawPullLink) {
            // 溜まるほど白へ寄せる。どちらの極かは節の輪郭が言うので、
            // 線は «どれだけ張り詰めたか» だけを担当する。
            const float   ratio = PullRatio();
            const Vector4 color{ Lerp(0.55f, 1.0f, ratio), Lerp(0.55f, 1.0f, ratio),
                                 Lerp(0.62f, 1.0f, ratio), 1.0f };
            debug.DrawLine(pa, pb, color);
        }
        debugPair = serpent::PartSuffix(a) + " / " + serpent::PartSuffix(b) + " (-" +
                    std::to_string(b - a - 1) + ")";
    } else {
        m_pull      = Max(0.0f, m_pull - dt * Max(pullDecay, 0.0f));
        m_ringTimer = 0.0f;
        debugPair   = "-";
    }

    debugPull = PullRatio();

    if (paired && m_pull >= Max(pullSeconds, 0.01f)) Fire(a, b);
}

} // namespace sandbox
