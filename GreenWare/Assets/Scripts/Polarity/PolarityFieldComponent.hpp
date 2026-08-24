// FBZZ Engine
// PolarityFieldComponent.hpp | sandbox
// 盤面全体の引力を仕切る 1 体だけのスクリプト。シーンの管理用 GameObject に付ける。
//
// 担当は 2 つ。
//   1. 誰と誰が引き合うかを毎フレーム決め直す (7.3 の引力条件)
//   2. その結果起きた衝突を 1 箇所で解決する (7.4 / 12.6 / 17 章の手触り)
//
// WHY 各自に最寄りを探させないか:
//   敵が自分で相手を選ぶと、A は B を、B は C を見る、という食い違いが起きる。
//   引力は 2 者の関係なので、片方だけが引かれている状態は成立しない。
//   企画書の「誰と誰を、どの順番で結びつけるか」を決めるのはプレイヤーであり、
//   その結果の解釈は 1 箇所に閉じていないと、撃った結果が読めなくなる。
//
// WHY 1 対象につきリンクを 1 本に絞るか:
//   軽い敵が 2 つのアンカーから同時に引かれると、間で引き裂かれて動きが濁る。
//   17 章が最重要とした「ギュンッ」は方向が 1 つに決まって初めて出る。近い順に
//   貪欲に組み、既に組まれた対象は次のリンクに参加させない。
#pragma once

#include <Engine/Scene/Components/CameraRigComponents.hpp>
// WHY Scene.hpp まで要るか: FindObjectsOfType / GetScript の実体が Scene.hpp の末尾にある。
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Scripts/Data/PolarityTuning.hpp>
#include <Scripts/Game/ImpactFeedbackManagerComponent.hpp>
#include <Scripts/Game/VfxManagerComponent.hpp>
#include <Scripts/Polarity/PolarityBodyComponent.hpp>
#include <Scripts/Polarity/PolarityTargetComponent.hpp>
#include <Scripts/Utils/BodyBounds.hpp>
#include <Scripts/Utils/ElectricArc.hpp>
#include <Scripts/Utils/PolarityTypes.hpp>
#include <algorithm>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

// コライダーが測れない対象の全高。Mite 相当の大きさ (VfxManagerComponent と同じ前提)。
inline constexpr float kArcFallbackHeight = 1.2f;

class PolarityFieldComponent : public Script {
    FBZZ_SCRIPT(PolarityFieldComponent)

public:
    FBZZ_REQUIRED_ASSET(PolarityTuning, tuning, "Tuning")
    FBZZ_TOOLTIP("作用半径・溜め・速度の共有調整値。未割り当てでは動作を開始しない")

    FBZZ_GROUP("Links")
    // 同時に成立させるリンクの上限。Wave で敵が増えたときに盤面が線だらけになるのを防ぐ。
    FBZZ_FIELD_RANGE_INT(int, maxLinks, 8, "Max Links", 1, 32)
    FBZZ_TOOLTIP("同時に成立する引力の本数。近い組から順に採用する")
    // 飛んだ側の極性を衝突で使い切るか。
    //
    // WHY 既定を true にするか (企画書に明記が無いため、ここで理由を残す):
    //   衝突しても極性が残ると、ぶつかって密着した 2 体がその場で再び引き合い、
    //   短い距離の衝突を延々と繰り返す。1 回の組み立てが 1 回の攻撃として終わらず、
    //   7.7 のクールダウン設計 (撃つ回数が資源であること) も意味を失う。
    //
    // WHY 飛んだ側だけを消すか:
    //   7.6 の柱は「置いておくと周囲の敵が集まってくる」設置型アンカーであり、
    //   最初の 1 体で消えては役目を果たせない。動かなかった側は極を保つ。
    FBZZ_FIELD(bool, consumePolarityOnImpact, true, "Consume On Impact")
    FBZZ_TOOLTIP("衝突した瞬間、飛んだ側の極性を消す。アンカー側は保持したまま")

