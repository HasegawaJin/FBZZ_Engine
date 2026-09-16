/// @file    BossStepDustComponent.hpp
/// @brief   ボスの脚が床へ着くたびに立つ土煙
/// @author  Hasegawa Jin
/// @date    2026-08-31
///
/// WHY 歩いているだけの間にも演出を足すか:
///   全高 6.6m の 4 脚が、床に何の跡も残さず滑るように近づいてくる。歩幅も接地も
///   モーションには入っているのに、盤面がそれを受け取っていないので «重さの無い置物が
///   寄ってくる» に見える。踏むたび床が応えて初めて、間合いを詰められること自体が
///   圧になる。
///
/// WHY 足音 (BossAudioComponent) の拍に合わせないか:
///   あちらは «歩調» を速さから逆算した周期で鳴らしていて、4 本のどの脚が着いたかは
///   知らない。土煙は場所を持つ演出なので、周期から出すと «脚は上がっているのに
///   その足元から煙が出る» が普通に起きる。骨の高さを直接見れば、どのクリップを
///   どの速さで再生していても «着いた脚の下» で必ず鳴る。
///
/// WHY しきい値を数値で置かず、足ごとの振れ幅から出すか:
///   «床から何 m 上» で接地を決めると、Walk_Crawl と Charge_Run で脚の上がる高さが
///   違うぶん、片方でしか鳴らない値になる。しかも脚を折られた後は引きずるので
///   振れ幅そのものが小さくなる。各脚の «最近の最高と最低» を追い掛けて、その間の
///   割合で判定すれば、どのクリップでも歩幅の大小に関わらず同じ位置で鳴る。
///
/// WHY 振れ幅が小さい脚では鳴らさないか:
///   引きずっている脚・持ち上げていない脚は «踏んだ» ことになっていない。割合だけで
///   判定すると、1cm の揺れでも «上がって下りた» と読めてしまい、止まっている
///   ボスの足元から煙が湧き続ける。
#pragma once

#include <Engine/Scene/Script.hpp>
#include <Scripts/Combat/BossAnimParams.hpp>
#include <Scripts/Combat/BossHitboxRigComponent.hpp>
#include <Scripts/Game/VfxManagerComponent.hpp>
#include <algorithm>
#include <cmath>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

/// 足ごとの «最近の高さの幅» をどれだけの速さで縮めるか [m/s]。
///
/// WHY 縮めるか: 一度でも大きく振れると、その最大値を覚えたままの脚は以後
///     «その高さまで上げないと着地と認めない» になる。速さが落ちて歩幅が
///     小さくなったときに 1 歩も鳴らなくなるので、幅は常に現在値へ寄せ続ける。
///
/// WHY 速く縮めないか: 脚が頂点から床へ降りるまでの 0.2 秒ほどの間にも幅は縮む。
///     速いと «測った歩幅» が実際より小さくなり、そもそも上げ幅の小さいクリップで
///     Min Lift を割って鳴らなくなる。
inline constexpr float kFootEnvelopeShrink = 0.35f;

/// 胴体が上下に動いているとみなす速さ [m/s]。これを超えている間は跳躍中とみなす。
///
/// WHY Animator の接地フラグだけに頼らないか: BossAi は着地の landContactTime «前» に
///     接地を立てる (潰れ込みを接地へ合わせるため)。その間はまだ空中に居るので、
///     フラグだけを見ると降りてくる脚が空中で土煙を出す。
inline constexpr float kAirborneRiseSpeed = 1.0f;

