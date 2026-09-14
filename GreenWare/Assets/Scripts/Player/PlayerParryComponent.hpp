/// @file    PlayerParryComponent.hpp
/// @brief   弾き (Parry) と、倒れたボスへの «とどめ» (Execute)。同じボタンの 2 つの顔
/// @author  Hasegawa Jin
/// @date    2026-09-04
///
/// WHY 弾きと とどめ を 1 つのボタンにするか:
///   ボスが立っている間に要るのは «受ける» で、倒れている間に要るのは «仕留める»。
///   同時に要ることは無い。ボタンを分けると、倒れた 5 秒の最初の 1 秒を
///   «どのボタンだったか» に使わせることになる。押した瞬間にボスの状態で決まる方が速い。
///
/// WHY 窓を «押した瞬間から» 開けるか (クリップの受け姿勢に合わせないか):
///   受け姿勢は f4-9 (0.13〜0.30 秒) にあるが、そこまで待つと «押したのに弾けない»
///   0.1 秒が生まれる。弾きで一番大事なのは入力と結果が同じ瞬間にあることなので、
///   窓は 0 秒から開き、クリップは少し速く流して絵を判定へ寄せる (斬撃と同じ判断)。
///
/// WHY 空振りに硬直を置くか:
///   硬直が無いと弾きを連打するのが最適になり、予兆を読む理由が消える。
///   «外したらその手は受けられない» 重さは残しつつ、硬直は回避で打ち切れる
///   (2026-09-11)。0.55 秒の «何もできない» は押すこと自体をためらわせていたので、
///   0.40 秒へ縮め、代わりに «外したら転がって逃げる» という手を返した。
///
/// WHY 回避 > 弾き > 斬撃 の順か:
///   3 つとも上半身の同じ Slot を使う。優先が決まっていないと、弾きの絵の裏で斬撃の
///   判定が出る・転がりながら上半身だけ構える、が起きる。逃げる意思 (回避) が最も強く、
///   受ける意思 (弾き) は振っている最中でも通す。
#pragma once

#include <Engine/Scene/EntityRef.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Scripts/Camera/BossCameraDirectorComponent.hpp>
#include <Scripts/Combat/BossBreakComponent.hpp>
#include <Scripts/Combat/BossHitboxRigComponent.hpp>
#include <Scripts/Combat/BossPartComponent.hpp>
#include <Scripts/Combat/IBoss.hpp>
#include <Scripts/Combat/PlayerHit.hpp>
#include <Scripts/Combat/SerpentHitboxRigComponent.hpp>
#include <Scripts/Game/CameraFollowManagerComponent.hpp>
#include <Scripts/Game/CameraShakeManagerComponent.hpp>
#include <Scripts/Game/HitstopManagerComponent.hpp>
#include <Scripts/Game/RumbleManagerComponent.hpp>
#include <Scripts/Game/ScreenEffectManagerComponent.hpp>
#include <Scripts/Game/TimeManagerComponent.hpp>
#include <Scripts/Game/VfxManagerComponent.hpp>
#include <Scripts/Player/PlayerControllerComponent.hpp>
#include <Scripts/Utils/InputActions.hpp>
#include <Scripts/Utils/BladeColors.hpp>
#include <Scripts/Utils/MotionTempo.hpp>
#include <Scripts/Utils/SeLibrary.hpp>
#include <algorithm>
#include <cmath>
#include <string>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

