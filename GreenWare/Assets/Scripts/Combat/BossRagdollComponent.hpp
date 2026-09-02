/// @file    BossRagdollComponent.hpp
/// @brief   転倒は脱力で落とし、被弾のひるみは筋力を入れたまま押す
/// @author  Hasegawa Jin
/// @date    2026-09-01
///
/// WHY 常時 «脱力» ラグドールにしないか:
///   21 クリップは踏みつけの «f16-20 で完全静止＝予兆» までフレーム単位で詰めてある。
///   歩行と攻撃を脱力させると、この読みやすさが最初に失われる。予兆が読めることが
///   この戦いのフェアさの根拠なので、脱力させるのは «崩れる瞬間» だけに限る。
///   被弾のひるみは筋力を入れた側 (Active) が持つ ── 釣り合い点がクリップそのものに
///   なるので、予兆の絵を 1 フレームも崩さずに «押されて沈む» だけを足せる。
///
/// WHY 倒れ «終わる» 前にクリップへ戻すか:
///   質点系が行き着く先は床の上の山で、そこに «倒れているボス» の絵は無い。
///   落ち始めの 1〜2 秒だけ物理で見せ、無防備な残りは既存の Boss_Crash が持つ。
///   崩れ方だけが毎回変わり、隙の絵は毎回同じ ─ 変える所と変えない所を分ける。
#pragma once

#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <algorithm>
#include <string>

using namespace fbzz::scene;
using namespace fbzz::math;

namespace sandbox {

class BossRagdollComponent : public Script {
    FBZZ_SCRIPT(BossRagdollComponent)

public:
    FBZZ_GROUP("Ragdoll")
    FBZZ_FIELD(bool, useRagdoll, true, "Use Ragdoll")
    FBZZ_TOOLTIP("切ると転倒が Boss_Crash だけになる。物理を入れる前と比べるための口")
    // 空ならスケルトンの根から。胴体ごと落とすので既定は空でよい。
    FBZZ_FIELD(std::string, rootBone, "", "Root Bone")
    FBZZ_TOOLTIP("落とし始める骨。空でスケルトンの根 ＝ 全身。脚だけ落とすなら Thigh_FR など")
    FBZZ_FIELD_RANGE_INT(int, maxDepth, 0, "Max Depth", 0, 16)
    FBZZ_TOOLTIP("根から何段まで質点にするか。0 で葉まで")

    FBZZ_FIELD_RANGE(float, gravity, 26.0f, "Gravity", 0.0f, 80.0f)
    FBZZ_TOOLTIP("実測の 9.8 では «ゆっくり傾く» にしかならない。重機が落ちる速さは"
                 "現実より速い方が伝わる")
    FBZZ_FIELD_RANGE(float, blendIn, 0.05f, "Blend In", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, blendOut, 0.60f, "Blend Out", 0.0f, 2.0f)
    FBZZ_TOOLTIP("物理の «床の上の山» から Boss_Crash の姿勢へ戻る秒数。"
                 "短いと崩れ切った形から別の形へ飛ぶ")

    FBZZ_GROUP("Topple")
    FBZZ_FIELD_RANGE(float, holdRatio, 0.55f, "Hold Ratio", 0.05f, 1.0f)
    FBZZ_TOOLTIP("転倒の無防備時間のうち、何割を物理に任せるか。残りは Boss_Crash。"
                 "1.0 にすると起き上がる直前まで物理のまま")
    FBZZ_FIELD_RANGE(float, fallPush, 2.6f, "Fall Push", 0.0f, 20.0f)
    FBZZ_TOOLTIP("支えを失った側へ体を倒す速さ [m/s]。全質点へ一律に掛かる")
    FBZZ_FIELD_RANGE(float, fallLift, 0.6f, "Fall Lift", 0.0f, 8.0f)
    FBZZ_TOOLTIP("わずかに浮かせて «崩れ落ちる» を作る。上げすぎると跳ねて見える")
    FBZZ_FIELD_RANGE(float, pairPush, 7.0f, "Pair Push", 0.0f, 30.0f)
    FBZZ_TOOLTIP("引き合った 2 本を互いへ弾く速さ [m/s]。ここが «磁力で寄った» の正体")
    FBZZ_FIELD_RANGE(float, pairPushRadius, 4.5f, "Pair Radius", 0.5f, 20.0f)
    FBZZ_TOOLTIP("脚 1 本を弾く影響半径。広げると胴体まで持っていかれる")

