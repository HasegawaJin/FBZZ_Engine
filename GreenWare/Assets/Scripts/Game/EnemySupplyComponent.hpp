/// @file    EnemySupplyComponent.hpp
/// @brief   ボス戦中に雑魚を供給し続ける。盤面の «弾薬» を切らさないための係
/// @author  Hasegawa Jin
/// @date    2026-08-27
///
/// WHY «ウェーブ» ではなく «供給» か:
///   15 章は雑魚を武器として使わせる設計で、8 章はボスが銃では削れず «帯電した雑魚を
///   ぶつけたときだけ» HP が減ると決めている。つまり雑魚は敵であると同時に弾薬で、
///   盤面から居なくなった瞬間にプレイヤーの攻撃手段そのものが消える。
///   実際、供給を入れる前の Main は雑魚 3 体しか置かれておらず (Mite は 1 回の激突で
///   死ぬ・Serpent と Roller は 2 回)、全部使い切っても最大 5 回 = 500 ダメージにしか
///   ならなかった。ボスは 800 なので、開始 15 秒で «撃つ弾が無いまま追い回される»
///   状態に入り、そこから何も起こらなくなる。
///   直すべきは進行の刻み方ではなく «燃料が続かないこと» なので、ここは «第 N 波を
///   耐える» ではなく «在庫を切らさない» という 1 つの仕事だけを持つ。
///
/// WHY 在庫数で出すか (一定間隔で湧かせるのではなく):
///   一定間隔だと、プレイヤーが手早く捌いたときほど盤面が空になり、下手に遊ぶほど
///   弾が余る。«盤面に何体居るか» を見て足りない分だけ足せば、消費した人にだけ
///   補充が来る。撃ち合いのテンポをプレイヤー側が決められる。
///
/// WHY フェーズで濃さを変えるか:
///   10.6 の P2 は極の切替が 6 秒から 4 秒へ詰まる。読む間隔が詰まるのに弾の供給が
///   同じだと、P2 は «忙しいだけで手数が増えない» 区間になる。切替が速くなるぶん
///   組み立てる材料も増やして、密度の上がり方を揃える。
#pragma once

#include <Engine/Scene/EntityRef.hpp>
#include <Engine/Scene/Script.hpp>
#include <Scripts/Combat/EnemyHealthComponent.hpp>
#include <Scripts/Combat/IBoss.hpp>
#include <Scripts/Combat/IDamageable.hpp>
#include <Scripts/Utils/SeLibrary.hpp>
#include <algorithm>
#include <cmath>
#include <string>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

class EnemySupplyComponent : public Script {
    FBZZ_SCRIPT(EnemySupplyComponent)

public:
    FBZZ_GROUP("Prefabs")
    FBZZ_TOOLTIP("Main.scene の同名オブジェクトから切り出したもの。空欄の種類は出さない")
    FBZZ_FIELD_FILE(mitePrefab,    "Assets/Prefabs/Enemy_Mite.prefab",    "Mite",    ".prefab")
    FBZZ_FIELD_FILE(serpentPrefab, "Assets/Prefabs/Enemy_Serpent.prefab", "Serpent", ".prefab")
    FBZZ_FIELD_FILE(rollerPrefab,  "Assets/Prefabs/Enemy_Roller.prefab",  "Roller",  ".prefab")

    FBZZ_GROUP("Stock")
    FBZZ_FIELD_RANGE_INT(int, targetAlive, 5, "Target Alive", 0, 32)
    FBZZ_TOOLTIP("盤面に居させたい雑魚の数。異極の組を作る遊びなので、最低でも 4 は要る")
    FBZZ_FIELD_RANGE_INT(int, targetAliveP2, 7, "Target Alive (P2)", 0, 32)
    FBZZ_TOOLTIP("ボスが第 2 フェーズへ入ってからの在庫。切替が詰まるぶん材料も増やす")
    FBZZ_FIELD_RANGE(float, spawnInterval, 1.8f, "Interval", 0.05f, 15.0f)
    FBZZ_TOOLTIP("1 体を足す間隔 [秒]。在庫が足りていても間隔は待つ (一度に湧かせない)")
    FBZZ_FIELD_RANGE(float, spawnIntervalP2, 1.1f, "Interval (P2)", 0.05f, 15.0f)
    FBZZ_FIELD_RANGE(float, firstSpawnDelay, 2.0f, "First Spawn Delay", 0.0f, 30.0f)
    FBZZ_TOOLTIP("開始直後は盤面に置いてある雑魚が居る。被らせないための待ち")