class PlayerParryComponent : public Script {
    FBZZ_SCRIPT(PlayerParryComponent)

public:
    // 斬撃と同じ Override レイヤーへ差し込む。レイヤーの重みは BladeComponent が
    // Slot の重みから毎フレーム流しているので、ここは Slot を鳴らすだけでよい。
    FBZZ_GROUP("動き")
    FBZZ_FIELD(std::string, layerName, "Attack", "Layer")
    FBZZ_FIELD_FILE(parryClipFile,
        "guid:666b941eebe9b223466ebc40c8219f33|Library/Baked/666b941eebe9b223466ebc40c8219f33/anims/Katana_Parry.anim",
        "Parry Clip", ".anim,.fbx")
    FBZZ_FIELD(std::string, parryClipName, "Katana_Parry", "Parry Clip Name")
    // Katana_Parry は 27F / 0.90s。受け f4-9、弾き返し f12。
    FBZZ_FIELD_RANGE(float, parrySpeed, 1.35f, "Parry Speed", 0.5f, 3.0f)
    FBZZ_TOOLTIP("空振りの硬直の再生速度。構え・弾き返しは «緩急» の値で流れる")
    FBZZ_FIELD_FILE(executeClipFile,
        "guid:9881396f555b1ba645879af4182ab242|Library/Baked/9881396f555b1ba645879af4182ab242/anims/Katana_Iai.anim",
        "Execute Clip", ".anim,.fbx")
    FBZZ_FIELD(std::string, executeClipName, "Katana_Iai", "Execute Clip Name")
    // Katana_Iai は 78F / 2.60s。斬り下ろしが f25-28 = 0.87 秒。
    FBZZ_FIELD_RANGE(float, executeHitTime, 26.0f / 30.0f, "Execute Hit Time", 0.05f, 3.0f)
    FBZZ_FIELD_RANGE(float, executeSpeed, 1.5f, "Execute Speed", 0.5f, 3.0f)
    FBZZ_TOOLTIP("居合の再生速度。1 だと斬り下ろしまで 0.87 秒 ─ 倒れている 5 秒に対して重すぎる")
    FBZZ_FIELD_RANGE(float, fadeIn,  0.03f, "フェードイン",  0.0f, 0.5f)
    FBZZ_FIELD_RANGE(float, fadeOut, 0.15f, "フェードアウト", 0.0f, 0.5f)

    // WHY 弾きを等速で流さないか: 受けの姿勢 (f4) まで等速だと、押してから «構えた» が
    //     見えるまで 0.1 秒かかる。窓は押した瞬間から開いているので、絵が判定に遅れる。
    //     構えまでは一気に、構えたら窓のあいだ留め、弾けたら鋭く返す。
    FBZZ_GROUP("緩急")
    FBZZ_FIELD_RANGE(float, parryGuardTime, 4.0f / 30.0f, "Guard Pose At [s]", 0.0f, 1.0f)
    FBZZ_TOOLTIP("Katana_Parry の受けの姿勢が始まるクリップ秒数 (f4)")
    FBZZ_FIELD_RANGE(float, parrySnapSpeed, 2.4f, "Guard Snap", 0.5f, 5.0f)
    FBZZ_TOOLTIP("押してから受けの姿勢までの再生速度。押した瞬間に «構えた» が見える速さ")
    FBZZ_FIELD_RANGE(float, parryHoldSpeed, 0.55f, "Guard Hold", 0.05f, 2.0f)
    FBZZ_TOOLTIP("受付時間のあいだの再生速度。受けの姿勢 (f4-9) を窓の終わりまで保つ")
    FBZZ_FIELD_RANGE(float, parryRiposteSpeed, 1.8f, "Riposte Speed", 0.5f, 4.0f)
    FBZZ_TOOLTIP("弾けた後の弾き返しの再生速度")
    // WHY 居合を «溜めて一閃» にするか: 居合は抜くまでの静と抜いた瞬間の動の落差そのもの。
    //     等速だと 0.58 秒かけてただ振り下ろす絵になる。斬り下ろしの時刻は変えない。
    FBZZ_FIELD_RANGE(float, executeWindup, 0.55f, "Execute Windup", 0.1f, 1.0f)
    FBZZ_FIELD_RANGE(float, executeWindupPower, 3.0f, "Execute Strike Curve", 0.5f, 6.0f)
    FBZZ_FIELD_RANGE(float, executeFollowEnd, 0.4f, "Execute Zanshin Speed", 0.05f, 2.0f)
    FBZZ_TOOLTIP("斬り下ろした後の残心の再生速度 [Execute Speed に対する比]。0.35 秒かけてここまで落とす")

