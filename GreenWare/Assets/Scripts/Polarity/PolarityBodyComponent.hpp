/// @file    PolarityBodyComponent.hpp
/// @brief   引き寄せられる側の運動。企画書 7.3 の 3 段階 (溜め → 加速 → 直進 → 衝突) を実行する。
/// @author  Hasegawa Jin
/// @date    2026-08-19
///
/// WHY PolarityTargetComponent と分けるか:
/// 「極性を帯びられる」と「引力で動ける」は別の性質である。柱と壁は前者だけを持つ
/// (7.3「動くのは軽い側だけ。ヘビースライムと柱は動かない」)。1 クラスにまとめると
/// 柱にも RigidBody を要求することになり、質量無限のアンカーという設計が崩れる。
/// このスクリプトが付いていないこと自体が「動かない側」の宣言になる。
///
/// WHY 引力を万有引力にしないか:
/// 7.3 が明示している。距離の二乗に反比例させると遠いと動かず近いと発散するため、
/// 17 章が最重要とした「ギュンッ」が絶対に出ない。ここでは物理法則ではなく、
/// 溜め → 一定速度の直進、という演出の手続きとして書く。
///
/// WHY 誰とリンクするかをここで決めないか:
/// 相手選びは PolarityFieldComponent が盤面全体を見て 1 箇所で決める。ここで各自が
/// 最寄りを探すと、A は B を、B は C を見る、という食い違いが起きて引力が成立しない。
/// このクラスは「指定された相手へ飛ぶ」ことだけに責任を持つ。
#pragma once

#include <Engine/Scene/Components/PresentationComponents.hpp>
#include <Engine/Scene/Components/ColliderComponent.hpp>
#include <Engine/Scene/Components/RigidBodyComponent.hpp>
#include <Engine/Scene/Script.hpp>
#include <Scripts/Data/PolarityTuning.hpp>
#include <Scripts/Game/VfxManagerComponent.hpp>
#include <Scripts/Polarity/PolarityTargetComponent.hpp>
#include <Scripts/Utils/BodyBounds.hpp>
#include <Scripts/Utils/LoopVoice.hpp>
#include <Scripts/Utils/PolarityTypes.hpp>
#include <Scripts/Utils/SeLibrary.hpp>
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
    // ぶつけた先が固定アンカーだったか (敵 ↔ 壁・地形)。
    bool struckIsAnchor = false;
    // 引力ではなく反発で飛んだ結果の衝突か。
    //
    // WHY 記録するか: ランク評価は «押し込みで落とした数» を集束とは別の項目に置いている
    //     (Docs/game-flow.md「評価とランク」)。どちらの手で倒したかを知っているのは
    //     飛んだ本人だけで、受け取った側から後で見分ける方法が無い。
    bool fromRepulse = false;
    // 飛んだ側が使い切った極。
    //
    // WHY 記録に含めるか: 盤面は衝突を回収するときに極を消してから演出へ流すため
    //     (PolarityFieldComponent::ResolveImpacts の consumePolarityOnImpact)、
    //     演出側が対象へ問い合わせても必ず無極が返る。12.6 の爆発を «どちらの極が
    //     ぶつかったか» の色で出せるよう、衝突した瞬間の極をここへ写して運ぶ。
    Polarity moverPolarity = Polarity::None;
};

// 7.3 の 3 段階に、衝突後の硬直 (7.4) を足した 4 状態。
//
// Held / Thrown はコアアクション草案 (Docs/core-action-draft.md) の «掴んで投げる» 用。
// WHY 別クラスに分けず同じ状態機械へ足すか:
//   掴んで投げた対象がぶつかったときに «衝突ダメージ・ヒットストップ・爆発» へ乗る道は、
//   PolarityImpact を積んで PolarityFieldComponent に回収させる 1 本しかない。別クラスに
//   すると、その 1 本を丸ごと二重化することになる。運動の出どころが変わるだけなので、
//   «誰がこの剛体の速度を決めているか» を表すこの enum に足すのが正しい。
enum class PullPhase : int {
    Idle       = 0,
    Windup     = 1, // ① 溜め。重力を抜いて浮かせ、相手と逆へ離してから震わせる
    Flying     = 2, // ②③ 一定速度で直進する
    Recovering = 3, // 衝突後の短時間スタン
    Held       = 4, // 掴まれている。掴んだ側が毎フレーム保持点を指定する
    Thrown     = 5, // 投げ出された。弾道は物理に任せ、接触したら衝突として記録する
    // 極を乗せられたが、まだ相手が決まっていない。重力を抜いてその場に浮いて待つ。
    //
    // WHY 待たせる «状態» を作るか: 塗った直後の敵がそのまま歩き回ると、なぞって
    //     並べた «列» が次の瞬間には崩れていて、仕込むという行為が成立しない。
    //     浮かせて止めれば、塗った瞬間の配置がそのまま集束までの盤面になる。
    Armed      = 6,
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

