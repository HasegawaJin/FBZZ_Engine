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
    // WHY 既定が false のままか (2026-09-10):
    //     衝突レイヤーの行列を物理へ通したので、原理上は «自分の胴とは当たらないが
    //     プレイヤーとは当たる» が作れる。だが**成立の条件が別ファイルにある**
    //     (ProjectSettings.toml の [physics] ignoreCollisions) のに、そのファイルは
    //     エディタが開いていると古い設定で上書き保存されて消える。
    //     消えた瞬間の壊れ方が «ボスが吹き飛ぶ» ── 一番重い部類なので、
    //     既定は «行列を要らない側» に置く。
    //     すり抜けを止めるのは PlayerBossBlockComponent (プレイヤーだけを押し出す)。
    //     on にするなら、行列を確かめてからにすること (SolidAllowed が毎フレーム見る)。
    FBZZ_FIELD(bool, solidLegs, false, "脚と胴でぶつかる (要・衝突行列)")
    FBZZ_TOOLTIP("プレイヤーが 6m の重機をすり抜けないようにする物理の当たり。"
                 "ProjectSettings > Physics の衝突行列で «当たりのレイヤー × ボス本体» と "
                 "«自分自身» を切っていないと自己衝突でボスが吹き飛ぶ。"
                 "行列が無い間は自動でトリガーへ畳まれ、ログに何を設定すべきか出る。"
                 "off のままなら PlayerBossBlockComponent がすり抜けを止める")
    FBZZ_FIELD_RANGE(float, radiusScale, 1.0f, "全体スケール", 0.1f, 3.0f)
    FBZZ_TOOLTIP("全体の太さ。個別の比率を保ったまま «当たりの甘さ» だけを動かす")

    FBZZ_GROUP("レイヤー")
    // WHY 当たりだけ別レイヤーへ置くか: ボス本体 (胴カプセル + RigidBody) と、
    //     骨に生やした当たりは «同じ物体の 2 つの表現» で、互いにぶつかってはいけない。
    //     行列で切れるのはレイヤーの組なので、分けておかないと切りようが無い。
    FBZZ_FIELD_RANGE_INT(int, hitboxLayer, 9, "当たりのレイヤー", 0, 31)
    FBZZ_TOOLTIP("生成する当たりに入れるレイヤー番号。ProjectSettings > Physics の "
                 "衝突行列で «このレイヤー × ボス本体のレイヤー» と «自分自身» を "
                 "切っておくこと。切らないとボスが自分の脚に押されて吹き飛ぶ")

    // WHY 甲板だけ別扱いか: 胴の球 (torsoRadius) の «上» に立たせると、球面なので
    //     必ず滑り落ちる。登った先は立って戦う場所なので、平らな面が要る。
    //     骨の子にしないのは、Blender のボーンはローカル +Y が骨の向きで、
    //     どちら向きに «上» があるかがボーンごとに違うから。ボスのルート姿勢
    //     (PlayerClimbComponent が着地点を測るのと同じ基準) から毎フレーム置く。
    FBZZ_GROUP("甲板")
    // WHY 甲板だけ物理の当たりでよいか (2026-09-10):
    //     箱は `attachToParentBody` を立ててボス本体の剛体へ属させる。同じ剛体の
    //     コライダー同士は当たらないので (PhysicsSolver の同一ボディ除外)、
    //     胴のカプセルと重なっても押し合わない ── 衝突行列が要らない。
    //     脚を同じ手で固くできないのは、**姿勢が剛体のものになる**から
    //     (骨と一緒に回る当たりはこの経路では正しく回らない) と、
    //     脚は地面に届くので «自分の脚で床を押して浮く» が起きるため。
    FBZZ_FIELD(bool, buildDeck, true, "甲板の足場を作る")
    FBZZ_TOOLTIP("背に立てる平らな当たり。ボス本体の剛体へ属させるので自己衝突しない。"
                 "off にすると登っても足場が無く、甲羅をすり抜けて落ちる")
    FBZZ_FIELD(std::string, deckAnchorBone, "Body", "基準の骨")
    FBZZ_TOOLTIP("PlayerClimbComponent の «基準の骨» と必ず同じにする。"
                 "食い違うと «登り着いた点» と «足場» が別の高さになる")
    FBZZ_FIELD_RANGE(float, deckRise, 1.33f, "甲板の高さ", 0.0f, 6.0f)
    FBZZ_TOOLTIP("基準の骨から足場の «上面» までの高さ [m]。"
                 "PlayerClimbComponent の同名の値と揃える")
    FBZZ_FIELD_RANGE(float, deckHalfWidth, 1.60f, "甲板の広さ (半分)", 0.2f, 5.0f)
    FBZZ_TOOLTIP("足場の半分の幅 [m]。襟の半径 1.34 より少し広くとって、"
                 "縁に立ったときに落ちないようにする")
    FBZZ_FIELD_RANGE(float, deckThickness, 0.60f, "甲板の厚み", 0.05f, 3.0f)
    FBZZ_TOOLTIP("足場の厚み [m]。薄いと高速で降りてきたときにすり抜ける")

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
        /// プレイヤーがぶつかる «固い» 当たりにするか。
        ///
        /// WHY 部位ごとに分けるか: 脚は «ぶつかって回り込む» 対象で、塞がないと
        ///     6m の重機の中をすり抜けて歩けてしまう。一方でコアは甲板に立った
        ///     プレイヤーが斬る的なので、固いと自分の足元に壁が生えることになる。
        ///     頭は届かない高さにあり、塞ぐ意味が無いうえ空中で引っ掛かる元になる。
        bool        blocking = false;
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
    /// 衝突行列の不備を 1 度だけ言うための旗。毎フレーム言うとログが埋まる。
    bool      m_solidWarned = false;
    static constexpr float kRetryInterval  = 0.20f;
    static constexpr float kGiveUpSeconds  = 6.0f;
    EntityRef m_feet[4];
    EntityRef m_core;
    void ResolveAnchors();

    /// 当たりを «固く» してよいか。衝突行列が前提を満たしていなければ false。
    ///
    /// WHY 毎回確かめるか (2026-09-10): 行列は ProjectSettings.toml に書いてあるだけで、
    ///     エディタが開いていると古い設定で上書き保存されて消えることがある。
    ///     消えたまま固くすると、ボスは自分の当たり (胴の球・甲板の箱・脚 12 本) に
    ///     押されて **毎フレーム前へずれ続ける**。速度を 0 にしても止まらない
    ///     ── めり込み解決は位置を直接動かすので、StopHorizontal では効かない。
    ///     しかも «AI が前進させている» ようにしか見えないので、原因に辿り着けない。
    ///     前提が崩れていたら固くせず、代わりに何を設定すべきかを名指しで言う。
    [[nodiscard]] bool SolidAllowed();
    /// «固い» つもりの当たりへ、今の判定を書き込む。
    ///
    /// WHY 作った後にも書き換えられるようにするか: Build が走るのは OnStart で、
    ///     物理ワールドがまだスクリプトへ渡っていない可能性がある。そこで 1 度きり
    ///     決め打ちにすると «ワールドが無かったから安全側» のまま固まる。
    ///     Inspector で solidLegs を触ったときにも Play 中に効く。
    void ApplySolidity(bool solid);
    /// 固くするつもりの当たり (脚・胴・甲板)。頭やコアは常にトリガーなので入れない。
    std::vector<EntityRef> m_solidParts;
    /// 直近に書いた «固さ»。変わったフレームだけ書き直す。
    bool m_solidApplied = false;
    /// 背の «立てる面»。作れたら true。基準の骨がまだ無ければ false。
    bool EnsureDeck();
    /// 甲板をボスのルート姿勢へ合わせて置き直す。毎フレーム。
    void UpdateDeck();
    EntityRef m_deck;
    EntityRef m_deckAnchor;
    /// 基準の骨を探し直すまでの残り [秒]。骨はまだ無いのが正常なので、
    /// 見つからない間ずっと全サブツリーを歩き続けないよう間隔を空ける。
    float     m_deckProbeCooldown = 0.0f;
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
    m_deck = {};
    m_deckAnchor = {};
    m_deckProbeCooldown = 0.0f;
    m_solidParts.clear();
    m_solidWarned  = false;
    m_solidApplied = false;
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
    segments.push_back({ "Body", "",       torsoRadius * scale, false, "", false, 0, solidLegs });
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
                             legRadius * scale, false, "", false, 0, solidLegs });
        segments.push_back({ std::string("Shin") + suffix,  std::string("Hock") + suffix,
                             legRadius * scale, false, "", false, 0, solidLegs });
        segments.push_back({ std::string("Hock") + suffix,  std::string("Foot") + suffix,
                             legRadius * scale, true, suffix, true, legHealth, solidLegs });
    }

    for (const Segment& segment : segments) {
        if (BuildSegment(segment)) ++debugHitboxes;
        else                       m_pending.push_back(segment);
    }
    debugMissingBones = static_cast<int>(m_pending.size());
    // 作った時点の判定を覚えておく。OnUpdate はここから «変わったか» だけを見る。
    m_solidApplied = SolidAllowed();

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