    FBZZ_GROUP("タイミング")
    FBZZ_FIELD_RANGE(float, windowSeconds, 0.22f, "受付時間", 0.05f, 1.0f)
    FBZZ_TOOLTIP("押してから弾ける時間。踏みつけの叩きつけは 0.10 秒なので、"
                 "予兆 (静止 0.13 秒) の途中で押せば必ず入る長さ")
    // WHY 窓の «頭» ではなく «尻» を Just にするか:
    //   窓は押した瞬間から開く。攻撃が来る «直前» に押すほど、当たるのは窓の
    //   早い時刻になる ─ つまり窓の頭で受けた弾きが «ぎりぎりまで引き付けた» 弾き。
    //   早押しで開けておいた窓の尻で拾う弾きは、読んだのではなく保険を掛けた弾き。
    FBZZ_FIELD_RANGE(float, justParrySeconds, 0.08f, "Just Window", 0.0f, 0.3f)
    FBZZ_TOOLTIP("押してからこの秒数以内に受けた弾きは «Just»。崩しが多く溜まり、"
                 "スローと閃光が深くなる。0 で無効")
    FBZZ_FIELD_RANGE(float, recoverySeconds, 0.40f, "Whiff Recovery", 0.05f, 2.0f)
    FBZZ_TOOLTIP("外したときの硬直。連打を最適にしないための重さ。回避で打ち切れる")
    FBZZ_FIELD_RANGE(float, successRecovery, 0.22f, "Hit Recovery", 0.0f, 1.0f)
    FBZZ_TOOLTIP("弾けたときの硬直。攻撃ボタンで打ち切って斬り返せる")
    // WHY 預かるか: 弾きは一番タイミングを狙って押すボタンなのに、硬直中・回避中の
    //     押下を捨てていた。明ける直前に押して何も出ないのが、手触りを一番損ねる。
    FBZZ_FIELD_RANGE(float, parryBufferSeconds, 0.12f, "Parry Buffer", 0.0f, 0.4f)
    FBZZ_TOOLTIP("硬直中・回避中に押した弾きを預かる秒数。明けた瞬間に構える。"
                 "長くすると «早押しの保険» になるので短く")
    FBZZ_FIELD_RANGE(float, executeLock, 1.05f, "Execute Lock", 0.2f, 3.0f)
    FBZZ_TOOLTIP("とどめの間、他の入力を受け付けない時間。斬り下ろしより少し長く")
    FBZZ_FIELD_RANGE(float, executeRange, 3.4f, "Execute Range", 0.5f, 10.0f)
    FBZZ_TOOLTIP("倒れたボスの部位からこの距離以内なら、押した瞬間にとどめへ入る")
    // WHY 高さの帯が要るか (2026-09-10): 届くかどうかは水平距離だけで測っている。
    //     背のコアが とどめ の的になったので、地上に立っていても «真上 5m の
    //     コア» が水平 0m として届いてしまい、何も無い空へ居合が出る。
    FBZZ_FIELD_RANGE(float, executeHeight, 3.0f, "Execute Height", 0.5f, 20.0f)
    FBZZ_TOOLTIP("足元からこの高さの帯にある部位だけが とどめ の的になる。"
                 "甲板 (足元から +5m 上) のコアと地上の脚を混ぜないための仕切り")
    FBZZ_FIELD_RANGE(float, moveScale, 0.35f, "Move Scale (busy)", 0.05f, 1.0f)

    // «パキッ» の配分。止め・閃光・破片は一瞬で、余韻を残さない。
    // ビネットや収差 (Distort) は使わない ─ 画面の縁が暗くなるのは «受けた» の語。
    FBZZ_GROUP("Parry Feel")
    FBZZ_FIELD_RANGE(float, parryHitStop, 0.85f, "ヒットストップ", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, parryShake, 0.50f, "揺れ", 0.0f, 1.0f)
    FBZZ_FIELD_COLOR(parryFlashColor, (Vector4{ 0.85f, 0.95f, 1.0f, 1.0f }), "Flash Color")
    FBZZ_FIELD_RANGE(float, parryFlash, 0.55f, "閃光", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, parryFlashSeconds, 0.07f, "閃光の長さ [秒]", 0.0f, 0.5f)
    FBZZ_FIELD_RANGE(float, parrySlowScale, 0.30f, "スロー", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, parrySlowSeconds, 0.12f, "スロー時間 [秒]", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, parryRumble, 0.95f, "振動", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, parryFov, 0.35f, "FOV の跳ね", 0.0f, 1.0f)
    FBZZ_TOOLTIP("弾いた瞬間に画角を開く強さ。重い手はこの 1.6 倍まで開く")
    FBZZ_FIELD_RANGE(float, parryVfxHeight, 1.35f, "VFX Height", 0.0f, 3.0f)
    FBZZ_FIELD_RANGE(float, parryVfxForward, 1.1f, "VFX Forward", 0.0f, 3.0f)
    FBZZ_FIELD_RANGE_INT(int, heavyDamageAt, 3, "Heavy At Damage", 1, 100)
    FBZZ_TOOLTIP("弾いた一撃のダメージがこれ以上なら «重い手» として扱う (突進 = 3)")

