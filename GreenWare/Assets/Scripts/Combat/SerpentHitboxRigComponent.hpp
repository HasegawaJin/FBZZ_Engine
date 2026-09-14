/// @file    SerpentHitboxRigComponent.hpp
/// @brief   蛇の当たり判定を骨から組み立て、節を名指しで引けるようにする。付ける先は Boss02
/// @author  Hasegawa Jin
/// @date    2026-08-31
///
/// WHY ボス 1 の BossHitboxRigComponent を使い回さないか:
///   あちらは «胴 + 脚 4 本 × 4 節» という固定の並びを直に書いた表で、脚の接尾辞
///   ("_FR") が判定・IK・輪郭の全部を貫いている。蛇は 29 本が同じ規則で並ぶ 1 本の鎖で、
///   共有できるのは «骨と骨の間にカプセルを張る» という 10 行だけ。そこを基底へ
///   引き上げると、四足の都合 (脚・足・コア) が鎖の側へ降りてくる。
///
/// WHY ランタイム生成か:
///   生成物は runtimeGenerated を立てるのでシーンには保存されない。Play のたびに
///   その時点のリグから組み直るため、«シーンに古い当たり判定が焼き付いたまま» が起きない。
///
/// WHY トリガーにするか:
///   衝突応答を持たせると、当たり判定が床の口や羽を押し返して蛇が経路から外れる。
///   蛇は経路に沿って «置かれる» もので、物理で動いていない。
#pragma once

#include <Engine/Scene/Components/ColliderComponent.hpp>
#include <Engine/Scene/EntityRef.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Scripts/Utils/RagdollPresentation.hpp>
#include <Scripts/Combat/BossPartComponent.hpp>
#include <Scripts/Combat/SerpentBones.hpp>
#include <Scripts/Utils/WeaponSockets.hpp>
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

using namespace fbzz::scene;
using namespace fbzz::math;

namespace sandbox {

class SerpentHitboxRigComponent : public Script {
    FBZZ_SCRIPT(SerpentHitboxRigComponent)

public:
    FBZZ_GROUP("半径")
    FBZZ_TOOLTIP("節の長さは骨の間隔から出す。ここで決めるのは太さだけ")
    FBZZ_FIELD_RANGE(float, headRadius, 0.72f, "頭", 0.05f, 3.0f)
    FBZZ_FIELD_RANGE(float, neckRadius, 0.48f, "Neck (S01)", 0.05f, 3.0f)
    FBZZ_FIELD_RANGE(float, peakRadius, 0.62f, "Peak (S06)", 0.05f, 3.0f)
    FBZZ_TOOLTIP("一番太い所。モデルは頭の後ろ (S06〜S07) が直径 1.24 m")
    FBZZ_FIELD_RANGE(float, tailRadius, 0.12f, "Tail (S28)", 0.02f, 3.0f)
    FBZZ_FIELD_RANGE(float, radiusScale, 1.0f, "全体スケール", 0.1f, 3.0f)

    FBZZ_GROUP("部位")
    FBZZ_FIELD_RANGE_INT(int, segmentHealth, 45, "節の耐久", 1, 1000)

    FBZZ_GROUP("Rest Lengths")
    FBZZ_FIELD_RANGE(float, fallbackSegment, 0.80f, "Segment", 0.05f, 4.0f)
    FBZZ_TOOLTIP("骨から測れなかったときに使う節の長さ。実測できていれば使われない")
    FBZZ_FIELD_RANGE(float, headLength, 1.90f, "Head Length", 0.05f, 6.0f)
    FBZZ_TOOLTIP("頭の当たりの長さ。頭には子の骨が無いので測れない")

    FBZZ_GROUP("デバッグ")
    FBZZ_FIELD_READ_ONLY(int, debugHitboxes, 0, "ヒットボックス")
    FBZZ_FIELD_READ_ONLY(int, debugMissingBones, 0, "見つからないボーン")
    FBZZ_FIELD(bool, drawHitboxes, false, "Draw Hitboxes")

    /// 頭からの添字 (0 = Head / 1..28 = Seg01..Seg28) のボーン。
    [[nodiscard]] GameObject* SegmentBone(int headIndex) const;
    /// その節の当たり判定オブジェクト。
    [[nodiscard]] GameObject* SegmentHitbox(int headIndex) const;
    /// 当たったオブジェクトから節の番号を引く。読めなければ -1。
    [[nodiscard]] int SegmentOf(GameObject* hit) const;
    /// 当たったオブジェクトから蛇の本体を引く。
    [[nodiscard]] static GameObject* SerpentRootOf(GameObject* hit);

    /// 骨と骨の間隔 (静止時の実測)。頭からの添字 i の «次の骨へ» の長さ。
    [[nodiscard]] float LinkLength(int headIndex) const;
    /// 骨がすべて揃っているか。1 本でも欠けると胴が途中で切れる。
    [[nodiscard]] bool IsBuilt() const { return m_built && debugMissingBones == 0; }