class BossStepDustComponent : public Script {
    FBZZ_SCRIPT(BossStepDustComponent)

public:
    FBZZ_GROUP("1 フレーム進める")
    FBZZ_FIELD_RANGE(float, minSpeed, 0.35f, "Dust Above (m/s)", 0.0f, 5.0f)
    FBZZ_TOOLTIP("これより遅ければ出さない。止まっているボスの脚も呼吸で微かに動くので、"
                 "0 にすると立っているだけで足元から煙が湧く")
    FBZZ_FIELD_RANGE(float, fullSpeed, 4.0f, "Full Dust At (m/s)", 0.5f, 20.0f)
    FBZZ_TOOLTIP("土煙が最大になる速さ [m/s]。巡回 2.4 / 突進 8.0 の間に置くと、"
                 "突進で明らかに派手になりつつ、歩きでも «出ていない» にはならない")
    FBZZ_FIELD_RANGE(float, minLift, 0.10f, "Min Lift", 0.01f, 1.5f)
    FBZZ_TOOLTIP("脚がこれだけ上下していなければ «踏んだ» と認めない [m]。"
                 "引きずっている脚から煙が出るのを止める")
    FBZZ_FIELD_RANGE(float, footCooldown, 0.18f, "Foot Cooldown", 0.0f, 2.0f)
    FBZZ_TOOLTIP("同じ脚が続けて鳴るまでの最短間隔 [秒]。接地の前後で骨が細かく震える"
                 "モーションだと、1 歩が 2 発に割れることがある")

    FBZZ_GROUP("見た目")
    FBZZ_FIELD_RANGE(float, dustScale, 0.85f, "スケール", 0.1f, 4.0f)
    FBZZ_TOOLTIP("VfxManager の Ground Dust に掛ける倍率。衝撃波の 1 発より小さく置く ─ "
                 "歩幅ごとに «叩きつけた» と同じ煙が出ると、着地との区別が消える")
    FBZZ_FIELD_RANGE(float, groundOffset, 0.0f, "接地のオフセット", -1.0f, 1.0f)
    FBZZ_TOOLTIP("胴体の足元から見た床の高さ [m]。煙が床へ潜る / 浮く場合だけ触る")

    FBZZ_GROUP("デバッグ")
    FBZZ_FIELD_READ_ONLY(int, debugSteps, 0, "Steps")
    FBZZ_FIELD_READ_ONLY(float, debugLift, 0.0f, "Widest Lift")
    FBZZ_FIELD(bool, drawDebugFeet, false, "Draw Feet")

    void OnStart()  override;
    void OnUpdate() override;

private:
    /// 脚 1 本の «最近どれだけ上下しているか»。
    struct Foot {
        float low   = 0.0f;
        float high  = 0.0f;
        /// 一度上がりきったか。下がるだけでは «踏んだ» にならない。
        bool  armed = false;
        /// 次に鳴らせるまでの残り [秒]。
        float wait  = 0.0f;
        bool  known = false;   ///< 1 フレーム目は幅を測れないので鳴らさない
    };

    /// 踏んだ脚の下へ 1 発。
    void Fire(const Vector3& footPoint, float rootY, float speed, const Vector3& moveDirection);

    Foot    m_feet[4];
    Vector3 m_lastPosition = Vector3::ZERO;
    bool    m_hasLastPose  = false;
    /// 均した水平速度 [m/s]。1 フレームの差分は跳ねるので、そのまま強さに使わない。
    float   m_speed = 0.0f;
    bool    m_warnedNoRig = false;
};

FBZZ_REFLECT(BossStepDustComponent)


inline void BossStepDustComponent::OnStart()
{
    for (Foot& foot : m_feet) foot = {};
    m_hasLastPose = false;
    m_speed       = 0.0f;
    m_warnedNoRig = false;
    debugSteps    = 0;
    debugLift     = 0.0f;

    if (!scene.GetScript<BossHitboxRigComponent>()) {
        m_warnedNoRig = true;
        debug.LogError("BossStepDustComponent requires a BossHitboxRigComponent on the same "
                       "object (it is the only thing that knows where the foot bones are).");
    }
}

inline void BossStepDustComponent::Fire(const Vector3& footPoint, float rootY, float speed,
                                        const Vector3& moveDirection)
{
    auto* vfx = VfxManagerComponent::Instance();
    if (!vfx) return;

    // 土煙は踏んだ脚の «後ろ» へ流れる。止まりかけで向きが決まらないときは、
    // 胴体から見て脚の外側へ逃がす ─ 体の下へ吹き込むと煙が全部隠れる。
    Vector3 flow{ -moveDirection.x, 0.0f, -moveDirection.z };
    if (flow.LengthSq() < EPSILON) {
        flow = { footPoint.x - transform.worldPosition.x, 0.0f,
                 footPoint.z - transform.worldPosition.z };
    }

    const Vector3 point{ footPoint.x, rootY + groundOffset, footPoint.z };
    vfx->PlayGroundDust(point, flow, Clamp01(speed / std::max(fullSpeed, 0.1f)),
                        std::max(dustScale, 0.1f));

    ++debugSteps;
}

