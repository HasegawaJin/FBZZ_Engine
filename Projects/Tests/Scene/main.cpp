// FBZZ Engine
// Tests/Scene/main.cpp
// Scene / GameObject モジュール単体テスト
// CreateGameObject / Find / Component CRUD / SceneView / 親子関係 / Destroy キューを検証する。
#include <cstdio>
#include <string>

#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Components/LifetimeComponent.hpp>
#include <Engine/Scene/Components/RigidBodyComponent.hpp>
#include <Engine/Asset/VFXGraphAsset.hpp>
#include <Engine/Asset/VFXParameterRuntime.hpp>
#include <Engine/Asset/ParticleEmitterAssetCodec.hpp>

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

static void TestVFXGraph_ScheduleAndValidation()
{
    std::printf("\n=== VFX Graph: schedule / validation ===\n");
    fbzz::asset::VFXGraphAsset graph;
    graph.nodes.push_back({ .id = 1, .type = fbzz::asset::VFXNodeType::Entry,
                            .name = "Entry", .duration = 0.0f });
    graph.nodes.push_back({ .id = 2, .type = fbzz::asset::VFXNodeType::Particle,
                            .name = "Spark", .duration = 1.0f });
    graph.nodes.push_back({ .id = 3, .type = fbzz::asset::VFXNodeType::Delay,
                            .name = "Delay", .duration = 0.5f });
    graph.nodes.push_back({ .id = 4, .type = fbzz::asset::VFXNodeType::Light,
                            .name = "Flash", .duration = 0.2f });
    graph.links = { { 1, 2 }, { 2, 3 }, { 3, 4 } };

    std::vector<float> starts;
    float duration = 0.0f;
    std::string error;
    check(fbzz::asset::ValidateVFXGraphAsset(graph, &error),
          "VFX Graph: valid DAG is accepted");
    check(fbzz::asset::BuildVFXGraphSchedule(graph, starts, duration, &error),
          "VFX Graph: schedule builds");
    check(starts.size() == 4 && starts[1] == 0.0f && starts[2] == 1.0f
          && starts[3] == 1.5f,
          "VFX Graph: sequential links accumulate duration");
    check(duration > 1.69f && duration < 1.71f,
          "VFX Graph: graph duration includes final node");

    graph.links[0].trigger = fbzz::asset::VFXLinkTrigger::OnStart;
    graph.links[0].delay = 0.25f;
    check(fbzz::asset::BuildVFXGraphSchedule(graph, starts, duration, &error)
          && starts[1] == 0.25f,
          "VFX Graph: OnStart link and event delay affect schedule");
    const auto budget = fbzz::asset::CalculateVFXGraphBudget(graph);
    check(budget.particles == 300 && budget.lights == 1 && budget.audioVoices == 0,
          "VFX Graph: authoring budget is calculated");

    // On CollisionはParticleノードだけが発火元になれる。Entryを発火元にすると保存を拒否する。
    graph.links[0].trigger = fbzz::asset::VFXLinkTrigger::OnCollision;
    check(!fbzz::asset::ValidateVFXGraphAsset(graph, &error),
          "VFX Graph: On Collision rejects a non-Particle source");
    graph.links[0].trigger = fbzz::asset::VFXLinkTrigger::OnStart;

    graph.links[1].trigger = fbzz::asset::VFXLinkTrigger::OnDeath;
    check(fbzz::asset::ValidateVFXGraphAsset(graph, &error),
          "VFX Graph: On Death accepts a Particle source");
    graph.links[1].trigger = fbzz::asset::VFXLinkTrigger::OnComplete;

    fbzz::scene::ParticleEmitter source;
    source.simulationMode = fbzz::scene::ParticleSimulationMode::Gpu;
    source.meshShapePath = "Assets/Meshes/Fire.fbx";
    source.useSizeCurve = true;
    source.sizeCurve.keyCount = 3;
    source.sizeCurve.keys[1] = { 0.4f, 2.0f };
    source.bursts = { { 0.1f, 64, 3, 0.2f, 0.75f } };
    source.distortion = true;
    source.sixWayLighting = true;
    source.motionVectorFlipbook = true;
    source.motionVectorTexturePath = "Assets/VFX/ExplosionMotion.tex";
    source.meshParticlePath = "Assets/Meshes/Shard.fbx";
    source.collisionMode = fbzz::scene::ParticleCollisionMode::Depth;
    const toml::table particleTable = fbzz::asset::SerializeParticleEmitterSettings(source);
    fbzz::scene::ParticleEmitter restored;
    fbzz::asset::DeserializeParticleEmitterSettings(particleTable, restored);
    check(restored.simulationMode == fbzz::scene::ParticleSimulationMode::Gpu
          && restored.meshShapePath == source.meshShapePath
          && restored.sizeCurve.keyCount == 3
          && restored.bursts.size() == 1 && restored.bursts[0].count == 64
           && restored.distortion && restored.sixWayLighting && restored.motionVectorFlipbook
           && restored.motionVectorTexturePath == source.motionVectorTexturePath
           && restored.meshParticlePath == source.meshParticlePath
           && restored.collisionMode == fbzz::scene::ParticleCollisionMode::Depth,
          "VFX Graph: full Particle modules survive asset round-trip");

    graph.links.push_back({ 4, 2 });
    check(!fbzz::asset::ValidateVFXGraphAsset(graph, &error),
          "VFX Graph: cycle is rejected");
}

