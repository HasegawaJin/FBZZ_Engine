// GreenWare
// MagnetPrototypeComponent.hpp | sandbox
// クリックで敵へ磁極を付与し、異極同士を引き寄せて衝突撃破するプロトタイプ
#pragma once

#include <Engine/Scene/Components/BehaviorTreeComponent.hpp>
#include <Engine/Scene/Components/CharacterControllerComponent.hpp>
#include <Engine/Scene/Components/ColliderComponent.hpp>
#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Components/NavMeshAgentComponent.hpp>
#include <Engine/Scene/Components/NavMeshModifierComponent.hpp>
#include <Engine/Scene/Components/NavMeshPatrolComponent.hpp>
#include <Engine/Scene/Components/NavMeshSurfaceComponent.hpp>
#include <Engine/Scene/Components/RigidBodyComponent.hpp>
#include <Engine/Scene/Components/UICanvas.hpp>
#include <Engine/Scene/Components/UIText.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Physics/RigidBody.hpp>
#include "Scripts/PlayerControllerComponent.hpp"
#include "Scripts/SceneManagerScript.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <random>
#include <string>

using namespace fbzz::scene;
using namespace fbzz::math;
using namespace fbzz::input;
using fbzz::Time;

namespace sandbox {

// WHY: MeshRenderer だけでは描画されない (RenderSystem は MaterialComponent が無い GO を
//      描画スキップする)。新規 .mat を作らず、共有 Fallback.mat + インスタンス単位の
//      albedo 上書きで単色マテリアルを与える、実行時生成オブジェクト共通のヘルパー。
inline void ApplyFlatColor(GameObject& go, const Vector4& color)
{
    auto& material = go.AddComponent<MaterialComponent>();
    material.materialPath = "Assets/Materials/Fallback/Fallback.mat";
    material.paramOverrides["albedo"] = { color.x, color.y, color.z, color.w };
}

// 磁極の状態。未付与を含めて明示的に持つことで、同極を誤って吸着させない。
enum class MagnetPole : int {
    None = 0,
    North = 1,
    South = 2,
};

class MagnetEnemyComponent : public Script {
    FBZZ_SCRIPT(MagnetEnemyComponent)

public:
    FBZZ_GROUP("Magnet")
    FBZZ_FIELD_RANGE_INT(int, startingPole, 0, "Starting Pole (0=None, 1=N, 2=S)", 0, 2)

    void OnStart() override;
    void OnCollisionEnter(const CollisionInfo& info) override;
    void OnCollisionStay(const CollisionInfo& info) override;

    // クリック操作と磁力システムから呼ぶ。磁極の変更と表示更新を同じ入口へ集約する。
    void AssignPole(MagnetPole pole);
    void ApplyMagneticForce(const Vector3& force);
    [[nodiscard]] MagnetPole GetPole() const { return m_pole; }

private:
    void CreatePolarityUI();
    void UpdatePolarityUI();
    bool IsOpposite(const MagnetEnemyComponent& other) const;
    void ResolveMagneticCollision(GameObject* otherObject);