    // 極を乗せられて相手待ちのあいだの浮き方。撃ち出し直前の «溜め» とは別物で、
    // あちらが 0.45 秒なのに対しこちらは極が切れるまで続く。
    FBZZ_GROUP("Armed Hover")
    FBZZ_FIELD_RANGE(float, armedLiftSpeed, 2.2f, "Lift", 0.0f, 10.0f)
    FBZZ_TOOLTIP("待ちに入った瞬間に上へ与える初速 [m/s]。地面から離れて «掴まれた» に見える高さ")
    FBZZ_FIELD_RANGE(float, armedBobSpeed, 0.35f, "Bob Speed", 0.0f, 3.0f)
    FBZZ_TOOLTIP("上下に漂う速さ [m/s]。0 でその高さに静止する")
    FBZZ_FIELD_RANGE(float, armedBobHz, 0.45f, "Bob Hz", 0.05f, 4.0f)

    FBZZ_GROUP("Audio")
    FBZZ_FIELD_RANGE(float, travelVoiceVolume, 0.55f, "Travel Loop", 0.0f, 1.0f)
    FBZZ_TOOLTIP("飛んでいる間ずっと鳴る音の音量。0 で鳴らさない。"
                 "連鎖では複数体が同時に飛ぶので、上げすぎると衝突音が埋もれる")

    FBZZ_GROUP("Debug")
    FBZZ_FIELD(bool, drawDebugFlight, false, "Draw Debug Flight")
    FBZZ_FIELD_READ_ONLY(std::string, debugPhase,   "Idle", "Phase")
    FBZZ_FIELD_READ_ONLY(std::string, debugPartner, "",     "Partner")

    // ── 盤面 (PolarityFieldComponent) から呼ばれる入口 ──────────────────────
    // 同じ相手で連続して呼ばれても溜めをやり直さない (毎フレーム呼ばれる前提)。
    /// 極を乗せられて «待ち» に入る。相手が決まったら BeginPull がそのまま上書きする。
    /// 既に何かの制御下 (溜め・飛行・掴まれ・硬直) にあるときは何もしない。
    void BeginArmed();
    /// 待ちを解いて地面へ戻す。極が切れた / 中和された側から呼ぶ。
    void EndArmed();
    [[nodiscard]] bool IsArmed() const { return m_phase == PullPhase::Armed; }

    void BeginPull(GameObject& partner, bool partnerIsAnchor);
    // リンクが切れた (極性が切れた / 相手が消えた / 別の相手に取られた)。
    void CancelPull();

    // ── 掴み (MagnetHandComponent から呼ばれる入口) ──────────────────────────
    // 保持点へ向かって毎フレーム引き寄せる。BeginPull と同じく毎フレーム呼ばれる前提で、
    // 初回の呼び出しが掴んだ瞬間になる。
    //
    // WHY 位置を直接書かずに速度で寄せるか: Transform を直接ずらすと物理同期に戻され、
    //     掴んだ相手が保持点と元の位置の間で毎フレーム往復する。速度で寄せれば
    //     ソルバーと喧嘩せず、壁や他の敵に当たれば素直に押し留まる。
    void Hold(const Vector3& point, float followSeconds, float maxSpeed);
    // 掴みを解いて自由落下へ戻す (振りほどかれた / 掴んだ側が消えた)。
    void ReleaseHold();
    // 投げ出す。以降は物理の弾道に任せ、接触したら衝突として記録する。
    // polarity は着弾エフェクトの色に使う (投げた «手» の極)。
    void BeginThrow(const Vector3& velocity, Polarity polarity);