    // 17 章「最も重要なのは、機能数ではなくギュンッ・ドンッの気持ちよさ」。
    // 止め方・揺らし方・鳴らし方は ImpactFeedbackManagerComponent が一括で持つ。
    // ここは「何が起きたか」と「どれくらい強い当たりだったか」を渡すだけ。

    FBZZ_GROUP("Link Arc (12.5)")
    // WHY 放電を盤面が張るか:
    //   12.5 のエネルギーラインは «対» の持ち物で、どちらか一方の敵に持たせると、
    //   A も B も自分の放電を張って同じ 2 点に 2 束が重なる。見た目は «明るさだけ
    //   倍の 1 本» になり、本数を増やした意味が消えたうえに負荷だけ倍になる。
    //   誰と誰が組んでいるかを知っているのはここだけなので、ここが 1 本だけ張る。
    //
    //   直線のエネルギーライン (PolarityBodyComponent の Link Line) は残す。
    //   放電は形が毎回変わるので «どこへ引かれているか» を読ませる役には立たない。
    //   読ませる線と、力が溜まっていることを見せる放電は別の仕事をしている。
    FBZZ_FIELD(bool, drawLinkArcs, true, "Draw Link Arcs")
    FBZZ_FIELD_RANGE_INT(int, arcStrands, 2, "Arc Strands", 1, 6)
    FBZZ_TOOLTIP("1 リンクあたりの筋の本数。上限 (Max Links) を掛けた数だけ線が増える")
    FBZZ_FIELD_RANGE(float, arcAmplitude, 0.5f, "Arc Amplitude", 0.0f, 3.0f)
    FBZZ_TOOLTIP("放電が直線から外れる幅 [m]。離れているほど自動で大きく振れる")
    FBZZ_FIELD_RANGE(float, arcWidth, 0.08f, "Arc Width", 0.005f, 0.5f)
    FBZZ_FIELD_RANGE(float, arcRate, 24.0f, "Arc Rate", 1.0f, 60.0f)
    FBZZ_TOOLTIP("撃ち出し直前の組み替え頻度 [Hz]。溜め始めはこれより緩やかに走る")
    FBZZ_FIELD_RANGE(float, arcIntensity, 1.6f, "Arc Intensity", 0.0f, 8.0f)
    FBZZ_FIELD_RANGE(float, arcRangeScale, 1.5f, "Arc Range", 0.2f, 3.0f)
    FBZZ_TOOLTIP("作用半径の何倍で放電が消えるか。近づくほど明るくなる = 溜まって見える。"
                 "1.0 を下回ると、組めたばかりの遠い組で放電が出ない")

    FBZZ_GROUP("Debug")
    FBZZ_FIELD(bool, drawDebugLinks, false, "Draw Debug Links")
    FBZZ_FIELD_READ_ONLY(int, debugLinkCount, 0, "Active Links")
    FBZZ_FIELD_READ_ONLY(int, debugChargedCount, 0, "Charged Targets")

    // 衝突の通知。衝突は本作で唯一のダメージ源 (7.4) なので、ダメージ処理は
    // 2 週目にここへ繋ぐ。
    //
    // WHY ここで HP を減らさないか:
    //   18.2「敵の HP と衝突ダメージ」は未決で、1 回で倒れるのか・速度に比例させるのか・
    //   柱への激突を強くするのかが決まっていない。決まっていない仕様を引力の中へ
    //   書き込むと、変えるたびにこのファイルを触ることになる。引力は「衝突を起こす」
    //   ところまでを担当し、その結果をどう扱うかは購読側が決める。
    std::function<void(const PolarityImpact&)> onImpact;