    MagnetPole  m_pole = MagnetPole::None;
    GameObject* m_uiRoot = nullptr;
    bool        m_defeated = false;
};

FBZZ_REFLECT(MagnetEnemyComponent)

inline void MagnetEnemyComponent::OnStart()
{
    // Scene からロードした敵にも初期極を設定できるようにする。
    m_pole = startingPole == 1 ? MagnetPole::North
             : startingPole == 2 ? MagnetPole::South
                                 : MagnetPole::None;
    CreatePolarityUI();
    UpdatePolarityUI();
}

inline void MagnetEnemyComponent::CreatePolarityUI()
{
    if (!m_gameObject || m_uiRoot) return;

    // WHY: WorldSpace Canvas を敵の子にすることで、敵が物理移動しても UI の追従処理を
    //      専用システムへ追加せず、既存の Transform 階層だけで実現する。
    GameObject& canvasObject = scene.Create(m_gameObject->name + "_PolarityUI");
    canvasObject.runtimeGenerated = true;
    canvasObject.SetParent(*m_gameObject);
    canvasObject.transform.position = { 0.0f, 1.45f, 0.0f };

    auto& canvas = canvasObject.AddComponent<UICanvas>();
    canvas.canvasWidth = 256.0f;
    canvas.canvasHeight = 128.0f;
    canvas.worldScale = 0.005f;
    canvas.renderMode = UIRenderMode::WorldSpace;
    canvas.faceCamera = true;
    canvas.sortOrder = 20;

    GameObject& labelObject = scene.Create(m_gameObject->name + "_PolarityLabel");
    labelObject.runtimeGenerated = true;
    labelObject.SetParent(canvasObject);
    labelObject.transform.position = { 128.0f, 64.0f, 0.0f };
    labelObject.transform.scale = { 120.0f, 120.0f, 1.0f };

    auto& label = labelObject.AddComponent<UIText>();
    label.fontSize = 96.0f;
    label.letterSpacing = 0.0f;
    label.align = TextAlign::Center;
    label.sortOrder = 21;
    m_uiRoot = &canvasObject;
}

inline void MagnetEnemyComponent::UpdatePolarityUI()
{
    if (!m_uiRoot) return;
    auto* labelObject = m_uiRoot->GetChild(0);
    auto* label = labelObject ? labelObject->GetComponent<UIText>() : nullptr;
    if (!label) return;

    const bool assigned = m_pole != MagnetPole::None;
    m_uiRoot->SetActive(assigned);
    if (!assigned) return;

    // N を +、S を - として表示し、色でも極性を補助する。
    label->text = m_pole == MagnetPole::North ? "+" : "-";
    label->color = m_pole == MagnetPole::North
        ? Vector4{ 1.0f, 0.20f, 0.20f, 1.0f }
        : Vector4{ 0.25f, 0.55f, 1.0f, 1.0f };
}

inline void MagnetEnemyComponent::AssignPole(MagnetPole pole)
{
    if (m_defeated) return;
    m_pole = pole;
    startingPole = static_cast<int>(pole);
    UpdatePolarityUI();

    // WHY: NavMeshPatrolSystem は BT を介さず独立に巡回を進める (BehaviorTreeComponent の
    //      状態を見ない) ため、BT だけでなく Patrol/Agent も個別に無効化しないと
    //      徘徊 AI の位置書き込みが磁力の物理挙動と競合する。
    if (auto* bt = scene.GetComponent<BehaviorTreeComponent>()) bt->enabled = false;
    if (auto* patrol = scene.GetComponent<NavMeshPatrolComponent>()) patrol->enabled = false;
    if (auto* agent = scene.GetComponent<NavMeshAgentComponent>()) {
        agent->enabled = false;
        agent->Stop();
    }
}

inline void MagnetEnemyComponent::ApplyMagneticForce(const Vector3& force)
{
    if (m_defeated || m_pole == MagnetPole::None || !physics.HasRigidBody()) return;
    physics.AddForce(force);
}

inline bool MagnetEnemyComponent::IsOpposite(const MagnetEnemyComponent& other) const
{
    return (m_pole == MagnetPole::North && other.m_pole == MagnetPole::South)
        || (m_pole == MagnetPole::South && other.m_pole == MagnetPole::North);
}

inline void MagnetEnemyComponent::ResolveMagneticCollision(GameObject* otherObject)
{
    if (m_defeated || !otherObject || otherObject == m_gameObject) return;
    auto* other = scene.GetScript<MagnetEnemyComponent>(otherObject);
    if (!other || !IsOpposite(*other) || other->m_defeated) return;

    // 両側の OnCollisionEnter が呼ばれても、同じ接触を二重処理しない。
    if (m_gameObject->instanceId > otherObject->instanceId) return;
    m_defeated = true;
    other->m_defeated = true;
    scene.Destroy(*m_gameObject);
    scene.Destroy(*otherObject);
}

inline void MagnetEnemyComponent::OnCollisionEnter(const CollisionInfo& info)
{
    ResolveMagneticCollision(info.other);
}

inline void MagnetEnemyComponent::OnCollisionStay(const CollisionInfo& info)
{
    ResolveMagneticCollision(info.other);
}

// プレイヤーが撃つ磁力弾。RigidBody は持たず、OnUpdate で transform を直接動かす
// (物理演算に乗せると敵に当たった瞬間に押し返してしまうため)。トリガー衝突で命中判定する。
class MagnetBulletComponent : public Script {
    FBZZ_SCRIPT(MagnetBulletComponent)

public:
    // スポーンした側 (MagnetShooterComponent::Fire) が直接セットするランタイム専用パラメータ。
    // Inspector 調整の必要が無いため FBZZ_FIELD 化はしない。
    Vector3    velocity = Vector3::ZERO;
    MagnetPole pole     = MagnetPole::None;
    float      lifetime = 3.0f;

