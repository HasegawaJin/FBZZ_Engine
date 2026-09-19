/// @file    SerpentBodyComponent.hpp
/// @brief   胴の長さと «折る» 操作。とどめで斬った所から潰し、両端を繋ぎ直す
/// @author  Hasegawa Jin
/// @date    2026-08-31
///
/// @note 潰れた節は削除せず両端を繋ぎ直して縮める (boss-serpent.md「胴を折る」)。
///       骨の親子は書き出しで固定され途中の 1 本を抜けないため、リンク長を 0 にして
///       畳む (実装は SerpentSpineComponent)。1 回で削れる長さが毎回同じになり、
///       残り節数がそのまま «あと何回とどめを通すか» になる。
/// @note 段階はフェーズ変数でなく節数そのもので判定する (28〜23 / 22〜15 / 14 以下)。
///       別変数を持つと節数とフェーズが食い違う状態を作れてしまう。
#pragma once

#include <Engine/Scene/Components/ColliderComponent.hpp>
#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Components/RigidBodyComponent.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Math/Quaternion.hpp>
#include <Scripts/Combat/BossPartDebrisComponent.hpp>
#include <Scripts/Combat/EnemyHealthComponent.hpp>
#include <Scripts/Combat/SerpentBones.hpp>
#include <Scripts/Combat/SerpentHitboxRigComponent.hpp>
#include <Scripts/Game/CameraShakeManagerComponent.hpp>
#include <Scripts/Game/CombatManagerComponent.hpp>
#include <Scripts/Game/HitstopManagerComponent.hpp>
#include <Scripts/Game/RumbleManagerComponent.hpp>
#include <Scripts/Game/VfxManagerComponent.hpp>
#include <Scripts/Utils/SeLibrary.hpp>
#include <Scripts/Utils/WeaponSockets.hpp>
#include <Math/Vector3.hpp>
#include <algorithm>
#include <cstdint>
#include <memory>
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

    /// @brief とどめ 1 回につき、潰れた一続きの先頭 1 本だけを床へ落とす
    ///        (Docs/part-break.md「柱 3 — 戦利品」)。
    /// @note 4 本まとめて落とすと開口 16 口の盤面が全部塞がり «詰み» になるため、
    ///       斬った 1 本だけを落とす。落ちた節は突き上げ・薙ぎで吹き飛び弾ける一撃になる。
    FBZZ_GROUP("落ちた節")
    FBZZ_FIELD(bool, dropDebris, true, "節を床へ落とす")
    FBZZ_TOOLTIP("とどめで潰れた先頭の 1 本を剛体として床へ落とす。"
                 "落ちた節は口を塞ぎ、突き上げ・薙ぎで吹き飛ばされて弾ける一撃になる")
    FBZZ_FIELD_RANGE_INT(int, maxDebris, 3, "上限 [本]", 0, 10)
    FBZZ_TOOLTIP("盤面に置ける本数。**16 口のうち何口まで塞がってよいか**と読む ─ "
                 "多いと渡れる組が尽きて、蛇が同じ 2 口を往復するだけになる")
    /// @note 立方体で囲う。節の «長い向き» はバインド姿勢の骨の +Y だが、当たりは
    ///       モデル空間の軸に沿った箱でしか作れず、向きを取り違えると見えている所で
    ///       止まらないずれになる。節長 0.80m・太さ 1.24m の両方を包む立方体にする。
    FBZZ_FIELD_RANGE(float, debrisRadius, 0.75f, "当たりの半径 [m]", 0.1f, 3.0f)
    FBZZ_TOOLTIP("落ちた節を囲う立方体の半径。節は長さ 0.80m・太さは前 1.24m 〜 尾 0.20m")
    FBZZ_FIELD_RANGE(float, debrisMass, 30.0f, "Mass", 1.0f, 300.0f)
    FBZZ_FIELD_RANGE(float, debrisKick, 5.0f, "蹴り [m/s]", 0.0f, 30.0f)
    FBZZ_TOOLTIP("潰れた瞬間に斬った側から離れる速さ")
    FBZZ_FIELD_RANGE(float, debrisLift, 4.5f, "浮き [m/s]", 0.0f, 20.0f)
    FBZZ_FIELD_RANGE(float, debrisSpin, 5.0f, "回転 [rad/s]", 0.0f, 30.0f)
    FBZZ_FIELD_RANGE(float, debrisDrag, 0.30f, "Drag", 0.0f, 5.0f)
    FBZZ_FIELD_READ_ONLY(int, debugDebris, 0, "落とした節")

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

    /// @brief とどめ (Docs/break-parry.md)。headIndex から尾の側へ count 本を潰す。
    /// @return 潰せた節の数。0 なら何も起きなかった。
    /// @note 飛ぶ節の数は斬った場所でなく «何回倒したか» で決まる。どこを斬っても
    ///       同じ長さだけ短くなる方が読める。
    int Sever(int headIndex, int count);

    /// 潰れた節が «居なかったこと» になっているか (絵と当たりの後始末が済んだか)。
    void OnStart() override;
    void OnUpdate() override;

