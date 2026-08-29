/// @file    PolarityFieldComponent.hpp
/// @brief   盤面全体の引力を仕切る 1 体だけのスクリプト。シーンの管理用 GameObject に付ける。
/// @author  Hasegawa Jin
/// @date    2026-08-19
///
/// 担当は 2 つ。
/// 1. 誰と誰が引き合うかを毎フレーム決め直す (7.3 の引力条件)
/// 2. その結果起きた衝突を 1 箇所で解決する (7.4 / 12.6 / 17 章の手触り)
///
/// WHY 各自に最寄りを探させないか:
/// 敵が自分で相手を選ぶと、A は B を、B は C を見る、という食い違いが起きる。
/// 引力は 2 者の関係なので、片方だけが引かれている状態は成立しない。
/// 企画書の「誰と誰を、どの順番で結びつけるか」を決めるのはプレイヤーであり、
/// その結果の解釈は 1 箇所に閉じていないと、撃った結果が読めなくなる。
///
/// WHY 1 対象につきリンクを 1 本に絞るか:
/// 軽い敵が 2 つのアンカーから同時に引かれると、間で引き裂かれて動きが濁る。
/// 17 章が最重要とした「ギュンッ」は方向が 1 つに決まって初めて出る。近い順に
/// 貪欲に組み、既に組まれた対象は次のリンクに参加させない。
#pragma once

