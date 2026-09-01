/// @file    SerpentBodyComponent.hpp
/// @brief   胴の長さと «折る» 操作。逆極の 2 節で輪を閉じ、間を潰して繋ぎ直す
/// @author  Hasegawa Jin
/// @date    2026-08-31
///
/// WHY 千切って後ろを落とさないか (boss-serpent.md「胴を折る」):
///   3 節目と 7 節目を組んだだけで 8〜16 節が丸ごと消えてしまい、1 回で大半が飛ぶ。
///   輪を潰す形にすると «2 節の距離がそのまま削れる長さ» になり、
///   刻むか一気に行くかの判断が毎回生まれる。
///
/// WHY 骨を減らさず «リンクを畳む» 形にするか:
///   骨の親子は書き出しで固定されていて、途中の 1 本を抜くことはできない。
///   代わりに潰れた節のリンク長を 0 にすると、残った節が隙間なく繋がったまま
///   鎖が短くなる ─ «両端が溶接されて胴は繋がったまま短くなる» が、
///   位置の計算を 1 行も足さずにそのまま出る (畳むのは SerpentSpineComponent)。
///
/// WHY 段階をフェーズ変数で持たないか:
///   長さがそのまま段階になる (28〜23 / 22〜15 / 14 以下)。別に変数を持つと、
///   «節は 14 なのにフェーズは 1» という、盤面と食い違った状態が作れてしまう。
#pragma once

#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Scripts/Combat/EnemyHealthComponent.hpp>
#include <Scripts/Combat/SerpentBones.hpp>
#include <Scripts/Combat/SerpentHitboxRigComponent.hpp>
#include <Scripts/Game/CameraShakeManagerComponent.hpp>
#include <Scripts/Game/CombatManagerComponent.hpp>
#include <Scripts/Game/HitstopManagerComponent.hpp>
#include <Scripts/Game/RumbleManagerComponent.hpp>
#include <Scripts/Game/VfxManagerComponent.hpp>
#include <Scripts/Utils/SeLibrary.hpp>
#include <algorithm>
#include <string>
#include <vector>

using namespace fbzz::scene;
using namespace fbzz::math;

namespace sandbox {

class SerpentBodyComponent : public Script {
    FBZZ_SCRIPT(SerpentBodyComponent)

public:
    FBZZ_GROUP("Length")
    FBZZ_FIELD_RANGE_INT(int, minSegments, 6, "Min Segments", 1, 28)
    FBZZ_TOOLTIP("ここまで削ったら決着。床下に隠せる長さがゼロになる所")
    FBZZ_FIELD_RANGE_INT(int, phase2At, 22, "Phase 2 At", 1, 28)
    FBZZ_TOOLTIP("この節数を下回ったら第 2 段 (頭の突進が入る)")
    FBZZ_FIELD_RANGE_INT(int, phase3At, 14, "Phase 3 At", 1, 28)
    FBZZ_TOOLTIP("この節数を下回ったら第 3 段 (隠せる床下がゼロ・頭が斬れる)")

    FBZZ_GROUP("Damage")
    FBZZ_FIELD_RANGE_INT(int, damagePerSegment, 45, "Per Segment", 0, 1000)
    FBZZ_TOOLTIP("節 1 本を潰したときに入る HP。体力バーは «あと何本» を映す物差しなので、"
                 "削れる節数 × ここ が最大 HP とおおよそ揃っている必要がある")

    FBZZ_GROUP("Feel")
    FBZZ_FIELD_RANGE(float, foldHitStop, 0.34f, "Hitstop", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, foldShake, 0.70f, "Shake", 0.0f, 1.0f)

    FBZZ_GROUP("Debug")
    FBZZ_FIELD_READ_ONLY(int, debugSegments, 28, "Segments")
    FBZZ_FIELD_READ_ONLY(int, debugPhase, 1, "Phase")
    FBZZ_FIELD_READ_ONLY(std::string, debugLastFold, "-", "Last Fold")