    // 斬られたときの «押されて泳ぐ»。転倒と同じ質点系を、筋力を入れたまま使う。
    //
    // WHY 転倒 (脱力) ではなく筋力を入れるか: 脱力した体には «立っている» という
    //     行き先が無く、押した分だけ崩れて戻らない。だから以前は適用率を 0.45 まで
    //     下げ、重力を 0.12 に絞って «崩れ切る前に» クリップへ逃がしていた ── 薄く
    //     乗せた結果、歩行クリップも半分消えて脚が滑る。筋力を入れると釣り合い点が
    //     «今のアニメーション» になるので、適用率 1・重力そのままで «押されて沈み、
    //     こらえて戻る» が出る。歩行は下に残るのではなく、そのまま再生され続ける。
    FBZZ_GROUP("Stagger")
    FBZZ_FIELD(bool, useStagger, true, "Use Stagger")
    FBZZ_TOOLTIP("斬られたときに体を物理で押す。切ると脚だけが反応する")
    FBZZ_FIELD_RANGE(float, staggerSeconds, 0.30f, "Hold", 0.0f, 1.0f)
    FBZZ_TOOLTIP("押されてから立ち姿へ戻し始めるまでの秒数。"
                 "Recovery より短いと «こらえ直す» 途中でクリップへ返ることになる")
    FBZZ_FIELD_RANGE(float, staggerMuscle, 0.34f, "Muscle", 0.0f, 1.0f)
    FBZZ_TOOLTIP("引き戻す強さ。上げるほどびくともせず、下げるほど大きくひるむ")
    FBZZ_FIELD_RANGE(float, staggerFalloff, 0.84f, "Falloff", 0.0f, 1.0f)
    FBZZ_TOOLTIP("根から 1 段ごとの筋力の落ち方。下げるほど胴が残って脚先だけ流れる")
    FBZZ_FIELD_RANGE(float, staggerMuscleDamping, 0.20f, "Damping", 0.0f, 1.0f)
    FBZZ_TOOLTIP("揺り返しの削り方。0 に近いと戻り際にぶるぶる残る")
    FBZZ_FIELD_RANGE(float, staggerSlack, 0.55f, "Slack", 0.0f, 1.0f)
    FBZZ_TOOLTIP("当たった瞬間に抜ける力み。0 で «硬い体が少しめり込んで即戻る» ＝ 手応えが無い")
    FBZZ_FIELD_RANGE(float, staggerRecovery, 0.28f, "Recovery", 0.0f, 2.0f)
    FBZZ_TOOLTIP("抜けた力みが戻るまでの秒数。ここが «こらえ直す» の長さ")
    FBZZ_FIELD_RANGE(float, staggerBlendIn, 0.03f, "Blend In", 0.0f, 0.5f)
    FBZZ_FIELD_RANGE(float, staggerBlendOut, 0.22f, "Blend Out", 0.0f, 2.0f)
    FBZZ_TOOLTIP("物理を降ろす秒数。戻り切った後で降ろすので、ここは短くてよい")
    FBZZ_FIELD_RANGE(float, staggerPush, 3.4f, "Push", 0.0f, 30.0f)
    FBZZ_TOOLTIP("体全体が押される速さ [m/s]。ノックバックの本体")
    FBZZ_FIELD_RANGE(float, staggerLocalPush, 5.0f, "Local Push", 0.0f, 40.0f)
    FBZZ_TOOLTIP("当たった所だけ余分に押す速さ。斬られた脚が先に流れて体が付いてくる")
    FBZZ_FIELD_RANGE(float, staggerLocalRadius, 3.6f, "Local Radius", 0.5f, 20.0f)

    FBZZ_GROUP("Debug")
    FBZZ_FIELD_READ_ONLY(bool, debugActive, false, "Active")
    FBZZ_FIELD_READ_ONLY(float, debugWeight, 0.0f, "Weight")
    // 骨が動かない理由を名指しする。Play 中に «Idle のまま» なら呼ばれていない、
    // «NoComponent» ならこのスクリプトが Animator と別の GameObject に付いている、
    // «NoParticles» なら Root Bone が骨の名前と一致していない。
    FBZZ_FIELD_READ_ONLY(std::string, debugStatus, "Idle", "Status")
    FBZZ_TOOLTIP("Idle / Running / NoComponent / NoAnimator / NoSkinnedMesh / "
                 "NoParticles / NoBones のいずれか")
    FBZZ_FIELD_READ_ONLY(int, debugParticles, 0, "Particles")
    FBZZ_TOOLTIP("組めた質点の数。0 なら Root Bone がスケルトンに無い")
    FBZZ_FIELD_READ_ONLY(float, debugDeviation, 0.0f, "Deviation")
    FBZZ_TOOLTIP("クリップの姿勢から一番離れた骨の距離 [m]。ひるみの «効き» の実測値。"
                 "Push を上げても伸びないなら Muscle が強すぎる")