    FBZZ_GROUP("Placement")
    FBZZ_FIELD_RANGE(float, ringRadius, 15.0f, "Ring Radius", 1.0f, 40.0f)
    FBZZ_TOOLTIP("このスクリプトの位置を中心とした円周上に出す。アリーナ半径は実測 20m")
    FBZZ_FIELD_RANGE(float, ringJitter, 2.5f, "Ring Jitter", 0.0f, 10.0f)
    FBZZ_FIELD_RANGE(float, spawnHeight, 0.4f, "Spawn Height", 0.0f, 10.0f)
    FBZZ_TOOLTIP("床へめり込ませないための持ち上げ。着地は物理に任せる")
    FBZZ_FIELD_RANGE(float, minPlayerDistance, 9.0f, "Keep Away From Player", 0.0f, 40.0f)
    FBZZ_TOOLTIP("プレイヤーからこの距離より近い場所には出さない。"
                 "背後に湧くと «避けようがない» になる")
    FBZZ_FIELD_TAG(playerTag, "Player", "Player Tag")

    FBZZ_GROUP("Debug")
    FBZZ_FIELD_READ_ONLY(int, debugAlive, 0, "Alive")
    FBZZ_FIELD_READ_ONLY(int, debugSpawned, 0, "Spawned Total")
    FBZZ_FIELD_READ_ONLY(int, debugPhase, 1, "Boss Phase")
    FBZZ_FIELD(bool, drawSpawnRing, false, "Draw Ring")

    /// 置き場所に依存しない窓口。Wave 進行が初期配置を組むのに引く。
    [[nodiscard]] static EnemySupplyComponent* Instance() { return s_instance; }

    /// 供給を止める / 再開する。
    ///
    /// WHY 要るか: この係は «ボス戦で弾薬を切らさない» ために作られている。
    ///     Wave 1〜4 のあいだも動いていると、倒しても倒しても補充が来て
    ///     «Wave を片付ける» が原理的に終わらない。進行側が窓を開ける。
    void SetSupplyActive(bool active) { m_supplyActive = active; }
    [[nodiscard]] bool IsSupplyActive() const { return m_supplyActive; }

    /// 種類を指定して 1 体出す。0 = Mite / 1 = Serpent / 2 = Roller。
    ///
    /// WHY Wave 側に置かないか: プレファブの参照とプールの温めがここに揃っている。
    ///     進行側にもう 1 組持たせると、機種を差し替えたときに片方だけ古いままになる。
    GameObject* SpawnAt(int kind, const Vector3& point);

    /// 湧かせる円の中心と半径。進行側が同じ円の上へ初期配置を組むために読む。
    [[nodiscard]] Vector3 RingCenter() const { return transform.worldPosition; }
    [[nodiscard]] float   RingRadius() const { return std::max(ringRadius, 0.0f); }

    void OnStart() override;
    void OnUpdate() override;
    void OnDestroy() override { if (s_instance == this) s_instance = nullptr; }
    void OnDrawGizmos() override;

private:
    static inline EnemySupplyComponent* s_instance = nullptr;
    /// 供給の窓。既定で開いているのは、Wave 進行を置かない検証シーンで
    /// «弾が湧かない» にしないため。
    bool m_supplyActive = true;

    /// 盤面に生きている «弾薬» の数。ボスは弾ではないので数えない。
    [[nodiscard]] int CountStock() const;
    /// 今のボスのフェーズ。ボスが居ない構成では 1。
    [[nodiscard]] int BossPhase() const;
    /// ボスが倒れたか。倒れていたら供給を止める。
    [[nodiscard]] bool BossDefeated() const;
    /// 出す場所を 1 つ選ぶ。プレイヤーから遠い側の円周上。
    [[nodiscard]] Vector3 PickSpawnPoint() const;
    /// 次に出す種類。3 種を順に回す。
    [[nodiscard]] const std::string& NextPrefab();