private:
    /// @brief 節 1 本を潰す。絵を伏せ、当たりを畳む。
    /// @return 潰した節が居た所 (ワールド)。
    /// @note 爆発はここで鳴らさない。潰れる節数は 2 節の距離で決まり最大 20 本超に
    ///       なるため、節ごとに鳴らすと爆発が同一フレームで重なり光源・陽炎が飽和して
    ///       «どこからどこまで潰れたか» が消える。呼ぶ側 (Sever) が 1 回ぶんへまとめる。
    /// @note 潰れた節の破片はその場に残さない。盤面に物が増えるほど次にどこを斬るか
    ///       読みにくくなる (boss-serpent.md「潰した節の破片」)。
    Vector3 Crush(int headIndex);
    /// 潰した一続きを 1 つの出来事として鳴らす。
    /// @param at 潰れた節が居た所 (頭側から順)
    /// @param heavy 折り (重い) か、とどめ (軽い) か。
    void CollapseBurst(const std::vector<Vector3>& at, bool heavy);
    /// その節の分割メッシュ (`E_*_S07`) を集める。
    void CollectMeshes();

    /// @brief 節 1 本を剛体として床へ落とす。
    /// @note 骨は SerpentSpineComponent が毎フレーム経路へ沿わせるため、潰した節だけを
    ///       別に動かす経路が無い。同じ submesh を `Serpent.fbx:N` として静的に描き、
    ///       バインド姿勢のまま «物» にする (BossRigComponent::SpawnLegDebris と同じ形)。
    /// @note scene.Create は呼ぶ側の最後に置くこと。GameObject 配列を再確保するので、
    ///       潰す処理の途中で作ると握っている GameObject* が無効になる。
    void DropDebris(int headIndex, const Vector3& at);

    /// @brief 節の骨のバインド姿勢を控える。落とした節の静的メッシュを «今の節» に
    ///        重ねる基準になる。
    /// @note OnStart で 1 回だけ呼ぶ。骨を動かす SerpentSpineComponent::OnUpdate より
    ///       前に必ず通るため、バインドに一番近い時点になる。
    void CaptureBind();
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
    /// 節ごとのバインド姿勢 (蛇の根空間)。CaptureBind が書く。
    bool       m_bindCaptured = false;
    Vector3    m_bindPos[serpent::kBoneCount];
    Quaternion m_bindRot[serpent::kBoneCount];

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
    debugDebris   = 0;
    CollectMeshes();
    CaptureBind();
}

inline void SerpentBodyComponent::CaptureBind()
{
    GameObject* self = scene.Self();
    if (!self || m_bindCaptured) return;

    const Vector3    rootPos = self->transform.worldPosition;
    const Quaternion rootInv = self->transform.worldRotation.Inverse();

    bool any = false;
    for (int i = 0; i < serpent::kBoneCount; ++i) {
        GameObject* bone = FindInSubtree(*self, serpent::BoneName(i));
        if (!bone) continue;
        any = true;
        m_bindPos[i] = rootInv * (bone->transform.worldPosition - rootPos);
        m_bindRot[i] = (rootInv * bone->transform.worldRotation).Normalized();
    }
    m_bindCaptured = any;
}

inline void SerpentBodyComponent::CollectMeshes()
{
    GameObject* self = scene.Self();
    if (!self) return;

    for (auto& list : m_meshes) list.clear();

    /// @note 蛇の直接の子だけを見る。FBX の階層表現として RootNode の下にも同名の
    ///       ノードが居るため、部分木で拾うと描いていないノードまで輪郭と欠損の
    ///       対象に入ってしまう (ボス 1 の脚と同じ理由)。
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

    /// @note 斬った節から尾へ向かって、生きている節を count 本。尾側が足りなければ頭側へ戻る
    ///       (尾の付け根を斬ったときに «何も飛ばない» にしない)。
    std::vector<int> crushed;
    for (int i = headIndex; i <= serpent::kSegmentCount && static_cast<int>(crushed.size()) < count; ++i)
        if (m_alive[i]) crushed.push_back(i);
    for (int i = headIndex - 1; i >= 1 && static_cast<int>(crushed.size()) < count; --i)
        if (m_alive[i]) crushed.push_back(i);
    if (crushed.empty()) return 0;

    /// @note 最小の長さは割らない。«繋がったまま短くなる» が «消える» になってしまう。
    const int allowed = m_count - minSegments;
    if (allowed <= 0) return 0;
    if (static_cast<int>(crushed.size()) > allowed) crushed.resize(static_cast<std::size_t>(allowed));

    std::vector<Vector3> at;
    at.reserve(crushed.size());
    for (const int index : crushed) at.push_back(Crush(index));
    /// @note とどめは «切断» の絵 (PlayExecute) を呼ぶ側が斬った所へ出す。ここは
    ///       飛んだ節が居た所を軽く言うだけ ─ 芯を 2 つ置くと閃光が重なって白く抜ける。
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

    /// @note 落とすのは «斬った 1 本» だけ、そして必ず最後に。scene.Create が
    ///       GameObject 配列を再確保するので、ここより前に置くと self が無効になる。
    DropDebris(crushed.front(), at.front());

    return static_cast<int>(crushed.size());
}

