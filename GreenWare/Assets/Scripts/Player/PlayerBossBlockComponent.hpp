/// @file    PlayerBossBlockComponent.hpp
/// @brief   ボスの脚と胴にプレイヤーがぶつかる。押されるのは **プレイヤーだけ**
/// @author  Hasegawa Jin
/// @date    2026-09-08
///
/// **2026-09-10: これが正本に戻った。**一度は物理のコライダー
///   (`BossHitboxRigComponent::solidLegs`) へ移したが、成立の条件が
///   `ProjectSettings.toml` の衝突行列という**別ファイル**にあり、そのファイルは
///   エディタが開いていると古い設定で上書き保存されて消える。消えた瞬間の壊れ方が
///   «ボスが吹き飛ぶ» で、しかも原因が «AI が歩かせている» ようにしか見えない。
///   **一番重い壊れ方を、消えうる設定に賭けさせない。**
///   物理の方は残してあるが既定 off で、行列を確かめてから入れる物にした。
///
/// **背に乗るのはここではない** (2026-09-10 深夜)。足場は `BossHitboxRigComponent` が
///   背に置く本物の当たり (`HB_Deck`) が持つ。あちらは `attachToParentBody` で
///   ボス本体の剛体へ属していて、同じ剛体のコライダー同士は当たらないので
///   自己衝突しない。こちらの `standOnBack` は «剛体を持たない体» 用の予備で既定 off
///   ── 両方を有効にすると同じフレームで 2 回持ち上げて震える。
///
/// WHY 当時は物理のコライダーでやらなかったか:
///   最初はボスの当たり (BossHitboxRigComponent) を isTrigger=false にして塞いだが、
///   **ボスが吹き飛んだ。**衝突レイヤーの行列が物理へ渡っておらず
///   (受け口 World::Step(layerFilter) はあったが誰も渡していなかった)、
///   ボス配下に置いた solid はボス自身の胴カプセルと RigidBody (mass 400) に必ず当たった。
///   しかもボーンは毎フレーム «瞬間移動» するので、めり込み解決が巨大な力になる。
///
/// WHY プレイヤーを押す側に置くか:
///   «ぶつかる» で本当に要るのは «プレイヤーがボスの中へ入れない» ことだけで、
///   ボスが押し返される必要は無い (6m・400kg の重機が小型ロボに押されては困る)。
///   片側だけを動かせば、自己衝突も質量比の破綻も原理的に起きない。
#pragma once

#include <Engine/Scene/EntityRef.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Math/Vector3.hpp>
#include <Scripts/Combat/IBoss.hpp>
#include <Scripts/Player/PlayerClimbComponent.hpp>
#include <algorithm>
#include <string>
#include <vector>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

class PlayerBossBlockComponent : public Script {
    FBZZ_SCRIPT(PlayerBossBlockComponent)

public:
    FBZZ_GROUP("太さ")
    FBZZ_FIELD_RANGE(float, legRadius, 0.42f, "脚の半径", 0.05f, 3.0f)
    FBZZ_TOOLTIP("脚 1 本の «ぶつかる» 太さ。BossHitboxRig の legRadius より少し太くして、"
                 "斬れる範囲の外側で止まるようにする")
    FBZZ_FIELD_RANGE(float, bodyRadius, 1.75f, "胴の半径", 0.1f, 6.0f)
    FBZZ_FIELD_RANGE(float, playerRadius, 0.52f, "プレイヤーの半径", 0.05f, 2.0f)

    FBZZ_GROUP("挙動")
    FBZZ_FIELD_RANGE(float, pushPerSecond, 14.0f, "押し戻す速さ [m/s]", 0.5f, 60.0f)
    FBZZ_TOOLTIP("めり込みを解く速さ。小さいと «ゆっくり押し出される»、"
                 "大きいと «壁に当たった» になる。瞬間移動させないのは、"
                 "1 フレームで飛ぶとカメラが追従しきれないため")
    FBZZ_FIELD_RANGE(float, skin, 0.02f, "余白", 0.0f, 0.5f)