inline bool BossHitboxRigComponent::SolidAllowed()
{
    if (!solidLegs) return false;

    GameObject* self = scene.Self();
    const int mine  = hitboxLayer & 31;
    const int owner = self ? (self->layer & 31) : 0;

    // 自分の剛体 (ボス本体) と、当たりどうし。この 2 組が切れていて初めて固くできる。
    const bool separated = !physics.LayersCollide(mine, owner)
                        && !physics.LayersCollide(mine, mine);
    if (separated) return true;

    if (!m_solidWarned) {
        m_solidWarned = true;
        debug.LogError(
            "BossHitboxRigComponent: layer " + std::to_string(mine) +
            " still collides with the boss body (layer " + std::to_string(owner) +
            ") or with itself, so the hitboxes stay triggers. "
            "Without this the boss is pushed by its own colliders and drifts forward every frame. "
            "Set ProjectSettings > Physics > Layer Collision Matrix to uncheck "
            + std::to_string(mine) + "x" + std::to_string(owner) + " and "
            + std::to_string(mine) + "x" + std::to_string(mine) +
            " (ProjectSettings.toml: [physics] ignoreCollisions).");
    }
    return false;
}

inline bool BossHitboxRigComponent::EnsureDeck()
{
    if (GameObject* deck = m_deck.Resolve(scene)) {
        // 走っている最中に off にされたら畳む。作り直しは Stop → Play で足りる。
        if (!buildDeck) { deck->SetActive(false); return false; }
        deck->SetActive(true);
        return true;
    }
    if (!buildDeck) return false;

    m_deckProbeCooldown -= std::max(Time::deltaTime, 0.0f);
    if (m_deckProbeCooldown > 0.0f) return false;
    m_deckProbeCooldown = kRetryInterval;

    GameObject* self = scene.Self();
    if (!self) return false;
    GameObject* anchor = FindInSubtree(*self, deckAnchorBone);
    if (!anchor) return false;
    m_deckAnchor = EntityRef{ anchor->GetID() };

    GameObject& deck = scene.Create("HB_Deck");
    deck.runtimeGenerated = true;
    deck.tag   = "Enemy";
    deck.layer = hitboxLayer & 31;
    // 親はボスのルート。骨の子にすると、骨のローカル軸に «上» が乗ってしまう。
    //
    // 引き直してから繋ぐ。scene.Create が GameObject 配列を再確保するので、
    // 上で掴んだ self は既に無効かもしれない (BuildSegment と同じ理由)。
    if (GameObject* owner = scene.Self()) deck.SetParent(*owner);

    auto& collider = deck.AddComponent<BoxColliderComponent>();
    const float half = std::max(deckHalfWidth, 0.05f);
    collider.SetSize({ half * 2.0f, std::max(deckThickness, 0.02f), half * 2.0f });
    // 甲板は «立つ» のが役目なので固い。
    collider.isTrigger = false;
    // ボス本体の剛体へ属させる。これが無いと «世界に固定された静的コライダー» として
    // 胴のカプセルを押し、ボスが勝手に動く / 吹き飛ぶ。
    collider.attachToParentBody = true;

    m_deck = EntityRef{ deck.GetID() };
    return true;
}