inline void SerpentBodyComponent::DropDebris(int headIndex, const Vector3& at)
{
    if (!dropDebris || debugDebris >= std::max(maxDebris, 0)) return;
    if (headIndex < 1 || headIndex > serpent::kSegmentCount) return;

    GameObject* self = scene.Self();
    if (!self) return;
    if (!m_bindCaptured) CaptureBind();
    if (!m_bindCaptured) return;

    GameObject* bone = FindInSubtree(*self, serpent::BoneName(headIndex));
    if (!bone) return;

    /// @note 今の骨の姿勢に、バインド姿勢の節を重ねる。静的メッシュはモデル空間
    ///       (＝ 蛇の根空間のバインド) で描かれるので、根をどこへ置けば節が一致するかを解く。
    const Quaternion rot =
        (bone->transform.worldRotation * m_bindRot[headIndex].Inverse()).Normalized();
    const Vector3 pos = bone->transform.worldPosition - rot * m_bindPos[headIndex];

    /// @note scene.Create の前に読み終える (Create は GameObject 配列を再確保する)。
    struct Piece {
        std::string   model;
        std::uint32_t submesh = 0;
        std::string   material;
    };
    std::vector<Piece> pieces;
    for (const EntityRef& ref : m_meshes[headIndex]) {
        GameObject* piece = ref.Resolve(scene);
        if (!piece) continue;
        auto* skin = piece->GetComponent<SkinnedMeshRenderer>();
        if (!skin || skin->modelPath.empty()) continue;
        Piece entry;
        /// @note "guid:xxx|Assets/..." の形なら、パスの側だけを使う。
        const std::size_t bar = skin->modelPath.find('|');
        entry.model = bar == std::string::npos ? skin->modelPath
                                               : skin->modelPath.substr(bar + 1);
        entry.submesh = skin->submeshIndices.empty() ? 0u : skin->submeshIndices[0];
        if (auto* material = piece->GetComponent<MaterialComponent>())
            entry.material = material->materialPath;
        pieces.push_back(entry);
    }
    if (pieces.empty()) return;

    const std::string name = "SerpentDebris_" + serpent::PartSuffix(headIndex);
    const EntityRef   root{ scene.Create(name).GetID() };

    /// @note 子を先に全部作る。作りながら root を掴み続けると、途中で無効になる。
    std::vector<EntityRef> children;
    children.reserve(pieces.size());
    for (std::size_t i = 0; i < pieces.size(); ++i)
        children.push_back(EntityRef{ scene.Create(name + "_" + std::to_string(i)).GetID() });

    GameObject* debris = root.Resolve(scene);
    if (!debris) return;
    debris->runtimeGenerated        = true;
    debris->transform.position      = pos;
    debris->transform.rotation      = rot;
    debris->transform.worldPosition = pos;
    debris->transform.worldRotation = rot;

    for (std::size_t i = 0; i < pieces.size(); ++i) {
        GameObject* child  = children[i].Resolve(scene);
        GameObject* parent = root.Resolve(scene);
        if (!child || !parent) continue;
        child->runtimeGenerated = true;
        child->SetParent(*parent);
        child->transform.position = Vector3::ZERO;
        child->transform.rotation = Quaternion::Identity();

        auto& renderer = child->AddComponent<MeshRenderer>();
        renderer.meshPath      = pieces[i].model + ":" + std::to_string(pieces[i].submesh);
        renderer.meshPathDirty = true;
        renderer.castShadows   = true;
        if (!pieces[i].material.empty()) {
            auto& material = child->AddComponent<MaterialComponent>();
            material.SetMaterialPath(pieces[i].material);
        }
    }

    debris = root.Resolve(scene);
    if (!debris) return;

    const float r = Max(debrisRadius, 0.1f);
    {
        auto& box = debris->AddComponent<BoxColliderComponent>();
        box.SetSize(Vector3{ r * 2.0f, r * 2.0f, r * 2.0f });
        /// @note 当たりは «節が居るところ»。静的メッシュは 24m の胴まるごとの座標系で
        ///       描かれているので、原点に置くと 10m 離れた所に箱が立つ。
        box.center = m_bindPos[headIndex];
    }
    {
        RigidBodyComponent rb{};
        rb.rigidBody = std::make_unique<fbzz::physics::RigidBody>();
        rb.rigidBody->SetMass(Max(debrisMass, 1.0f));
        rb.rigidBody->SetPosition(pos);
        rb.rigidBody->SetRotation(rot);
        rb.rigidBody->m_linearDrag  = Max(debrisDrag, 0.0f);
        rb.rigidBody->m_angularDrag = Max(debrisDrag, 0.0f) * 1.5f;
        rb.rigidBody->m_useCCD      = true;
        rb.rigidBody->m_ccdRadius   = r;
        rb.ResetPhysicsSyncState(pos, rot);
        debris->AddComponent<RigidBodyComponent>(std::move(rb));
    }

    /// @note 斬った側から離れる向きへ蹴る。at は潰れた節が居た所なので、蛇の根から見て
    ///       その外向きが «飛んだ向き»。
    Vector3 away = at - self->transform.worldPosition;
    away.y = 0.0f;
    away = away.NormalizedOr(Vector3::FORWARD);

    const Vector3 velocity = away * Max(debrisKick, 0.0f) + Vector3::UP * Max(debrisLift, 0.0f);
    const Vector3 axis     = Vector3::Cross(Vector3::UP, away).NormalizedOr(Vector3::FORWARD);

    auto& script = debris->AddScript<BossPartDebrisComponent>();
    script.Setup(velocity, axis * Max(debrisSpin, 0.0f), r * 2.0f);

    ++debugDebris;
}

