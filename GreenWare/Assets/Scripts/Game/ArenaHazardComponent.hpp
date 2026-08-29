/// @file    ArenaHazardComponent.hpp
/// @brief   アリーナ中央の床ハザード。押し込みの «到達点» になる
/// @author  Hasegawa Jin
/// @date    2026-08-28
///
/// WHY 要るか (Docs/arena.md「中央ハザード」):
///   反発は位置を変えるだけでダメージを持たない。押した先に «倒せる場所» が無いと、
///   押しは «盤面を整える» だけの手に留まり、明確な使い道を持てない。
///   ハザードは «押せば倒せる» を成立させる 1 点で、同時に盤面の中央を削るノブでもある。
///
/// WHY 敵は即撃破・プレイヤーは被弾か:
///   同じ場所が両者にとって同じ意味だと «近づかない» が唯一の正解になり、
///   盤面の中央がただ消える。敵にとっては死で、プレイヤーにとっては痛いが通れる、
///   という非対称にして初めて «自分は縁に立ち、敵だけを落とす» という手が生まれる。
///
/// WHY 撃破を CombatManagerComponent へ通すか:
///   撃破数の集計と撃破コアの生成は «倒れた» という 1 つの出来事に紐づいている。
///   ここで直接 HP を 0 にすると、ハザードで倒した敵だけコアを残さない。
#pragma once

#include <Engine/Scene/Script.hpp>
#include <Scripts/Combat/EnemyHealthComponent.hpp>
#include <Scripts/Game/CombatManagerComponent.hpp>
#include <Scripts/Utils/SeLibrary.hpp>
#include <algorithm>
#include <cmath>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

class ArenaHazardComponent : public Script {
    FBZZ_SCRIPT(ArenaHazardComponent)

public:
    FBZZ_GROUP("Area")
    // WHY 円で持つか: アリーナが円形なので、中央を «削る» 形も円が素直に読める。
    //     コライダーで取ると、その形をシーンで編集できてしまい、
    //     «どこまでが危険か» の答えがシーンとコードの 2 つになる。
    FBZZ_FIELD_RANGE(float, radius, 6.0f, "Radius", 0.0f, 30.0f)
    FBZZ_TOOLTIP("このスクリプトの位置を中心とした危険半径 [m]")
    FBZZ_FIELD_RANGE(float, height, 3.0f, "Height", 0.0f, 20.0f)
    FBZZ_TOOLTIP("この高さまでを «中に居る» と見なす。跳び越えられる高さにしない")

    FBZZ_GROUP("State")
    // Wave3 以降で作動させる。作動前は無害な床として見えているのが正しい。
    FBZZ_FIELD(bool, activeOnStart, false, "Active On Start")
    FBZZ_TOOLTIP("開始時から作動させるか。Wave 進行から SetActiveHazard で切り替える")
    FBZZ_FIELD_RANGE(float, warmupSeconds, 1.2f, "Warmup", 0.0f, 6.0f)
    FBZZ_TOOLTIP("作動を宣言してから実際に効き始めるまで。予告なしに殺さないための間")

    FBZZ_GROUP("Damage")
    FBZZ_FIELD_RANGE_INT(int, enemyDamage, 9999, "Enemy Damage", 1, 99999)
    FBZZ_TOOLTIP("敵へ 1 回で入れる量。既定は即撃破。«落ちたら死ぬ» が読めないと、"
                 "押し込むという手を覚えられない")
    FBZZ_FIELD_RANGE_INT(int, playerDamage, 8, "Player Damage", 0, 999)
    FBZZ_TOOLTIP("プレイヤーへ刻む量。0 で無害。即死にはしない (縁を使う遊びが消える)")
    FBZZ_FIELD_RANGE(float, playerTickSeconds, 0.6f, "Player Tick", 0.05f, 5.0f)
    FBZZ_TOOLTIP("プレイヤーへ刻む間隔 [秒]")
    FBZZ_FIELD_TAG(playerTag, "Player", "Player Tag")

    FBZZ_GROUP("Debug")
    FBZZ_FIELD_READ_ONLY(int, debugPushKills, 0, "Push Kills")
    FBZZ_TOOLTIP("ハザードで落とした数。ランク評価の «押し込み撃破» はここを読む")
    FBZZ_FIELD_READ_ONLY(bool, debugArmed, false, "Armed")
    FBZZ_FIELD(bool, drawArea, true, "Draw Area")

    /// 置き場所に依存しない窓口。Wave 進行とランク評価の両方が引く。
    [[nodiscard]] static ArenaHazardComponent* Instance() { return s_instance; }

