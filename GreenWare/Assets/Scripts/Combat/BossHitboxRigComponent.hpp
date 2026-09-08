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
#include <Scripts/Combat/BossPartComponent.hpp>
#include <Scripts/Utils/WeaponSockets.hpp>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

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
    FBZZ_GROUP("耐久")
    FBZZ_FIELD_RANGE_INT(int, legHealth, 150, "脚", 1, 1000)
    FBZZ_TOOLTIP("膝下 1 本を削り切るのに要る量。削り切ると脚が落ちる")
    FBZZ_FIELD_RANGE_INT(int, coreHealth, 90, "コア", 1, 1000)
    FBZZ_TOOLTIP("背のコアを削り切るのに要る量。蓋が開いているあいだしか削れない")
    FBZZ_FIELD_RANGE(float, legRadius, 0.38f, "脚", 0.05f, 3.0f)
    FBZZ_FIELD_RANGE(float, radiusScale, 1.0f, "全体スケール", 0.1f, 3.0f)
    FBZZ_TOOLTIP("全体の太さ。個別の比率を保ったまま «当たりの甘さ» だけを動かす")

    FBZZ_GROUP("デバッグ")
    FBZZ_FIELD_READ_ONLY(int, debugHitboxes, 0, "ヒットボックス")
    FBZZ_FIELD_READ_ONLY(int, debugMissingBones, 0, "見つからないボーン")

    void OnStart()  override;
    void OnUpdate() override;

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
        /// 斬撃で削れる部位にするか (BossPartComponent を付ける)。
        ///
        /// WHY 名前の印 (severable) と分けるか: とどめ が通るのは膝下だけだが、
        ///     «斬って削れる» のは膝下とコアの 2 種類ある。1 つの旗で兼ねると、
        ///     コアへ とどめ が通ってしまうか、コアが斬れないかのどちらかになる。
        bool        damageable = false;
        /// 部位の耐久。0 なら BossPartComponent の既定値のまま。
        int         health = 0;
    };

    void Build();
    /// from → to の間にカプセルを 1 本張る。to が空なら球。
    /// @ret 作れたら true。ボーンが見つからなければ false。
    bool BuildSegment(const Segment& segment);
    /// ローカル +Y を direction へ向ける回転。
    [[nodiscard]] static Quaternion AlignUpTo(const Vector3& direction);

    /// ボーンが揃うまで持ち越すセグメント。
    ///
    /// WHY 1 回で作り切らないか: ボスのボーン GameObject はシーンに保存されず、
    ///     AnimatorSystem が実行時に作る。OnStart の時点ではまだ 1 本も無いことがあり、
    ///     そのときは 15 本すべてが «骨が無い» で落ちて当たり判定が 1 つも生まれない。
    ///     «すり抜けるボス» はこの形でしか出ないので、揃うまで毎フレーム作り直す。
    std::vector<Segment> m_pending;
    /// 揃わないまま経った時間 [秒]。諦めて名指しで言うまでの猶予に使う。
    float     m_waited = 0.0f;
    /// 次に作り直しを試すまでの残り [秒]。
    float     m_retryCooldown = 0.0f;
    bool      m_reported = false;
    static constexpr float kRetryInterval  = 0.20f;
    static constexpr float kGiveUpSeconds  = 6.0f;
    EntityRef m_feet[4];
    EntityRef m_core;
    void ResolveAnchors();
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
    m_pending.clear();
    m_waited          = 0.0f;
    m_retryCooldown   = 0.0f;
    m_reported        = false;
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
    // WHY 球にするか (2026-09-08): 以前は Core→Muzzle のカプセルだった。
    //     コアを背面へ移し Muzzle を Body の子へ付け替えたので、この 2 点を結ぶと
    //     «背中から腹下まで胴体を貫く当たり» になる。コアは背に載った的なので球で足りる。
    // コアは «蓋が開いているあいだだけ» 斬れる的。開閉と当たりの有効化は
    // BossHatchComponent が握る (ここでは作るだけで、閉じている間は畳まれる)。
    segments.push_back({ "Core", "",       coreRadius  * scale, false, "", true, coreHealth });

    // 脚は 4 本とも同じ骨並び。README のリグ構成 (Thigh → Shin → Hock → Foot) に従う。
    static constexpr const char* kLegSuffix[4] = { "_FR", "_FL", "_BR", "_BL" };
    for (const char* suffix : kLegSuffix) {
        segments.push_back({ std::string("Thigh") + suffix, std::string("Shin") + suffix,
                             legRadius * scale });
        segments.push_back({ std::string("Shin") + suffix,  std::string("Hock") + suffix,
                             legRadius * scale });
        segments.push_back({ std::string("Hock") + suffix,  std::string("Foot") + suffix,
                             legRadius * scale, true, suffix, true, legHealth });
    }

    for (const Segment& segment : segments) {
        if (BuildSegment(segment)) ++debugHitboxes;
        else                       m_pending.push_back(segment);
    }
    debugMissingBones = static_cast<int>(m_pending.size());

    ResolveAnchors();
}