    FBZZ_GROUP("背に乗る")
    // WHY 既定を off にしたか (2026-09-10): 足場は BossHitboxRigComponent が背に置く
    //     本物の当たり (HB_Deck) が持つようになった。あちらはボス本体の剛体へ
    //     属しているので自己衝突しない。こちらは «剛体を持たない体» 用の予備で、
    //     両方を有効にすると同じフレームで 2 回持ち上げて震える。
    FBZZ_FIELD(bool, standOnBack, false, "背に乗れる (物理の足場が無いとき用)")
    FBZZ_TOOLTIP("座標で持ち上げる予備の足場。BossHitboxRig の «甲板の足場» が "
                 "on なら要らない。両方 on にすると震える")
    FBZZ_FIELD(std::string, deckAnchorBone, "Body", "基準の骨")
    FBZZ_TOOLTIP("PlayerClimbComponent の «基準の骨» と必ず同じにする")
    FBZZ_FIELD_RANGE(float, deckRise, 1.33f, "甲板の高さ", 0.0f, 6.0f)
    FBZZ_TOOLTIP("基準の骨から足場の面までの高さ [m]。"
                 "PlayerClimbComponent の同名の値と揃える")
    FBZZ_FIELD_RANGE(float, deckRadius, 1.60f, "甲板の広さ", 0.2f, 5.0f)
    FBZZ_TOOLTIP("面の半径 [m]。この外へ出ると乗らなくなって落ちる")
    FBZZ_FIELD_RANGE(float, deckGrip, 1.20f, "拾い上げる深さ", 0.05f, 4.0f)
    FBZZ_TOOLTIP("面より下この距離までなら «乗っている» として持ち上げる。"
                 "浅いと登り着いた直後にすり抜け、深いと甲羅の中から吸い上がる")

    FBZZ_GROUP("デバッグ")
    FBZZ_FIELD(bool, drawShapes, false, "形を描く")
    FBZZ_FIELD_READ_ONLY(int, debugBlocked, 0, "押し出した本数")
    FBZZ_FIELD_READ_ONLY(bool, debugOnBack, false, "背に乗っている")

    void OnStart()  override;
    void OnUpdate() override;
    void OnDrawGizmos() override;

private:
    /// 脚 1 本ぶんの «線分»。両端は骨そのものを覚えておき、位置だけ毎フレーム読む。
    ///
    /// WHY GameObject を覚えるか: 名前で引くと 1 本ごとにボスの全サブツリーを再帰で
    ///     歩くことになる (100 ノード超)。13 本を毎フレームやると、それだけで重くなる。
    ///     骨の «並び» は転倒しても変わらないので、覚えたまま位置だけ読めばいい。
    struct Limb { EntityRef a, b; float radius; };

    [[nodiscard]] GameObject* Boss() const;
    [[nodiscard]] static GameObject* FindInSubtree(GameObject& root, const std::string& name);
    /// 線分 ab へ p から下ろした最近点。
    [[nodiscard]] static Vector3 ClosestOnSegment(const Vector3& a, const Vector3& b,
                                                  const Vector3& p);
    void CollectLimbs(GameObject& boss);
    /// 背の «面» に乗せる。乗せたら true。
    ///
    /// WHY 面を «円 + 深さ» で書くか: 甲板は骨から測った点で、そこに形は無い。
    ///     箱を置くと物理の当たりが要り、それはボス自身を押す危険に戻る。
    ///     «この円の中で、面より少し下に居るなら面へ上げる» だけなら、
    ///     動くのは必ずプレイヤーだけになる。
    bool HoldOnBack(GameObject& boss);
    /// プレイヤーをこのワールド座標へ置く。
    ///
    /// WHY ローカルまで書くか: スクリプトの次に走る TransformSystem (PrePhysics) が
    ///     ルートから «ローカル値でワールドを組み直す» ので、ワールドだけ書いた 1 行は
    ///     物理へ渡る前に捨てられる ── 押し出しも «面へ上げる» も黙って効かなくなる
    ///     (PlayerClimbComponent::Place と同じ理由)。プレイヤーはルートなので
    ///     local = world。
    void Place(const Vector3& world);

