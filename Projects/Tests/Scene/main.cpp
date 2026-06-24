// FBZZ Engine
// Tests/Scene/main.cpp
// Scene / GameObject モジュール単体テスト
// CreateGameObject / Find / Component CRUD / SceneView / 親子関係 / Destroy キューを検証する。
#include <cstdio>
#include <string>

#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Components/LifetimeComponent.hpp>
#include <Engine/Scene/Components/RigidBodyComponent.hpp>

#include "../TestHelper.hpp"

using namespace fbzz::scene;

// ─── CreateGameObject / Find ──────────────────────────────────────────────────

static void TestScene_CreateAndFind()
{
    std::printf("\n=== Scene: CreateGameObject / Find ===\n");

    Scene scene;

    // デフォルト名
    GameObject& go = scene.CreateGameObject("Player");
    check(scene.GameObjectCount() == 1, "Scene: GameObjectCount == 1 after Create");

    // Find by name
    GameObject* found = scene.Find("Player");
    check(found != nullptr, "Scene: Find('Player') returns non-null");
    check(found == &go,     "Scene: Find returns the same object");

    // 存在しない名前は nullptr
    check(scene.Find("NotExist") == nullptr,
          "Scene: Find for missing name returns nullptr");

    // 複数オブジェクト
    scene.CreateGameObject("Enemy");
    scene.CreateGameObject("Enemy");
    check(scene.GameObjectCount() == 3, "Scene: GameObjectCount == 3");

    const auto all = scene.FindAllWithTag("Untagged");
    check(all.size() == 3, "Scene: FindAllWithTag('Untagged') finds all 3");
}

// ─── Tag / Layer ──────────────────────────────────────────────────────────────

static void TestScene_TagLayer()
{
    std::printf("\n=== Scene: Tag / Layer ===\n");

    Scene scene;

    GameObject& a = scene.CreateGameObject("A");
    a.tag   = "Enemy";
    a.layer = 3;

    GameObject& b = scene.CreateGameObject("B");
    b.tag   = "Player";
    b.layer = 1;

    GameObject& c = scene.CreateGameObject("C");
    c.tag   = "Enemy";
    c.layer = 3;

    // FindWithTag
    GameObject* e = scene.FindWithTag("Enemy");
    check(e != nullptr, "Scene: FindWithTag('Enemy') returns non-null");

    // FindAllWithTag
    const auto enemies = scene.FindAllWithTag("Enemy");
    check(enemies.size() == 2, "Scene: FindAllWithTag('Enemy') returns 2");

    // FindWithLayer
    GameObject* lay3 = scene.FindWithLayer(3);
    check(lay3 != nullptr, "Scene: FindWithLayer(3) returns non-null");

    // FindAllWithLayer
    const auto layer3 = scene.FindAllWithLayer(3);
    check(layer3.size() == 2, "Scene: FindAllWithLayer(3) returns 2");

    // CompareTag
    check(a.CompareTag("Enemy"),  "GameObject: CompareTag('Enemy') == true");
    check(!a.CompareTag("Player"),"GameObject: CompareTag('Player') == false for Enemy tag");
}

// ─── Component CRUD ───────────────────────────────────────────────────────────

static void TestScene_Component()
{
    std::printf("\n=== Scene: Component CRUD ===\n");

    Scene scene;
    GameObject& go = scene.CreateGameObject("Obj");

    const EntityID id = go.GetID();

    // HasComponent: 初期は false (Scene 側 API を使う。GO には HasComponent がない)
    check(!scene.HasComponent<LifetimeComponent>(id),
          "Scene: HasComponent<Lifetime> == false before Add");

    // AddComponent → GetComponent
    LifetimeComponent lc;
    lc.remaining = 3.5f;
    lc.enabled   = true;
    go.AddComponent<LifetimeComponent>(lc);

    check(scene.HasComponent<LifetimeComponent>(id),
          "Scene: HasComponent<Lifetime> == true after Add");

    LifetimeComponent* ptr = go.GetComponent<LifetimeComponent>();
    check(ptr != nullptr, "Scene: GetComponent<Lifetime> returns non-null");
    checkF(ptr->remaining, "Scene: component field value preserved",
           ptr->remaining, "== 3.5");

    // 値変更が反映される
    ptr->remaining = 1.0f;
    check(go.GetComponent<LifetimeComponent>()->remaining == 1.0f,
          "Scene: component value change persists");

    // RemoveComponent
    go.RemoveComponent<LifetimeComponent>();
    check(!scene.HasComponent<LifetimeComponent>(id),
          "Scene: HasComponent == false after Remove");
    check(go.GetComponent<LifetimeComponent>() == nullptr,
          "Scene: GetComponent returns nullptr after Remove");
}

