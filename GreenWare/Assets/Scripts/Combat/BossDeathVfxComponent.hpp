/// @file    BossDeathVfxComponent.hpp
/// @brief   ボスが «自壊してから» 崩れるまでの撃破演出
/// @author  Hasegawa Jin
/// @date    2026-08-29
///
/// WHY 雑魚と同じ 1 発では足りないか:
///   雑魚は EnemyDeathVfxComponent の «体が粒になって立ち昇る» 1 段だけで読める。
///   倒れた瞬間に画面から消えても、盤面には次の敵がいるので «1 体片付いた» で足りる。
///   ボスは戦いそのものの終わりで、しかも全高 6.6m ある。同じ 1 段だと、大きい体が
///   何の前触れもなく消える «バグに見える終わり方» になる。終わりには «壊れていく
///   時間» が要る。
///
/// WHY 段を «自壊 → 決定打 → 崩壊» に分けるか:
///   撃破の確定 (ヒットストップ・集束) は倒した瞬間に返さないと、当てた手応えが
///   遅れる。一方で «消える» のは遅らせたい。この 2 つは両立しないので、
///   確定を BossAiComponent::AnnounceDeath が t=0 で返し、こちらは
///   «確定した後の 4 秒» だけを受け持つ。
///     0.0 秒        装甲のあちこちが順に爆ぜる (体の «あちこち» を順に見せる)
///     buildup 秒    コアが落ちる決定打。ここが一番大きい 1 発
///     以降          EnemyDeathVfxComponent のディゾルブが体を食っていく
///
/// WHY ディゾルブと粒をここが持たないか:
///   «体そのものが崩れる» 絵は雑魚とまったく同じ仕組みでよく、実装は
///   EnemyDeathVfxComponent が持っている。ボスに必要なのは «崩れ始めるまでの間» と
///   «その間に何が爆ぜるか» だけなので、あちらの Delay を伸ばして噛み合わせる
///   (シーンの Body Dissolve > Delay を buildupSeconds に合わせること)。
///   同じ絵を 2 つの実装で持つと、雑魚を直したときにボスだけ古いままになる。
///
/// WHY 画面演出をここが持つか (EnemyDeathVfxComponent は持たないのに):
///   あちらが持たないのは、配分を決めるのが ImpactFeedbackManagerComponent だから。
///   ボスの攻撃はどれも BossAiComponent が自分で配分している (あちらの Feedback 群)
///   ので、ボスに関しては «自分で配って良い» が既にこの盤面の規則になっている。
///   減衰の式は shock::NearnessTo を共有するので、揺れと振動が食い違うことはない。
#pragma once

#include <Engine/Scene/Script.hpp>
#include <Scripts/Combat/BossHitboxRigComponent.hpp>
#include <Scripts/Game/CameraShakeManagerComponent.hpp>
#include <Scripts/Game/RumbleManagerComponent.hpp>
#include <Scripts/Game/ScreenEffectManagerComponent.hpp>
#include <Scripts/Game/VfxManagerComponent.hpp>
#include <Scripts/Utils/BladeColors.hpp>
#include <Scripts/Utils/SeLibrary.hpp>
#include <Scripts/Utils/ShockFalloff.hpp>
#include <algorithm>
#include <cmath>
#include <iterator>
#include <string>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

// 爆ぜる順に並べたボーン名。前後・左右・上下を交互に跨ぐ並びにしてある。
//
// WHY 順番を固定するか: 抽選すると «同じ側で 3 連続» が普通に出て、大きい体の
//     反対側が最後まで無傷のまま残る。«どこもかしこも壊れている» を見せたいので、
//     跨ぐ順序そのものが演出になる。
inline constexpr const char* kBossBlastBones[] = {
    "Head", "Thigh_BL", "Ring", "Hock_FR", "Rear", "Thigh_FL", "Hock_BR", "Body",
};

class BossDeathVfxComponent : public Script {
    FBZZ_SCRIPT(BossDeathVfxComponent)

public:
    FBZZ_GROUP("対象")
    FBZZ_FIELD_TAG(playerTag, "Player", "プレイヤーのタグ")
    FBZZ_TOOLTIP("衝撃の減衰を測る相手。パッドを持っているのはプレイヤー本人")

    FBZZ_FIELD(bool, wingedDeath, false, "翼を順に起爆")

    FBZZ_GROUP("Self Destruct")
    FBZZ_FIELD_RANGE(float, buildupSeconds, 1.70f, "蓄積", 0.1f, 8.0f)
    FBZZ_TOOLTIP("決定打までの秒数。EnemyDeathVfxComponent の Body Dissolve > Delay と "
                 "同じ値にすること (崩れ始めが決定打に重なる)")
    FBZZ_FIELD_RANGE_INT(int, blastCount, 7, "爆発の数", 0, 32)
    FBZZ_TOOLTIP("決定打までに順に爆ぜる数。体の «あちこち» を見せるための連発")
    FBZZ_FIELD_RANGE(float, blastSpread, 0.9f, "爆発の拡がり", 0.0f, 6.0f)
    FBZZ_TOOLTIP("ボーンからどれだけ散らして出すか [m]。0 だと関節の芯で毎回光る")
    FBZZ_FIELD_RANGE(float, blastStrengthStart, 0.34f, "強度 (最初)", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, blastStrengthEnd, 0.72f, "強度 (最後)", 0.0f, 1.0f)
    FBZZ_TOOLTIP("最後の 1 発ほど強く。決定打へ向けて «加速していく» 読みを作る")

