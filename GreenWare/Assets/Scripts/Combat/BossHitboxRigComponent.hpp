/// @file    BossHitboxRigComponent.hpp
/// @brief   ボスの当たり判定をボーン階層から組み立てる。移動用コライダーとは別系統
/// @author  Hasegawa Jin
/// @date    2026-08-26
///
/// @note 移動用 (ルートのカプセル) と判定用 (骨ごとの当たり) を分ける: 兼用すると脚の間を
///       抜けたはずの一撃が当たるか、逆にボスが床をすり抜けるかになる。
/// @note ボーン階層から組み立てる (シーンに手で置かない): 59 本を手で置くと左右で寸法が
///       揃わずリグ更新のたびに置き直しになる。トリガーにするのは、とどめの間合い
///       (脚の間へ潜り込む) を押し返さないため。ランタイム生成のためシーンには保存されず、
///       Play のたびに現在のリグから組み直す。
#pragma once

#include <Engine/Scene/Components/ColliderComponent.hpp>
#include <Engine/Scene/EntityRef.hpp>
#include <Engine/Scene/GameObject.hpp>
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

/// @note Scene.hpp が要る理由: AddComponent / GetScript の template 本体は Scene.hpp の
///       末尾にある (GameObject.hpp では Scene が前方宣言のみ)。
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
    /// @note 既定 false (2026-09-10): 成立条件の衝突行列 (ProjectSettings.toml [physics]
    ///       ignoreCollisions) はエディタが開いていると古い設定で上書きされ消えることがある。
    ///       消えた瞬間の壊れ方は「ボスが吹き飛ぶ」と重いため、既定は行列不要側に置く。
    ///       on にする前に SolidAllowed が毎フレーム見る行列を確認すること。
    FBZZ_FIELD(bool, solidLegs, false, "脚と胴でぶつかる (要・衝突行列)")
    FBZZ_TOOLTIP("プレイヤーが 6m の重機をすり抜けないようにする物理の当たり。"
                 "ProjectSettings > Physics の衝突行列で «当たりのレイヤー × ボス本体» と "
                 "«自分自身» を切っていないと自己衝突でボスが吹き飛ぶ。"
                 "行列が無い間は自動でトリガーへ畳まれ、ログに何を設定すべきか出る。"
                 "off のままなら PlayerBossBlockComponent がすり抜けを止める")
    FBZZ_FIELD_RANGE(float, radiusScale, 1.0f, "全体スケール", 0.1f, 3.0f)
    FBZZ_TOOLTIP("全体の太さ。個別の比率を保ったまま «当たりの甘さ» だけを動かす")

    FBZZ_GROUP("レイヤー")
    /// @note 当たりを別レイヤーへ置く理由: 本体とボーン当たりは「同じ物体の 2 表現」で
    ///       互いに衝突してはいけない。行列はレイヤーの組でしか切れないため分離が必須。
    FBZZ_FIELD_RANGE_INT(int, hitboxLayer, 9, "当たりのレイヤー", 0, 31)
    FBZZ_TOOLTIP("生成する当たりに入れるレイヤー番号。ProjectSettings > Physics の "
                 "衝突行列で «このレイヤー × ボス本体のレイヤー» と «自分自身» を "
                 "切っておくこと。切らないとボスが自分の脚に押されて吹き飛ぶ")

    /// @note 甲板だけ別扱い: 胴の球面 (torsoRadius) の上は必ず滑り落ちるため平らな面が要る。
    ///       骨の子にしない理由: ボーンのローカル +Y (上方向) が骨ごとに違うため、ボスの
    ///       ルート姿勢 (PlayerClimbComponent と同じ基準) から毎フレーム置く。
    FBZZ_GROUP("甲板")
    /// @note 甲板だけ物理の当たりで済む理由: `attachToParentBody` でボス本体の剛体へ属させると
    ///       同一ボディのコライダー同士は当たらず (PhysicsSolver) 衝突行列が不要。脚は姿勢が
    ///       剛体基準になり骨と一緒に回らないうえ地面に届くため同じ手法は使えない。
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

    /// @brief 当たったオブジェクトからボス本体を引く。
    /// @note 親を遡る: HP/スクリプトはルートにあり、ヒットボックス側に持ち主を書き込むと
    ///       階層を組み替えるたびに貼り直しになる。
    [[nodiscard]] static GameObject* BossRootOf(GameObject* hit);