    // ── 反発 (PolarityFieldComponent から呼ばれる入口) ───────────────────────
    /// 同極から弾き飛ばされる。溜めは無く、この 1 フレームで速度が乗る。
    ///
    /// WHY 投げ (Thrown) と同じ状態を使うか: どちらも «自前の等速直進ではなく物理の弾道で
    ///     飛んでいて、当たった先で衝突として記録される» という同じ運動をしている。
    ///     状態を分けると、衝突の記録・AI 停止・着地の畳み方を丸ごと二重に書くことになる。
    ///
    /// WHY 溜め・飛行・掴みの最中は無視するか: そちらは行き先が既に決まっている運動で、
    ///     横から速度を書くと «引かれていたはずの敵が別の方向へ消える» が起きる。
    ///     弾けるのは «待っているだけ» か «自由に動いている» 相手だけ。
    /// @param fromField 盤面の反発として数えるか。ランク評価の «押し込み撃破» は
    ///        これが立っている飛行の衝突だけを数える。斬撃のノックバックのように
    ///        «押しではない» 経路は false を渡すこと (Docs/game-flow.md「評価とランク」)。
    void ApplyRepulse(const Vector3& direction, float speed, float lift, float maxSeconds,
                      bool fromField = true);
    /// 今フレーム弾いてよい状態か。盤面が間隔 (repulseCooldown) と併せて確かめる。
    [[nodiscard]] bool IsAvailableForRepulse() const
    {
        return m_phase == PullPhase::Idle || m_phase == PullPhase::Armed;
    }

    // ── 参照する側の問い合わせ ───────────────────────────────────────────────
    // 7.3「引かれている間、敵 AI は停止する。抵抗させるとギュンッが濁る」。
    // 敵 AI はこれが true の間、移動と攻撃を止める。
    [[nodiscard]] bool IsBeingPulled() const
    {
        return m_phase == PullPhase::Windup || m_phase == PullPhase::Flying;
    }
    // 衝突後の硬直中 (7.4)。敵 AI はこの間も動かない。
    [[nodiscard]] bool IsStunned() const { return m_phase == PullPhase::Recovering; }
    [[nodiscard]] bool IsHeld()    const { return m_phase == PullPhase::Held; }
    [[nodiscard]] bool IsThrown()  const { return m_phase == PullPhase::Thrown; }
    // この剛体の速度を極性システムが決めている最中か。敵 AI は自分で動くのをやめる。
    [[nodiscard]] bool IsUnderControl() const { return m_phase != PullPhase::Idle; }
    // 盤面が新しいリンクの相手に選んでよいか。硬直中の対象を選ぶと、
    // ぶつかった相手と密着したまま組み直してその場で震え続ける。
    //
    // WHY 掴み中と投げ中も外すか: どちらもプレイヤーが速度を決めている最中で、
    //     盤面が横から BeginPull を掛けると手の中の相手が引きちぎられて飛んでいく。
    //     «掴んだものが勝手にどこかへ行く» は原因が画面から読めない壊れ方になる。
    [[nodiscard]] bool IsAvailableForLink() const
    {
        return !IsStunned() && !IsHeld() && !IsThrown();
    }
    // 新しく掴んでよいか。既に誰かの手の中／飛行中／硬直中の相手は掴ませない。
    [[nodiscard]] bool IsAvailableForGrab() const { return m_phase == PullPhase::Idle; }
    // 今どれへ引かれているか。盤面が「継続中のリンクを乗り換えさせない」判定に使う。
    [[nodiscard]] EntityID PartnerId() const { return m_partner.id; }
    // 線と飛行の基準点。原点が足元にあるモデルでも胴体の高さを狙う。
    //
    // WHY 盤面へ公開するか: 12.5 の放電は «対» の持ち物なので張るのは盤面側だが、
    //     端点の高さを決めているのはこちら (linkHeightOffset)。盤面が独自に高さを
    //     決めると、同じ 2 体を結ぶ線と放電が別の高さから出る。
    [[nodiscard]] Vector3 LinkPoint() const;
    // 溜めの進み [0,1]。0 = 溜め始め / 1 = 撃ち出し直前。溜め以外では 0。
    //
    // WHY 演出のために公開するか: 7.3 ① の «引き絞られている» は、絵の強さが
    //     進行度に比例して初めて «来ると分かる予兆» になる。段階 (PullPhase) だけ
    //     渡すと、演出側は溜めのどこに居るかを自前で数え直すことになる。
    [[nodiscard]] float WindupProgress() const
    {
        if (m_phase != PullPhase::Windup) return 0.0f;
        return Clamp01(1.0f - m_timer / Max(WindupSeconds(), EPSILON));
    }
    // 撃ち出されて相手へ突っ込んでいる最中か。放電を «張り詰めた» 側へ振るのに使う。
    [[nodiscard]] bool IsFlying() const { return m_phase == PullPhase::Flying; }