    // WHY 単体参照を持つか: 盤面の引力はシーンに 1 つしか無い。購読したい側
    //     (CombatManagerComponent) が同じ GameObject に居ることを前提にすると、
    //     マネージャーを別オブジェクトへ分けた瞬間に購読が黙って外れ、
    //     ダメージだけが入らなくなる。置き場所に依存しない窓口を持たせる。
    [[nodiscard]] static PolarityFieldComponent* Instance() { return s_instance; }

    void OnStart()     override;
    void OnUpdate()    override;
    // 盤面を止めた瞬間に放電も消す。OnUpdate が止まるだけだと、最後のフレームの
    // 放電が «誰とも組んでいない 2 点» の間に張り付いたまま残る。
    void OnDisable()   override { for (ElectricArcBundle& arc : m_linkArcs) arc.Extinguish(*this); }
    // WHY 衝突の回収を OnLateUpdate に置くか:
    //   OnCollisionEnter は物理ステップの中で呼ばれ、Phase::Physics は Phase::Script の
    //   後に走る。OnUpdate で回収すると必ず 1 フレーム遅れて演出が出る。
    //   物理より後に走る LateUpdate なら、ぶつかったフレームでヒットストップがかかる。
    void OnLateUpdate() override;
    void OnDestroy()    override
    {
        // 放電の筋はルートに置いた GameObject なので、盤面と一緒には消えない。
        for (ElectricArcBundle& arc : m_linkArcs) arc.Detach(*this);
        m_linkArcs.clear();
        if (s_instance == this) s_instance = nullptr;
    }

private:
    static inline PolarityFieldComponent* s_instance = nullptr;

    // 盤面上の 1 対象。毎フレーム作り直す。
    struct Candidate {
        GameObject*              object  = nullptr;
        PolarityTargetComponent* target  = nullptr;
        PolarityBodyComponent*   body    = nullptr;  // 無ければ動かない側
        Vector3                  position = Vector3::ZERO;
        bool                     movable = false;    // 引力で動けるか (7.3)
        bool                     pairable = false;   // 今フレーム引力の候補になれるか
        bool                     linked  = false;
    };

    struct PairCandidate {
        int   a = 0;
        int   b = 0;
        // 小さいほど優先。距離の二乗、ただし継続中のリンクは最優先にする。
        float priority = 0.0f;
    };

    void CollectCandidates();
    void BuildLinks(float dt);
    /// 1 リンクぶんの放電を今フレームの両端・強さへ張り直す。
    void DriveLinkArc(std::size_t slot, const Candidate& a, const Candidate& b, float dt);
    /// 今フレーム使わなかった束を消灯する。
    ///
    /// WHY 破棄しないか: リンクは 1 秒に何度も入れ替わる。そのたびに筋の GameObject を
    ///     作り直すと、シーンの GameObject 数が戦闘の激しさに合わせて上下し、
    ///     EntityID を握っている側の参照が揺さぶられる。枠は寝かせて使い回す。
    void ExtinguishArcsFrom(std::size_t used);
    /// 放電の端点。動く側は本人の LinkPoint、動かない側 (柱・壁) は胴体の中心。
    [[nodiscard]] Vector3 ArcPointOf(const Candidate& candidate) const;
    void ResolveImpacts();
    void ApplyImpactFeel(const PolarityImpact& impact);
    /// 12.6 の「衝突速度に比例」。最低速度の 2 倍で最大になる 0..1。
    [[nodiscard]] float ImpactStrength(const PolarityImpact& impact) const;
    // 既に飛んでいる相手との組み合わせか (継続中のリンクを途中で乗り換えさせない)。
    [[nodiscard]] bool IsOngoingLink(const Candidate& a, const Candidate& b) const;

    [[nodiscard]] float AttractionRadius() const { return tuning->attractionRadius; }
    [[nodiscard]] float MinImpactSpeed()   const { return tuning->minImpactSpeed; }

