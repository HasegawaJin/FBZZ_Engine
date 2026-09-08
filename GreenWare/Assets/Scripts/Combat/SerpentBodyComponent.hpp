/// @file    SerpentBodyComponent.hpp
/// @brief   胴の長さと «折る» 操作。とどめで斬った所から潰し、両端を繋ぎ直す
/// @author  Hasegawa Jin
/// @date    2026-08-31
///
/// WHY 千切って後ろを落とさないか (boss-serpent.md「胴を折る」):
///   斬った所から後ろを丸ごと落とすと 1 回で大半が飛び、«あと何回» が読めなくなる。
///   潰した節ぶんだけ縮めて両端を繋ぎ直す形なら、1 回で削れる長さが毎回同じになり、
///   残りの節数がそのまま «あと何回とどめを通すか» になる。
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
#include <Math/Vector3.hpp>
#include <algorithm>
#include <string>
#include <vector>

using namespace fbzz::scene;
using namespace fbzz::math;

namespace sandbox {

class SerpentBodyComponent : public Script {
    FBZZ_SCRIPT(SerpentBodyComponent)

public:
    FBZZ_GROUP("長さ")
    FBZZ_FIELD_RANGE_INT(int, minSegments, 6, "Min Segments", 1, 28)
    FBZZ_TOOLTIP("ここまで削ったら決着。床下に隠せる長さがゼロになる所")
    FBZZ_FIELD_RANGE_INT(int, phase2At, 22, "Phase 2 At", 1, 28)
    FBZZ_TOOLTIP("この節数を下回ったら第 2 段 (頭の突進が入る)")
    FBZZ_FIELD_RANGE_INT(int, phase3At, 14, "Phase 3 At", 1, 28)
    FBZZ_TOOLTIP("この節数を下回ったら第 3 段 (隠せる床下がゼロ・頭が斬れる)")

    FBZZ_GROUP("ダメージ")
    FBZZ_FIELD_RANGE_INT(int, damagePerSegment, 45, "Per Segment", 0, 1000)
    FBZZ_TOOLTIP("節 1 本を潰したときに入る HP。体力バーは «あと何本» を映す物差しなので、"
                 "削れる節数 × ここ が最大 HP とおおよそ揃っている必要がある")

    FBZZ_GROUP("手触り")
    FBZZ_FIELD_RANGE(float, foldHitStop, 0.34f, "ヒットストップ", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, foldShake, 0.70f, "揺れ", 0.0f, 1.0f)

    FBZZ_GROUP("デバッグ")
    FBZZ_FIELD_READ_ONLY(int, debugSegments, 28, "分割数")
    FBZZ_FIELD_READ_ONLY(int, debugPhase, 1, "位相")
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

    /// とどめ (Docs/break-parry.md)。headIndex から尾の側へ count 本を潰す。
    /// @ret 潰せた節の数。0 なら何も起きなかった。
    ///
    /// WHY 斬った場所で本数が変わらないか: 飛ぶ節の数を選ぶのはプレイヤーではなく
    ///     «何回倒したか» なので、どこを斬っても同じ長さだけ短くなる方が読める。
    int Sever(int headIndex, int count);