    float m_timer   = 0.0f;
    /// 種類の巡回位置。抽選にすると同じ機種が続いて «3 種居る» が伝わらない。
    int   m_rotation = 0;
    /// 円周上の角度。等分に置くと湧き位置が読めてしまうので、黄金角で送る。
    float m_angle    = 0.0f;
    bool  m_warnedNoPrefab = false;
};

FBZZ_REFLECT(EnemySupplyComponent)


inline void EnemySupplyComponent::OnStart()
{
    s_instance       = this;
    m_timer          = std::max(firstSpawnDelay, 0.0f);
    m_rotation       = 0;
    m_angle          = 0.0f;
    m_warnedNoPrefab = false;
    debugSpawned     = 0;

    // 湧いた位置で鳴らすので 3D。出どころが無いと PlayAtPoint が黙って捨てられる。
    se::EnsureSource(scene, "SE", 1.0f);

    // WHY 事前に作っておくか: Instantiate は .prefab の読み込みから TOML 再構築まで走り、
    //     しかも GameObject 配列を再確保して既存の GameObject* を無効化する
    //     (ScriptSceneProxy.hpp)。戦闘中の初回にそれを踏むと、その 1 フレームだけ
    //     盤面が固まる。プールへ先に積んでおけば、以降の Spawn はその経路を通らない。
    const int warm = std::max(targetAlive, targetAliveP2);
    if (!mitePrefab.empty())    (void)scene.Prewarm(mitePrefab,    warm);
    if (!serpentPrefab.empty()) (void)scene.Prewarm(serpentPrefab, warm);
    if (!rollerPrefab.empty())  (void)scene.Prewarm(rollerPrefab,  warm);
}

inline int EnemySupplyComponent::CountStock() const
{
    int count = 0;
    for (GameObject* object : scene.FindObjectsOfType<EnemyHealthComponent>()) {
        if (!object || !object->activeInHierarchy()) continue;
        // ボスも EnemyHealthComponent を持っている。数に入れると、ボスが生きている
        // 限り在庫が 1 多く見え、その 1 体ぶん永久に補充されない。
        if (scene.GetScript<IBoss>(object)) continue;
        const auto* health = scene.GetScript<EnemyHealthComponent>(object);
        if (health && health->IsAlive()) ++count;
    }
    return count;
}

inline int EnemySupplyComponent::BossPhase() const
{
    if (GameObject* object = FindBossOnBoard(scene)) {
        if (const auto* boss = scene.GetScript<IBoss>(object))
            return boss->CurrentPhase();
    }
    return 1;
}

inline bool EnemySupplyComponent::BossDefeated() const
{
    // ボスが居ない / まだ出ていない構成では «倒された» とは言わない (供給を止めない)。
    GameObject* object = FindBossOnBoard(scene);
    if (!object) return false;

    // 生死は IDamageable の側が持っている (IBoss.hpp: HP をあちらに重ねない)。
    const auto* health = scene.GetScript<EnemyHealthComponent>(object);
    return health && !health->IsAlive();
}

inline const std::string& EnemySupplyComponent::NextPrefab()
{
    static const std::string kEmpty;
    const std::string* order[3] = { &mitePrefab, &serpentPrefab, &rollerPrefab };

    // 空欄の種類は飛ばす。3 つとも空なら供給しない。
    for (int step = 0; step < 3; ++step) {
        const std::string& candidate = *order[m_rotation % 3];
        m_rotation = (m_rotation + 1) % 3;
        if (!candidate.empty()) return candidate;
    }
    return kEmpty;
}

