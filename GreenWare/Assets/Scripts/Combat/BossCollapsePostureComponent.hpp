/// @file    BossCollapsePostureComponent.hpp
/// @brief   脚を失ったボスの «崩れた体» を、再生中のクリップの上から手続き的に作る
/// @author  Hasegawa Jin
/// @date    2026-08-30

/// @note 崩れ方ごとのクリップは焼かない。焼いた姿勢は他の moveset (歩き・踏みつけ等) と
/// @note 両立せず出した瞬間に立ち上がるため、崩れは «体に掛ける変形» として持ち、
/// @note 胴を傾けて沈め残った脚を IK で床へ留めるだけで、上のクリップを問わず成立させる。
/// @note 残った脚を結ぶ線を蝶番にして解く。四足が 2 本を失うと残り 2 本を結ぶ線が唯一の
/// @note 支持になり、体はそのまわりで «脚を失った側» へ倒れる。1 本目・3 本目も同じ式で、
/// @note «失った側に近い 2 本» の線が蝶番になる (変わるのは倒れる量だけ)。
/// @note 倒れ角は定数でなくリグから出す (θ = atan2(付け根の高さ, 蝶番までの距離))。
/// @note 定数だと前後と左右で同じだけ傾いて «途中で支えられている» 絵になるため。
#pragma once

#include <Engine/Scene/Components/IKSolverComponent.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>
#include <Scripts/Combat/BossAnimParams.hpp>
#include <Scripts/Combat/BossHitboxRigComponent.hpp>
#include <Scripts/Utils/WeaponSockets.hpp>
#include <algorithm>
#include <cmath>
#include <string>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

/// @note 崩れた体を作る層。BossRigComponent が «どの脚を失ったか» を押し込む。
class BossCollapsePostureComponent : public Script {
    FBZZ_SCRIPT(BossCollapsePostureComponent)

public:
    FBZZ_GROUP("Posture")
    FBZZ_FIELD(bool, collapse, true, "Collapse")
    FBZZ_TOOLTIP("脚を失ったとき体を崩す。切ると立ったまま脚だけ欠ける")
    FBZZ_FIELD(std::string, pivotName, "VisualPivot", "Pivot Node")
    FBZZ_TOOLTIP("傾けて沈める節。Boss と RootNode の間に挟んだ空オブジェクト。"
                 "骨ではなく «骨より上» を動かすので、どのクリップの上にも重なる")
    FBZZ_FIELD_RANGE(float, fallRatio, 1.0f, "Fall", 0.0f, 1.2f)
    FBZZ_TOOLTIP("1.0 で «支えを失った側の付け根が床に着く» まで倒れる。角度はリグから"
                 "出すので、前後に失えば浅く・左右に失えば深く自動で変わる。"
                 "0.5 で途中まで、1.2 で床へめり込むまで")
    FBZZ_FIELD_RANGE(float, sagDegrees, 0.0f, "Sag (3 legs)", 0.0f, 30.0f)
    FBZZ_TOOLTIP("3 本残っているときのかしぎ角。既定 0 ─ 三点で立っている体を «剛体として» "
                 "傾けると、蝶番から外れた 1 本が必ず床から浮く。歩いている間は IK で"
                 "留められない (脚が止まって滑る) ので、入れるなら浮きを承知で")
    FBZZ_FIELD_RANGE(float, sinkMeters, 0.25f, "Extra Sink", 0.0f, 2.0f)
    FBZZ_TOOLTIP("倒した後にさらに落とす高さ。傾き自体で下がる分は蝶番から自動で出るので、"
                 "ここは «脚が軋んで沈む» 分だけの足し前。2 本残りを基準にした値で、"
                 "残る脚が減るほど 1 本あたりの荷重が増えるぶん自動で深くなる "
                 "(3本 x0.7 / 2本 x1 / 1本以下 x2)")
    FBZZ_FIELD_RANGE(float, settleSeconds, 0.85f, "Settle", 0.05f, 5.0f)
    FBZZ_TOOLTIP("崩れきるまでの秒数。0 に近いほど «折れた» 感じになり、長いほど «沈む»")

    /// @note 脚を斬られた «一瞬» の体の反応。崩れ (不可逆) とは別で、必ず立ち姿へ戻る。
    /// @note 崩れと同じコンポーネントへ入れる。書き込む先は VisualPivot 1 つしかなく、
    /// @note 別スクリプトから書くと崩れていない間 (m_blend==0) にここが毎フレーム ZERO へ
    /// @note 戻すため、よろめきが 1 フレームも残らない。
    FBZZ_GROUP("のけぞり")
    FBZZ_FIELD(bool, stagger, true, "のけぞり")
    FBZZ_TOOLTIP("斬られた脚の側へ体を泳がせる。切ると脚だけが反応する")
    FBZZ_FIELD_RANGE(float, staggerDegrees, 4.5f, "Degrees", 0.0f, 30.0f)
    FBZZ_TOOLTIP("一撃で傾く角度。全高 6m では 5 度でも足元が 0.5m 動くので、"
                 "«よろめき» には 3〜6 度で足りる")
    FBZZ_FIELD_RANGE(float, staggerMaxDegrees, 9.0f, "Max Degrees", 0.0f, 45.0f)
    FBZZ_TOOLTIP("連続で斬られても傾きはここで頭打ち。無いと連撃で体が回り続ける")
    FBZZ_FIELD_RANGE(float, staggerSink, 0.22f, "沈み", 0.0f, 2.0f)
    FBZZ_TOOLTIP("傾きに連動して沈む量 [m]。支えを失った «荷重が抜けた» 感を作る")
    FBZZ_FIELD_RANGE(float, staggerStiffness, 90.0f, "硬さ", 1.0f, 400.0f)
    FBZZ_TOOLTIP("立ち姿へ戻ろうとする強さ。大きいほど速く戻る")
    FBZZ_FIELD_RANGE(float, staggerDamping, 11.0f, "減衰", 0.0f, 60.0f)
    FBZZ_TOOLTIP("揺り戻しの収まり。小さいと «ゆらゆら» 続き、大きいと戻るだけになる")

