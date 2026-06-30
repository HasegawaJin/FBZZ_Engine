// FBZZ Engine
// EnemySpawnerComponent.hpp | sandbox
// Wave ベースの Enemy 動的スポーナー。
//
// 設計意図 (WHY):
//   - EnemySpawnPointComponent を付けた GO をシーンに複数配置し、そこへ Prefab から
//     敵を順番にスポーンする。スポーンポイントが 1 つも無い場合は Spawner 自身の位置 +
//     ランダム半径 (spawnRadius) にフォールバックする。
//   - Wave ベース: 現 Wave の全敵が死亡 (HealthComponent::IsDead()) したら waveDelay 秒
//     後に次 Wave が始まる。firstWaveCount + waveCountIncrement * (wave-1) で数が増える。
//   - 敵は EntityID リストで追跡する。destroyOnDeath=true で GO が削除された場合も
//     GetGameObject() が nullptr を返すことで安全に「死亡扱い」にできる。
//   - EnemySpawnPointComponent はマーカーのみのスクリプト。
//     EnemySpawnerComponent が FindObjectsOfType<> で収集して位置を参照する。
#pragma once

#include <Engine/Scene/Script.hpp>
#include "EnemyControllerComponent.hpp"
#include "HealthComponent.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

// =========================================================
// EnemySpawnPointComponent — シーンに置くだけの位置マーカー。
//   EnemySpawnerComponent が OnStart で FindObjectsOfType<> により自動収集する。
// =========================================================
class EnemySpawnPointComponent : public Script {
    FBZZ_SCRIPT(EnemySpawnPointComponent)
    // フィールドなし — transform.position が位置情報のすべて。
};
FBZZ_REFLECT(EnemySpawnPointComponent)

// =========================================================
// EnemySpawnerComponent — Wave ベースの Enemy スポーナー本体。
// =========================================================
class EnemySpawnerComponent : public Script {
    FBZZ_SCRIPT(EnemySpawnerComponent)

public:
    FBZZ_GROUP("Prefab")
    // WHY: パスを文字列で持つことで、シーンファイルを開かずに Inspector から差し替えられる。
    FBZZ_FIELD(std::string, enemyPrefabPath, "", "Enemy Prefab Path")

    FBZZ_GROUP("Wave")
    // Wave 1 の敵数。各 Wave で waveCountIncrement ずつ増える。
    FBZZ_FIELD(int, firstWaveCount,     3, "First Wave Count")
    FBZZ_FIELD(int, waveCountIncrement, 1, "Wave Count Increment")
    // 0 のとき Wave は無限に続く (エンドレスモード)。
    FBZZ_FIELD(int, maxWaves, 0, "Max Waves (0 = infinite)")
    // 前 Wave 全滅から次 Wave 開始までの待機時間 (秒)。
    FBZZ_FIELD_RANGE(float, waveDelay,      3.0f, "Wave Delay",      0.0f, 30.0f)
    // 同 Wave 内で敵を 1 体ずつスポーンする間隔 (秒)。
    FBZZ_FIELD_RANGE(float, spawnInterval,  0.4f, "Spawn Interval",  0.0f,  5.0f)

    FBZZ_GROUP("Spawn Position")
    // EnemySpawnPointComponent が 0 個の場合、Spawner 位置からこの半径内にランダムスポーン。
    FBZZ_FIELD_RANGE(float, spawnRadius, 5.0f, "Spawn Radius (fallback)", 0.0f, 50.0f)

    FBZZ_GROUP("Animator Params")
    // スポーン直後に再生したいトリガー (例: 出現モーション)。空なら何もしない。
    FBZZ_FIELD(std::string, spawnTriggerParam, "", "Spawn Trigger Param")

    void OnStart() override;
    void OnUpdate() override;