    // 今フレームに起きた衝突。盤面が回収したら ConsumeImpact() で空にする。
    [[nodiscard]] bool HasImpact() const { return m_hasImpact; }
    [[nodiscard]] const PolarityImpact& PendingImpact() const { return m_impact; }
    void ConsumeImpact() { m_hasImpact = false; }

    void OnStart() override;
    void OnUpdate() override;
    // 無効化された体から飛行音が鳴り続けないようにする。
    void OnDisable() override { m_travelVoice.Stop(*this); }
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
    void TickHeld(float dt);
    void TickThrown(float dt);
    // 相手から自分へ向く水平単位ベクトル。離れる向きと飛ぶ向きの符号違いで共有する。
    [[nodiscard]] Vector3 AwayFromPartner(const GameObject& partner) const;
    // 「接触した」とみなす中心間距離。両者のコライダー半径 (スケール込み) から出す。
    [[nodiscard]] float ContactDistanceTo(GameObject& partner) const;
    // 1 体ぶんの水平方向の当たり半径。取れなければ 0。
    // WHY 非 const 参照か: GameObject::GetComponent<T>() が非 const にしか無い。
    [[nodiscard]] static float ContactRadiusOf(GameObject& object);
    // 重力を一時的に差し替える。抜けるときは必ず元へ戻す。
    // 溜めは弱い重力 + 上向きの初速、掴みは完全な無重力で初速なし。
    void BeginFloat(float gravityScale, float liftSpeed);
    void EndFloat();
    void RegisterImpact(GameObject& other, const Vector3& contactPoint,
                        const Vector3& contactNormal, float approachSpeed,
                        float impactImpulse);
    void UpdateLinkLine();
    void EndFlight(PullPhase next);

    [[nodiscard]] GameObject* Partner() const { return m_partner.Resolve(scene); }

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

    // 掴まれている間の保持点。掴んだ側が毎フレーム上書きする。
    Vector3 m_holdPoint    = Vector3::ZERO;
    float   m_holdFollow   = 0.08f;
    float   m_holdMaxSpeed = 30.0f;
    // 投げた «手» の極。着弾エフェクトの色に使う。
    // WHY 本人の極を読まないか: 掴んで投げた相手は無極のことが多く、そのまま流すと
    //     爆発が全部灰色になって «誰が投げたか» が絵から消える。
    Polarity m_thrownPolarity = Polarity::None;
    // この 1 回の弾道を打ち切るまでの秒数。投げは maxFlightSeconds、反発は短い。
    //
    // WHY 反発だけ短いか: 反発は毎秒使う手なので、当たらなかった 1 回のために
    //     3 秒も AI が止まると «押しただけで敵が固まる» 足止めになる。
    float m_thrownCap = 0.0f;
    // この弾道が反発によるものか。着地を衝突として数えないための区別。
    bool  m_repelled  = false;
    // その反発が «盤面の押し» だったか。斬撃のノックバックは同じ運動をするが、
    // ランク評価の «押し込み撃破» には数えない。
    bool  m_repelledByField = false;

    PolarityImpact m_impact;
    bool           m_hasImpact = false;

    // WHY 主 voice で鳴らさないか: 飛行音は撃ち出しから着弾まで鳴り続ける。同じ口から
    //     出すと、その音量が着弾音や被弾音にも掛かる (LoopVoice.hpp)。
    se::LoopVoice m_travelVoice;
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
    m_thrownPolarity = Polarity::None;
    m_thrownCap   = tuning->maxFlightSeconds;
    m_repelled    = false;
    // 溜めの途中でリロードされた場合に備え、重力を通常へ戻してから始める。
    m_floating          = true;
    m_savedGravityScale = 1.0f;
    EndFloat();