inline Vector3 EnemySupplyComponent::PickSpawnPoint() const
{
    const Vector3 center = transform.worldPosition;
    const GameObject* player = scene.FindWithTag(playerTag);

    // 黄金角 (137.5 度) で送る。等分だと «次はあそこ» が読め、90 度ずつだと
    // 4 か所しか使われない。近い角度が連続しないので、湧き位置が偏らない。
    constexpr float kGoldenAngle = 2.39996323f;

    float angle = m_angle;
    for (int attempt = 0; attempt < 8; ++attempt) {
        const float radius = std::max(ringRadius, 0.0f) +
                             (attempt % 2 == 0 ? 0.0f : std::max(ringJitter, 0.0f));
        Vector3 point{ center.x + std::cos(angle) * radius,
                       center.y + std::max(spawnHeight, 0.0f),
                       center.z + std::sin(angle) * radius };

        if (!player) return point;

        Vector3 toPlayer = player->transform.worldPosition - point;
        toPlayer.y = 0.0f;
        if (toPlayer.Length() >= std::max(minPlayerDistance, 0.0f)) return point;

        angle += kGoldenAngle;
    }

    // 8 回試して全部プレイヤーの近くだった (アリーナが狭い / 距離設定が大きすぎる)。
    // 出さないと供給が止まって詰むので、最後の候補をそのまま使う。
    return Vector3{ center.x + std::cos(angle) * std::max(ringRadius, 0.0f),
                    center.y + std::max(spawnHeight, 0.0f),
                    center.z + std::sin(angle) * std::max(ringRadius, 0.0f) };
}

inline GameObject* EnemySupplyComponent::SpawnAt(int kind, const Vector3& point)
{
    const std::string* order[3] = { &mitePrefab, &serpentPrefab, &rollerPrefab };
    if (kind < 0 || kind > 2) return nullptr;

    const std::string& prefab = *order[kind];
    if (prefab.empty()) return nullptr;

    GameObject* spawned = scene.Spawn(prefab, point, Quaternion::Identity());
    if (!spawned) return nullptr;

    ++debugSpawned;
    se::PlayAt(audio, se::kEnemySpawn, point);
    return spawned;
}

inline void EnemySupplyComponent::OnUpdate()
{
    const int phase = BossPhase();
    debugPhase = phase;

    const int stock = CountStock();
    debugAlive = stock;

    // Wave 進行が窓を閉じている間は補充しない。開いていなければ «Wave を片付ける» が
    // 終わらない (SetSupplyActive の WHY)。
    if (!m_supplyActive) return;

    // 倒したあとも湧き続けると、決着の間に盤面が埋まっていく。
    if (BossDefeated()) return;

    const int   target   = phase >= 2 ? targetAliveP2   : targetAlive;
    const float interval = phase >= 2 ? spawnIntervalP2 : spawnInterval;

    m_timer -= Time::deltaTime;
    if (m_timer > 0.0f) return;
    if (stock >= target) return;

    const std::string& prefab = NextPrefab();
    if (prefab.empty()) {
        if (!m_warnedNoPrefab) {
            m_warnedNoPrefab = true;
            debug.LogError("EnemySupplyComponent has no enemy prefab assigned. "
                           "The board runs out of ammunition and the boss cannot be damaged.");
        }
        return;
    }

    const Vector3 point = PickSpawnPoint();
    m_angle += 2.39996323f;

    // WHY Instantiate ではなく Spawn か: プール経由なら 2 回目以降は .prefab の
    //     読み直しも GameObject 配列の再確保も走らない (ScriptSceneProxy.hpp)。
    GameObject* spawned = scene.Spawn(prefab, point, Quaternion::Identity());
    if (!spawned) {
        // パスが解決できない・プレファブが壊れている。毎フレーム言うと埋まるので 1 度だけ。
        if (!m_warnedNoPrefab) {
            m_warnedNoPrefab = true;
            debug.LogError(("EnemySupplyComponent could not spawn '" + prefab +
                            "'. Check that the .prefab exists under the project root.").c_str());
        }
        return;
    }

    ++debugSpawned;
    m_timer = std::max(interval, 0.05f);

    // 湧いたことは «そこで起きた» 出来事なので場所で鳴らす。姿を見る前に方向が分かる。
    se::PlayAt(audio, se::kEnemySpawn, point);
}

inline void EnemySupplyComponent::OnDrawGizmos()
{
    if (!drawSpawnRing) return;
    debug.DrawSphere(transform.worldPosition, std::max(ringRadius, 0.0f),
                     { 0.3f, 1.0f, 0.5f, 1.0f });
}

} // namespace sandbox
