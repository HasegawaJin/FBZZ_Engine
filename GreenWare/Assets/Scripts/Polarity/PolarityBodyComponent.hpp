// FBZZ Engine
// PolarityBodyComponent.hpp | sandbox
// 引き寄せられる側の運動。企画書 7.3 の 3 段階 (溜め → 加速 → 直進 → 衝突) を実行する。
//
// WHY PolarityTargetComponent と分けるか:
//   「極性を帯びられる」と「引力で動ける」は別の性質である。柱と壁は前者だけを持つ
//   (7.3「動くのは軽い側だけ。ヘビースライムと柱は動かない」)。1 クラスにまとめると
//   柱にも RigidBody を要求することになり、質量無限のアンカーという設計が崩れる。
//   このスクリプトが付いていないこと自体が「動かない側」の宣言になる。
//
// WHY 引力を万有引力にしないか:
//   7.3 が明示している。距離の二乗に反比例させると遠いと動かず近いと発散するため、
//   17 章が最重要とした「ギュンッ」が絶対に出ない。ここでは物理法則ではなく、
//   溜め → 一定速度の直進、という演出の手続きとして書く。
//
// WHY 誰とリンクするかをここで決めないか:
//   相手選びは PolarityFieldComponent が盤面全体を見て 1 箇所で決める。ここで各自が
//   最寄りを探すと、A は B を、B は C を見る、という食い違いが起きて引力が成立しない。
//   このクラスは「指定された相手へ飛ぶ」ことだけに責任を持つ。
#pragma once

#include <Engine/Scene/Components/PresentationComponents.hpp>
#include <Engine/Scene/Components/RigidBodyComponent.hpp>
#include <Engine/Scene/Script.hpp>
#include <Scripts/Data/PolarityTuning.hpp>
#include <Scripts/Polarity/PolarityTargetComponent.hpp>
#include <Scripts/Utils/PolarityTypes.hpp>
#include <cmath>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

// 衝突 1 件ぶんの記録 (7.4)。飛んだ側が積み、PolarityFieldComponent が回収して解決する。
//
// WHY その場で演出まで済ませないか:
//   正面衝突では両者が同じ 1 回の衝突を別々に検知する。各自がヒットストップと
//   カメラシェイクを出すと二重にかかり、12.6 の「衝突速度に比例」が壊れる。
//   事実だけを積み、盤面を見ている 1 箇所で重複を潰してから演出へ流す。
struct PolarityImpact {
    GameObject* mover  = nullptr; // 飛んできた側
    GameObject* struck = nullptr; // ぶつけられた側 (柱・壁なら動かない)
    Vector3 point  = Vector3::ZERO;
    Vector3 normal = Vector3::UP;
    // 法線方向の接近速度 (m/s)。12.6 の「衝突速度に比例」はこの値で計る。
    float speed   = 0.0f;
    float impulse = 0.0f;
    // ぶつけた先が固定アンカーだったか (7.5 の「敵 ↔ 柱・壁」)。
    bool struckIsAnchor = false;
};

// 7.3 の 3 段階に、衝突後の硬直 (7.4) を足した 4 状態。
enum class PullPhase : int {
    Idle       = 0,
    Charging   = 1, // ① 溜め。その場で震える
    Flying     = 2, // ②③ 加速して直進する
    Recovering = 3, // 衝突後の短時間スタン
};

class PolarityBodyComponent : public Script {
    FBZZ_SCRIPT(PolarityBodyComponent)

    // 引力は速度で与えるため剛体が要る。無いと「極性は乗るのに一切動かない」という、
    // 原因がどこにも出ない壊れ方をする。
    FBZZ_REQUIRE_COMPONENT(RigidBodyComponent)
    // 12.5 のエネルギーライン。無ければ線が出ないだけで、引力そのものは成立する。
    FBZZ_OPTIONAL_COMPONENT(LineRendererComponent)

public:
    FBZZ_ASSET(PolarityTuning, tuning, "Tuning")
    FBZZ_TOOLTIP("引力の数値。未割り当てだと既定値で動く")

    FBZZ_GROUP("Link Line (12.5)")
    FBZZ_FIELD(bool, drawLink, true, "Draw Link Line")
    FBZZ_TOOLTIP("同 GameObject の Line Renderer に、相手までのエネルギーラインを張る")
    FBZZ_FIELD_RANGE(float, linkWidth, 0.12f, "Link Width", 0.01f, 1.0f)
    // 線を胴体の中心から出すための高さオフセット。原点が足元にあるモデルで
    // 線が床を這うのを防ぐ。
    FBZZ_FIELD_RANGE(float, linkHeightOffset, 0.6f, "Link Height", 0.0f, 5.0f)

