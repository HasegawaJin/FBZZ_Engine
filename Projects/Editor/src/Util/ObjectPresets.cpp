// FBZZ Engine
// ObjectPresets.cpp | fbzz::editor
// Add Object プリセットの実体。SceneHierarchyPanel のメニューと AI (preset.create) が同じ表を使う。
#include <Editor/Util/ObjectPresets.hpp>

#include <Editor/EditorContext.hpp>
#include <Editor/Util/ColliderFit.hpp>
#include <Editor/Util/TerrainWaterDefaults.hpp>
// 全コンポーネント型の登録表。個別 include を並べるより、追加時に漏れが出ない。
#include <Engine/Scene/ComponentRegistry.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/ScriptComponent.hpp>
#include <Engine/Scene/ScriptFactory.hpp>
#include <Engine/Renderer/PrimitiveMesh.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>

#include <array>
#include <iterator>
#include <utility>

namespace fbzz::editor {

namespace {

// ── プリミティブ ────────────────────────────────────────────────────────────

enum class PrimitiveKind { Cube, Sphere, Plane, Quad, Cylinder, Cone, Torus, Capsule };

renderer::Mesh* PrimitiveMeshFor(PrimitiveKind kind)
{
    auto* resources = renderer::ResourceManager::Active();
    if (resources == nullptr) return nullptr;
    switch (kind) {
    case PrimitiveKind::Cube:     return renderer::PrimitiveMesh::Cube(*resources);
    case PrimitiveKind::Sphere:   return renderer::PrimitiveMesh::Sphere(*resources);
    case PrimitiveKind::Plane:    return renderer::PrimitiveMesh::Plane(*resources);
    case PrimitiveKind::Quad:     return renderer::PrimitiveMesh::Quad(*resources);
    case PrimitiveKind::Cylinder: return renderer::PrimitiveMesh::Cylinder(*resources);
    case PrimitiveKind::Cone:     return renderer::PrimitiveMesh::Cone(*resources);
    case PrimitiveKind::Torus:    return renderer::PrimitiveMesh::Torus(*resources);
    case PrimitiveKind::Capsule:  return renderer::PrimitiveMesh::Capsule(*resources);
    }
    return nullptr;
}

const char* PrimitiveMeshPath(PrimitiveKind kind)
{
    switch (kind) {
    case PrimitiveKind::Cube:     return "primitive:cube";
    case PrimitiveKind::Sphere:   return "primitive:sphere";
    case PrimitiveKind::Plane:    return "primitive:plane";
    case PrimitiveKind::Quad:     return "primitive:quad";
    case PrimitiveKind::Cylinder: return "primitive:cylinder";
    case PrimitiveKind::Cone:     return "primitive:cone";
    case PrimitiveKind::Torus:    return "primitive:torus";
    case PrimitiveKind::Capsule:  return "primitive:capsule";
    }
    return "";
}

// WHY: 固定寸法テンプレートだとメッシュの実寸 (Capsule 等) とズレる。
//      colliderfit がアタッチ済みメッシュの bounds から寸法を自動計算するため、
//      プリミティブの形状を変更してもここは修正不要になる。
void AttachFittedCollider(scene::GameObject& go, PrimitiveKind kind)
{
    switch (kind) {
    case PrimitiveKind::Sphere:
        go.AddComponent<scene::SphereColliderComponent>(colliderfit::MakeFittedSphereCollider(go));
        break;
    case PrimitiveKind::Capsule:
        go.AddComponent<scene::CapsuleColliderComponent>(colliderfit::MakeFittedCapsuleCollider(go));
        break;
    case PrimitiveKind::Cylinder:
        go.AddComponent<scene::CylinderColliderComponent>(colliderfit::MakeFittedCylinderCollider(go));
        break;
    default:
        go.AddComponent<scene::BoxColliderComponent>(colliderfit::MakeFittedBoxCollider(go));
        break;
    }
}

// 描画に必要な MeshRenderer + MaterialComponent を付ける。コライダーは呼び出し側の選択。
scene::GameObject& NewPrimitive(EditorContext& ctx, const char* name, PrimitiveKind kind,
                                bool withCollider = true)
{
    auto& go = ctx.activeScene->CreateGameObject(name);

    scene::MeshRenderer mr;
    mr.meshPath = PrimitiveMeshPath(kind);
    mr.mesh     = PrimitiveMeshFor(kind);
    go.AddComponent<scene::MeshRenderer>(mr);

    scene::MaterialComponent mc;
    mc.materialPath = "Assets/Materials/Surface/Lit.mat";
    go.AddComponent<scene::MaterialComponent>(std::move(mc));

    if (withCollider) AttachFittedCollider(go, kind);
    return go;
}

// Quad だけは常にカメラを向く必要があるため、専用スクリプトを付ける (従来の挙動を維持)。
void AttachQuadBillboardScript(EditorContext& ctx, scene::GameObject& go)
{
    auto script = scene::ScriptFactory::Create("QuadBillboardComponent");
    if (!script) return;
    script->SetContext(ctx.activeScene, &go);
    script->Reset();
    script->OnValidate();
    scene::ScriptComponent sc;
    scene::ScriptEntry entry;
    entry.script = std::move(script);
    sc.scripts.emplace_back(std::move(entry));
    go.AddComponent<scene::ScriptComponent>(std::move(sc));
}

// ── 生成関数 ────────────────────────────────────────────────────────────────
// 表 (kPresets) から関数ポインタで呼ぶため、すべて同じシグネチャにする。

scene::GameObject* MakeEmpty(EditorContext& ctx)
{
    return &ctx.activeScene->CreateGameObject("GameObject");
}

scene::GameObject* MakeCube(EditorContext& ctx)     { return &NewPrimitive(ctx, "Cube", PrimitiveKind::Cube); }
scene::GameObject* MakeSphere(EditorContext& ctx)   { return &NewPrimitive(ctx, "Sphere", PrimitiveKind::Sphere); }
scene::GameObject* MakePlane(EditorContext& ctx)    { return &NewPrimitive(ctx, "Plane", PrimitiveKind::Plane); }
scene::GameObject* MakeCylinder(EditorContext& ctx) { return &NewPrimitive(ctx, "Cylinder", PrimitiveKind::Cylinder); }
scene::GameObject* MakeCone(EditorContext& ctx)     { return &NewPrimitive(ctx, "Cone", PrimitiveKind::Cone); }
scene::GameObject* MakeTorus(EditorContext& ctx)    { return &NewPrimitive(ctx, "Torus", PrimitiveKind::Torus); }
scene::GameObject* MakeCapsule(EditorContext& ctx)  { return &NewPrimitive(ctx, "Capsule", PrimitiveKind::Capsule); }

scene::GameObject* MakeQuad(EditorContext& ctx)
{
    auto& go = NewPrimitive(ctx, "Quad", PrimitiveKind::Quad, /*withCollider=*/false);
    AttachQuadBillboardScript(ctx, go);
    AttachFittedCollider(go, PrimitiveKind::Quad);
    return &go;
}

scene::GameObject* MakeLight(EditorContext& ctx, const char* name, scene::LightComponent::Type type)
{
    auto& go = ctx.activeScene->CreateGameObject(name);
    scene::LightComponent light;
    light.type = type;
    if (type == scene::LightComponent::Type::Point) {
        light.intensity = 4.0f;
        light.range     = 8.0f;
    } else if (type == scene::LightComponent::Type::Spot) {
        light.intensity = 5.0f;
        light.range     = 12.0f;
    }
    go.AddComponent<scene::LightComponent>(light);
    return &go;
}

scene::GameObject* MakeDirectionalLight(EditorContext& ctx) { return MakeLight(ctx, "Directional Light", scene::LightComponent::Type::Directional); }
scene::GameObject* MakePointLight(EditorContext& ctx)       { return MakeLight(ctx, "Point Light", scene::LightComponent::Type::Point); }
scene::GameObject* MakeSpotLight(EditorContext& ctx)        { return MakeLight(ctx, "Spot Light", scene::LightComponent::Type::Spot); }

scene::GameObject* MakeCamera(EditorContext& ctx)
{
    auto& go = ctx.activeScene->CreateGameObject("Camera");
    go.transform.position = { 0.0f, 2.0f, -5.0f };
    go.AddComponent<scene::CameraComponent>();
    // WHY Listener も付けるか: Listener が 1 つも無いシーンでは 3D 音の距離減衰が
    //     まるごと効かないのに、エラーも警告も出ない。カメラは受聴点として最も
    //     自然な既定で、複数あっても AudioListener::priority で選ばれる。
    go.AddComponent<scene::AudioListenerComponent>();
    return &go;
}

// ── Rendering ──────────────────────────────────────────────────────────────

scene::GameObject* MakeSprite(EditorContext& ctx)
{
    auto& go = ctx.activeScene->CreateGameObject("Sprite");
    go.AddComponent<scene::SpriteRendererComponent>();
    return &go;
}

scene::GameObject* MakeLineRenderer(EditorContext& ctx)
{
    auto& go = ctx.activeScene->CreateGameObject("Line");
    scene::LineRendererComponent line;
    // WHY 2 点を入れるか: points が空だと何も描かれず、置いた直後は「壊れている」ようにしか
    //      見えない。始点と終点があれば Scene View に線が出て、そこから編集を始められる。
    line.points = { math::Vector3{ 0.0f, 0.0f, 0.0f }, math::Vector3{ 0.0f, 0.0f, 3.0f } };
    go.AddComponent<scene::LineRendererComponent>(std::move(line));
    return &go;
}

scene::GameObject* MakeBillboard(EditorContext& ctx)
{
    auto& go = NewPrimitive(ctx, "Billboard", PrimitiveKind::Quad, /*withCollider=*/false);
    go.AddComponent<scene::BillboardComponent>();
    return &go;
}

scene::GameObject* MakeProjector(EditorContext& ctx)
{
    auto& go = ctx.activeScene->CreateGameObject("Projector");
    go.AddComponent<scene::ProjectorComponent>();
    return &go;
}

scene::GameObject* MakeLODGroup(EditorContext& ctx)
{
    auto& go = ctx.activeScene->CreateGameObject("LOD Group");
    go.AddComponent<scene::LODGroupComponent>();
    return &go;
}

// ── Environment ────────────────────────────────────────────────────────────

scene::GameObject* MakeSky(EditorContext& ctx)
{
    auto& go = ctx.activeScene->CreateGameObject("Sky");
    go.AddComponent<scene::SkyRenderer>();
    return &go;
}

scene::GameObject* MakeSunMoon(EditorContext& ctx)
{
    auto& go = ctx.activeScene->CreateGameObject("Sun & Moon");
    go.AddComponent<scene::SunMoonRenderer>();
    return &go;
}

scene::GameObject* MakeAtmosphere(EditorContext& ctx)
{
    auto& go = ctx.activeScene->CreateGameObject("Atmospheric Scattering");
    go.AddComponent<scene::AtmosphericScatteringComponent>();
    return &go;
}

scene::GameObject* MakeEnvironmentLight(EditorContext& ctx)
{
    auto& go = ctx.activeScene->CreateGameObject("Environment Light");
    go.AddComponent<scene::EnvironmentLightComponent>();
    return &go;
}

scene::GameObject* MakeVolumetricCloud(EditorContext& ctx)
{
    auto& go = ctx.activeScene->CreateGameObject("Volumetric Cloud");
    go.AddComponent<scene::VolumetricCloudComponent>();
    return &go;
}

scene::GameObject* MakePostProcessVolume(EditorContext& ctx)
{
    auto& go = ctx.activeScene->CreateGameObject("Post Process Volume");
    auto& ppv = go.AddComponent<scene::PostProcessVolumeComponent>();

    // プロジェクト同梱の既定プロファイルを最初から挿しておく。
    // WHY: 未アサインのボリュームは何も適用しないため、置いた直後は
    //      「追加したのに何も起きない」状態になる。テンプレートが必ず持っている
    //      既定プロファイルを指しておけば、その場で値をいじって効果を確認できる。
    //      パスが無いプロジェクトでは単に解決に失敗し、Inspector が警告を出す。
    ppv.profile.ref.path = "Assets/PostProcess/DefaultPostProcess.fzdata";
    return &go;
}

scene::GameObject* MakeReflectionProbe(EditorContext& ctx)
{
    auto& go = ctx.activeScene->CreateGameObject("Reflection Probe");
    go.AddComponent<scene::ReflectionProbeComponent>();
    return &go;
}

scene::GameObject* MakeWindZone(EditorContext& ctx)
{
    auto& go = ctx.activeScene->CreateGameObject("Wind Zone");
    go.AddComponent<scene::WindZoneComponent>();
    return &go;
}

// 空・太陽・大気・IBL をまとめた 1 個の環境ルート。
// WHY: 屋外シーンはこの 4 つが揃って初めて成立する。1 個ずつ置く導線しか無いと、
//      どれか 1 つを忘れた状態 (太陽はあるが空が黒い等) が普通に起きる。
scene::GameObject* MakeSkySystem(EditorContext& ctx)
{
    auto& go = ctx.activeScene->CreateGameObject("Sky System");
    go.AddComponent<scene::SkyRenderer>();
    go.AddComponent<scene::SunMoonRenderer>();
    go.AddComponent<scene::AtmosphericScatteringComponent>();
    go.AddComponent<scene::EnvironmentLightComponent>();
    return &go;
}

// ── Terrain / Water ────────────────────────────────────────────────────────

scene::GameObject* MakeTerrain(EditorContext& ctx)
{
    auto& go = ctx.activeScene->CreateGameObject("Terrain");
    scene::TerrainComponent terrain;
    terrain.InitFlat(0.0f);
    // レイヤーマテリアルの既定は Map Editor のセル生成と同じ表を使う (見た目を揃える)。
    for (int layer = 0; layer < 4; ++layer)
        terrain.layerMaterials[static_cast<size_t>(layer)] = DefaultTerrainLayerMaterialPath(layer);
    terrain.heightDirty   = true;
    terrain.colliderDirty = true;
    go.AddComponent<scene::TerrainComponent>(std::move(terrain));
    go.AddComponent<scene::TerrainColliderComponent>();
    return &go;
}

scene::GameObject* MakeTerrainGrid(EditorContext& ctx)
{
    auto& go = ctx.activeScene->CreateGameObject("Terrain Grid");
    go.AddComponent<scene::TerrainGridComponent>();
    return &go;
}

scene::GameObject* MakeWater(EditorContext& ctx)
{
    auto& go = ctx.activeScene->CreateGameObject("Water");
    scene::WaterComponent water{};
    water.resolutionX  = 96;
    water.resolutionZ  = 96;
    water.extentX      = 80.0f;
    water.extentZ      = 80.0f;
    water.chunkCount   = 4;
    water.materialPath = DefaultWaterMaterialPath();
    water.meshDirty    = true;
    water.foamDirty    = true;
    water.texDirty     = true;
    go.AddComponent<scene::WaterComponent>(std::move(water));
    return &go;
}

// ── Navigation / AI ────────────────────────────────────────────────────────

scene::GameObject* MakeNavMeshSurface(EditorContext& ctx)
{
    auto& go = ctx.activeScene->CreateGameObject("NavMesh Surface");
    // WHY needsBake を立てないか: NavMeshSurfaceComponent は「AddComponent 直後に自動ベイクが
    //      走ってエディタが固まるのを防ぐため」既定 false と決められている。置いた直後は
    //      polygons が空なので、ベイクは Tools > Navigation / Inspector / navmesh_bake から
    //      明示的に実行する。
    go.AddComponent<scene::NavMeshSurfaceComponent>();
    return &go;
}

scene::GameObject* MakeNavMeshAgent(EditorContext& ctx)
{
    auto& go = NewPrimitive(ctx, "NavMesh Agent", PrimitiveKind::Capsule, /*withCollider=*/false);
    go.AddComponent<scene::NavMeshAgentComponent>();
    return &go;
}

scene::GameObject* MakeNavMeshModifier(EditorContext& ctx)
{
    auto& go = NewPrimitive(ctx, "NavMesh Modifier", PrimitiveKind::Cube, /*withCollider=*/true);
    go.AddComponent<scene::NavMeshModifierComponent>();
    return &go;
}

scene::GameObject* MakeOffMeshLink(EditorContext& ctx)
{
    auto& go = ctx.activeScene->CreateGameObject("Off-Mesh Link");
    go.AddComponent<scene::NavMeshOffMeshLinkComponent>();
    return &go;
}

// 巡回する敵の骨格。Agent + Patrol + Sensor + BehaviorTree を 1 セットで置く。
// WHY: この 4 つは「敵 1 体」として常に一緒に要る。個別に足す導線しか無いと、
//      Sensor だけ忘れて「BT の条件が永久に偽」という最も追いにくい壊れ方をする。
scene::GameObject* MakeAIAgent(EditorContext& ctx)
{
    auto& go = NewPrimitive(ctx, "AI Agent", PrimitiveKind::Capsule, /*withCollider=*/false);
    go.AddComponent<scene::NavMeshAgentComponent>();
    go.AddComponent<scene::NavMeshSensorComponent>();
    go.AddComponent<scene::NavMeshPatrolComponent>();
    go.AddComponent<scene::BehaviorTreeComponent>();
    return &go;
}

// ── Effects ────────────────────────────────────────────────────────────────

scene::GameObject* MakeParticleEmitter(EditorContext& ctx)
{
    auto& go = ctx.activeScene->CreateGameObject("Particle Emitter");
    go.AddComponent<scene::ParticleEmitter>();
    return &go;
}

scene::GameObject* MakeParticleForceField(EditorContext& ctx)
{
    auto& go = ctx.activeScene->CreateGameObject("Particle Force Field");
    go.AddComponent<scene::ParticleForceField>();
    return &go;
}

scene::GameObject* MakeVFXGraph(EditorContext& ctx)
{
    auto& go = ctx.activeScene->CreateGameObject("VFX Graph");
    go.AddComponent<scene::VFXGraphComponent>();
    return &go;
}

scene::GameObject* MakeTrail(EditorContext& ctx)
{
    auto& go = ctx.activeScene->CreateGameObject("Trail");
    go.AddComponent<scene::TrailComponent>();
    return &go;
}

scene::GameObject* MakeMeshTrail(EditorContext& ctx)
{
    auto& go = ctx.activeScene->CreateGameObject("Mesh Trail");
    go.AddComponent<scene::MeshTrailComponent>();
    return &go;
}

// デカール投影ボリューム: X/Z が投影面サイズ、Y が投影深度
scene::GameObject* MakeDecal(EditorContext& ctx, const char* name, float sizeXZ, float depth)
{
    auto& go = ctx.activeScene->CreateGameObject(name);
    go.transform.scale = { sizeXZ, depth, sizeXZ };
    go.AddComponent<scene::DecalComponent>();
    return &go;
}

scene::GameObject* MakeDecalMedium(EditorContext& ctx) { return MakeDecal(ctx, "Decal", 2.0f, 0.5f); }
scene::GameObject* MakeDecalSmall(EditorContext& ctx)  { return MakeDecal(ctx, "Decal (Small)", 1.0f, 0.3f); }
scene::GameObject* MakeDecalLarge(EditorContext& ctx)  { return MakeDecal(ctx, "Decal (Large)", 5.0f, 1.0f); }

// ── Physics ────────────────────────────────────────────────────────────────

scene::GameObject* MakeRigidBodyCube(EditorContext& ctx)
{
    auto& go = NewPrimitive(ctx, "Rigid Body", PrimitiveKind::Cube);
    go.transform.position = { 0.0f, 3.0f, 0.0f };
    go.AddComponent<scene::RigidBodyComponent>();
    return &go;
}

scene::GameObject* MakeCharacterController(EditorContext& ctx)
{
    auto& go = NewPrimitive(ctx, "Character", PrimitiveKind::Capsule);
    go.AddComponent<scene::CharacterControllerComponent>();
    return &go;
}

scene::GameObject* MakeTriggerVolume(EditorContext& ctx)
{
    auto& go = ctx.activeScene->CreateGameObject("Trigger");
    scene::BoxColliderComponent box;
    box.isTrigger = true;
    go.AddComponent<scene::BoxColliderComponent>(std::move(box));
    return &go;
}

scene::GameObject* MakeForceVolume(EditorContext& ctx)
{
    auto& go = ctx.activeScene->CreateGameObject("Force Volume");
    go.AddComponent<scene::VolumeComponent>();
    return &go;
}

// ── Audio ──────────────────────────────────────────────────────────────────

scene::GameObject* MakeAudioSource(EditorContext& ctx)
{
    auto& go = ctx.activeScene->CreateGameObject("Audio Source");
    go.AddComponent<scene::AudioSourceComponent>();
    return &go;
}

scene::GameObject* MakeAudioListener(EditorContext& ctx)
{
    auto& go = ctx.activeScene->CreateGameObject("Audio Listener");
    go.AddComponent<scene::AudioListenerComponent>();
    return &go;
}

// ── Spline ─────────────────────────────────────────────────────────────────

scene::GameObject* MakeSpline(EditorContext& ctx)
{
    auto& go = ctx.activeScene->CreateGameObject("Spline");
    scene::SplineComponent spline;
    // 点が無いと Scene View に何も出ず編集の起点が無いため、直線 3 点を入れておく。
    spline.points = {
        math::Vector3{ 0.0f, 0.0f, 0.0f },
        math::Vector3{ 5.0f, 0.0f, 0.0f },
        math::Vector3{ 10.0f, 0.0f, 0.0f },
    };
    go.AddComponent<scene::SplineComponent>(std::move(spline));
    return &go;
}

scene::GameObject* MakeSplineFollower(EditorContext& ctx)
{
    auto& go = NewPrimitive(ctx, "Spline Follower", PrimitiveKind::Cube, /*withCollider=*/false);
    go.AddComponent<scene::SplineFollowerComponent>();
    return &go;
}

// ── UI ─────────────────────────────────────────────────────────────────────

scene::GameObject* MakeUICanvas(EditorContext& ctx)
{
    auto& go = ctx.activeScene->CreateGameObject("Canvas");
    go.AddComponent<scene::UICanvas>();
    // 編集対象 Canvas を確定させる (UIViewport の操作対象と一致させる)。
    ctx.activeUICanvas = go.GetID();
    return &go;
}

// UIImage のみのシンプルな画像要素。サイズは transform.scale.xy で制御する。
scene::GameObject* MakeUIImageObject(EditorContext& ctx, const char* name,
                                     const math::Vector4& color, float w, float h)
{
    auto& go = ctx.activeScene->CreateGameObject(name);
    go.transform.scale = { w, h, 1.0f };
    scene::UIImage img;
    img.color = color;
    go.AddComponent<scene::UIImage>(img);
    return &go;
}

scene::GameObject* MakeUIImage(EditorContext& ctx)
{
    return MakeUIImageObject(ctx, "Image", { 1.0f, 1.0f, 1.0f, 1.0f }, 100.0f, 100.0f);
}

// 半透明グレーで canvas 全体を覆う背景パネル
scene::GameObject* MakeUIPanel(EditorContext& ctx)
{
    return MakeUIImageObject(ctx, "Panel", { 0.20f, 0.20f, 0.20f, 0.80f }, 1920.0f, 1080.0f);
}

scene::GameObject* MakeUIText(EditorContext& ctx)
{
    auto& go = ctx.activeScene->CreateGameObject("Text");
    scene::UIText txt;
    txt.text     = "Text";
    txt.fontSize = 42.0f;
    txt.color    = { 1.0f, 1.0f, 1.0f, 1.0f };
    go.AddComponent<scene::UIText>(txt);
    return &go;
}

// UIButton: 背景 Image + Button コンポーネントを親に、ラベル Text を子として持つ標準構成。
scene::GameObject* MakeUIButton(EditorContext& ctx)
{
    auto& go = ctx.activeScene->CreateGameObject("Button");
    const scene::EntityID buttonId = go.GetID();
    go.transform.scale = { 160.0f, 40.0f, 1.0f };

    scene::UIImage img;
    img.color = { 0.90f, 0.90f, 0.90f, 1.0f };
    go.AddComponent<scene::UIImage>(img);
    go.AddComponent<scene::UIButton>();

    // ラベル: 暗めテキストで中央配置 (位置は inspector で調整)
    auto& label = ctx.activeScene->CreateGameObject("Label");
    scene::UIText txt;
    txt.text     = "Button";
    txt.fontSize = 24.0f;
    txt.color    = { 0.20f, 0.20f, 0.20f, 1.0f };
    label.AddComponent<scene::UIText>(txt);
    // WHY 引き直すか: CreateGameObject は GameObject 配列を再確保し得るため、
    //      2 個目を作った後の go 参照は使わない。
    if (auto* button = ctx.activeScene->GetGameObject(buttonId)) label.SetParent(button);

    return ctx.activeScene->GetGameObject(buttonId);
}

scene::GameObject* MakeUILayoutGroup(EditorContext& ctx, const char* name, scene::UILayoutAxis axis)
{
    auto& go = ctx.activeScene->CreateGameObject(name);
    scene::UILayoutGroup layout;
    layout.axis    = axis;
    layout.spacing = 8.0f;
    go.AddComponent<scene::UILayoutGroup>(layout);
    return &go;
}

scene::GameObject* MakeUIHorizontalLayout(EditorContext& ctx)
{
    return MakeUILayoutGroup(ctx, "Horizontal Layout Group", scene::UILayoutAxis::Horizontal);
}

scene::GameObject* MakeUIVerticalLayout(EditorContext& ctx)
{
    return MakeUILayoutGroup(ctx, "Vertical Layout Group", scene::UILayoutAxis::Vertical);
}

// ── 登録表 ──────────────────────────────────────────────────────────────────
// 並び順がそのままメニューの並びになる。カテゴリは連続して並べること。
constexpr ObjectPreset kPresets[] = {
    { "empty", "", "Empty", "コンポーネントを持たない空の GameObject", &MakeEmpty },

    { "3d.cube",     "3D Object", "Cube",     "MeshRenderer(cube) + Lit マテリアル + Box Collider", &MakeCube },
    { "3d.sphere",   "3D Object", "Sphere",   "MeshRenderer(sphere) + Lit マテリアル + Sphere Collider", &MakeSphere },
    { "3d.plane",    "3D Object", "Plane",    "MeshRenderer(plane) + Lit マテリアル + Box Collider", &MakePlane },
    { "3d.quad",     "3D Object", "Quad",     "MeshRenderer(quad) + Lit マテリアル + Box Collider + カメラを向くスクリプト", &MakeQuad },
    { "3d.cylinder", "3D Object", "Cylinder", "MeshRenderer(cylinder) + Lit マテリアル + Cylinder Collider", &MakeCylinder },
    { "3d.cone",     "3D Object", "Cone",     "MeshRenderer(cone) + Lit マテリアル + Box Collider", &MakeCone },
    { "3d.torus",    "3D Object", "Torus",    "MeshRenderer(torus) + Lit マテリアル + Box Collider", &MakeTorus },
    { "3d.capsule",  "3D Object", "Capsule",  "MeshRenderer(capsule) + Lit マテリアル + Capsule Collider", &MakeCapsule },

    { "rendering.sprite",    "Rendering", "Sprite",    "SpriteRenderer。spritePath を設定して 2D 画像を表示する", &MakeSprite },
    { "rendering.line",      "Rendering", "Line",      "LineRenderer。始点と終点の 2 点を入れた状態で生成する", &MakeLineRenderer },
    { "rendering.billboard", "Rendering", "Billboard", "Quad メッシュ + Billboard。常にカメラを向く板", &MakeBillboard },
    { "rendering.projector", "Rendering", "Projector", "Projector。テクスチャを面へ投影する", &MakeProjector },
    { "rendering.lodGroup",  "Rendering", "LOD Group", "LODGroup。子オブジェクトを距離で切り替える親", &MakeLODGroup },

    { "light.directional", "Light", "Directional Light", "太陽光。向きだけが効き、位置は影響しない", &MakeDirectionalLight },
    { "light.point",       "Light", "Point Light",       "点光源 (intensity 4 / range 8)", &MakePointLight },
    { "light.spot",        "Light", "Spot Light",        "スポットライト (intensity 5 / range 12)", &MakeSpotLight },

    { "camera", "", "Camera", "CameraComponent。Game View の描画元になる", &MakeCamera },

    { "env.skySystem",        "Environment", "Sky System",             "Sky + Sun/Moon + 大気散乱 + 環境光(IBL) をまとめた屋外シーンの環境ルート", &MakeSkySystem },
    { "env.sky",              "Environment", "Sky",                    "SkyRenderer 単体", &MakeSky },
    { "env.sunMoon",          "Environment", "Sun & Moon",             "SunMoonRenderer。太陽と月の天体描画", &MakeSunMoon },
    { "env.atmosphere",       "Environment", "Atmospheric Scattering", "大気散乱。空の色と遠景の霞み", &MakeAtmosphere },
    { "env.environmentLight", "Environment", "Environment Light",      "環境光 (IBL)。空からの間接光", &MakeEnvironmentLight },
    { "env.volumetricCloud",  "Environment", "Volumetric Cloud",       "ボリューメトリック雲", &MakeVolumetricCloud },
    { "env.postProcess",      "Environment", "Post Process Volume",    "ルック設定 (Post Process Profile を割り当てて使う)", &MakePostProcessVolume },
    { "env.reflectionProbe",  "Environment", "Reflection Probe",       "反射プローブ。周囲をキューブマップへ焼く", &MakeReflectionProbe },
    { "env.windZone",         "Environment", "Wind Zone",              "風。植生とパーティクルを揺らす", &MakeWindZone },

    { "terrain.terrain", "Terrain", "Terrain",      "平坦な地形 (65x65) + Terrain Collider + 既定レイヤーマテリアル", &MakeTerrain },
    { "terrain.grid",    "Terrain", "Terrain Grid", "Terrain セルを並べて広い地形を作る親", &MakeTerrainGrid },
    { "terrain.water",   "Terrain", "Water",        "水面 (80m x 80m / 96x96 分割)", &MakeWater },

    { "nav.surface",    "Navigation", "NavMesh Surface",  "NavMesh のベイク範囲と設定。ベイクは Tools > Navigation で明示的に実行する", &MakeNavMeshSurface },
    { "nav.agent",      "Navigation", "NavMesh Agent",    "Capsule メッシュ + NavMeshAgent。経路移動する実体", &MakeNavMeshAgent },
    { "nav.modifier",   "Navigation", "NavMesh Modifier", "Cube + Collider + NavMeshModifier。歩行可否とエリアコストを上書きする", &MakeNavMeshModifier },
    { "nav.offMeshLink", "Navigation", "Off-Mesh Link",   "離れた 2 点をつなぐジャンプ経路", &MakeOffMeshLink },
    { "nav.aiAgent",    "Navigation", "AI Agent",         "Agent + Sensor + Patrol + BehaviorTree。敵 1 体の骨格一式", &MakeAIAgent },

    { "fx.particle",   "Effects", "Particle Emitter",     "ParticleEmitter 単体", &MakeParticleEmitter },
    { "fx.forceField", "Effects", "Particle Force Field", "パーティクルへ働く力場", &MakeParticleForceField },
    { "fx.vfxGraph",   "Effects", "VFX Graph",            "VFXGraphComponent。graphPath に .vfx を設定して使う", &MakeVFXGraph },
    { "fx.trail",      "Effects", "Trail",                "移動軌跡を帯で描く", &MakeTrail },
    { "fx.meshTrail",  "Effects", "Mesh Trail",           "メッシュの残像を残す", &MakeMeshTrail },

    { "decal.medium", "Decal", "Decal (2m x 2m)", "デカール投影ボリューム", &MakeDecalMedium },
    { "decal.small",  "Decal", "Decal (1m x 1m)", "小さいデカール投影ボリューム", &MakeDecalSmall },
    { "decal.large",  "Decal", "Decal (5m x 5m)", "大きいデカール投影ボリューム", &MakeDecalLarge },

    { "physics.rigidBody",          "Physics", "Rigid Body",          "Cube + Box Collider + RigidBody。y=3 から落下する", &MakeRigidBodyCube },
    { "physics.characterController", "Physics", "Character Controller", "Capsule + Collider + CharacterController", &MakeCharacterController },
    { "physics.trigger",            "Physics", "Trigger",             "isTrigger の Box Collider。侵入検知に使う", &MakeTriggerVolume },
    { "physics.forceVolume",        "Physics", "Force Volume",        "重力・磁力などの力場ボリューム", &MakeForceVolume },

    { "audio.source",   "Audio", "Audio Source",   "AudioSource。clipPath を設定して鳴らす", &MakeAudioSource },
    { "audio.listener", "Audio", "Audio Listener", "AudioListener。3D 音の距離減衰の基準点", &MakeAudioListener },

    { "spline.spline",   "Spline", "Spline",          "3 点の直線スプライン。制御点は Inspector で編集する", &MakeSpline },
    { "spline.follower", "Spline", "Spline Follower", "Cube + SplineFollower。Spline を参照させて走らせる", &MakeSplineFollower },

    { "ui.canvas",           "UI", "Canvas",                  "UI のルート。生成すると編集対象 Canvas になる", &MakeUICanvas },
    { "ui.image",            "UI", "Image",                   "白い 100x100 の UIImage", &MakeUIImage },
    { "ui.text",             "UI", "Text",                    "白文字 42px の UIText", &MakeUIText },
    { "ui.button",           "UI", "Button",                  "背景 Image + Button + 子 Label の標準構成", &MakeUIButton },
    { "ui.panel",            "UI", "Panel",                   "画面を覆う半透明の背景パネル", &MakeUIPanel },
    { "ui.layoutHorizontal", "UI", "Horizontal Layout Group", "子を横に並べる (spacing 8)", &MakeUIHorizontalLayout },
    { "ui.layoutVertical",   "UI", "Vertical Layout Group",   "子を縦に並べる (spacing 8)", &MakeUIVerticalLayout },
};

} // namespace

std::span<const ObjectPreset> ObjectPresetCatalog()
{
    return std::span<const ObjectPreset>(kPresets, std::size(kPresets));
}

const ObjectPreset* FindObjectPreset(std::string_view id)
{
    for (const ObjectPreset& preset : kPresets) {
        if (preset.id == id) return &preset;
    }
    return nullptr;
}

scene::GameObject* CreateObjectFromPreset(EditorContext& ctx, std::string_view id, scene::EntityID parent)
{
    if (ctx.activeScene == nullptr) return nullptr;
    const ObjectPreset* preset = FindObjectPreset(id);
    if (preset == nullptr || preset->create == nullptr) return nullptr;

    scene::GameObject* created = preset->create(ctx);
    if (created == nullptr) return nullptr;

    // 生成後に EntityID で引き直す。プリセットによっては内部で更に GameObject を作るため、
    // 直前に得たポインタは配列再確保で無効になり得る。
    const scene::EntityID createdId = created->GetID();
    if (parent.IsValid()) {
        if (auto* parentObject = ctx.activeScene->GetGameObject(parent)) {
            if (auto* child = ctx.activeScene->GetGameObject(createdId)) child->SetParent(parentObject);
        }
    }
    ctx.selectedEntities = { createdId };
    return ctx.activeScene->GetGameObject(createdId);
}

} // namespace fbzz::editor