    std::vector<Candidate>      m_candidates;
    std::vector<PairCandidate>  m_pairs;
    std::vector<PolarityImpact> m_impacts;
    /// 成立中のリンク 1 本につき 1 束。使わなくなった枠は消灯して寝かせる。
    std::vector<ElectricArcBundle> m_linkArcs;
    bool                        m_warnedNoFeedback = false;
};

FBZZ_REFLECT(PolarityFieldComponent)

// ── 実装 (inline) ─────────────────────────────────────────────────────────────

inline void PolarityFieldComponent::OnStart()
{
    s_instance = this;
    if (!tuning) {
        debug.LogError("PolarityFieldComponent requires PolarityTuning.fzdata.");
        enabled = false;
        return;
    }
    // 2 体居ると盤面の解釈が二重になり、リンクが毎フレーム奪い合いになる。
    if (scene.FindObjectsOfType<PolarityFieldComponent>().size() > 1)
        debug.LogError("PolarityFieldComponent must exist exactly once in the scene.");

    m_warnedNoFeedback = false;
}

inline void PolarityFieldComponent::OnUpdate()
{
    // 停止中は盤面も止める。組み替えても物理が進まないので意味が無いうえ、
    // 止まっている間に相手が変わると復帰した瞬間に別方向へ飛んで見える。
    // 放電も同じフレームで固まるので、止まった画面で線だけが走り続けることもない。
    if (auto* hitstop = HitstopManagerComponent::Instance(); hitstop && hitstop->IsActive())
        return;

    CollectCandidates();
    BuildLinks(Max(Time::deltaTime, 0.0f));
}

inline void PolarityFieldComponent::CollectCandidates()
{
    m_candidates.clear();
    debugChargedCount = 0;

    for (GameObject* object : scene.FindObjectsOfType<PolarityTargetComponent>()) {
        if (!object || !object->activeInHierarchy()) continue;

        auto* target = scene.GetScript<PolarityTargetComponent>(object);
        if (!target) continue;

        Candidate candidate;
        candidate.object   = object;
        candidate.target   = target;
        candidate.body     = scene.GetScript<PolarityBodyComponent>(object);
        candidate.position = object->transform.worldPosition;
        // 動けるのは「剛体の運動を持っていて、かつアンカー指定でない」対象だけ (7.3)。
        candidate.movable  = candidate.body != nullptr && !target->isAnchor;

        if (target->IsCharged()) ++debugChargedCount;

        // 衝突直後の硬直中は引力から外す。外さないと、ぶつかった相手と密着したまま
        // 再リンクし、その場で震え続ける。
        const bool available = !candidate.body || candidate.body->IsAvailableForLink();
        candidate.pairable   = target->IsCharged() && available;

        m_candidates.push_back(candidate);
    }
}

inline bool PolarityFieldComponent::IsOngoingLink(const Candidate& a, const Candidate& b) const
{
    if (a.body && a.body->IsBeingPulled() && a.body->PartnerId() == b.object->GetID())
        return true;
    if (b.body && b.body->IsBeingPulled() && b.body->PartnerId() == a.object->GetID())
        return true;
    return false;
}

inline Vector3 PolarityFieldComponent::ArcPointOf(const Candidate& candidate) const
{
    // 動く側は本人が高さを決めている。線と放電が別の高さから出ると、同じ 2 体を
    // 結んでいるのに «2 本の別の関係» に見える。
    if (candidate.body) return candidate.body->LinkPoint();
    // 柱・壁は原点が足元にあり、全高もまちまち。実寸の中心から出す。決め打ちの高さだと、
    // 8m の柱では足元から、1m の岩では頭上から放電が出る。
    return candidate.object ? bodybounds::CenterWorld(*candidate.object, kArcFallbackHeight)
                            : candidate.position;
}