    FBZZ_GROUP("Execute Feel")
    FBZZ_FIELD_RANGE(float, executeHitStop, 1.0f, "ヒットストップ", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, executeShake, 0.9f, "揺れ", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, executeSlowScale, 0.25f, "スロー", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, executeSlowSeconds, 0.35f, "スロー時間 [秒]", 0.0f, 2.0f)
    FBZZ_FIELD_RANGE(float, executeFov, 1.0f, "FOV の跳ね", 0.0f, 1.0f)
    FBZZ_TOOLTIP("とどめが入った瞬間に画角を開く強さ。盤面で一番大きい一撃なので最大")

    FBZZ_GROUP("デバッグ")
    FBZZ_FIELD_READ_ONLY(std::string, debugPhase, "Idle", "位相")
    FBZZ_FIELD_READ_ONLY(int, debugParries, 0, "Parries")
    FBZZ_FIELD_READ_ONLY(int, debugJustParries, 0, "Just Parries")
    FBZZ_FIELD_READ_ONLY(int, debugExecutions, 0, "Executions")

    /// 弾きの窓が開いているか。PlayerComponent が一撃を受けた瞬間に読む。
    [[nodiscard]] bool IsParryActive() const { return m_phase == Phase::Window; }
    /// 弾きかとどめの最中か。剣はこの間、攻撃を受け付けない。
    [[nodiscard]] bool IsBusy() const { return m_phase != Phase::Idle; }
    /// 弾けた後の硬直中か。この間は攻撃ボタンで硬直を打ち切って斬り返せる。
    [[nodiscard]] bool CanAttackCancel() const
    { return m_phase == Phase::Recovery && m_succeeded; }
    /// 構え・硬直を打ち切り、上半身のクリップも畳む。とどめは演出ごと預かっているので切らない。
    void Cancel();

    // ── 出来事の回数 ────────────────────────────────────────────────────────
    // WHY «起きた» を伝える通知ではなく回数で返すか:
    //     弾きは 1 フレームの出来事で、見ている側 (刃の焼き・HUD) は自分の更新順で
    //     読む。通知を配ると受け手ごとに «取り逃した / 二重に受けた» が出るが、
    //     回数なら前フレームとの差だけで «今フレームに何回起きたか» が誰にでも出る。
    [[nodiscard]] int ParryCount() const { return m_parries; }
    /// 読み切って弾いた回数。ParryCount の内数。
    [[nodiscard]] int JustParryCount() const { return m_justParries; }
    [[nodiscard]] int ExecuteCount() const { return m_executions; }

    /// 弾けた。手触りを返し、崩しを溜め、ボスへ «弾かれた» を伝える。
    /// @param amount 弾いた一撃のダメージ (重さの判定に使う)
    /// @param from   攻撃してきた物の位置。nullptr なら正面とみなす
    void OnParried(int amount, const Vector3* from);

    /// 倒れているボスの部位が届く距離に居れば、とどめを始めて true。
    /// 攻撃ボタンからも呼ばれる (倒れている間はどのボタンでも仕留められる)。
    bool TryExecute();

    void SetController(PlayerControllerComponent* controller) { m_controller = controller; }

    void OnStart()  override;
    void OnUpdate() override;

private:
    enum class Phase : int { Idle = 0, Window, Recovery, Execute };

    void BeginParry();
    void ResolveExecute();
    /// とどめが届く部位と、その持ち主。無ければ nullptr。
    [[nodiscard]] GameObject* FindExecutablePart(GameObject*& outRoot) const;
    /// 部位からボス本体を引く。4 足と蛇でリグが違うので両方試す。
    [[nodiscard]] static GameObject* RootOf(GameObject* part);
    /// 今戦っているボスの本体。居なければ nullptr。
    [[nodiscard]] GameObject* Boss() const { return FindBossOnBoard(scene); }
    void SetPhase(Phase phase, float seconds);
    /// 弾き・とどめのクリップの再生速度を位相ごとに決める (緩急)。
    void DriveTempo(float dt);

    PlayerControllerComponent* m_controller = nullptr;

