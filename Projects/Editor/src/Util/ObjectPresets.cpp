/// @file    ObjectPresets.cpp
/// @brief   Create プリセットの実体。Hierarchy・メインメニュー・Operator・AI が同じ表を使う。
/// @author  Hasegawa Jin
/// @date    2026-08-14
#include <Editor/Util/ObjectPresets.hpp>

#include <Editor/EditorContext.hpp>
#include <Editor/Util/ColliderFit.hpp>
#include <Editor/Util/TerrainWaterDefaults.hpp>
#include <Engine/Renderer/PrimitiveMesh.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Scene/ComponentRegistry.hpp>
#include <Engine/Scene/Components/VFXLineComponent.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/ScriptComponent.hpp>
#include <Engine/Scene/ScriptFactory.hpp>
#include <Engine/Scene/VFXLineGeometry.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
#include <Physics/RigidBody.hpp>

#include <iterator>
#include <memory>
#include <utility>

namespace fbzz::editor {

namespace {

/// @name プリミティブ

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

/// @brief 付いているメッシュの bounds から寸法を決めたコライダーを付ける。
/// @note 固定寸法だとプリミティブの実寸 (Capsule 等) とずれる。
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

/// @brief MeshRenderer + Lit マテリアル (+ 任意でフィットしたコライダー) を持つ GameObject。
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

/// @brief 質量 1 の剛体。
/// @note 既定構築の RigidBodyComponent は rigidBody が null で、物理にも保存にも乗らない
///       (PhysicsSystem と SceneSerializer は null を飛ばす)。Add Component と同じ中身にする。
scene::RigidBodyComponent MakePresetRigidBody()
{
    scene::RigidBodyComponent body;
    body.rigidBody = std::make_unique<physics::RigidBody>();
    body.rigidBody->SetMass(1.0f);
    return body;
}

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

/// @brief 子を作って parentId の下へ入れる。
/// @note 2 個目を作った後は先に得た参照を使わず EntityID で引き直す。
scene::GameObject& NewPresetChild(EditorContext& ctx, scene::EntityID parentId, const char* name)
{
    auto& child = ctx.activeScene->CreateGameObject(name);
    if (auto* parent = ctx.activeScene->GetGameObject(parentId)) child.SetParent(parent);
    return child;
}

/// @name 生成関数

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
    using Type = scene::LightComponent::Type;
    auto& go = ctx.activeScene->CreateGameObject(name);
    scene::LightComponent light;
    light.type = type;
    switch (type) {
    case Type::Point:
        light.intensity = 4.0f;
        light.range     = 8.0f;
        break;
    case Type::Spot:
        light.intensity = 5.0f;
        light.range     = 12.0f;
        break;
    case Type::Area:
        /// @note Area の intensity は輝度で、Point の感覚の数値ではほぼ見えない (LightComponent.hpp)。
        light.intensity  = 150.0f;
        light.range      = 12.0f;
        light.areaWidth  = 2.0f;
        light.areaHeight = 1.0f;
        break;
    case Type::Sphere:
        light.intensity    = 4.0f;
        light.range        = 8.0f;
        light.sourceRadius = 0.25f;
        break;
    case Type::Tube:
        light.intensity    = 4.0f;
        light.range        = 8.0f;
        light.sourceRadius = 0.05f;
        light.sourceLength = 1.0f;
        break;
    case Type::Directional:
        break;
    }
    go.AddComponent<scene::LightComponent>(light);
    return &go;
}

scene::GameObject* MakeDirectionalLight(EditorContext& ctx) { return MakeLight(ctx, "Directional Light", scene::LightComponent::Type::Directional); }
scene::GameObject* MakePointLight(EditorContext& ctx)       { return MakeLight(ctx, "Point Light", scene::LightComponent::Type::Point); }
scene::GameObject* MakeSpotLight(EditorContext& ctx)        { return MakeLight(ctx, "Spot Light", scene::LightComponent::Type::Spot); }
scene::GameObject* MakeAreaLight(EditorContext& ctx)        { return MakeLight(ctx, "Area Light", scene::LightComponent::Type::Area); }
scene::GameObject* MakeSphereLight(EditorContext& ctx)      { return MakeLight(ctx, "Sphere Light", scene::LightComponent::Type::Sphere); }
scene::GameObject* MakeTubeLight(EditorContext& ctx)        { return MakeLight(ctx, "Tube Light", scene::LightComponent::Type::Tube); }

/// @note AudioListener も付ける。Listener が無いシーンでは 3D 音の減衰が黙って効かない。
scene::GameObject* MakeCamera(EditorContext& ctx)
{
    auto& go = ctx.activeScene->CreateGameObject("Camera");
    go.transform.position = { 0.0f, 2.0f, -5.0f };
    go.AddComponent<scene::CameraComponent>();
    go.AddComponent<scene::AudioListenerComponent>();
    return &go;
}

/// @note VirtualCamera は同じ GameObject の CameraComponent を main に切り替える (GameplayComponentSystems)。
scene::GameObject* MakeVirtualCamera(EditorContext& ctx)
{
    auto& go = ctx.activeScene->CreateGameObject("Virtual Camera");
    go.AddComponent<scene::CameraComponent>();
    go.AddComponent<scene::VirtualCameraComponent>();
    return &go;
}

scene::GameObject* MakeFollowCamera(EditorContext& ctx)
{
    auto& go = ctx.activeScene->CreateGameObject("Follow Camera");
    go.AddComponent<scene::CameraComponent>();
    go.AddComponent<scene::VirtualCameraComponent>();
    go.AddComponent<scene::CameraFollowComponent>();
    return &go;
}

/// @name Rendering

scene::GameObject* MakeSprite(EditorContext& ctx)
{
    auto& go = ctx.activeScene->CreateGameObject("Sprite");
    go.AddComponent<scene::SpriteRendererComponent>();
    return &go;
}

/// @note points が空だと何も描かれず、置いた直後に壊れて見えるので 2 点入れる。
scene::GameObject* MakeLineRenderer(EditorContext& ctx)
{
    auto& go = ctx.activeScene->CreateGameObject("Line");
    scene::LineRendererComponent line;
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

scene::GameObject* MakeSortingGroup(EditorContext& ctx)
{
    auto& go = ctx.activeScene->CreateGameObject("Sorting Group");
    go.AddComponent<scene::SortingGroupComponent>();
    return &go;
}

/// @name Environment

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

/// @note 未割り当てのボリュームは何も適用しないので、同梱の既定プロファイルを最初から挿す。
///       パスが無いプロジェクトでは解決に失敗し、Inspector が警告を出す。
scene::GameObject* MakePostProcessVolume(EditorContext& ctx)
{
    auto& go = ctx.activeScene->CreateGameObject("Post Process Volume");
    auto& ppv = go.AddComponent<scene::PostProcessVolumeComponent>();
    ppv.profile.ref.path = "Assets/PostProcess/DefaultPostProcess.fzdata";
    return &go;
}

scene::GameObject* MakeReflectionProbe(EditorContext& ctx)
{
    auto& go = ctx.activeScene->CreateGameObject("Reflection Probe");
    go.AddComponent<scene::ReflectionProbeComponent>();
    return &go;
}

scene::GameObject* MakeLightProbeVolume(EditorContext& ctx)
{
    auto& go = ctx.activeScene->CreateGameObject("Light Probe Volume");
    go.AddComponent<scene::LightProbeVolumeComponent>();
    return &go;
}

/// @brief 環境流を有効にする。**GameObject は作らない。**
/// @return 常に nullptr。生成コマンド (ObjectCreation.cpp) はこれを «シーン設定だけを変えた» と読み、
///         Undo で SceneEnvironment を元の値へ戻す。
/// @note 環境流はシーン設定 (SceneEnvironment) で、置き場所を持たない。GameObject にすると
///       «どれが環境風か» が並び順で決まる沈黙のバグが戻る (flow-field.md §6)。
scene::GameObject* MakeAmbientWind(EditorContext& ctx)
{
    if (ctx.activeScene == nullptr) return nullptr;
    scene::SceneEnvironment& environment = ctx.activeScene->Environment();
    environment.enabled = true;
    /// @note 置いた瞬間に何も起きないと «壊れている» と映るので、そよ風ぶんの速さを入れる。
    if (environment.speed <= 0.0f) environment.speed = 5.0f;
    if (ctx.markSceneDirty) ctx.markSceneDirty();
    return nullptr;
}

/// @brief 天候ルート + 子の雨エミッター。
/// @note 雨量の正本は WeatherComponent.rainIntensity で、子のエミッターは WeatherSystem が毎フレーム駆動する。
///       既定の rainIntensity 0 では置いても何も起きないので、降っている状態で置く。
scene::GameObject* MakeWeather(EditorContext& ctx)
{
    auto& weatherGo = ctx.activeScene->CreateGameObject("Weather");
    scene::WeatherComponent weather;
    weather.rainIntensity = 0.5f;
    weatherGo.AddComponent<scene::WeatherComponent>(weather);
    const scene::EntityID weatherId = weatherGo.GetID();

    auto& rainGo = NewPresetChild(ctx, weatherId, "Rain");

    scene::ParticleEmitter rain;
    auto& s = rain.settings;
    s.shape          = scene::ParticleEmitterShape::Box;
    s.boxExtents     = { 20.0f, 0.5f, 20.0f };
    s.emitPosition   = { 0.0f, 12.0f, 0.0f };
    s.emitVelocity   = { 0.0f, -14.0f, 0.0f };
    s.velocitySpread = 0.6f;
    s.SetGravityAcceleration({ 0.0f, -9.0f, 0.0f });
    s.lifetime       = 1.6f;
    s.lifetimeRandom = 0.2f;
    s.emitRate       = 0.0f;
    s.maxParticles   = 8000;
    s.sizeStart      = 0.05f;
    s.sizeEnd        = 0.05f;
    /// @note RainDrop.hlsl は StretchedBillboard でないと縦棒を描けない。
    s.renderMode             = scene::ParticleRenderMode::StretchedBillboard;
    s.stretchedLengthScale   = 0.6f;
    s.stretchedVelocityScale = 0.06f;
    /// @note Local だと親を動かした瞬間に降っている粒ごと平行移動する。
    s.simulationSpace = scene::ParticleSimulationSpace::World;
    s.colorStart   = { 1.0f, 1.0f, 1.0f, 1.0f };
    s.colorEnd     = { 1.0f, 1.0f, 1.0f, 1.0f };
    s.materialPath = "Assets/Materials/Particles/Rain_Alpha.mat";
    s.loop         = true;
    rainGo.AddComponent<scene::ParticleEmitter>(std::move(rain));

    return ctx.activeScene->GetGameObject(weatherId);
}

/// @brief 空・太陽・大気・IBL をまとめた屋外シーンの環境ルート。
scene::GameObject* MakeSkySystem(EditorContext& ctx)
{
    auto& go = ctx.activeScene->CreateGameObject("Sky System");
    go.AddComponent<scene::SkyRenderer>();
    go.AddComponent<scene::SunMoonRenderer>();
    go.AddComponent<scene::AtmosphericScatteringComponent>();
    go.AddComponent<scene::EnvironmentLightComponent>();
    return &go;
}

/// @name Terrain / Water

scene::GameObject* MakeTerrain(EditorContext& ctx)
{
    auto& go = ctx.activeScene->CreateGameObject("Terrain");
    scene::TerrainComponent terrain;
    terrain.InitFlat(0.0f);
    for (int layer = 0; layer < terrain.LayerCount(); ++layer)
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

/// @name Navigation / AI

/// @note needsBake は立てない。AddComponent 直後の自動ベイクでエディタが固まるのを防ぐ既定 (false) に従う。
scene::GameObject* MakeNavMeshSurface(EditorContext& ctx)
{
    auto& go = ctx.activeScene->CreateGameObject("NavMesh Surface");
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

/// @note 4 つを 1 セットで置く。Sensor だけ忘れると BT の条件が永久に偽になり、最も追いにくい。
scene::GameObject* MakeAIAgent(EditorContext& ctx)
{
    auto& go = NewPrimitive(ctx, "AI Agent", PrimitiveKind::Capsule, /*withCollider=*/false);
    go.AddComponent<scene::NavMeshAgentComponent>();
    go.AddComponent<scene::NavMeshSensorComponent>();
    go.AddComponent<scene::NavMeshPatrolComponent>();
    go.AddComponent<scene::BehaviorTreeComponent>();
    return &go;
}

/// @name Effects

scene::GameObject* MakeParticleEmitter(EditorContext& ctx)
{
    auto& go = ctx.activeScene->CreateGameObject("Particle Emitter");
    go.AddComponent<scene::ParticleEmitter>();
    return &go;
}

scene::GameObject* MakeFlowField(EditorContext& ctx)
{
    auto& go = ctx.activeScene->CreateGameObject("Flow Field");
    go.AddComponent<scene::FlowField>();
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

/// @name VFX (.vfx プレハブ) の部品

/// @brief 立ち上がり 0.1 → ピーク → 減衰。閃光・画面フラッシュの重み。
scene::ParticleCurve MakeFlashWeightCurve()
{
    scene::ParticleCurve curve;
    curve.keys[0] = { 0.0f, 0.0f };
    curve.keys[1] = { 0.1f, 1.0f };
    curve.keys[2] = { 1.0f, 0.0f };
    for (std::size_t i = 3; i < scene::kMaxParticleCurveKeys; ++i) curve.keys[i] = curve.keys[2];
    curve.keyCount = 3;
    return curve;
}

/// @brief 頭でっかちに減衰する。カメラ揺れの重み (旧 falloffPower = 2 相当)。
scene::ParticleCurve MakeDecayWeightCurve()
{
    scene::ParticleCurve curve;
    curve.keys[0] = { 0.0f, 1.0f };
    curve.keys[1] = { 0.25f, 0.56f };
    curve.keys[2] = { 0.5f, 0.25f };
    curve.keys[3] = { 0.75f, 0.06f };
    curve.keys[4] = { 1.0f, 0.0f };
    for (std::size_t i = 5; i < scene::kMaxParticleCurveKeys; ++i) curve.keys[i] = curve.keys[4];
    curve.keyCount = 5;
    return curve;
}

scene::GameObject* MakeVFXRoot(EditorContext& ctx)
{
    auto& go = ctx.activeScene->CreateGameObject("VFX");
    go.AddComponent<scene::VFXComponent>();
    return &go;
}

/// @brief 層のまとまり。Transform だけを持ち、時間には関与しない。
scene::GameObject* MakeVFXGroup(EditorContext& ctx)
{
    return &ctx.activeScene->CreateGameObject("Layer");
}

/// @note 常設の fx.particle と違い、1 発鳴らして自分で畳む前提 (loop なし・有限の duration)。
scene::GameObject* MakeVFXParticle(EditorContext& ctx)
{
    auto& go = ctx.activeScene->CreateGameObject("Particle");
    scene::ParticleEmitter emitter;
    emitter.settings.loop     = false;
    emitter.settings.duration = 1.0f;
    emitter.settings.lifetime = 0.8f;
    go.AddComponent<scene::ParticleEmitter>(std::move(emitter));
    return &go;
}

/// @note 影は切る。点光源の影は 6 面描くので、一瞬光らせるだけで目に見えて重くなる。
scene::GameObject* MakeVFXLightFlash(EditorContext& ctx)
{
    auto& go = ctx.activeScene->CreateGameObject("Light Flash");
    scene::LightComponent light;
    light.type        = scene::LightComponent::Type::Point;
    light.intensity   = 20.0f;
    light.range       = 8.0f;
    light.castShadows = false;
    go.AddComponent<scene::LightComponent>(light);
    scene::VFXElement element;
    element.duration = 0.4f;
    go.AddComponent<scene::VFXElement>(std::move(element));
    go.AddComponent<scene::VFXLightEnvelope>();
    return &go;
}

scene::GameObject* MakeVFXMeshShell(EditorContext& ctx)
{
    auto& go = ctx.activeScene->CreateGameObject("Shell");
    scene::MeshRenderer renderer;
    renderer.meshPath = "primitive:sphere";
    go.AddComponent<scene::MeshRenderer>(std::move(renderer));
    scene::MaterialComponent material;
    material.materialPath = scene::kVFXMeshFallbackMaterial;
    go.AddComponent<scene::MaterialComponent>(std::move(material));
    scene::VFXElement element;
    element.duration = 0.5f;
    go.AddComponent<scene::VFXElement>(std::move(element));
    go.AddComponent<scene::VFXTransformEnvelope>();
    go.AddComponent<scene::VFXMaterialEnvelope>();
    return &go;
}

scene::GameObject* MakeVFXFlowField(EditorContext& ctx)
{
    auto& go = ctx.activeScene->CreateGameObject("Blast Push");
    scene::FlowFieldSettings push;
    push.fieldType = scene::FlowFieldType::Source;
    /// @note 流速 [m/s]。爆風の «押しのける» は 4 m/s あれば十分に見える。
    push.strength  = 4.0f;
    push.radius    = 4.5f;
    scene::FlowField field;
    field.forces = { push };
    go.AddComponent<scene::FlowField>(std::move(field));
    scene::VFXElement element;
    element.duration = 0.25f;
    go.AddComponent<scene::VFXElement>(std::move(element));
    return &go;
}

/// @note 明るさだけでは «押し出された» にならないので radialBlur を少し足す。
scene::GameObject* MakeVFXScreenEffect(EditorContext& ctx)
{
    auto& go = ctx.activeScene->CreateGameObject("Screen Flash");
    scene::VFXScreenEffect effect;
    effect.flashIntensity = 0.35f;
    effect.bloomBoost     = 0.4f;
    effect.radialBlur     = 0.05f;
    go.AddComponent<scene::VFXScreenEffect>(std::move(effect));
    scene::VFXElement element;
    element.duration    = 0.3f;
    element.weightCurve = MakeFlashWeightCurve();
    go.AddComponent<scene::VFXElement>(std::move(element));
    return &go;
}

/// @note 揺れだけでは «揺れた» で終わるので、発生源から押しのける kick を少し入れる。
scene::GameObject* MakeVFXCameraShake(EditorContext& ctx)
{
    auto& go = ctx.activeScene->CreateGameObject("Camera Shake");
    scene::VFXCameraShake shake;
    shake.kick = 0.06f;
    go.AddComponent<scene::VFXCameraShake>(std::move(shake));
    scene::VFXElement element;
    element.duration    = 0.35f;
    element.weightCurve = MakeDecayWeightCurve();
    go.AddComponent<scene::VFXElement>(std::move(element));
    return &go;
}

scene::GameObject* MakeVFXTimeScale(EditorContext& ctx)
{
    auto& go = ctx.activeScene->CreateGameObject("Hit Stop");
    go.AddComponent<scene::VFXTimeScale>();
    scene::VFXElement element;
    element.duration = 0.12f;
    go.AddComponent<scene::VFXElement>(std::move(element));
    return &go;
}

/// @brief 生存窓に沿って音量を動かす音。音量カーブの基準は AudioSource.volume。
scene::GameObject* MakeVFXAudio(EditorContext& ctx)
{
    auto& go = ctx.activeScene->CreateGameObject("Audio");
    scene::AudioSourceComponent audio;
    audio.playOnAwake = true;
    go.AddComponent<scene::AudioSourceComponent>(std::move(audio));
    scene::VFXElement element;
    element.duration = 1.0f;
    go.AddComponent<scene::VFXElement>(std::move(element));
    go.AddComponent<scene::VFXAudioEnvelope>();
    return &go;
}

/// @note 見た目は Trail が持ち、経路だけ VFXBeam が毎フレーム書く。
///       先細ると «飛んだ跡» に見えるので幅は一定、流れる向きで引かれる方向を出す。
scene::GameObject* MakeVFXBeam(EditorContext& ctx)
{
    auto& go = ctx.activeScene->CreateGameObject("Beam");
    scene::TrailComponent trail;
    trail.beamMode      = true;
    trail.widthStart    = 0.12f;
    trail.widthEnd      = 0.12f;
    trail.uvMode        = scene::TrailUVMode::Tile;
    trail.uvTiling      = 4.0f;
    trail.uvScrollSpeed = -2.0f;
    go.AddComponent<scene::TrailComponent>(std::move(trail));

    scene::VFXBeamComponent beam;
    beam.segments = 12;
    beam.jitter   = 0.08f;
    go.AddComponent<scene::VFXBeamComponent>(std::move(beam));
    return &go;
}

scene::GameObject* MakeVFXLine(EditorContext& ctx, scene::VFXLinePreset preset, const char* name)
{
    auto& go = ctx.activeScene->CreateGameObject(name);
    scene::VFXLineComponent line;
    scene::ApplyVFXLinePreset(line, preset);
    go.AddComponent<scene::VFXLineComponent>(std::move(line));
    return &go;
}

scene::GameObject* MakeVFXLightning(EditorContext& ctx)   { return MakeVFXLine(ctx, scene::VFXLinePreset::Lightning, "Lightning"); }
scene::GameObject* MakeVFXElectricArc(EditorContext& ctx) { return MakeVFXLine(ctx, scene::VFXLinePreset::ElectricArc, "Electric Arc"); }
scene::GameObject* MakeVFXLaser(EditorContext& ctx)       { return MakeVFXLine(ctx, scene::VFXLinePreset::Laser, "Laser"); }
scene::GameObject* MakeVFXEnergyBeam(EditorContext& ctx)  { return MakeVFXLine(ctx, scene::VFXLinePreset::EnergyBeam, "Energy Beam"); }
scene::GameObject* MakeVFXTether(EditorContext& ctx)      { return MakeVFXLine(ctx, scene::VFXLinePreset::Tether, "Tether"); }

scene::GameObject* MakeVFXDecal(EditorContext& ctx)
{
    auto& go = ctx.activeScene->CreateGameObject("Ground Mark");
    scene::DecalComponent decal;
    decal.lifetime = 4.0f;
    decal.fadeTime = 1.0f;
    go.AddComponent<scene::DecalComponent>(std::move(decal));
    go.AddComponent<scene::VFXDecalEnvelope>();
    go.transform.scale = { 2.0f, 3.0f, 2.0f };
    return &go;
}

/// @param sizeXZ 投影面の一辺 [m]。
/// @param depth  投影の深さ [m]。
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

/// @name Physics

/// @note y=3 はルートに置いたときだけ効く (子に置くと親の原点へ置き直される)。
scene::GameObject* MakeRigidBodyCube(EditorContext& ctx)
{
    auto& go = NewPrimitive(ctx, "Rigid Body", PrimitiveKind::Cube);
    go.transform.position = { 0.0f, 3.0f, 0.0f };
    go.AddComponent<scene::RigidBodyComponent>(MakePresetRigidBody());
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

scene::GameObject* MakeSphereTrigger(EditorContext& ctx)
{
    auto& go = ctx.activeScene->CreateGameObject("Sphere Trigger");
    scene::SphereColliderComponent sphere;
    sphere.isTrigger = true;
    go.AddComponent<scene::SphereColliderComponent>(std::move(sphere));
    return &go;
}

scene::GameObject* MakeCapsuleTrigger(EditorContext& ctx)
{
    auto& go = ctx.activeScene->CreateGameObject("Capsule Trigger");
    scene::CapsuleColliderComponent capsule;
    capsule.isTrigger = true;
    go.AddComponent<scene::CapsuleColliderComponent>(std::move(capsule));
    return &go;
}

/// @note meshPath は空のままにする。ColliderSync は同じ GameObject の MeshRenderer を先に見る。
scene::GameObject* MakeMeshColliderObject(EditorContext& ctx)
{
    auto& go = NewPrimitive(ctx, "Mesh Collider", PrimitiveKind::Torus, /*withCollider=*/false);
    go.AddComponent<scene::MeshColliderComponent>();
    return &go;
}

scene::GameObject* MakeConvexHullObject(EditorContext& ctx)
{
    auto& go = NewPrimitive(ctx, "Convex Hull Collider", PrimitiveKind::Cone, /*withCollider=*/false);
    go.AddComponent<scene::ConvexHullColliderComponent>();
    return &go;
}

/// @note 相手は connectToParent で祖先の剛体を探す。ルートに置いただけでは繋がらない。
scene::GameObject* MakeRopeJoint(EditorContext& ctx)
{
    auto& go = NewPrimitive(ctx, "Joint", PrimitiveKind::Cube);
    go.transform.scale = { 0.5f, 0.5f, 0.5f };
    go.AddComponent<scene::RigidBodyComponent>(MakePresetRigidBody());
    scene::JointComponent joint;
    joint.type            = scene::JointType::Rope;
    joint.connectToParent = true;
    joint.autoDistance    = true;
    go.AddComponent<scene::JointComponent>(std::move(joint));
    return &go;
}

scene::GameObject* MakeForceVolume(EditorContext& ctx)
{
    auto& go = ctx.activeScene->CreateGameObject("Force Volume");
    go.AddComponent<scene::VolumeComponent>();
    return &go;
}

/// @name Audio

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

scene::GameObject* MakeAudioReverbZone(EditorContext& ctx)
{
    auto& go = ctx.activeScene->CreateGameObject("Audio Reverb Zone");
    go.AddComponent<scene::AudioReverbZoneComponent>();
    return &go;
}

/// @name Animation

scene::GameObject* MakeSequencePlayer(EditorContext& ctx)
{
    auto& go = ctx.activeScene->CreateGameObject("Sequence Player");
    go.AddComponent<scene::SequencePlayerComponent>();
    return &go;
}

scene::GameObject* MakeSocketAttachment(EditorContext& ctx)
{
    auto& go = ctx.activeScene->CreateGameObject("Socket Attachment");
    go.AddComponent<scene::SocketAttachmentComponent>();
    return &go;
}

scene::GameObject* MakeTransformConstraint(EditorContext& ctx)
{
    auto& go = ctx.activeScene->CreateGameObject("Transform Constraint");
    go.AddComponent<scene::TransformConstraintComponent>();
    return &go;
}

/// @name Spline

/// @note 点が無いと Scene View に編集の起点が出ないので、直線 3 点を入れる。
scene::GameObject* MakeSpline(EditorContext& ctx)
{
    auto& go = ctx.activeScene->CreateGameObject("Spline");
    scene::SplineComponent spline;
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

/// @name UI

/// @note UIViewport の編集対象 Canvas をこれに確定させる。
scene::GameObject* MakeUICanvas(EditorContext& ctx)
{
    auto& go = ctx.activeScene->CreateGameObject("Canvas");
    go.AddComponent<scene::UICanvas>();
    ctx.activeUICanvas = go.GetID();
    return &go;
}

/// @param w 幅 (Canvas 単位)。UI 要素の大きさは transform.scale.xy が正本 (UIRect.hpp)。
/// @param h 高さ (Canvas 単位)。
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

scene::UIText MakeUILabelText(const char* text, float fontSize, const math::Vector4& color)
{
    scene::UIText txt;
    txt.text     = text;
    txt.fontSize = fontSize;
    txt.color    = color;
    return txt;
}

scene::GameObject* MakeUIImage(EditorContext& ctx)
{
    return MakeUIImageObject(ctx, "Image", { 1.0f, 1.0f, 1.0f, 1.0f }, 100.0f, 100.0f);
}

scene::GameObject* MakeUIPanel(EditorContext& ctx)
{
    return MakeUIImageObject(ctx, "Panel", { 0.20f, 0.20f, 0.20f, 0.80f }, 1920.0f, 1080.0f);
}

scene::GameObject* MakeUIText(EditorContext& ctx)
{
    auto& go = ctx.activeScene->CreateGameObject("Text");
    go.AddComponent<scene::UIText>(MakeUILabelText("Text", 42.0f, { 1.0f, 1.0f, 1.0f, 1.0f }));
    return &go;
}

scene::GameObject* MakeUIButton(EditorContext& ctx)
{
    auto* button = MakeUIImageObject(ctx, "Button", { 0.90f, 0.90f, 0.90f, 1.0f }, 160.0f, 40.0f);
    button->AddComponent<scene::UIButton>();
    const scene::EntityID buttonId = button->GetID();

    auto& label = NewPresetChild(ctx, buttonId, "Label");
    label.AddComponent<scene::UIText>(MakeUILabelText("Button", 24.0f, { 0.20f, 0.20f, 0.20f, 1.0f }));
    return ctx.activeScene->GetGameObject(buttonId);
}

/// @note UISystem はドラッグ中に同じ GameObject の UIImage.fillAmount を値で書き換える。
scene::GameObject* MakeUISlider(EditorContext& ctx)
{
    auto* go = MakeUIImageObject(ctx, "Slider", { 0.35f, 0.65f, 1.0f, 1.0f }, 200.0f, 20.0f);
    scene::UISlider slider;
    slider.value = 0.5f;
    go->AddComponent<scene::UISlider>(slider);
    if (auto* image = go->GetComponent<scene::UIImage>()) image->fillAmount = slider.value;
    return go;
}

scene::GameObject* MakeUIToggle(EditorContext& ctx)
{
    auto* toggle = MakeUIImageObject(ctx, "Toggle", { 1.0f, 1.0f, 1.0f, 1.0f }, 24.0f, 24.0f);
    toggle->AddComponent<scene::UIToggle>();
    const scene::EntityID toggleId = toggle->GetID();

    auto& label = NewPresetChild(ctx, toggleId, "Label");
    label.transform.position = { 32.0f, 0.0f, 0.0f };
    label.AddComponent<scene::UIText>(MakeUILabelText("Toggle", 20.0f, { 1.0f, 1.0f, 1.0f, 1.0f }));
    return ctx.activeScene->GetGameObject(toggleId);
}

/// @brief 背景 + スクロール + マスク。子の Content を縦に並べる。
scene::GameObject* MakeUIScrollView(EditorContext& ctx)
{
    auto* view = MakeUIImageObject(ctx, "Scroll View", { 0.15f, 0.15f, 0.15f, 0.90f }, 300.0f, 300.0f);
    scene::UIScrollView scroll;
    scroll.contentSize = { 300.0f, 600.0f };
    view->AddComponent<scene::UIScrollView>(scroll);
    view->AddComponent<scene::UIMask>();
    const scene::EntityID viewId = view->GetID();

    auto& content = NewPresetChild(ctx, viewId, "Content");
    content.transform.scale = { 300.0f, 600.0f, 1.0f };
    scene::UILayoutGroup layout;
    layout.axis    = scene::UILayoutAxis::Vertical;
    layout.spacing = 8.0f;
    content.AddComponent<scene::UILayoutGroup>(layout);
    return ctx.activeScene->GetGameObject(viewId);
}

scene::GameObject* MakeUIMask(EditorContext& ctx)
{
    auto* go = MakeUIImageObject(ctx, "Mask", { 1.0f, 1.0f, 1.0f, 1.0f }, 200.0f, 200.0f);
    go->AddComponent<scene::UIMask>();
    return go;
}

/// @note UISystem は入力内容 (空なら placeholder) を同じ GameObject の UIText へ書く。
scene::GameObject* MakeUIInputField(EditorContext& ctx)
{
    auto* go = MakeUIImageObject(ctx, "Input Field", { 0.95f, 0.95f, 0.95f, 1.0f }, 240.0f, 40.0f);
    scene::UIInputField field;
    field.placeholder = "Enter text...";
    go->AddComponent<scene::UIInputField>(field);
    go->AddComponent<scene::UIText>(MakeUILabelText("Enter text...", 24.0f, { 0.20f, 0.20f, 0.20f, 1.0f }));
    return go;
}

scene::GameObject* MakeUILayoutGroup(EditorContext& ctx, const char* name, scene::UILayoutAxis axis)
{
    auto& go = ctx.activeScene->CreateGameObject(name);
    scene::UILayoutGroup layout;
    layout.axis    = axis;
    layout.spacing = 8.0f;
    if (axis == scene::UILayoutAxis::Grid) {
        go.transform.scale  = { 320.0f, 320.0f, 1.0f };
        layout.spacingCross = 8.0f;
        layout.cellSize     = { 100.0f, 100.0f };
    }
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

scene::GameObject* MakeUIGridLayout(EditorContext& ctx)
{
    return MakeUILayoutGroup(ctx, "Grid Layout Group", scene::UILayoutAxis::Grid);
}

scene::GameObject* MakeUICanvasGroup(EditorContext& ctx)
{
    auto& go = ctx.activeScene->CreateGameObject("Canvas Group");
    go.AddComponent<scene::UICanvasGroup>();
    return &go;
}

/// @name 登録表

using PresetPlace = PresetPlacement;

/// @brief 並び順がそのままメニューの並びになる。
/// @note vfx.* は vfx.root の子として組む部品 (Docs/design/vfx-prefab.md)。
/// @note Ragdoll と Procedural Mesh は載せない。前者は骨格を持つモデルへ足すもので単体では働かず、
///       後者は保存されない内部コンポーネント (FBZZ_INTERNAL_COMPONENT) で保存や Play で消える。
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

    { "rendering.sprite",       "Rendering", "Sprite",        "SpriteRenderer。spritePath を設定して 2D 画像を表示する", &MakeSprite },
    { "rendering.line",         "Rendering", "Line",          "LineRenderer。始点と終点の 2 点を入れた状態で生成する", &MakeLineRenderer },
    { "rendering.billboard",    "Rendering", "Billboard",     "Quad メッシュ + Billboard。常にカメラを向く板", &MakeBillboard },
    { "rendering.projector",    "Rendering", "Projector",     "Projector。テクスチャを面へ投影する", &MakeProjector },
    { "rendering.lodGroup",     "Rendering", "LOD Group",     "LODGroup。子オブジェクトを距離で切り替える親", &MakeLODGroup },
    { "rendering.sortingGroup", "Rendering", "Sorting Group", "SortingGroup。子の Sprite の描画順をまとめて決める親", &MakeSortingGroup },

    { "light.directional", "Light", "Directional Light", "太陽光。向きだけが効き、位置は影響しない", &MakeDirectionalLight },
    { "light.point",       "Light", "Point Light",       "点光源 (intensity 4 / range 8)", &MakePointLight },
    { "light.spot",        "Light", "Spot Light",        "スポットライト (intensity 5 / range 12)", &MakeSpotLight },
    { "light.area",        "Light", "Area Light",        "矩形の面光源 2m x 1m。intensity は輝度 (150)。面は +Z を向く", &MakeAreaLight },
    { "light.sphere",      "Light", "Sphere Light",      "半径 0.25m の球光源 (intensity 4 / range 8)", &MakeSphereLight },
    { "light.tube",        "Light", "Tube Light",        "長さ 1m のカプセル光源 (intensity 4 / range 8)", &MakeTubeLight },

    { "camera", "", "Camera", "CameraComponent + AudioListener。Game View の描画元になる", &MakeCamera },

    { "camera.virtual", "Camera Rig", "Virtual Camera", "Camera + VirtualCamera。priority が最大のものが main になる", &MakeVirtualCamera },
    { "camera.follow",  "Camera Rig", "Follow Camera",  "Camera + VirtualCamera + CameraFollow。target を Inspector で指定する", &MakeFollowCamera },

    { "env.skySystem",        "Environment", "Sky System",             "Sky + Sun/Moon + 大気散乱 + 環境光(IBL) をまとめた屋外シーンの環境ルート", &MakeSkySystem },
    { "env.sky",              "Environment", "Sky",                    "SkyRenderer 単体", &MakeSky },
    { "env.sunMoon",          "Environment", "Sun & Moon",             "SunMoonRenderer。太陽と月の天体描画", &MakeSunMoon },
    { "env.atmosphere",       "Environment", "Atmospheric Scattering", "大気散乱。空の色と遠景の霞み", &MakeAtmosphere },
    { "env.environmentLight", "Environment", "Environment Light",      "環境光 (IBL)。空からの間接光", &MakeEnvironmentLight },
    { "env.volumetricCloud",  "Environment", "Volumetric Cloud",       "ボリューメトリック雲", &MakeVolumetricCloud },
    { "env.postProcess",      "Environment", "Post Process Volume",    "ルック設定 (Post Process Profile を割り当てて使う)", &MakePostProcessVolume },
    { "env.reflectionProbe",  "Environment", "Reflection Probe",       "反射プローブ。周囲をキューブマップへ焼く", &MakeReflectionProbe },
    { "env.lightProbeVolume", "Environment", "Light Probe Volume",     "箱の中の間接光を球面調和で焼き、拡散環境光を差し替える", &MakeLightProbeVolume },
    { "env.ambientWind",      "Environment", "Ambient Wind",           "シーン設定の環境流を有効にする (GameObject は作らない)。雲・水面・粒子が同じ流れに乗る", &MakeAmbientWind },
    { "env.weather",          "Environment", "Weather",                "天候。雨量と路面の濡れ (雨エミッターを子に持つ)", &MakeWeather },

    { "terrain.terrain", "Terrain", "Terrain",      "平坦な地形 (65x65) + Terrain Collider + 既定レイヤーマテリアル", &MakeTerrain },
    { "terrain.grid",    "Terrain", "Terrain Grid", "Terrain セルを並べて広い地形を作る親", &MakeTerrainGrid },
    { "terrain.water",   "Terrain", "Water",        "水面 (80m x 80m / 96x96 分割)", &MakeWater },

    { "nav.surface",     "Navigation", "NavMesh Surface",  "NavMesh のベイク範囲と設定。ベイクは Tools > Navigation で明示的に実行する", &MakeNavMeshSurface },
    { "nav.agent",       "Navigation", "NavMesh Agent",    "Capsule メッシュ + NavMeshAgent。経路移動する実体", &MakeNavMeshAgent },
    { "nav.modifier",    "Navigation", "NavMesh Modifier", "Cube + Collider + NavMeshModifier。歩行可否とエリアコストを上書きする", &MakeNavMeshModifier },
    { "nav.offMeshLink", "Navigation", "Off-Mesh Link",    "離れた 2 点をつなぐジャンプ経路", &MakeOffMeshLink },
    { "nav.aiAgent",     "Navigation", "AI Agent",         "Agent + Sensor + Patrol + BehaviorTree。敵 1 体の骨格一式", &MakeAIAgent },

    { "fx.particle",   "Effects", "Particle Emitter", "ParticleEmitter 単体", &MakeParticleEmitter },
    { "fx.flowField",  "Effects", "Flow Field",       "媒質の流れ [m/s]。粒子・雲・水面が同じ場を読む", &MakeFlowField },
    { "fx.trail",      "Effects", "Trail",            "移動軌跡を帯で描く", &MakeTrail },
    { "fx.meshTrail",  "Effects", "Mesh Trail",       "メッシュの残像を残す", &MakeMeshTrail },

    { "vfx.root",         "VFX", "VFX Root",          "VFXComponent。これを .vfx として保存する。子が層になる", &MakeVFXRoot },
    { "vfx.group",        "VFX", "Layer",             "空 GameObject。層のまとまり。時間には関与しない", &MakeVFXGroup },
    { "vfx.particle",     "VFX", "Particle",          "1 発ぶんの ParticleEmitter (loop なし・duration 1 秒)", &MakeVFXParticle },
    { "vfx.lightFlash",   "VFX", "Light Flash",       "点光源 + 生存窓 + 明るさカーブ。影は落とさない", &MakeVFXLightFlash },
    { "vfx.meshShell",    "VFX", "Mesh Shell",        "球シェル + 膨張カーブ + 色フェード。衝撃波・斬撃に使う", &MakeVFXMeshShell },
    { "vfx.flowField",    "VFX", "Flow Field",        "爆風。周囲のパーティクルを押しのける", &MakeVFXFlowField },
    { "vfx.decal",        "VFX", "Ground Mark",       "床の跡。濃さのカーブ付き", &MakeVFXDecal },
    { "vfx.audio",        "VFX", "Audio",             "AudioSource + 生存窓 (1 秒) + 音量カーブ。clipPath を設定して使う", &MakeVFXAudio },
    { "vfx.beam",         "VFX", "Beam (2 点を結ぶ)", "Trail + VFXBeam。2 つの実体を結ぶ。引き寄せの線・電弧に使う", &MakeVFXBeam },
    { "vfx.lightning",    "VFX", "Lightning (雷)",    "VFX Line。本流と枝を毎秒十数回打ち直す雷", &MakeVFXLightning },
    { "vfx.electricArc",  "VFX", "Electric Arc",      "VFX Line。電極の間を細かく走る放電", &MakeVFXElectricArc },
    { "vfx.laser",        "VFX", "Laser",             "VFX Line。まっすぐな光線 (芯とグロー)", &MakeVFXLaser },
    { "vfx.energyBeam",   "VFX", "Energy Beam",       "VFX Line。うねりと流れる輝点を持つ太いビーム", &MakeVFXEnergyBeam },
    { "vfx.tether",       "VFX", "Tether",            "VFX Line。垂れて揺れる引き寄せの線", &MakeVFXTether },
    { "vfx.screenEffect", "VFX", "Screen Flash",      "画面フラッシュ + ブルーム上乗せ", &MakeVFXScreenEffect },
    { "vfx.cameraShake",  "VFX", "Camera Shake",      "カメラ揺れ。頭でっかちに減衰する", &MakeVFXCameraShake },
    { "vfx.timeScale",    "VFX", "Hit Stop",          "一瞬の時間減速", &MakeVFXTimeScale },

    { "decal.medium", "Decal", "Decal (2m x 2m)", "デカール投影ボリューム", &MakeDecalMedium },
    { "decal.small",  "Decal", "Decal (1m x 1m)", "小さいデカール投影ボリューム", &MakeDecalSmall },
    { "decal.large",  "Decal", "Decal (5m x 5m)", "大きいデカール投影ボリューム", &MakeDecalLarge },

    { "physics.rigidBody",           "Physics", "Rigid Body",           "Cube + Box Collider + RigidBody (質量 1)。ルートに置くと注視点の 3m 上から落ちる", &MakeRigidBodyCube },
    { "physics.characterController", "Physics", "Character Controller", "Capsule + Collider + CharacterController", &MakeCharacterController },
    { "physics.trigger",             "Physics", "Trigger",              "isTrigger の Box Collider。侵入検知に使う", &MakeTriggerVolume },
    { "physics.triggerSphere",       "Physics", "Sphere Trigger",       "isTrigger の Sphere Collider", &MakeSphereTrigger },
    { "physics.triggerCapsule",      "Physics", "Capsule Trigger",      "isTrigger の Capsule Collider", &MakeCapsuleTrigger },
    { "physics.meshCollider",        "Physics", "Mesh Collider",        "Torus メッシュ + Mesh Collider。表示メッシュそのままの凹形状の当たり", &MakeMeshColliderObject },
    { "physics.convexHull",          "Physics", "Convex Hull Collider", "Cone メッシュ + Convex Hull Collider。表示メッシュを包む凸包の当たり", &MakeConvexHullObject },
    { "physics.joint",               "Physics", "Joint (Rope)",         "小さな Cube + RigidBody + Joint(Rope)。剛体を持つ親の子に置くとぶら下がる", &MakeRopeJoint },
    { "physics.forceVolume",         "Physics", "Force Volume",         "重力・磁力などの力場ボリューム", &MakeForceVolume },

    { "audio.source",     "Audio", "Audio Source",      "AudioSource。clipPath を設定して鳴らす", &MakeAudioSource },
    { "audio.listener",   "Audio", "Audio Listener",    "AudioListener。3D 音の距離減衰の基準点", &MakeAudioListener },
    { "audio.reverbZone", "Audio", "Audio Reverb Zone", "残響ゾーン (inner 2m / outer 10m)", &MakeAudioReverbZone },

    { "animation.sequencePlayer",      "Animation", "Sequence Player",      "SequencePlayer。sequencePath に .sequence を設定して再生する", &MakeSequencePlayer },
    { "animation.socketAttachment",    "Animation", "Socket Attachment",    "SocketAttachment。骨や子のソケットへ追従する (target 省略時は祖先から探す)", &MakeSocketAttachment },
    { "animation.transformConstraint", "Animation", "Transform Constraint", "TransformConstraint。target の位置・回転・スケールへ拘束する", &MakeTransformConstraint },

    { "spline.spline",   "Spline", "Spline",          "3 点の直線スプライン。制御点は Inspector で編集する", &MakeSpline },
    { "spline.follower", "Spline", "Spline Follower", "Cube + SplineFollower。Spline を参照させて走らせる", &MakeSplineFollower },

    { "ui.canvas",           "UI", "Canvas",                  "UI のルート。生成すると編集対象 Canvas になる", &MakeUICanvas, PresetPlace::Screen },
    { "ui.image",            "UI", "Image",                   "白い 100x100 の UIImage", &MakeUIImage, PresetPlace::UIElement },
    { "ui.text",             "UI", "Text",                    "白文字 42px の UIText", &MakeUIText, PresetPlace::UIElement },
    { "ui.button",           "UI", "Button",                  "背景 Image + Button + 子 Label の標準構成", &MakeUIButton, PresetPlace::UIElement },
    { "ui.toggle",           "UI", "Toggle",                  "24x24 の Image + Toggle + 子 Label。isOn の見た目はスクリプトで付ける", &MakeUIToggle, PresetPlace::UIElement },
    { "ui.slider",           "UI", "Slider",                  "200x20 の Image + Slider。値に応じて Image の fillAmount が変わる", &MakeUISlider, PresetPlace::UIElement },
    { "ui.inputField",       "UI", "Input Field",             "背景 Image + InputField + Text。入力内容が同じ Text へ書かれる", &MakeUIInputField, PresetPlace::UIElement },
    { "ui.scrollView",       "UI", "Scroll View",             "背景 Image + ScrollView + Mask + 縦並びの子 Content", &MakeUIScrollView, PresetPlace::UIElement },
    { "ui.mask",             "UI", "Mask",                    "200x200 の Image + Mask。子を矩形で切り抜く", &MakeUIMask, PresetPlace::UIElement },
    { "ui.panel",            "UI", "Panel",                   "画面を覆う半透明の背景パネル", &MakeUIPanel, PresetPlace::UIElement },
    { "ui.layoutHorizontal", "UI", "Horizontal Layout Group", "子を横に並べる (spacing 8)", &MakeUIHorizontalLayout, PresetPlace::UIElement },
    { "ui.layoutVertical",   "UI", "Vertical Layout Group",   "子を縦に並べる (spacing 8)", &MakeUIVerticalLayout, PresetPlace::UIElement },
    { "ui.layoutGrid",       "UI", "Grid Layout Group",       "子を 100x100 のマスで折り返して並べる (spacing 8)", &MakeUIGridLayout, PresetPlace::UIElement },
    { "ui.canvasGroup",      "UI", "Canvas Group",            "CanvasGroup。配下の UI をまとめて薄くする・触れなくする", &MakeUICanvasGroup, PresetPlace::UIElement },
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

} // namespace fbzz::editor