    // 対を組まずに崩れ方だけ見るための口。引き合いを 2 回通してからでないと
    // 一度も見られない、では 1 回の確認に数分かかる (Break FR ボタンと同じ理由)。
    void DebugFall();
    FBZZ_BUTTON(DebugFall, "Test Fall")
    FBZZ_TOOLTIP("Play 中に押すとその場で倒れる。戻すには Stop → Play")

    /// 今の姿勢のまま落とし始める。壁への激突など、方向を作らない転倒はこちら。
    void Begin(float toppleSeconds);
    /// 引き合った 2 本の足元を渡して落とす。倒れる向きが対の位置で決まる。
    void BeginFromPair(const Vector3& footA, const Vector3& footB, float toppleSeconds);
    /// 斬られた。押された方向へ体を泳がせ、勝手に立ち姿へ戻る。
    ///
    /// 転倒中は何もしない ── 倒れている体を «よろめかせる» と、崩れかけの姿勢を
    /// 捕獲し直して転倒がそこから始め直しになる。押すのは PushAt の担当。
    ///
    /// @param hitPoint  当たった場所。ここだけ余分に押して «斬られた脚が先に流れる»。
    /// @param direction 押す向き (水平)。プレイヤーから部位への向き ＝ ノックバック。
    /// @param strength  1.0 で既定値ぶん。溜め斬りは 1 より大きい値を渡す。
    void Stagger(const Vector3& hitPoint, const Vector3& direction, float strength);
    [[nodiscard]] bool IsStaggering() const { return m_staggering && ragdoll.IsActive(); }
    /// よろめきを物理で出すか。false なら呼び出し側が別の手 (傾け) へ落とす。
    [[nodiscard]] bool UsesStagger() const { return useRagdoll && useStagger; }

    /// クリップへ戻し始める。転倒が早く明けたときに呼ぶ。
    void Stop();
    [[nodiscard]] bool IsActive() const { return ragdoll.IsActive(); }

    /// 一点を押す。倒れている体へ斬撃を当てたときの反応がこれ。
    ///
    /// 立っているボスを押すのは Stagger() の担当 ── あちらは筋力を入れたまま押すので
    /// 押し返しが返るが、こちらは既に脱力しているので押した分だけ崩れる。同じ Push でも
    /// «こらえる» と «崩れる» に分かれるのは、筋力が入っているかどうかだけの違い。
    void PushAt(const Vector3& origin, const Vector3& direction,
                float speed, float radius) const
    {
        if (!ragdoll.IsActive()) return;
        ragdoll.PushAt(origin, direction.NormalizedOr(Vector3::ZERO) * speed, radius);
    }

    void OnStart()  override;
    void OnUpdate() override;

private:
    /// Inspector の値を実体へ流す。毎フレーム呼んでも差分しか動かない。
    void Configure();