private:
    /// ボーン 1 本ぶんの当たり。to が空なら bone の位置に球を置く。
    struct Segment {
        std::string from;
        /// @note 空なら球
        std::string to;
        float       radius = 0.3f;
        /// @brief とどめの的になる部位か (Docs/break-parry.md)。
        /// @note 膝下だけにする理由: プレイヤーの射程 2.6m に対しボスは全高 6m あり、
        ///       Thigh は届かない高さのため。
        bool        severable = false;
        /// 脚の接尾辞 ("_FR" など)。とどめ の的だけが持つ。
        std::string suffix;
        /// @brief 斬撃で削れる部位にするか (BossPartComponent を付ける)。
        /// @note severable と分ける理由: とどめが通るのは膝下だけだが、斬れる部位は
        ///       膝下+コアの 2 種類あるため、1 つの旗で兼ねると片方が壊れる。
        bool        damageable = false;
        /// 部位の耐久。0 なら BossPartComponent の既定値のまま。
        int         health = 0;
        /// @brief プレイヤーがぶつかる「固い」当たりにするか。
        /// @note 部位ごとに分ける理由: 脚は塞がないと 6m の重機をすり抜けて歩けてしまう。
        ///       コアは足場で固いと壁になり、頭は届かず引っ掛かる元になるだけのため外す。
        bool        blocking = false;
    };

    void Build();
    /// from → to の間にカプセルを 1 本張る。to が空なら球。
    /// @return 作れたら true。ボーンが見つからなければ false。
    bool BuildSegment(const Segment& segment);
    /// ローカル +Y を direction へ向ける回転。
    [[nodiscard]] static Quaternion AlignUpTo(const Vector3& direction);

    /// @brief ボーンが揃うまで持ち越すセグメント。
    /// @note 1 回で作り切らない理由: ボーン GameObject は AnimatorSystem が実行時に作るため
    ///       OnStart 時点で 0 本のことがあり、揃うまで毎フレーム作り直さないと当たりが出ない。
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

    /// @brief 当たりを「固く」してよいか。衝突行列が前提を満たしていなければ false。
    /// @note 毎回確かめる (2026-09-10): 行列はエディタが開いていると古い設定で消えることが
    ///       あり、消えたまま固くすると自己衝突で毎フレーム前へずれ続ける (めり込み解決は
    ///       位置を直接動かすため速度 0 でも止まらず、AI が前進させているように見える)。
    [[nodiscard]] bool SolidAllowed();
    /// @brief 「固い」つもりの当たりへ、今の判定を書き込む。
    /// @note 作成後も書き換え可能にする理由: OnStart 時点では物理ワールド未接続のことがあり、
    ///       1 度きり決め打ちだと安全側のまま固まる。Play 中の solidLegs 変更にも効かせる。
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

    /// @note 真上ならそのまま。真下は軸が定まらないので X 軸まわりに 180 度倒す。
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
    /// @note 球にする (2026-09-08): 以前は Core→Muzzle のカプセルだったが、コアを背面へ
    ///       移し Muzzle を Body の子へ付け替えたため、結ぶと胴体を貫く当たりになってしまう。
    ///       コアは「蓋が開いている間だけ」斬れる的で、開閉と有効化は BossHatchComponent が握る。
    segments.push_back({ "Core", "",       coreRadius  * scale, false, "", true, coreHealth });

    /// @note 脚は 4 本とも同じ骨並び。README のリグ構成 (Thigh → Shin → Hock → Foot) に従う。
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
    /// @note 作った時点の判定を覚えておく。OnUpdate はここから «変わったか» だけを見る。
    m_solidApplied = SolidAllowed();

    ResolveAnchors();
}