    // ID から作る決定的な位相ずれ。乱数にすると Play のたびに震え方が変わり、
    // 手触りの調整で「今の値が良かったのか」を判断できなくなる。
    m_jitterSeed = static_cast<float>(scene.Self() ? scene.Self()->GetID().index : 0u) * 1.37f;

    // 引力の音は «盤面のどれが動いたか» が方向で読める必要があるので 3D。
    se::EnsureSource(scene, "SE", 1.0f);
    // 持ち主ごとに一意でないと、同時に飛ぶ 2 体が同じ子音源を奪い合う。
    m_travelVoice.SetKey("AttractTravel");
    m_travelVoice.Stop(*this);

    if (!scene.GetScript<PolarityTargetComponent>()) {
        // 極性を帯びられない相手は盤面が候補に入れないため、引力が一度も発生しない。
        // 「付けたのに動かない」を無言で通さない。
        debug.LogError("PolarityBodyComponent requires PolarityTargetComponent "
                       "on the same object (attraction never triggers without it).");
    }
}

inline void PolarityBodyComponent::BeginArmed()
{
    // 既に誰かの制御下にあるなら触らない。溜めや飛行を «待ち» で上書きすると、
    // 撃ち出された瞬間にその場へ引き戻される。
    if (m_phase != PullPhase::Idle) return;

    m_phase = PullPhase::Armed;
    m_timer = 0.0f;

    // WHY 溜め (WindupGravity 0.15) を流用しないか:
    //   あちらは 0.45 秒で終わる «撃ち出す直前» の浮きで、0.15 倍でも落ち切る前に
    //   飛んでいく。待ちは極が切れるまで数秒続くので、同じ値だと 1 度跳ねたあと
    //   ただ地面へ落ちて座る。«ふわふわ浮いて待っている» にならない。
    //   待ちは重力を完全に抜き、垂直速度をこちらで抑える。
    BeginFloat(0.0f, armedLiftSpeed);
}

inline void PolarityBodyComponent::EndArmed()
{
    if (m_phase != PullPhase::Armed) return;
    EndFloat();
    m_phase = PullPhase::Idle;
    m_timer = 0.0f;
}

inline void PolarityBodyComponent::BeginPull(GameObject& partner, bool partnerIsAnchor)
{
    m_partnerIsAnchor = partnerIsAnchor;

    // 同じ相手なら継続。毎フレーム呼ばれるので、ここで作り直すと
    // 溜めが永久に終わらず ② の加速へ進めない。
    if (m_partner.id == partner.GetID() && IsBeingPulled())
        return;

    // 待ちから溜めへは «そのまま繋ぐ»。EndArmed を挟んで重力を戻すと、
    // 相手が決まった 1 フレームだけ落下してから浮き直すことになる。
    if (m_phase == PullPhase::Armed) m_phase = PullPhase::Idle;

    m_partner = EntityRef{ partner.GetID() };
    m_phase   = PullPhase::Windup;
    m_timer   = std::max(WindupSeconds(), 0.0f);
    BeginFloat(WindupGravity(), WindupLift());

    // リンクが «成立した» 瞬間。溜めは一度引き離してから撃ち出すので、見た目には
    // 離れていく側から始まる。ここで音を置かないと、17 章の «ギュンッ» の前触れが
    // «勝手に浮き上がった» としか読めなくなる。
    se::Play(audio, se::kAttractConverge);
}