    EntityRef          m_boss;
    std::vector<Limb>  m_limbs;
    /// ボスを引き直すまでの残り [秒]。名簿引きは安いが、骨の探索は全サブツリーを歩く。
    float              m_probeCooldown = 0.0f;
    static constexpr float kProbeInterval = 0.5f;
};

FBZZ_REFLECT(PlayerBossBlockComponent)

inline void PlayerBossBlockComponent::OnStart()
{
    m_boss = {};
    m_limbs.clear();
    m_probeCooldown = 0.0f;
    debugBlocked = 0;
}

inline void PlayerBossBlockComponent::Place(const Vector3& world)
{
    transform.position      = world;
    transform.worldPosition = world;
}

inline GameObject* PlayerBossBlockComponent::Boss() const
{
    if (GameObject* cached = m_boss.Resolve(scene)) return cached;
    return FindBossOnBoard(scene);
}

inline GameObject* PlayerBossBlockComponent::FindInSubtree(GameObject& root,
                                                           const std::string& name)
{
    if (root.name == name) return &root;
    const int count = root.GetChildCount();
    for (int i = 0; i < count; ++i)
        if (GameObject* child = root.GetChild(i))
            if (GameObject* found = FindInSubtree(*child, name)) return found;
    return nullptr;
}

inline Vector3 PlayerBossBlockComponent::ClosestOnSegment(const Vector3& a, const Vector3& b,
                                                          const Vector3& p)
{
    const Vector3 ab = b - a;
    const float   len2 = Vector3::Dot(ab, ab);
    if (len2 < EPSILON) return a;
    const float t = Clamp01(Vector3::Dot(p - a, ab) / len2);
    return a + ab * t;
}

inline void PlayerBossBlockComponent::CollectLimbs(GameObject& boss)
{
    m_limbs.clear();
    // 脚は 4 本とも 3 リンク。膝下だけでなく腿も入れる ─ 腿を抜くと
    // «脚の間から胴の下へ潜り込める» が残る。
    static constexpr const char* kSuffix[4] = { "_FR", "_FL", "_BR", "_BL" };
    static constexpr const char* kChain[4]  = { "Thigh", "Shin", "Hock", "Foot" };
    for (const char* suffix : kSuffix) {
        GameObject* prev = nullptr;
        for (const char* link : kChain) {
            GameObject* node = FindInSubtree(boss, std::string(link) + suffix);
            if (prev && node)
                m_limbs.push_back({ EntityRef{ prev->GetID() }, EntityRef{ node->GetID() },
                                    std::max(legRadius, 0.01f) });
            prev = node;
        }
    }
    // 胴。腹から尻までを 1 本の太い線分にする。
    GameObject* body = FindInSubtree(boss, "Body");
    GameObject* rear = FindInSubtree(boss, "Rear");
    if (body)
        m_limbs.push_back({ EntityRef{ body->GetID() },
                            EntityRef{ (rear ? rear : body)->GetID() },
                            std::max(bodyRadius, 0.01f) });
}

inline bool PlayerBossBlockComponent::HoldOnBack(GameObject& boss)
{
    if (!standOnBack) return false;
    GameObject* anchor = FindInSubtree(boss, deckAnchorBone);
    if (!anchor) return false;

    const Vector3 up      = boss.transform.up;
    const Vector3 surface = anchor->transform.worldPosition + up * std::max(deckRise, 0.0f);

    Vector3 offset = transform.worldPosition - surface;
    // 面からの «高さ» と «横のずれ» に分ける。転倒で背が傾いても面に沿う。
    const float height = Vector3::Dot(offset, up);
    Vector3     flat   = offset - up * height;
    if (flat.Length() > std::max(deckRadius, 0.05f)) return false;

    // 面より上に居るなら何もしない ─ 落ちてくる途中を掴むと «空中で止まる» になる。
    // 下に居ても deckGrip より深ければ、甲羅の «中» なので拾わない。
    if (height > 0.0f || height < -std::max(deckGrip, 0.05f)) return false;

    Place(transform.worldPosition - up * height);

    // 面に乗せたのに落下速度が残っていると、次のステップでまた沈んで «震える»。
    if (auto* rb = scene.GetComponent<RigidBodyComponent>();
        rb && rb->enabled && rb->rigidBody) {
        Vector3 velocity = rb->rigidBody->GetVelocity();
        const float into = Vector3::Dot(velocity, up);
        if (into < 0.0f) rb->rigidBody->SetVelocity(velocity - up * into);
    }
    return true;
}