    Phase m_phase = Phase::Idle;
    float m_timer = 0.0f;
    /// 預かっている弾き入力の残り [秒]。
    float m_buffer = 0.0f;
    /// 今の硬直が «弾けた» 後か (空振りの硬直と分ける)。
    bool  m_succeeded = false;
    /// とどめの斬り下ろしを既に判定したか。1 回のとどめで 1 度だけ。
    bool      m_executeResolved = false;
    EntityRef m_executePart;
    EntityRef m_executeRoot;
    int m_parries     = 0;
    int m_justParries = 0;
    int m_executions  = 0;
};

FBZZ_REFLECT(PlayerParryComponent)

inline void PlayerParryComponent::OnStart()
{
    m_phase = Phase::Idle;
    m_timer = 0.0f;
    m_buffer    = 0.0f;
    m_succeeded = false;
    m_executeResolved = false;
    m_executePart = {};
    m_executeRoot = {};
    m_parries     = 0;
    m_justParries = 0;
    m_executions  = 0;
    debugPhase       = "Idle";
    debugParries     = 0;
    debugJustParries = 0;
    debugExecutions  = 0;
    se::EnsureSource(scene);
}

inline void PlayerParryComponent::SetPhase(Phase phase, float seconds)
{
    m_phase = phase;
    m_timer = std::max(seconds, 0.0f);
    switch (phase) {
    case Phase::Idle:     debugPhase = "Idle";     break;
    case Phase::Window:   debugPhase = "Window";   break;
    case Phase::Recovery: debugPhase = "Recovery"; break;
    case Phase::Execute:  debugPhase = "Execute";  break;
    }
}

inline GameObject* PlayerParryComponent::RootOf(GameObject* part)
{
    if (GameObject* root = BossHitboxRigComponent::BossRootOf(part)) return root;
    return SerpentHitboxRigComponent::SerpentRootOf(part);
}

inline GameObject* PlayerParryComponent::FindExecutablePart(GameObject*& outRoot) const
{
    outRoot = nullptr;
    GameObject* boss = Boss();
    if (!boss) return nullptr;
    const IBoss* iboss = IBoss::Of(boss);
    if (!iboss || !iboss->IsToppled()) return nullptr;

    // 部位の当たり判定 (脚の膝下・蛇の節) の中で一番近いもの。部位はレンダラーを
    // 持たないので、太さは部位が申告している hitRadius を足す。
    const Vector3 origin = transform.worldPosition;
    const float   reach  = std::max(executeRange, 0.1f);

    GameObject* best     = nullptr;
    float       bestDist = 0.0f;
    for (GameObject* object : scene.FindObjectsOfType<BossPartComponent>()) {
        if (!object || !object->activeInHierarchy()) continue;
        const auto* part = scene.GetScript<BossPartComponent>(object);
        if (!part || part->IsBroken()) continue;
        if (RootOf(object) != boss) continue;

        const Vector3 delta  = object->transform.worldPosition - origin;
        const float   radius = std::max(part->hitRadius, 0.0f);
        if (std::abs(delta.y) > std::max(executeHeight, 0.1f) + radius) continue;

        Vector3 flat = delta;
        flat.y = 0.0f;
        if (flat.Length() - radius > reach) continue;

        // WHY 選ぶのは 3 次元の近さか (2026-09-10): 届くかどうかは水平で測る
        //     (脚の膝下は胸の高さにあり、3D で測ると立ち位置で届かなくなる) が、
        //     «どれを斬るか» まで水平で決めると、甲板に立ったとき真下 5m の脚と
        //     目の前のコアが同じ «0m» になる。届く的の中から一番近いものを選ぶ。
        const float distance = delta.Length() - radius;
        if (!best || distance < bestDist) {
            best     = object;
            bestDist = distance;
        }
    }
    if (best) outRoot = boss;
    return best;
}