// 溜めの入口で重力を弱め、上向きの初速を一度だけ与える。
// WHY 毎ステップ y 速度を書かないか: 書き続けると重力が積み上がらず、上がって止まるだけの
//     直線運動になる。初速だけ与えて弱い重力に任せると、上がって落ちる弧を描く。
inline void PolarityBodyComponent::BeginFloat(float gravityScale, float liftSpeed)
{
    if (m_floating) return;
    m_floating = true;
    m_savedGravityScale = physics.GetGravityScale();
    physics.SetGravityScale(gravityScale);

    if (liftSpeed > 0.0f) {
        Vector3 velocity = physics.GetVelocity();
        velocity.y = liftSpeed;
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
    // 掴まれている / 投げられている間は盤面の管轄外。ここで解くと、盤面が候補を
    // 組み替えたフレームに手の中の相手が落ちる。
    if (m_phase == PullPhase::Held || m_phase == PullPhase::Thrown) return;
    // 待ちも解かない。盤面は «組めなかった» 対象へ毎フレーム CancelPull を投げるので、
    // ここで畳むと «解除 → 待ち直し» を毎フレーム繰り返すことになる。BeginFloat は
    // 呼ばれるたびに上向きの初速を入れ直すため、浮くどころか上空へ飛んでいく。
    // 待ちを終えるのは極が切れたときで、その判断は EndArmed の呼び出し側が持つ。
    if (m_phase == PullPhase::Armed) return;

    EndFloat();
    m_travelVoice.Stop(*this);
    m_partner = {};
    m_phase   = PullPhase::Idle;
    m_timer   = 0.0f;
}

inline void PolarityBodyComponent::Hold(const Vector3& point, float followSeconds,
                                        float maxSpeed)
{
    m_holdPoint    = point;
    m_holdFollow   = std::max(followSeconds, 0.01f);
    m_holdMaxSpeed = std::max(maxSpeed, 0.0f);

    if (m_phase == PullPhase::Held) return; // 掴み直しではない (毎フレーム呼ばれる)

    // 溜めの途中を掴んだ場合、相手へのリンクを先に解いてから引き受ける。
    m_partner     = {};
    m_flightSpeed = 0.0f;
    m_timer       = 0.0f;
    m_phase       = PullPhase::Held;
    // 手の中では完全な無重力にする。弱い重力を残すと、保持点へ寄る速度と
    // 落下が釣り合った高さで止まり、狙った位置より必ず下にぶら下がる。
    EndFloat();
    BeginFloat(0.0f, 0.0f);
}

inline void PolarityBodyComponent::ReleaseHold()
{
    if (m_phase != PullPhase::Held) return;
    EndFloat();
    m_phase = PullPhase::Idle;
    m_timer = 0.0f;
}

inline void PolarityBodyComponent::BeginThrow(const Vector3& velocity, Polarity polarity)
{
    // 掴んだままでも、床に転がっている相手 (押し出し) でも同じ入口を通す。
    EndFloat();
    m_partner        = {};
    m_phase          = PullPhase::Thrown;
    m_timer          = 0.0f;
    m_thrownPolarity = polarity;
    m_flightSpeed    = velocity.Length();
    m_thrownCap      = MaxFlightSeconds();
    m_repelled       = false;
    physics.SetVelocity(velocity);
}

inline void PolarityBodyComponent::ApplyRepulse(const Vector3& direction, float speed,
                                                float lift, float maxSeconds, bool fromField)
{
    if (!IsAvailableForRepulse()) return;

    // 待ち (Armed) は重力を切って浮かせてある。戻さずに速度だけ書くと、
    // 弾かれた相手が «無重力のまま横へ滑っていく» ことになる。
    EndFloat();

    Vector3 away = direction;
    away.y = 0.0f;
    // 真上から重なった 2 体は水平の向きが決まらない。震えの種から向きを作れば
    // 個体ごとに散り、重なった山が一点へ潰れたまま残ることがない。
    away = away.NormalizedOr({ std::cos(m_jitterSeed), 0.0f, std::sin(m_jitterSeed) });

    Vector3 velocity = away * Max(speed, 0.0f);
    // 上へ乗せるのは «弾けた» を絵にするため。水平だけだと床を滑るので、
    // 同じ速度でも «押しのけられた» にしか見えない。
    velocity.y = Max(lift, 0.0f);

    m_partner        = {};
    m_phase          = PullPhase::Thrown;
    m_timer          = 0.0f;
    m_thrownPolarity = Polarity::None; // 本人の極を使う (弾かれても極は残る)
    m_flightSpeed    = velocity.Length();
    m_thrownCap      = Max(maxSeconds, 0.1f);
    m_repelled       = true;
    m_repelledByField = fromField;
    physics.SetVelocity(velocity);
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
    case PullPhase::Armed: {
        m_timer += dt;
        // 水平は抜いて «その場» を保つ。抜かないと、塗られる直前まで走っていた
        // 勢いで列から流れ出ていき、並べた配置が崩れる。
        //
        // 垂直はゆっくりした上下へ寄せる。重力を切ってあるので、放っておくと
        // 浮き上がった高さで完全に静止して «止まった置物» になる。
        // 個体ごとに位相をずらすのは、揃うと盤面全体が 1 枚の板に見えるため
        // (m_jitterSeed は震えと共用の決定的な種)。
        const float bob = std::sin(m_timer * armedBobHz * TWO_PI + m_jitterSeed) *
                          armedBobSpeed;
        Vector3 velocity = physics.GetVelocity();
        velocity.x = 0.0f;
        velocity.z = 0.0f;
        velocity.y += (bob - velocity.y) * Clamp01(dt * 6.0f);
        physics.SetVelocity(velocity);
        break;
    }
    case PullPhase::Windup:     TickWindup(dt); break;
    case PullPhase::Flying:     TickFlying(dt); break;
    case PullPhase::Held:       TickHeld(dt);   break;
    case PullPhase::Thrown:     TickThrown(dt); break;
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

    // 17 章が「最も重要」とした «ギュンッ» の頭。
    // WHY 飛んでいる間ではなくこの 1 フレームだけ出すか: 7.3 は撃ち出しを漸進させず
    //     1 ステップで最高速へ乗せると決めている。加速の «途中» が無いので、速さは
    //     動いている本体からは読めない。読めるのは出発点に何が残ったかだけ。
    if (auto* vfx = VfxManagerComponent::Instance()) {
        const auto* target = scene.GetScript<PolarityTargetComponent>();
        vfx->PlayLaunch(LinkPoint(), toPartner.NormalizedOr(Vector3::FORWARD),
                        target ? target->Current() : Polarity::None, m_flightSpeed);
    }

    // 撃ち出しからは自前の等速直進なので、重力は元へ戻しておく。
    EndFloat();
    m_phase = PullPhase::Flying;
    m_timer = 0.0f;

    // 飛んでいる «あいだ» の音。撃ち出しの一発 (Launch) は VfxManager 側の絵と同じく
    // 出発点に残る音で、こちらは動いている本体に付いていく。7.3 が加速を漸進させない
    // と決めている以上、飛んでいる速さは絵からは読めない。近づいてくる音の方で読ませる。
    m_travelVoice.Update(*this, se::kAttractTravelLoop.First(), travelVoiceVolume);
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

// 保持点へ寄せる。距離を追従時間で割った速度なので、遠いほど速く寄り、
// 着いたところで自然に緩む (指数的な減衰と同じ形)。
inline void PolarityBodyComponent::TickHeld(float)
{
    const Vector3 toHold  = m_holdPoint - LinkPoint();
    const float   distance = toHold.Length();
    if (distance <= EPSILON) {
        physics.SetVelocity(Vector3::ZERO);
        return;
    }

    float speed = distance / m_holdFollow;
    if (m_holdMaxSpeed > 0.0f) speed = std::min(speed, m_holdMaxSpeed);
    physics.SetVelocity((toHold / distance) * speed);
}

// 投げたあとは弾道を物理に任せる。速度を書き続けると重力が乗らず、
// «放り投げた» ではなく «誘導弾» に見える。ここは打ち切りだけを数える。
inline void PolarityBodyComponent::TickThrown(float dt)
{
    m_timer += dt;
    if (m_timer < Max(m_thrownCap, EPSILON)) return;

    // 何にも当たらないまま飛び切った。
    //
    // WHY 反発だけ硬直を挟まないか: 反発は毎秒使う手で、外すこと自体が普通に起きる。
    //     そこに硬直を足すと «押しただけで敵が数秒固まる» 足止めになり、押しが
    //     «位置を変える道具» から «弱いスタン» に化ける。押しにダメージも拘束も持たせない。
    EndFlight(m_repelled ? PullPhase::Idle : PullPhase::Recovering);
    m_thrownPolarity = Polarity::None;
    m_repelled       = false;
}

inline void PolarityBodyComponent::EndFlight(PullPhase next)
{
    // 溜めの途中で打ち切られる経路もここを通る。重力を戻し忘れると浮いたままになる。
    EndFloat();
    // 飛行の終わりは «着いた» だけでなく «打ち切られた» 経路も通る。どちらでも
    // 動いていない体から移動音が鳴り続けないよう、必ずここで畳む。
    m_travelVoice.Stop(*this);
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
    m_impact.fromRepulse    = m_repelled && m_repelledByField;

    // 投げられた相手は無極のことが多い。投げた手の極を優先して、爆発の色で
    // «どちらの手で投げたか» が読めるようにする。
    const auto* selfTarget = scene.GetScript<PolarityTargetComponent>();
    m_impact.moverPolarity = m_thrownPolarity != Polarity::None
                           ? m_thrownPolarity
                           : (selfTarget ? selfTarget->Current() : Polarity::None);
    m_hasImpact = true;
    m_thrownPolarity = Polarity::None;
    m_repelled       = false;
    EndFlight(PullPhase::Recovering);

    if (KnockbackSpeed() > 0.0f && MinImpactSpeed() > 0.0f) {
        const float ratio = Clamp01(approachSpeed / (MinImpactSpeed() * 2.0f));
        physics.SetVelocity(contactNormal * (KnockbackSpeed() * ratio));
    }
}

inline void PolarityBodyComponent::OnCollisionEnter(const CollisionInfo& info)
{
    // 投げられた相手・弾かれた相手は «狙った 1 体» を持たないので、
    // 最低速度だけで衝突かどうかを選り分ける。
    if (m_phase == PullPhase::Thrown) {
        if (m_hasImpact || !info.other) return;
        if (info.other->tag == "Player") return;

        // 弾かれた相手が «落ちて着いただけ» の接触を衝突として数えない。
        //
        // WHY 反発だけ除くか: 反発は上へも乗せて弾く (repulseLift) ので、必ず最後に
        //     着地する。それをダメージにすると «押せば必ず 1 発入る» ことになり、
        //     «押しにダメージは無い。ダメージは飛んだ先でぶつかって入る» が崩れる。
        //     投げ (掴んで叩きつける) は逆に地面へ叩きつけるのが目的なので除かない。
        if (m_repelled) {
            const Vector3 velocity = physics.GetVelocity();
            const float horizontal = std::sqrt(velocity.x * velocity.x +
                                               velocity.z * velocity.z);
            if (horizontal < std::abs(velocity.y)) {
                EndFlight(PullPhase::Idle);
                m_repelled = false;
                return;
            }
        }

        if (info.approachSpeed < MinImpactSpeed()) return;
        RegisterImpact(*info.other, info.contactPoint, info.contactNormal,
                       info.approachSpeed, info.impactImpulse);
        return;
    }

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
    case PullPhase::Held:       debugPhase = "Held";       break;
    case PullPhase::Thrown:     debugPhase = "Thrown";     break;
    case PullPhase::Armed:      debugPhase = "Armed";      break;
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

    // 7.3 ① の「引き絞られる」を線の張りで見せる。溜めが進むほど太く明るく、
    // 脈も速くなる。飛んでいる間は逆に細く落として、線が «力» から «軌跡» へ
    // 変わったことを絵で切り替える。
    //
    // WHY 明滅を線の «明るさ» でやるか: 太さだけを振ると近距離で画面を覆い、
    //     アルファだけを振ると背景の明るさで消える。HDR の RGB ならどちらも起きず、
    //     そのままブルームが拾って «電圧が上がった» に見える。
    const float charge = IsFlying() ? 1.0f : WindupProgress();
    const float pulse  = 0.78f + 0.22f * std::sin(Time::time * Lerp(9.0f, 34.0f, charge));
    const float glow   = Lerp(0.9f, 2.6f, charge) * pulse;
    const float width  = linkWidth * Lerp(0.6f, 1.35f, charge) * (IsFlying() ? 0.55f : 1.0f);

    const Vector4 fromColor = PolarityColor(self->Current());
    const Vector4 toColor   = PolarityColor(otherTarget->Current());

    gameplay.SetLineEnabled(true);
    gameplay.SetLine(LinkPoint(), partnerPoint, true);
    // 自分の極から相手の極へのグラデーション。どちらへ飛んでいるかが線の色で読める。
    gameplay.SetLineColors({ fromColor.x * glow, fromColor.y * glow, fromColor.z * glow, 1.0f },
                           { toColor.x   * glow, toColor.y   * glow, toColor.z   * glow, 1.0f });
    gameplay.SetLineWidth(width, width);
}

} // namespace sandbox
