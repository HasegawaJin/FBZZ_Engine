/// @file    BossDeathVfxComponent.hpp
/// @brief   ボスが自壊してから崩れるまでの撃破演出。
/// @author  Hasegawa Jin
/// @date    2026-08-29
///
/// @note 段は自壊 (装甲を順に起爆) → 決定打 (コア) → 崩壊の 3 段。撃破確定は
///       BossAiComponent::AnnounceDeath が t=0 で返すので、ここは確定後の演出のみ持つ。
///       崩壊 (ディゾルブ) は EnemyDeathVfxComponent と共有実装のため、buildupSeconds を
///       そちらの Body Dissolve > Delay に揃えること。画面演出の配分は BossAiComponent が
///       自前で行う盤面のため、ここでも自前で配ってよい。
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

/// @brief 爆ぜる順に並べたボーン名。前後・左右・上下を交互に跨ぐ並び。
/// @note 固定順にする理由: 抽選だと同じ側が連続し、反対側が無傷のまま残るため。
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

    /// @brief 決定打までの秒数。撃破側はこれ以上待ってから GameObject を畳むこと。
    /// @note 決定打の後はディゾルブ (EnemyDeathVfxComponent) が引き継ぐ。長い方を採ること。
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
    /// @note リグが無い構成へのフォールバック。胴体の高さだけは外さないようにする。
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

    /// @note 黄金角で散らす。番号から一意に決まるので、同じ撃破は何度見ても同じ位置で爆ぜる
    ///       (乱数だと «さっきの方が良かった» が起きて、調整の手がかりにならない)。
    const float angle  = static_cast<float>(index) * 2.39996323f;
    const float radius = std::max(blastSpread, 0.0f);
    point.x += std::cos(angle) * radius;
    point.z += std::sin(angle) * radius;
    /// @note 上下にも散らす。水平だけだと «腰の高さの輪» に並んで見える。
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

    /// @note 近さは 1 度だけ出す。揺れと振動が別々に距離を測ると、画面は静かなのに手だけ
    ///       震える距離ができて «どこで起きたか» の答えが 2 つになる。
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

    /// @note 音源はこの位置で鳴らす。爆ぜる点はどれも体の «どこか» なので方向が読める必要がある。
    se::EnsureSource(scene, "SE", 1.0f);
}

inline void BossDeathVfxComponent::OnUpdate()
{
    if (!m_running) return;

    /// @note スケール時間で数える: 実時間だとヒットストップぶんディゾルブ (実時間非依存) とずれる。
    m_elapsed += Time::deltaTime;

    const int   count   = std::max(blastCount, 0);
    const float buildup = std::max(buildupSeconds, 0.0f);

    /// @note 連発。決定打の «直前» まで等間隔で置く。最後の 1 発が決定打と重なると、
    ///       一番大きい 1 発が小さい爆発に埋もれる。
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

    /// @note 決定打。ここでコアが落ち、EnemyDeathVfxComponent のディゾルブが体を食い始める。
    m_climaxed = true;
    debugStage = "Core Detonation";

    Vector3 core = CorePoint();
    core.y += climaxLift;
    Blast(core, climaxStrength, climaxVolume, se::kImpactFinal);
    ++debugBlastsFired;

    /// @note 白フラッシュでなくサージにする: 白は被弾表現のため、倒した瞬間に使うと誤読する。
    if (climaxSurgeSeconds > 0.0f) {
        if (auto* screen = ScreenEffectManagerComponent::Instance())
            screen->Surge(climaxSurgeColor, Clamp01(climaxStrength), climaxSurgeSeconds);
    }
}

} // namespace sandbox