#include <Engine/Scene/Components/CameraRigComponents.hpp>
// WHY Scene.hpp まで要るか: FindObjectsOfType / GetScript の実体が Scene.hpp の末尾にある。
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Scripts/Data/PolarityTuning.hpp>
#include <Scripts/Game/CameraFollowManagerComponent.hpp>
#include <Scripts/Game/ImpactFeedbackManagerComponent.hpp>
#include <Scripts/Game/ScreenEffectManagerComponent.hpp>
#include <Scripts/Game/VfxManagerComponent.hpp>
#include <Scripts/Polarity/PolarityBodyComponent.hpp>
#include <Scripts/Polarity/PolarityRingComponent.hpp>
#include <Scripts/Polarity/PolarityTargetComponent.hpp>
#include <Scripts/Utils/BodyBounds.hpp>
#include <Scripts/Utils/ElectricArc.hpp>
#include <Scripts/Utils/PolarityTypes.hpp>
#include <Scripts/Utils/SeLibrary.hpp>
#include <Scripts/Utils/ShockFalloff.hpp>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
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

    // WHY «最後に塗った 1 体» を受け側にするか (企画書 7.9):
    //   これが無いと、動ける個体は 1 本しかリンクを持てない (BuildLinks の WHY) ため、
    //   ＋を 4 体並べて一を 1 体撃ち込んでも «いちばん近い 1 組» しか成立せず、残りは
    //   置き去りになる。仕込んだ列がまとめて動く «7.9 の集束» は、相手がアンカー
    //   (柱・Roller・ボス) のときにしか起きなかった。
    //   受け側だけリンクの本数を数えないことにすれば、動ける雑魚 1 体でも «集束点»
    //   として機能し、«並べる → 逆極を置く» がそのまま一手になる。
    //
    // WHY 受け側を «動かさない» か:
    //   受け側も引かれると、集束点そのものが列の重心へ流れていく。仕込んだ位置と
    //   ぶつかる位置がずれ、どこへ集まるのかを事前に読めなくなる。置いた所へ集める。
    FBZZ_GROUP("Converge (7.9)")
    FBZZ_FIELD(bool, chainConverge, true, "Chain Converge")
    FBZZ_TOOLTIP("最後に極を乗せた 1 体を集束点にし、射程内の逆極すべてをそこへ集める。"
                 "切ると従来どおり «近い順に 1 対 1» へ戻る")

    // WHY 連鎖の «続き» を距離ではなく時間で許すか:
    //   作用半径 (10m) は «その場に居合わせた 2 体» を組ませるための値で、静止した
    //   盤面には正しい。だが連鎖は既に動き出した後の話で、次に繋がる相手が 10m 内に
    //   居るかどうかは仕込みの巧拙とは関係のない «たまたま» になる。実際、丁寧に
    //   並べても 2〜3 手で «近くに誰も居ない» で途切れ、長く繋ぐ動機が消えていた。
    //   直前の衝突からの時間で許せば、途切れる条件が «間に合わなかった» になり、
    //   プレイヤーが読んで伸ばせるものになる。
    //
    // WHY 窓の残りに比例させるか (点いている間ずっと最大ではなく):
    //   binary だと窓が閉じる瞬間に届く距離が 26m から 10m へ落ち、最後の 1 手だけが
    //   理由もなく繋がらない。残り時間に沿って縮めれば «伸ばすほど届かなくなる» という
    //   手触りになり、どこで畳むかの判断がそのまま連鎖の長さの上限になる。
    // WHY 相手の決まっていない帯電体を浮かせて止めるか:
    //   ビームでなぞって «並べた» 直後、その敵が今までどおり歩き出すと、集束する頃には
    //   列が崩れている。仕込みという行為そのものが成立しない。塗った瞬間の配置を
    //   集束まで保たせるのがこの状態で、«並べた列へ逆極を撃ち込む» が
    //   «並べてから撃ち込む» にまで広がる。
    //
    // NOTE: 入れると «塗る» が足止めを兼ねる。雑魚は塗られた時点で攻撃してこなくなるので、
    //       戦闘の圧はその分だけ下がる。切れば従来どおり «塗られても向かってくる»。
    FBZZ_GROUP("Armed Hold")
    FBZZ_FIELD(bool, holdArmedBodies, true, "Hold Armed")
    FBZZ_TOOLTIP("極を乗せられて相手がまだ居ない敵を、その場に浮かせて待たせる")

    FBZZ_GROUP("Chain Reach")
    FBZZ_FIELD_RANGE(float, chainRangeScale, 2.6f, "Chain Reach x", 1.0f, 8.0f)
    FBZZ_TOOLTIP("連鎖が続いている間、作用半径をこの倍率まで広げる。"
                 "1.0 で従来どおり «常に作用半径だけ»")
    FBZZ_FIELD_READ_ONLY(float, debugChainWindow, 0.0f, "Chain Window")

    /// 連鎖の残り (1 = 直前に衝突した / 0 = 途切れている)。
    ///
    /// WHY 盤面が数えず外から受け取るか: 連鎖を数えているのは CombatManagerComponent で、
    ///     あちらは既にこのスクリプトを include している。こちらから引きに行くと
    ///     include が輪になる。数える側から押してもらう。
    void SetChainWindow(float remaining01) { m_chainWindow01 = Clamp01(remaining01); }

    // 同極どうしの反発。本作の即応性はここが担う。
    //
    // WHY 引力と同じ «リンク» の器に乗せないか: 引力は 2 者の関係で、どちらが飛ぶか・
    //     どこへ集まるかを盤面が 1 箇所で決めなければ濁る。反発はその場のインパルスで、
    //     関係が続かない。枠を数える必要も、乗り換えを防ぐ必要も無い。同じ器に入れると
    //     «関係を持たない出来事» のためにリンク枠 (Max Links) を食うことになる。
    FBZZ_GROUP("Repulsion")
    FBZZ_FIELD(bool, applyRepulsion, true, "Apply Repulsion")
    FBZZ_TOOLTIP("同極どうしを弾く。切ると旧仕様 (同極は何も起きない) へ戻る。"
                 "半径と速さは PolarityTuning が持つ")
    FBZZ_FIELD_RANGE(float, repulseFlightSeconds, 1.0f, "Flight Seconds", 0.1f, 3.0f)
    FBZZ_TOOLTIP("弾かれた対象が何にも当たらないまま飛べる秒数。"
                 "長いと «押しただけで敵が止まる» 足止めになる")
    FBZZ_FIELD(bool, repulseVfx, true, "Repulse VFX")
    FBZZ_TOOLTIP("弾けた瞬間の放射と音。毎秒出る手なので、残らない絵にすること")
    FBZZ_FIELD_READ_ONLY(int, debugRepulseCount, 0, "Repulses / frame")

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

    // 7.9 の集束を画面へ渡す。ScreenEffectManagerComponent が起点へ画面を引き込む。
    //
    // WHY 銃の側ではなくここから出すか:
    //   撃った «つもり» と、実際に何体が動き出したかは別。外した / 中和した 1 発でも
    //   画面が渦を巻くと、手応えが操作の結果を表さなくなる。誰が誰へ引かれ始めたかを
    //   知っているのは盤面だけなので、巻き込んだ数もここでしか出せない。
    //
    // NOTE: 動く側は 1 本しかリンクを持てない (BuildLinks の WHY)。例外は Chain Converge
    //       で選ばれた集束点で、そこだけは動ける雑魚でも受け側に回って何本でも受ける。
    //       渦が出るのは «アンカー相手» か «集束点» のどちらかになる。
    FBZZ_GROUP("Implode (7.9)")
    FBZZ_FIELD(bool, screenImplode, true, "Screen Implode")
    FBZZ_FIELD_RANGE_INT(int, implodeMinBodies, 2, "Min Bodies", 2, 16)
    FBZZ_TOOLTIP("渦を出す最小の巻き込み数。1 にすると普通の 1 対 1 でも毎回出て、"
                 "集束が «特別な一手» でなくなる")
    FBZZ_FIELD_RANGE_INT(int, implodeFullBodies, 5, "Full Bodies", 2, 16)
    FBZZ_TOOLTIP("渦が最大になる巻き込み数。10.8 の «5 体以上で 3 点» に合わせてある")
    FBZZ_FIELD_RANGE(float, implodeMinStrength, 0.45f, "Min Strength", 0.0f, 1.0f)
    FBZZ_TOOLTIP("最小の巻き込み数で出る強さ。ここから Full Bodies で 1.0 まで伸びる")
    FBZZ_FIELD_RANGE(float, implodeSeconds, 0.30f, "Seconds", 0.05f, 2.0f)
    FBZZ_TOOLTIP("渦が続く長さ。12.6 の «タイムスケール 0.85 を 0.3 秒» に揃えてある")

    FBZZ_GROUP("Debug")
    FBZZ_FIELD(bool, drawDebugLinks, false, "Draw Debug Links")
    FBZZ_FIELD_READ_ONLY(int, debugLinkCount, 0, "Active Links")
    FBZZ_FIELD_READ_ONLY(int, debugChargedCount, 0, "Charged Targets")
    FBZZ_FIELD_READ_ONLY(int, debugConvergeCount, 0, "Last Converge")

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
        /// 7.9 の集束点。動ける個体でも «受ける側» に回るので、リンクの本数を数えない。
        bool                     isSink  = false;
    };

    struct PairCandidate {
        int   a = 0;
        int   b = 0;
        // 小さいほど優先。距離の二乗、ただし継続中のリンクは最優先にする。
        float priority = 0.0f;
    };

    /// 1 つの的へ «今フレーム新しく» 引かれ始めた数。
    struct ConvergeCount {
        EntityID partner{};
        int      count = 0;
    };

    void CollectCandidates();
    /// 同極どうしを弾く。CollectCandidates の直後、リンクを組む «前» に 1 回だけ呼ぶ。
    ///
    /// WHY 引力より先に走らせるか: 弾かれた対象はその瞬間から物理の弾道に乗るので、
    ///     同じフレームに引力のリンクを張られると «押された直後に引き戻される» が起きる。
    ///     先に弾いて pairable を降ろしておけば、その 1 フレームは押しだけが効く。
    void ApplyRepulsion();
    /// 1 体を弾く。弾けたら true。弾かれ方を本人が持っていればそちらへ渡す。
    bool PushAway(Candidate& candidate, const Vector3& direction, float strength);
    /// 集束点を 1 つ選ぶ。BuildLinks の «前» に 1 回だけ呼ぶ。
    void PickSink();
    void BuildLinks(float dt);
    /// 多対 1 の集束を見つけて画面へ渡す。BuildLinks の直後に 1 回だけ呼ぶ。
    void DetectConvergence();
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
    [[nodiscard]] float RepulsionRadius()  const { return tuning->repulsionRadius; }
    [[nodiscard]] float MinImpactSpeed()   const { return tuning->minImpactSpeed; }

    std::vector<Candidate>      m_candidates;
    /// 連鎖の残り。CombatManagerComponent が毎フレーム押す (SetChainWindow)。
    float m_chainWindow01 = 0.0f;
    std::vector<PairCandidate>  m_pairs;
    std::vector<PolarityImpact> m_impacts;
    /// 成立中のリンク 1 本につき 1 束。使わなくなった枠は消灯して寝かせる。
    std::vector<ElectricArcBundle> m_linkArcs;
    /// 前フレームに引かれていた対象。«今» 始まった引力だけを拾うための差分。
    std::vector<EntityID>       m_pulledLast;
    std::vector<EntityID>       m_pulledNow;
    std::vector<ConvergeCount>  m_converge;
    /// 反発の音を鳴らしたフレーム。同じフレームの 2 組目以降は鳴らさない。
    std::uint64_t               m_repulseSfxFrame = 0;
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

    // 反発の «バンッ» はここから鳴らす。どこで弾けたかが方向で分かる必要があるので 3D。
    se::EnsureSource(scene, "SE", 1.0f);

    // 制約① 反発半径 < 引力半径。破ると «固めると弾け、散らすと集まる» が成立せず、
    // 同極を並べて 1 点へ集束させる手そのものが消える。クラッシュしないので明示する。
    if (!tuning->SatisfiesRadiusConstraint()) {
        debug.LogError("PolarityTuning breaks the radius constraint: Repulsion Radius "
                       "must be smaller than Attraction Radius. Same-pole groups can "
                       "never be assembled.");
    }

    m_pulledLast.clear();
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
    // 押しが先、引きが後。順番の理由は ApplyRepulsion の宣言に書いてある。
    ApplyRepulsion();
    // 受け側を先に決めてからリンクを組む。組んだ後では «誰が埋まったか» が
    // 既に確定していて、集束点だけ空けておくことができない。
    PickSink();
    BuildLinks(Max(Time::deltaTime, 0.0f));
    DetectConvergence();
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
        // 転がっている Roller のように «今は的として使えない» と本人が申告している
        // 相手も外す (PolarityTargetComponent::IsLinkSuspended)。
        const bool available = !candidate.body || candidate.body->IsAvailableForLink();
        candidate.pairable   = target->IsCharged() && available &&
                               !target->IsLinkSuspended();

        m_candidates.push_back(candidate);
    }
}