    void OnUpdate() override;
    void OnTriggerEnter(const CollisionInfo& info) override;

private:
    float m_age = 0.0f;
};

FBZZ_REFLECT(MagnetBulletComponent)

inline void MagnetBulletComponent::OnUpdate()
{
    if (!transform) return;
    transform.position += velocity * Time::deltaTime;

    m_age += Time::deltaTime;
    if (m_age >= lifetime)
        scene.Destroy(*m_gameObject);
}

inline void MagnetBulletComponent::OnTriggerEnter(const CollisionInfo& info)
{
    if (auto* enemy = scene.GetScript<MagnetEnemyComponent>(info.other))
        enemy->AssignPole(pole);
    scene.Destroy(*m_gameObject);
}

// プレイヤーにアタッチする射撃コンポーネント。左クリック = North(+)、右クリック = South(-)。
class MagnetShooterComponent : public Script {
    FBZZ_SCRIPT(MagnetShooterComponent)

public:
    FBZZ_GROUP("Shooter")
    FBZZ_FIELD_RANGE(float, bulletSpeed,  20.0f, "Bullet Speed",   1.0f, 60.0f)
    FBZZ_FIELD_RANGE(float, fireCooldown, 0.25f, "Fire Cooldown",  0.0f, 2.0f)

    void OnUpdate() override;

private:
    void Fire(MagnetPole pole);

    float m_cooldownTimer = 0.0f;
};

FBZZ_REFLECT(MagnetShooterComponent)

inline void MagnetShooterComponent::OnUpdate()
{
    m_cooldownTimer = std::max(0.0f, m_cooldownTimer - Time::deltaTime);
    if (m_cooldownTimer > 0.0f) return;

    if (input.MouseButtonDown(MouseBtn::Left)) {
        Fire(MagnetPole::North);
        m_cooldownTimer = fireCooldown;
    } else if (input.MouseButtonDown(MouseBtn::Right)) {
        Fire(MagnetPole::South);
        m_cooldownTimer = fireCooldown;
    }
}

inline void MagnetShooterComponent::Fire(MagnetPole pole)
{
    if (!transform) return;
    auto* camGO = scene.GetMainCameraObject();
    const Vector3 dir = camGO ? camGO->transform.forward.Normalized() : transform.forward;

    // WHY: プレイヤー自身のコライダーに埋まった状態で発射するとトリガーが自己検出しかねないため、
    //      胸の高さ・少し前方から発射する。
    GameObject& bullet = scene.Create("MagnetBullet");
    bullet.runtimeGenerated = true;
    bullet.transform.position = transform.worldPosition + Vector3::UP * 1.4f + dir * 0.8f;
    bullet.transform.scale    = { 0.25f, 0.25f, 0.25f };

    auto& mesh = bullet.AddComponent<MeshRenderer>();
    mesh.meshPath = "primitive:sphere";
    // 既存の極性表示 (MagnetEnemyComponent::UpdatePolarityUI) と同じ配色で揃える。
    ApplyFlatColor(bullet, pole == MagnetPole::North
        ? Vector4{ 1.0f, 0.20f, 0.20f, 1.0f }
        : Vector4{ 0.25f, 0.55f, 1.0f, 1.0f });

    auto& collider = bullet.AddComponent<SphereColliderComponent>();
    collider.SetRadius(0.18f);
    collider.SetTrigger(true);

    auto& bulletScript = bullet.AddScript<MagnetBulletComponent>();
    bulletScript.velocity = dir * bulletSpeed;
    bulletScript.pole     = pole;
}

class MagnetPrototypeComponent : public Script {
    FBZZ_SCRIPT(MagnetPrototypeComponent)

public:
    FBZZ_GROUP("Magnet Gameplay")
    FBZZ_FIELD_RANGE(float, attractionStrength, 60.0f, "Attraction Strength", 0.0f, 500.0f)
    FBZZ_FIELD_RANGE(float, attractionRadius, 18.0f, "Attraction Radius", 0.5f, 100.0f)
    FBZZ_FIELD_RANGE(float, minimumDistance, 1.1f, "Minimum Distance", 0.1f, 10.0f)
    FBZZ_FIELD(bool, spawnDemoEnemies, true, "Spawn Demo Enemies")
    FBZZ_FIELD_RANGE_INT(int, demoEnemyCount, 6, "Demo Enemy Count", 2, 12)

