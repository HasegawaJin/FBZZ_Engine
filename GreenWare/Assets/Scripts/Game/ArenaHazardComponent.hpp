/// @file    ArenaHazardComponent.hpp
/// @brief   アリーナ中央の床ハザード。押し込みの «到達点» になる
/// @author  Hasegawa Jin
/// @date    2026-08-28
///
/// @note 反発は位置を変えるだけでダメージを持たない。押した先に倒せる場所が無いと盤面整理の手にしかならないため、中央にハザードを置き「押せば倒せる」を成立させる (Docs/arena.md)。
/// @note 敵は即撃破・プレイヤーは被弾の非対称にする。同じ意味だと「近づかない」が唯一の正解になり中央が死地として機能しなくなる。
/// @note 撃破は `CombatManagerComponent` 経由。ここで直接 HP を 0 にすると撃破数の集計と撃破コアの生成から漏れる。
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
    /// @note コライダーでなく円で持つ。コライダーだとシーン側で形を編集でき、危険範囲の答えがシーンとコードの 2 つになる。
    FBZZ_FIELD_RANGE(float, radius, 6.0f, "半径", 0.0f, 30.0f)
    FBZZ_TOOLTIP("このスクリプトの位置を中心とした危険半径 [m]")
    FBZZ_FIELD_RANGE(float, height, 3.0f, "高さ", 0.0f, 20.0f)
    FBZZ_TOOLTIP("この高さまでを «中に居る» と見なす。跳び越えられる高さにしない")

    FBZZ_GROUP("状態")
    /// Wave3 以降で作動させる。作動前は無害な床として見えているのが正しい。
    FBZZ_FIELD(bool, activeOnStart, false, "Active On Start")
    FBZZ_TOOLTIP("開始時から作動させるか。Wave 進行から SetActiveHazard で切り替える")
    FBZZ_FIELD_RANGE(float, warmupSeconds, 1.2f, "Warmup", 0.0f, 6.0f)
    FBZZ_TOOLTIP("作動を宣言してから実際に効き始めるまで。予告なしに殺さないための間")

    FBZZ_GROUP("ダメージ")
    FBZZ_FIELD_RANGE_INT(int, enemyDamage, 9999, "Enemy Damage", 1, 99999)
    FBZZ_TOOLTIP("敵へ 1 回で入れる量。既定は即撃破。«落ちたら死ぬ» が読めないと、"
                 "押し込むという手を覚えられない")
    FBZZ_FIELD_RANGE_INT(int, playerDamage, 8, "Player Damage", 0, 999)
    FBZZ_TOOLTIP("プレイヤーへ刻む量。0 で無害。即死にはしない (縁を使う遊びが消える)")
    FBZZ_FIELD_RANGE(float, playerTickSeconds, 0.6f, "Player Tick", 0.05f, 5.0f)
    FBZZ_FIELD_RANGE(float, hazardVolume, 0.85f, "ハザードの音量", 0.0f, 2.0f)
    FBZZ_TOOLTIP("踏んでいる間の被弾音。Player Tick ごとに 1 回鳴るので、刻みを短くするとうるさくなる")
    FBZZ_TOOLTIP("プレイヤーへ刻む間隔 [秒]")
    FBZZ_FIELD_TAG(playerTag, "Player", "プレイヤーのタグ")

    FBZZ_GROUP("デバッグ")
    FBZZ_FIELD_READ_ONLY(int, debugPushKills, 0, "押しで倒す")
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


inline void ArenaHazardComponent::OnStart()
{
    s_instance = this;

    m_active     = activeOnStart;
    m_warmup     = activeOnStart ? 0.0f : std::max(warmupSeconds, 0.0f);
    m_playerTick = 0.0f;
    m_pushKills  = 0;
    debugPushKills = 0;
    debugArmed     = IsArmed();

    /// @note 落ちた «場所» で鳴らす。画面の別の場所を見ていても、何が起きたか方向で分かる。
    se::EnsureSource(scene, "SE", 1.0f);
}

inline void ArenaHazardComponent::SetHazardActive(bool active)
{
    if (m_active == active) return;
    m_active = active;
    /// @note 予告の間は «作動を宣言したが、まだ効かない» 状態。切るときは即座でよい。
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

    /// @note 敵。落ちた時点で終わり。«押し込めば倒せる» が読めるよう、削らずに落とす。
    for (GameObject* object : scene.FindObjectsOfType<EnemyHealthComponent>()) {
        if (!object || !object->activeInHierarchy()) continue;
        if (!Contains(object->transform.worldPosition)) continue;

        const auto* health = scene.GetScript<EnemyHealthComponent>(object);
        if (!health || !health->IsAlive()) continue;

        /// @note マネージャー越しに殺す。撃破数とコア生成はあちらが持つため、直接 HP を 0 にすると倒した分だけ的が残らない。
        if (!combat) continue;
        if (!combat->DamageEnemyDirect(object, std::max(enemyDamage, 1))) continue;

        /// @note ランク評価の «押し込み撃破» は 1 箇所で数える。ここで自前に数えるだけだと、
        ///       壁への押し込みと合算されずに評価が半分になる。
        combat->AddPushKill();
        ++m_pushKills;
        debugPushKills = m_pushKills;
        se::PlayAt(audio, se::kImpactPushKill, object->transform.worldPosition);
    }

    /// @note プレイヤー。痛いが通れる。即死にすると «縁に立って敵だけ落とす» が消える。
    m_playerTick = std::max(0.0f, m_playerTick - Time::deltaTime);
    if (playerDamage <= 0 || m_playerTick > 0.0f) return;

    GameObject* player = scene.FindWithTag(playerTag);
    if (!player || !Contains(player->transform.worldPosition)) return;

    m_playerTick = std::max(playerTickSeconds, 0.05f);
    if (combat) (void)combat->DamagePlayer(player, playerDamage);
    /// @note 通り抜けている間ずっと削られるが、画面はボスを見ているため削られていることは音でしか分からない。
    se::Play(audio, se::kEnvHazardDamage, hazardVolume);
}

inline void ArenaHazardComponent::OnDrawGizmos()
{
    if (!drawArea) return;
    /// @note 作動しているかを色で分ける。切ってあるのに «危険» に見えると、
    ///       配置を確かめている最中に判断を誤る。
    const Vector4 color = IsArmed() ? Vector4{ 1.0f, 0.35f, 0.15f, 1.0f }
                                    : Vector4{ 0.45f, 0.45f, 0.50f, 1.0f };
    debug.DrawSphere(transform.worldPosition, std::max(radius, 0.0f), color);
}

} // namespace sandbox
