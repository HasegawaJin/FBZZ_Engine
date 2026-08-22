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
#include <Scripts/Polarity/PolarityBodyComponent.hpp>
#include <Scripts/Polarity/PolarityTargetComponent.hpp>
#include <Scripts/Utils/PolarityTypes.hpp>
#include <algorithm>
#include <functional>
#include <vector>

using namespace fbzz::scene;
using namespace fbzz::math;

namespace sandbox {

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
    // WHY 衝突の回収を OnLateUpdate に置くか:
    //   OnCollisionEnter は物理ステップの中で呼ばれ、Phase::Physics は Phase::Script の
    //   後に走る。OnUpdate で回収すると必ず 1 フレーム遅れて演出が出る。
    //   物理より後に走る LateUpdate なら、ぶつかったフレームでヒットストップがかかる。
    void OnLateUpdate() override;
    void OnDestroy()    override { if (s_instance == this) s_instance = nullptr; }

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
    void BuildLinks();
    void ResolveImpacts();
    void ApplyImpactFeel(const PolarityImpact& impact);
    // 既に飛んでいる相手との組み合わせか (継続中のリンクを途中で乗り換えさせない)。
    [[nodiscard]] bool IsOngoingLink(const Candidate& a, const Candidate& b) const;

    [[nodiscard]] float AttractionRadius() const { return tuning->attractionRadius; }
    [[nodiscard]] float MinImpactSpeed()   const { return tuning->minImpactSpeed; }

    std::vector<Candidate>      m_candidates;
    std::vector<PairCandidate>  m_pairs;
    std::vector<PolarityImpact> m_impacts;
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
    if (auto* hitstop = HitstopManagerComponent::Instance(); hitstop && hitstop->IsActive())
        return;

    CollectCandidates();
    BuildLinks();
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

inline void PolarityFieldComponent::BuildLinks()
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

        if (drawDebugLinks)
            debug.DrawLine(a.position, b.position, PolarityColor(a.target->Current()));
    }

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
        if (!strongest || impact.speed > strongest->speed) strongest = &impact;
    }

    // 演出は同フレーム最大の 1 件だけに絞る。複数を足し合わせると、
    // 弱い衝突が重なっただけで画面が長時間止まる。
    if (strongest) ApplyImpactFeel(*strongest);
}

inline void PolarityFieldComponent::ApplyImpactFeel(const PolarityImpact& impact)
{
    // 12.6「強いヒットストップ (衝突速度に比例)」。最低速度の 2 倍で最大になる曲線にする。
    const float reference = Max(MinImpactSpeed() * 2.0f, EPSILON);
    const float ratio     = Clamp01(impact.speed / reference);

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