inline void BossHitboxRigComponent::UpdateDeck()
{
    GameObject* deck = m_deck.Resolve(scene);
    if (!deck) return;

    GameObject* anchor = m_deckAnchor.Resolve(scene);
    GameObject* self   = scene.Self();
    if (!anchor || !self) { deck->SetActive(false); return; }

    // 上面を «骨から deckRise» に置く。箱の中心はそこから厚みの半分ぶん下。
    //
    // WHY ローカルで書くか: 甲板はボスのルートの子なので、ワールド姿勢は
    //     TransformSystem が «親 × ローカル» から毎フレーム組み直す。
    //     worldPosition へ書いても、走る順によっては同じフレームで捨てられる。
    //
    // 向きはルートに合わせる (ローカル回転は恒等) ─ 転倒で骨が回っても、
    // 立つ面はボスの «背中の向き» であってほしい
    // (PlayerClimbComponent が着地点を測るのと同じ基準)。
    const float      half = std::max(deckThickness, 0.02f) * 0.5f;
    const Quaternion inv  = self->transform.worldRotation.Inverse();
    Vector3 local = inv * (anchor->transform.worldPosition - self->transform.worldPosition);
    local.y += std::max(deckRise, 0.0f) - half;

    deck->SetActive(true);
    deck->transform.position = local;
    deck->transform.rotation = Quaternion::Identity();
}