inline void BossHitboxRigComponent::ResolveAnchors()
{
    // 足とコアは AI と演出が名指しで使う。GameObject を作り終えてから引く
    // (scene.Create が GameObject 配列を再確保するため、生成前に掴んだポインタは無効)。
    GameObject* self = scene.Self();
    if (!self) return;
    for (int i = 0; i < 4; ++i) {
        static constexpr const char* kSuffix[4] = { "_FR", "_FL", "_BR", "_BL" };
        if (GameObject* bone = FindInSubtree(*self, std::string("Foot") + kSuffix[i]))
            m_feet[i] = EntityRef{ bone->GetID() };
    }
    if (GameObject* core = FindInSubtree(*self, "Core"))
        m_core = EntityRef{ core->GetID() };
}

inline void BossHitboxRigComponent::OnUpdate()
{
    if (m_pending.empty()) return;

    // WHY 毎フレーム試さないか (2026-09-08): BuildSegment は 1 本ごとにボスの全サブツリーを
    //     再帰で歩く。15 本が保留のままだと毎フレーム 15 回歩くことになり、
    //     «ボーンがまだ無い» という正常な状態のあいだ中ずっと重くなる。
    //     ボーンが出来るのを数百 ms 待っても、当たりが遅れて生えるだけで害は無い。
    m_waited += Time::deltaTime;
    m_retryCooldown -= Time::deltaTime;
    if (m_retryCooldown > 0.0f) return;
    m_retryCooldown = kRetryInterval;

    // 揃った物から順に生かす。1 フレームで全部揃う保証は無い
    // (AnimatorSystem がボーンを作る順はこちらから見えない)。
    for (std::size_t i = m_pending.size(); i-- > 0; ) {
        if (!BuildSegment(m_pending[i])) continue;
        ++debugHitboxes;
        m_pending.erase(m_pending.begin() + static_cast<std::ptrdiff_t>(i));
    }
    debugMissingBones = static_cast<int>(m_pending.size());

    if (m_pending.empty()) { ResolveAnchors(); return; }

    // 綴り違いは «その部位だけ当たらない» という形でしか出ない。名指しで言う。
    // WHY すぐ言わないか: 起動直後はまだボーンが無いのが正常で、そこで出すと
    //     毎回エラーが出て «本当に綴りが違うとき» を見分けられなくなる。
    if (m_waited > kGiveUpSeconds && !m_reported) {
        m_reported = true;
        debug.LogError("BossHitboxRigComponent could not find " +
                       std::to_string(m_pending.size()) +
                       " bone(s) after " + std::to_string(static_cast<int>(kGiveUpSeconds)) +
                       "s. Check the rig names against Assets/Models/Boss_01/README.md.");
        // WHY 諦めるか: ここまで来たら «まだ出来ていない» ではなく «名前が違う»。
        //     待ち続けても永遠に見つからず、探索の負荷だけが残る。
        m_pending.clear();
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

    // 斬撃の扇は BossPartComponent を名指しで探す。付いていない部位は
    // «斬っても何も起きない» になるので、削れる部位はここで必ず宣言する。
    //
    // WHY とどめ の的 (severable) は名前で判るのにスクリプトが要るか:
    //     とどめ が «通るか» は名前 (`HB_Hock_*`) で足りるが、«削れるか» は
    //     耐久という状態を持つので、置き場所がどうしても要る。
    if (segment.damageable) {
        auto& part = hitbox.AddScript<BossPartComponent>();
        part.legSuffix = segment.suffix;
        // 扇の判定はレンダラーを持たない部位に対して «本人の申告» を使う。
        part.hitRadius = radius;
        if (segment.health > 0) part.maxHealth = segment.health;
    }

    return true;
}

} // namespace sandbox