    FBZZ_GROUP("Legs")
    FBZZ_FIELD(bool, holdFeet, true, "Hold Feet")
    FBZZ_TOOLTIP("残った脚を IK で床へ留める。切ると脚も一緒に傾いて宙に浮く。"
                 "3 本残っている間はまだ歩くので、留めるのは 2 本以下になってから")
    FBZZ_FIELD_RANGE(float, footIkWeight, 1.0f, "IK Weight", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, footSpread, 1.15f, "拡がり", 0.5f, 2.0f)
    FBZZ_TOOLTIP("残った脚を外へ張り出させる倍率。荷重が 2 本に寄るので、"
                 "立っていたときの位置のままだと支えているように見えない")
    FBZZ_FIELD_RANGE(float, groundOffset, 0.0f, "接地のオフセット", -1.0f, 1.0f)
    FBZZ_TOOLTIP("足を置く高さの微調整。ボス本体の足元を床とみなす")

    /// @note 壊れた脚は本体に残る (BossRigComponent の «壊れた脚を残す»)。残っている以上、
    /// @note 崩れた体と一緒に床へ突き刺さる ── 脚が «伸びたまま» 動かないためで、傾きの側の
    /// @note 問題ではない。
    /// @note «畳む» で解く。IK の的を «付け根の真下・床の上・伸びきらない距離» へ置けば、
    /// @note 足が床を貫通しない・脚が伸びきらない・体が沈むほど畳まれる、が同時に満たされる。
    /// @note メッシュを消す/傾きを浅くする案は採らない。どちらも «残す» «脚を失って崩れた»
    /// @note という直したい絵そのものを削ることになる。
    FBZZ_GROUP("壊れた脚")
    FBZZ_FIELD(bool, foldBrokenLegs, true, "壊れた脚を畳む")
    FBZZ_TOOLTIP("壊れた脚を膝から畳んで床の上へ置く。切ると脚がクリップのまま伸びて"
                 "床を貫く (脚を消す構成なら切ってよい)")
    FBZZ_FIELD_RANGE(float, foldReach, 0.55f, "畳む深さ", 0.15f, 1.0f)
    FBZZ_TOOLTIP("伸びきり長さに対する «付け根から足まで» の比。小さいほど深く潰れる。"
                 "1.0 に近づけると伸びたままになり、また床へ刺さる")
    FBZZ_FIELD_RANGE(float, foldSpread, 0.45f, "外への逃がし [m]", 0.0f, 2.0f)
    FBZZ_TOOLTIP("畳んだ脚を体の外側へどれだけ逃がすか。0 だと胴の真下で潰れて"
                 "自分の腹に埋まる")
    FBZZ_FIELD_RANGE(float, foldSeconds, 0.45f, "畳むまで [s]", 0.05f, 3.0f)
    FBZZ_TOOLTIP("もげた瞬間から畳み切るまで。短いほど «折れた»、長いほど «崩れ落ちた»")
    FBZZ_FIELD_RANGE(float, foldWeight, 1.0f, "IK Weight", 0.0f, 1.0f)
    FBZZ_TOOLTIP("畳みの効き。0 でクリップのまま (＝貫通する)")

    FBZZ_GROUP("デバッグ")
    /// @brief 戦わずに崩れ「方」だけを見る口。倒れる向き・角・沈みの調整用。
    /// @note 本番のもぐボタン (BossRigComponent の Break FR 等) と別にする: あちらは
    /// @note 一度もぐと Stop まで戻らず微調整に向かない。これは見た目だけで AI/判定は動かない。
    FBZZ_FIELD(bool, previewFR, false, "Preview FR")
    FBZZ_FIELD(bool, previewFL, false, "Preview FL")
    FBZZ_FIELD(bool, previewBR, false, "Preview BR")
    FBZZ_FIELD(bool, previewBL, false, "Preview BL")
    FBZZ_TOOLTIP("«その脚を失ったことにする» 見た目だけの差し替え。1 つでも入れると"
                 "実際の欠損より優先される。調整が済んだら全部外すこと "
                 "(入れたまま保存すると最初から崩れたボスになる)")
    FBZZ_FIELD_READ_ONLY(std::string, debugPose, "-", "Pose")
    FBZZ_FIELD_READ_ONLY(float, debugBlend, 0.0f, "ブレンド")

    /// @brief 失った脚のビット (FR=1 / FL=2 / BR=4 / BL=8)。壊した側から押し込む。
    /// @note こちらから引かない: 状態を持つ BossRigComponent が既にこちらを include して
    /// @note いるため、逆方向に引くと include が循環する。
    void SetBrokenMask(int mask) { m_mask = mask & 0xF; }
    [[nodiscard]] int  BrokenMask() const
    {
        const int preview = (previewFR ? 1 : 0) | (previewFL ? 2 : 0)
                          | (previewBR ? 4 : 0) | (previewBL ? 8 : 0);
        return preview != 0 ? preview : m_mask;
    }
    /// @note 崩れ切っているか。0 = 健在 / 1 = 完全に崩れた。
    [[nodiscard]] float Blend() const { return m_blend; }

