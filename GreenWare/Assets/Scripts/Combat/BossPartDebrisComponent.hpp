/// @file    BossPartDebrisComponent.hpp
/// @brief   もいだ部位の «その後»。床に残り、ボスに拾われて飛んでくる（弾ける）
/// @author  Hasegawa Jin
/// @date    2026-09-11
///
/// ボス 2 体が同じ物を使う（Docs/part-break.md）。
///   - コア（Spider）: とどめでもげた**脚 1 本**（`BossRigComponent::SpawnLegDebris`）
///   - サーペント     : とどめで潰れた**節 1 本**（`SerpentBodyComponent::DropDebris`）
///
/// 生成側が「部位の分割メッシュを静的メッシュとして写した GameObject」＋剛体＋
/// コライダーまで組んでから、このスクリプトを載せて `Setup` を呼ぶ。
/// こちらが持つのは **一生（落ちる → 床に居る → 拾われる → 飛ぶ → また床）** だけ。
///
/// WHY «落として消す» をやめたか (2026-09-11):
///   もげた脚は 2.6 秒で砕いて消していた（盤面を汚さないため）。だが消してしまうと
///   **部位破壊が «相手が減る» ことしか生まない** ── 終盤ほど盤面が静かになる。
///   床に残せば、同じ 1 つの物が «プレイヤーの遮蔽» と «ボスの弾» の両方になり、
///   落とした数がそのまま盤面の濃さになる。
///
/// WHY 飛んでくる部位を «弾ける» 一撃にするか:
///   落とした数 = 弾ける機会の数。終盤ほど飛んでくる物が増え、同時に終盤ほど
///   崩しが速く溜まる。圧と手応えが同じ 1 つの仕掛けから出る。
///   `Parryable` で撃てば Just 窓・連続弾き・土壇場・閃光まで既存の経路が丸ごと乗る。
///
/// WHY 剛体を捨てて transform を直に運ばないか:
///   浮いている間・飛んでいる間も床と壁に当たり続けてほしい。速度だけ書いて
///   重力の掛かり方（SetGravityScale）を切り替えれば、落ちる・浮く・飛ぶが
///   1 つの剛体のまま繋がり、最後はまた普通に床へ落ち着く。
#pragma once

#include <Engine/Scene/Components/ColliderComponent.hpp>
#include <Engine/Scene/Components/RigidBodyComponent.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Math/Vector3.hpp>
#include <Scripts/Combat/PlayerHit.hpp>
#include <Scripts/Game/CombatManagerComponent.hpp>
#include <Scripts/Game/VfxManagerComponent.hpp>
#include <Scripts/Utils/BladeColors.hpp>
#include <Scripts/Utils/SeLibrary.hpp>
#include <algorithm>
#include <string>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

class BossPartDebrisComponent : public Script {
    FBZZ_SCRIPT(BossPartDebrisComponent)

public:
    FBZZ_GROUP("対象")
    FBZZ_FIELD_TAG(playerTag, "Player", "プレイヤーのタグ")

    FBZZ_GROUP("落下")
    FBZZ_FIELD_RANGE(float, settleSpeed, 0.45f, "落ち着く速さ", 0.0f, 3.0f)
    FBZZ_TOOLTIP("これ未満の速さ [m/s] が続いたら «床に落ち着いた» ＝ 拾われる対象になる")
    FBZZ_FIELD_RANGE(float, settleSeconds, 0.35f, "落ち着くまで [s]", 0.0f, 3.0f)
    FBZZ_FIELD_RANGE(float, settleTimeout, 6.0f, "強制的に落ち着かせる [s]", 1.0f, 30.0f)
    FBZZ_TOOLTIP("どこかで転がり続けても、この時間で «落ち着いた» 扱いにする。"
                 "**入れないと、坂や隙間で震え続ける 1 本が永久に拾われない**")

