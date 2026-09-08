/// @file    BossCollapsePostureComponent.hpp
/// @brief   脚を失ったボスの «崩れた体» を、再生中のクリップの上から手続き的に作る
/// @author  Hasegawa Jin
/// @date    2026-08-30
///
/// WHY 崩れ方ごとのクリップを焼かないか:
///   焼いた姿勢は «その姿勢のまま何もしない» ときにしか使えない。歩き・旋回・
///   踏みつけ・ビーム・撃破は全部 «四つ脚で立っている» 前提で作ってあるので、
///   崩れた体で 1 つでも出すと、その瞬間だけボスが立ち上がる。崩れ姿勢を
///   «動きの一種» として持つ限り、moveset の数だけ崩れ版が要る。
///
///   ここでは崩れを «姿勢» ではなく «体に掛ける変形» として持つ。胴を傾けて沈め、
///   残った脚を IK で床へ留めるだけなので、上で何のクリップが回っていても成立する。
///   失う組み合わせも 6 通りを列挙せず、残った脚を結ぶ線を蝶番にして解く。
///
/// WHY 蝶番で解くか (前後・左右・対角を場合分けしないか):
///   四足が 2 本失うと、残った 2 本を結ぶ線が唯一の支持になる。体はその線のまわりで
///   «脚を失った側» へ倒れる ─ 前 2 本なら後輪軸を中心に前へ突っ伏し、右列なら
///   左のレールを中心に右へ倒れ、対角なら対角線を中心にねじれる。3 つとも同じ式で、
///   場合分けは «どちら側へ倒すか» の符号だけになる。
///
///   1 本目・3 本目も同じ式で解く。残った足のうち «失った側に近い 2 本» を結ぶ線が
///   蝶番になるのは本数に依らず同じで、変わるのは倒れる量だけ。
///
/// WHY 倒れ角を手で決めないか:
///   四足獣は脚を失えば «立てるところまで» ではなく «床に当たるまで» 倒れる。角度を
///   定数で持つと、前 2 本を失っても右 2 本を失っても同じだけ傾いて止まり、どちらも
///   «途中で見えない何かに支えられている» 絵になる。倒れ角はリグから出す ─ 支えを
///   失った側の脚の付け根が床へ着いた時点が終点で、蝶番までの距離と付け根の高さで
///   決まる (θ = atan2(高さ, 距離))。前 2 本を失えば蝶番までの距離が胴の長さぶん
///   あるので浅く前のめりに、右 2 本を失えば幅しかないので深く横倒しになる ─
///   «どちらが派手か» を手で決めなくても、体の形からその差が出る。
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

