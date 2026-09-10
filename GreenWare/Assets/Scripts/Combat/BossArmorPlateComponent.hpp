/// @file    BossArmorPlateComponent.hpp
/// @brief   とどめで剥がれた装甲板。床に残り、磁力パルスで吸い上げられて飛んでくる
/// @author  Hasegawa Jin
/// @date    2026-09-11
///
/// BossRigComponent::ExecutePart が とどめ 1 回につき 1 枚を落とす
/// (Docs/part-break.md「柱 3 — 戦利品」)。板は消えない。
///
/// WHY 脚 1 本ではなく «板 1 枚» を落とすか:
///   もげた脚を床に残すのをやめたのは «6m 級が 4 本転がると走る場所が減る» から
///   (BossLegDebrisComponent の WHY)。落とすのを膝カバー相当の 1 枚に絞れば、
///   盤面は塞がないまま «自分が何本落としたか» が床に残り、遮蔽と足場としても働く。
///
/// WHY 飛んでくる板を «弾ける» 一撃にするか:
///   落とした枚数がそのまま弾ける機会の数になる ── 終盤ほど飛んでくる物が増え、
///   同時に終盤ほど崩しが速く溜まる。圧と手応えが同じ 1 つの仕掛けから出る。
///   Parryable で撃てば Just 窓・連続弾き・土壇場・閃光まで既存の経路が丸ごと乗る。
///
/// WHY 剛体を捨てて transform を直に運ばないか:
///   浮いている間・飛んでいる間も床と壁に当たり続けてほしい。速度だけ書いて
///   重力の掛かり方 (SetGravityScale) を切り替えれば、落ちる・浮く・飛ぶの
///   3 つが 1 つの剛体のまま繋がり、最後はまた普通に床へ落ち着く。
#pragma once

#include <Engine/Scene/Components/ColliderComponent.hpp>
#include <Engine/Scene/Components/RigidBodyComponent.hpp>
#include <Engine/Scene/MeshBuilder.hpp>
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

class BossArmorPlateComponent : public Script {
    FBZZ_SCRIPT(BossArmorPlateComponent)

public:
    FBZZ_GROUP("対象")
    FBZZ_FIELD_TAG(playerTag, "Player", "プレイヤーのタグ")

    // WHY 2.2 x 1.6m か: 遮蔽として意味がある (立てば線が切れる) 一番小さい大きさ。
    //     これ以上大きいと «脚が転がっている» に戻り、盤面が読めなくなる。
    FBZZ_GROUP("板")
    FBZZ_FIELD_RANGE(float, plateWidth, 2.2f, "幅 [m]", 0.5f, 6.0f)
    FBZZ_FIELD_RANGE(float, plateDepth, 1.6f, "奥行き [m]", 0.5f, 6.0f)
    FBZZ_FIELD_RANGE(float, plateThickness, 0.22f, "厚み [m]", 0.05f, 1.0f)
    FBZZ_FIELD_RANGE(float, settleSpeed, 0.45f, "落ち着く速さ", 0.0f, 3.0f)
    FBZZ_TOOLTIP("これ未満の速さ [m/s] が続いたら «床に落ち着いた» ＝ 吸い上げの対象になる")
    FBZZ_FIELD_RANGE(float, settleSeconds, 0.35f, "落ち着くまで [s]", 0.0f, 3.0f)

    // 磁力パルスの予兆 (0.85 秒) の間に浮き上がる。予兆と同じ長さで上がり切るように置く。
    FBZZ_GROUP("引き寄せ")
    FBZZ_FIELD_RANGE(float, drawHeight, 3.4f, "浮く高さ [m]", 0.5f, 12.0f)
    FBZZ_TOOLTIP("ボスの周りのどの高さまで吸い上げられるか。プレイヤーの背 (2.5m) より"
                 "上に置く ─ 低いと «浮いた» が «滑っている» に見える")
    FBZZ_FIELD_RANGE(float, drawRadius, 5.5f, "寄る半径 [m]", 1.0f, 20.0f)
    FBZZ_TOOLTIP("ボスからこの距離まで引き寄せる。0 に近いと体へめり込む")
    FBZZ_FIELD_RANGE(float, drawGain, 3.6f, "引きの強さ", 0.5f, 20.0f)
    FBZZ_TOOLTIP("目標へ向かう速さの比例係数 [1/s]。上げると «吸われる» が鋭くなる")
    FBZZ_FIELD_RANGE(float, drawMaxSpeed, 12.0f, "引きの上限 [m/s]", 1.0f, 40.0f)
    FBZZ_TOOLTIP("遠くの板が «飛来する» ほど速く吸われないようにする上限。"
                 "比例のままだと 15m 先の板が 54 m/s で突っ込んでくる")
    FBZZ_FIELD_RANGE(float, drawSpin, 3.0f, "浮いている間の回転", 0.0f, 20.0f)
    FBZZ_FIELD_RANGE(float, drawTimeout, 3.0f, "吸い上げの上限 [s]", 0.5f, 10.0f)
    FBZZ_TOOLTIP("撃たれないまま浮いていられる長さ。パルスが途中で潰れても床へ戻る")

