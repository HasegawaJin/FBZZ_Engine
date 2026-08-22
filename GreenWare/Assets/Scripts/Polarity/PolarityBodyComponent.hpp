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
#include <Engine/Scene/Components/ColliderComponent.hpp>
#include <Engine/Scene/Components/RigidBodyComponent.hpp>
#include <Engine/Scene/Script.hpp>
#include <Scripts/Data/PolarityTuning.hpp>
#include <Scripts/Polarity/PolarityTargetComponent.hpp>
#include <Scripts/Utils/BodyBounds.hpp>
#include <Scripts/Utils/PolarityTypes.hpp>
#include <algorithm>
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
    Windup     = 1, // ① 溜め。重力を抜いて浮かせ、相手と逆へ離してから震わせる
    Flying     = 2, // ②③ 一定速度で直進する
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
    FBZZ_REQUIRED_ASSET(PolarityTuning, tuning, "Tuning")
    FBZZ_TOOLTIP("引力の共有調整値。未割り当てでは動作を開始しない")

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
        return m_phase == PullPhase::Windup || m_phase == PullPhase::Flying;
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
    // 溜めの途中で消える / リロードされる経路でも重力を戻す。
    void OnDestroy() override { EndFloat(); }
    // WHY 運動を OnFixedUpdate に置くか:
    //   速度の書き込みは物理ステップの直前で行わないと、書いた直後に重力が 1 フレーム分
    //   乗って軌道が沈む。固定ステップならフレームレートが変わっても飛距離が一定になり、
    //   17 章で調整する「ギュンッ」の手触りが PC 性能で変わらない。
    void OnFixedUpdate() override;
    void OnCollisionEnter(const CollisionInfo& info) override;

private:
    void TickWindup(float dt);
    void TickFlying(float dt);
    // 相手から自分へ向く水平単位ベクトル。離れる向きと飛ぶ向きの符号違いで共有する。
    [[nodiscard]] Vector3 AwayFromPartner(const GameObject& partner) const;
    // 「接触した」とみなす中心間距離。両者のコライダー半径 (スケール込み) から出す。
    [[nodiscard]] float ContactDistanceTo(GameObject& partner) const;
    // 1 体ぶんの水平方向の当たり半径。取れなければ 0。
    // WHY 非 const 参照か: GameObject::GetComponent<T>() が非 const にしか無い。
    [[nodiscard]] static float ContactRadiusOf(GameObject& object);
    // 溜め中だけ重力を弱める。抜けるときは必ず元へ戻す。
    void BeginFloat();
    void EndFloat();
    void RegisterImpact(GameObject& other, const Vector3& contactPoint,
                        const Vector3& contactNormal, float approachSpeed,
                        float impactImpulse);
    void UpdateLinkLine();
    void EndFlight(PullPhase next);

    [[nodiscard]] GameObject* Partner() const { return m_partner.Resolve(scene); }
    // 線と飛行の基準点。原点が足元にあるモデルでも胴体の高さを狙う。
    [[nodiscard]] Vector3 LinkPoint() const;

    [[nodiscard]] float WindupSeconds()   const { return tuning->windupSeconds; }
    [[nodiscard]] float WindupGravity()   const { return tuning->windupGravityScale; }
    [[nodiscard]] float WindupLift()      const { return tuning->windupLiftSpeed; }
    [[nodiscard]] float RecoilSpeed()     const { return tuning->recoilSpeed; }
    [[nodiscard]] float ImpactSeconds()   const { return tuning->impactSeconds; }
    [[nodiscard]] float AttractSpeed()    const { return tuning->attractSpeed; }
    [[nodiscard]] float JitterSpeed()     const { return tuning->chargeJitterSpeed; }
    [[nodiscard]] float MaxFlightSeconds()const { return tuning->maxFlightSeconds; }
    [[nodiscard]] float MinImpactSpeed()  const { return tuning->minImpactSpeed; }
    [[nodiscard]] float StunSeconds()     const { return tuning->impactStunSeconds; }
    [[nodiscard]] float KnockbackSpeed()  const { return tuning->impactKnockbackSpeed; }

    EntityRef m_partner;
    // 撃ち出し速度。距離と impactSeconds から launch 時に 1 度だけ決め、飛行中は変えない。
    float     m_flightSpeed = 0.0f;
    // 溜めに入る前の重力倍率。シーンが 1.0 以外を設定している場合があるので保存して戻す。
    float     m_savedGravityScale = 1.0f;
    bool      m_floating = false;
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
    if (!tuning) {
        debug.LogError("PolarityBodyComponent requires PolarityTuning.fzdata.");
        enabled = false;
        return;
    }
    m_phase       = PullPhase::Idle;
    m_timer       = 0.0f;
    m_hasImpact   = false;
    m_partner     = {};
    m_flightSpeed = 0.0f;
    // 溜めの途中でリロードされた場合に備え、重力を通常へ戻してから始める。
    m_floating          = true;
    m_savedGravityScale = 1.0f;
    EndFloat();

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
    m_phase   = PullPhase::Windup;
    m_timer   = std::max(WindupSeconds(), 0.0f);
    BeginFloat();
}