// ─── 複数コンポーネント / SceneView ───────────────────────────────────────────

static void TestScene_SceneView()
{
    std::printf("\n=== Scene: SceneView ===\n");

    Scene scene;

    // Lifetime を持つ 3 体と持たない 2 体を作る
    for (int i = 0; i < 3; ++i)
    {
        GameObject& go = scene.CreateGameObject("WithLifetime");
        LifetimeComponent lc;
        lc.remaining = static_cast<float>(i + 1);
        go.AddComponent<LifetimeComponent>(lc);
    }
    for (int i = 0; i < 2; ++i)
        scene.CreateGameObject("NoLifetime");

    // SceneView で LifetimeComponent を持つ Entity だけをイテレート
    int count = 0;
    float sumRemaining = 0.0f;
    for (auto [lifetime] : scene.View<LifetimeComponent>())
    {
        ++count;
        sumRemaining += lifetime.remaining;
    }

    checkF(static_cast<float>(count),
           "SceneView: iterates only 3 objects with Lifetime",
           static_cast<float>(count), "== 3");
    checkF(sumRemaining,
           "SceneView: sum of remaining == 6 (1+2+3)",
           sumRemaining, "== 6.0");
}

// ─── 親子関係 ─────────────────────────────────────────────────────────────────

static void TestScene_Hierarchy()
{
    std::printf("\n=== Scene: Parent / Child Hierarchy ===\n");

    Scene scene;
    GameObject& parent = scene.CreateGameObject("Parent");
    GameObject& child1 = scene.CreateGameObject("Child1");
    GameObject& child2 = scene.CreateGameObject("Child2");

    child1.SetParent(parent);
    child2.SetParent(parent);

    check(child1.GetParent() == &parent, "Hierarchy: child1.GetParent == parent");
    check(child2.GetParent() == &parent, "Hierarchy: child2.GetParent == parent");
    check(parent.GetChildCount() == 2,   "Hierarchy: parent has 2 children");
    check(parent.GetParent() == nullptr, "Hierarchy: parent has no parent");

    // IsDescendantOf
    check(child1.IsDescendantOf(parent),  "Hierarchy: child1 is descendant of parent");
    check(!parent.IsDescendantOf(child1), "Hierarchy: parent is NOT descendant of child1");

    // GetRootGameObjects: Parent だけがルートのはず (子は除外)
    const auto roots = scene.GetRootGameObjects();
    bool parentIsRoot = false;
    bool childIsRoot  = false;
    for (GameObject* g : roots) {
        if (g == &parent) parentIsRoot = true;
        if (g == &child1 || g == &child2) childIsRoot = true;
    }
    check(parentIsRoot, "Hierarchy: parent appears in GetRootGameObjects");
    check(!childIsRoot, "Hierarchy: children do NOT appear in GetRootGameObjects");

    // ClearParent
    child1.ClearParent();
    check(child1.GetParent() == nullptr, "Hierarchy: ClearParent removes parent");
    check(parent.GetChildCount() == 1,   "Hierarchy: parent has 1 child after ClearParent");
}

// ─── SetActive / activeInHierarchy ────────────────────────────────────────────