    /// @note 脚を斬られた。その脚の側へ体を泳がせる。

    /// @param towardWorld  傾ける先のワールド座標 (斬られた脚の位置)。水平成分だけ使う。
    /// @param strength     1.0 で staggerDegrees ぶん。溜め斬りは 1 より大きい値を渡す。
    void Stagger(const Vector3& towardWorld, float strength);

    /// @note 今よろめいている量 [degrees]。HUD や他の演出が読める。
    [[nodiscard]] float StaggerDegrees() const { return ToDeg(m_lean.Length()); }

    void OnUpdate() override;

private:
    /// @brief 崩れ用チェーンの order 起点。
    /// @note 引き合い (700) より前に置く: 両方が同時に掛かる瞬間 (残り 2 本を引かれて全損)
    /// @note は引かれる絵を見せたいため、後から解く方が勝つ規則を利用して先に解く。
    static constexpr int kChainOrder = 600;

    [[nodiscard]] static const char* SuffixOf(int leg);
    [[nodiscard]] bool IsBroken(int leg) const { return (BrokenMask() & (1 << leg)) != 0; }
    [[nodiscard]] int  AliveCount() const
    {
        int alive = 0;
        for (int leg = 0; leg < 4; ++leg) alive += IsBroken(leg) ? 0 : 1;
        return alive;
    }

    void EnsureTargets();
    [[nodiscard]] IKChain* EnsureChain(int leg);
    void ReleaseChains();
    /// @brief 壊れた脚を膝から畳んで床の上へ置く。崩れているかに関わらず毎フレーム。
    /// @note 崩れ (m_blend) から切り離す: 脚がクリップで床を出入りするのは崩れ中に限らない。
    /// @note 畳む条件は「壊れているから」だけで足りる。
    void DriveBrokenLegs(GameObject& self, float groundY, float dt);
    /// @note 蝶番の軸・場所・倒す向き・倒れ角 (rad) を解く。1 本も失っていなければ false。
    [[nodiscard]] bool SolveHinge(Vector3& axisLocal, Vector3& pivotLocal,
                                  float& sign, float& angleRad) const;
    [[nodiscard]] bool FootLocal(int leg, Vector3& out) const;
    /// @note 脚の付け根 (Thigh)。床に当たって倒れが止まる点。
    [[nodiscard]] bool HipLocal(int leg, Vector3& out) const;
    /// @note 付け根のワールド位置。IK の的が届くかを測る起点。
    [[nodiscard]] bool HipWorld(int leg, Vector3& out) const;
    /// @note 付け根から足先までの伸びきった長さ。骨の並びから測る。
    [[nodiscard]] float LegReach(int leg) const;
    /// @note ボスのローカル系へ落とす。x = 右 / y = 高さ / z = 前。
    [[nodiscard]] bool ToLocal(const GameObject* bone, Vector3& out) const;

    int     m_mask  = 0;
    /// @note よろめきのバネ。向きは «倒れる先» の水平単位ベクトル、長さは傾き [rad]。
    /// @note 速度と対で持ち、減衰バネで 0 へ戻る。
    Vector3 m_lean{};
    Vector3 m_leanVel{};

    /// @note よろめきを VisualPivot の姿勢へ足す。崩れているぶんだけ効きを弱める。
    void ApplyStagger(GameObject& pivot, float dt);

    float   m_blend = 0.0f;
    bool    m_built = false;

    /// @note 「失った脚が変わった瞬間」にだけ解き直す。蝶番/足置き場は今の足位置から出すため、
    /// @note 毎フレーム解くと IK の出力を次の入力に混ぜて蝶番が流れ続ける (解く瞬間の値は
    /// @note ToLocal が立ち姿へ割り戻して揃える)。
    int     m_solvedMask = -1;
    bool    m_solved     = false;
    Vector3 m_axis{};
    Vector3 m_hinge{};
    float   m_sign  = 1.0f;
    float   m_angle = 0.0f;   ///< @note 倒れきったときの角 (rad)

    /// @note 既に崩れている体が «別の崩れ方» へ移るとき (2 本目の後に 3 本目を失う等) の
    /// @note 繋ぎ。蝶番も向きも変わるので、解き直した姿勢をそのまま書くと 1 フレームで
    /// @note 別の倒れ方へ飛ぶ。前の姿勢から新しい姿勢へ渡す。
    Quaternion m_fromRot = Quaternion::Identity();
    Vector3    m_fromPos{};
    float      m_shift = 1.0f;