    FBZZ_GROUP("Debug")
    FBZZ_FIELD(bool, drawDebugFlight, false, "Draw Debug Flight")
    FBZZ_FIELD_READ_ONLY(std::string, debugPhase,   "Idle", "Phase")
    FBZZ_FIELD_READ_ONLY(std::string, debugPartner, "",     "Partner")

    // ── 盤面 (PolarityFieldComponent) から呼ばれる入口 ──────────────────────
    // 同じ相手で連続して呼ばれても溜めをやり直さない (毎フレーム呼ばれる前提)。
    void BeginPull(GameObject& partner, bool partnerIsAnchor);
    // リンクが切れた (極性が切れた / 相手が消えた / 別の相手に取られた)。
    void CancelPull();

    // ── 参照する側の問い合わせ ───────────────────────────────────────────────
    // 7.3「引かれている間、敵 AI は停止する。抵抗させるとギュンッが濁る」。
    // 敵 AI はこれが true の間、移動と攻撃を止める。
    [[nodiscard]] bool IsBeingPulled() const
    {
        return m_phase == PullPhase::Charging || m_phase == PullPhase::Flying;
    }
    // 衝突後の硬直中 (7.4)。敵 AI はこの間も動かない。
    [[nodiscard]] bool IsStunned() const { return m_phase == PullPhase::Recovering; }
    // 盤面が新しいリンクの相手に選んでよいか。硬直中の対象を選ぶと、
    // ぶつかった相手と密着したまま組み直してその場で震え続ける。
    [[nodiscard]] bool IsAvailableForLink() const { return !IsStunned(); }
    // 今どれへ引かれているか。盤面が「継続中のリンクを乗り換えさせない」判定に使う。
    [[nodiscard]] EntityID PartnerId() const { return m_partner.id; }

    // 今フレームに起きた衝突。盤面が回収したら ConsumeImpact() で空にする。
    [[nodiscard]] bool HasImpact() const { return m_hasImpact; }
    [[nodiscard]] const PolarityImpact& PendingImpact() const { return m_impact; }
    void ConsumeImpact() { m_hasImpact = false; }

    void OnStart() override;
    void OnUpdate() override;
    // WHY 運動を OnFixedUpdate に置くか:
    //   速度の書き込みは物理ステップの直前で行わないと、書いた直後に重力が 1 フレーム分
    //   乗って軌道が沈む。固定ステップならフレームレートが変わっても飛距離が一定になり、
    //   17 章で調整する「ギュンッ」の手触りが PC 性能で変わらない。
    void OnFixedUpdate() override;
    void OnCollisionEnter(const CollisionInfo& info) override;

private:
    void TickCharging(float dt);
    void TickFlying(float dt);
    void UpdateLinkLine();
    void EndFlight(PullPhase next);

    [[nodiscard]] GameObject* Partner() const { return m_partner.Resolve(scene); }
    // 線と飛行の基準点。原点が足元にあるモデルでも胴体の高さを狙う。
    [[nodiscard]] Vector3 LinkPoint() const;

    [[nodiscard]] float ChargeSeconds()   const { return tuning ? tuning->chargeSeconds        : 0.15f; }
    [[nodiscard]] float AttractSpeed()    const { return tuning ? tuning->attractSpeed         : 18.0f; }
    [[nodiscard]] float JitterSpeed()     const { return tuning ? tuning->chargeJitterSpeed    : 0.9f; }
    [[nodiscard]] float MaxFlightSeconds()const { return tuning ? tuning->maxFlightSeconds     : 3.0f; }
    [[nodiscard]] float MinImpactSpeed()  const { return tuning ? tuning->minImpactSpeed       : 6.0f; }
    [[nodiscard]] float StunSeconds()     const { return tuning ? tuning->impactStunSeconds    : 0.45f; }
    [[nodiscard]] float KnockbackSpeed()  const { return tuning ? tuning->impactKnockbackSpeed : 6.0f; }

    EntityRef m_partner;
    PullPhase m_phase   = PullPhase::Idle;
    // Charging では溜めの残り / Flying では経過 / Recovering では硬直の残り。
    float     m_timer   = 0.0f;
    bool      m_partnerIsAnchor = false;

    // 震えの位相をオブジェクトごとにずらす種。全員が同位相で震えると、
    // 個々が震えているのではなく盤面全体が揺れているように見える。
    float m_jitterSeed = 0.0f;

    PolarityImpact m_impact;
    bool           m_hasImpact = false;
};

FBZZ_REFLECT(PolarityBodyComponent)

// ── 実装 (inline) ─────────────────────────────────────────────────────────────