    /// 潰れた節が «居なかったこと» になっているか (絵と当たりの後始末が済んだか)。
    void OnStart() override;
    void OnUpdate() override;

private:
    /// 節 1 本を潰す。絵を伏せ、当たりを畳む。@ret 潰した節が居た所 (ワールド)。
    ///
    /// WHY ここで爆発を鳴らさないか: 折りは «輪が閉じて間が潰れる» という 1 つの
    ///     出来事で、潰れる節の数は 2 節の距離で決まる (最大 20 本超)。節ごとに
    ///     1 発ずつ鳴らすと、同じフレームに同じ爆発が 20 発重なる ─ 光源は
    ///     先着 3 発が満光、陽炎も同数、絵としては «白い塊» にしかならず、
    ///     «どこからどこまでが潰れたか» という肝心の情報が消える。
    ///     鳴らすのは呼ぶ側 (Sever) が «1 回ぶん» としてまとめる。
    ///
    /// WHY 破片をその場に残さないか: 盤面に物が増えるほど «次にどこを斬るか» が
    ///     読みにくくなる (boss-serpent.md「潰した節の破片」)。
    Vector3 Crush(int headIndex);
    /// 潰した一続きを 1 つの出来事として鳴らす。
    /// @param at 潰れた節が居た所 (頭側から順)
    /// @param heavy 折り (重い) か、とどめ (軽い) か。
    void CollapseBurst(const std::vector<Vector3>& at, bool heavy);
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

inline int SerpentBodyComponent::Sever(int headIndex, int count)
{
    if (headIndex < 1 || headIndex > serpent::kSegmentCount || count <= 0) return 0;

    // 斬った節から尾へ向かって、生きている節を count 本。尾側が足りなければ頭側へ戻る
    // (尾の付け根を斬ったときに «何も飛ばない» にしない)。
    std::vector<int> crushed;
    for (int i = headIndex; i <= serpent::kSegmentCount && static_cast<int>(crushed.size()) < count; ++i)
        if (m_alive[i]) crushed.push_back(i);
    for (int i = headIndex - 1; i >= 1 && static_cast<int>(crushed.size()) < count; --i)
        if (m_alive[i]) crushed.push_back(i);
    if (crushed.empty()) return 0;

    // 最小の長さは割らない。«繋がったまま短くなる» が «消える» になってしまう。
    const int allowed = m_count - minSegments;
    if (allowed <= 0) return 0;
    if (static_cast<int>(crushed.size()) > allowed) crushed.resize(static_cast<std::size_t>(allowed));

    std::vector<Vector3> at;
    at.reserve(crushed.size());
    for (const int index : crushed) at.push_back(Crush(index));
    // とどめは «切断» の絵 (PlayExecute) を呼ぶ側が斬った所へ出す。ここは
    // 飛んだ節が居た所を軽く言うだけ ─ 芯を 2 つ置くと閃光が重なって白く抜ける。
    CollapseBurst(at, /*heavy=*/false);

    m_count -= static_cast<int>(crushed.size());
    debugSegments = m_count;
    debugPhase    = Phase();
    debugLastFold = "Execute " + serpent::PartSuffix(headIndex) + " -> -" +
                    std::to_string(static_cast<int>(crushed.size()));

    if (auto* stop = HitstopManagerComponent::Instance()) stop->Hit(Clamp01(foldHitStop));
    if (auto* shake = CameraShakeManagerComponent::Instance()) shake->Shake(Clamp01(foldShake));
    if (auto* pad = RumbleManagerComponent::Instance()) pad->Rumble(0.9f, 0.65f, 0.30f);
    se::Play(audio, se::kSerpentDestroy);
    se::Play(audio, se::kImpactHeavy);

    if (auto* combat = CombatManagerComponent::Instance())
        if (GameObject* self = scene.Self())
            (void)combat->DamageEnemyDirect(
                self, std::max(static_cast<int>(crushed.size()) * damagePerSegment, 1));

    return static_cast<int>(crushed.size());
}

inline Vector3 SerpentBodyComponent::Crush(int headIndex)
{
    if (headIndex < 1 || headIndex > serpent::kSegmentCount) return Vector3::ZERO;
    m_alive[headIndex] = false;

    Vector3 at = Vector3::ZERO;
    if (auto* rig = Rig()) {
        if (GameObject* hitbox = rig->SegmentHitbox(headIndex)) {
            at = hitbox->transform.worldPosition;
            // 絵だけ消して当たりが残ると «見えない節を斬れる» になる。
            if (auto* part = scene.GetScript<BossPartComponent>(hitbox)) part->Break();
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
    return at;
}

inline void SerpentBodyComponent::CollapseBurst(const std::vector<Vector3>& at, bool heavy)
{
    auto* vfx = VfxManagerComponent::Instance();
    if (!vfx || at.empty()) return;

    // 芯は «真ん中» に 1 発。潰れた一続きの重心がそのまま «どこが縮んだか» になる。
    Vector3 centre = Vector3::ZERO;
    for (const Vector3& point : at) centre = centre + point;
    centre = centre / static_cast<float>(at.size());
    vfx->PlayImpact(centre, BladeSide::None, heavy ? 1.0f : 0.7f, /*againstAnchor=*/true);

    // 残りは «破片» として軽く。爆発を並べると光と陽炎がその数だけ増えるので、
    // 光源も陽炎も持たない土煙のグラフで «そこも潰れた» だけを言う。
    //
    // WHY 全部の節に置かないか: 20 本潰れる回でも読めるのは «端から端まで» で、
    //     間の 1 本 1 本ではない。両端と、その間を 3 つに割った点で足りる。
    const std::size_t count = at.size();
    const auto puff = [&](std::size_t index) {
        if (index >= count) return;
        Vector3 away = at[index] - centre;
        away.y = 0.0f;
        vfx->PlaySerpentRush(at[index], away.NormalizedOr(Vector3::FORWARD), 0.8f);
    };
    puff(0);
    puff(count - 1);
    if (count >= 4) {
        puff(count / 3);
        puff((count * 2) / 3);
    }
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