inline void BossHitboxRigComponent::ApplySolidity(bool solid)
{
    for (EntityRef& ref : m_solidParts) {
        GameObject* object = ref.Resolve(scene);
        if (!object) continue;
        if (auto* capsule = object->GetComponent<CapsuleColliderComponent>()) capsule->isTrigger = !solid;
        if (auto* sphere  = object->GetComponent<SphereColliderComponent>())  sphere->isTrigger  = !solid;
        if (auto* box     = object->GetComponent<BoxColliderComponent>())     box->isTrigger     = !solid;
    }
}

inline void BossHitboxRigComponent::OnUpdate()
{
    if (EnsureDeck()) UpdateDeck();

    // 前提が満たされたか / 崩れたかを追う。Build は OnStart で走るので、そこでは
    // まだ物理ワールドがスクリプトへ渡っていないことがある。
    const bool solid = SolidAllowed();
    if (solid != m_solidApplied) {
        ApplySolidity(solid);
        m_solidApplied = solid;
    }

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
    // 当たりのレイヤー。ボス本体と分けないと、固くした瞬間に自分の胴カプセルを
    // 押してボスが吹き飛ぶ (solidLegs の WHY)。
    hitbox.layer = hitboxLayer & 31;

    // 親を引き直してから繋ぐ。ローカル姿勢は «繋いだ後» に書く
    // (SetParent がワールド姿勢を保つ実装でも、後から書けば必ずこちらが勝つ)。
    GameObject* owner = scene.Self();
    if (GameObject* parent = owner ? FindInSubtree(*owner, segment.from) : nullptr)
        hitbox.SetParent(*parent);
    hitbox.transform.position = center;
    hitbox.transform.rotation = rotation;

    // WHY トリガーのままでよい部位があるか: 斬撃は物理を使わず BossPartComponent を
    //     型で集めて扇の内側かを測る (BladeComponent)。当たり判定としては
    //     トリガーで足りていて、solid にするのは «プレイヤーがぶつかる» ためだけ。
    // «固くするつもりか» (segment.blocking) と «今このフレーム固くしてよいか» を分ける。
    // 前者は作者の意図で、後者は衝突行列が前提を満たしているかで決まる。
    const bool solid = segment.blocking && SolidAllowed();
    if (segment.blocking) m_solidParts.push_back(EntityRef{ hitbox.GetID() });
    if (capsule) {
        auto& collider = hitbox.AddComponent<CapsuleColliderComponent>();
        collider.SetCapsule(radius, halfHeight);
        collider.isTrigger = !solid;
    } else {
        auto& collider = hitbox.AddComponent<SphereColliderComponent>();
        collider.SetRadius(radius);
        collider.isTrigger = !solid;
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