    // 磁力パルスの予兆 (0.85 秒) の間に浮き上がる。コアだけが使う。
    FBZZ_GROUP("引き寄せ")
    FBZZ_FIELD_RANGE(float, drawHeight, 3.4f, "浮く高さ [m]", 0.5f, 12.0f)
    FBZZ_TOOLTIP("プレイヤーの背 (2.5m) より上に置く ─ 低いと «浮いた» が «滑っている» に見える")
    FBZZ_FIELD_RANGE(float, drawRadius, 6.0f, "寄る半径 [m]", 1.0f, 20.0f)
    FBZZ_TOOLTIP("ボスからこの距離まで引き寄せる。コアは接地径 9m なので、"
                 "小さすぎると体へめり込む")
    FBZZ_FIELD_RANGE(float, drawGain, 3.6f, "引きの強さ", 0.5f, 20.0f)
    FBZZ_FIELD_RANGE(float, drawMaxSpeed, 12.0f, "引きの上限 [m/s]", 1.0f, 40.0f)
    FBZZ_TOOLTIP("比例のままだと 15m 先の 1 本が 54 m/s で突っ込んでくる")
    FBZZ_FIELD_RANGE(float, drawSpin, 3.0f, "浮いている間の回転", 0.0f, 20.0f)
    FBZZ_FIELD_RANGE(float, drawTimeout, 3.0f, "吸い上げの上限 [s]", 0.5f, 10.0f)
    FBZZ_TOOLTIP("撃たれないまま浮いていられる長さ。パルスが途中で潰れても床へ戻る")

    FBZZ_GROUP("射出")
    FBZZ_FIELD_RANGE(float, launchSpeed, 18.0f, "速さ [m/s]", 3.0f, 60.0f)
    FBZZ_TOOLTIP("プレイヤーの走り (10 m/s) より速くする。遅いと «走って避ける» が"
                 "常に正解になり、弾く理由が消える")
    FBZZ_FIELD_RANGE_INT(int, damage, 1, "ダメージ", 0, 10)
    FBZZ_FIELD_RANGE(float, hitRadius, 1.8f, "当たり半径 [m]", 0.2f, 6.0f)
    FBZZ_FIELD_RANGE(float, launchLifetime, 2.6f, "飛べる長さの上限 [s]", 0.3f, 10.0f)
    FBZZ_FIELD_RANGE(float, parryBounce, 0.45f, "弾かれた跳ね返り", 0.0f, 2.0f)
    FBZZ_TOOLTIP("弾かれたときに来た向きへ跳ね返る比。0 でその場に落ちる")

    FBZZ_FIELD_READ_ONLY(std::string, debugState, "Falling", "状態")

    /// 生成した側が呼ぶ。もげた勢いと、部位のおおよその大きさ [m]。
    ///
    /// ⚠ ここで剛体へ書いてはいけない。呼ばれるのは `AddScript` の直後で、context は
    ///   まだ結ばれていない ─ `GameObject::AddScript` は `unique_ptr` を積むだけで、
    ///   `SetContext` は `ScriptSystem` が後から呼ぶ。未結線の `scene` から
    ///   `GetComponent` を引くと空が返り、**もげた部位が速度も回転も貰えないまま
    ///   真下へ落ちる**。値は控えるだけにして、context が揃っている `OnStart` で当てる。
    void Setup(const Vector3& velocity, const Vector3& spin, float extent)
    {
        m_velocity = velocity;
        m_spin     = spin;
        m_extent   = std::max(extent, 0.5f);
    }

    /// 床に落ち着いているか。«拾える / 遮蔽になる / 口を塞ぐ» のすべてがこれ。
    [[nodiscard]] bool IsResting() const { return m_phase == Phase::Resting; }
    /// 磁力で浮いている（＝ 撃てる）か。
    [[nodiscard]] bool IsDrawn() const { return m_phase == Phase::Drawn; }
    /// 部位のおおよその大きさ [m]。塞いだ口の判定と VFX の大きさに使う。
    [[nodiscard]] float Extent() const { return m_extent; }

    /// 磁力で吸い上げ始める。コアの `BeginPulse` が呼ぶ。
    void Draw(const Vector3& bossPosition);

    /// 部位を target へ撃ち出す。浮いていても床に居ても撃てる。
    ///
    /// WHY 床からも直接撃てるようにするか: コアは磁力で «吸い上げてから» 撃つが、
    ///     蛇に磁力は無い ─ 突き上げと薙ぎが床の部位を **叩いて飛ばす**。
    ///     溜めの絵が違うだけで、飛んでからのことは同じなので入口を 1 つにする。
    void Launch(const Vector3& target);

