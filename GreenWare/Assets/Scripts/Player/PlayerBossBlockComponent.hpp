/// @file    PlayerBossBlockComponent.hpp
/// @brief   ボスの脚と胴にプレイヤーがぶつかる。押されるのは **プレイヤーだけ**
/// @author  Hasegawa Jin
/// @date    2026-09-08
///
/// WHY 物理のコライダーでやらないか:
///   最初はボスの当たり (BossHitboxRigComponent) を isTrigger=false にして塞いだが、
///   **ボスが吹き飛んだ。**このエンジンには衝突レイヤーのマトリクスが無く
///   (ProjectSettings の [layers] はタグで、コライダー側に layerMask が無い)、
///   ボス配下に置いた solid はボス自身の胴カプセルと RigidBody (mass 400) に必ず当たる。
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

    FBZZ_GROUP("デバッグ")
    FBZZ_FIELD(bool, drawShapes, false, "形を描く")
    FBZZ_FIELD_READ_ONLY(int, debugBlocked, 0, "押し出した本数")

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

inline void PlayerBossBlockComponent::OnUpdate()
{
    debugBlocked = 0;

    GameObject* boss = Boss();
    if (!boss) { m_limbs.clear(); return; }
    m_boss = EntityRef{ boss->GetID() };

    // 登っている / 背に乗っている間は押し出さない。
    // WHY 切るか: 登攀は経路の上へ座標を置く手で、そこへ «脚から離れろ» を足すと
    //     脚の表面を這うはずのプレイヤーが毎フレーム外へ弾かれる。
    if (const auto* climb = scene.GetScript<PlayerClimbComponent>())
        if (climb->IsClimbing() || climb->IsOnDeck()) { m_limbs.clear(); return; }

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

    if (debugBlocked > 0) transform.worldPosition = pos;
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