inline bool PlayerParryComponent::TryExecute()
{
    if (IsBusy()) return false;

    GameObject* root = nullptr;
    GameObject* part = FindExecutablePart(root);
    if (!part || !root) return false;

    m_executePart     = EntityRef{ part->GetID() };
    m_executeRoot     = EntityRef{ root->GetID() };
    m_executeResolved = false;
    SetPhase(Phase::Execute, std::max(executeLock, 0.2f));

    // 斬る相手を向く。居合は正面へ振り下ろすので、向いていないと空を斬る。
    if (m_controller) {
        Vector3 to = part->transform.worldPosition - transform.worldPosition;
        to.y = 0.0f;
        if (to.LengthSq() > EPSILON) m_controller->RequestFacing(to);
    }

    if (!executeClipFile.empty())
        animator.PlaySlot(layerName, executeClipFile, executeClipName, fadeIn, fadeOut,
                          std::max(executeSpeed, 0.1f), /*loop=*/false);

    // 切断面へ寄るカットイン。居合の尺だけ預かり、もげたら返る。
    if (auto* camera = BossCameraDirectorComponent::Instance())
        camera->PlayAt(BossShot::Execute, part->transform.worldPosition);

    se::Play(audio, se::BladeChargeSlash(BladeSide::Right));
    if (auto* pad = RumbleManagerComponent::Instance()) pad->Rumble(0.4f, 0.2f, 0.10f);
    return true;
}

inline void PlayerParryComponent::BeginParry()
{
    SetPhase(Phase::Window, std::max(windowSeconds, 0.05f));
    m_succeeded = false;

    if (!parryClipFile.empty())
        animator.PlaySlot(layerName, parryClipFile, parryClipName, fadeIn, fadeOut,
                          std::max(parrySpeed, 0.1f), /*loop=*/false);

    // 構える «シャッ»。当たったかどうかとは別に、構えたこと自体を返す。
    se::Play(audio, se::BladeSwing(BladeSide::Left, 0), 0.7f);
    if (auto* pad = RumbleManagerComponent::Instance()) pad->Rumble(0.0f, 0.18f, 0.05f);
}

inline void PlayerParryComponent::OnParried(int amount, const Vector3* from)
{
    ++m_parries;
    debugParries = m_parries;

    const bool heavy = amount >= std::max(heavyDamageAt, 1);
    // 窓の頭で受けたか。m_timer は窓の残りなので、経過 = 窓 − 残り。
    const float elapsed = std::max(windowSeconds, 0.05f) - std::max(m_timer, 0.0f);
    const bool  just    = justParrySeconds > 0.0f && elapsed <= justParrySeconds;
    if (just) {
        ++m_justParries;
        debugJustParries = m_justParries;
    }
    // Just は手触りも一段深い。止めは同じ (これ以上は操作が重い) で、
    // 戻りのスローと閃光と画角で «読み切った» を言う。
    const float feel = just ? 1.6f : 1.0f;

    // 弾いた «場所»。刃は正面にあるので、自分から攻撃してきた側へ少し出た所。
    Vector3 toward = transform.worldRotation * Vector3::FORWARD;
    if (from) {
        Vector3 d = *from - transform.worldPosition;
        d.y = 0.0f;
        if (d.LengthSq() > EPSILON) toward = d.Normalized();
    }
    toward.y = 0.0f;
    toward = toward.NormalizedOr(Vector3::FORWARD);
    const Vector3 point = transform.worldPosition + toward * std::max(parryVfxForward, 0.0f)
                        + Vector3{ 0.0f, std::max(parryVfxHeight, 0.0f), 0.0f };

    GameObject* boss = Boss();

    // 止め。世界を一瞬固め、当事者 2 体の芝居も固める ─ «噛み合った» が出るのは
    // 刃と脚が同じ 1 コマで止まるからで、片方だけだと «すり抜けた» に見える。
    if (auto* stop = HitstopManagerComponent::Instance()) {
        stop->Hit(Clamp01(parryHitStop));
        stop->FreezeAnimation(scene.Self(), Clamp01(parryHitStop));
        if (boss) stop->FreezeAnimation(boss, Clamp01(parryHitStop));
    }
    // 止めが解けた直後の数フレームだけ遅くする。«パキッ» の後に世界が戻る速さが
    // «弾いた» の重さになる。長いと «スローが掛かった» という別の出来事になる。
    if (parrySlowSeconds > 0.0f && parrySlowScale < 1.0f)
        if (auto* timeManager = TimeManagerComponent::Instance())
            timeManager->SlowFor(parrySlowScale, parrySlowSeconds * feel, 0.0f);
    // 閃光は白。画面全体を一瞬だけ持ち上げる。縁を暗くする Distort / Surge は使わない
    // (ビネットは «受けた» の語で、弾きは «防いだ» の語)。
    if (auto* screen = ScreenEffectManagerComponent::Instance())
        screen->Flash(parryFlashColor, Clamp01(parryFlash * feel),
                      std::max(parryFlashSeconds, 0.01f) * feel);
    if (auto* shake = CameraShakeManagerComponent::Instance())
        shake->Shake(Clamp01(parryShake) * (heavy ? 1.3f : 1.0f));
    // 画角を弾いた側へ «開く»。刃と攻撃が触れた瞬間に画面が一瞬広がると、
    // 止めの後に世界が戻る速さが «弾き返した» 勢いとして読める。
    if (parryFov > 0.0f)
        if (auto* follow = CameraFollowManagerComponent::Instance())
            follow->PunchFov(Clamp01(parryFov) * (heavy ? 1.6f : 1.0f) * feel);
    if (auto* pad = RumbleManagerComponent::Instance())
        pad->Rumble(heavy ? 0.6f : 0.3f, Clamp01(parryRumble), heavy ? 0.2f : 0.13f);
    if (auto* vfx = VfxManagerComponent::Instance())
        vfx->PlayParry(point, -toward, just ? 1.0f : (heavy ? 1.0f : 0.5f), just);

    // 音は乾いた «バンッ» と刀の «キン» を重ねる。爆発の音を使うと «壊れた» に聞こえる。
    se::Play(audio, se::kBladeRepulse, 1.0f);
    se::Play(audio, se::BladeHitFinish(BladeSide::Right), heavy ? 1.0f : 0.8f);
    // Just だけ «満ちた» の鐘を重ねる。溜めとジャスト回避で覚えた «良いことが起きた» の音。
    if (just) se::Play(audio, se::kBladeChargeUpFull, 0.8f);

    // 崩しを溜め、ボスへ «弾かれた» を渡す。脚が跳ね上がる・突進が転ぶのはボスの側。
    if (boss) {
        if (auto* brk = scene.GetScript<BossBreakComponent>(boss)) brk->AddParry(heavy, just);
        if (auto* iboss = IBoss::Of(boss)) iboss->OnParried(point);
    }

    // 弾けたら短い硬直で次へ。窓の残りは捨てる (1 回の構えで 2 発は受けない)。
    SetPhase(Phase::Recovery, successRecovery);
    m_succeeded = true;
}