// 同極どうしを弾く。溜めは無く、この 1 フレームで速度が乗る。
//
// WHY 全ペアを毎フレーム見てよいか: 盤面の同時出現上限は 8 体 + 撃破コアで、
//     二重ループでも 100 組に届かない。空間分割を持ち込むと、盤面が «どれとどれが
//     近いか» を 2 通りの方法で答えることになり、引力と反発で結果がずれる余地ができる。
inline void PolarityFieldComponent::ApplyRepulsion()
{
    debugRepulseCount = 0;
    if (!applyRepulsion) return;

    const float radius = Max(RepulsionRadius(), 0.0f);
    if (radius <= 0.0f) return;   // 反発を切っている構成 (旧仕様の挙動)

    const float radiusSq = radius * radius;
    const int   count    = static_cast<int>(m_candidates.size());

    for (int i = 0; i < count; ++i) {
        Candidate& a = m_candidates[i];
        if (!a.target->IsCharged()) continue;

        for (int j = i + 1; j < count; ++j) {
            Candidate& b = m_candidates[j];
            if (!b.target->IsCharged()) continue;
            // 同極だけ。異極は引力の担当で、そちらは溜めを持つ。
            if (a.target->Current() != b.target->Current()) continue;

            // 水平だけで測る。高さを含めると、浮いている Mite と地を這う Serpent が
            // 真上に重なっただけで «届いていない» 判定になる。
            Vector3 delta = b.position - a.position;
            delta.y = 0.0f;
            const float distanceSq = delta.LengthSq();
            if (distanceSq > radiusSq) continue;

            const float distance = std::sqrt(distanceSq);
            const float strength = shock::Falloff(distance, radius, tuning->repulsePlateau);
            if (strength <= 0.0f) continue;

            // 完全に重なった 2 体は向きが決まらない。各自の震えの種から作らせる
            // (PolarityBodyComponent::ApplyRepulse がゼロ長を受けて散らす)。
            const Vector3 direction = delta.NormalizedOr(Vector3::ZERO);

            // 両方に効かせる。片方だけだと «押した方だけが動く» になり、
            // 同じ極どうしという対称な関係が絵から読めない。
            const bool pushedA = PushAway(a, -direction, strength);
            const bool pushedB = PushAway(b,  direction, strength);
            if (!pushedA && !pushedB) continue;
            ++debugRepulseCount;

            // 押し «そのもの» の絵は 2 体の間に 1 枚だけ出す。弾かれた側それぞれに
            // 出すと、同じ 1 回の出来事が 2 回起きたように見える。
            //
            // WHY 地面へ描くか: 押しは «どこからどこまで届いたか» が情報で、
            //     宙に浮いた球ではそれが読めない。床を走る帯なら、届いた範囲が
            //     そのまま距離として見える (絵は DecalPolarityRing.hlsl)。
            if (repulseVfx) {
                const Vector3 midpoint = (a.position + b.position) * 0.5f;
                // 地面の放射は «盤面のその場所» で起きた出来事なので、同時に何組
                // 弾けてもその数だけ出す (爆発を 1 件に絞らないのと同じ理由)。
                if (auto* rings = PolarityRingComponent::Instance())
                    rings->Burst(midpoint, radius, a.target->Current());

                // WHY 音だけ 1 フレーム 1 回に絞るか: 反発は «組» ごとに成立する。
                //     同極が 8 体固まっていれば 1 フレームで 28 組が同時に弾けうる。
                //     絵は場所が違うので重なっても読めるが、音は同じ «バンッ» が
                //     28 本重なって割れるだけで、しかも voice を一気に食い潰す。
                if (m_repulseSfxFrame != time.FrameCount()) {
                    m_repulseSfxFrame = time.FrameCount();
                    se::PlayAt(audio, se::kPolarityRepulse, midpoint);
                }
            }
        }
    }
}