/// 崩れた体を作る層。BossRigComponent が «どの脚を失ったか» を押し込む。
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

    // 脚を斬られた «一瞬» の体の反応。崩れ (不可逆) とは別で、必ず立ち姿へ戻る。
    //
    // WHY 崩れと同じコンポーネントへ入れるか: どちらも «ボスの体ごと» の変位で、
    //     書き込む先は VisualPivot 1 つしかない。別のスクリプトから書くと、崩れて
    //     いない間 (m_blend == 0) にこちらが毎フレーム ZERO へ戻すので、よろめきが
    //     1 フレームも残らない。ピボットの書き手は 1 つに保ち、ここで足す。
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

    FBZZ_GROUP("デバッグ")
    // 戦わずに崩れ «方» だけを見る口。倒れる向き・角・沈みの調整はここで回す。
    //
    // WHY 実際にもぐボタン (BossRigComponent の Break FR 等) と別に持つか:
    //   あちらは本番と同じ道を通るので «一度もぐと Stop するまで戻らない»。角度を
    //   1 度ずつ詰める作業には向かない。こちらは見た目だけを差し替えるので、
    //   チェックを外せばその場で立ち姿へ戻る。AI も判定も動かない。
    FBZZ_FIELD(bool, previewFR, false, "Preview FR")
    FBZZ_FIELD(bool, previewFL, false, "Preview FL")
    FBZZ_FIELD(bool, previewBR, false, "Preview BR")
    FBZZ_FIELD(bool, previewBL, false, "Preview BL")
    FBZZ_TOOLTIP("«その脚を失ったことにする» 見た目だけの差し替え。1 つでも入れると"
                 "実際の欠損より優先される。調整が済んだら全部外すこと "
                 "(入れたまま保存すると最初から崩れたボスになる)")
    FBZZ_FIELD_READ_ONLY(std::string, debugPose, "-", "Pose")
    FBZZ_FIELD_READ_ONLY(float, debugBlend, 0.0f, "ブレンド")

    /// 失った脚のビット (FR=1 / FL=2 / BR=4 / BL=8)。壊した側から押し込む。
    ///
    /// WHY こちらから引かないか: 脚の状態を持っているのは BossRigComponent で、
    ///     あちらは既にこちらを include している。引き返すと include が循環する。
    void SetBrokenMask(int mask) { m_mask = mask & 0xF; }
    [[nodiscard]] int  BrokenMask() const
    {
        const int preview = (previewFR ? 1 : 0) | (previewFL ? 2 : 0)
                          | (previewBR ? 4 : 0) | (previewBL ? 8 : 0);
        return preview != 0 ? preview : m_mask;
    }
    /// 崩れ切っているか。0 = 健在 / 1 = 完全に崩れた。
    [[nodiscard]] float Blend() const { return m_blend; }

    /// 脚を斬られた。その脚の側へ体を泳がせる。
    ///
    /// @param towardWorld  傾ける先のワールド座標 (斬られた脚の位置)。水平成分だけ使う。
    /// @param strength     1.0 で staggerDegrees ぶん。溜め斬りは 1 より大きい値を渡す。
    void Stagger(const Vector3& towardWorld, float strength);

    /// 今よろめいている量 [degrees]。HUD や他の演出が読める。
    [[nodiscard]] float StaggerDegrees() const { return ToDeg(m_lean.Length()); }

    void OnUpdate() override;

private:
    /// 崩れ用チェーンの order 起点。
    ///
    /// WHY 引き合い (700) より前に置くか: 崩れは «床へ留める» で、引き合いは
    ///     «中点へ寄せる»。同じ脚に同時に掛かるのは «残り 2 本を引かれて全損する»
    ///     瞬間で、そこは引かれる絵の方を見せたい。後から解いた方が勝つので、
    ///     崩れを先に解いて引き合いに上書きさせる。
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
    /// 蝶番の軸・場所・倒す向き・倒れ角 (rad) を解く。1 本も失っていなければ false。
    [[nodiscard]] bool SolveHinge(Vector3& axisLocal, Vector3& pivotLocal,
                                  float& sign, float& angleRad) const;
    [[nodiscard]] bool FootLocal(int leg, Vector3& out) const;
    /// 脚の付け根 (Thigh)。床に当たって倒れが止まる点。
    [[nodiscard]] bool HipLocal(int leg, Vector3& out) const;
    /// 付け根のワールド位置。IK の的が届くかを測る起点。
    [[nodiscard]] bool HipWorld(int leg, Vector3& out) const;
    /// 付け根から足先までの伸びきった長さ。骨の並びから測る。
    [[nodiscard]] float LegReach(int leg) const;
    /// ボスのローカル系へ落とす。x = 右 / y = 高さ / z = 前。
    [[nodiscard]] bool ToLocal(const GameObject* bone, Vector3& out) const;

    int     m_mask  = 0;
    /// よろめきのバネ。向きは «倒れる先» の水平単位ベクトル、長さは傾き [rad]。
    /// 速度と対で持ち、減衰バネで 0 へ戻る。
    Vector3 m_lean{};
    Vector3 m_leanVel{};

    /// よろめきを VisualPivot の姿勢へ足す。崩れているぶんだけ効きを弱める。
    void ApplyStagger(GameObject& pivot, float dt);

    float   m_blend = 0.0f;
    bool    m_built = false;

    // 崩れ方は «失った脚が変わった瞬間» にだけ解く。
    //
    // WHY 毎フレーム解き直さないか: 蝶番も足の置き場も «今の足の位置» から出している。
    //     崩れ始めた後の足は «自分が傾けて IK で留めた結果» なので、それを次の入力に
    //     すると出力を入力へ混ぜることになり、蝶番がじりじり流れて止まらなくなる。
    //     (解き直す瞬間の値は ToLocal が pivot を割り戻して立ち姿へ揃えている。
    //      それでも毎フレーム解けば IK の分だけ入力が動くので、回数は増やさない)
    int     m_solvedMask = -1;
    bool    m_solved     = false;
    Vector3 m_axis{};
    Vector3 m_hinge{};
    float   m_sign  = 1.0f;
    float   m_angle = 0.0f;   ///< 倒れきったときの角 (rad)

    // 既に崩れている体が «別の崩れ方» へ移るとき (2 本目の後に 3 本目を失う等) の
    // 繋ぎ。蝶番も向きも変わるので、解き直した姿勢をそのまま書くと 1 フレームで
    // 別の倒れ方へ飛ぶ。前の姿勢から新しい姿勢へ渡す。
    Quaternion m_fromRot = Quaternion::Identity();
    Vector3    m_fromPos{};
    float      m_shift = 1.0f;

    struct LegPlant {
        EntityRef target;
        /// 崩れ始めに控えた、ボスのローカル系での足の置き場。
        Vector3   offset;
        /// 付け根から足先までの長さ。骨の間隔は姿勢に依らないので 1 度測れば足りる。
        float     reach   = 0.0f;
        bool      planted = false;
    };
    LegPlant m_legs[4];
};