inline void BossStepDustComponent::OnUpdate()
{
    const float dt = std::max(Time::deltaTime, 0.0f);
    if (dt <= 0.0f) return;

    for (Foot& foot : m_feet) foot.wait = std::max(0.0f, foot.wait - dt);

    const auto* rig = scene.GetScript<BossHitboxRigComponent>();
    if (!rig) {
        if (!m_warnedNoRig) {
            m_warnedNoRig = true;
            debug.LogError("BossStepDustComponent found no BossHitboxRigComponent. "
                           "The boss walks without raising dust.");
        }
        return;
    }

    const Vector3 position = transform.worldPosition;
    if (!m_hasLastPose) {
        m_lastPosition = position;
        m_hasLastPose  = true;
        return;
    }

    const Vector3 delta{ position.x - m_lastPosition.x,
                         position.y - m_lastPosition.y,
                         position.z - m_lastPosition.z };
    const float rise = std::fabs(delta.y) / dt;
    const Vector3 move{ delta.x, 0.0f, delta.z };
    const float   speed = move.Length() / dt;
    m_lastPosition = position;

    // 1 フレームの差分は接地の押し戻しで跳ねる。均さないと、歩いている最中に
    // 強さが毎フレーム上下して煙の大きさがちらつく。
    const float alpha = 1.0f - std::exp(-8.0f * dt);
    m_speed += (speed - m_speed) * std::clamp(alpha, 0.0f, 1.0f);

    const Vector3 direction = move.NormalizedOr(Vector3::ZERO);
    const bool    walking   = m_speed >= std::max(minSpeed, 0.0f);
    // 跳躍中は «降りてくる脚» が空中で下がり続ける。着地そのものは衝撃波が
    // 受け持っているので、ここは床の上に居るあいだだけ働く。
    const bool    grounded  = rise < kAirborneRiseSpeed;

    float widest = 0.0f;
    for (int i = 0; i < 4; ++i) {
        Foot& foot = m_feet[i];

        GameObject* bone = rig->FootBone(static_cast<BossLeg>(i));
        if (!bone) continue;

        // 胴体の足元から見た «脚の高さ»。ワールドの Y をそのまま使うと、坂を上がる
        // だけで 4 本まとめて «上がった» ことになる。
        const float height = bone->transform.worldPosition.y - position.y;
        if (!foot.known) {
            foot.low   = height;
            foot.high  = height;
            foot.known = true;
            continue;
        }

        foot.low  = std::min(height, foot.low  + kFootEnvelopeShrink * dt);
        foot.high = std::max(height, foot.high - kFootEnvelopeShrink * dt);

        const float span = foot.high - foot.low;
        widest = std::max(widest, span);

        if (drawDebugFeet) {
            const Vector3 base{ bone->transform.worldPosition.x, position.y + groundOffset,
                                bone->transform.worldPosition.z };
            debug.DrawLine(base, bone->transform.worldPosition,
                           foot.armed ? Vector4{ 0.3f, 1.0f, 0.4f, 1.0f }
                                      : Vector4{ 1.0f, 0.6f, 0.2f, 1.0f });
        }

        if (span < std::max(minLift, 0.01f)) { foot.armed = false; continue; }

        // 上げきった所で構え、下がりきる手前で撃つ。«下がりきってから» にすると、
        // 足が床に着いた 2〜3 フレーム後に煙が出て、踏んだ音とずれる。
        if (height >= foot.low + span * 0.62f) foot.armed = true;
        if (!foot.armed || height > foot.low + span * 0.28f) continue;

        foot.armed = false;
        if (!walking || !grounded || foot.wait > 0.0f) continue;

        foot.wait = std::max(footCooldown, 0.0f);
        Fire(bone->transform.worldPosition, position.y, m_speed, direction);
    }

    debugLift = widest;
}

} // namespace sandbox