inline bool PolarityFieldComponent::PushAway(Candidate& candidate,
                                             const Vector3& direction, float strength)
{
    if (!candidate.object || !candidate.target) return false;
    // 連続で弾くと押し合いが毎フレーム反転して、盤面が痙攣しているようにしか見えない。
    if (!candidate.target->CanBeRepulsed()) return false;

    const float speed = Max(tuning->repulseSpeed, 0.0f) * strength;
    const float lift  = Max(tuning->repulseLift,  0.0f) * strength;
    if (speed <= 0.0f) return false;

    // 弾かれ «方» を本人が引き受けている相手 (Roller は転がり出す) はそちらへ渡す。
    // 盤面は «誰が弾かれたか» までしか決めない。
    if (candidate.target->onRepulse) {
        candidate.target->onRepulse(direction, speed);
    } else {
        // 既定の弾け方。動けない側 (アンカー・ボス・壁) はここで落ちる。
        if (!candidate.body || !candidate.movable) return false;
        if (!candidate.body->IsAvailableForRepulse()) return false;
        candidate.body->ApplyRepulse(direction, speed, lift, repulseFlightSeconds);
    }

    candidate.target->NotifyRepulsed();
    // 今フレームは引力に参加させない。押された直後に引き戻されると、
    // どちらの手が効いたのかが 1 フレームごとに入れ替わって読めなくなる。
    candidate.pairable = false;

    if (repulseVfx) {
        // 弾かれた «この 1 体» に付く速度線。押しは «画面» の出来事にしないので、
        // ヒットストップにも画角にも一切触らない。
        //
        // WHY 音をここで鳴らさないか: 反発は必ず 2 体に効く。1 体ずつ鳴らすと
        //     1 回の «バンッ» が毎回 2 重になり、押しだけが不自然に重く聞こえる。
        //     音と地面の環は «出来事» 側 (ApplyRepulsion) が 1 回だけ出す。
        if (auto* vfx = VfxManagerComponent::Instance()) {
            vfx->PlayLaunch(candidate.body ? candidate.body->LinkPoint()
                                           : bodybounds::CenterWorld(*candidate.object,
                                                                     kArcFallbackHeight),
                            direction, candidate.target->Current(), speed);
        }
    }
    return true;
}