    FBZZ_GROUP("射出")
    FBZZ_FIELD_RANGE(float, launchSpeed, 18.0f, "速さ [m/s]", 3.0f, 60.0f)
    FBZZ_TOOLTIP("プレイヤーの走り (10 m/s) より速くする。遅いと «走って避ける» が"
                 "常に正解になり、弾く理由が消える")
    FBZZ_FIELD_RANGE_INT(int, damage, 1, "ダメージ", 0, 10)
    FBZZ_FIELD_RANGE(float, hitRadius, 1.6f, "当たり半径 [m]", 0.2f, 5.0f)
    FBZZ_FIELD_RANGE(float, launchLifetime, 2.6f, "飛べる長さ [s]", 0.3f, 10.0f)
    FBZZ_TOOLTIP("撃たれてから床へ戻るまで。外した板はここで落ちる")
    FBZZ_FIELD_RANGE(float, parryBounce, 0.45f, "弾かれた跳ね返り", 0.0f, 2.0f)
    FBZZ_TOOLTIP("弾かれたときに来た向きへ跳ね返る比。0 でその場に落ちる")

    FBZZ_FIELD_READ_ONLY(std::string, debugState, "Falling", "状態")

    /// 生成した側が呼ぶ。もげた勢いと、板に張るマテリアルを渡す。
    ///
    /// ⚠ ここで剛体へ書いてはいけない。呼ばれるのは `AddScript` の直後で、context は
    ///   まだ結ばれていない (BossLegDebrisComponent::Launch と同じ理由)。控えるだけ。
    void Setup(const Vector3& velocity, const Vector3& spin, const std::string& materialPath)
    {
        m_velocity = velocity;
        m_spin     = spin;
        m_material = materialPath;
    }

    /// 床に落ち着いていて、吸い上げの対象になれるか。
    [[nodiscard]] bool IsAvailable() const { return m_phase == Phase::Resting; }
    /// 今まさに浮いている (＝ 撃てる) か。
    [[nodiscard]] bool IsDrawn() const { return m_phase == Phase::Drawn; }

    /// 磁力で吸い上げ始める。BossAiComponent::BeginPulse が呼ぶ。
    void Draw(const Vector3& bossPosition);

    /// 浮いている板をプレイヤーへ撃ち出す。BossAiComponent::TickPulse が呼ぶ。
    void Launch(const Vector3& target);

    void OnStart() override;
    void OnUpdate() override;

private:
    /// 板 1 枚の一生。Resting へ何度でも戻ってくる ── 弾き返された板は資源に戻る。
    enum class Phase : int { Falling = 0, Resting, Drawn, Flying };

    void BuildMesh();
    /// 飛んでいる板がプレイヤーに届いたか。届いたら 1 回だけ返す。
    void ResolveHit();
    /// 重力を戻して落下へ帰す。
    void FallBack();

    /// 板を «押しのける物» にするか、すり抜ける物にするか。
    ///
    /// WHY 浮いている間と飛んでいる間はすり抜けさせるか: 吸い上げ中はボスの脚と胴の
    ///     すぐ横に居るので、押し合うと板もボスも小刻みに震える。飛んでいる間も、
    ///     当たりは距離で見ている (ResolveHit) ので剛体の接触は要らない。
    ///     **床に落ち着いている間だけ固い** ＝ そのときだけ遮蔽と足場になる。
    void SetSolid(bool solid) const
    {
        if (auto* box = scene.GetComponent<BoxColliderComponent>()) box->SetTrigger(!solid);
    }

    [[nodiscard]] float Speed() const { return physics.GetVelocity().Length(); }

    Phase       m_phase    = Phase::Falling;
    float       m_slowFor  = 0.0f;
    float       m_phaseAge = 0.0f;
    /// この射出で飛ばす長さ [秒]。Launch が «狙った所へ届く時間» から決める。
    float       m_flyTime  = 0.0f;
    bool        m_dealt    = false;
    Vector3     m_anchor;      ///< 吸い上げ中の目標点。Draw が決めて動かさない。
    Vector3     m_flyDir;
    Vector3     m_velocity;
    Vector3     m_spin;
    std::string m_material;
};

FBZZ_REFLECT(BossArmorPlateComponent)

inline void BossArmorPlateComponent::OnStart()
{
    if (!m_material.empty()) mesh.SetProceduralMaterial(m_material);
    BuildMesh();

    if (auto* rb = scene.GetComponent<RigidBodyComponent>())
        if (rb->rigidBody) {
            rb->rigidBody->SetVelocity(m_velocity);
            rb->rigidBody->SetAngularVelocity(m_spin);
        }
}