    /// 作動を切り替える。Wave 進行が呼ぶ。
    void SetHazardActive(bool active);
    [[nodiscard]] bool IsArmed() const { return m_active && m_warmup <= 0.0f; }
    /// ハザードで落とした数。ランク評価の «押し込み撃破» の一部。
    [[nodiscard]] int PushKills() const { return m_pushKills; }

    void OnStart()  override;
    void OnUpdate() override;
    void OnDestroy() override { if (s_instance == this) s_instance = nullptr; }
    void OnDrawGizmos() override;

private:
    static inline ArenaHazardComponent* s_instance = nullptr;

    /// point がハザードの中か。高さも見る (跳び越えている最中は «中» ではない)。
    [[nodiscard]] bool Contains(const Vector3& point) const;

    bool  m_active    = false;
    float m_warmup    = 0.0f;
    float m_playerTick = 0.0f;
    int   m_pushKills  = 0;
};

FBZZ_REFLECT(ArenaHazardComponent)

// ── 実装 (inline) ─────────────────────────────────────────────────────────────

inline void ArenaHazardComponent::OnStart()
{
    s_instance = this;

    m_active     = activeOnStart;
    m_warmup     = activeOnStart ? 0.0f : std::max(warmupSeconds, 0.0f);
    m_playerTick = 0.0f;
    m_pushKills  = 0;
    debugPushKills = 0;
    debugArmed     = IsArmed();

    // 落ちた «場所» で鳴らす。画面の別の場所を見ていても、何が起きたか方向で分かる。
    se::EnsureSource(scene, "SE", 1.0f);
}

inline void ArenaHazardComponent::SetHazardActive(bool active)
{
    if (m_active == active) return;
    m_active = active;
    // 予告の間は «作動を宣言したが、まだ効かない» 状態。切るときは即座でよい。
    m_warmup = active ? std::max(warmupSeconds, 0.0f) : 0.0f;
}

inline bool ArenaHazardComponent::Contains(const Vector3& point) const
{
    const Vector3 center = transform.worldPosition;
    const float dy = point.y - center.y;
    if (dy < -1.0f || dy > std::max(height, 0.0f)) return false;

    const float dx = point.x - center.x;
    const float dz = point.z - center.z;
    return dx * dx + dz * dz <= std::max(radius, 0.0f) * std::max(radius, 0.0f);
}

inline void ArenaHazardComponent::OnUpdate()
{
    if (m_warmup > 0.0f) m_warmup = std::max(0.0f, m_warmup - Time::deltaTime);
    debugArmed = IsArmed();
    if (!IsArmed() || radius <= 0.0f) return;

    auto* combat = CombatManagerComponent::Instance();

    // 敵。落ちた時点で終わり。«押し込めば倒せる» が読めるよう、削らずに落とす。
    for (GameObject* object : scene.FindObjectsOfType<EnemyHealthComponent>()) {
        if (!object || !object->activeInHierarchy()) continue;
        if (!Contains(object->transform.worldPosition)) continue;

        const auto* health = scene.GetScript<EnemyHealthComponent>(object);
        if (!health || !health->IsAlive()) continue;

        // WHY マネージャー越しに殺すか: 撃破数もコアもあちらが持っている。
        //     ここで HP を直接 0 にすると、ハザードで倒した分だけ的が残らない。
        if (!combat) continue;
        if (!combat->DamageEnemyDirect(object, std::max(enemyDamage, 1))) continue;

        // ランク評価の «押し込み撃破» は 1 箇所で数える。ここで自前に数えるだけだと、
        // 壁への押し込みと合算されずに評価が半分になる。
        combat->AddPushKill();
        ++m_pushKills;
        debugPushKills = m_pushKills;
        se::PlayAt(audio, se::kImpactHeavy, object->transform.worldPosition);
    }

    // プレイヤー。痛いが通れる。即死にすると «縁に立って敵だけ落とす» が消える。
    m_playerTick = std::max(0.0f, m_playerTick - Time::deltaTime);
    if (playerDamage <= 0 || m_playerTick > 0.0f) return;

    GameObject* player = scene.FindWithTag(playerTag);
    if (!player || !Contains(player->transform.worldPosition)) return;

    m_playerTick = std::max(playerTickSeconds, 0.05f);
    if (combat) (void)combat->DamagePlayer(player, playerDamage);
}

inline void ArenaHazardComponent::OnDrawGizmos()
{
    if (!drawArea) return;
    // 作動しているかを色で分ける。切ってあるのに «危険» に見えると、
    // 配置を確かめている最中に判断を誤る。
    const Vector4 color = IsArmed() ? Vector4{ 1.0f, 0.35f, 0.15f, 1.0f }
                                    : Vector4{ 0.45f, 0.45f, 0.50f, 1.0f };
    debug.DrawSphere(transform.worldPosition, std::max(radius, 0.0f), color);
}

} // namespace sandbox