static void TestScene_Active()
{
    std::printf("\n=== Scene: SetActive / activeInHierarchy ===\n");

    Scene scene;
    GameObject& parent = scene.CreateGameObject("Parent");
    GameObject& child  = scene.CreateGameObject("Child");
    child.SetParent(parent);

    // デフォルト: 全て有効
    check(parent.activeSelf(),          "Active: parent.activeSelf == true by default");
    check(child.activeInHierarchy(),    "Active: child.activeInHierarchy == true by default");

    // 親を無効化 → 子も非アクティブになる
    parent.SetActive(false);
    check(!parent.activeSelf(),         "Active: parent.activeSelf == false after SetActive(false)");
    check(!child.activeInHierarchy(),   "Active: child.activeInHierarchy == false when parent inactive");
    check(child.activeSelf(),           "Active: child.activeSelf unchanged when parent disabled");

    // 親を再有効化
    parent.SetActive(true);
    check(child.activeInHierarchy(),    "Active: child.activeInHierarchy == true after parent re-enabled");

    // 子だけを無効化
    child.SetActive(false);
    check(parent.activeSelf(),          "Active: parent unaffected by child SetActive");
    check(!child.activeInHierarchy(),   "Active: child.activeInHierarchy == false after SetActive(false)");
}

// ─── Destroy キュー / FlushDestroyQueue ───────────────────────────────────────

static void TestScene_DestroyQueue()
{
    std::printf("\n=== Scene: Destroy Queue ===\n");

    Scene scene;
    GameObject& go = scene.CreateGameObject("Temp");
    const EntityID id = go.GetID();

    check(scene.IsValid(id), "Scene: IsValid == true before Destroy");
    check(scene.GameObjectCount() == 1, "Scene: count == 1 before Destroy");

    // 遅延 0 秒で破棄キューに積む
    GameObject::Destroy(go, 0.0f);

    // FlushDestroyQueue を呼ぶ前はまだ生きている
    check(scene.IsValid(id), "Scene: IsValid still true before Flush");

    // フラッシュ → 削除される
    scene.FlushDestroyQueue(1.0f / 60.0f);
    check(!scene.IsValid(id),           "Scene: IsValid == false after Flush");
    check(scene.GameObjectCount() == 0, "Scene: count == 0 after Flush");
}

// ─── Clear ────────────────────────────────────────────────────────────────────

static void TestScene_Clear()
{
    std::printf("\n=== Scene: Clear ===\n");

    Scene scene;
    scene.CreateGameObject("A");
    scene.CreateGameObject("B");
    scene.CreateGameObject("C");
    check(scene.GameObjectCount() == 3, "Scene: 3 objects before Clear");

    scene.Clear();
    check(scene.GameObjectCount() == 0, "Scene: GameObjectCount == 0 after Clear");
    check(scene.Find("A") == nullptr,   "Scene: Find returns nullptr after Clear");
}

// ─── GetGameObject / IsValid ──────────────────────────────────────────────────

static void TestScene_EntityId()
{
    std::printf("\n=== Scene: EntityID / GetGameObject ===\n");

    Scene scene;
    GameObject& go = scene.CreateGameObject("Lookup");
    const EntityID id = go.GetID();

    GameObject* looked = scene.GetGameObject(id);
    check(looked == &go, "Scene: GetGameObject(id) returns correct pointer");
    check(scene.IsValid(id), "Scene: IsValid(id) == true for live object");

    // 無効な EntityID は false
    EntityID invalid{ 9999, 0 };
    check(!scene.IsValid(invalid), "Scene: IsValid for bogus EntityID == false");
}

// ─── エントリポイント ─────────────────────────────────────────────────────────

int main()
{
    std::printf("FBZZ Scene Tests\n");
    std::printf("================\n");

    TestScene_CreateAndFind();
    TestScene_TagLayer();
    TestScene_Component();
    TestScene_SceneView();
    TestScene_Hierarchy();
    TestScene_Active();
    TestScene_DestroyQueue();
    TestScene_Clear();
    TestScene_EntityId();

    std::printf("\n================\n");
    std::printf("Results: %d passed, %d failed\n", g_passed, g_failed);

    return g_failed == 0 ? 0 : 1;
}