inline void BossHitboxRigComponent::ResolveAnchors()
{
    /// @note 足とコアは AI と演出が名指しで使う。GameObject を作り終えてから引く
    ///       (scene.Create が GameObject 配列を再確保するため、生成前に掴んだポインタは無効)。
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

    /// @note 自分の剛体 (ボス本体) と、当たりどうし。この 2 組が切れていて初めて固くできる。
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
        /// @note 走っている最中に off にされたら畳む。作り直しは Stop → Play で足りる。
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
    /// @note 親はボスのルート。骨の子にすると、骨のローカル軸に «上» が乗ってしまう。
    ///
    ///       引き直してから繋ぐ。scene.Create が GameObject 配列を再確保するので、
    ///       上で掴んだ self は既に無効かもしれない (BuildSegment と同じ理由)。
    if (GameObject* owner = scene.Self()) deck.SetParent(*owner);

    auto& collider = deck.AddComponent<BoxColliderComponent>();
    const float half = std::max(deckHalfWidth, 0.05f);
    collider.SetSize({ half * 2.0f, std::max(deckThickness, 0.02f), half * 2.0f });
    /// @note 甲板は «立つ» のが役目なので固い。
    collider.isTrigger = false;
    /// @note ボス本体の剛体へ属させる。これが無いと «世界に固定された静的コライダー» として
    ///       胴のカプセルを押し、ボスが勝手に動く / 吹き飛ぶ。
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

    /// @note 上面は「骨から deckRise」、箱の中心はそこから厚みの半分下。ローカルで書く理由:
    ///       甲板はルートの子で、ワールド姿勢は TransformSystem が親×ローカルから毎フレーム
    ///       組み直すため、worldPosition へ書くと走る順によって同フレームで捨てられる。
    ///       向きはルートに合わせる (転倒で骨が回っても背中の向きを保つ)。
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

    /// @note 前提が満たされたか / 崩れたかを追う。Build は OnStart で走るので、そこでは
    ///       まだ物理ワールドがスクリプトへ渡っていないことがある。
    const bool solid = SolidAllowed();
    if (solid != m_solidApplied) {
        ApplySolidity(solid);
        m_solidApplied = solid;
    }

    if (m_pending.empty()) return;

    /// @note 毎フレーム試さない (2026-09-08): BuildSegment は 1 本ごとに全サブツリーを再帰で
    ///       歩くため、15 本保留のままだと正常時もずっと重くなる。数百 ms 遅れても害は無い。
    m_waited += Time::deltaTime;
    m_retryCooldown -= Time::deltaTime;
    if (m_retryCooldown > 0.0f) return;
    m_retryCooldown = kRetryInterval;

    /// @note 揃った物から順に生かす。1 フレームで全部揃う保証は無い
    ///       (AnimatorSystem がボーンを作る順はこちらから見えない)。
    for (std::size_t i = m_pending.size(); i-- > 0; ) {
        if (!BuildSegment(m_pending[i])) continue;
        ++debugHitboxes;
        m_pending.erase(m_pending.begin() + static_cast<std::ptrdiff_t>(i));
    }
    debugMissingBones = static_cast<int>(m_pending.size());

    if (m_pending.empty()) { ResolveAnchors(); return; }

    /// @note 綴り違いは「その部位だけ当たらない」形でしか出ないため名指しで言う。すぐ言わない
    ///       理由: 起動直後はボーン未生成が正常で、毎回出すと本当の綴り違いと見分けられない。
    if (m_waited > kGiveUpSeconds && !m_reported) {
        m_reported = true;
        debug.LogError("BossHitboxRigComponent could not find " +
                       std::to_string(m_pending.size()) +
                       " bone(s) after " + std::to_string(static_cast<int>(kGiveUpSeconds)) +
                       "s. Check the rig names against Assets/Models/Boss_01/README.md.");
        /// @note ここまで来たら「まだ出来ていない」でなく「名前が違う」。待っても見つからず
        ///       探索負荷だけが残るため諦める。
        m_pending.clear();
    }
}

inline bool BossHitboxRigComponent::BuildSegment(const Segment& segment)
{
    /// @note 生成のたびに引き直す。scene.Create が GameObject 配列を再確保するので、
    ///       ループの外で掴んだポインタは 2 本目以降で無効になる。
    GameObject* self = scene.Self();
    if (!self || !FindInSubtree(*self, segment.from)) return false;

    const float radius = std::max(segment.radius, 0.01f);

    /// @note カプセルの向きと長さは次の骨の位置から出す。ローカル座標を使う理由: ワールド姿勢は
    ///       TransformSystem 実行後でないと確定しないが、子ボーンのローカル位置はシーンに
    ///       書かれた値がそのまま使える。
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
    /// @note シーンには保存しない。Play のたびにその時点のリグから組み直す。
    hitbox.runtimeGenerated = true;
    /// @note 部位はボス本体と同じ «敵» として扱わせる。Mite の接地レイキャストは
    ///       タグで敵を捨てているので、これが無いとボスの脚を地面と読んで脚の上に浮く。
    hitbox.tag = "Enemy";
    /// @note 当たりのレイヤー。ボス本体と分けないと、固くした瞬間に自分の胴カプセルを
    ///       押してボスが吹き飛ぶ (solidLegs の @note を参照)。
    hitbox.layer = hitboxLayer & 31;

    /// @note 親を引き直してから繋ぐ。ローカル姿勢は «繋いだ後» に書く
    ///       (SetParent がワールド姿勢を保つ実装でも、後から書けば必ずこちらが勝つ)。
    GameObject* owner = scene.Self();
    if (GameObject* parent = owner ? FindInSubtree(*owner, segment.from) : nullptr)
        hitbox.SetParent(*parent);
    hitbox.transform.position = center;
    hitbox.transform.rotation = rotation;

    /// @note 斬撃は物理でなく BossPartComponent を型で集めて判定するため、当たりはトリガーで
    ///       足りる。solid にするのはプレイヤーが物理的にぶつかるためだけ。segment.blocking
    ///       (固くするつもりか) と SolidAllowed (今固くしてよいか) は別軸として分ける。
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

    /// @note 斬撃の扇は BossPartComponent を名指しで探すため、削れる部位はここで必ず宣言する
    ///       (無いと斬っても何も起きない)。とどめの的 (severable) は名前だけで足りるが、
    ///       削れるかは耐久という状態を持つためスクリプトの置き場所が要る。
    if (segment.damageable) {
        auto& part = hitbox.AddScript<BossPartComponent>();
        part.legSuffix = segment.suffix;
        /// @note 扇の判定はレンダラーを持たない部位に対して «本人の申告» を使う。
        part.hitRadius = radius;
        if (segment.health > 0) part.maxHealth = segment.health;
    }

    return true;
}

} // namespace sandbox