    FBZZ_GROUP("Core Detonation")
    FBZZ_FIELD_RANGE(float, climaxStrength, 1.0f, "強度", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, climaxLift, 0.6f, "浮き", -4.0f, 8.0f)
    FBZZ_TOOLTIP("コアの «上» どれだけで爆ぜるか [m]。0 でコアの芯そのもの")
    FBZZ_FIELD_RANGE(float, climaxSurgeSeconds, 0.55f, "サージ", 0.0f, 3.0f)
    FBZZ_TOOLTIP("画面の縁が灼ける時間。0 で縁の演出を出さない")
    FBZZ_FIELD_COLOR(climaxSurgeColor, (Vector4{ 0.62f, 1.00f, 0.78f, 1.00f }), "Surge Color")
    FBZZ_TOOLTIP("ディゾルブの縁と同じ色にすること。極性色 (赤 / 青) を使うと "
                 "«どちらかの極でやられた» と読める")

    FBZZ_GROUP("手応え")
    FBZZ_FIELD_RANGE(float, shakeRatio, 0.80f, "揺れの比率", 0.0f, 1.0f)
    FBZZ_TOOLTIP("カメラ揺れを振動の何割で出すか。BossAiComponent と同じ比にすること")
    FBZZ_FIELD_RANGE(float, feedbackRange, 30.0f, "減衰の範囲", 1.0f, 80.0f)
    FBZZ_FIELD_RANGE(float, feedbackNear, 4.0f, "全開になる距離", 0.0f, 20.0f)
    FBZZ_FIELD_RANGE(float, blastVolume, 0.85f, "爆発の音量", 0.0f, 2.0f)
    FBZZ_FIELD_RANGE(float, climaxVolume, 1.0f, "起爆の音量", 0.0f, 2.0f)

    FBZZ_GROUP("デバッグ")
    FBZZ_FIELD_READ_ONLY(std::string, debugStage, "Idle", "ステージ")
    FBZZ_FIELD_READ_ONLY(int, debugBlastsFired, 0, "発射した爆発")

    /// 撃破された瞬間に 1 度だけ呼ぶ。2 度目以降は無視する。
    void Begin();

    /// 決定打までの秒数。撃破側はこれ以上待ってから GameObject を畳むこと。
    /// WHY 決定打«まで»か: 決定打の後は EnemyDeathVfxComponent のディゾルブが引き継ぐ。
    ///     どちらが長いかは調整で入れ替わるので、待つ側は両方の最大を採ること。
    [[nodiscard]] float TotalSeconds() const { return std::max(buildupSeconds, 0.0f); }

    void OnUpdate() override;

private:
    /// 1 発ぶんの «見え» と «手触り»。減衰は 1 度だけ出して揺れと振動で共有する。
    void Blast(const Vector3& point, float strength01, float volume, const se::Bank& bank) const;
    /// index 番目の爆発を置く点。ボーンが引ければその実座標、無ければ胴体まわり。
    [[nodiscard]] Vector3 BlastPoint(int index) const;
    /// コアの位置。リグが無い構成では胴体の高さで代用する。
    [[nodiscard]] Vector3 CorePoint() const;
    /// 名前で子孫を引く。リグが持っていないボーン (Head / Rear など) 用。
    [[nodiscard]] static GameObject* FindInSubtree(GameObject& root, const char* name);

    [[nodiscard]] GameObject* Player() const { return scene.FindWithTag(playerTag); }

    float m_elapsed  = 0.0f;
    /// 次に出す爆発の番号。決定打は blastCount 番目として同じ数え方に載せる。
    int   m_next     = 0;
    bool  m_running  = false;
    bool  m_climaxed = false;
};

FBZZ_REFLECT(BossDeathVfxComponent)

// ── 実装 (inline) ─────────────────────────────────────────────────────────────

inline GameObject* BossDeathVfxComponent::FindInSubtree(GameObject& root, const char* name)
{
    return root.FindInSubtree(name);
}

inline Vector3 BossDeathVfxComponent::CorePoint() const
{
    if (const auto* rig = scene.GetScript<BossHitboxRigComponent>()) {
        if (GameObject* core = rig->CoreBone())
            return core->transform.worldPosition;
    }
    if (auto* self = scene.Self())
        if (auto* core = FindInSubtree(*self, "SOCKET_Core")) return core->transform.worldPosition;
    // リグが無い構成へのフォールバック。胴体の高さだけは外さないようにする。
    Vector3 point = transform.worldPosition;
    point.y += 3.3f;
    return point;
}