inline Vector3 SerpentBodyComponent::Crush(int headIndex)
{
    if (headIndex < 1 || headIndex > serpent::kSegmentCount) return Vector3::ZERO;
    m_alive[headIndex] = false;

    Vector3 at = Vector3::ZERO;
    if (auto* rig = Rig()) {
        if (GameObject* hitbox = rig->SegmentHitbox(headIndex)) {
            at = hitbox->transform.worldPosition;
            /// @note 絵だけ消して当たりが残ると «見えない節を斬れる» になる。
            if (auto* part = scene.GetScript<BossPartComponent>(hitbox)) part->Break();
            hitbox->SetActive(false);
        }
    }

    for (const EntityRef& ref : m_meshes[headIndex]) {
        if (GameObject* piece = ref.Resolve(scene)) {
            /// @note 同じフレームで輪郭も取り下げる。次のフレームまで残ると、
            ///       消えた節の輪郭だけが 1 コマ空中に浮く。
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

    /// @note 芯は «真ん中» に 1 発。潰れた一続きの重心がそのまま «どこが縮んだか» になる。
    Vector3 centre = Vector3::ZERO;
    for (const Vector3& point : at) centre = centre + point;
    centre = centre / static_cast<float>(at.size());
    vfx->PlayImpact(centre, BladeSide::None, heavy ? 1.0f : 0.7f, /*againstAnchor=*/true);

    /// @note 残りは «破片» として軽く。爆発を並べると光と陽炎がその数だけ増えるので、
    ///       光源も陽炎も持たない土煙のグラフで «そこも潰れた» だけを言う。全節には
    ///       置かない ─ 20 本潰れても読めるのは «端から端まで» なので、両端とその間を
    ///       3 つに割った点で足りる。
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

    /// @note 斬撃の列挙中に節を落とすと、残骸生成で後続の参照が無効になる。
    ///       耐久の消費はここで行い、とどめと同じ欠損・残骸・進行の経路へ渡す。
    if (!m_finished && m_count > minSegments) {
        if (auto* rig = Rig()) {
            for (int i = 1; i <= serpent::kSegmentCount; ++i) {
                if (!IsAlive(i)) continue;
                GameObject* hitbox = rig->SegmentHitbox(i);
                auto* part = hitbox ? hitbox->GetScript<BossPartComponent>() : nullptr;
                if (!part || part->IsBroken() || !part->IsDepleted()) continue;
                const Vector3 at = hitbox->transform.worldPosition;
                if (Sever(i, 1) > 0) {
                    if (auto* vfx = VfxManagerComponent::Instance())
                        vfx->PlayExecute(at, Vector3::UP, 0.55f);
                }
                break;
            }
        }
    }

    debugSegments = m_count;
    debugPhase    = Phase();

    /// @note 最小の長さまで削られたら決着 (HP の残量に関わらず倒れる)。節を削るのが
    ///       この蛇の核となる詰め方で、HP を削るのとは別ルート。最後だけ «結局 HP» に
    ///       なると機構そのものが勝ち筋にならないため、専用の決着条件として残す。
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