    struct LegPlant {
        EntityRef target;
        /// @note 崩れ始めに控えた、ボスのローカル系での足の置き場。
        Vector3   offset;
        /// @note 付け根から足先までの長さ。骨の間隔は姿勢に依らないので 1 度測れば足りる。
        float     reach   = 0.0f;
        bool      planted = false;
    };
    LegPlant m_legs[4];
    /// @note 壊れた脚を畳み切った度合い [0,1]。もげた瞬間に IK を全開で入れると、
    /// @note 伸びた脚が 1 フレームで潰れて «消えた» ように見える。
    float    m_fold[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
};

FBZZ_REFLECT(BossCollapsePostureComponent)

inline const char* BossCollapsePostureComponent::SuffixOf(int leg)
{
    static constexpr const char* kSuffix[4] = { "_FR", "_FL", "_BR", "_BL" };
    return kSuffix[std::clamp(leg, 0, 3)];
}

inline void BossCollapsePostureComponent::EnsureTargets()
{
    /// @note 毎回名前で拾い直す: DLL リロードで EntityRef は空に戻るが GameObject は
    /// @note Scene に残るため、「作った」と覚えたままだと的が増え続ける。
    if (m_built && m_legs[0].target.Resolve(scene)) return;
    std::size_t missing = 0;
    for (int leg = 0; leg < 4; ++leg)
        if (!scene.Find(std::string("BossCollapseTarget") + SuffixOf(leg), true)) ++missing;
    if (!scene.CanCreate(missing)) return;

    GameObject* self = scene.Self();
    if (!self) return;
    auto* ik = self->GetComponent<IKSolverComponent>();
    if (!ik) ik = &self->AddComponent<IKSolverComponent>();
    ik->enabled = true;

    for (int leg = 0; leg < 4; ++leg) {
        const std::string name = std::string("BossCollapseTarget") + SuffixOf(leg);
        GameObject* target = scene.Find(name, true);
        if (!target) {
            GameObject* created = scene.Create(name);
            if (!created) return;
            created->runtimeGenerated = true;
            target = created;
        }
        m_legs[leg].target = EntityRef{ target->GetID() };
    }
    m_built = true;
}

inline IKChain* BossCollapsePostureComponent::EnsureChain(int leg)
{
    GameObject* self   = scene.Self();
    GameObject* target = m_legs[leg].target.Resolve(scene);
    if (!self || !target) return nullptr;

    auto* ik = self->GetComponent<IKSolverComponent>();
    if (!ik) return nullptr;

    const int order = kChainOrder + leg;
    for (IKChain& chain : ik->chains)
        if (chain.type == IKSolverType::FABRIK && chain.order == order) {
            chain.targetEntity = target->GetID();
            return &chain;
        }

    const std::string suffix = SuffixOf(leg);
    IKChain chain{};
    chain.type = IKSolverType::FABRIK;
    /// @note README のリグ構成どおり 4 節。TwoBone は 3 本しか受けない。
    chain.boneNames    = { "Thigh" + suffix, "Shin" + suffix,
                           "Hock" + suffix, "Foot" + suffix };
    chain.order        = order;
    chain.enabled      = false;
    chain.weight       = 0.0f;
    chain.targetEntity = target->GetID();
    ik->chains.push_back(std::move(chain));
    return &ik->chains.back();
}

inline void BossCollapsePostureComponent::ReleaseChains()
{
    GameObject* self = scene.Self();
    if (!self) return;
    if (auto* ik = self->GetComponent<IKSolverComponent>())
        for (IKChain& chain : ik->chains) {
            if (chain.type != IKSolverType::FABRIK) continue;
            if (chain.order < kChainOrder || chain.order >= kChainOrder + 4) continue;
            /// @note 畳んだ脚は手放さない。ここで解くと、崩れていない間 (歩けるうちに 1 本
            /// @note 失った等) だけ脚が伸びて床へ刺さる ── 直したはずの絵が状況で戻る。
            if (foldBrokenLegs && IsBroken(chain.order - kChainOrder)) continue;
            chain.enabled = false;
            chain.weight  = 0.0f;
        }
    /// @note 置き場は消さない。植え直すのは «失った脚が変わったとき» だけで、
    /// @note ここで消すと崩れた直後の 1 フレームで控えた値が飛ぶ。
}

inline void BossCollapsePostureComponent::DriveBrokenLegs(GameObject& self, float groundY,
                                                          float dt)
{
    for (int leg = 0; leg < 4; ++leg) {
        if (!foldBrokenLegs || !IsBroken(leg)) { m_fold[leg] = 0.0f; continue; }

        IKChain* chain = EnsureChain(leg);
        GameObject* target = m_legs[leg].target.Resolve(scene);
        if (!chain || !target) continue;

        m_fold[leg] = std::clamp(m_fold[leg] + dt / std::max(foldSeconds, 0.05f), 0.0f, 1.0f);

        Vector3 hip{};
        if (!HipWorld(leg, hip)) continue;
        if (m_legs[leg].reach <= 0.0f) m_legs[leg].reach = LegReach(leg);
        const float reach = m_legs[leg].reach;
        if (reach <= 0.0f) continue;

        /// @note 逃がす向きは «体の中心から付け根へ» の水平。脚が付いている側そのものなので、
        /// @note どの脚でも «外へ» になる。
        Vector3 outward = hip - self.transform.worldPosition;
        outward.y = 0.0f;
        outward = outward.NormalizedOr(self.transform.Right());

        /// @note 的は «付け根の真下、少し外、床の上»。
        Vector3 goal{ hip.x + outward.x * std::max(foldSpread, 0.0f),
                      groundY,
                      hip.z + outward.z * std::max(foldSpread, 0.0f) };

        /// @note 伸びきらせない。的が遠いと FABRIK は «真っ直ぐ伸ばして的を指す» ので、
        /// @note 潰れた脚のはずが «床を突いて踏ん張っている脚» になる。
        const Vector3 toGoal = goal - hip;
        const float   span   = toGoal.Length();
        const float   limit  = std::max(reach * std::clamp(foldReach, 0.15f, 1.0f), 0.05f);
        if (span > limit && span > 0.0001f) goal = hip + toGoal * (limit / span);

        /// @note 縮めた結果が床より下に来ることはないが、床の高さは体の足元から取っている
        /// @note ので、傾いた体では下回りうる。床より下は «貫通» そのものなので必ず戻す。
        goal.y = std::max(goal.y, groundY);

        target->transform.position      = goal;
        target->transform.worldPosition = goal;

        chain->enabled = true;
        chain->weight  = std::clamp(m_fold[leg] * std::max(foldWeight, 0.0f), 0.0f, 1.0f);
    }
}

inline bool BossCollapsePostureComponent::ToLocal(const GameObject* bone, Vector3& out) const
{
    GameObject* self = scene.Self();
    if (!bone || !self) return false;

    /// @note ボスの向きは戦闘中ずっと変わる。ワールドのまま控えると、向き直った瞬間に
    /// @note 足の置き場が体の反対側へ回り込む。自分の右前ベクトルで分解して持つ。
    const Vector3 d = bone->transform.worldPosition - self->transform.worldPosition;
    Vector3 local{ Vector3::Dot(d, self->transform.Right()),
                   d.y,
                   Vector3::Dot(d, self->transform.Forward()) };

    /// @note 自分が pivot へ掛けた変形を外して「立っていたときの」値へ戻す。骨のワールド
    /// @note 位置は崩れた後の姿勢そのもので、そのまま測ると出力を入力に混ぜてしまい、
    /// @note 3 本目を失う等の解き直しで倒れ代が 0 と出て崩れが取り消され立ち姿へ戻る。
    if (GameObject* pivot = scene.Find(pivotName, true)) {
        const Vector3 rel = local - pivot->transform.position;
        local = pivot->transform.rotation.Inverse() * rel;
    }

    out = local;
    return true;
}

inline bool BossCollapsePostureComponent::FootLocal(int leg, Vector3& out) const
{
    const auto* rig = scene.GetScript<BossHitboxRigComponent>();
    return rig && ToLocal(rig->FootBone(static_cast<BossLeg>(leg)), out);
}

inline bool BossCollapsePostureComponent::HipLocal(int leg, Vector3& out) const
{
    GameObject* self = scene.Self();
    if (!self) return false;
    /// @note 付け根は当たり判定を持たないので RigComponent には控えられていない。
    /// @note 名前で引く ─ 骨名はシーン内で一意でないため、必ずボスの部分木だけを見る。
    return ToLocal(FindInSubtree(*self, std::string("Thigh") + SuffixOf(leg)), out);
}

inline bool BossCollapsePostureComponent::HipWorld(int leg, Vector3& out) const
{
    GameObject* self = scene.Self();
    if (!self) return false;
    GameObject* hip = FindInSubtree(*self, std::string("Thigh") + SuffixOf(leg));
    if (!hip) return false;
    out = hip->transform.worldPosition;
    return true;
}

inline float BossCollapsePostureComponent::LegReach(int leg) const
{
    GameObject* self = scene.Self();
    if (!self) return 0.0f;

    /// @note README のリグ構成どおり Thigh → Shin → Hock → Foot。節の間隔は姿勢を変えても
    /// @note 変わらないので、立っている間に測った値がそのまま伸びきった長さになる。
    static constexpr const char* kJoints[4] = { "Thigh", "Shin", "Hock", "Foot" };
    Vector3 previous{};
    float   total = 0.0f;
    for (int i = 0; i < 4; ++i) {
        GameObject* joint = FindInSubtree(*self, std::string(kJoints[i]) + SuffixOf(leg));
        if (!joint) return 0.0f;
        const Vector3 here = joint->transform.worldPosition;
        if (i > 0) total += (here - previous).Length();
        previous = here;
    }
    return total;
}

inline bool BossCollapsePostureComponent::SolveHinge(Vector3& axisLocal,
                                                    Vector3& pivotLocal,
                                                    float&   sign,
                                                    float&   angleRad) const
{
    Vector3 foot[4]{};
    bool    known[4]{};
    for (int leg = 0; leg < 4; ++leg)
        known[leg] = FootLocal(leg, foot[leg]);

    /// @note 倒れる先は «失った脚の重心»。対角では両側が釣り合って 0 になる ─
    /// @note そのときはどちらでもよく、符号を固定してねじれの向きを一定にする。
    Vector3 lost{};
    int     lostCount = 0;
    for (int leg = 0; leg < 4; ++leg)
        if (IsBroken(leg) && known[leg]) {
            lost = Vector3{ lost.x + foot[leg].x, 0.0f, lost.z + foot[leg].z };
            ++lostCount;
        }
    if (lostCount == 0) return false;
    lost = Vector3{ lost.x / lostCount, 0.0f, lost.z / lostCount };

    int alive[4];
    int aliveCount = 0;
    for (int leg = 0; leg < 4; ++leg)
        if (!IsBroken(leg) && known[leg]) alive[aliveCount++] = leg;

    if (aliveCount >= 4) return false;

    Vector3 a{};
    Vector3 b{};
    if (aliveCount >= 2) {
        /// @note 蝶番は «失った側に近い 2 本» を結ぶ線。残り 2 本ならその 2 本しかなく、
        /// @note 3 本なら失った角を挟む 2 本が選ばれる (残る 1 本は反対側で、体はそれを
        /// @note 乗り越えて倒れるのではなく、その線の上でかしぐ)。
        const Vector3 dir{ lost.x, 0.0f, lost.z };
        int   pick[2] = { alive[0], alive[1] };
        float best[2] = { -1.0e9f, -1.0e9f };
        for (int i = 0; i < aliveCount; ++i) {
            const int   leg = alive[i];
            const float d   = foot[leg].x * dir.x + foot[leg].z * dir.z;
            if (d > best[0]) { best[1] = best[0]; pick[1] = pick[0]; best[0] = d; pick[0] = leg; }
            else if (d > best[1]) { best[1] = d; pick[1] = leg; }
        }
        a = foot[pick[0]];
        b = foot[pick[1]];
    } else {
        /// @note 残り 1 本以下。線が引けないので、その足を通る «倒れる向きに直交する» 線を
        /// @note 蝶番にする。四本とも失えば倒れる先も消えるので、前へ突っ伏す向きを既定にする。
        const Vector3 f = aliveCount == 1 ? foot[alive[0]] : Vector3{ 0.0f, 0.0f, 0.0f };
        Vector3 away{ lost.x - f.x, 0.0f, lost.z - f.z };
        const float   awayLen = std::sqrt(away.x * away.x + away.z * away.z);
        away = awayLen < 0.01f
            ? Vector3{ 0.0f, 0.0f, 1.0f }
            : Vector3{ away.x / awayLen, 0.0f, away.z / awayLen };
        a = Vector3{ f.x - away.z, f.y, f.z + away.x };
        b = Vector3{ f.x + away.z, f.y, f.z - away.x };
    }

    /// @note 蝶番の «向き»。水平成分だけを見る。
    Vector3 hinge{ b.x - a.x, 0.0f, b.z - a.z };
    const float len = std::sqrt(hinge.x * hinge.x + hinge.z * hinge.z);
    if (len < 0.01f) return false;
    hinge     = Vector3{ hinge.x / len, 0.0f, hinge.z / len };
    axisLocal = hinge;

    /// @note 蝶番の「場所」は選んだ 2 本の足の中点、高さは接地面。原点 (足元中央) 回転だと
    /// @note 4.5m 上の胴が横へ振り出され「倒れた」でなく「横っ飛びした」絵になるため、
    /// @note 実際に回るのは残った足を結ぶ線の上 (原点を通らない) にする。
    const Vector3 mid{ (a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f, (a.z + b.z) * 0.5f };
    pivotLocal = mid;
    /// @note 蝶番に直交する水平ベクトル。hinge × up がこれになる。
    const Vector3 perp{ -hinge.z, 0.0f, hinge.x };
    const float   side = (lost.x - mid.x) * perp.x + (lost.z - mid.z) * perp.z;

    /// @note 角 θ で蝶番周りに回すと点 r は θ·(hinge×r) へ動く (Quaternion の 1 次展開)。
    /// @note 真上の点は hinge×up = perp なので「+θ で perp 側へ」倒れる。失った側
    /// @note (side>0 が +perp 側) へ倒すため符号をそのまま使う。
    /// @warning 符号を反転すると見た目が大きく崩れるのではなく「なんか変」としか見えず
    /// @note 気付きにくい (対角喪失時は side=0 でどちらでも同じ)。
    sign = side >= 0.0f ? 1.0f : -1.0f;

    /// @note 倒れ角。支えを失った側の付け根が床へ着いたら止まる。蝶番から付け根までの
    /// @note 水平距離 d と付け根の高さ h に対して、θ = atan2(h, d) で床に触れる。
    /// @note 複数本失っていれば «最初に着いた 1 本» が体を止めるので、最小角を採る。
    float contact = -1.0f;
    for (int leg = 0; leg < 4; ++leg) {
        Vector3 hip{};
        if (!IsBroken(leg) || !HipLocal(leg, hip)) continue;
        /// @note 倒れる側にある付け根だけが床へ近づく。蝶番の反対側にあるものは «上がる»
        /// @note ので、絶対値で測ると «上がっていく点が床に着く» という嘘の終点になる。
        const float d = ((hip.x - mid.x) * perp.x + (hip.z - mid.z) * perp.z) * sign;
        if (d <= 0.0f) continue;
        const float h = std::max(hip.y - mid.y, 0.0f);
        const float t = std::atan2(h, std::max(d, 0.05f));
        if (contact < 0.0f || t < contact) contact = t;
    }
    /// @note 付け根が引けないリグでも «崩れない» にはしない。四足の胴を横倒しにする
    /// @note ときの目安として 45 度を置く。
    if (contact < 0.0f) contact = ToRad(45.0f);

    /// @note 3 本残る間はまだ三点で立てる (床当たりまで倒すと「1 本折れただけで転倒」に
    /// @note なるため撓みぶんの角で止める)。sagDegrees が既定 0 な理由: 三点接地の剛体は
    /// @note 回すと軸外の 1 本が浮く。撓ませるには脚ごとの IK が要るが歩行中は滑るため両立しない。
    angleRad = aliveCount >= 3 ? ToRad(sagDegrees)
                               : contact * std::max(fallRatio, 0.0f);
    /// @note 90 度を越えると «倒れる» ではなく «裏返る»。リグが壊れていても超えさせない。
    angleRad = std::clamp(angleRad, 0.0f, ToRad(85.0f));
    return angleRad > 0.0f;
}

inline void BossCollapsePostureComponent::Stagger(const Vector3& towardWorld, float strength)
{
    if (!stagger || staggerDegrees <= 0.0f) return;

    GameObject* self = scene.Self();
    if (!self) return;

    /// @note 水平成分だけを見る。上下を混ぜると «斬られて浮く» になる。
    const Vector3 d = towardWorld - self->transform.worldPosition;
    const Vector3 toward = Vector3{ d.x, 0.0f, d.z }.NormalizedOr(Vector3::ZERO);
    if (toward.LengthSq() <= EPSILON) return;

    /// @note 速度へ足す。位置へ直接入れると «瞬間移動してから戻る» になり、当たった
    /// @note 衝撃ではなく «別のポーズへ切り替わった» ように見える。
    m_leanVel += toward * (ToRad(staggerDegrees) * std::max(strength, 0.0f) * staggerStiffness
                           * 0.05f);
}

inline void BossCollapsePostureComponent::ApplyStagger(GameObject& pivot, float dt)
{
    if (!stagger) {
        m_lean    = Vector3::ZERO;
        m_leanVel = Vector3::ZERO;
        return;
    }

    /// @note 減衰バネ。刻みが荒いフレームで発散しないよう、1 回の更新量を 1/60 で切る。
    const float step = std::min(std::max(dt, 0.0f), 1.0f / 60.0f);
    m_leanVel += (m_lean * -staggerStiffness - m_leanVel * staggerDamping) * step;
    m_lean    += m_leanVel * step;

    /// @note 連撃で回り続けないよう頭打ちにする。速度ごと削らないと、次のフレームで
    /// @note また上限を超えて «上限に張り付いたまま震える» になる。
    const float maxLean = ToRad(std::max(staggerMaxDegrees, 0.0f));
    const float amount  = m_lean.Length();
    if (maxLean > 0.0f && amount > maxLean) {
        const Vector3 dir = m_lean / amount;
        m_lean    = dir * maxLean;
        m_leanVel -= dir * std::max(Vector3::Dot(m_leanVel, dir), 0.0f);
    }

    if (m_lean.LengthSq() <= EPSILON) return;

    /// @note 崩れているほど弱める。倒れ切った体がまだ泳いでいると «死体が呼吸している» になる。
    const float gain = 1.0f - Clamp01(m_blend);
    const float lean = m_lean.Length() * gain;
    if (lean <= EPSILON) return;

    const Vector3 dir = m_lean.NormalizedOr(Vector3::ZERO);
    /// @note 傾く先が dir なので、回転軸はその左右。UP との外積で取る。
    const Vector3 axis = Vector3::Cross(Vector3::UP, dir).NormalizedOr(Vector3::ZERO);
    if (axis.LengthSq() <= EPSILON) return;

    pivot.transform.rotation =
        (Quaternion::FromAxisAngle(axis, lean) * pivot.transform.rotation).Normalized();
    pivot.transform.position.y -= staggerSink * std::sin(lean);
}

inline void BossCollapsePostureComponent::OnUpdate()
{
    GameObject* self  = scene.Self();
    GameObject* pivot = self ? scene.Find(pivotName, true) : nullptr;
    if (!self || !pivot) {
        if (self && !pivot)
            debug.LogWarning("BossCollapsePostureComponent: pivot node '" + pivotName +
                             "' not found. Insert an empty between Boss and RootNode, "
                             "or clear Collapse.");
        return;
    }

    EnsureTargets();

    const float dt   = std::max(Time::deltaTime, 0.0f);
    const float rate = dt / std::max(settleSeconds, 0.01f);

    /// @note 失った脚が変わった瞬間にだけ解く。2 本目の後に 3 本目を失えば蝶番も倒れる
    /// @note 向きも変わるので、そのたびに解き直す (測る値は ToLocal が立ち姿へ戻している)。
    const int mask = BrokenMask();
    if (mask != m_solvedMask) {
        m_solvedMask = mask;
        for (LegPlant& leg : m_legs) leg.planted = false;
        /// @note 既に崩れているなら、今の姿勢を繋ぎの起点として控える。
        m_shift = m_blend > 0.0f ? 0.0f : 1.0f;
        if (m_shift <= 0.0f) {
            m_fromRot = pivot->transform.rotation;
            m_fromPos = pivot->transform.position;
        }
        m_solved = SolveHinge(m_axis, m_hinge, m_sign, m_angle);
        if (m_solved)
            for (int leg = 0; leg < 4; ++leg) {
                Vector3 local{};
                if (!IsBroken(leg) && FootLocal(leg, local)) {
                    m_legs[leg].offset  = Vector3{ local.x, 0.0f, local.z };
                    m_legs[leg].reach   = LegReach(leg);
                    m_legs[leg].planted = true;
                }
            }
    }
    /// @note 床は «ボス本体の足元»。崩れの足留めと同じ基準にしないと、畳んだ脚だけが
    /// @note 別の高さへ置かれて «片脚だけ床が違う» になる。
    const float groundY = self->transform.worldPosition.y + groundOffset;

    /// @note 壊れた脚を畳むのは崩れの前。崩れていない間も脚は壊れたままで、
    /// @note 伸びていれば床を出入りする (理由は DriveBrokenLegs の @note を参照)。
    DriveBrokenLegs(*self, groundY, dt);

    const bool down = collapse && m_solved;

    /// @note 崩れは不可逆なので戻りは «切られたとき» だけ。入りだけ時間を掛ける。
    m_blend = std::clamp(m_blend + (down ? rate : -rate * 3.0f), 0.0f, 1.0f);
    debugBlend = m_blend;
    debugPose  = down ? (std::string("mask ") + std::to_string(mask) +
                        (m_sign > 0 ? " +" : " -") + " " +
                        std::to_string(static_cast<int>(ToDeg(m_angle))) + "deg") : "-";

    if (m_blend <= 0.0f) {
        pivot->transform.position = Vector3::ZERO;
        pivot->transform.rotation = Quaternion::Identity();
        ApplyStagger(*pivot, dt);
        ReleaseChains();
        return;
    }

    /// @note 沈むより先に傾き始めると «宙で回ってから落ちる» に見える。傾きを少し遅らせる。
    const float ease = m_blend * m_blend * (3.0f - 2.0f * m_blend);
    const float rad  = m_angle * ease * m_sign;
    /// @note 沈みと脚の張り出しの重み。«どれだけ倒れたか» は角そのものでなく倒れた高さで
    /// @note 効くので sin を採る ─ 9 度のかしぎでは 0.16、60 度の転倒では 0.87 になる。
    const float amt  = std::sin(std::abs(rad));

    /// @note 沈みは «1 本あたりの荷重» で効く。残り 1 本で 2 本残りと同じだけしか沈まないと、
    /// @note 支えが半分になったのに姿勢が変わらず «片脚で平然と立っている» 絵になる。
    /// @note 2 本残りを 1 とした比 (2/本数) を掛ける ─ 1 本以下は倍まで。
    const int   alive = AliveCount();
    const float load  = std::min(2.0f / static_cast<float>(std::max(alive, 1)), 2.0f);

    /// @note 蝶番の線を «その場に留めたまま» 回す。回転だけでは節の原点が中心になるので、
    /// @note T(P) · R · T(-P) を «回転 + 位置» の 2 つに畳んで入れる。
    const Quaternion rot = Quaternion::FromAxisAngle(m_axis, rad);
    const Vector3    p   = m_hinge;
    const Vector3    moved = rot * p;
    const Vector3    pos{ p.x - moved.x,
                          p.y - moved.y - sinkMeters * amt * load,
                          p.z - moved.z };

    /// @note 崩れ方が切り替わった直後だけ、前の姿勢から渡す。初回の崩れでは m_shift は
    /// @note 最初から 1 なので、ease の掛かった素の値がそのまま出る。
    m_shift = std::clamp(m_shift + rate, 0.0f, 1.0f);
    const float shift = m_shift * m_shift * (3.0f - 2.0f * m_shift);
    pivot->transform.rotation = shift >= 1.0f
        ? rot : Quaternion::Slerp(m_fromRot, rot, shift).Normalized();
    pivot->transform.position = shift >= 1.0f
        ? pos : Vector3::Lerp(m_fromPos, pos, shift);

    /// @note 崩れた姿勢の «上へ» 足す。崩れているほど効きを弱めるのは ApplyStagger 側の仕事。
    ApplyStagger(*pivot, dt);

    /// @note 3 本残っている間はまだ歩ける (BossRigComponent の crippleAtBrokenLegs)。
    /// @note そこで足を床へ縫い付けると、かしいだ体の下で歩行クリップの脚だけが止まって
    /// @note «滑って移動する置物» になる。留めるのは歩けなくなってからにする。
    if (!holdFeet || alive > 2) {
        ReleaseChains();
        return;
    }

    for (int leg = 0; leg < 4; ++leg) {
        IKChain* chain = EnsureChain(leg);
        if (!chain) continue;

        /// @note 壊れた脚は DriveBrokenLegs が畳んで持っている。ここで触ると的が
        /// @note «床へ留める» と «畳む» で毎フレーム奪い合う。
        if (IsBroken(leg)) {
            m_legs[leg].planted = false;
            if (!foldBrokenLegs) {
                chain->enabled = false;
                chain->weight  = 0.0f;
            }
            continue;
        }

        if (!m_legs[leg].planted) continue;

        GameObject* target = m_legs[leg].target.Resolve(scene);
        if (!target) continue;

        /// @note 置き場はボスのローカル系で持っているので、引きずって動いても向き直っても
        /// @note 足は «体に対して同じ所» の床に付いたままになる ─ それが引きずる絵になる。
        const Vector3 origin = self->transform.worldPosition;
        const Vector3 right  = self->transform.Right();
        const Vector3 fwd    = self->transform.Forward();
        /// @note 張り出しは毎フレーム掛ける。Inspector で詰めている最中に効きが変わらないと、
        /// @note 一度崩してから直しても «動かない» ように見える。倒れる量に比例させる ─
        /// @note 1 本失っただけでかしいでいる間に脚だけ全開で開くと «踏ん張り» を通り越す。
        const float   spread = 1.0f + (footSpread - 1.0f) * std::clamp(amt, 0.0f, 1.0f);
        const float   sx  = m_legs[leg].offset.x * spread;
        const float   sz  = m_legs[leg].offset.z * spread;
        Vector3 goal{ origin.x + right.x * sx + fwd.x * sz,
                      groundY,
                      origin.z + right.z * sx + fwd.z * sz };

        /// @note 脚の長さの中へ収める: 体が倒れると付け根は足から遠ざかる。届かない的のまま
        /// @note FABRIK に渡すと脚が真っ直ぐ伸びて的の方を向くだけで足が宙に浮くため、
        /// @note 届く範囲で床の上へ引き寄せる。
        Vector3 hip{};
        if (HipWorld(leg, hip) && m_legs[leg].reach > 0.0f) {
            const float rise = std::max(hip.y - groundY, 0.0f);
            /// @note 床の上で足が置ける半径。付け根が高いほど狭くなる。
            const float span = std::sqrt(
                std::max(m_legs[leg].reach * m_legs[leg].reach - rise * rise, 0.0f)) * 0.98f;
            const float dx = goal.x - hip.x;
            const float dz = goal.z - hip.z;
            const float dist = std::sqrt(dx * dx + dz * dz);
            if (dist > span && dist > 0.001f) {
                const float k = span / dist;
                goal = Vector3{ hip.x + dx * k, groundY, hip.z + dz * k };
            }
        }

        target->transform.position      = goal;
        target->transform.worldPosition = goal;

        chain->enabled = true;
        chain->weight  = std::clamp(ease * std::max(footIkWeight, 0.0f), 0.0f, 1.0f);
    }
}

} /// @note namespace sandbox