inline void PlayerParryComponent::Cancel()
{
    if (m_phase != Phase::Window && m_phase != Phase::Recovery) return;
    SetPhase(Phase::Idle, 0.0f);
    m_succeeded = false;
    animator.StopSlot(layerName, fadeOut);
}

inline void PlayerParryComponent::ResolveExecute()
{
    m_executeResolved = true;

    GameObject* part = m_executePart.Resolve(scene);
    GameObject* root = m_executeRoot.Resolve(scene);
    IBoss*      boss = root ? IBoss::Of(root) : nullptr;

    // 起き上がられていたら空振り。斬り下ろしの絵は最後まで流す。
    if (!part || !boss || !boss->IsToppled() || !boss->Execute(part, transform.worldPosition)) {
        se::Play(audio, se::BladeSwing(BladeSide::Right, 2), 0.8f);
        return;
    }

    ++m_executions;
    debugExecutions = m_executions;

    if (auto* stop = HitstopManagerComponent::Instance()) {
        stop->Hit(Clamp01(executeHitStop));
        stop->FreezeAnimation(scene.Self(), Clamp01(executeHitStop));
        stop->FreezeAnimation(root, Clamp01(executeHitStop));
    }
    if (executeSlowSeconds > 0.0f && executeSlowScale < 1.0f)
        if (auto* timeManager = TimeManagerComponent::Instance())
            timeManager->SlowFor(executeSlowScale, executeSlowSeconds, 0.02f);
    if (auto* shake = CameraShakeManagerComponent::Instance()) shake->Shake(Clamp01(executeShake));
    if (executeFov > 0.0f)
        if (auto* follow = CameraFollowManagerComponent::Instance())
            follow->PunchFov(Clamp01(executeFov));
    if (auto* pad = RumbleManagerComponent::Instance()) pad->Rumble(1.0f, 0.7f, 0.35f);
    if (auto* screen = ScreenEffectManagerComponent::Instance())
        screen->Flash(Vector4{ 1.0f, 1.0f, 1.0f, 1.0f }, 0.4f, 0.08f);
    se::Play(audio, se::kImpactFinal);
    se::Play(audio, se::BladeHitFinish(BladeSide::Left));
}

