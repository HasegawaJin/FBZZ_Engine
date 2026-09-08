/// @file    BossHitboxRigComponent.hpp
/// @brief   ボスの当たり判定をボーン階層から組み立てる。移動用コライダーとは別系統
/// @author  Hasegawa Jin
/// @date    2026-08-26
///
/// WHY 移動用と判定用を分けるか:
///   ルートのカプセルは «ボスが床に立ち、壁を抜けない» ための体で、形が雑でも困らない。
///   一方 «どこに当たったか» は脚の 1 本ずつが要る。1 つのコライダーで兼ねると、
///   胴体を包む大きさにするしかなく、脚の間を通り抜けたはずのものが当たる。
///   逆に脚に合わせて細くすると、今度はボスが床をすり抜ける。要求が正反対なので分ける。
///
/// WHY ボーンから組み立てるか (シーンに手で置かないか):
///   ボーンは 59 本あり、脚だけで 4 本 × 4 節。手で置くと «右前脚だけ半径が違う» が
///   必ず混ざるうえ、リグを書き出し直すたびに置き直しになる。骨と骨の間隔から寸法を
///   出せば、リグが変わってもそのまま追従する。
///
/// WHY トリガーにするか:
///   衝突応答を持たせると、脚 1 本ずつがプレイヤーを押し返す体になる。全高 6m の相手の
///   脚の間へ潜り込むのが とどめ の間合いなので (Docs/break-parry.md)、そこで押されると
///   寄れない。ボスを押し返すのはルートのカプセル 1 個だけ、という役割分担を崩さない。
///
/// WHY ランタイム生成か:
///   生成物は runtimeGenerated を立てるのでシーンには保存されない。Play のたびに
///   その時点のリグから組み直るため、«シーンに古い当たり判定が焼き付いたまま» が起きない。
#pragma once

#include <Engine/Scene/Components/ColliderComponent.hpp>
#include <Engine/Scene/EntityRef.hpp>
#include <Engine/Scene/GameObject.hpp>
// WHY Scene.hpp まで要るか: AddComponent / GetScript の template 本体は Scene.hpp の
//     末尾にある (GameObject.hpp では Scene が前方宣言しかされていない)。
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Scripts/Combat/BossAnimParams.hpp>
#include <Scripts/Utils/WeaponSockets.hpp>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace fbzz::scene;
using namespace fbzz::math;

namespace sandbox {

class BossHitboxRigComponent : public Script {
    FBZZ_SCRIPT(BossHitboxRigComponent)

public:
    FBZZ_GROUP("半径")
    FBZZ_TOOLTIP("節の長さはボーン間隔から出す。ここで決めるのは太さだけ")
    FBZZ_FIELD_RANGE(float, torsoRadius, 1.90f, "Torso", 0.05f, 6.0f)
    FBZZ_FIELD_RANGE(float, headRadius, 0.70f, "頭", 0.05f, 4.0f)
    FBZZ_FIELD_RANGE(float, rearRadius, 0.95f, "Rear", 0.05f, 4.0f)
    FBZZ_FIELD_RANGE(float, coreRadius, 0.55f, "Core", 0.05f, 4.0f)
    FBZZ_FIELD_RANGE(float, legRadius, 0.38f, "脚", 0.05f, 3.0f)
    FBZZ_FIELD_RANGE(float, radiusScale, 1.0f, "全体スケール", 0.1f, 3.0f)
    FBZZ_TOOLTIP("全体の太さ。個別の比率を保ったまま «当たりの甘さ» だけを動かす")

    FBZZ_GROUP("デバッグ")
    FBZZ_FIELD_READ_ONLY(int, debugHitboxes, 0, "ヒットボックス")
    FBZZ_FIELD_READ_ONLY(int, debugMissingBones, 0, "見つからないボーン")

    void OnStart() override;

    /// 踏みつける脚の «足» ボーン。AI が着弾点を実座標で取るために使う。
    [[nodiscard]] GameObject* FootBone(BossLeg leg) const;
    /// 吊りコア。弱点表示や VFX の発生点に使う。
    [[nodiscard]] GameObject* CoreBone() const { return m_core.Resolve(scene); }