inline void PolarityBodyComponent::OnStart()
{
    m_phase     = PullPhase::Idle;
    m_timer     = 0.0f;
    m_hasImpact = false;
    m_partner   = {};

    // ID から作る決定的な位相ずれ。乱数にすると Play のたびに震え方が変わり、
    // 手触りの調整で「今の値が良かったのか」を判断できなくなる。
    m_jitterSeed = static_cast<float>(scene.Self() ? scene.Self()->GetID().index : 0u) * 1.37f;

    if (!scene.GetScript<PolarityTargetComponent>()) {
        // 極性を帯びられない相手は盤面が候補に入れないため、引力が一度も発生しない。
        // 「付けたのに動かない」を無言で通さない。
        debug.LogError("PolarityBodyComponent requires PolarityTargetComponent "
                       "on the same object (attraction never triggers without it).");
    }
}

inline void PolarityBodyComponent::BeginPull(GameObject& partner, bool partnerIsAnchor)
{
    m_partnerIsAnchor = partnerIsAnchor;

    // 同じ相手なら継続。毎フレーム呼ばれるので、ここで作り直すと
    // 溜めが永久に終わらず ② の加速へ進めない。
    if (m_partner.id == partner.GetID() && IsBeingPulled())
        return;

    m_partner = EntityRef{ partner.GetID() };
    m_phase   = PullPhase::Charging;
    m_timer   = ChargeSeconds();
}

inline void PolarityBodyComponent::CancelPull()
{
    if (m_phase == PullPhase::Recovering) return; // 硬直は最後まで通す

    m_partner = {};
    m_phase   = PullPhase::Idle;
    m_timer   = 0.0f;
}

inline Vector3 PolarityBodyComponent::LinkPoint() const
{
    Vector3 point = transform.worldPosition;
    point.y += linkHeightOffset;
    return point;
}

inline void PolarityBodyComponent::OnFixedUpdate()
{
    const float dt = time.FixedDeltaTime();

    switch (m_phase) {
    case PullPhase::Charging:   TickCharging(dt); break;
    case PullPhase::Flying:     TickFlying(dt);   break;
    case PullPhase::Recovering:
        m_timer -= dt;
        if (m_timer <= 0.0f) {
            m_phase = PullPhase::Idle;
            m_timer = 0.0f;
        }
        break;
    case PullPhase::Idle:
        break;
    }
}

// ① 溜め (7.3)。その場で震えるだけで、位置は動かさない。
// 予備動作であると同時に、衝突より前に結果を予測させて次の組み立てへ移らせるための間。
inline void PolarityBodyComponent::TickCharging(float dt)
{
    GameObject* partner = Partner();
    if (!partner) {                 // 相手が消えた (倒された / Wave リセット)
        CancelPull();
        return;
    }

    // 高周波で向きを変える水平速度。1 周期で往復するため純移動量はほぼゼロになる。
    // WHY y を触らないか: 上下に震わせると接地が外れて浮き、次の直進が空中から始まる。
    const float phase = Time::time * 55.0f + m_jitterSeed;
    const Vector3 jitter{ std::sin(phase * 1.7f), 0.0f, std::cos(phase * 2.3f) };

    Vector3 velocity = physics.GetVelocity();
    velocity.x = jitter.x * JitterSpeed();
    velocity.z = jitter.z * JitterSpeed();
    physics.SetVelocity(velocity);

    m_timer -= dt;
    if (m_timer > 0.0f) return;

    // ② 加速。漸進させず、この 1 ステップで最高速へ乗せる (7.3「一気に高速へ乗せる」)。
    m_phase = PullPhase::Flying;
    m_timer = 0.0f;
}

// ②③ 一定速度で最短距離を突っ切る (7.3)。重力も抗力も効かせない。
inline void PolarityBodyComponent::TickFlying(float dt)
{
    GameObject* partner = Partner();
    if (!partner) {
        CancelPull();
        return;
    }

    Vector3 targetPoint = partner->transform.worldPosition;
    targetPoint.y += linkHeightOffset;

    // WHY 相手が固定アンカーのときだけ高さを自分に合わせるか:
    //   柱は原点が足元・全高 8m のため、中心を狙うと敵が斜め上へ飛んで側面に
    //   空中で刺さる。柱は垂直なので、水平に突っ込ませた方が「叩きつけた」に見える。
    //   敵どうしは元々同じ高さに居るので、素直に中心を狙う。
    if (m_partnerIsAnchor)
        targetPoint.y = LinkPoint().y;

    const Vector3 toPartner = targetPoint - LinkPoint();
    const float   distance  = toPartner.Length();
    if (distance < EPSILON) {
        EndFlight(PullPhase::Recovering);
        return;
    }

    const Vector3 direction = toPartner / distance;
    physics.SetVelocity(direction * AttractSpeed());

    if (drawDebugFlight)
        debug.DrawLine(LinkPoint(), targetPoint, PolarityColor(Polarity::Plus));

    // 届かないまま飛び続けるのを打ち切る。届かない配置は必ず作れてしまうので、
    // ここが無いと敵が壁に刺さったまま AI 停止が解けず盤面が死ぬ。
    //
    // WHY Idle ではなく硬直へ落とすか: Idle にすると、極性も距離も条件を満たしたままなので
    //     盤面が次のフレームに同じ相手で組み直し、溜め → 飛行 → 打ち切りを高速で繰り返す。
    //     硬直を挟めば間隔が空き、大抵はその間に極性が切れて自然に収束する。
    m_timer += dt;
    if (m_timer >= MaxFlightSeconds())
        EndFlight(PullPhase::Recovering);
}