    void OnStart() override;
    void ReactToHit(const Vector3& point, const Vector3& direction, bool heavy)
    {
        if (m_reactionFrame == fbzz::Time::frameCount) return;
        m_reactionFrame = fbzz::Time::frameCount;
        ragdoll.BeginActive();
        PushRagdollReaction(ragdoll, direction.NormalizedOr(Vector3::FORWARD) *
            (heavy ? 2.2f : 1.0f), 2.0f, point, 1.8f);
    }
    void OnUpdate() override;

private:
    void Build();
    /// 節 1 本ぶんのカプセルを張る。@ret 作れたら true。
    bool BuildSegment(int headIndex);
    /// 骨のローカル位置から «次の骨まで» の長さを測る。
    ///
    /// WHY 定数で持たないか: モデルを割り直したときに黙ってずれる。骨の間隔は
    ///     書き出しの結果そのものなので、そこから読めば必ず一致する。
    void MeasureLinks();

    /// 骨と当たりの参照。DLL リロードで空へ戻るので «空なら引き直す» で書く。
    EntityRef m_bones[serpent::kBoneCount];
    EntityRef m_hitboxes[serpent::kBoneCount];
    float     m_links[serpent::kBoneCount]{};
    bool      m_built = false;
    std::uint64_t m_reactionFrame = ~std::uint64_t{0};
};

FBZZ_REFLECT(SerpentHitboxRigComponent)

inline GameObject* SerpentHitboxRigComponent::SerpentRootOf(GameObject* hit)
{
    for (GameObject* go = hit; go; go = go->GetParent())
        if (go->GetScript<SerpentHitboxRigComponent>()) return go;
    return nullptr;
}

inline GameObject* SerpentHitboxRigComponent::SegmentBone(int headIndex) const
{
    if (headIndex < 0 || headIndex >= serpent::kBoneCount) return nullptr;
    return m_bones[headIndex].Resolve(scene);
}

inline GameObject* SerpentHitboxRigComponent::SegmentHitbox(int headIndex) const
{
    if (headIndex < 0 || headIndex >= serpent::kBoneCount) return nullptr;
    return m_hitboxes[headIndex].Resolve(scene);
}

inline int SerpentHitboxRigComponent::SegmentOf(GameObject* hit) const
{
    // WHY 名前で照合しないか: 当たったのは «とどめ» が斬った物で、節そのものとは
    //     限らない (子を挟むこともある)。生成したときの参照と突き合わせれば、
    //     命名を変えても番号の引き当てだけは外れない。
    for (GameObject* go = hit; go; go = go->GetParent())
        for (int i = 0; i < serpent::kBoneCount; ++i)
            if (SegmentHitbox(i) == go) return i;
    return -1;
}

inline float SerpentHitboxRigComponent::LinkLength(int headIndex) const
{
    if (headIndex < 0 || headIndex >= serpent::kBoneCount) return fallbackSegment;
    return m_links[headIndex];
}

inline void SerpentHitboxRigComponent::MeasureLinks()
{
    GameObject* self = scene.Self();
    if (!self) return;

    // 骨 i のローカル位置は «親 (骨 i+1) から見た自分» で、そのまま節の長さになる。
    //
    // WHY 測った値をそのまま信じないか: Spine は死んだ節のリンクをローカル位置 0 へ
    //     畳む。Play 中に DLL をリロードしてここが走り直すと、畳んだ 0 を «節の長さ»
    //     として読んでしまう。ありえない値は既定値へ落とす。
    for (int i = 0; i < serpent::kBoneCount; ++i) {
        float measured = 0.0f;
        if (GameObject* bone = SegmentBone(i)) measured = bone->transform.position.Length();
        m_links[i] = (measured > 0.2f && measured < 4.0f) ? measured : Max(fallbackSegment, 0.05f);
    }
}

inline void SerpentHitboxRigComponent::OnStart()
{
    for (EntityRef& bone : m_bones)    bone = {};
    for (EntityRef& hitbox : m_hitboxes) hitbox = {};
    debugHitboxes     = 0;
    debugMissingBones = 0;
    m_built           = false;

    if (!scene.Self()) return;
    Build();
    ragdoll.SetRoot("Seg28");
    ConfigureStandingReaction(ragdoll, fbzz::scene::ScriptRagdollProfile::Mech,
                              0.12f, 8.0f, 8.0f, 0.05f);
    ragdoll.SetMuscle(1.4f, 0.99f, 0.9f);
    ragdoll.SetRecovery(0.2f, 0.4f);
    ragdoll.SetBlend(0.08f, 0.25f);
    ragdoll.BeginActive();
}

inline void SerpentHitboxRigComponent::Build()
{
    debugHitboxes     = 0;
    debugMissingBones = 0;

    // 骨は先に全部掴む。scene.Create は GameObject 配列を再確保するので、
    // 生成を跨いで握ったポインタは無効になる。
    for (int i = 0; i < serpent::kBoneCount; ++i) {
        GameObject* self = scene.Self();
        if (!self) return;
        // 骨名はシーン内で一意でない (プレイヤーにも Head と Root が居る)。
        // 必ず蛇の部分木だけを見る。
        if (GameObject* bone = FindInSubtree(*self, serpent::BoneName(i)))
            m_bones[i] = EntityRef{ bone->GetID() };
        else
            ++debugMissingBones;
    }

    MeasureLinks();

    for (int i = 0; i < serpent::kBoneCount; ++i)
        if (BuildSegment(i)) ++debugHitboxes;

    if (debugMissingBones > 0) {
        // 綴り違いは «その節だけ当たらない» という形でしか出ない。名指しで言う。
        debug.LogError("SerpentHitboxRigComponent could not find " +
                       std::to_string(debugMissingBones) +
                       " bone(s) under the serpent. Expected Head and Seg01..Seg28 "
                       "(see Assets/Docs/boss-serpent.md).");
    }
    m_built = true;
}

inline bool SerpentHitboxRigComponent::BuildSegment(int headIndex)
{
    // 生成のたびに引き直す。ループの外で掴んだ self は 2 本目の Create で無効になる。
    GameObject* bone = SegmentBone(headIndex);
    if (!bone) return false;

    const float radius =
        Max((headIndex == 0 ? headRadius :
             serpent::SegmentRadius(headIndex, neckRadius, peakRadius, tailRadius)) *
                Max(radiusScale, 0.01f),
            0.02f);

    // 節の «長さ» は骨の +Y 側にある。節 i の胴は骨 i から骨 i-1 までで、
    // その長さは骨 i-1 のローカル位置 (= リンク i-1) になる。頭には次の骨が無い。
    const float length = headIndex <= 0 ? Max(headLength, 0.1f)
                                        : Max(m_links[headIndex - 1], 0.1f);

    const std::string name = serpent::HitboxName(headIndex);

    GameObject* hitbox = FindInSubtree(*bone, name);
    if (!hitbox) {
        GameObject& created = scene.Create(name);
        created.runtimeGenerated = true;
        // 部位は蛇本体と同じ «敵» として扱わせる。接地レイキャストがタグで敵を
        // 捨てているので、これが無いと胴を地面と読んで胴の上に浮く。
        created.tag = "Enemy";
        hitbox = &created;

        // 親を引き直してから繋ぐ。Create でポインタが動いている。
        if (GameObject* parent = SegmentBone(headIndex)) hitbox->SetParent(*parent);
    }
    m_hitboxes[headIndex] = EntityRef{ hitbox->GetID() };

    hitbox->transform.position = Vector3{ 0.0f, length * 0.5f, 0.0f };
    hitbox->transform.rotation = Quaternion::Identity();

    auto* collider = hitbox->GetComponent<CapsuleColliderComponent>();
    if (!collider) collider = &hitbox->AddComponent<CapsuleColliderComponent>();
    collider->SetCapsule(radius, Max(length * 0.5f - radius, 0.01f));
    collider->isTrigger = true;

    // 斬撃・照準・とどめは BossPart を列挙する。コライダーだけでは対象にならない。
    // 頭は切断対象でないため、胴の28節だけを登録する。
    if (headIndex > 0) {
        auto* part = hitbox->GetScript<BossPartComponent>();
        if (!part) part = &hitbox->AddScript<BossPartComponent>();
        part->legSuffix = serpent::PartSuffix(headIndex);
        part->hitRadius = radius;
        part->maxHealth = std::max(segmentHealth, 1);
    }
    return true;
}

inline void SerpentHitboxRigComponent::OnUpdate()
{
    // 骨は掴めているが当たりが消えている状態 (DLL リロード直後) を拾い直す。
    //
    // WHY 骨が引けたときだけ組み直すか: 骨名が違うリグを差し替えると当たりは
    //     永遠に作れない。«作れないから毎フレーム作り直す» にすると、
    //     Build() のエラーがログを埋めて本当の原因が読めなくなる。
    if (!m_built || (m_bones[0].Resolve(scene) && !m_hitboxes[0].Resolve(scene))) {
        Build();
        return;
    }

    if (!drawHitboxes) return;
    for (int i = 0; i < serpent::kBoneCount; ++i) {
        GameObject* hitbox = SegmentHitbox(i);
        if (!hitbox || !hitbox->activeInHierarchy()) continue;
        const auto* collider = hitbox->GetComponent<CapsuleColliderComponent>();
        if (!collider) continue;
        debug.DrawSphere(hitbox->transform.worldPosition, collider->radius,
                         Vector4{ 0.3f, 0.9f, 0.5f, 1.0f });
    }
}

} // namespace sandbox