    void OnStart() override;
    void OnUpdate() override;

private:
    /// 部位 1 つの一生。Resting へ何度でも戻ってくる ── 弾き返された部位は資源に戻る。
    enum class Phase : int { Falling = 0, Resting, Drawn, Flying };

    /// 飛んでいる部位がプレイヤーに届いたか。届いたら 1 回だけ返す。
    void ResolveHit();
    /// 重力を戻して落下へ帰す。
    void FallBack();

    /// 部位を «押しのける物» にするか、すり抜ける物にするか。
    ///
    /// WHY 浮いている間と飛んでいる間はすり抜けさせるか: 吸い上げ中はボスの体の
    ///     すぐ横に居るので、押し合うと部位もボスも小刻みに震える。飛んでいる間も、
    ///     当たりは距離で見ている (ResolveHit) ので剛体の接触は要らない。
    ///     **床に落ち着いている間だけ固い** ＝ そのときだけ遮蔽・足場・口の栓になる。
    void SetSolid(bool solid) const
    {
        if (auto* box = scene.GetComponent<BoxColliderComponent>()) box->SetTrigger(!solid);
    }

    [[nodiscard]] float Speed() const { return physics.GetVelocity().Length(); }

    Phase   m_phase    = Phase::Falling;
    float   m_slowFor  = 0.0f;
    float   m_phaseAge = 0.0f;
    /// この射出で飛ばす長さ [秒]。Launch が «狙った所へ届く時間» から決める。
    float   m_flyTime  = 0.0f;
    float   m_extent   = 2.0f;
    bool    m_dealt    = false;
    Vector3 m_anchor;   ///< 吸い上げ中の目標点。Draw が決めて動かさない。
    Vector3 m_flyDir;
    Vector3 m_velocity;
    Vector3 m_spin;
};

FBZZ_REFLECT(BossPartDebrisComponent)

inline void BossPartDebrisComponent::OnStart()
{
    // Setup が控えた勢いをここで当てる (上の ⚠ を参照)。
    if (auto* rb = scene.GetComponent<RigidBodyComponent>())
        if (rb->rigidBody) {
            rb->rigidBody->SetVelocity(m_velocity);
            rb->rigidBody->SetAngularVelocity(m_spin);
        }
}

inline void BossPartDebrisComponent::Draw(const Vector3& bossPosition)
{
    if (m_phase != Phase::Resting) return;

    // 吸い寄せ先はボスの «横» 上空。真上へ集めると 3 本が同じ点で重なって 1 本に見える。
    Vector3 out = transform.worldPosition - bossPosition;
    out.y = 0.0f;
    const Vector3 dir = out.NormalizedOr(Vector3::FORWARD);

    m_anchor = bossPosition + dir * std::max(drawRadius, 0.5f)
             + Vector3::UP * std::max(drawHeight, 0.1f);

    m_phase    = Phase::Drawn;
    m_phaseAge = 0.0f;
    debugState = "Drawn";

    SetSolid(false);
    physics.SetGravityScale(0.0f);
    physics.SetAngularVelocity(Vector3::UP * std::max(drawSpin, 0.0f));
    // 吸われ始めたことは音でしか気付けない ── 部位は足元ではなく背後にも落ちている。
    se::Play(audio, se::kEnvHazardWarn, 0.55f);
}

inline void BossPartDebrisComponent::Launch(const Vector3& target)
{
    if (m_phase != Phase::Resting && m_phase != Phase::Drawn) return;

    Vector3 to = target - transform.worldPosition;
    // 狙いは水平へ寄せる。真下へ叩き付けると回避も弾きも間に合わない角度になる。
    to.y *= 0.35f;
    const float reach = to.Length();
    m_flyDir = to.NormalizedOr(Vector3::FORWARD);

    // 狙った所を通り過ぎたら落とす。
    //
    // WHY 一定時間飛ばさないか: 部位は飛んでいる間すり抜けるので、外した 1 本は
    //     闘技場の壁も抜けて場外へ出る ── **弾いても外しても盤面から 1 本減る**
    //     ことになり、置いた資源が戦っているうちに消える。届く時間だけ飛ばして床へ返す。
    m_flyTime  = std::min(reach / std::max(launchSpeed, 0.1f) + 0.35f,
                          std::max(launchLifetime, 0.3f));
    m_phase    = Phase::Flying;
    m_phaseAge = 0.0f;
    m_dealt    = false;
    debugState = "Flying";

    SetSolid(false);
    physics.SetGravityScale(0.0f);
    physics.SetVelocity(m_flyDir * std::max(launchSpeed, 0.1f));
    // 回転は進行方向まわりに。縦に回すと «飛んでいる» が輪郭で読めなくなる。
    physics.SetAngularVelocity(m_flyDir * std::max(drawSpin, 0.0f) * 1.6f);
    se::Play(audio, se::kImpactWall, 0.7f);
}