inline void PolarityFieldComponent::DriveLinkArc(std::size_t slot, const Candidate& a,
                                                 const Candidate& b, float dt)
{
    // 枠ごとに違う鍵。同じ鍵だと 2 本目の束が 1 本目の筋を掴み、リンクが
    // 何本あっても放電は 1 本ぶんしか出なくなる。
    while (slot >= m_linkArcs.size()) {
        m_linkArcs.emplace_back();
        m_linkArcs.back().SetKey("PolarityLink" + std::to_string(m_linkArcs.size() - 1));
    }

    // 溜めの進みがそのまま «電圧»。飛んでいる間は張り切った状態で固定する。
    // WHY 動く側から取るか: 7.3 の溜めを持っているのは動く側だけで、柱は何も数えていない。
    //     両方が動く組では «より進んでいる方» を採る (同じ溜めを共有しているので、
    //     片方が 1 フレーム先行しているだけ)。
    float charge = 0.0f;
    if (a.body) charge = Max(charge, a.body->IsFlying() ? 1.0f : a.body->WindupProgress());
    if (b.body) charge = Max(charge, b.body->IsFlying() ? 1.0f : b.body->WindupProgress());

    const Vector3 from     = ArcPointOf(a);
    const Vector3 to       = ArcPointOf(b);
    const float   distance = (to - from).Length();

    ElectricArcStyle style;
    style.strandCount = arcStrands;
    // 折れ点は距離から決める。10m を 24 点で折ると 1 区間 0.4m の «稲妻» になるが、
    // 密着寸前の 1m でも同じ点数だと、計算だけ増えて見た目は直線に戻る。
    style.segments    = static_cast<int>(Clamp(distance * 3.0f, 8.0f, 40.0f));
    style.amplitude   = Max(arcAmplitude, 0.0f) * Clamp(distance / 5.0f, 0.5f, 2.0f);
    style.width       = Max(arcWidth, 0.001f);
    // 溜め始めは緩やかに、撃ち出し直前ほど速く走らせる。頻度そのものが «張り詰め» になる。
    style.strikeRate  = Lerp(9.0f, Max(arcRate, 1.0f), charge);
    // 近づくほど明るい。7.3 ③ の突進はここが自動で最大へ寄り、衝突の瞬間が最も強く光る。
    style.strikeRange = AttractionRadius() * Max(arcRangeScale, 0.01f);
    style.intensity   = Max(arcIntensity, 0.0f) * (0.55f + 0.85f * charge);
    style.breakup     = Lerp(0.65f, 0.35f, charge); // 溜め中は途切れ、張り切ると繋がる
    style.travel      = Lerp(4.0f, 14.0f, charge);
    style.coreTint    = 0.5f;
    // 電荷が «流れ始めた» ことを粒で見せる。溜めの序盤は出さない。
    style.beadDensity = charge > 0.35f ? Lerp(0.0f, 4.0f, charge) : 0.0f;

    // 色の対応は PolarityTypes が唯一の正本。ここで赤青を書き直さない。
    style.fromColor = PolarityColor(a.target->Current());
    style.toColor   = PolarityColor(b.target->Current());

    m_linkArcs[slot].Update(*this, from, to, style, dt);
}

inline void PolarityFieldComponent::ExtinguishArcsFrom(std::size_t used)
{
    for (std::size_t i = used; i < m_linkArcs.size(); ++i)
        m_linkArcs[i].Extinguish(*this);
}