inline Vector3 BossDeathVfxComponent::BlastPoint(int index) const
{
    constexpr int kBoneCount = static_cast<int>(std::size(kBossBlastBones));

    Vector3 point = transform.worldPosition;
    point.y += 3.3f;
    if (GameObject* self = scene.Self()) {
        constexpr const char* wings[] = { "Wing_L_Lower", "Wing_R_Lower", "Wing_L_Middle",
                                          "Wing_R_Middle", "Wing_L_Upper", "Wing_R_Upper", "Body" };
        const char* boneName = wingedDeath ? wings[index % 7] : kBossBlastBones[index % kBoneCount];
        if (GameObject* bone = FindInSubtree(*self, boneName))
            point = bone->transform.worldPosition;
    }

    // 黄金角で散らす。番号から一意に決まるので、同じ撃破は何度見ても同じ位置で爆ぜる
    // (乱数だと «さっきの方が良かった» が起きて、調整の手がかりにならない)。
    const float angle  = static_cast<float>(index) * 2.39996323f;
    const float radius = std::max(blastSpread, 0.0f);
    point.x += std::cos(angle) * radius;
    point.z += std::sin(angle) * radius;
    // 上下にも散らす。水平だけだと «腰の高さの輪» に並んで見える。
    point.y += std::sin(angle * 1.7f) * radius * 0.6f;
    return point;
}

inline void BossDeathVfxComponent::Blast(const Vector3& point, float strength01,
                                         float volume, const se::Bank& bank) const
{
    const float strength = Clamp01(strength01);
    if (strength <= 0.0f) return;

    if (auto* vfx = VfxManagerComponent::Instance())
        vfx->PlayImpact(point, BladeSide::None, strength, /*againstAnchor=*/false);
    se::PlayAt(audio, bank, point, volume);

    // 近さは 1 度だけ出す。揺れと振動が別々に距離を測ると、画面は静かなのに手だけ
    // 震える距離ができて «どこで起きたか» の答えが 2 つになる。
    const float nearness = shock::NearnessTo(Player(), point,
                                             std::max(feedbackRange, 1.0f),
                                             std::max(feedbackNear, 0.0f));
    if (nearness <= 0.0f) return;

    const float weight = strength * nearness;
    if (auto* pad = RumbleManagerComponent::Instance()) pad->Rumble(weight);
    if (auto* shake = CameraShakeManagerComponent::Instance())
        shake->Shake(weight * Clamp01(shakeRatio));
}

inline void BossDeathVfxComponent::Begin()
{
    if (m_running) return;
    m_running  = true;
    m_climaxed = false;
    m_elapsed  = 0.0f;
    m_next     = 0;
    debugBlastsFired = 0;
    debugStage = "Self Destruct";

    // 音源はこの位置で鳴らす。爆ぜる点はどれも体の «どこか» なので方向が読める必要がある。
    se::EnsureSource(scene, "SE", 1.0f);
}

inline void BossDeathVfxComponent::OnUpdate()
{
    if (!m_running) return;

    // WHY 実時間ではなくスケール時間か: 決定打はディゾルブが始まる瞬間と重なっていないと
    //     «崩れ始めたのに何も起きない» が出る。あちら (EnemyDeathVfxComponent) が
    //     Time::deltaTime で数えている以上、こちらだけ実時間で数えるとヒットストップの
    //     ぶんだけ 2 つがずれる。同じ出来事を刻む時計は 1 つに揃える。
    m_elapsed += Time::deltaTime;

    const int   count   = std::max(blastCount, 0);
    const float buildup = std::max(buildupSeconds, 0.0f);

    // 連発。決定打の «直前» まで等間隔で置く。最後の 1 発が決定打と重なると、
    // 一番大きい 1 発が小さい爆発に埋もれる。
    while (m_next < count) {
        const float at = buildup * (static_cast<float>(m_next) + 0.5f)
                       / static_cast<float>(count + 1);
        if (m_elapsed < at) break;

        const float t = count > 1
            ? static_cast<float>(m_next) / static_cast<float>(count - 1) : 1.0f;
        Blast(BlastPoint(m_next), blastStrengthStart + (blastStrengthEnd - blastStrengthStart) * t,
              blastVolume, se::kImpactHeavy);
        ++m_next;
        debugBlastsFired = m_next;
    }

    if (m_climaxed || m_elapsed < buildup) return;

    // 決定打。ここでコアが落ち、EnemyDeathVfxComponent のディゾルブが体を食い始める。
    m_climaxed = true;
    debugStage = "Core Detonation";

    Vector3 core = CorePoint();
    core.y += climaxLift;
    Blast(core, climaxStrength, climaxVolume, se::kImpactFinal);
    ++debugBlastsFired;

    // WHY 白いフラッシュではなくサージか: 画面を塗る白は «こちらが受けた» を表す語で、
    //     倒した瞬間に出すと被弾と読み違える (BossAiComponent の集束と同じ判断)。
    //     縁だけを灼く色付きの一撃なら、盤面の中央で起きたことを隠さずに済む。
    if (climaxSurgeSeconds > 0.0f) {
        if (auto* screen = ScreenEffectManagerComponent::Instance())
            screen->Surge(climaxSurgeColor, Clamp01(climaxStrength), climaxSurgeSeconds);
    }
}

} // namespace sandbox