    /// 当たったオブジェクトからボス本体を引く。
    /// WHY 親を遡るか: 当たるのは脚に生えた子オブジェクトで、HP もスクリプトもルートにある。
    ///     ヒットボックスの側に持ち主を書き込むと、階層を組み替えるたびに貼り直しになる。
    [[nodiscard]] static GameObject* BossRootOf(GameObject* hit);

private:
    /// ボーン 1 本ぶんの当たり。to が空なら bone の位置に球を置く。
    struct Segment {
        std::string from;
        std::string to;      // 空なら球
        float       radius = 0.3f;
        /// とどめ の的になる部位か (Docs/break-parry.md)。
        ///
        /// WHY 脚の «下» だけか: プレイヤーの射程は 2.6m でボスは全高 6m ある。
        ///     Thigh は届かない高さにあるので、的として置くと «狙っているのに
        ///     届かない» になる。届くのは膝から下だけ。
        bool        severable = false;
        /// 脚の接尾辞 ("_FR" など)。とどめ の的だけが持つ。
        std::string suffix;
    };

    void Build();
    /// from → to の間にカプセルを 1 本張る。to が空なら球。
    /// @ret 作れたら true。ボーンが見つからなければ false。
    bool BuildSegment(const Segment& segment);
    /// ローカル +Y を direction へ向ける回転。
    [[nodiscard]] static Quaternion AlignUpTo(const Vector3& direction);