inline void PolarityFieldComponent::BuildLinks(float dt)
{
    m_pairs.clear();

    const float radius   = AttractionRadius();
    const float radiusSq = radius * radius;
    const int   count    = static_cast<int>(m_candidates.size());

    for (int i = 0; i < count; ++i) {
        if (!m_candidates[i].pairable) continue;
        for (int j = i + 1; j < count; ++j) {
            if (!m_candidates[j].pairable) continue;

            const Candidate& a = m_candidates[i];
            const Candidate& b = m_candidates[j];

            // 引力が発生する唯一の条件 (7.3)。同極どうしは 7.8 の判断により何もしない。
            if (!IsAttracting(a.target->Current(), b.target->Current())) continue;
            // 両方が動かない側 (柱どうし・柱とヘビースライム) なら何も起きない。
            // ここで捨てないと、動かない組がリンク枠を食って本当に飛ぶ組が作れなくなる。
            if (!a.movable && !b.movable) continue;

            // 継続中のリンクは距離に関わらず最優先で維持する。途中で相手が変わると
            // 溜めからやり直しになり、飛んでいる最中に方向が切り替わって何が起きたのか
            // 読めなくなる。半径の足切りも同じ理由で継続中の組には掛けない
            // (予備動作で離れた瞬間に半径を跨いだ組が、離れただけで終わってしまう)。
            const bool  ongoing    = IsOngoingLink(a, b);
            const float distanceSq = (a.position - b.position).LengthSq();
            if (!ongoing && distanceSq > radiusSq) continue;

            const float priority = ongoing ? -1.0f : distanceSq;
            m_pairs.push_back({ i, j, priority });
        }
    }

    // 近い組から貪欲に採る。「今まさに引き合っている 2 体」が最も自然な組み合わせで、
    // プレイヤーが撃った意図とも一致しやすい。
    std::sort(m_pairs.begin(), m_pairs.end(),
              [](const PairCandidate& x, const PairCandidate& y) {
                  return x.priority < y.priority;
              });

    int linkCount = 0;
    for (const PairCandidate& pair : m_pairs) {
        if (linkCount >= maxLinks) break;

        Candidate& a = m_candidates[static_cast<std::size_t>(pair.a)];
        Candidate& b = m_candidates[static_cast<std::size_t>(pair.b)];
        if (a.linked || b.linked) continue;

        // WHY 動く側だけを埋まり扱いにするか:
        //   飛べる方向は 1 つしかないので、動く側は 1 本しか持てない。対して動かない側は
        //   何本引き受けても軌道が濁らない。ここでアンカーまで埋めてしまうと、
        //   7.6 の「柱に＋を置いておく → 周囲の敵が集まってくる」が 1 体ずつ順番待ちになり、
        //   設置型の戦術という柱の存在理由そのものが消える。
        if (a.movable) a.linked = true;
        if (b.movable) b.linked = true;
        ++linkCount;

        // 動く側だけに引き寄せを指示する。相手が動かないなら、その相手は
        // 質量無限のアンカーとして扱われる (7.6 の柱と同じ扱い)。
        if (a.movable) a.body->BeginPull(*b.object, !b.movable);
        if (b.movable) b.body->BeginPull(*a.object, !a.movable);

        // WHY BeginPull の «後» に張るか: 放電の強さは溜めの進み (WindupProgress) から
        //     取る。先に張ると、リンクが成立した最初のフレームだけ «溜めていないのに
        //     放電が出ている» 状態になり、予兆が 1 フレーム前倒しで漏れる。
        if (drawLinkArcs)
            DriveLinkArc(static_cast<std::size_t>(linkCount - 1), a, b, dt);

        if (drawDebugLinks)
            debug.DrawLine(a.position, b.position, PolarityColor(a.target->Current()));
    }

    // 今フレーム張らなかった枠を消灯する。リンクが減った場合も、Inspector で
    // drawLinkArcs を切った場合も、残っている放電はすべてここで消える。
    ExtinguishArcsFrom(drawLinkArcs ? static_cast<std::size_t>(linkCount) : std::size_t{ 0 });

    // 組めなかった対象の引力を解除する。極性が切れた・相手が倒された・
    // 別のより近い組に相手を取られた、のいずれもここを通る。
    for (Candidate& candidate : m_candidates) {
        if (candidate.linked || !candidate.body) continue;
        candidate.body->CancelPull();
    }

    debugLinkCount = linkCount;
}