    void OnStart() override;
    void OnUpdate() override;

private:
    void ApplyAttraction();
    void SpawnFloor();
    void SpawnPlayer();
    void SpawnDemoEnemies();

    bool m_demoSpawned  = false;
    bool m_floorSpawned = false;
    Vector3 m_arenaCenter  = Vector3::ZERO;
    Vector3 m_arenaRight   = Vector3::RIGHT;
    Vector3 m_arenaForward = Vector3::FORWARD;
};

FBZZ_REFLECT(MagnetPrototypeComponent)

inline void MagnetPrototypeComponent::OnStart()
{
    // 極付与はプレイヤーが撃つ弾 (MagnetBulletComponent) のヒットだけで行う。
    // カーソル直下を即座にクリック判定する旧方式は廃止した。
    if (spawnDemoEnemies && !scene.FindObjectOfType<MagnetEnemyComponent>()) {
        SpawnFloor();
        SpawnPlayer();
        SpawnDemoEnemies();
    }
}

inline void MagnetPrototypeComponent::OnUpdate()
{
    ApplyAttraction();
}

inline void MagnetPrototypeComponent::ApplyAttraction()
{
    const auto enemies = scene.FindObjectsOfType<MagnetEnemyComponent>();
    const float radiusSq = attractionRadius * attractionRadius;
    const float minimumDistanceSq = minimumDistance * minimumDistance;

    for (GameObject* sourceObject : enemies) {
        auto* source = scene.GetScript<MagnetEnemyComponent>(sourceObject);
        if (!source || source->GetPole() == MagnetPole::None) continue;

        for (GameObject* targetObject : enemies) {
            auto* target = scene.GetScript<MagnetEnemyComponent>(targetObject);
            if (!target || target == source || target->GetPole() == MagnetPole::None) continue;
            if (!((source->GetPole() == MagnetPole::North && target->GetPole() == MagnetPole::South)
               || (source->GetPole() == MagnetPole::South && target->GetPole() == MagnetPole::North)))
                continue;

            const Vector3 delta = targetObject->transform.worldPosition
                                - sourceObject->transform.worldPosition;
            const float distanceSq = delta.LengthSq();
            if (distanceSq <= minimumDistanceSq || distanceSq > radiusSq) continue;

            const float distance = std::sqrt(distanceSq);
            const float falloff = std::clamp(1.0f - distance / attractionRadius, 0.15f, 1.0f);
            source->ApplyMagneticForce(delta * (attractionStrength * falloff / distance));
        }
    }
}

inline void MagnetPrototypeComponent::SpawnFloor()
{
    if (m_floorSpawned || !transform) return;
    m_floorSpawned = true;

    // WHY: NavMesh Bake のソースになる床を、カメラの実際の向きから安全に (シーン TOML の
    //      クォータニオンを手計算せずに) 実行時生成する。既存のカメラ相対デモ敵配置と同じ方式。
    Vector3 fwd = transform.forward;
    fwd.y = 0.0f;
    fwd = fwd.LengthSq() > EPSILON ? fwd.Normalized() : Vector3::FORWARD;
    m_arenaRight   = Vector3::Cross(Vector3::UP, fwd).Normalized();
    m_arenaForward = fwd;
    m_arenaCenter  = transform.worldPosition + fwd * 8.0f;
    m_arenaCenter.y = 0.0f; // 床上面をワールド Y=0 とする

    constexpr float kArenaSize = 20.0f;
    constexpr float kThickness = 0.4f;

    GameObject& floor = scene.Create("MagnetArenaFloor");
    floor.runtimeGenerated = true;
    floor.transform.position = m_arenaCenter - Vector3::UP * (kThickness * 0.5f);
    floor.transform.scale    = { kArenaSize, kThickness, kArenaSize };

    auto& mesh = floor.AddComponent<MeshRenderer>();
    mesh.meshPath = "primitive:cube";
    ApplyFlatColor(floor, { 0.2f, 0.22f, 0.2f, 1.0f });

    auto& collider = floor.AddComponent<BoxColliderComponent>();
    collider.SetSize({ kArenaSize, kThickness, kArenaSize });

    auto& modifier = floor.AddComponent<NavMeshModifierComponent>();
    modifier.mode = NavMeshModifierMode::Walkable;

    auto& surface = floor.AddComponent<NavMeshSurfaceComponent>();
    // WHY: AddComponent 直後は既定で false (エディタでの意図しない自動ベイクを防ぐため)。
    //      シーン TOML 経由の読込ではないため、ここで明示的にベイクを要求する。
    surface.needsBake = true;
}

inline void MagnetPrototypeComponent::SpawnPlayer()
{
    if (!transform || scene.FindWithTag("Player")) return;

    // WHY: primitive:capsule は既定で半径0.5・半円柱長0.5 (全高2m) で生成される。
    //      CapsuleColliderComponent の既定 halfHeight=1.0 は見た目と噛み合わないため、
    //      radius==halfHeight のまま SetCapsule() で明示的に揃える。
    constexpr float kRadius = 0.5f;
    constexpr float kHalfHeight = 0.5f;
    constexpr float kRestY = kRadius + kHalfHeight; // カプセル下端が床 (Y=0) に接する高さ

    GameObject& player = scene.Create("Player");
    player.tag = "Player";
    player.transform.position = m_arenaCenter - m_arenaForward * 3.0f + Vector3::UP * kRestY;

    auto& mesh = player.AddComponent<MeshRenderer>();
    mesh.meshPath = "primitive:capsule";
    ApplyFlatColor(player, { 0.25f, 0.55f, 1.0f, 1.0f });

    auto& collider = player.AddComponent<CapsuleColliderComponent>();
    collider.SetCapsule(kRadius, kHalfHeight);

    auto& body = player.AddComponent<RigidBodyComponent>();
    body.rigidBody = std::make_unique<fbzz::physics::RigidBody>();
    body.rigidBody->SetMass(70.0f);
    body.rigidBody->m_linearDrag = 0.05f;
    body.rigidBody->SetPosition(player.transform.position);

    player.AddComponent<CharacterControllerComponent>();
    player.AddScript<PlayerControllerComponent>();
    player.AddScript<MagnetShooterComponent>();
}

inline void MagnetPrototypeComponent::SpawnDemoEnemies()
{
    if (m_demoSpawned || !transform) return;
    m_demoSpawned = true;

    // WHY: アセット依存なしで入力と物理を確認できるよう、primitive:capsule を使った
    //      デモ敵を実行時だけ生成する。runtimeGenerated のためシーンへ保存されない。
    // WHY 床基準の水平グリッドか: 敵は NavMeshAgent で床上を徘徊するため、
    //      旧来の縦積み配置 (Vector3::UP) ではなく床の平面 (right/forward) 上に並べる。
    constexpr float kEnemyRadius = 0.35f;
    constexpr float kEnemyHalfHeight = 0.35f; // radius と揃えて見た目とコライダーを一致させる
    constexpr float kEnemyScale  = kEnemyRadius / 0.5f; // primitive:capsule の既定半径0.5からの倍率
    constexpr float kEnemyRestY  = kEnemyRadius + kEnemyHalfHeight; // カプセル下端を床に接地させる
    constexpr float kPatrolRadius = 3.0f;
    constexpr int   kColumns      = 3;

    std::mt19937 rng{ std::random_device{}() };
    std::uniform_real_distribution<float> jitter(-kPatrolRadius, kPatrolRadius);

    for (int i = 0; i < demoEnemyCount; ++i) {
        const int row    = i / kColumns;
        const int column = i % kColumns;
        const Vector3 spawnPos = m_arenaCenter
            + m_arenaRight   * (static_cast<float>(column - 1) * 3.0f)
            + m_arenaForward * (static_cast<float>(row) * 3.0f + 3.0f)
            + Vector3::UP * kEnemyRestY;

        GameObject& enemy = scene.Create("MagnetEnemy_" + std::to_string(i + 1));
        enemy.runtimeGenerated = true;
        enemy.tag = "MagnetEnemy";
        enemy.transform.position = spawnPos;
        enemy.transform.scale = { kEnemyScale, kEnemyScale, kEnemyScale };

        auto& mesh = enemy.AddComponent<MeshRenderer>();
        mesh.meshPath = "primitive:capsule";
        ApplyFlatColor(enemy, { 0.85f, 0.15f, 0.15f, 1.0f });

        auto& collider = enemy.AddComponent<CapsuleColliderComponent>();
        collider.SetCapsule(kEnemyRadius, kEnemyHalfHeight);

        auto& body = enemy.AddComponent<RigidBodyComponent>();
        body.rigidBody = std::make_unique<fbzz::physics::RigidBody>();
        body.rigidBody->SetMass(1.0f);
        body.rigidBody->m_useGravity = false;
        body.rigidBody->m_linearDrag = 0.35f;
        body.rigidBody->SetPosition(enemy.transform.position);
        body.rigidBody->SetFreezePosition({ false, true, false });

        // WHY: 極が付くまでは BehaviorTree (Repeat->Patrol) が NavMeshAgent 経由で
        //      ランダムなウェイポイントを徘徊させる。極が付いた瞬間 AssignPole が
        //      これらを無効化し、以降は磁力の物理挙動だけが位置を書き換える。
        auto& agent = enemy.AddComponent<NavMeshAgentComponent>();
        agent.radius           = kEnemyRadius;
        agent.maxSpeed         = 1.8f;
        agent.acceleration     = 4.0f;
        agent.stoppingDistance = 0.3f;
        agent.snapToNavMesh    = false; // Y はここで固定した高さのまま NavMesh に触らせない

        auto& patrol = enemy.AddComponent<NavMeshPatrolComponent>();
        patrol.mode     = NavMeshPatrolComponent::Mode::LOOP;
        patrol.waitTime = 1.5f;
        for (int p = 0; p < 3; ++p) {
            Vector3 point = spawnPos + Vector3{ jitter(rng), 0.0f, jitter(rng) };
            point.y = kEnemyRestY;
            patrol.waypoints.push_back(point);
        }

        auto& bt = enemy.AddComponent<BehaviorTreeComponent>();
        bt.treePath = "Assets/AI/MagnetEnemyWander.behaviortree";

        enemy.AddScript<MagnetEnemyComponent>();
    }
}

// 磁力ミニゲームの勝敗管理。敵の残数とタイマーを監視し、HUD 表示と
// 勝敗判定、Result シーンへの遷移をまとめて担当する。
class MagnetGameManager : public Script {
    FBZZ_SCRIPT(MagnetGameManager)

public:
    FBZZ_GROUP("Magnet Rules")
    FBZZ_FIELD_RANGE(float, timeLimit,            45.0f, "Time Limit (s)",      10.0f, 300.0f)
    FBZZ_FIELD_RANGE_INT(int, pointsPerEnemy,     50,    "Points Per Enemy",    0,     500)
    FBZZ_FIELD_RANGE_INT(int, timeBonusPerSecond, 10,    "Time Bonus / Second", 0,     100)