    /// 残っている節の数。28 から減る。
    [[nodiscard]] int SegmentCount() const { return m_count; }
    [[nodiscard]] bool IsAlive(int headIndex) const
    {
        return headIndex >= 1 && headIndex <= serpent::kSegmentCount && m_alive[headIndex];
    }
    /// 長さから出る段階。1..3。
    [[nodiscard]] int Phase() const;
    [[nodiscard]] static constexpr int PhaseCount() { return 3; }
    /// 頭に斬撃が通る段か。終盤だけ。
    [[nodiscard]] bool HeadIsVulnerable() const { return m_count <= phase3At; }

    /// 逆極の 2 節で輪を閉じる。間の節を潰して両端を繋ぎ直す。
    /// @ret 潰せた節の数。0 なら成立しなかった。
    int TryFold(int a, int b);

    /// 潰れた節が «居なかったこと» になっているか (絵と当たりの後始末が済んだか)。
    void OnStart() override;
    void OnUpdate() override;

private:
    /// 節 1 本を潰す。絵を伏せ、当たりを畳み、破片をその場で爆ぜさせる。
    ///
    /// WHY 破片をその場に残さないか: この蛇の芯は «離れた 2 節を選ぶ» ことで、
    ///     盤面に帯電体が増えるほど選びにくくなる (boss-serpent.md「潰した節の破片」)。
    void Crush(int headIndex);
    /// その節の分割メッシュ (`E_*_S07`) を集める。
    void CollectMeshes();
    [[nodiscard]] SerpentHitboxRigComponent* Rig() const
    {
        return scene.GetScript<SerpentHitboxRigComponent>();
    }