    EntityRef m_feet[4];
    EntityRef m_core;
};

FBZZ_REFLECT(BossHitboxRigComponent)


inline GameObject* BossHitboxRigComponent::BossRootOf(GameObject* hit)
{
    for (GameObject* go = hit; go; go = go->GetParent())
        if (go->GetScript<BossHitboxRigComponent>()) return go;
    return nullptr;
}

inline GameObject* BossHitboxRigComponent::FootBone(BossLeg leg) const
{
    const int index = std::clamp(static_cast<int>(leg), 0, 3);
    return m_feet[index].Resolve(scene);
}

inline Quaternion BossHitboxRigComponent::AlignUpTo(const Vector3& direction)
{
    const Vector3 dir = direction.NormalizedOr(Vector3::UP);
    const float   dot = std::clamp(Vector3::Dot(Vector3::UP, dir), -1.0f, 1.0f);

    // 真上ならそのまま。真下は軸が定まらないので X 軸まわりに 180 度倒す。
    if (dot > 0.9999f)  return Quaternion::Identity();
    if (dot < -0.9999f) return Quaternion::FromAxisAngle(Vector3::RIGHT, PI);

    const Vector3 axis = Vector3::Cross(Vector3::UP, dir).NormalizedOr(Vector3::RIGHT);
    return Quaternion::FromAxisAngle(axis, std::acos(dot));
}

inline void BossHitboxRigComponent::OnStart()
{
    m_core = {};
    for (EntityRef& foot : m_feet) foot = {};
    debugHitboxes     = 0;
    debugMissingBones = 0;

    if (!scene.Self()) return;
    Build();
}

inline void BossHitboxRigComponent::Build()
{
    const float scale = std::max(radiusScale, 0.01f);

    std::vector<Segment> segments;
    segments.push_back({ "Body", "",       torsoRadius * scale });
    segments.push_back({ "Head", "",       headRadius  * scale });
    segments.push_back({ "Rear", "",       rearRadius  * scale });
    segments.push_back({ "Core", "Muzzle", coreRadius  * scale });

    // 脚は 4 本とも同じ骨並び。README のリグ構成 (Thigh → Shin → Hock → Foot) に従う。
    static constexpr const char* kLegSuffix[4] = { "_FR", "_FL", "_BR", "_BL" };
    for (const char* suffix : kLegSuffix) {
        segments.push_back({ std::string("Thigh") + suffix, std::string("Shin") + suffix,
                             legRadius * scale });
        segments.push_back({ std::string("Shin") + suffix,  std::string("Hock") + suffix,
                             legRadius * scale });
        segments.push_back({ std::string("Hock") + suffix,  std::string("Foot") + suffix,
                             legRadius * scale, true, suffix });
    }

    for (const Segment& segment : segments) {
        if (BuildSegment(segment)) ++debugHitboxes;
        else                       ++debugMissingBones;
    }

    // 足とコアは AI と演出が名指しで使う。GameObject を作り終えてから引く
    // (scene.Create は GameObject 配列を再確保するため、生成前に掴んだポインタは無効)。
    if (GameObject* self = scene.Self()) {
        for (int i = 0; i < 4; ++i) {
            const std::string name = std::string("Foot") + kLegSuffix[i];
            if (GameObject* bone = FindInSubtree(*self, name))
                m_feet[i] = EntityRef{ bone->GetID() };
        }
        if (GameObject* core = FindInSubtree(*self, "Core"))
            m_core = EntityRef{ core->GetID() };
    }

    if (debugMissingBones > 0) {
        // 綴りが違うと «その部位だけ当たらない» という形でしか出ない。名指しで言う。
        debug.LogError("BossHitboxRigComponent could not find " +
                       std::to_string(debugMissingBones) +
                       " bone(s). Check the rig names against Assets/Models/Boss/README.md.");
    }
}

inline bool BossHitboxRigComponent::BuildSegment(const Segment& segment)
{
    // 生成のたびに引き直す。scene.Create が GameObject 配列を再確保するので、
    // ループの外で掴んだポインタは 2 本目以降で無効になる。
    GameObject* self = scene.Self();
    if (!self || !FindInSubtree(*self, segment.from)) return false;

    const float radius = std::max(segment.radius, 0.01f);

    // カプセルの向きと長さは «次の骨がどこにあるか» から出す。
    // WHY ローカル座標を使うか: ワールド姿勢は TransformSystem が回った後でないと
    //     確定しないが、子ボーンのローカル位置は «親から見た骨の長さと向き» そのもので、
    //     開始直後でもシーンに書かれている値がそのまま使える。
    Vector3    center = Vector3::ZERO;
    Quaternion rotation = Quaternion::Identity();
    float      halfHeight = 0.0f;
    bool       capsule = false;

    if (!segment.to.empty()) {
        if (GameObject* tip = FindInSubtree(*self, segment.to)) {
            const Vector3 offset = tip->transform.position;
            const float   length = offset.Length();
            if (length > radius * 0.5f) {
                center     = offset * 0.5f;
                rotation   = AlignUpTo(offset);
                halfHeight = std::max(length * 0.5f - radius, 0.01f);
                capsule    = true;
            }
        }
    }

    GameObject& hitbox = scene.Create("HB_" + segment.from);
    // シーンには保存しない。Play のたびにその時点のリグから組み直す。
    hitbox.runtimeGenerated = true;
    // 部位はボス本体と同じ «敵» として扱わせる。Mite の接地レイキャストは
    // タグで敵を捨てているので、これが無いとボスの脚を地面と読んで脚の上に浮く。
    hitbox.tag = "Enemy";

    // 親を引き直してから繋ぐ。ローカル姿勢は «繋いだ後» に書く
    // (SetParent がワールド姿勢を保つ実装でも、後から書けば必ずこちらが勝つ)。
    GameObject* owner = scene.Self();
    if (GameObject* parent = owner ? FindInSubtree(*owner, segment.from) : nullptr)
        hitbox.SetParent(*parent);
    hitbox.transform.position = center;
    hitbox.transform.rotation = rotation;

    if (capsule) {
        auto& collider = hitbox.AddComponent<CapsuleColliderComponent>();
        collider.SetCapsule(radius, halfHeight);
        collider.isTrigger = true;
    } else {
        auto& collider = hitbox.AddComponent<SphereColliderComponent>();
        collider.SetRadius(radius);
        collider.isTrigger = true;
    }

    // WHY 的の印を «名前» で済ませるか: とどめ が通るのは膝下 (`HB_Hock_*`) だけで、
    //     その 4 つは骨の名前から決まっている。印のためだけにスクリプトを 1 枚
    //     足すと、«付け忘れた脚だけ とどめ が入らない» という壊れ方が増える。
    //     脚を引くのは BossRigComponent::LegSuffixOf。
    (void)segment.severable;

    return true;
}

} // namespace sandbox