    // 現在の Wave 番号 (1 始まり)。UI 表示などに使う。
    [[nodiscard]] int  GetCurrentWave()   const { return m_currentWave; }
    [[nodiscard]] bool IsFinished()       const { return m_finished; }
    [[nodiscard]] bool IsWaitingForWave() const { return m_waitingForNextWave; }

private:
    void StartWave();
    void SpawnNext();
    [[nodiscard]] Vector3 PickSpawnPosition();
    [[nodiscard]] bool AllEnemiesDead() const;

    // FindObjectsOfType<EnemySpawnPointComponent>() で OnStart に収集するスポーン地点 GO。
    std::vector<GameObject*> m_spawnPoints;
    // 現 Wave でスポーンした敵の EntityID リスト。
    // WHY: raw pointer は destroyOnDeath で GO 削除後にダングリングするため、
    //      EntityID + GetGameObject() で参照の有効性を毎フレーム確認する。
    std::vector<EntityID> m_activeEnemies;

    int   m_currentWave         = 0;
    int   m_spawnedThisWave     = 0;
    int   m_targetCountThisWave = 0;
    int   m_spawnPointIndex     = 0;
    float m_spawnTimer          = 0.0f;
    float m_waveDelayTimer      = 0.0f;
    bool  m_waitingForNextWave  = false;
    bool  m_finished            = false;

    // WHY: GO ごとに異なる seed を与えることで、複数 Spawner が同フレームに起動しても
    //      各自が独立した乱数列を持ち、スポーン位置が重複しにくくなる。
    std::mt19937 m_rng;
};

FBZZ_REFLECT(EnemySpawnerComponent)

// ---------------------------------------------------------
// EnemySpawnPointComponent 実装 (ボディなし — Reflect のみ)
// ---------------------------------------------------------

// ---------------------------------------------------------
// EnemySpawnerComponent 実装
// ---------------------------------------------------------

inline void EnemySpawnerComponent::OnStart()
{
    // GO の EntityID index を seed に加えることで Spawner 間の乱数列を分散させる。
    const uint32_t seed = (m_gameObject ? m_gameObject->GetID().index : 0u)
                          ^ static_cast<uint32_t>(std::random_device{}());
    m_rng = std::mt19937(seed);

    // シーン全体から EnemySpawnPoint マーカーを収集する。
    // WHY: OnStart 時点でシーンが確定しているため、ここで 1 度だけ検索すれば十分。
    //      動的スポーンした SpawnPoint を後から追加したい場合は都度 FindObjectsOfType を呼ぶ拡張が容易。
    for (auto* go : scene.FindObjectsOfType<EnemySpawnPointComponent>())
        if (go && go->IsValid() && go->activeInHierarchy())
            m_spawnPoints.push_back(go);

    StartWave();
}

inline void EnemySpawnerComponent::OnUpdate()
{
    if (m_finished) return;

    // Wave 間ウェイト中: カウントダウンして次 Wave を開始する。
    if (m_waitingForNextWave) {
        m_waveDelayTimer -= Time::deltaTime;
        if (m_waveDelayTimer <= 0.0f) {
            m_waitingForNextWave = false;
            StartWave();
        }
        return;
    }

    // まだスポーンすべき敵が残っている: interval ごとに 1 体ずつ出す。
    if (m_spawnedThisWave < m_targetCountThisWave) {
        m_spawnTimer -= Time::deltaTime;
        if (m_spawnTimer <= 0.0f) {
            SpawnNext();
            m_spawnTimer = spawnInterval;
        }
        return;
    }

    // 全体スポーン完了 & 全員死亡 → 次 Wave へ。
    if (AllEnemiesDead()) {
        if (maxWaves > 0 && m_currentWave >= maxWaves) {
            // 全 Wave クリア。
            m_finished = true;
            return;
        }
        m_waitingForNextWave = true;
        m_waveDelayTimer     = waveDelay;
    }
}