inline void PlayerParryComponent::DriveTempo(float dt)
{
    if (layerName.empty() || !animator.IsSlotPlaying(layerName)) return;

    switch (m_phase) {
    case Phase::Window:
        animator.SetSlotSpeed(layerName, animator.GetSlotTime(layerName) < parryGuardTime
                                             ? parrySnapSpeed : parryHoldSpeed);
        break;

    case Phase::Recovery:
        animator.SetSlotSpeed(layerName, m_succeeded ? parryRiposteSpeed : parrySpeed);
        break;

    case Phase::Execute: {
        const float lock    = std::max(executeLock, 0.2f);
        const float speed   = std::max(executeSpeed, 0.1f);
        const float hitAt   = std::max(executeHitTime, 0.01f) / speed;
        const float elapsed = lock - m_timer;
        if (elapsed < hitAt) {
            // 斬り下ろしの時刻 (ResolveExecute) にクリップの斬り下ろしがちょうど来る。
            const float target = executeHitTime
                * tempo::WindupProgress(elapsed / hitAt, executeWindup, executeWindupPower);
            animator.SetSlotSpeed(layerName, tempo::SpeedToReach(
                target, animator.GetSlotTime(layerName), dt, speed));
        } else {
            animator.SetSlotSpeed(layerName, speed * tempo::FollowThrough(
                elapsed - hitAt, 1.0f, executeFollowEnd, 0.35f));
        }
        break;
    }

    case Phase::Idle:
        break;
    }
}

inline void PlayerParryComponent::OnUpdate()
{
    if (!enabled) return;
    const float dt = std::max(Time::deltaTime, 0.0f);

    // 登攀のように数秒またぐ拘束は cutscene には乗らない (BladeComponent と同じ理由)。
    const bool held = cutscene::HoldsPlayer(Time::unscaledTime)
                   || (m_controller && m_controller->IsInputLocked());
    const bool dodging = m_controller && m_controller->IsDodging();

    // 回避は弾きより優先する。構えていても硬直中でも、転がった瞬間に畳む。
    if (dodging) Cancel();

    if (held) m_buffer = 0.0f;
    else if (input.GetActionDown(actions::kParry))
        m_buffer = std::max(parryBufferSeconds, 1.0e-4f);

    if (m_buffer > 0.0f && !IsBusy() && !dodging) {
        m_buffer = 0.0f;
        // 倒れている相手が届く所に居れば とどめ。居なければ弾き。
        if (!TryExecute()) BeginParry();
    }
    m_buffer = std::max(0.0f, m_buffer - dt);

    if (m_phase == Phase::Idle) return;

    // 構えている間は足を鈍らせる。全速で走りながら弾けると «受けた» に見えない。
    if (m_controller) m_controller->RequestMoveSpeedScale(std::max(moveScale, 0.05f));

    // 構えは攻撃してきた側へ向く。向いていない弾きは «偶然当たった» に見える。
    if (m_phase == Phase::Window && m_controller)
        if (GameObject* boss = Boss()) {
            Vector3 to = boss->transform.worldPosition - transform.worldPosition;
            to.y = 0.0f;
            if (to.LengthSq() > EPSILON) m_controller->RequestFacing(to);
        }

    m_timer -= dt;

    switch (m_phase) {
    case Phase::Window:
        if (m_timer <= 0.0f) SetPhase(Phase::Recovery, recoverySeconds);
        break;

    case Phase::Recovery:
        if (m_timer <= 0.0f) SetPhase(Phase::Idle, 0.0f);
        break;

    case Phase::Execute: {
        const float lock = std::max(executeLock, 0.2f);
        const float hitAt = std::max(executeHitTime, 0.01f) / std::max(executeSpeed, 0.1f);
        const float elapsed = lock - m_timer;
        if (!m_executeResolved && elapsed >= hitAt) ResolveExecute();
        if (m_timer <= 0.0f) {
            // 居合は 2.6 秒ある。斬り下ろしが済んだら残心を待たずに畳む。
            animator.StopSlot(layerName, fadeOut);
            SetPhase(Phase::Idle, 0.0f);
        }
        break;
    }

    case Phase::Idle:
        break;
    }

    DriveTempo(dt);
}

} // namespace sandbox