inline void PolarityFieldComponent::ResolveImpacts()
{
    m_impacts.clear();

    for (GameObject* object : scene.FindObjectsOfType<PolarityBodyComponent>()) {
        auto* body = scene.GetScript<PolarityBodyComponent>(object);
        if (!body || !body->HasImpact()) continue;

        m_impacts.push_back(body->PendingImpact());
        body->ConsumeImpact();

        // 飛んだ側は極を使い切る。動かなかった側 (柱・ヘビースライム) は保持する。
        if (consumePolarityOnImpact) {
            if (auto* target = scene.GetScript<PolarityTargetComponent>(object))
                target->ClearPolarity();
        }
    }

    if (m_impacts.empty()) return;

    const PolarityImpact* strongest = nullptr;
    for (std::size_t i = 0; i < m_impacts.size(); ++i) {
        const PolarityImpact& impact = m_impacts[i];

        // 正面衝突は 2 体が同じ 1 回を別々に記録する。先に採った記録と
        // 相手が入れ替わっただけの組は捨てる (音とヒットストップの二重掛けを防ぐ)。
        bool duplicate = false;
        for (std::size_t k = 0; k < i; ++k) {
            if (m_impacts[k].mover == impact.struck && m_impacts[k].struck == impact.mover) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) continue;

        if (onImpact) onImpact(impact);

        // WHY 爆発だけは 1 件に絞らないか:
        //   ヒットストップやカメラ揺れは «画面» に掛かるので、重ねた分だけ壊れる。
        //   一方で爆発は «盤面のその場所» で起きる出来事で、集束で 3 体が別々の
        //   場所で潰れたなら 3 つ見えないと、何体巻き込んだのかが絵から読めない。
        //   12.6 が求める「コンボが大きいほど絵が派手になる」はここでしか出せない。
        //   同時発火で画面が白く飛ばないための減衰は VfxManagerComponent が持つ。
        if (auto* vfx = VfxManagerComponent::Instance()) {
            vfx->PlayImpact(impact.point, impact.moverPolarity,
                            ImpactStrength(impact), impact.struckIsAnchor);
        }

        if (!strongest || impact.speed > strongest->speed) strongest = &impact;
    }

    // 画面の反応は同フレーム最大の 1 件だけに絞る。複数を足し合わせると、
    // 弱い衝突が重なっただけで画面が長時間止まる。
    if (strongest) ApplyImpactFeel(*strongest);
}

inline float PolarityFieldComponent::ImpactStrength(const PolarityImpact& impact) const
{
    const float reference = Max(MinImpactSpeed() * 2.0f, EPSILON);
    return Clamp01(impact.speed / reference);
}

inline void PolarityFieldComponent::ApplyImpactFeel(const PolarityImpact& impact)
{
    // 12.6「強いヒットストップ (衝突速度に比例)」。
    const float ratio = ImpactStrength(impact);

    // 止め方も揺らし方も鳴らし方も ImpactFeedbackManagerComponent が持つ。
    // ここが渡すのは「何が起きたか」と「どれくらい強い当たりか」の 2 つだけ。
    //
    // WHY OnStart で有無を確かめないか: マネージャーの OnStart がこのスクリプトより後に
    //     走る並びもありうる。スクリプトの並び順に依存した警告は、順番を入れ替えただけで
    //     嘘になる。実際に必要になった瞬間に確かめる。
    if (auto* feedback = ImpactFeedbackManagerComponent::Instance()) {
        feedback->Play(impact.struckIsAnchor ? FeedbackEvent::AnchorImpact
                                             : FeedbackEvent::EnemyImpact,
                       ratio);
    } else if (!m_warnedNoFeedback) {
        m_warnedNoFeedback = true;
        debug.LogWarning("PolarityFieldComponent: no ImpactFeedbackManagerComponent in the scene. "
                         "Impacts land without hitstop, shake or rumble.");
    }
}

inline void PolarityFieldComponent::OnLateUpdate()
{
    ResolveImpacts();
}

} // namespace sandbox