FBZZ_REFLECT(BossCollapsePostureComponent)

inline const char* BossCollapsePostureComponent::SuffixOf(int leg)
{
    static constexpr const char* kSuffix[4] = { "_FR", "_FL", "_BR", "_BL" };
    return kSuffix[std::clamp(leg, 0, 3)];
}

inline void BossCollapsePostureComponent::EnsureTargets()
{
    // WHY 毎回名前で拾い直すか: スクリプト DLL をリロードすると Script は作り直され、
    //     EntityRef は空へ戻る。作った GameObject は Scene に残るので、«作った» を
    //     覚えたままだと的が 1 組ずつ増え続ける。
    if (m_built && m_legs[0].target.Resolve(scene)) return;

    GameObject* self = scene.Self();
    if (!self) return;
    auto* ik = self->GetComponent<IKSolverComponent>();
    if (!ik) ik = &self->AddComponent<IKSolverComponent>();
    ik->enabled = true;

    for (int leg = 0; leg < 4; ++leg) {
        const std::string name = std::string("BossCollapseTarget") + SuffixOf(leg);
        GameObject* target = scene.Find(name);
        if (!target) {
            // scene.Create は GameObject 配列を再確保するので、掴んだポインタを
            // 跨いで作らない。ここは «作って即しまう» だけに留める。
            GameObject& created      = scene.Create(name);
            created.runtimeGenerated = true;
            target = &created;
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
    // README のリグ構成どおり 4 節。TwoBone は 3 本しか受けない。
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
        for (IKChain& chain : ik->chains)
            if (chain.type == IKSolverType::FABRIK &&
                chain.order >= kChainOrder && chain.order < kChainOrder + 4) {
                chain.enabled = false;
                chain.weight  = 0.0f;
            }
    // 置き場は消さない。植え直すのは «失った脚が変わったとき» だけで、
    // ここで消すと崩れた直後の 1 フレームで控えた値が飛ぶ。
}

inline bool BossCollapsePostureComponent::ToLocal(const GameObject* bone, Vector3& out) const
{
    GameObject* self = scene.Self();
    if (!bone || !self) return false;

    // ボスの向きは戦闘中ずっと変わる。ワールドのまま控えると、向き直った瞬間に
    // 足の置き場が体の反対側へ回り込む。自分の右前ベクトルで分解して持つ。
    const Vector3 d = bone->transform.worldPosition - self->transform.worldPosition;
    Vector3 local{ Vector3::Dot(d, self->transform.Right()),
                   d.y,
                   Vector3::Dot(d, self->transform.Forward()) };

    // 自分が pivot へ掛けた変形を外して «立っていたときの» 値へ戻す。
    //
    // WHY 要るか: 骨のワールド位置は崩れた後の姿勢そのもの。3 本目を失って解き直す
    //     ときの入力がそれだと、もう倒れている体を «これから倒す体» として測ることに
    //     なる。失った脚の付け根はとっくに床の高さまで降りているので «倒れ代 0» と
    //     出て、崩れ自体が取り消され、立ち姿へ戻ってしまう。
    //     出力を入力へ混ぜないよう、測る前に自分の変形だけ割り戻す。
    if (GameObject* pivot = scene.Find(pivotName)) {
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
    // 付け根は当たり判定を持たないので RigComponent には控えられていない。
    // 名前で引く ─ 骨名はシーン内で一意でないため、必ずボスの部分木だけを見る。
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

    // README のリグ構成どおり Thigh → Shin → Hock → Foot。節の間隔は姿勢を変えても
    // 変わらないので、立っている間に測った値がそのまま伸びきった長さになる。
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

    // 倒れる先は «失った脚の重心»。対角では両側が釣り合って 0 になる ─
    // そのときはどちらでもよく、符号を固定してねじれの向きを一定にする。
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
        // 蝶番は «失った側に近い 2 本» を結ぶ線。残り 2 本ならその 2 本しかなく、
        // 3 本なら失った角を挟む 2 本が選ばれる (残る 1 本は反対側で、体はそれを
        // 乗り越えて倒れるのではなく、その線の上でかしぐ)。
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
        // 残り 1 本以下。線が引けないので、その足を通る «倒れる向きに直交する» 線を
        // 蝶番にする。四本とも失えば倒れる先も消えるので、前へ突っ伏す向きを既定にする。
        const Vector3 f = aliveCount == 1 ? foot[alive[0]] : Vector3{ 0.0f, 0.0f, 0.0f };
        Vector3 away{ lost.x - f.x, 0.0f, lost.z - f.z };
        const float   awayLen = std::sqrt(away.x * away.x + away.z * away.z);
        away = awayLen < 0.01f
            ? Vector3{ 0.0f, 0.0f, 1.0f }
            : Vector3{ away.x / awayLen, 0.0f, away.z / awayLen };
        a = Vector3{ f.x - away.z, f.y, f.z + away.x };
        b = Vector3{ f.x + away.z, f.y, f.z - away.x };
    }

    // 蝶番の «向き»。水平成分だけを見る。
    Vector3 hinge{ b.x - a.x, 0.0f, b.z - a.z };
    const float len = std::sqrt(hinge.x * hinge.x + hinge.z * hinge.z);
    if (len < 0.01f) return false;
    hinge     = Vector3{ hinge.x / len, 0.0f, hinge.z / len };
    axisLocal = hinge;

    // 蝶番の «場所»。選んだ 2 本の足の中点で、高さは接地面。
    //
    // WHY 原点まわりに回さないか: VisualPivot の原点はボスの足元中央にある。そこを
    //     中心に回すと、4.5m 上にある胴が横へ大きく振り出される ─ «倒れた» では
    //     なく «横っ飛びした» 絵になる。実際に体が回るのは残った足を結ぶ線の上で、
    //     その線は原点を通らない (前 2 本を失えば後輪軸、右列を失えば左のレール)。
    const Vector3 mid{ (a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f, (a.z + b.z) * 0.5f };
    pivotLocal = mid;
    // 蝶番に直交する水平ベクトル。hinge × up がこれになる。
    const Vector3 perp{ -hinge.z, 0.0f, hinge.x };
    const float   side = (lost.x - mid.x) * perp.x + (lost.z - mid.z) * perp.z;

    // 蝶番のまわりに角 θ で回すと、点 r は θ·(hinge × r) へ動く (Quaternion の
    // v + 2w(qv×v) + 2qv×(qv×v) を θ で 1 次展開した形)。真上にある点なら
    // hinge × up = perp なので «+θ で perp 側へ» 倒れる。失った側 (side > 0 が
    // +perp 側) へ倒したいので、符号はそのまま合わせる。
    //
    // WHY ここを間違えると気付きにくいか: 符号を反転すると «残っている脚の方へ»
    //     倒れる。前 2 本を失って後ろへのけぞる絵になるが、崩れること自体は
    //     起きているので «倒れない» ではなく «なんか変» としか見えない。
    //     対角に失ったときは side が 0 になり、どちらへ倒しても同じ。
    sign = side >= 0.0f ? 1.0f : -1.0f;

    // 倒れ角。支えを失った側の付け根が床へ着いたら止まる。蝶番から付け根までの
    // 水平距離 d と付け根の高さ h に対して、θ = atan2(h, d) で床に触れる。
    // 複数本失っていれば «最初に着いた 1 本» が体を止めるので、最小角を採る。
    float contact = -1.0f;
    for (int leg = 0; leg < 4; ++leg) {
        Vector3 hip{};
        if (!IsBroken(leg) || !HipLocal(leg, hip)) continue;
        // 倒れる側にある付け根だけが床へ近づく。蝶番の反対側にあるものは «上がる»
        // ので、絶対値で測ると «上がっていく点が床に着く» という嘘の終点になる。
        const float d = ((hip.x - mid.x) * perp.x + (hip.z - mid.z) * perp.z) * sign;
        if (d <= 0.0f) continue;
        const float h = std::max(hip.y - mid.y, 0.0f);
        const float t = std::atan2(h, std::max(d, 0.05f));
        if (contact < 0.0f || t < contact) contact = t;
    }
    // 付け根が引けないリグでも «崩れない» にはしない。四足の胴を横倒しにする
    // ときの目安として 45 度を置く。
    if (contact < 0.0f) contact = ToRad(45.0f);

    // 3 本残っているうちはまだ三点で立てる。床当たりまで倒すと «脚が 1 本折れた
    // だけで転倒する» になるので、ここだけは撓みぶんの角で止める。
    //
    // WHY 既定を 0 にしてあるか: 三点で接地している剛体はそもそも回れない。蝶番に
    //     選べるのは 3 本のうち 2 本だけで、残る 1 本は必ず軸から外れているから、
    //     少しでも回すとその足が床から浮く。浮かせずに «撓ませる» には脚ごとに IK が
    //     要るが、この段階のボスはまだ歩くので足を留めると滑る。両立しない。
    angleRad = aliveCount >= 3 ? ToRad(sagDegrees)
                               : contact * std::max(fallRatio, 0.0f);
    // 90 度を越えると «倒れる» ではなく «裏返る»。リグが壊れていても超えさせない。
    angleRad = std::clamp(angleRad, 0.0f, ToRad(85.0f));
    return angleRad > 0.0f;
}

inline void BossCollapsePostureComponent::Stagger(const Vector3& towardWorld, float strength)
{
    if (!stagger || staggerDegrees <= 0.0f) return;

    GameObject* self = scene.Self();
    if (!self) return;

    // 水平成分だけを見る。上下を混ぜると «斬られて浮く» になる。
    const Vector3 d = towardWorld - self->transform.worldPosition;
    const Vector3 toward = Vector3{ d.x, 0.0f, d.z }.NormalizedOr(Vector3::ZERO);
    if (toward.LengthSq() <= EPSILON) return;

    // 速度へ足す。位置へ直接入れると «瞬間移動してから戻る» になり、当たった
    // 衝撃ではなく «別のポーズへ切り替わった» ように見える。
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

    // 減衰バネ。刻みが荒いフレームで発散しないよう、1 回の更新量を 1/60 で切る。
    const float step = std::min(std::max(dt, 0.0f), 1.0f / 60.0f);
    m_leanVel += (m_lean * -staggerStiffness - m_leanVel * staggerDamping) * step;
    m_lean    += m_leanVel * step;

    // 連撃で回り続けないよう頭打ちにする。速度ごと削らないと、次のフレームで
    // また上限を超えて «上限に張り付いたまま震える» になる。
    const float maxLean = ToRad(std::max(staggerMaxDegrees, 0.0f));
    const float amount  = m_lean.Length();
    if (maxLean > 0.0f && amount > maxLean) {
        const Vector3 dir = m_lean / amount;
        m_lean    = dir * maxLean;
        m_leanVel -= dir * std::max(Vector3::Dot(m_leanVel, dir), 0.0f);
    }

    if (m_lean.LengthSq() <= EPSILON) return;

    // 崩れているほど弱める。倒れ切った体がまだ泳いでいると «死体が呼吸している» になる。
    const float gain = 1.0f - Clamp01(m_blend);
    const float lean = m_lean.Length() * gain;
    if (lean <= EPSILON) return;

    const Vector3 dir = m_lean.NormalizedOr(Vector3::ZERO);
    // 傾く先が dir なので、回転軸はその左右。UP との外積で取る。
    const Vector3 axis = Vector3::Cross(Vector3::UP, dir).NormalizedOr(Vector3::ZERO);
    if (axis.LengthSq() <= EPSILON) return;

    pivot.transform.rotation =
        (Quaternion::FromAxisAngle(axis, lean) * pivot.transform.rotation).Normalized();
    pivot.transform.position.y -= staggerSink * std::sin(lean);
}

inline void BossCollapsePostureComponent::OnUpdate()
{
    GameObject* self  = scene.Self();
    GameObject* pivot = self ? scene.Find(pivotName) : nullptr;
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

    // 失った脚が変わった瞬間にだけ解く。2 本目の後に 3 本目を失えば蝶番も倒れる
    // 向きも変わるので、そのたびに解き直す (測る値は ToLocal が立ち姿へ戻している)。
    const int mask = BrokenMask();
    if (mask != m_solvedMask) {
        m_solvedMask = mask;
        for (LegPlant& leg : m_legs) leg.planted = false;
        // 既に崩れているなら、今の姿勢を繋ぎの起点として控える。
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
    const bool down = collapse && m_solved;

    // 崩れは不可逆なので戻りは «切られたとき» だけ。入りだけ時間を掛ける。
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

    // 沈むより先に傾き始めると «宙で回ってから落ちる» に見える。傾きを少し遅らせる。
    const float ease = m_blend * m_blend * (3.0f - 2.0f * m_blend);
    const float rad  = m_angle * ease * m_sign;
    // 沈みと脚の張り出しの重み。«どれだけ倒れたか» は角そのものでなく倒れた高さで
    // 効くので sin を採る ─ 9 度のかしぎでは 0.16、60 度の転倒では 0.87 になる。
    const float amt  = std::sin(std::abs(rad));

    // 沈みは «1 本あたりの荷重» で効く。残り 1 本で 2 本残りと同じだけしか沈まないと、
    // 支えが半分になったのに姿勢が変わらず «片脚で平然と立っている» 絵になる。
    // 2 本残りを 1 とした比 (2/本数) を掛ける ─ 1 本以下は倍まで。
    const int   alive = AliveCount();
    const float load  = std::min(2.0f / static_cast<float>(std::max(alive, 1)), 2.0f);

    // 蝶番の線を «その場に留めたまま» 回す。回転だけでは節の原点が中心になるので、
    //     T(P) · R · T(-P) を «回転 + 位置» の 2 つに畳んで入れる。
    const Quaternion rot = Quaternion::FromAxisAngle(m_axis, rad);
    const Vector3    p   = m_hinge;
    const Vector3    moved = rot * p;
    const Vector3    pos{ p.x - moved.x,
                          p.y - moved.y - sinkMeters * amt * load,
                          p.z - moved.z };

    // 崩れ方が切り替わった直後だけ、前の姿勢から渡す。初回の崩れでは m_shift は
    // 最初から 1 なので、ease の掛かった素の値がそのまま出る。
    m_shift = std::clamp(m_shift + rate, 0.0f, 1.0f);
    const float shift = m_shift * m_shift * (3.0f - 2.0f * m_shift);
    pivot->transform.rotation = shift >= 1.0f
        ? rot : Quaternion::Slerp(m_fromRot, rot, shift).Normalized();
    pivot->transform.position = shift >= 1.0f
        ? pos : Vector3::Lerp(m_fromPos, pos, shift);

    // 崩れた姿勢の «上へ» 足す。崩れているほど効きを弱めるのは ApplyStagger 側の仕事。
    ApplyStagger(*pivot, dt);

    // 3 本残っている間はまだ歩ける (BossRigComponent の crippleAtBrokenLegs)。
    // そこで足を床へ縫い付けると、かしいだ体の下で歩行クリップの脚だけが止まって
    // «滑って移動する置物» になる。留めるのは歩けなくなってからにする。
    if (!holdFeet || alive > 2) {
        ReleaseChains();
        return;
    }

    const float groundY = self->transform.worldPosition.y + groundOffset;

    for (int leg = 0; leg < 4; ++leg) {
        IKChain* chain = EnsureChain(leg);
        if (!chain) continue;

        if (IsBroken(leg)) {
            chain->enabled = false;
            chain->weight  = 0.0f;
            m_legs[leg].planted = false;
            continue;
        }

        if (!m_legs[leg].planted) continue;

        GameObject* target = m_legs[leg].target.Resolve(scene);
        if (!target) continue;

        // 置き場はボスのローカル系で持っているので、引きずって動いても向き直っても
        // 足は «体に対して同じ所» の床に付いたままになる ─ それが引きずる絵になる。
        const Vector3 origin = self->transform.worldPosition;
        const Vector3 right  = self->transform.Right();
        const Vector3 fwd    = self->transform.Forward();
        // 張り出しは毎フレーム掛ける。Inspector で詰めている最中に効きが変わらないと、
        // 一度崩してから直しても «動かない» ように見える。倒れる量に比例させる ─
        // 1 本失っただけでかしいでいる間に脚だけ全開で開くと «踏ん張り» を通り越す。
        const float   spread = 1.0f + (footSpread - 1.0f) * std::clamp(amt, 0.0f, 1.0f);
        const float   sx  = m_legs[leg].offset.x * spread;
        const float   sz  = m_legs[leg].offset.z * spread;
        Vector3 goal{ origin.x + right.x * sx + fwd.x * sz,
                      groundY,
                      origin.z + right.z * sx + fwd.z * sz };

        // 脚の長さの中へ収める。
        //
        // WHY 要るか: 体が倒れると付け根は蝶番のまわりを回って足から遠ざかる。届かない
        //     所を指したまま FABRIK に渡すと、脚は «真っ直ぐ伸びて的の方を向く» だけで
        //     終わり、足は的まで届かず宙で止まる ─ 床に置いたはずの足が浮く。
        //     的を引き寄せて «届く範囲で床の上» へ移す。実際の四足も支えきれなく
        //     なれば足を内へ入れるので、絵としてもこちらが正しい。
        Vector3 hip{};
        if (HipWorld(leg, hip) && m_legs[leg].reach > 0.0f) {
            const float rise = std::max(hip.y - groundY, 0.0f);
            // 床の上で足が置ける半径。付け根が高いほど狭くなる。
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

} // namespace sandbox