inline void BossArmorPlateComponent::BuildMesh()
{
    MeshBuilder mb;
    const float hw = std::max(plateWidth, 0.1f) * 0.5f;
    const float hd = std::max(plateDepth, 0.1f) * 0.5f;
    const float ht = std::max(plateThickness, 0.02f) * 0.5f;

    mb.AddBox(Vector3::ZERO, Vector3{ hw, ht, hd });
    // 縁のリブ 2 本。平たい箱のままだと «装甲板» ではなく «床に落ちた立方体» に見える。
    mb.AddBox(Vector3{ 0.0f, ht, -hd * 0.62f }, Vector3{ hw * 0.86f, ht * 0.6f, hd * 0.10f });
    mb.AddBox(Vector3{ 0.0f, ht,  hd * 0.62f }, Vector3{ hw * 0.86f, ht * 0.6f, hd * 0.10f });
    mb.RecalculateTangents();

    mesh.Apply(mb);
}

inline void BossArmorPlateComponent::Draw(const Vector3& bossPosition)
{
    if (m_phase != Phase::Resting) return;

    // 吸い寄せ先はボスの «横» 上空。真上へ集めると 3 枚が同じ点で重なって 1 枚に見える。
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
    // 吸われ始めたことは音でしか気付けない ── 板は足元ではなく背後にも落ちている。
    se::Play(audio, se::kEnvHazardWarn, 0.55f);
}

inline void BossArmorPlateComponent::Launch(const Vector3& target)
{
    if (m_phase != Phase::Drawn) return;

    Vector3 to = target - transform.worldPosition;
    // 板は «撃ち出す» ので、狙いは水平へ寄せる。真下へ叩き付けると回避も弾きも
    // 間に合わない角度になる。
    to.y *= 0.35f;
    const float reach = to.Length();
    m_flyDir = to.NormalizedOr(Vector3::FORWARD);

    // 狙った所を通り過ぎたら落とす。
    //
    // WHY 一定時間飛ばさないか: 板は飛んでいる間すり抜けるので、外した板は闘技場の
    //     壁も抜けて場外へ出る ── **弾いても外しても «板が 1 枚減る»** ことになり、
    //     盤面に置いた資源が戦っているうちに消える。届く時間だけ飛ばして床へ返す。
    m_flyTime  = std::min(reach / std::max(launchSpeed, 0.1f) + 0.35f,
                          std::max(launchLifetime, 0.3f));
    m_phase    = Phase::Flying;
    m_phaseAge = 0.0f;
    m_dealt    = false;
    debugState = "Flying";

    physics.SetVelocity(m_flyDir * std::max(launchSpeed, 0.1f));
    // 回転は進行方向まわりに。縦に回すと «板が回っている» が輪郭で読めなくなる。
    physics.SetAngularVelocity(m_flyDir * std::max(drawSpin, 0.0f) * 1.6f);
    se::Play(audio, se::kImpactWall, 0.7f);
}

inline void BossArmorPlateComponent::FallBack()
{
    m_phase    = Phase::Falling;
    m_phaseAge = 0.0f;
    m_slowFor  = 0.0f;
    debugState = "Falling";
    SetSolid(true);
    physics.SetGravityScale(1.0f);
}

inline void BossArmorPlateComponent::ResolveHit()
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

    // 押しは «板が来た向き»。飛んできた物に弾かれる形が正しい。
    const Vector3 source = at - m_flyDir;
    const PlayerHitResult result =
        combat->HitPlayer(player, std::max(damage, 0), &source, PlayerHitKind::Parryable);

    if (result == PlayerHitResult::Parried) {
        // 弾かれた板は資源に戻る。跳ね返して落とすだけで、崩し・閃光・止めは
        // すべて PlayerParryComponent 側が既に返している。
        physics.SetVelocity(-m_flyDir * std::max(launchSpeed, 0.1f) * Clamp01(parryBounce)
                            + Vector3::UP * 3.0f);
        FallBack();
        return;
    }

    if (auto* vfx = VfxManagerComponent::Instance())
        vfx->PlayImpact(at, BladeSide::None, 0.7f, false);
    se::Play(audio, se::kImpactMid, 0.8f);
    FallBack();
}

inline void BossArmorPlateComponent::OnUpdate()
{
    const float dt = std::max(Time::deltaTime, 0.0f);
    m_phaseAge += dt;

    switch (m_phase) {
    case Phase::Falling: {
        // 跳ねている途中の «頂点で一瞬遅くなる» は数えない (もげた脚と同じ扱い)。
        m_slowFor = Speed() < std::max(settleSpeed, 0.0f) ? m_slowFor + dt : 0.0f;
        if (m_slowFor >= std::max(settleSeconds, 0.0f)) {
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
        // 速さは毎フレーム押し直す。壁を擦ったときに減速したまま «漂う板» にしない。
        physics.SetVelocity(m_flyDir * std::max(launchSpeed, 0.1f));
        ResolveHit();
        if (m_phase == Phase::Flying && m_phaseAge >= m_flyTime) FallBack();
        break;
    }
    }
}

} // namespace sandbox