    bool  m_alive[serpent::kBoneCount]{};
    int   m_count = serpent::kSegmentCount;
    /// 節ごとの分割メッシュ。輪郭と欠損で名指しする。
    std::vector<EntityRef> m_meshes[serpent::kBoneCount];
    bool  m_meshesBuilt = false;
    /// 決着へ落とす処理を 1 度だけ通すための札。
    bool  m_finished = false;

public:
    /// 節 1 本ぶんの分割メッシュ。輪郭を描く側が読む。
    [[nodiscard]] const std::vector<EntityRef>& Meshes(int headIndex) const
    {
        static const std::vector<EntityRef> empty;
        return (headIndex >= 0 && headIndex < serpent::kBoneCount) ? m_meshes[headIndex] : empty;
    }
};

FBZZ_REFLECT(SerpentBodyComponent)

inline int SerpentBodyComponent::Phase() const
{
    if (m_count <= phase3At) return 3;
    if (m_count <= phase2At) return 2;
    return 1;
}

inline void SerpentBodyComponent::OnStart()
{
    for (int i = 0; i < serpent::kBoneCount; ++i) m_alive[i] = true;
    m_count       = serpent::kSegmentCount;
    m_finished    = false;
    m_meshesBuilt = false;
    debugSegments = m_count;
    debugPhase    = Phase();
    debugLastFold = "-";
    CollectMeshes();
}

inline void SerpentBodyComponent::CollectMeshes()
{
    GameObject* self = scene.Self();
    if (!self) return;

    for (auto& list : m_meshes) list.clear();

    // WHY 蛇の «直接の子» だけを見るか: FBX の階層表現として RootNode の下にも
    //     同名のノードが居る。部分木で拾うと、描いていないノードまで輪郭と
    //     欠損の対象に入る (ボス 1 の脚と同じ理由)。
    int found = 0;
    const int childCount = self->GetChildCount();
    for (int i = 0; i < childCount; ++i) {
        GameObject* child = self->GetChild(i);
        if (!child) continue;
        const std::string& name = child->name;
        if (name.rfind("E_", 0) != 0) continue;
        const std::size_t cut = name.find_last_of('_');
        if (cut == std::string::npos) continue;
        const int index = serpent::IndexFromSuffix(std::string_view(name).substr(cut + 1));
        if (index < 0) continue;
        m_meshes[index].push_back(EntityRef{ child->GetID() });
        ++found;
    }

    if (found == 0)
        debug.LogError("SerpentBodyComponent: no split segment meshes found under the serpent. "
                       "Expected direct children named E_*_Head / E_*_S01 .. E_*_S28 "
                       "(see Assets/Docs/boss-serpent.md).");
    m_meshesBuilt = found > 0;
}

inline int SerpentBodyComponent::TryFold(int a, int b)
{
    if (a == b) return 0;
    if (!IsAlive(a) || !IsAlive(b)) return 0;

    const int low  = std::min(a, b);
    const int high = std::max(a, b);

    // 削れる長さは «その 2 節の間に残っている節の数»。既に潰した節は数えない ─
    // 数えると «見えていない所を削った» ことになり、選んだ 2 節の距離と
    // 実際に縮む長さが合わなくなる。
    std::vector<int> crushed;
    for (int i = low + 1; i < high; ++i)
        if (m_alive[i]) crushed.push_back(i);

    if (crushed.empty()) return 0;

    // 最小の長さは割らない。«繋がったまま短くなる» が «消える» になってしまう。
    const int remaining = m_count - static_cast<int>(crushed.size());
    if (remaining < minSegments) {
        // 削りすぎる組でも «何も起きない» にはしない。届く所まで潰す。
        const int allowed = m_count - minSegments;
        if (allowed <= 0) return 0;
        crushed.resize(static_cast<std::size_t>(allowed));
    }

    for (const int index : crushed) Crush(index);

    m_count -= static_cast<int>(crushed.size());
    debugSegments = m_count;
    debugPhase    = Phase();
    debugLastFold = serpent::PartSuffix(low) + " / " + serpent::PartSuffix(high) + " -> -" +
                    std::to_string(static_cast<int>(crushed.size()));

    if (auto* stop = HitstopManagerComponent::Instance()) stop->Hit(Clamp01(foldHitStop));
    if (auto* shake = CameraShakeManagerComponent::Instance()) shake->Shake(Clamp01(foldShake));
    if (auto* pad = RumbleManagerComponent::Instance()) pad->Rumble(0.9f, 0.65f, 0.30f);
    se::Play(audio, se::kAttractConverge);
    se::Play(audio, se::kImpactHeavy);

    // HP は «あと何本» の写しでしかない。折った本数ぶんだけ削る。
    if (auto* combat = CombatManagerComponent::Instance())
        if (GameObject* self = scene.Self())
            (void)combat->DamageEnemyDirect(
                self, std::max(static_cast<int>(crushed.size()) * damagePerSegment, 1));

    return static_cast<int>(crushed.size());
}

inline void SerpentBodyComponent::Crush(int headIndex)
{
    if (headIndex < 1 || headIndex > serpent::kSegmentCount) return;
    m_alive[headIndex] = false;

    Vector3 at = Vector3::ZERO;
    if (auto* rig = Rig()) {
        if (GameObject* hitbox = rig->SegmentHitbox(headIndex)) {
            at = hitbox->transform.worldPosition;
            // 絵だけ消して当たりが残ると «見えない節を斬れる» になる。
            if (auto* part = scene.GetScript<BossPartPolarityComponent>(hitbox)) part->Break();
            hitbox->SetActive(false);
        }
    }

    for (const EntityRef& ref : m_meshes[headIndex]) {
        if (GameObject* piece = ref.Resolve(scene)) {
            // 同じフレームで輪郭も取り下げる。次のフレームまで残ると、
            // 消えた節の輪郭だけが 1 コマ空中に浮く。
            objectMask.Clear(*piece);
            piece->SetActive(false);
        }
    }

    // 破片は床下へ落ちる扱い。その場に «帯電できる残骸» を残さない。
    if (auto* vfx = VfxManagerComponent::Instance())
        vfx->PlayImpact(at, Polarity::None, 1.0f, true);
}

inline void SerpentBodyComponent::OnUpdate()
{
    if (!m_meshesBuilt) CollectMeshes();

    debugSegments = m_count;
    debugPhase    = Phase();

    // 最小の長さまで削られたら決着。HP がいくら残っていても倒れる。
    //
    // WHY 別のルートを用意するか: 削るのは «逆極の 2 節を選ぶ» というこの蛇の核で、
    //     HP を削るのとは別の詰め方になっている。最後だけ «結局 HP» になると、
    //     機構そのものが勝ち筋にならない。
    if (m_finished || m_count > minSegments) return;
    m_finished = true;

    if (auto* combat = CombatManagerComponent::Instance()) {
        if (GameObject* self = scene.Self()) {
            if (auto* health = scene.GetScript<EnemyHealthComponent>(self))
                (void)combat->DamageEnemyDirect(self, std::max(health->Current(), 1));
        }
    }
}

} // namespace sandbox