inline void PolarityFieldComponent::PickSink()
{
    if (!chainConverge) return;

    // BuildLinks と同じ届き方で選ぶ。ここだけ作用半径のままだと、連鎖中に
    // «リンクは張れるのに集束点には選ばれない» 相手ができて挙動が食い違う。
    const float radius   = AttractionRadius() *
                           Lerp(1.0f, Max(chainRangeScale, 1.0f), m_chainWindow01);
    const float radiusSq = radius * radius;
    const int   count    = static_cast<int>(m_candidates.size());

    int   best      = -1;
    float bestStamp = -1.0f;

    for (int i = 0; i < count; ++i) {
        Candidate& sink = m_candidates[i];
        if (!sink.pairable) continue;

        // 既に引かれている個体は受け側になれない。飛びながら受けると、集束点が
        // 動いたうえに «誰が誰へ向かっているのか» が 1 フレームごとに入れ替わる。
        if (sink.body && sink.body->IsBeingPulled()) continue;

        const float stamp = sink.target->ChargedAt();
        if (stamp <= bestStamp) continue;

        // 逆極が射程に 1 体も居なければ回路が閉じていない。閉じていない列を
        // 受け側に選ぶと、本当に閉じている別の列がその枠を失う。
        bool hasPartner = false;
        for (int j = 0; j < count && !hasPartner; ++j) {
            if (i == j) continue;
            const Candidate& mover = m_candidates[j];
            // 寄って来られるのは動ける側だけ。柱どうしでは何も起きない。
            if (!mover.pairable || !mover.movable) continue;
            if (!IsAttracting(sink.target->Current(), mover.target->Current())) continue;
            if ((sink.position - mover.position).LengthSq() > radiusSq) continue;
            hasPartner = true;
        }
        if (!hasPartner) continue;

        best      = i;
        bestStamp = stamp;
    }

    if (best >= 0) m_candidates[static_cast<std::size_t>(best)].isSink = true;
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

    // 連鎖が続いている間だけ遠くへ届く。窓が閉じるにつれて作用半径へ戻るので、
    // «伸ばすほど次が遠い» が自然に効いて、無限には繋がらない。
    debugChainWindow = m_chainWindow01;
    const float radius   = AttractionRadius() *
                           Lerp(1.0f, Max(chainRangeScale, 1.0f), m_chainWindow01);
    const float radiusSq = radius * radius;
    const int   count    = static_cast<int>(m_candidates.size());

    for (int i = 0; i < count; ++i) {
        if (!m_candidates[i].pairable) continue;
        for (int j = i + 1; j < count; ++j) {
            if (!m_candidates[j].pairable) continue;

            const Candidate& a = m_candidates[i];
            const Candidate& b = m_candidates[j];

            // 引力が発生する唯一の条件。同極どうしは ApplyRepulsion が既に弾いている。
            if (!IsAttracting(a.target->Current(), b.target->Current())) continue;
            // 両方が動かない側 (柱どうし・柱とヘビースライム) なら何も起きない。
            // ここで捨てないと、動かない組がリンク枠を食って本当に飛ぶ組が作れなくなる。
            // 集束点は «動ける» まま受け側に回るので、こちらも動かない側として数える。
            const bool aReceives = !a.movable || a.isSink;
            const bool bReceives = !b.movable || b.isSink;
            if (aReceives && bReceives) continue;

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
        // 集束点は何本でも受けられるので埋まり扱いにしない (7.9)。埋めると、
        // 並べた列のうち 1 体が寄った時点で残りが弾かれ、集束が成立しない。
        if (a.movable && !a.isSink) a.linked = true;
        if (b.movable && !b.isSink) b.linked = true;
        ++linkCount;

        // 動く側だけに引き寄せを指示する。相手が動かないなら、その相手は
        // 質量無限のアンカーとして扱われる (7.6 の柱と同じ扱い)。
        //
        // WHY 集束点に partnerIsAnchor を渡さないか: あちらは «柱は原点が足元で全高 8m»
        //     という形の話で、狙う高さを変える (PolarityBodyComponent の TickFlying)。
        //     集束点が雑魚なら高さは普通の敵と同じなので、幾何の扱いまでアンカーへ
        //     寄せると、集まった先が足元へ潜り込む。
        if (a.movable && !a.isSink) a.body->BeginPull(*b.object, !b.movable);
        if (b.movable && !b.isSink) b.body->BeginPull(*a.object, !a.movable);

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

        // 相手が決まらなかった «帯電したまま» の体はその場に浮かせて待たせる。
        // 集束点は受け側なので浮かせない ─ 撃ち込まれる的が宙に浮いていると、
        // 寄ってきた側がどこへ当たるのか事前に読めない。
        if (!holdArmedBodies || candidate.isSink || !candidate.movable) {
            candidate.body->EndArmed();
            continue;
        }
        if (candidate.target->IsCharged()) candidate.body->BeginArmed();
        else                               candidate.body->EndArmed();
    }

    debugLinkCount = linkCount;
}

inline void PolarityFieldComponent::DetectConvergence()
{
    // WHY «今フレーム始まった» だけを数えるか:
    //   引かれている状態は溜め (0.1〜0.2 秒) と飛行のあいだ続く。状態をそのまま
    //   数えると、同じ 1 回の集束を飛んでいる間じゅう毎フレーム報告することになり、
    //   渦が掛かりっぱなしになる。始まりは差分でしか取れない。
    m_pulledNow.clear();
    m_converge.clear();

    for (const Candidate& candidate : m_candidates) {
        if (!candidate.object || !candidate.body || !candidate.body->IsBeingPulled()) continue;

        const EntityID id = candidate.object->GetID();
        m_pulledNow.push_back(id);
        if (std::find(m_pulledLast.begin(), m_pulledLast.end(), id) != m_pulledLast.end())
            continue;

        const EntityID partner = candidate.body->PartnerId();
        auto it = std::find_if(m_converge.begin(), m_converge.end(),
                               [partner](const ConvergeCount& entry) {
                                   return entry.partner == partner;
                               });
        if (it != m_converge.end()) ++it->count;
        else                        m_converge.push_back({ partner, 1 });
    }
    m_pulledLast.swap(m_pulledNow);

    // 同じフレームに 2 つの集束が起きても、渦は 1 つしか置けない (起点が 1 点しか
    // 無いため)。大きい方を採り、小さい方は爆発と音の側で拾わせる。
    const ConvergeCount* biggest = nullptr;
    for (const ConvergeCount& entry : m_converge)
        if (!biggest || entry.count > biggest->count) biggest = &entry;

    if (!biggest) return;
    debugConvergeCount = biggest->count;

    if (!screenImplode || biggest->count < implodeMinBodies) return;

    GameObject* focus = scene.GetGameObject(biggest->partner);
    if (!focus) return;

    auto* screen = ScreenEffectManagerComponent::Instance();
    if (!screen) return;

    const int   span = std::max(implodeFullBodies - implodeMinBodies, 1);
    const float ramp = static_cast<float>(biggest->count - implodeMinBodies)
                     / static_cast<float>(span);
    const float strength = Lerp(Clamp01(implodeMinStrength), 1.0f, Clamp01(ramp));

    // 起点は放電が集まる点と同じ高さにする。足元を中心にすると、渦だけが地面へ
    // 落ちて «線は胸で交わっているのに画面は足元へ吸われる» という二重の中心になる。
    screen->Implode(bodybounds::CenterWorld(*focus, kArcFallbackHeight),
                    strength, implodeSeconds);

    // 集束は四方から 1 点へ飛び込む絵になる。通常の画角だと巻き込んだ敵が画面外に出て、
    // 何体まとめたのかが絵から読めない (Docs/camera-controls.md「集束時のカメラ」)。
    //
    // WHY 反発では広げないか: 押しは 1〜2 フレームで終わる即時の手で、毎秒使う。
    //     そこで画角が動くと、連続して押したときに画面が絶えず揺れる。
    //     «見せ場» として扱うのは引きだけ、という配分を画面の側でも守る。
    if (auto* follow = CameraFollowManagerComponent::Instance())
        follow->PunchFov(strength);
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