// 溜めの入口で重力を弱め、上向きの初速を一度だけ与える。
// WHY 毎ステップ y 速度を書かないか: 書き続けると重力が積み上がらず、上がって止まるだけの
//     直線運動になる。初速だけ与えて弱い重力に任せると、上がって落ちる弧を描く。
inline void PolarityBodyComponent::BeginFloat()
{
    if (m_floating) return;
    m_floating = true;
    m_savedGravityScale = physics.GetGravityScale();
    physics.SetGravityScale(WindupGravity());

    if (WindupLift() > 0.0f) {
        Vector3 velocity = physics.GetVelocity();
        velocity.y = WindupLift();
        physics.SetVelocity(velocity);
    }
}

inline void PolarityBodyComponent::EndFloat()
{
    if (!m_floating) return;
    m_floating = false;
    physics.SetGravityScale(m_savedGravityScale);
}

inline void PolarityBodyComponent::CancelPull()
{
    if (m_phase == PullPhase::Recovering) return; // 硬直は最後まで通す

    EndFloat();
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
    case PullPhase::Windup:     TickWindup(dt); break;
    case PullPhase::Flying:     TickFlying(dt); break;
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

inline float PolarityBodyComponent::ContactRadiusOf(GameObject& object)
{
    return bodybounds::RadiusWorld(object);
}

inline float PolarityBodyComponent::ContactDistanceTo(GameObject& partner) const
{
    GameObject* self = scene.Self();
    const float sum = (self ? ContactRadiusOf(*self) : 0.0f) + ContactRadiusOf(partner);
    // 少し余裕を持たせてトンネルを防ぐ。コライダーが取れない構成でも 0 で止まらないよう下限を置く。
    return std::max(sum * 1.1f, 0.5f);
}

inline Vector3 PolarityBodyComponent::AwayFromPartner(const GameObject& partner) const
{
    Vector3 away = transform.worldPosition - partner.transform.worldPosition;
    // 水平だけで測る。上下成分を残すと、離れる動きで浮いて次の直進が空中から始まる。
    away.y = 0.0f;
    const float distance = away.Length();
    return distance > EPSILON ? away / distance : Vector3::ZERO;
}

// ① 溜め (7.3)。重力を抜いて浮かせながら、相手と逆へ離し、終盤で震わせる。
//
// WHY 1 つの段階で「離れる」と「震える」を両方やるか:
//   段階を分けると、離れ終わった瞬間に速度が不連続に切り替わって一度止まって見える。
//   進行度で重みを移せば、離れる力が抜けるのと震えが強まるのが同じ曲線の上で起き、
//   「引き絞られて、耐えきれずに撃ち出される」という 1 本の動きになる。
inline void PolarityBodyComponent::TickWindup(float dt)
{
    GameObject* partner = Partner();
    if (!partner) {                 // 相手が消えた (倒された / Wave リセット)
        CancelPull();
        return;
    }

    const float total = std::max(WindupSeconds(), EPSILON);
    // 0 = 溜め始め / 1 = 撃ち出し直前。
    const float progress = Clamp01(1.0f - m_timer / total);

    const Vector3 away = AwayFromPartner(*partner);
    // 高周波で向きを変える水平速度。1 周期で往復するため純移動量はほぼゼロになる。
    const float   phase = Time::time * 55.0f + m_jitterSeed;
    const Vector3 jitter{ std::sin(phase * 1.7f), 0.0f, std::cos(phase * 2.3f) };

    const float recoilWeight = 1.0f - progress;
    Vector3 velocity = physics.GetVelocity();
    // WHY y を書かないか: 上向きの初速は BeginFloat が一度だけ与えている。毎ステップ
    //     上書きすると弱めた重力が積み上がらず、浮いたまま落ちてこない。
    velocity.x = away.x * RecoilSpeed() * recoilWeight + jitter.x * JitterSpeed() * progress;
    velocity.z = away.z * RecoilSpeed() * recoilWeight + jitter.z * JitterSpeed() * progress;
    physics.SetVelocity(velocity);

    m_timer -= dt;
    if (m_timer > 0.0f) return;

    // ② 加速。漸進させず、この 1 ステップで最高速へ乗せる (7.3「一気に高速へ乗せる」)。
    //    速度は今の距離と impactSeconds から逆算し、飛行中は変えない。
    //    こうすると「離れていても近くても同じ間合いでぶつかる」が成立する。
    Vector3 toPartner = partner->transform.worldPosition - transform.worldPosition;
    if (m_partnerIsAnchor) toPartner.y = 0.0f;
    const float distance = toPartner.Length();
    // WHY 相手が動くときだけ半分にするか: 敵どうしは同じ溜めを共有していて、互いに
    //     同じ速度で寄る。距離をそのまま時間で割ると、寄る速度が 2 倍になって
    //     impactSeconds の半分でぶつかる。柱・壁は動かないので割り引かない。
    const float closingFactor = m_partnerIsAnchor ? 1.0f : 2.0f;
    const float travelSeconds = std::max(ImpactSeconds(), 0.02f) * closingFactor;
    m_flightSpeed = std::max(distance / travelSeconds, AttractSpeed());

    // 撃ち出しからは自前の等速直進なので、重力は元へ戻しておく。
    EndFloat();
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
    physics.SetVelocity(direction * m_flightSpeed);

    // 物理イベントは高速移動・接触解決の順序によって、相手の中心へ到達したフレームを
    // 取りこぼす場合がある。相手がリンク先であることは既に確定しているため、
    // コライダー同士の接触に相当する距離へ入ったら同じ Impact 経路へ送る。
    // WHY 固定値をやめたか: 以前は「半径 0.5 × scale 2 → 中心間 1.0m」と決め打ちしていたが、
    //     これはコライダーが Transform スケールを掛けていなかった頃の値。今はスケールが
    //     効くので、敵の大きさを変えるたびにここの定数がずれる。実寸から出す。
    const float CONTACT_DISTANCE = ContactDistanceTo(*partner);
    if (distance <= CONTACT_DISTANCE) {
        RegisterImpact(*partner, LinkPoint(), -direction, m_flightSpeed, 0.0f);
        return;
    }

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
    // 溜めの途中で打ち切られる経路もここを通る。重力を戻し忘れると浮いたままになる。
    EndFloat();
    m_partner     = {};
    m_phase       = next;
    m_flightSpeed = 0.0f;
    m_timer       = (next == PullPhase::Recovering) ? StunSeconds() : 0.0f;
}

inline void PolarityBodyComponent::RegisterImpact(GameObject& other,
                                                   const Vector3& contactPoint,
                                                   const Vector3& contactNormal,
                                                   float approachSpeed,
                                                   float impactImpulse)
{
    if (m_hasImpact)
        return;

    m_impact.mover   = scene.Self();
    m_impact.struck  = &other;
    m_impact.point   = contactPoint;
    m_impact.normal  = contactNormal;
    m_impact.speed   = approachSpeed;
    m_impact.impulse = impactImpulse;

    const auto* struckTarget = scene.GetScript<PolarityTargetComponent>(&other);
    m_impact.struckIsAnchor = struckTarget ? struckTarget->isAnchor : true;
    m_hasImpact = true;
    EndFlight(PullPhase::Recovering);

    if (KnockbackSpeed() > 0.0f && MinImpactSpeed() > 0.0f) {
        const float ratio = Clamp01(approachSpeed / (MinImpactSpeed() * 2.0f));
        physics.SetVelocity(contactNormal * (KnockbackSpeed() * ratio));
    }
}

inline void PolarityBodyComponent::OnCollisionEnter(const CollisionInfo& info)
{
    if (m_phase != PullPhase::Flying) return;
    if (m_hasImpact) return;
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

    RegisterImpact(*info.other, info.contactPoint, info.contactNormal,
                   info.approachSpeed, info.impactImpulse);
}

inline void PolarityBodyComponent::OnUpdate()
{
    UpdateLinkLine();

    switch (m_phase) {
    case PullPhase::Idle:       debugPhase = "Idle";       break;
    case PullPhase::Windup:     debugPhase = "Windup";     break;
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