static void TestVFXGraph_ParametersAndSchema()
{
    std::printf("\n=== VFX Graph: parameters / schema ===\n");
    fbzz::asset::VFXGraphAsset graph;
    graph.nodes.push_back({ .id = 1, .type = fbzz::asset::VFXNodeType::Entry,
                            .name = "Entry", .duration = 0.0f });
    graph.nodes.push_back({ .id = 2, .type = fbzz::asset::VFXNodeType::Particle,
                            .name = "Particle", .duration = 1.0f });
    graph.links.push_back({ 1, 2 });
    fbzz::asset::VFXParamDefinition intensity;
    intensity.name = "Intensity";
    intensity.type = fbzz::asset::VFXParamType::Float;
    intensity.defaultValue.source = fbzz::asset::VFXConstant{ 48.0f };
    graph.parameters.push_back(intensity);
    graph.bindings.push_back({ "Intensity", 2, "particle.emitRate" });
    fbzz::asset::VFXVariantSet largeVariant;
    largeVariant.name = "Large";
    fbzz::asset::VFXParamValue largeIntensity;
    largeIntensity.source = fbzz::asset::VFXConstant{ 96.0f };
    largeVariant.overrides.push_back({ "Intensity", largeIntensity });
    graph.variants.push_back(std::move(largeVariant));
    graph.signalNodes.push_back({ .id = 1, .operation = fbzz::asset::VFXSignalOperation::Time });
    graph.signalOutputs.push_back({ "Pulse", 1 });
    std::string error;
    check(fbzz::asset::ValidateVFXGraphAsset(graph, &error),
          "VFX Graph: schemaPath binding validates");

    fbzz::scene::VFXGraphComponent component;
    fbzz::asset::VFXGraphNode node = graph.nodes[1];
    fbzz::asset::ApplyVFXBindings(graph, component, node, 0.0f);
    check(node.particle.emitRate == 48.0f,
          "VFX Graph: default parameter applies through schema");

    fbzz::asset::VFXParamValue overrideValue;
    overrideValue.source = fbzz::asset::VFXConstant{ 96.0f };
    component.parameterOverrides.push_back({ "Intensity", overrideValue });
    node = graph.nodes[1];
    fbzz::asset::ApplyVFXBindings(graph, component, node, 0.0f);
    check(node.particle.emitRate == 96.0f,
          "VFX Graph: instance override wins over default");

    const std::string encoded = fbzz::asset::SerializeVFXOverrides(component.parameterOverrides);
    std::vector<fbzz::asset::VFXParamOverride> decoded;
    check(fbzz::asset::DeserializeVFXOverrides(encoded, decoded)
          && decoded.size() == 1
          && std::get<float>(std::get<fbzz::asset::VFXConstant>(decoded[0].value.source)) == 96.0f,
          "VFX Graph: component overrides survive scene string codec");

    fbzz::asset::VFXParamValue attributeValue;
    attributeValue.source = fbzz::asset::VFXAttributeRef{ "self.physics.speed" };
    fbzz::asset::VFXParamValue signalValue;
    signalValue.source = fbzz::asset::VFXSignalRef{ "Pulse" };
    const std::string dynamicEncoded = fbzz::asset::SerializeVFXOverrides({
        { "Speed", attributeValue }, { "PulseValue", signalValue }
    });
    check(fbzz::asset::DeserializeVFXOverrides(dynamicEncoded, decoded)
          && decoded.size() == 2
          && std::get<fbzz::asset::VFXAttributeRef>(decoded[0].value.source).path == "self.physics.speed"
          && std::get<fbzz::asset::VFXSignalRef>(decoded[1].value.source).signalName == "Pulse",
          "VFX Graph: Attribute/Signal sources survive scene string codec");

    graph.bindings[0].schemaPath = "particle.missingField";
    check(!fbzz::asset::ValidateVFXGraphAsset(graph, &error),
          "VFX Graph: unknown schemaPath is rejected");
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
    TestVFXGraph_ScheduleAndValidation();
    TestVFXGraph_ParametersAndSchema();

    std::printf("\n================\n");
    std::printf("Results: %d passed, %d failed\n", g_passed, g_failed);

    return g_failed == 0 ? 0 : 1;
}