    /// 今動いているのが «よろめき» か «転倒» か。押し直してよいのは前者だけ。
    bool m_staggering = false;
};

FBZZ_REFLECT(BossRagdollComponent)

inline void BossRagdollComponent::Configure()
{
    ragdoll.SetRoot(rootBone.c_str(), std::max(maxDepth, 0));
    ragdoll.SetGravity(gravity);
    ragdoll.SetBlend(blendIn, blendOut);
    ragdoll.SetEnabled(useRagdoll);
}

inline void BossRagdollComponent::OnStart()
{
    Configure();
}

inline void BossRagdollComponent::OnUpdate()
{
    debugActive    = ragdoll.IsActive();
    debugWeight    = ragdoll.GetWeight();
    debugStatus    = ragdoll.GetStatus();
    debugParticles = ragdoll.GetParticleCount();
    debugDeviation = ragdoll.GetDeviation();
    if (!debugActive) m_staggering = false;
}

inline void BossRagdollComponent::Stagger(const Vector3& hitPoint,
                                          const Vector3& direction,
                                          float strength)
{
    if (!useRagdoll || !useStagger) return;
    // 転倒中は触らない。崩れかけの姿勢を捕獲し直すと、そこから転倒が始め直しになる。
    if (ragdoll.IsActive() && !m_staggering) return;

    const Vector3 push = direction.NormalizedOr(Vector3::ZERO);
    if (push.LengthSq() <= EPSILON) return;
    const float scale = std::max(strength, 0.0f);

    ragdoll.SetRoot(rootBone.c_str(), std::max(maxDepth, 0));
    ragdoll.SetGravity(gravity);
    ragdoll.SetBlend(staggerBlendIn, staggerBlendOut);
    ragdoll.SetMuscle(staggerMuscle, staggerFalloff, staggerMuscleDamping);
    ragdoll.SetRecovery(staggerSlack, staggerRecovery);
    // WHY ひるみでは崩落させないか: 転倒は «対を組み違えた結果» が持つ一枚看板で、
    //     斬るたびに倒れるとその 1 回が安くなる。ひるみは «押しても立っている» が
    //     結論で、倒すかどうかは AI が決める。
    ragdoll.SetCollapse(0.0f);
    ragdoll.SetEnabled(true);
    // 既に泳いでいる最中でも呼んでよい。Active は捕獲し直さず保つ秒数を延ばすだけで、
    // 目標はどのみち毎フレームのクリップから取り直される。
    ragdoll.BeginActive(std::max(staggerSeconds, 0.0f));
    m_staggering = true;

    ragdoll.Push(push * (staggerPush * scale));
    // 当たった所だけ余分に押す。斬られた脚が先に流れ、胴が遅れて付いてくる。
    ragdoll.PushAt(hitPoint, push * (staggerLocalPush * scale), staggerLocalRadius);
}

inline void BossRagdollComponent::DebugFall()
{
    // WHY 編集中を弾くか: 捕獲するのは «今の骨の姿勢» で、それを決めているのは
    //     Animator。停止中はクリップが進んでいないので、バインドポーズを捕まえて
    //     そこから落ちる ── 本番と違う崩れ方を見ながら数値を調整することになる。
    if (!app.IsPlaying()) {
        debug.LogWarning("BossRagdollComponent: Test Fall works only during Play.");
        return;
    }
    Configure();
    m_staggering = false;
    ragdoll.Begin(0.0f, 1.0f, 1.0f);
    ragdoll.Push(Vector3{ 0.0f, fallLift, 0.0f });
}

inline void BossRagdollComponent::Begin(float toppleSeconds)
{
    if (!useRagdoll) return;
    // 転倒はよろめきを上書きしてよい。適用率も重力も «丸ごと物理» へ戻す。
    Configure();
    m_staggering = false;
    ragdoll.Begin(std::max(toppleSeconds, 0.1f) * Clamp01(holdRatio), 1.0f, 1.0f);
}

inline void BossRagdollComponent::BeginFromPair(const Vector3& footA,
                                                const Vector3& footB,
                                                float toppleSeconds)
{
    if (!useRagdoll) return;
    Begin(toppleSeconds);

    const Vector3 midpoint = (footA + footB) * 0.5f;

    // 2 本を互いへ弾く。IK が «寄せて» 見せていた 0.9 秒の結末を、ここで実際に起こす。
    ragdoll.PushAt(footA, (midpoint - footA).NormalizedOr(Vector3::ZERO) * pairPush,
                   pairPushRadius);
    ragdoll.PushAt(footB, (midpoint - footB).NormalizedOr(Vector3::ZERO) * pairPush,
                   pairPushRadius);

    // 組んだ 2 本の側が支えを失う。体はそちらへ倒れる ── どの対を組んだかが
    // そのまま «どちらへ倒れるか» になり、6 通りの対が初めて別々の結果を持つ。
    const Vector3 self = transform.worldPosition;
    const Vector3 toward =
        Vector3{ midpoint.x - self.x, 0.0f, midpoint.z - self.z }
            .NormalizedOr(Vector3::ZERO);
    ragdoll.Push(toward * fallPush + Vector3::UP * fallLift);
}

inline void BossRagdollComponent::Stop()
{
    ragdoll.End();
}

} // namespace sandbox