inline void EnemySpawnerComponent::StartWave()
{
    ++m_currentWave;
    m_activeEnemies.clear();
    m_spawnedThisWave     = 0;
    m_spawnTimer          = 0.0f; // 最初の敵は即座にスポーン
    // Wave が進むたびに敵数を increment 増やす。
    m_targetCountThisWave = firstWaveCount + waveCountIncrement * (m_currentWave - 1);
}

inline void EnemySpawnerComponent::SpawnNext()
{
    if (enemyPrefabPath.empty()) return;

    const Vector3 pos = PickSpawnPosition();

    // WHY: init コールバック内で position を上書きすることで、Prefab 本体の初期座標に
    //      依存せず任意のスポーン位置に正確に配置できる。
    GameObject* enemy = scene.Instantiate(enemyPrefabPath, [&](GameObject& go) {
        go.transform.position = pos;
        // スポーン演出トリガーを Prefab 生成直後に発火する。
        // WHY: OnStart より前にここで呼ぶと AnimatorSystem がまだ動いていないため、
        //      実際のトリガー設定は OnStart で初期化済みの ScriptAnimatorProxy 経由で行う。
        //      ここでは位置設定のみ担う。
    });

    if (!enemy) return;

    m_activeEnemies.push_back(enemy->GetID());
    ++m_spawnedThisWave;

    // スポーントリガーアニメーション。
    // WHY: Instantiate のコールバック内では AnimatorProxy が未初期化のため、
    //      GO ポインタ経由でスクリプトに後からトリガーを掛ける。
    if (!spawnTriggerParam.empty()) {
        if (auto* ec = scene.GetScript<EnemyControllerComponent>(enemy)) {
            // EnemyControllerComponent は animator を持つため、外部から直接呼べない。
            // アニメーション発火は EnemyControllerComponent の OnStart に委ねる設計とし、
            // ここでは位置確定のみ担う。(拡張: EnemyControllerComponent に spawnTrigger フィールドを追加)
            (void)ec;
        }
    }
}

inline Vector3 EnemySpawnerComponent::PickSpawnPosition()
{
    // SpawnPoint GO がある場合は順番に使い回す。
    if (!m_spawnPoints.empty()) {
        // 無効化された SpawnPoint はスキップして次を探す。
        const int total = static_cast<int>(m_spawnPoints.size());
        for (int attempt = 0; attempt < total; ++attempt) {
            const int idx = m_spawnPointIndex % total;
            ++m_spawnPointIndex;
            GameObject* sp = m_spawnPoints[static_cast<size_t>(idx)];
            if (sp && sp->IsValid() && sp->activeInHierarchy())
                return sp->transform.worldPosition;
        }
    }

    // フォールバック: Spawner 自身の XZ 平面上でランダム半径内に配置し、
    // 地形高さに合わせる。
    std::uniform_real_distribution<float> angleDist(0.0f, 6.2831853f); // 0〜2π
    std::uniform_real_distribution<float> radiusDist(0.5f, std::max(spawnRadius, 0.5f));
    const float angle  = angleDist(m_rng);
    const float radius = radiusDist(m_rng);

    Vector3 pos = transform.worldPosition;
    pos.x += std::cosf(angle) * radius;
    pos.z += std::sinf(angle) * radius;
    // WHY: 地形高さに吸着させることで、Spawner が宙に浮いていても敵は地面から出現する。
    pos.y = scene.GetTerrainHeightAt(pos);
    return pos;
}

inline bool EnemySpawnerComponent::AllEnemiesDead() const
{
    for (const EntityID& id : m_activeEnemies) {
        GameObject* go = scene.GetGameObject(id);
        // GO が nullptr → destroyOnDeath で削除済み → 死亡扱い。
        if (!go || !go->IsValid()) continue;
        // HealthComponent がない GO は生存とみなす (スポーン直後など)。
        const auto* health = scene.GetScript<HealthComponent>(go);
        if (!health || !health->IsDead()) return false;
    }
    return true;
}

} // namespace sandbox