inline void BossPartDebrisComponent::FallBack()
{
    m_phase    = Phase::Falling;
    m_phaseAge = 0.0f;
    m_slowFor  = 0.0f;
    debugState = "Falling";
    SetSolid(true);
    physics.SetGravityScale(1.0f);
}

inline void BossPartDebrisComponent::ResolveHit()
{
    if (m_dealt) return;

    GameObject* player = scene.FindWithTag(playerTag);
    if (!player) return;

    const Vector3 at = transform.worldPosition;
    Vector3 toPlayer = player->transform.worldPosition - at;
    toPlayer.y = 0.0f;
    if (toPlayer.Length() > std::max(hitRadius, 0.1f)) return;

    m_dealt = true;

    auto* combat = CombatManagerComponent::Instance();
    if (!combat) return;

    // 押しは «部位が来た向き»。飛んできた物に弾かれる形が正しい。
    const Vector3 source = at - m_flyDir;
    const PlayerHitResult result =
        combat->HitPlayer(player, std::max(damage, 0), &source, PlayerHitKind::Parryable);

    if (result == PlayerHitResult::Parried) {
        // 弾かれた部位は資源に戻る。跳ね返して落とすだけで、崩し・閃光・止めは
        // すべて PlayerParryComponent 側が既に返している。
        FallBack();
        physics.SetVelocity(-m_flyDir * std::max(launchSpeed, 0.1f) * Clamp01(parryBounce)
                            + Vector3::UP * 3.0f);
        return;
    }

    if (auto* vfx = VfxManagerComponent::Instance())
        vfx->PlayImpact(at, BladeSide::None, 0.7f, false);
    se::Play(audio, se::kImpactMid, 0.8f);
    FallBack();
}

inline void BossPartDebrisComponent::OnUpdate()
{
    const float dt = std::max(Time::deltaTime, 0.0f);
    m_phaseAge += dt;

    switch (m_phase) {
    case Phase::Falling: {
        // 跳ねている途中の «頂点で一瞬遅くなる» は数えない。
        m_slowFor = Speed() < std::max(settleSpeed, 0.0f) ? m_slowFor + dt : 0.0f;
        if (m_slowFor >= std::max(settleSeconds, 0.0f) ||
            m_phaseAge >= std::max(settleTimeout, 1.0f)) {
            m_phase    = Phase::Resting;
            m_phaseAge = 0.0f;
            debugState = "Resting";
        }
        break;
    }

    case Phase::Resting:
        debugState = "Resting";
        break;

    case Phase::Drawn: {
        // 目標へ比例で寄せる。着いたら止まるので «吸い上げられて漂う» になる。
        const Vector3 delta = m_anchor - transform.worldPosition;
        Vector3       want  = delta * std::max(drawGain, 0.1f);
        const float   speed = want.Length();
        const float   cap   = std::max(drawMaxSpeed, 1.0f);
        if (speed > cap) want = want * (cap / speed);
        physics.SetVelocity(want);
        if (m_phaseAge >= std::max(drawTimeout, 0.5f)) FallBack();
        break;
    }

    case Phase::Flying: {
        // 速さは毎フレーム押し直す。壁を擦ったときに減速したまま «漂う» にしない。
        physics.SetVelocity(m_flyDir * std::max(launchSpeed, 0.1f));
        ResolveHit();
        if (m_phase == Phase::Flying && m_phaseAge >= m_flyTime) FallBack();
        break;
    }
    }
}

} // namespace sandbox