inline void PolarityBodyComponent::EndFlight(PullPhase next)
{
    m_partner = {};
    m_phase   = next;
    m_timer   = (next == PullPhase::Recovering) ? StunSeconds() : 0.0f;
}

inline void PolarityBodyComponent::OnCollisionEnter(const CollisionInfo& info)
{
    if (m_phase != PullPhase::Flying) return;
    if (!info.other) return;
    // プレイヤーは極性システムの外側にいる (5章 / 7.5)。飛行経路へ偶然入っても、
    // プレイヤーを柱扱いして敵へ衝突ダメージを与えてはいけない。
    if (info.other->tag == "Player") return;

    GameObject* partner = Partner();
    const bool  isPartner = (partner && info.other == partner);

    // WHY 相手以外との接触を速度で選り分けるか:
    //   飛行中は床や小石にも触れる。それを全部衝突として扱うと、目的地へ着く前に
    //   飛行が終わって「引かれたのにぶつからない」が起きる。狙った相手なら速度に
    //   関係なく到達、それ以外は 7.4 の「一定速度以上」を満たしたときだけ衝突とする。
    if (!isPartner && info.approachSpeed < MinImpactSpeed())
        return;

    m_impact.mover   = info.self ? info.self : scene.Self();
    m_impact.struck  = info.other;
    m_impact.point   = info.contactPoint;
    m_impact.normal  = info.contactNormal;
    m_impact.speed   = info.approachSpeed;
    m_impact.impulse = info.impactImpulse;

    const auto* struckTarget = scene.GetScript<PolarityTargetComponent>(info.other);
    m_impact.struckIsAnchor =
        struckTarget ? struckTarget->isAnchor : true; // 柱・壁・地形は極性を持たない = 動かない
    m_hasImpact = true;

    EndFlight(PullPhase::Recovering);

    // 接触法線に沿って弾き返す。ソルバーの反発だけだと正面衝突で両者がその場に
    // 止まり、12.6 の「ドンッ」が出ない。速度に比例させて弱い衝突では跳ねさせない。
    if (KnockbackSpeed() > 0.0f && MinImpactSpeed() > 0.0f) {
        const float ratio = Clamp01(info.approachSpeed / (MinImpactSpeed() * 2.0f));
        physics.SetVelocity(info.contactNormal * (KnockbackSpeed() * ratio));
    }
}

inline void PolarityBodyComponent::OnUpdate()
{
    UpdateLinkLine();

    switch (m_phase) {
    case PullPhase::Idle:       debugPhase = "Idle";       break;
    case PullPhase::Charging:   debugPhase = "Charging";   break;
    case PullPhase::Flying:     debugPhase = "Flying";     break;
    case PullPhase::Recovering: debugPhase = "Recovering"; break;
    }
    GameObject* partner = Partner();
    debugPartner = partner ? partner->name : std::string{};
}

// 12.5 の「異極の対象同士の間に赤青のエネルギーライン」。
// 溜めの段階から出すことで、7.3 ① の「来ると分かる予兆」がここで成立する。
inline void PolarityBodyComponent::UpdateLinkLine()
{
    if (!gameplay.HasLineRenderer()) return;

    GameObject* partner = Partner();
    if (!drawLink || !partner || !IsBeingPulled()) {
        gameplay.SetLineEnabled(false);
        return;
    }

    const auto* self        = scene.GetScript<PolarityTargetComponent>();
    const auto* otherTarget = scene.GetScript<PolarityTargetComponent>(partner);
    if (!self || !otherTarget) {
        gameplay.SetLineEnabled(false);
        return;
    }

    Vector3 partnerPoint = partner->transform.worldPosition;
    partnerPoint.y += linkHeightOffset;

    gameplay.SetLineEnabled(true);
    gameplay.SetLine(LinkPoint(), partnerPoint, true);
    // 自分の極から相手の極へのグラデーション。どちらへ飛んでいるかが線の色で読める。
    gameplay.SetLineColors(PolarityColor(self->Current()), PolarityColor(otherTarget->Current()));
    gameplay.SetLineWidth(linkWidth, linkWidth);
}

} // namespace sandbox