    void OnStart() override;
    void OnUpdate() override;

    // Result シーンが読む勝敗結果。SceneManagerScript::s_next と同じく、
    // シーン境界をまたいで結果を伝える static キャリア。
    static inline bool s_won      = false;
    static inline int  s_score    = 0;
    static inline int  s_defeated = 0;
    static inline int  s_total    = 0;

private:
    void BuildHud();
    void UpdateHud();
    void FinishGame(bool won);

    GameObject* m_remainingLabel = nullptr;
    GameObject* m_timerLabel     = nullptr;
    GameObject* m_scoreLabel     = nullptr;
    int   m_totalEnemies  = 0;
    int   m_remaining     = 0;
    float m_timeRemaining = 0.0f;
    bool  m_finished       = false;
};

FBZZ_REFLECT(MagnetGameManager)

inline void MagnetGameManager::OnStart()
{
    // WHY: MainCamera の ScriptComponents リストで本スクリプトは
    //      MagnetPrototypeComponent の後ろに置く前提。ScriptSystem は 1 エンティティ内の
    //      スクリプトを配列順に Awake→Start→Update まで完了させてから次へ進むため、
    //      同フレーム内で敵スポーンが先に終わっている状態でここに来る。
    m_totalEnemies = static_cast<int>(scene.FindObjectsOfType<MagnetEnemyComponent>().size());
    m_remaining     = m_totalEnemies;
    m_timeRemaining = timeLimit;
    if (m_totalEnemies <= 0) {
        m_finished = true; // 監視対象の敵がいなければ何もしない
        return;
    }
    BuildHud();
    UpdateHud();
}

inline void MagnetGameManager::BuildHud()
{
    GameObject& canvasObject = scene.Create("MagnetHUD");
    canvasObject.runtimeGenerated = true;

    auto& canvas = canvasObject.AddComponent<UICanvas>();
    canvas.canvasWidth  = 1920.0f;
    canvas.canvasHeight = 1080.0f;
    canvas.renderMode   = UIRenderMode::ScreenSpaceOverlay;
    canvas.scaleMode    = UICanvasScaleMode::ScaleWithScreenSize;
    canvas.sortOrder    = 10;

    auto makeLabel = [&](const char* name, float y, float fontSize) -> GameObject& {
        GameObject& labelObject = scene.Create(name);
        labelObject.runtimeGenerated = true;
        labelObject.SetParent(canvasObject);
        labelObject.transform.position = { 40.0f, y, 0.0f };
        labelObject.transform.scale    = { 400.0f, fontSize, 1.0f };

        auto& label = labelObject.AddComponent<UIText>();
        label.fontSize = fontSize;
        label.align     = TextAlign::Left;
        label.color     = { 1.0f, 1.0f, 1.0f, 1.0f };
        return labelObject;
    };

    m_remainingLabel = &makeLabel("HudRemaining", 40.0f, 34.0f);
    m_timerLabel     = &makeLabel("HudTimer",     84.0f, 34.0f);
    m_scoreLabel     = &makeLabel("HudScore",    128.0f, 26.0f);
}

inline void MagnetGameManager::UpdateHud()
{
    // WHY: UIText へのポインタは他コンポーネント追加時のストレージ再配置で
    //      無効化しうるため、MagnetEnemyComponent::UpdatePolarityUI() と同様に
    //      毎回 GetComponent() で取り直す。
    if (auto* text = m_remainingLabel ? m_remainingLabel->GetComponent<UIText>() : nullptr)
        text->text = "REMAINING " + std::to_string(m_remaining);

    if (auto* text = m_timerLabel ? m_timerLabel->GetComponent<UIText>() : nullptr) {
        const int totalSeconds = static_cast<int>(std::ceil(std::max(0.0f, m_timeRemaining)));
        char buf[16];
        std::snprintf(buf, sizeof(buf), "TIME %d:%02d", totalSeconds / 60, totalSeconds % 60);
        text->text = buf;
    }

    if (auto* text = m_scoreLabel ? m_scoreLabel->GetComponent<UIText>() : nullptr) {
        const int defeated = m_totalEnemies - m_remaining;
        text->text = "SCORE " + std::to_string(defeated * pointsPerEnemy);
    }
}

inline void MagnetGameManager::OnUpdate()
{
    if (m_finished) return;

    m_remaining = static_cast<int>(scene.FindObjectsOfType<MagnetEnemyComponent>().size());
    m_timeRemaining = std::max(0.0f, m_timeRemaining - Time::deltaTime);
    UpdateHud();

    if (m_remaining <= 0) {
        FinishGame(true);
    } else if (m_timeRemaining <= 0.0f) {
        FinishGame(false);
    }
}

inline void MagnetGameManager::FinishGame(bool won)
{
    m_finished = true;

    const int defeated = m_totalEnemies - m_remaining;
    int score = defeated * pointsPerEnemy;
    if (won) score += static_cast<int>(m_timeRemaining) * timeBonusPerSecond;

    s_won      = won;
    s_score    = score;
    s_defeated = defeated;
    s_total    = m_totalEnemies;

    // WHY: 新規フェード処理を書かず、既存の Load シーン (auto transition + fade-in) を
    //      SceneManagerScript の static キャリア経由でそのまま再利用する。
    SceneManagerScript::s_next   = "Result";
    SceneManagerScript::s_fadeIn = true;
    scene.LoadScene("Load");
}

} // namespace sandbox