inline void PlayerBossBlockComponent::OnUpdate()
{
    debugBlocked = 0;
    debugOnBack  = false;

    GameObject* boss = Boss();
    if (!boss) { m_limbs.clear(); return; }
    m_boss = EntityRef{ boss->GetID() };

    const auto* climb = scene.GetScript<PlayerClimbComponent>();

    // 登っている間は押し出さない。
    // WHY 切るか: 登攀は経路の上へ座標を置く手で、そこへ «脚から離れろ» を足すと
    //     脚の表面を這うはずのプレイヤーが毎フレーム外へ弾かれる。
    if (climb && climb->IsClimbing()) { m_limbs.clear(); return; }

    // 背に乗っている間は «乗せる» だけ。脚の押し出しは掛けない ── 甲板の真下に
    // 脚があるので、掛けると立っているだけで横へ滑り出す。
    if (climb && climb->IsOnDeck()) {
        m_limbs.clear();
        debugOnBack = HoldOnBack(*boss);
        return;
    }
    debugOnBack = HoldOnBack(*boss);

    // 骨の «探索» は高いが、位置を読むのは安い。探し直しは間隔を空ける。
    m_probeCooldown -= std::max(Time::deltaTime, 0.0f);
    if (m_limbs.empty() || m_probeCooldown <= 0.0f) {
        m_probeCooldown = kProbeInterval;
        CollectLimbs(*boss);
    }
    if (m_limbs.empty()) return;

    Vector3     pos  = transform.worldPosition;
    const float step = std::max(pushPerSecond, 0.1f) * std::max(Time::deltaTime, 0.0f);
    const float pr   = std::max(playerRadius, 0.01f);
    const Vector3 center = boss->transform.worldPosition;

    for (const Limb& limb : m_limbs) {
        GameObject* ga = limb.a.Resolve(scene);
        GameObject* gb = limb.b.Resolve(scene);
        if (!ga || !gb) continue;

        // 水平だけで測る。垂直を入れると、脚をまたいだ瞬間に上へ突き上げられる。
        const Vector3 hit = ClosestOnSegment(ga->transform.worldPosition,
                                             gb->transform.worldPosition, pos);
        Vector3 delta = pos - hit;
        delta.y = 0.0f;
        const float want = limb.radius + pr + std::max(skin, 0.0f);
        const float dist = delta.Length();
        if (dist >= want) continue;

        // 真上・真下から重なったときは向きが定まらない。ボスの中心から外へ逃がす。
        Vector3 out;
        if (dist > EPSILON) {
            out = delta / dist;
        } else {
            Vector3 away = pos - center;
            away.y = 0.0f;
            out = away.NormalizedOr(Vector3::FORWARD);
        }

        pos += out * std::min(want - dist, step);
        ++debugBlocked;
    }

    if (debugBlocked > 0) Place(pos);
}

inline void PlayerBossBlockComponent::OnDrawGizmos()
{
    if (!drawShapes) return;
    const Vector4 color{ 1.0f, 0.55f, 0.15f, 1.0f };
    for (const Limb& limb : m_limbs) {
        GameObject* ga = limb.a.Resolve(scene);
        GameObject* gb = limb.b.Resolve(scene);
        if (!ga || !gb) continue;
        const Vector3 a = ga->transform.worldPosition;
        const Vector3 b = gb->transform.worldPosition;
        debug.DrawLine(a, b, color);
        debug.DrawSphere(a, limb.radius, color);
        debug.DrawSphere(b, limb.radius, color);
    }
}

} // namespace sandbox
