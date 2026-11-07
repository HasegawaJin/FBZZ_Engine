/// @file    RenderSceneExtractorTests.cpp
/// @brief   Scene から独立したメッシュ資源・姿勢・submesh 対応の抽出を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-21
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/OwnedResources.hpp>
#include "../../../Engine/src/Scene/Managed/ManagedRenderComponentServices.hpp"
#include "../../../Engine/src/Scene/Managed/ManagedServicesBridge.hpp"
#include "../../../Engine/src/Scene/Systems/MaterialDemandTrace.hpp"
#include <limits>
#include <TestKit/Engine/EngineFixture.hpp>
#include <TestKit/TempDir.hpp>
#include <Core/Profiler/Profiler.hpp>
#include <Engine/Core/Scheduler/SystemContext.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Scene/Systems/RenderSceneExtractor.hpp>
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <Engine/Scene/Systems/ScriptSystem.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/SceneSerializer.hpp>
#include <Engine/Scene/SceneUtils.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Engine/Scene/Components/CameraComponent.hpp>
#include <Engine/Scene/ScriptComponent.hpp>
#include <Engine/Scene/Components/TrailComponent.hpp>
#include <Engine/Scene/Components/SunMoonRenderer.hpp>
#include <Engine/Scene/Components/SkyRenderer.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <Engine/Scene/Components/LODGroupComponent.hpp>
#include <Engine/Asset/Model.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Scene/MeshResolver.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Renderer/Mesh.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Graphics/Renderer/IRenderer.hpp>
#include <Graphics/Renderer/ResourceManager.hpp>
#include <Physics/World.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

namespace fbzz::tests {
namespace {

/// @note Tests author through the same owned draft/publication path as production; no mutable stored alias is exposed.
template<typename Owner, typename Change>
void EditMaterialAuthoring(Owner& owner, Change&& change)
{
    auto edit = owner.BeginAuthoringEdit();
    change(edit.Draft());
    EXPECT_NE(edit.Commit(owner), asset::detail::MaterialAuthoringCommitResult::CONFLICT);
}


class RenderSceneExtractorTest : public testkit::EngineFixture {
protected:
    scene::Scene m_scene;
    renderer::Mesh m_mesh;
    asset::Model m_model;

    scene::GameObject& StaticObject()
    {
        auto& go = m_scene.CreateGameObject("Mesh");
        scene::MeshRenderer component;
        component.mesh = &m_mesh;
        go.AddComponent<scene::MeshRenderer>(component);
        return go;
    }

    scene::GameObject& SkinnedObject()
    {
        for (uint32_t i = 0; i < 3; ++i) {
            auto mesh = std::make_unique<renderer::Mesh>();
            mesh->vertexBuffer = { 10 + i, 2 };
            mesh->indexBuffer = { 20 + i, 3 };
            mesh->indexCount = 30 + i;
            mesh->vertexCount = 40 + i;
            mesh->boundsRadius = 1.0f + static_cast<float>(i);
            m_model.meshes.push_back(std::move(mesh));
        }
        auto& go = m_scene.CreateGameObject("Skinned");
        scene::SkinnedMeshRenderer component;
        component.model = &m_model;
        component.submeshIndices = { 2, 0 };
        go.AddComponent<scene::SkinnedMeshRenderer>(component);
        return go;
    }
};

/// @note Restore the invoking process's comparison setting after each deterministic CPU extraction test.
class SpatialProducerEnvironment {
public:
    explicit SpatialProducerEnvironment(const char* mode = "journal")
    {
        if (const char* value = std::getenv("FBZZ_MESH_SPATIAL_PRODUCER")) m_previous = value;
        _putenv_s("FBZZ_MESH_SPATIAL_PRODUCER", mode ? mode : "");
    }
    ~SpatialProducerEnvironment()
    {
        _putenv_s("FBZZ_MESH_SPATIAL_PRODUCER", m_previous ? m_previous->c_str() : "");
    }
private:
    std::optional<std::string> m_previous;
};

/// @note Restore the caller's comparison mode; extraction reads the selection anew for each independent view.
class ExtractionScanEnvironment {
public:
    explicit ExtractionScanEnvironment(const char* mode)
    {
        if (const char* value = std::getenv("FBZZ_RENDER_EXTRACTION_SCAN_MODE")) m_previous = value;
        _putenv_s("FBZZ_RENDER_EXTRACTION_SCAN_MODE", mode ? mode : "");
    }
    ~ExtractionScanEnvironment()
    {
        _putenv_s("FBZZ_RENDER_EXTRACTION_SCAN_MODE", m_previous ? m_previous->c_str() : "");
    }
private:
    std::optional<std::string> m_previous;
};

class GeometryRestoreEnvironment {
public:
    explicit GeometryRestoreEnvironment(const char* mode)
    {
        if (const char* value = std::getenv("FBZZ_RENDER_GEOMETRY_RESTORE_MODE")) m_previous = value;
        _putenv_s("FBZZ_RENDER_GEOMETRY_RESTORE_MODE", mode ? mode : "");
    }
    ~GeometryRestoreEnvironment()
    {
        _putenv_s("FBZZ_RENDER_GEOMETRY_RESTORE_MODE", m_previous ? m_previous->c_str() : "");
    }
private:
    std::optional<std::string> m_previous;
};

/// @note Trigger a one-shot extraction callback through the existing AssetManager importer boundary.
class ExtractionCallbackMaterialImporter final : public asset::IAssetImporter<asset::MaterialAsset> {
public:
    std::function<void()> beforeImport;
    std::unique_ptr<asset::MaterialAsset> Import(const std::string&, renderer::ResourceManager*) override
    {
        auto callback = std::move(beforeImport);
        if (callback) callback();
        return std::make_unique<asset::MaterialAsset>();
    }
    std::span<const std::string_view> SupportedExtensions() const override
    {
        static constexpr std::string_view extensions[] = {".mat"};
        return extensions;
    }
};

class RenderSpatialPublicationTest : public RenderSceneExtractorTest {
protected:
    renderer::RenderScene Warm(uint64_t frame = 10)
    {
        (void)scene::ExtractRenderSceneGeometry(m_scene, frame);
        (void)scene::ExtractRenderSceneGeometry(m_scene, frame);
        return scene::ExtractRenderSceneGeometry(m_scene, frame);
    }
private:
    SpatialProducerEnvironment m_mode;
};

class ExtractionDestroyObserver final : public scene::Script {
public:
    ExtractionDestroyObserver(scene::Scene& sourceScene, renderer::RenderScene& snapshot)
        : m_sourceScene(sourceScene), m_snapshot(snapshot) {}
    bool ExecuteInEditMode() const override { return true; }
    void OnDestroy() override { m_snapshot = scene::ExtractRenderSceneGeometry(m_sourceScene, 10); }
private:
    scene::Scene& m_sourceScene;
    renderer::RenderScene& m_snapshot;
};

/// @note 材質の CPU ロードだけを提供し、抽出が GPU 資源の生成に依存しない境界を保つ。
class ExtractionCpuRenderer final : public renderer::IRenderer {
public:
    void Shutdown() override {}
    void BeginFrame() override {}
    void EndFrame() override {}
    void Clear(const math::Vector4&) override {}
    void Submit(const renderer::DrawCall&, renderer::ResourceManager&) override {}
    void Dispatch(const renderer::ComputeCall&, renderer::ResourceManager&) override {}
    void Resize(uint32_t, uint32_t) override {}
    uint32_t GetWidth() const override { return 1; }
    uint32_t GetHeight() const override { return 1; }
    void SetRenderTarget(renderer::ResourceHandle<renderer::RenderTargetTag>, renderer::ResourceManager&) override {}
    void ClearDepth() override {}
    void SetViewport(uint32_t, uint32_t, uint32_t, uint32_t) override {}
private:
    std::unique_ptr<renderer::IBuffer> CreateNativeVertexBuffer(const void*, size_t, uint32_t) override { return {}; }
    std::unique_ptr<renderer::IBuffer> CreateNativeIndexBuffer(const void*, uint32_t) override { return {}; }
    std::unique_ptr<renderer::IConstantBuffer> CreateNativeConstantBuffer(size_t) override { return {}; }
    std::unique_ptr<renderer::IShader> CreateNativeShader(const std::string&) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeTexture(const std::string&) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeTextureFromData(const uint8_t*, uint32_t, uint32_t) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeTexture3DFromData(const uint8_t*, uint32_t, uint32_t, uint32_t) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeTextureFromRenderTarget(renderer::IRenderTarget&, uint32_t, renderer::RenderTargetTextureKind) override { return {}; }
    std::unique_ptr<renderer::IPipelineState> CreateNativePipelineState(const renderer::PipelineStateDesc&) override { return {}; }
    std::unique_ptr<renderer::IRenderTarget> CreateNativeRenderTarget(uint32_t, uint32_t, const renderer::RenderTargetDesc&) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeComputeTexture(uint32_t, uint32_t) override { return {}; }
    std::unique_ptr<renderer::IStructuredBuffer> CreateNativeStructuredBuffer(const void*, uint32_t, uint32_t) override { return {}; }
    std::unique_ptr<renderer::IStructuredBuffer> CreateNativeRWStructuredBuffer(const void*, uint32_t, uint32_t) override { return {}; }
};

class RenderSceneExtractorMaterialTest : public RenderSceneExtractorTest {
protected:
    void SetUp() override
    {
        RenderSceneExtractorTest::SetUp();
        m_previousFrame = Time::frameCount;
        m_wasProfilerEnabled = profiler::Profiler::IsEnabled();
        profiler::Profiler::SetEnabled(false);
        profiler::Profiler::SetEnabled(true);
        ASSERT_TRUE(m_temp.IsValid());
        m_resources = std::make_unique<renderer::ResourceManager>(m_renderer);
        asset::AssetManager::Init(*m_resources, m_temp.Path().generic_string() + "/");
        m_assetsInitialized = true;
    }

    void TearDown() override
    {
        m_scene.Clear();
        if (m_assetsInitialized) asset::AssetManager::UnloadAll();
        m_resources.reset();
        Time::frameCount = m_previousFrame;
        profiler::Profiler::SetEnabled(false);
        profiler::Profiler::SetEnabled(m_wasProfilerEnabled);
        RenderSceneExtractorTest::TearDown();
    }

    std::string ShaderPath(const std::string& name) const { return m_temp.File(name).generic_string(); }

    bool WriteShader(const std::string& name, bool supportsGBuffer)
    {
        const std::string path = ShaderPath(name);
        const std::string declaration = supportsGBuffer
            ? "[shader]\nversion=1\nvertex='standard_surface_v1'\nsurface='metallic_roughness_v1'\nopacity='alpha_clip_v1'\nvariants='standard_surface_v1'\n"
            : "[shader]\nversion=1\nvertex='custom'\nsurface='custom'\nopacity='custom'\nvariants='none'\n";
        return util::FileSystem::WriteText(path, "float4 PSMain() : SV_Target { return 1; }\n")
            && util::FileSystem::WriteText(path + ".meta", declaration);
    }

    asset::AssetHandle<asset::MaterialAsset> LoadMaterial(const std::string& name, const asset::MaterialAssetAuthoring& material)
    {
        const std::string path = m_temp.File(name).generic_string();
        if (!asset::SaveMaterialAssetToFile(path, material)) return {};
        return asset::AssetManager::Load<asset::MaterialAsset>(path);
    }

    scene::MaterialComponent& AddMaterialObject(asset::AssetHandle<asset::MaterialAsset> handle)
    {
        auto& component = StaticObject().AddComponent<scene::MaterialComponent>();
        component.SetMaterialAsset(handle);
        return component;
    }

    renderer::RenderScene ExtractWithProfile(uint64_t frameStamp)
    {
        profiler::Profiler::BeginFrame();
        auto result = scene::ExtractRenderSceneGeometry(m_scene, frameStamp);
        profiler::Profiler::EndFrame();
        return result;
    }

    size_t ShaderCapabilitySampleCount() const
    {
        const auto& records = profiler::Profiler::GetLastFrameRecords();
        return static_cast<size_t>(std::count_if(records.begin(), records.end(), [](const auto& record) {
            return std::string_view(record.name) == "RenderSceneExtractor::ShaderCapabilities";
        }));
    }

    std::shared_ptr<const renderer::RenderScene> ExtractView(const renderer::RenderSettings& settings,
        scene::RenderFrameGeometryCache& cache, uint32_t layerMask = UINT32_MAX, bool experimentalRayTracing = false,
        float boundsPadding = 0.0f)
    {
        renderer::Camera camera;
        renderer::RenderPassHandles handles;
        scene::RenderPassContext context(m_scene, m_renderer, *m_resources, camera, settings, {}, layerMask, handles);
        context.experimentalRayTracingEnabled = experimentalRayTracing;
        context.cullingBoundsPadding = boundsPadding;
        scene::ExtractRenderScene(context, &cache);
        return context.renderScene;
    }

    testkit::TempDir m_temp{"render-scene-materials"};
private:
    ExtractionCpuRenderer m_renderer;
    std::unique_ptr<renderer::ResourceManager> m_resources;
    bool m_assetsInitialized = false;
    bool m_wasProfilerEnabled = true;
    uint64_t m_previousFrame = 0;
};

TEST_F(RenderSceneExtractorMaterialTest, RetainedSnapshotsStayImmutableAfterWorkerRetirementAndSceneClear)
{
    m_mesh.vertexBuffer = {11, 2}; m_mesh.indexBuffer = {12, 3};
    m_mesh.vertexCount = 3; m_mesh.indexCount = 3; m_mesh.boundsRadius = 1;
    auto& object = StaticObject();
    auto& material = object.AddComponent<scene::MaterialComponent>();
    material.enabled = false;
    object.transform.position = object.transform.worldPosition = {1, 0, 0};
    renderer::RenderSettings settings;
    scene::RenderFrameGeometryCache firstCache;
    Time::frameCount = 1;
    const auto retained = ExtractView(settings, firstCache);
    ASSERT_EQ(retained->Objects().size(), 1u);
    ASSERT_EQ(retained->items.size(), 1u);
    firstCache = {};

    object.transform.position = object.transform.worldPosition = {2, 0, 0};
    scene::RenderFrameGeometryCache secondCache;
    Time::frameCount = 2;
    auto retiring = ExtractView(settings, secondCache);
    ASSERT_EQ(retiring->items.size(), 1u);
    secondCache = {};
    std::thread worker([snapshot = std::move(retiring)]() mutable { snapshot.reset(); });
    worker.join();

    object.transform.position = object.transform.worldPosition = {3, 0, 0};
    m_mesh.vertexCount = 6; m_mesh.indexCount = 12;
    material.SetVisible(false);
    scene::RenderFrameGeometryCache thirdCache;
    Time::frameCount = 3;
    auto current = ExtractView(settings, thirdCache);
    ASSERT_EQ(current->Objects().size(), 1u);
    ASSERT_EQ(current->items.size(), 1u);
    EXPECT_FLOAT_EQ(current->Objects()[0].world.m[0][3], 3);
    EXPECT_FLOAT_EQ(current->Objects()[0].previousWorld.m[0][3], 2);
    EXPECT_EQ(current->items[0].vertexCount, 6u);
    EXPECT_EQ(current->items[0].indexCount, 12u);
    EXPECT_FALSE(current->items[0].slotVisible);
    EXPECT_FLOAT_EQ(retained->Objects()[0].world.m[0][3], 1);
    EXPECT_EQ(retained->items[0].vertexCount, 3u);
    EXPECT_TRUE(retained->items[0].slotVisible);

    current.reset(); thirdCache = {};
    m_scene.Clear();
    scene::RenderFrameGeometryCache emptyCache;
    Time::frameCount = 4;
    const auto empty = ExtractView(settings, emptyCache);
    EXPECT_TRUE(empty->Objects().empty());
    EXPECT_TRUE(empty->items.empty());
    EXPECT_FLOAT_EQ(retained->Objects()[0].world.m[0][3], 1);
    EXPECT_EQ(retained->items[0].vertexCount, 3u);
}

TEST_F(RenderSceneExtractorTest, SparseCameraQueriesPreserveScenePriorityAndObserveSameFrameEdits)
{
    auto& first = m_scene.CreateGameObject("FirstCameraOwner");
    for (int i = 0; i < 512; ++i) m_scene.CreateGameObject("UnrelatedBone");
    auto& second = m_scene.CreateGameObject("SecondCameraOwner");
    auto& secondCamera = second.AddComponent<scene::CameraComponent>();
    auto& firstCamera = first.AddComponent<scene::CameraComponent>();
    first.transform.worldPosition = {1, 2, 3}; second.transform.worldPosition = {9, 8, 7};
    firstCamera.fovY = 41; secondCamera.fovY = 73;
    firstCamera.cullingMask = 3; secondCamera.cullingMask = 5;
    firstCamera.backgroundColor = {0.1f, 0.2f, 0.3f, 1};
    secondCamera.backgroundColor = {0.7f, 0.6f, 0.5f, 1};
    firstCamera.maxDrawDistance = -4; firstCamera.layerCullDistances[3] = -2;
    renderer::Camera editorFallback;
    editorFallback.m_position = {100, 200, 300};
    auto result = scene::ResolveGameCamera(m_scene, 1.75f);
    EXPECT_VEC3_NEAR(result.m_position, first.transform.worldPosition, 0.0f);
    EXPECT_FLOAT_EQ(result.m_fovY, 41); EXPECT_FLOAT_EQ(firstCamera.aspectRatio, 1.75f);
    EXPECT_EQ(scene::ResolveGameCullingMask(m_scene), 3u);
    EXPECT_FLOAT_EQ(scene::ResolveGameBackgroundColor(m_scene).x, 0.1f);
    EXPECT_FLOAT_EQ(scene::ResolveEditorGameCamera(m_scene, editorFallback, 2.0f).m_fovY, 41);
    EXPECT_FLOAT_EQ(scene::ResolveGameCullingSettings(m_scene).maxDrawDistance, 0);
    EXPECT_FLOAT_EQ(scene::ResolveGameCullingSettings(m_scene).layerCullDistances[3], 0);
    ASSERT_TRUE(m_scene.MoveGameObjectToIndex(second.GetID(), 0));
    EXPECT_FLOAT_EQ(scene::ResolveGameCamera(m_scene, 1.5f).m_fovY, 73);
    secondCamera.enabled = false;
    firstCamera.fovY = 59;
    EXPECT_FLOAT_EQ(scene::ResolveGameCamera(m_scene, 1.5f).m_fovY, 59);
    first.SetActive(false);
    EXPECT_EQ(scene::ResolveGameCullingMask(m_scene), fbzz::Layer::Everything);
    EXPECT_FLOAT_EQ(scene::ResolveGameCamera(m_scene, 1.25f).m_aspect, 1.25f);
    EXPECT_VEC3_NEAR(scene::ResolveEditorGameCamera(m_scene, editorFallback, 1.25f).m_position,
        editorFallback.m_position, 0.0f);
    first.SetActive(true); secondCamera.enabled = true;
    second.RemoveComponent<scene::CameraComponent>();
    EXPECT_FLOAT_EQ(scene::ResolveGameCamera(m_scene, 1.5f).m_fovY, 59);
}

TEST_F(RenderSceneExtractorTest, KeepsOffscreenAndOtherLayerCandidatesButRejectsInactiveHierarchy)
{
    auto& go = StaticObject();
    go.layer = 7;
    go.transform.position = { 10000.0f, 0.0f, 0.0f };
    go.transform.worldPosition = go.transform.position;
    go.GetComponent<scene::MeshRenderer>()->lodVisible = false;
    auto snapshot = scene::ExtractRenderSceneGeometry(m_scene, 10);
    ASSERT_EQ(snapshot.objects.size(), 1u);
    EXPECT_EQ(snapshot.objects[0].layer, 7u);
    EXPECT_FALSE(snapshot.objects[0].lodVisible);
    auto& parent = m_scene.CreateGameObject("Parent");
    go.SetParent(parent);
    parent.SetActive(false);
    EXPECT_TRUE(scene::ExtractRenderSceneGeometry(m_scene, 10).objects.empty());
}

TEST_F(RenderSceneExtractorTest, PreservesSceneOrderForLateRendererRegistrationAndMixedRenderersAmongBones)
{
    auto& first = m_scene.CreateGameObject("First");
    auto* parent = &first;
    for (int index = 0; index < 64; ++index) {
        auto& bone = m_scene.CreateGameObject("Bone");
        ASSERT_TRUE(bone.SetParent(parent));
        parent = &bone;
    }
    auto& mixed = SkinnedObject();
    ASSERT_TRUE(mixed.SetParent(parent));
    auto& last = StaticObject();
    scene::MeshRenderer mesh;
    mesh.mesh = &m_mesh;
    first.AddComponent<scene::MeshRenderer>(mesh);
    mixed.AddComponent<scene::MeshRenderer>(mesh);
    const auto firstId = first.GetID();
    const auto mixedId = mixed.GetID();
    const auto lastId = last.GetID();

    const auto snapshot = scene::ExtractRenderSceneGeometry(m_scene, 10);

    ASSERT_EQ(snapshot.objects.size(), 4u);
    ASSERT_EQ(snapshot.items.size(), 5u);
    EXPECT_EQ(snapshot.objects[0].sourceIndex, firstId.index);
    EXPECT_EQ(snapshot.objects[1].sourceIndex, mixedId.index);
    EXPECT_EQ(snapshot.objects[2].sourceIndex, mixedId.index);
    EXPECT_EQ(snapshot.objects[3].sourceIndex, lastId.index);
    EXPECT_FALSE(snapshot.objects[0].skinned);
    EXPECT_FALSE(snapshot.objects[1].skinned);
    EXPECT_TRUE(snapshot.objects[2].skinned);
    EXPECT_FALSE(snapshot.objects[3].skinned);
    EXPECT_EQ(snapshot.objects[2].firstItem, 2u);
    EXPECT_EQ(snapshot.objects[2].itemCount, 2u);
    EXPECT_EQ(snapshot.items[2].objectIndex, 2u);
    EXPECT_EQ(snapshot.items[3].objectIndex, 2u);
    EXPECT_EQ(snapshot.items[4].objectIndex, 3u);

    ASSERT_TRUE(m_scene.MoveGameObjectToIndex(lastId, 0));
    const auto reordered = scene::ExtractRenderSceneGeometry(m_scene, 10);

    ASSERT_EQ(reordered.objects.size(), 4u);
    EXPECT_EQ(reordered.objects[0].sourceIndex, lastId.index);
    EXPECT_EQ(reordered.objects[1].sourceIndex, firstId.index);
    EXPECT_EQ(reordered.objects[2].sourceIndex, mixedId.index);
    EXPECT_FALSE(reordered.objects[2].skinned);
    EXPECT_EQ(reordered.objects[3].sourceIndex, mixedId.index);
    EXPECT_TRUE(reordered.objects[3].skinned);
    EXPECT_EQ(snapshot.objects[0].sourceIndex, firstId.index);
}

TEST_F(RenderSceneExtractorTest, RefreshesRendererMembershipAndHierarchyActivityWithinTheSameFrame)
{
    auto& first = StaticObject();
    auto& second = StaticObject();
    auto& parent = m_scene.CreateGameObject("Parent");
    const auto firstId = first.GetID();
    const auto secondId = second.GetID();
    const auto initial = scene::ExtractRenderSceneGeometry(m_scene, 10);
    ASSERT_EQ(initial.objects.size(), 2u);

    first.RemoveComponent<scene::MeshRenderer>();
    const auto removed = scene::ExtractRenderSceneGeometry(m_scene, 10);

    ASSERT_EQ(removed.objects.size(), 1u);
    EXPECT_EQ(removed.objects[0].sourceIndex, secondId.index);

    scene::MeshRenderer mesh;
    mesh.mesh = &m_mesh;
    first.AddComponent<scene::MeshRenderer>(mesh);
    ASSERT_TRUE(second.SetParent(&parent));
    parent.SetActive(false);
    const auto inactive = scene::ExtractRenderSceneGeometry(m_scene, 10);

    ASSERT_EQ(inactive.objects.size(), 1u);
    EXPECT_EQ(inactive.objects[0].sourceIndex, firstId.index);

    ASSERT_TRUE(second.ClearParent());
    const auto restored = scene::ExtractRenderSceneGeometry(m_scene, 10);

    ASSERT_EQ(restored.objects.size(), 2u);
    EXPECT_EQ(restored.objects[0].sourceIndex, firstId.index);
    EXPECT_EQ(restored.objects[1].sourceIndex, secondId.index);
    EXPECT_EQ(initial.objects[0].sourceIndex, firstId.index);
    EXPECT_EQ(initial.objects[1].sourceIndex, secondId.index);
}

TEST_F(RenderSceneExtractorTest, PreservesOrderAcrossDestroyCallbacksCompactionAndEntityReuse)
{
    auto& first = StaticObject();
    auto& second = StaticObject();
    auto& survivor = SkinnedObject();
    const auto firstId = first.GetID();
    const auto secondId = second.GetID();
    const auto survivorId = survivor.GetID();
    renderer::RenderScene duringDestroy;
    auto& scripts = second.AddComponent<scene::ScriptComponent>();
    scripts.scripts.emplace_back().script = std::make_unique<ExtractionDestroyObserver>(m_scene, duringDestroy);
    physics::World world;
    const bool playing = scene::Script::IsInPlayMode();
    SystemContext context{m_scene, world, nullptr, nullptr, 0.0f, 0.02f, playing, playing};
    scene::ScriptSystem{}.Update(context);
    const scene::EntityID destroyed[]{firstId, secondId};

    m_scene.DestroyGameObjects(destroyed);
    auto& replacement = StaticObject();
    const auto replacementId = replacement.GetID();
    const auto afterDestroy = scene::ExtractRenderSceneGeometry(m_scene, 10);

    ASSERT_EQ(duringDestroy.objects.size(), 2u);
    EXPECT_EQ(duringDestroy.objects[0].sourceIndex, secondId.index);
    EXPECT_EQ(duringDestroy.objects[0].sourceGeneration, secondId.generation);
    EXPECT_EQ(duringDestroy.objects[1].sourceIndex, survivorId.index);
    EXPECT_EQ(replacementId.index, secondId.index);
    EXPECT_NE(replacementId.generation, secondId.generation);
    EXPECT_EQ(m_scene.GetGameObject(firstId), nullptr);
    EXPECT_EQ(m_scene.GetGameObject(secondId), nullptr);
    ASSERT_EQ(afterDestroy.objects.size(), 2u);
    EXPECT_EQ(afterDestroy.objects[0].sourceIndex, survivorId.index);
    EXPECT_EQ(afterDestroy.objects[1].sourceIndex, replacementId.index);
    EXPECT_EQ(afterDestroy.objects[1].sourceGeneration, replacementId.generation);
}

TEST_F(RenderSceneExtractorTest, SelectsCanonicalLodZeroWithoutFilteringRasterHiddenRenderers)
{
    auto& selected = StaticObject();
    const auto selectedId = selected.GetID();
    selected.GetComponent<scene::MeshRenderer>()->lodVisible = true;
    selected.GetComponent<scene::MeshRenderer>()->lodDither = 0.0f;
    auto& hidden = SkinnedObject();
    const auto hiddenId = hidden.GetID();
    hidden.GetComponent<scene::SkinnedMeshRenderer>()->lodVisible = false;
    hidden.GetComponent<scene::SkinnedMeshRenderer>()->lodDither = 0.0f;
    const auto unrelatedId = StaticObject().GetID();
    scene::LODGroupComponent group;
    group.activeLevel = 0;
    group.levels = {
        { 0.75f, { { {}, selectedId } } },
        { 0.25f, { { {}, hiddenId } } }
    };
    m_scene.CreateGameObject("LOD Group").AddComponent<scene::LODGroupComponent>(group);

    const auto snapshot = scene::ExtractRenderSceneGeometry(m_scene, 10, {}, true);
    ASSERT_EQ(snapshot.objects.size(), 3u);
    for (const auto& object : snapshot.objects) {
        const scene::EntityID source{ object.sourceIndex, object.sourceGeneration };
        EXPECT_FALSE(object.rayLodSelectionRequired);
        EXPECT_EQ(object.rayVisible, source != hiddenId);
        if (source == selectedId) EXPECT_TRUE(object.lodVisible);
        if (source == hiddenId) EXPECT_FALSE(object.lodVisible);
        if (source == unrelatedId) EXPECT_FALSE(object.rayLodSelectionRequired);
        EXPECT_FLOAT_EQ(object.lodDither, 0.0f);
    }
}

TEST_F(RenderSceneExtractorTest, ResolvesLodGuidForMembershipWithoutChangingReferences)
{
    auto& member = StaticObject();
    const auto memberId = member.GetID();
    const auto instanceId = member.instanceId;
    ASSERT_FALSE(instanceId.empty());
    StaticObject();
    scene::LODGroupComponent group;
    group.levels = { { 0.5f, { { instanceId, scene::EntityID::INVALID } } } };
    auto& owner = m_scene.CreateGameObject("LOD Group");
    owner.AddComponent<scene::LODGroupComponent>(group);

    const auto snapshot = scene::ExtractRenderSceneGeometry(m_scene, 10, {}, true);
    ASSERT_EQ(snapshot.objects.size(), 2u);
    for (const auto& object : snapshot.objects) {
        const scene::EntityID source{ object.sourceIndex, object.sourceGeneration };
        EXPECT_FALSE(object.rayLodSelectionRequired);
        EXPECT_TRUE(object.rayVisible);
    }
    const auto* unchanged = owner.GetComponent<scene::LODGroupComponent>();
    ASSERT_NE(unchanged, nullptr);
    ASSERT_EQ(unchanged->levels.size(), 1u);
    ASSERT_EQ(unchanged->levels[0].renderers.size(), 1u);
    EXPECT_EQ(unchanged->levels[0].renderers[0].entity, scene::EntityID::INVALID);
    EXPECT_EQ(unchanged->levels[0].renderers[0].instanceId, instanceId);
}

TEST_F(RenderSceneExtractorTest, SkinnedTextureQualityUsesAnimatedBoundsInsteadOfBindPose)
{
    auto& go = SkinnedObject();
    scene::AnimatorComponent animator;
    animator.skinnedBoundsCenter = {0.0f, 0.0f, 100.0f};
    animator.skinnedBoundsRadius = 7.0f;
    go.AddComponent<scene::AnimatorComponent>(animator);
    const auto snapshot = scene::ExtractRenderSceneGeometry(m_scene, 1);
    ASSERT_EQ(snapshot.objects.size(), 1u);
    const auto& object = snapshot.objects[0];
    const auto& item = snapshot.items[0];
    EXPECT_FLOAT_EQ(object.boundsRadius, 10.0f);
    EXPECT_FLOAT_EQ(scene::EstimateRenderTexturePixels(object, item, {}, 1.0f, 1000, false), 100.0f);
    EXPECT_FLOAT_EQ(scene::EstimateRenderTexturePixels(object, item, {0,0,90}, 1.0f, 1000, false), 1000.0f);
    EXPECT_FLOAT_EQ(scene::EstimateRenderTexturePixels(object, item, {}, 0.1f, 1000, true), 1000.0f);
    EXPECT_FLOAT_EQ(scene::EstimateRenderTexturePixels(object, item, {}, 0.0f, 1000, false), 0.0f);
}

TEST_F(RenderSceneExtractorTest, CopiesWorldBoundsAndHandlesWithoutOwningTheSourceMesh)
{
    auto& go = StaticObject();
    go.transform.worldPosition = { 10.0f, 2.0f, 3.0f };
    m_mesh.boundsCenter = { 1.0f, 0.0f, 0.0f };
    m_mesh.boundsRadius = 2.0f;
    m_mesh.vertexBuffer = { 11, 4 };
    m_mesh.indexBuffer = { 12, 5 };
    m_mesh.indexCount = 36;
    const auto snapshot = scene::ExtractRenderSceneGeometry(m_scene, 10);
    ASSERT_EQ(snapshot.items.size(), 1u);
    EXPECT_VEC3_NEAR(snapshot.objects[0].boundsCenter, (math::Vector3{ 11.0f, 2.0f, 3.0f }), testkit::kTolerance);
    EXPECT_FLOAT_EQ(snapshot.objects[0].boundsRadius, 2.0f);
    m_mesh.vertexBuffer = {};
    m_mesh.indexCount = 0;
    go.transform.worldPosition = {};
    EXPECT_EQ(snapshot.items[0].vertexBuffer, (renderer::ResourceHandle<renderer::BufferTag>{ 11, 4 }));
    EXPECT_EQ(snapshot.items[0].indexCount, 36u);
    EXPECT_FLOAT_EQ(snapshot.objects[0].world.m[0][3], 10.0f);
}

TEST_F(RenderSceneExtractorTest, WarmRendererIndexKeepsCurrentValuesAndOldGeometrySnapshotsIndependent)
{
    auto& object = StaticObject();
    auto& skinned = SkinnedObject();
    object.transform.worldPosition = {1, 2, 3};
    m_mesh.vertexCount = 5;
    const auto first = scene::ExtractRenderSceneGeometry(m_scene, 10);
    ASSERT_EQ(first.objects.size(), 2u);
    ASSERT_EQ(first.items.size(), 3u);

    object.transform.worldPosition = {9, 2, 3};
    m_mesh.vertexCount = 17;
    skinned.GetComponent<scene::SkinnedMeshRenderer>()->enabled = false;
    const auto second = scene::ExtractRenderSceneGeometry(m_scene, 10);
    ASSERT_EQ(second.objects.size(), 1u);
    ASSERT_EQ(second.items.size(), 1u);
    EXPECT_FLOAT_EQ(second.objects[0].world.m[0][3], 9);
    EXPECT_EQ(second.items[0].vertexCount, 17u);
    object.GetComponent<scene::MeshRenderer>()->enabled = false;
    EXPECT_TRUE(scene::ExtractRenderSceneGeometry(m_scene, 10).objects.empty());
    object.GetComponent<scene::MeshRenderer>()->enabled = true;
    object.SetActive(false);
    EXPECT_TRUE(scene::ExtractRenderSceneGeometry(m_scene, 10).objects.empty());
    object.SetActive(true);
    object.GetComponent<scene::MeshRenderer>()->mesh = nullptr;
    EXPECT_TRUE(scene::ExtractRenderSceneGeometry(m_scene, 10).objects.empty());

    EXPECT_FLOAT_EQ(first.objects[0].world.m[0][3], 1);
    EXPECT_EQ(first.items[0].vertexCount, 5u);
    EXPECT_EQ(first.objects.size(), 2u);
}

TEST_F(RenderSceneExtractorTest, StaticBoundsUseCurrentRotatedNegativeNonuniformScaleAcrossViews)
{
    auto& go = StaticObject();
    go.transform.worldPosition = {10, 2, 3};
    go.transform.worldScale = {-2, 3, 4};
    go.transform.worldRotation = {0, 0, 0.70710678f, 0.70710678f};
    m_mesh.boundsCenter = {1, 2, 0};
    m_mesh.boundsRadius = 2;
    const auto first = scene::ExtractRenderSceneGeometry(m_scene, 10);
    ASSERT_EQ(first.items.size(), 1u);
    EXPECT_VEC3_NEAR(first.items[0].boundsCenter, (math::Vector3{4, 0, 3}), testkit::kTolerance);
    EXPECT_FLOAT_EQ(first.items[0].boundsRadius, 8);
    EXPECT_VEC3_NEAR(first.objects[0].boundsCenter, first.items[0].boundsCenter, testkit::kTolerance);

    go.transform.worldPosition = {-3, 4, 5};
    go.transform.worldScale = {-5, 1, 0.5f};
    go.transform.worldRotation = {};
    const auto second = scene::ExtractRenderSceneGeometry(m_scene, 10);
    ASSERT_EQ(second.items.size(), 1u);
    EXPECT_VEC3_NEAR(second.items[0].boundsCenter, (math::Vector3{-8, 6, 5}), testkit::kTolerance);
    EXPECT_FLOAT_EQ(second.items[0].boundsRadius, 10);
    EXPECT_VEC3_NEAR(second.objects[0].boundsCenter, second.items[0].boundsCenter, testkit::kTolerance);
    EXPECT_VEC3_NEAR(first.items[0].boundsCenter, (math::Vector3{4, 0, 3}), testkit::kTolerance);
    EXPECT_FLOAT_EQ(first.items[0].boundsRadius, 8);
}

TEST_F(RenderSceneExtractorTest, SkinnedSubmeshBoundsKeepRendererScaleAndAnimatorOwnerSphereIndependent)
{
    auto& go = SkinnedObject();
    go.transform.worldPosition = {10, 2, 3};
    go.transform.worldScale = {-2, 3, 4};
    m_model.meshes[2]->boundsCenter = {1, 0, 0};
    m_model.meshes[0]->boundsCenter = {-2, 0, 0};
    m_model.meshes[1]->boundsRadius = 200;
    const auto first = scene::ExtractRenderSceneGeometry(m_scene, 20);
    ASSERT_EQ(first.items.size(), 2u);
    EXPECT_VEC3_NEAR(first.items[0].boundsCenter, (math::Vector3{8, 2, 3}), testkit::kTolerance);
    EXPECT_VEC3_NEAR(first.items[1].boundsCenter, (math::Vector3{14, 2, 3}), testkit::kTolerance);
    EXPECT_FLOAT_EQ(first.items[0].boundsRadius, 12);
    EXPECT_FLOAT_EQ(first.items[1].boundsRadius, 4);
    EXPECT_VEC3_NEAR(first.objects[0].boundsCenter, (math::Vector3{9.5f, 2, 3}), testkit::kTolerance);
    EXPECT_FLOAT_EQ(first.objects[0].boundsRadius, 13.5f);

    go.GetComponent<scene::SkinnedMeshRenderer>()->submeshIndices = {0, 2};
    go.transform.worldScale = {-1, 2, 3};
    const auto reordered = scene::ExtractRenderSceneGeometry(m_scene, 20);
    ASSERT_EQ(reordered.items.size(), 2u);
    EXPECT_VEC3_NEAR(reordered.items[0].boundsCenter, (math::Vector3{12, 2, 3}), testkit::kTolerance);
    EXPECT_VEC3_NEAR(reordered.items[1].boundsCenter, (math::Vector3{9, 2, 3}), testkit::kTolerance);
    EXPECT_FLOAT_EQ(reordered.items[0].boundsRadius, 3);
    EXPECT_FLOAT_EQ(reordered.items[1].boundsRadius, 9);
    EXPECT_VEC3_NEAR(reordered.objects[0].boundsCenter, (math::Vector3{9.75f, 2, 3}), testkit::kTolerance);
    EXPECT_FLOAT_EQ(reordered.objects[0].boundsRadius, 9.75f);

    auto& parent = m_scene.CreateGameObject("BoundsAnimatorOwner");
    go.SetParent(parent);
    ASSERT_EQ(go.GetParent(), &parent);
    parent.transform.worldPosition = {100, 10, 0};
    parent.transform.worldScale = {-2, 3, 4};
    go.transform.worldPosition = {10, 2, 3};
    go.transform.worldScale = {-1, 2, 3};
    auto& animator = parent.AddComponent<scene::AnimatorComponent>();
    animator.skinnedBoundsCenter = {1, 0, 0};
    animator.skinnedBoundsRadius = 5;
    const auto animated = scene::ExtractRenderSceneGeometry(m_scene, 20);
    ASSERT_EQ(animated.items.size(), 2u);
    EXPECT_VEC3_NEAR(animated.objects[0].boundsCenter, (math::Vector3{98, 10, 0}), testkit::kTolerance);
    EXPECT_FLOAT_EQ(animated.objects[0].boundsRadius, 32);
    EXPECT_VEC3_NEAR(animated.items[0].boundsCenter, (math::Vector3{12, 2, 3}), testkit::kTolerance);
    EXPECT_VEC3_NEAR(animated.items[1].boundsCenter, (math::Vector3{9, 2, 3}), testkit::kTolerance);
    EXPECT_FLOAT_EQ(animated.items[0].boundsRadius, 3);
    EXPECT_FLOAT_EQ(animated.items[1].boundsRadius, 9);
    EXPECT_FLOAT_EQ(first.objects[0].boundsRadius, 13.5f);
}

TEST_F(RenderSceneExtractorMaterialTest, LeafWindBoundsFollowAssetOverridesAndScaledSpatialUpdates)
{
    SpatialProducerEnvironment producer;
    auto& go = StaticObject();
    go.transform.worldScale = {-2, 3, 4};
    m_mesh.boundsRadius = 2;
    m_mesh.occlusionRadius = 0.25f;
    asset::MaterialAssetAuthoring values;
    values.params["leafWindStrength"] = {0.25f};
    const auto handle = LoadMaterial("wind.mat", values);
    ASSERT_TRUE(handle.IsValid());
    auto& material = go.AddComponent<scene::MaterialComponent>();
    material.SetMaterialAsset(handle);
    const auto first = scene::ExtractRenderSceneGeometry(m_scene, 10);
    ASSERT_EQ(first.objects.size(), 1u);
    ASSERT_EQ(first.items.size(), 1u);
    EXPECT_FLOAT_EQ(first.objects[0].boundsRadius, 10);
    EXPECT_FLOAT_EQ(first.items[0].boundsRadius, 10);
    EXPECT_FLOAT_EQ(first.items[0].occluderRadius, 0);
    (void)scene::ExtractRenderSceneGeometry(m_scene, 10);

    EditMaterialAuthoring(material, [](auto& authored) { authored.paramOverrides["leafWindStrength"] = {0.5f}; });
    const auto overridden = scene::ExtractRenderSceneGeometry(m_scene, 10);
    EXPECT_FLOAT_EQ(overridden.objects[0].boundsRadius, 12);
    ASSERT_NE(overridden.spatialUpdate, nullptr);
    ASSERT_EQ(overridden.spatialUpdate->changedObjectOrdinals.size(), 1u);
    EXPECT_EQ(overridden.spatialUpdate->changedObjectOrdinals[0], 0u);

    EditMaterialAuthoring(material, [](auto& authored) { authored.integerParamOverrides["leafWindStrength"] = {1}; });
    EXPECT_FLOAT_EQ(scene::ExtractRenderSceneGeometry(m_scene, 10).objects[0].boundsRadius, 16);
    EditMaterialAuthoring(material, [](auto& authored) {
        authored.paramOverrides.clear(); authored.integerParamOverrides.clear();
    });
    auto* sourceAsset = asset::AssetManager::Get<asset::MaterialAsset>(handle);
    ASSERT_NE(sourceAsset, nullptr);
    EditMaterialAuthoring(*sourceAsset, [](auto& authored) { authored.params["leafWindStrength"] = {0}; });
    const auto rigid = scene::ExtractRenderSceneGeometry(m_scene, 10);
    EXPECT_FLOAT_EQ(rigid.objects[0].boundsRadius, 8);
    EXPECT_FLOAT_EQ(rigid.items[0].occluderRadius, 0.5f);
    EXPECT_FLOAT_EQ(first.objects[0].boundsRadius, 10);
}

TEST_F(RenderSceneExtractorTest, UnresolvedLeafMaterialBoundsFailOpenWithoutLoading)
{
    auto& go = StaticObject();
    m_mesh.boundsRadius = 1;
    auto& material = go.AddComponent<scene::MaterialComponent>();
    material.SetMaterialPath("UnresolvedLeafMaterial.mat");
    const auto unresolved = scene::ExtractRenderSceneGeometry(m_scene, 10);
    ASSERT_EQ(unresolved.objects.size(), 1u);
    EXPECT_FLOAT_EQ(unresolved.objects[0].boundsRadius, 0);
    EXPECT_FALSE(material.GetMaterialAsset().IsValid());
    EditMaterialAuthoring(material, [](auto& authored) { authored.paramOverrides["leafWindStrength"] = {0.5f}; });
    EXPECT_FLOAT_EQ(scene::ExtractRenderSceneGeometry(m_scene, 10).objects[0].boundsRadius, 2);
}

TEST_F(RenderSceneExtractorTest, SkinnedLeafWindUsesRendererScaleAndAllMaterialSlots)
{
    auto& go = SkinnedObject();
    go.transform.worldScale = {-2, 3, 4};
    auto& animator = go.AddComponent<scene::AnimatorComponent>();
    animator.skinnedBoundsRadius = 5;
    auto& material = go.AddComponent<scene::MaterialComponent>();
    EditMaterialAuthoring(material, [](auto& authored) { authored.paramOverrides["leafWindStrength"] = {0.25f}; });
    material.ResizeSlots(2);
    material.extraSlots[0].SetMaterialPath("SecondLeafMaterial.mat");
    EditMaterialAuthoring(material.extraSlots[0], [](auto& authored) { authored.paramOverrides["leafWindStrength"] = {0.5f}; });
    const auto animated = scene::ExtractRenderSceneGeometry(m_scene, 10);
    ASSERT_EQ(animated.objects.size(), 1u);
    EXPECT_FLOAT_EQ(animated.objects[0].boundsRadius, 36);
    animator.skinnedBoundsRadius = 0;
    const auto bindPose = scene::ExtractRenderSceneGeometry(m_scene, 10);
    EXPECT_FLOAT_EQ(bindPose.objects[0].boundsRadius, 16);
}

TEST_F(RenderSceneExtractorMaterialTest, LeafWindMaterialEditsRebuildSharedGeometryBetweenViews)
{
    auto& go = StaticObject();
    m_mesh.boundsRadius = 1;
    m_mesh.occlusionRadius = 0.25f;
    go.transform.worldScale = {-2, 3, 4};
    auto& material = go.AddComponent<scene::MaterialComponent>();
    renderer::RenderSettings settings;
    scene::RenderFrameGeometryCache cache;
    Time::frameCount = 10;
    const auto first = ExtractView(settings, cache, UINT32_MAX, false, 7.0f);
    ASSERT_EQ(first->Objects().size(), 1u);
    EXPECT_FLOAT_EQ(first->Objects()[0].boundsRadius, 4);
    EXPECT_FLOAT_EQ(first->items[0].occluderRadius, 0.5f);
    const auto warm = ExtractView(settings, cache, UINT32_MAX, false, 7.0f);
    EXPECT_EQ(warm->sharedObjects, first->sharedObjects);
    EXPECT_FLOAT_EQ(warm->items[0].occluderRadius, 0.5f);
    EditMaterialAuthoring(material, [](auto& authored) { authored.paramOverrides["leafWindStrength"] = {0.5f}; });
    const auto windy = ExtractView(settings, cache);
    EXPECT_FLOAT_EQ(windy->Objects()[0].boundsRadius, 8);
    EXPECT_FLOAT_EQ(windy->items[0].occluderRadius, 0);
    EXPECT_FLOAT_EQ(first->Objects()[0].boundsRadius, 4);
    const auto retainedGeometry = cache.geometry;
    const auto unchanged = ExtractView(settings, cache);
    EXPECT_EQ(cache.geometry, retainedGeometry);
    EXPECT_EQ(unchanged->sharedObjects, windy->sharedObjects);
}

TEST_F(RenderSceneExtractorMaterialTest, LeafWindImporterCallbacksFailOpenWithoutMutatingRetainedGeometry)
{
    struct RestoreImporter {
        std::unique_ptr<asset::IAssetImporter<asset::MaterialAsset>> previous;
        ~RestoreImporter() { asset::AssetManager::RegisterImporter<asset::MaterialAsset>(std::move(previous)); }
    } restore{std::move(asset::AssetStore<asset::MaterialAsset>::Get().importer)};
    for (const char* selectedMode : {"legacy", "fused"}) {
        ExtractionScanEnvironment mode(selectedMode);
        SpatialProducerEnvironment producer;
        m_scene.Clear();
        m_mesh.boundsRadius = 1;
        m_mesh.occlusionRadius = 0.25f;
        auto& leaves = StaticObject();
        leaves.AddComponent<scene::MaterialComponent>();
        auto& trigger = StaticObject();
        trigger.AddComponent<scene::MaterialComponent>().SetVisible(false);
        scene::RenderFrameGeometryCache cache;
        const auto first = ExtractView({}, cache);
        ASSERT_EQ(first->Objects().size(), 2u);
        EXPECT_FLOAT_EQ(first->Objects()[0].boundsRadius, 1);
        auto* triggerMaterial = trigger.GetComponent<scene::MaterialComponent>();
        triggerMaterial->SetMaterialPath(m_temp.File(std::string(selectedMode) + "-wind-callback.mat").generic_string());
        auto importer = std::make_unique<ExtractionCallbackMaterialImporter>();
        std::shared_ptr<const renderer::RenderScene> nested;
        importer->beforeImport = [&] {
            EditMaterialAuthoring(*leaves.GetComponent<scene::MaterialComponent>(), [](auto& authored) {
                authored.paramOverrides["leafWindStrength"] = {1};
            });
            nested = ExtractView({}, cache);
        };
        asset::AssetManager::RegisterImporter<asset::MaterialAsset>(std::move(importer));
        const auto outer = ExtractView({}, cache);
        ASSERT_NE(nested, nullptr);
        EXPECT_FLOAT_EQ(outer->Objects()[0].boundsRadius, 0);
        EXPECT_FLOAT_EQ(outer->items[0].occluderRadius, 0);
        EXPECT_EQ(outer->spatialUpdate, nullptr);
        EXPECT_FLOAT_EQ(nested->Objects()[0].boundsRadius, 3);
        EXPECT_FLOAT_EQ(first->Objects()[0].boundsRadius, 1);
        const auto current = ExtractView({}, cache);
        EXPECT_FLOAT_EQ(current->Objects()[0].boundsRadius, 3);
        EXPECT_FLOAT_EQ(outer->Objects()[0].boundsRadius, 0);
    }
}

TEST_F(RenderSceneExtractorTest, LocalSlotsResolveReorderedSubmeshesAndMorphInputs)
{
    auto& go = SkinnedObject();
    auto* smr = go.GetComponent<scene::SkinnedMeshRenderer>();
    smr->morphVertexBuffers.resize(3);
    smr->morphVertexBuffers[2] = renderer::ResourceHandle<renderer::BufferTag>{ 90, 7 };
    const auto snapshot = scene::ExtractRenderSceneGeometry(m_scene, 10);
    ASSERT_EQ(snapshot.items.size(), 2u);
    EXPECT_EQ(snapshot.items[0].sourceSubmesh, 2u);
    EXPECT_EQ(snapshot.items[0].materialSlot, 0u);
    EXPECT_EQ(snapshot.items[0].vertexBuffer.id, 12u);
    EXPECT_EQ(snapshot.items[0].skinningVertexBuffer.id, 90u);
    EXPECT_EQ(snapshot.items[1].sourceSubmesh, 0u);
    EXPECT_EQ(snapshot.items[1].materialSlot, 1u);
    EXPECT_EQ(snapshot.items[1].skinningVertexBuffer.id, 10u);
}

TEST_F(RenderSceneExtractorTest, DeformedOutputRequiresCurrentFrameAndMatchingModelIncludingMorph)
{
    auto& go = SkinnedObject();
    auto* smr = go.GetComponent<scene::SkinnedMeshRenderer>();
    smr->gpuSkinnedThisFrame = true;
    smr->gpuSkinningFrame = 10;
    smr->skinnedBufferModel = &m_model;
    smr->skinnedVertexBuffers = testkit::MakeOwnedResources<renderer::BufferTag>({ { 60, 1 }, { 61, 1 }, { 62, 1 } });
    auto snapshot = scene::ExtractRenderSceneGeometry(m_scene, 10);
    EXPECT_EQ(snapshot.items[0].deformedVertexBuffer.id, 62u);
    EXPECT_EQ(snapshot.items[1].deformedVertexBuffer.id, 60u);
    EXPECT_FALSE(scene::ExtractRenderSceneGeometry(m_scene, 11).items[0].deformedVertexBuffer.IsValid());
    smr->skinnedBufferModel = nullptr;
    EXPECT_FALSE(scene::ExtractRenderSceneGeometry(m_scene, 10).items[0].deformedVertexBuffer.IsValid());
    smr->skinnedBufferModel = &m_model;
    smr->morphVertexBuffers.resize(3);
    smr->morphVertexBuffers[2] = renderer::ResourceHandle<renderer::BufferTag>{ 80, 1 };
    EXPECT_EQ(scene::ExtractRenderSceneGeometry(m_scene, 10).items[0].deformedVertexBuffer.id, 62u);
}

TEST_F(RenderSceneExtractorTest, RejectsRendererMembershipInMultipleLodGroups)
{
    const auto memberId = StaticObject().GetID();
    scene::LODGroupComponent group;
    group.levels = {{0.5f, {{{}, memberId}}}};
    m_scene.CreateGameObject("First LOD").AddComponent<scene::LODGroupComponent>(group);
    m_scene.CreateGameObject("Second LOD").AddComponent<scene::LODGroupComponent>(group);
    const auto snapshot = scene::ExtractRenderSceneGeometry(m_scene, 10, {}, true);
    ASSERT_EQ(snapshot.objects.size(), 1u);
    EXPECT_TRUE(snapshot.objects[0].rayLodSelectionRequired);
}

TEST_F(RenderSceneExtractorTest, DiagnosesUnresolvedCanonicalLodInsteadOfPublishingAnEmptyShape)
{
    auto& lower = StaticObject();
    lower.layer = 31;
    const auto lowId = lower.GetID();
    scene::LODGroupComponent group;
    group.levels = {{0.75f, {{"unresolved-renderer-guid", scene::EntityID::INVALID}}},
        {0.25f, {{{}, lowId}}}};
    const auto ownerId = m_scene.CreateGameObject("Unresolved LOD").GetID();
    m_scene.GetGameObject(ownerId)->AddComponent<scene::LODGroupComponent>(group);
    const auto snapshot = scene::ExtractRenderSceneGeometry(m_scene, 10, {}, true);
    ASSERT_EQ(snapshot.objects.size(), 1u);
    EXPECT_FALSE(snapshot.objects[0].rayVisible);
    ASSERT_EQ(snapshot.rayLodDiagnostics.size(), 1u);
    EXPECT_EQ(snapshot.rayLodDiagnostics[0].sourceIndex, ownerId.index);
    EXPECT_EQ(snapshot.rayLodDiagnostics[0].sourceGeneration, ownerId.generation);
    EXPECT_EQ(snapshot.rayLodDiagnostics[0].layerMask, UINT32_MAX);
}

TEST_F(RenderSceneExtractorTest, NormalExtractionSkipsUnresolvedRayLodDiagnosticsAndKeepsRasterGeometry)
{
    auto& lower = StaticObject();
    lower.GetComponent<scene::MeshRenderer>()->lodVisible = true;
    const auto lowerId = lower.GetID();
    scene::LODGroupComponent group;
    group.levels = {{0.75f, {{"unresolved-renderer-guid", scene::EntityID::INVALID}}},
        {0.25f, {{{}, lowerId}}}};
    m_scene.CreateGameObject("Unresolved LOD").AddComponent<scene::LODGroupComponent>(group);
    const auto normal = scene::ExtractRenderSceneGeometry(m_scene, 10);
    ASSERT_EQ(normal.objects.size(), 1u);
    EXPECT_TRUE(normal.objects[0].lodVisible);
    EXPECT_FALSE(normal.objects[0].rayLodSelectionRequired);
    EXPECT_TRUE(normal.rayLodDiagnostics.empty());
    const auto experimental = scene::ExtractRenderSceneGeometry(m_scene, 10, {}, true);
    ASSERT_EQ(experimental.objects.size(), 1u);
    EXPECT_TRUE(experimental.objects[0].lodVisible);
    EXPECT_FALSE(experimental.objects[0].rayVisible);
    EXPECT_EQ(experimental.rayLodDiagnostics.size(), 1u);
}

TEST_F(RenderSceneExtractorTest, ParentAnimatorWinsOverReferencePoseAndPreservesPreviousPalette)
{
    auto& child = SkinnedObject();
    auto& parent = m_scene.CreateGameObject("AnimatorRoot");
    child.SetParent(parent);
    scene::AnimatorComponent animator;
    animator.skinningBuffer = renderer::ResourceHandle<renderer::ConstantBufferTag>{ 71, 1 };
    animator.prevSkinningBuffer = renderer::ResourceHandle<renderer::ConstantBufferTag>{ 72, 2 };
    animator.prevBoneMatricesValid = true;
    parent.AddComponent<scene::AnimatorComponent>(std::move(animator));
    m_model.referencePoseCB = { 81, 1 };
    const auto snapshot = scene::ExtractRenderSceneGeometry(m_scene, 10, { 91, 1 });
    ASSERT_EQ(snapshot.objects.size(), 1u);
    EXPECT_EQ(snapshot.objects[0].skinningPalette.id, 71u);
    EXPECT_EQ(snapshot.objects[0].previousSkinningPalette.id, 72u);
    EXPECT_TRUE(snapshot.objects[0].previousSkinningValid);
}

TEST_F(RenderSceneExtractorTest, ReferencePosePrecedesIdentityFallbackWithoutAnimator)
{
    SkinnedObject();
    m_model.referencePoseCB = { 81, 1 };
    EXPECT_EQ(scene::ExtractRenderSceneGeometry(m_scene, 10, { 91, 1 }).objects[0].skinningPalette.id, 81u);
    m_model.referencePoseCB = {};
    const auto snapshot = scene::ExtractRenderSceneGeometry(m_scene, 10, { 91, 1 });
    EXPECT_EQ(snapshot.objects[0].skinningPalette.id, 91u);
    EXPECT_FALSE(snapshot.objects[0].previousSkinningValid);
}

TEST_F(RenderSceneExtractorTest, PreviousWorldAdvancesOnceAcrossViewsAndResetsAfterGap)
{
    auto& go = StaticObject();
    go.transform.worldPosition.x = 1.0f;
    EXPECT_FLOAT_EQ(scene::ExtractRenderSceneGeometry(m_scene, 10).objects[0].previousWorld.m[0][3], 1.0f);
    go.transform.worldPosition.x = 2.0f;
    const auto firstView = scene::ExtractRenderSceneGeometry(m_scene, 11);
    const auto secondView = scene::ExtractRenderSceneGeometry(m_scene, 11);
    EXPECT_FLOAT_EQ(firstView.objects[0].previousWorld.m[0][3], 1.0f);
    EXPECT_FLOAT_EQ(secondView.objects[0].previousWorld.m[0][3], 1.0f);
    go.transform.worldPosition.x = 5.0f;
    EXPECT_FLOAT_EQ(scene::ExtractRenderSceneGeometry(m_scene, 15).objects[0].previousWorld.m[0][3], 5.0f);
    EXPECT_NE(firstView.snapshotSerial, secondView.snapshotSerial);
}

TEST_F(RenderSceneExtractorTest, InitialFrameDoesNotTreatUninitializedHistoryAsPreviousFrame)
{
    auto& go = StaticObject();
    go.transform.worldPosition.x = 3.0f;
    const auto snapshot = scene::ExtractRenderSceneGeometry(m_scene, 0);
    ASSERT_EQ(snapshot.objects.size(), 1u);
    EXPECT_FLOAT_EQ(snapshot.objects[0].previousWorld.m[0][3], 3.0f);
    go.transform.worldPosition.x = 4.0f;
    EXPECT_FLOAT_EQ(scene::ExtractRenderSceneGeometry(m_scene, 1).objects[0].previousWorld.m[0][3], 3.0f);
}

TEST_F(RenderSceneExtractorTest, MaterialSlotsStayIndependentAndHiddenSlotsRemainAddressable)
{
    auto& go = SkinnedObject();
    scene::MaterialComponent material;
    material.extraSlots.resize(1);
    EditMaterialAuthoring(material.SlotAt(0), [&](auto& values) { values.hasBlendModeOverride = true; });
    EditMaterialAuthoring(material.SlotAt(0), [&](auto& values) { values.blendModeOverride = renderer::BlendMode::ALPHA_BLEND; });
    material.RawSlotAt(1).visible = false;
    go.AddComponent<scene::MaterialComponent>(material);
    const auto snapshot = scene::ExtractRenderSceneGeometry(m_scene, 10);
    ASSERT_EQ(snapshot.items.size(), 2u);
    EXPECT_EQ(snapshot.items[0].material.capabilities.blend, renderer::BlendMode::ALPHA_BLEND);
    EXPECT_TRUE(snapshot.items[0].visible);
    EXPECT_FALSE(snapshot.items[1].visible);
}

TEST_F(RenderSceneExtractorTest, SceneGenerationIsStableAcrossViewsAndChangesBeforeEntityReuse)
{
    const auto originalId = StaticObject().GetID();
    const auto first = scene::ExtractRenderSceneGeometry(m_scene, 10);
    const auto second = scene::ExtractRenderSceneGeometry(m_scene, 11);
    EXPECT_NE(first.sceneGeneration, 0u);
    EXPECT_EQ(first.sceneGeneration, second.sceneGeneration);
    m_scene.Clear();
    const auto reusedId = StaticObject().GetID();
    const auto replaced = scene::ExtractRenderSceneGeometry(m_scene, 12);
    EXPECT_EQ(originalId, reusedId);
    EXPECT_NE(first.sceneGeneration, replaced.sceneGeneration);
}

TEST_F(RenderSceneExtractorTest, SceneMoveDoesNotReuseAnotherScenesRenderIdentity)
{
    StaticObject();
    const auto sourceGeneration = m_scene.GetRenderSceneGeneration();
    scene::Scene destination;
    const auto destinationGeneration = destination.GetRenderSceneGeneration();
    destination = std::move(m_scene);
    const auto snapshot = scene::ExtractRenderSceneGeometry(destination, 10);
    ASSERT_EQ(snapshot.objects.size(), 1u);
    EXPECT_NE(snapshot.sceneGeneration, sourceGeneration);
    EXPECT_NE(snapshot.sceneGeneration, destinationGeneration);
    EXPECT_NE(snapshot.sceneGeneration, m_scene.GetRenderSceneGeneration());
}

TEST_F(RenderSceneExtractorMaterialTest, ResolvesEachShaderOnceWhilePreservingIndependentMaterialOverrides)
{
    ASSERT_TRUE(WriteShader("Shared.hlsl", true));
    ASSERT_TRUE(WriteShader("Custom.hlsl", false));
    asset::MaterialAssetAuthoring shared;
    shared.shaderPath = ShaderPath("Shared.hlsl");
    shared.params["clearcoat"] = {0.5f};
    const auto sharedHandle = LoadMaterial("Shared.mat", shared);
    asset::MaterialAssetAuthoring additive;
    additive.shaderPath = shared.shaderPath;
    additive.blendMode = renderer::BlendMode::ADDITIVE;
    const auto additiveHandle = LoadMaterial("Additive.mat", additive);
    asset::MaterialAssetAuthoring custom;
    custom.shaderPath = ShaderPath("Custom.hlsl");
    const auto customHandle = LoadMaterial("Custom.mat", custom);
    ASSERT_TRUE(sharedHandle.IsValid());
    ASSERT_TRUE(additiveHandle.IsValid());
    ASSERT_TRUE(customHandle.IsValid());
    EditMaterialAuthoring(AddMaterialObject(sharedHandle), [&](auto& values) { values.paramOverrides["clearcoat"] = {0.0f}; });
    auto& transparent = AddMaterialObject(sharedHandle);
    EditMaterialAuthoring(transparent, [&](auto& values) { values.hasBlendModeOverride = true; });
    EditMaterialAuthoring(transparent, [&](auto& values) { values.blendModeOverride = renderer::BlendMode::ALPHA_BLEND; });
    AddMaterialObject(additiveHandle);
    AddMaterialObject(customHandle);

    const auto snapshot = ExtractWithProfile(10);

    ASSERT_EQ(snapshot.items.size(), 4u);
    EXPECT_EQ(ShaderCapabilitySampleCount(), 2u);
    EXPECT_TRUE(snapshot.items[0].material.capabilities.gbufferEquivalentShader);
    EXPECT_FALSE(snapshot.items[0].material.capabilities.advancedLobe);
    EXPECT_EQ(snapshot.items[0].material.capabilities.blend, renderer::BlendMode::OPAQUE_BLEND);
    EXPECT_TRUE(snapshot.items[1].material.capabilities.gbufferEquivalentShader);
    EXPECT_TRUE(snapshot.items[1].material.capabilities.advancedLobe);
    EXPECT_EQ(snapshot.items[1].material.capabilities.blend, renderer::BlendMode::ALPHA_BLEND);
    EXPECT_TRUE(snapshot.items[2].material.capabilities.gbufferEquivalentShader);
    EXPECT_FALSE(snapshot.items[2].material.capabilities.advancedLobe);
    EXPECT_EQ(snapshot.items[2].material.capabilities.blend, renderer::BlendMode::ADDITIVE);
    EXPECT_FALSE(snapshot.items[3].material.capabilities.gbufferEquivalentShader);
    EXPECT_FALSE(snapshot.items[3].material.capabilities.advancedLobe);
    EXPECT_EQ(snapshot.items[3].material.capabilities.blend, renderer::BlendMode::OPAQUE_BLEND);
}

TEST_F(RenderSceneExtractorMaterialTest, RechecksChangedAndRemovedShaderMetadataWithoutAdvancingTheRenderFrame)
{
    ASSERT_TRUE(WriteShader("Changing.hlsl", true));
    asset::MaterialAssetAuthoring material;
    material.shaderPath = ShaderPath("Changing.hlsl");
    const auto handle = LoadMaterial("Changing.mat", material);
    ASSERT_TRUE(handle.IsValid());
    AddMaterialObject(handle);
    AddMaterialObject(handle);
    const std::filesystem::path metaPath(material.shaderPath + ".meta");
    std::error_code error;
    const auto originalTime = std::filesystem::last_write_time(metaPath, error);
    ASSERT_FALSE(error);

    const auto initial = ExtractWithProfile(10);

    ASSERT_EQ(initial.items.size(), 2u);
    EXPECT_TRUE(initial.items[0].material.capabilities.gbufferEquivalentShader);
    EXPECT_TRUE(initial.items[1].material.capabilities.gbufferEquivalentShader);
    EXPECT_EQ(ShaderCapabilitySampleCount(), 1u);

    ASSERT_TRUE(WriteShader("Changing.hlsl", false));
    /// @note sleep を使わず、能力キャッシュが照合するファイル時刻だけを確実に進める。
    std::filesystem::last_write_time(metaPath, originalTime + std::chrono::seconds(2), error);
    ASSERT_FALSE(error);
    const auto changed = ExtractWithProfile(10);

    ASSERT_EQ(changed.items.size(), 2u);
    EXPECT_FALSE(changed.items[0].material.capabilities.gbufferEquivalentShader);
    EXPECT_FALSE(changed.items[1].material.capabilities.gbufferEquivalentShader);
    EXPECT_EQ(ShaderCapabilitySampleCount(), 1u);

    ASSERT_TRUE(WriteShader("Changing.hlsl", true));
    std::filesystem::last_write_time(metaPath, originalTime + std::chrono::seconds(4), error);
    ASSERT_FALSE(error);
    const auto restored = ExtractWithProfile(10);

    ASSERT_EQ(restored.items.size(), 2u);
    EXPECT_TRUE(restored.items[0].material.capabilities.gbufferEquivalentShader);
    EXPECT_TRUE(restored.items[1].material.capabilities.gbufferEquivalentShader);
    EXPECT_EQ(ShaderCapabilitySampleCount(), 1u);

    ASSERT_TRUE(std::filesystem::remove(metaPath, error));
    ASSERT_FALSE(error);
    const auto removed = ExtractWithProfile(10);

    ASSERT_EQ(removed.items.size(), 2u);
    EXPECT_FALSE(removed.items[0].material.capabilities.gbufferEquivalentShader);
    EXPECT_FALSE(removed.items[1].material.capabilities.gbufferEquivalentShader);
    EXPECT_EQ(ShaderCapabilitySampleCount(), 1u);
    EXPECT_TRUE(initial.items[0].material.capabilities.gbufferEquivalentShader);
}

TEST_F(RenderSceneExtractorMaterialTest, FullExtractionDefersItemCapabilitiesUntilMaterialSynchronization)
{
    ASSERT_TRUE(WriteShader("FullState.hlsl", true));
    asset::MaterialAssetAuthoring material;
    material.shaderPath = ShaderPath("FullState.hlsl");
    material.params["clearcoat"] = {0.5f};
    const auto handle = LoadMaterial("FullState.mat", material);
    ASSERT_TRUE(handle.IsValid());
    auto& component = AddMaterialObject(handle);
    component.RawSlotAt(0).visible = false;
    EditMaterialAuthoring(component, [](auto& values) {
        values.hasBlendModeOverride = true;
        values.blendModeOverride = renderer::BlendMode::ALPHA_BLEND;
    });
    struct RestoreFilter {
        profiler::PerformanceCaptureFilter previous;
        ~RestoreFilter() { profiler::Profiler::RequestCaptureFilter(std::move(previous)); }
    } restore{profiler::Profiler::GetRequestedCaptureFilter()};
    ASSERT_TRUE(profiler::Profiler::RequestCaptureFilter({}));
    const auto shaderCapabilityParents = [] {
        const auto& snapshot = profiler::Profiler::GetSnapshot();
        const auto nameOf = [&](uint64_t key) -> std::string_view {
            for (const auto& descriptor : snapshot.descriptors)
                if (descriptor.sampleKey == key) return descriptor.name;
            return {};
        };
        std::vector<std::string> parents;
        for (const auto& sample : snapshot.samples) {
            if (nameOf(sample.sampleKey) != "RenderSceneExtractor::ShaderCapabilities") continue;
            const auto parent = std::find_if(snapshot.samples.begin(), snapshot.samples.end(), [&](const auto& candidate) {
                return candidate.sampleId == sample.parentSampleId;
            });
            parents.emplace_back(parent == snapshot.samples.end() ? std::string_view{} : nameOf(parent->sampleKey));
        }
        return parents;
    };
    const auto standalone = ExtractWithProfile(10);
    ASSERT_EQ(standalone.items.size(), 1u);
    EXPECT_EQ(ShaderCapabilitySampleCount(), 1u);
    EXPECT_EQ(shaderCapabilityParents(), (std::vector<std::string>{"RenderSceneExtractor::GeometryMaterialCapabilities"}));
    EXPECT_TRUE(standalone.items[0].material.capabilities.gbufferEquivalentShader);
    EXPECT_TRUE(standalone.items[0].material.capabilities.advancedLobe);
    EXPECT_EQ(standalone.items[0].material.capabilities.blend, renderer::BlendMode::ALPHA_BLEND);
    EXPECT_FALSE(standalone.items[0].visible);

    scene::RenderFrameGeometryCache cache;
    profiler::Profiler::BeginFrame();
    const auto full = ExtractView({}, cache);
    profiler::Profiler::EndFrame();
    ASSERT_EQ(full->items.size(), 1u);
    /// @note Scope-name filtering is not hierarchical, so unfiltered parent identities locate the actual derivation boundary.
    EXPECT_EQ(ShaderCapabilitySampleCount(), 1u);
    EXPECT_EQ(shaderCapabilityParents(), (std::vector<std::string>{"RenderSceneExtractor::Materials"}));
    EXPECT_TRUE(full->items[0].material.capabilities.gbufferEquivalentShader);
    EXPECT_TRUE(full->items[0].material.capabilities.advancedLobe);
    EXPECT_EQ(full->items[0].material.capabilities.blend, renderer::BlendMode::ALPHA_BLEND);
    EXPECT_FALSE(full->items[0].visible);
}

TEST_F(RenderSceneExtractorMaterialTest, FullViewsRecheckMetadataAndSlotEditsWithoutChangingRetainedSnapshots)
{
    ASSERT_TRUE(WriteShader("FullChanging.hlsl", true));
    asset::MaterialAssetAuthoring material;
    material.shaderPath = ShaderPath("FullChanging.hlsl");
    material.params["clearcoat"] = {0.5f};
    const auto handle = LoadMaterial("FullChanging.mat", material);
    ASSERT_TRUE(handle.IsValid());
    auto& component = AddMaterialObject(handle);
    component.RawSlotAt(0).visible = false;
    const auto meta = std::filesystem::path(material.shaderPath + ".meta");
    std::error_code error;
    const auto originalTime = std::filesystem::last_write_time(meta, error);
    ASSERT_FALSE(error);
    scene::RenderFrameGeometryCache cache;
    const auto first = ExtractView({}, cache);
    ASSERT_EQ(first->items.size(), 1u);
    EXPECT_TRUE(first->items[0].material.capabilities.gbufferEquivalentShader);
    EXPECT_TRUE(first->items[0].material.capabilities.advancedLobe);

    ASSERT_TRUE(WriteShader("FullChanging.hlsl", false));
    std::filesystem::last_write_time(meta, originalTime + std::chrono::seconds(2), error);
    ASSERT_FALSE(error);
    EditMaterialAuthoring(component, [](auto& values) {
        values.paramOverrides["clearcoat"] = {0.0f};
        values.hasBlendModeOverride = true;
        values.blendModeOverride = renderer::BlendMode::ADDITIVE;
    });
    component.enabled = false;
    const auto second = ExtractView({}, cache);
    ASSERT_EQ(second->items.size(), 1u);
    EXPECT_FALSE(second->Objects()[0].colorEligible);
    EXPECT_FALSE(second->items[0].visible);
    EXPECT_FALSE(second->items[0].material.capabilities.gbufferEquivalentShader);
    EXPECT_FALSE(second->items[0].material.capabilities.advancedLobe);
    EXPECT_EQ(second->items[0].material.capabilities.blend, renderer::BlendMode::ADDITIVE);
    EXPECT_TRUE(first->Objects()[0].colorEligible);
    EXPECT_TRUE(first->items[0].material.capabilities.gbufferEquivalentShader);
    EXPECT_TRUE(first->items[0].material.capabilities.advancedLobe);
    EXPECT_EQ(first->items[0].material.capabilities.blend, renderer::BlendMode::OPAQUE_BLEND);
}

TEST_F(RenderSceneExtractorMaterialTest, SharesPosesAcrossViewsWhileKeepingSelectionAndMaterialsIndependent)
{
    const auto firstId = StaticObject().GetID();
    const auto secondId = StaticObject().GetID();
    scene::RenderFrameGeometryCache cache;
    renderer::RenderSettings firstSettings;
    firstSettings.selectedObjects.push_back({firstId.index, firstId.generation});
    const auto first = ExtractView(firstSettings, cache);
    renderer::RenderSettings secondSettings;
    secondSettings.selectedObjects.push_back({secondId.index, secondId.generation});
    const auto second = ExtractView(secondSettings, cache);
    ASSERT_EQ(first->Objects().size(), 2u);
    ASSERT_EQ(second->Objects().size(), 2u);
    EXPECT_EQ(first->Objects().data(), second->Objects().data());
    EXPECT_EQ(first->Objects().data(), cache.geometry->Objects().data());
    EXPECT_NE(first->items.data(), second->items.data());
    EXPECT_NE(first->snapshotSerial, second->snapshotSerial);
    EXPECT_TRUE(first->IsObjectSelected(0));
    EXPECT_FALSE(first->IsObjectSelected(1));
    EXPECT_FALSE(second->IsObjectSelected(0));
    EXPECT_TRUE(second->IsObjectSelected(1));
    EXPECT_TRUE(first->Objects()[0].colorEligible);
    EXPECT_EQ(m_scene.GetGameObject(firstId)->GetComponent<scene::MaterialComponent>(), nullptr);
}

TEST_F(RenderSceneExtractorMaterialTest, StaticRasterCacheOwnsRetiredSeedUntilTheFirstLaterView)
{
    ExtractionScanEnvironment mode("fused");
    m_mesh.vertexCount = 24;
    m_mesh.indexCount = 36;
    auto& owner = StaticObject();
    auto& material = owner.AddComponent<scene::MaterialComponent>();
    material.SetVisible(false);
    scene::RenderFrameGeometryCache cache;
    const auto materializations = [] {
        const auto& records = profiler::Profiler::GetLastFrameRecords();
        return std::count_if(records.begin(), records.end(), [](const auto& record) {
            return std::string_view(record.name) == "RenderSceneExtractor::GeometryCacheMaterialize";
        });
    };

    profiler::Profiler::BeginFrame();
    auto first = ExtractView({}, cache);
    profiler::Profiler::EndFrame();
    ASSERT_EQ(first->items.size(), 1u);
    EXPECT_EQ(materializations(), 0);
    std::weak_ptr<const renderer::RenderScene> retired = first;
    first.reset();
    EXPECT_FALSE(retired.expired());

    profiler::Profiler::BeginFrame();
    const auto second = ExtractView({}, cache);
    profiler::Profiler::EndFrame();
    ASSERT_EQ(second->items.size(), 1u);
    EXPECT_EQ(second->items[0].vertexCount, 24u);
    EXPECT_EQ(second->items[0].indexCount, 36u);
    EXPECT_FALSE(second->items[0].visible);
    EXPECT_EQ(materializations(), 1);
    EXPECT_TRUE(retired.expired());

    profiler::Profiler::BeginFrame();
    const auto third = ExtractView({}, cache);
    profiler::Profiler::EndFrame();
    EXPECT_EQ(materializations(), 0);
    EXPECT_EQ(second->Objects().data(), third->Objects().data());
    EXPECT_NE(second->items.data(), third->items.data());
    cache = {};
    m_scene.Clear();
    EXPECT_EQ(second->items[0].vertexCount, 24u);
    EXPECT_EQ(third->items[0].indexCount, 36u);
}

TEST_F(RenderSceneExtractorMaterialTest, PrototypeAndLegacyRestoreKeepFourViewsIndependentAfterSeedRetirement)
{
    ExtractionScanEnvironment scans("fused");
    for (const char* selectedMode : {"legacy", "reuse", "unknown"}) {
        GeometryRestoreEnvironment mode(selectedMode);
        for (bool retainFirst : {false, true}) {
            m_scene.Clear();
            m_mesh.vertexCount = 24;
            m_mesh.indexCount = 36;
            auto& owner = StaticObject();
            const auto id = owner.GetID();
            auto& material = owner.AddComponent<scene::MaterialComponent>();
            material.SetVisible(false);
            renderer::Mesh alternateMesh;
            alternateMesh.vertexBuffer = {33, 9};
            alternateMesh.indexBuffer = {44, 8};
            alternateMesh.vertexCount = 5;
            alternateMesh.indexCount = 9;
            alternateMesh.boundsCenter = {2, 3, 4};
            alternateMesh.boundsRadius = 2;
            auto& alternate = m_scene.CreateGameObject("Alternate");
            alternate.AddComponent<scene::MeshRenderer>().mesh = &alternateMesh;
            alternate.AddComponent<scene::MaterialComponent>().enabled = false;
            scene::RenderFrameGeometryCache cache;
            auto first = ExtractView({}, cache);
            std::weak_ptr<const renderer::RenderScene> seed = first;
            if (!retainFirst) first.reset();
            EditMaterialAuthoring(material, [](auto& draft) { draft.hasRenderQueueOverride = true; draft.renderQueueOverride = 2450; });
            renderer::RenderSettings selected;
            selected.selectedObjects.push_back({id.index, id.generation});
            const auto second = ExtractView(selected, cache);
            EXPECT_EQ(seed.expired(), !retainFirst);
            EditMaterialAuthoring(material, [](auto& draft) { draft.renderQueueOverride = 2100; });
            const auto third = ExtractView({}, cache);
            const auto fourth = ExtractView(selected, cache);
            cache = {};
            m_scene.Clear();
            for (const auto& view : {second, third, fourth}) {
                ASSERT_EQ(view->items.size(), 2u);
                EXPECT_EQ(view->items[0].vertexBuffer, m_mesh.vertexBuffer);
                EXPECT_EQ(view->items[0].indexBuffer, m_mesh.indexBuffer);
                EXPECT_EQ(view->items[0].vertexCount, 24u);
                EXPECT_EQ(view->items[0].indexCount, 36u);
                EXPECT_FALSE(view->items[0].preparedSkinning);
                EXPECT_FALSE(view->items[0].visible);
                EXPECT_EQ(view->items[0].objectIndex, 0u);
                EXPECT_EQ(view->items[1].objectIndex, 1u);
                EXPECT_EQ(view->items[1].vertexBuffer, alternateMesh.vertexBuffer);
                EXPECT_EQ(view->items[1].indexBuffer, alternateMesh.indexBuffer);
                EXPECT_EQ(view->items[1].vertexCount, 5u);
                EXPECT_EQ(view->items[1].indexCount, 9u);
                EXPECT_FALSE(view->items[1].preparedSkinning);
                EXPECT_FALSE(view->items[1].visible);
                EXPECT_EQ(view->items[1].material.renderQueue, renderer::RenderQueue::GEOMETRY);
            }
            EXPECT_TRUE(second->IsObjectSelected(0));
            EXPECT_FALSE(third->IsObjectSelected(0));
            EXPECT_TRUE(fourth->IsObjectSelected(0));
            EXPECT_EQ(second->Objects().data(), fourth->Objects().data());
            EXPECT_NE(second->items.data(), third->items.data());
            EXPECT_EQ(second->items[0].material.renderQueue, 2450);
            EXPECT_EQ(third->items[0].material.renderQueue, 2100);
        }
    }
}

TEST_F(RenderSceneExtractorMaterialTest, StaticRasterSeedRejectsReplacedOwnersAndMissingRows)
{
    ExtractionScanEnvironment mode("fused");
    m_mesh.vertexCount = 3;
    auto& material = StaticObject().AddComponent<scene::MaterialComponent>();
    material.SetVisible(false);
    scene::RenderFrameGeometryCache cache;
    const auto retained = ExtractView({}, cache);
    ASSERT_EQ(retained->items.size(), 1u);
    cache.geometry = std::make_shared<renderer::RenderScene>(*cache.geometry);
    m_mesh.vertexCount = 6;
    const auto replaced = ExtractView({}, cache);
    ASSERT_EQ(replaced->items.size(), 1u);
    EXPECT_EQ(replaced->items[0].vertexCount, 6u);
    EXPECT_EQ(retained->items[0].vertexCount, 3u);

    /// @note A retired deferred source without compact rows is an incomplete cache, even when its pose owner still matches.
    cache.deferredItemGeometry.reset();
    m_mesh.vertexCount = 9;
    const auto missing = ExtractView({}, cache);
    ASSERT_EQ(missing->items.size(), 1u);
    EXPECT_EQ(missing->items[0].vertexCount, 9u);
    EXPECT_EQ(replaced->items[0].vertexCount, 6u);
}

TEST_F(RenderSceneExtractorMaterialTest, StaticRasterReentrantHitsKeepOwnedGeometryAndObserveDirectMaterialEdits)
{
    struct RestoreImporter {
        std::unique_ptr<asset::IAssetImporter<asset::MaterialAsset>> previous;
        ~RestoreImporter() { asset::AssetManager::RegisterImporter<asset::MaterialAsset>(std::move(previous)); }
    } restore{std::move(asset::AssetStore<asset::MaterialAsset>::Get().importer)};
    for (const char* selectedMode : {"legacy", "fused"}) {
        ExtractionScanEnvironment mode(selectedMode);
        m_scene.Clear();
        auto& owner = StaticObject();
        const auto id = owner.GetID();
        auto& material = owner.AddComponent<scene::MaterialComponent>();
        material.SetMaterialPath(m_temp.File(std::string(selectedMode) + "-static-reentrant.mat").generic_string());
        material.SetVisible(false);
        auto importer = std::make_unique<ExtractionCallbackMaterialImporter>();
        auto* callback = importer.get();
        asset::AssetManager::RegisterImporter<asset::MaterialAsset>(std::move(importer));
        scene::RenderFrameGeometryCache cache;
        std::shared_ptr<const renderer::RenderScene> nested, disabled;
        callback->beforeImport = [&] {
            nested = ExtractView({}, cache);
            material.enabled = false;
            disabled = ExtractView({}, cache);
        };
        renderer::RenderSettings selected;
        selected.selectedObjects.push_back({id.index, id.generation});
        const auto outer = ExtractView(selected, cache);
        ASSERT_NE(nested, nullptr);
        ASSERT_NE(disabled, nullptr);
        ASSERT_EQ(outer->Objects().size(), 1u);
        ASSERT_EQ(nested->items.size(), 1u);
        EXPECT_EQ(outer->Objects().data(), nested->Objects().data());
        EXPECT_NE(outer->Objects().data(), disabled->Objects().data());
        EXPECT_TRUE(outer->Objects()[0].colorEligible);
        EXPECT_TRUE(nested->Objects()[0].colorEligible);
        EXPECT_FALSE(disabled->Objects()[0].colorEligible);
        EXPECT_TRUE(outer->IsObjectSelected(0));
        EXPECT_FALSE(nested->IsObjectSelected(0));
        EXPECT_FALSE(outer->items[0].visible);
        const auto current = ExtractView({}, cache);
        EXPECT_EQ(current->Objects().data(), disabled->Objects().data());
        EXPECT_FALSE(current->Objects()[0].colorEligible);
        cache = {};
        m_scene.Clear();
        EXPECT_TRUE(outer->Objects()[0].colorEligible);
        EXPECT_TRUE(nested->Objects()[0].colorEligible);
        EXPECT_FALSE(disabled->Objects()[0].colorEligible);
    }
}

TEST_F(RenderSceneExtractorMaterialTest, MixedAndRayCachesKeepEagerSeedsAndCurrentViewInputs)
{
    ExtractionScanEnvironment mode("fused");
    auto& material = StaticObject().AddComponent<scene::MaterialComponent>();
    material.SetVisible(false);
    renderer::RenderSettings raySettings;
    raySettings.modeRequest.mode = renderer::RenderMode::HYBRID;
    scene::RenderFrameGeometryCache rayCache;
    auto ray = ExtractView(raySettings, rayCache, UINT32_MAX, true);
    ASSERT_EQ(ray->items.size(), 1u);
    std::weak_ptr<const renderer::RenderScene> retiredRay = ray;
    ray.reset();
    EXPECT_TRUE(retiredRay.expired());
    EXPECT_EQ(ExtractView(raySettings, rayCache, UINT32_MAX, true)->items.size(), 1u);

    auto& skinnedOwner = SkinnedObject();
    skinnedOwner.AddComponent<scene::MaterialComponent>().SetVisible(false);
    auto& skinned = *skinnedOwner.GetComponent<scene::SkinnedMeshRenderer>();
    skinned.gpuSkinnedThisFrame = true;
    skinned.gpuSkinningFrame = Time::frameCount;
    skinned.skinnedBufferModel = &m_model;
    skinned.skinnedVertexBuffers = testkit::MakeOwnedResources<renderer::BufferTag>({{71, 4}, {72, 4}, {73, 4}});
    scene::RenderFrameGeometryCache mixedCache;
    auto mixed = ExtractView({}, mixedCache);
    ASSERT_EQ(mixed->items.size(), 3u);
    EXPECT_EQ(mixed->items[1].deformedVertexBuffer, (renderer::ResourceHandle<renderer::BufferTag>{73, 4}));
    std::weak_ptr<const renderer::RenderScene> retiredMixed = mixed;
    mixed.reset();
    EXPECT_TRUE(retiredMixed.expired());
    skinned.skinnedVertexBuffers[2] = renderer::ResourceHandle<renderer::BufferTag>{91, 6};
    const auto current = ExtractView({}, mixedCache);
    ASSERT_EQ(current->items.size(), 3u);
    EXPECT_EQ(current->items[1].deformedVertexBuffer, (renderer::ResourceHandle<renderer::BufferTag>{91, 6}));
}

TEST_F(RenderSceneExtractorMaterialTest, CompactFrameGeometryPreservesStaticAndSkinnedInputsAcrossViews)
{
    m_mesh.vertexBuffer = {11, 2};
    m_mesh.indexBuffer = {12, 3};
    m_mesh.vertexCount = 24;
    m_mesh.indexCount = 36;
    m_mesh.boundsCenter = {0.25f, 0, 0.5f};
    m_mesh.boundsRadius = 0.75f;
    auto& rigid = StaticObject();
    rigid.transform.position = rigid.transform.worldPosition = {2, 3, 4};
    rigid.transform.scale = rigid.transform.worldScale = {2, 3, 4};
    auto& skinnedOwner = SkinnedObject();
    auto& skinned = *skinnedOwner.GetComponent<scene::SkinnedMeshRenderer>();
    skinned.gpuSkinningFrame = Time::frameCount;
    skinned.skinnedBufferModel = &m_model;
    skinned.skinnedVertexBuffers = testkit::MakeOwnedResources<renderer::BufferTag>({{71, 4}, {72, 4}, {73, 4}});
    scene::RenderFrameGeometryCache cache;
    const auto first = ExtractView({}, cache);
    const auto second = ExtractView({}, cache);
    ASSERT_EQ(first->items.size(), 3u);
    ASSERT_EQ(second->items.size(), first->items.size());
    EXPECT_EQ(first->Objects().data(), second->Objects().data());
    EXPECT_NE(first->items.data(), second->items.data());
    for (size_t index = 0; index < first->items.size(); ++index) {
        const auto& before = first->items[index];
        const auto& after = second->items[index];
        EXPECT_EQ(after.objectIndex, before.objectIndex);
        EXPECT_EQ(after.materialSlot, before.materialSlot);
        EXPECT_EQ(after.sourceSubmesh, before.sourceSubmesh);
        EXPECT_EQ(after.vertexBuffer, before.vertexBuffer);
        EXPECT_EQ(after.indexBuffer, before.indexBuffer);
        EXPECT_EQ(after.skinningVertexBuffer, before.skinningVertexBuffer);
        EXPECT_EQ(after.deformedVertexBuffer, before.deformedVertexBuffer);
        EXPECT_EQ(after.preparedSkinning, before.preparedSkinning);
        EXPECT_EQ(after.preparedSkinningWrite, before.preparedSkinningWrite);
        EXPECT_EQ(after.indexCount, before.indexCount);
        EXPECT_EQ(after.vertexCount, before.vertexCount);
        EXPECT_EQ(after.vertexStride, before.vertexStride);
        EXPECT_EQ(after.vertexPositionOffset, before.vertexPositionOffset);
        EXPECT_EQ(after.vertexContentVersion, before.vertexContentVersion);
        EXPECT_EQ(after.indexContentVersion, before.indexContentVersion);
        EXPECT_EQ(after.deformedContentVersion, before.deformedContentVersion);
        EXPECT_EQ(after.reliableOccluder, before.reliableOccluder);
        EXPECT_VEC3_NEAR(after.boundsCenter, before.boundsCenter, testkit::kTolerance);
        EXPECT_FLOAT_EQ(after.boundsRadius, before.boundsRadius);
    }
    EXPECT_EQ(second->items[0].vertexBuffer, (renderer::ResourceHandle<renderer::BufferTag>{11, 2}));
    EXPECT_EQ(second->items[1].sourceSubmesh, 2u);
    EXPECT_EQ(second->items[2].sourceSubmesh, 0u);
}

TEST_F(RenderSceneExtractorMaterialTest, FusedScansKeepInterleavedSkinnedInputsFreshAcrossCachedViews)
{
    ExtractionScanEnvironment mode("fused");
    (void)StaticObject();
    auto& mixedOwner = SkinnedObject();
    scene::MeshRenderer staticRenderer;
    staticRenderer.mesh = &m_mesh;
    mixedOwner.AddComponent<scene::MeshRenderer>(staticRenderer);
    auto& mixed = *mixedOwner.GetComponent<scene::SkinnedMeshRenderer>();
    mixed.gpuSkinnedThisFrame = true;
    mixed.gpuSkinningFrame = Time::frameCount;
    mixed.skinnedBufferModel = &m_model;
    mixed.skinnedVertexBuffers = testkit::MakeOwnedResources<renderer::BufferTag>({{71, 4}, {72, 4}, {73, 4}});
    ASSERT_EQ(mixed.ResolveSlotSkinnedVertexBuffer(0), (renderer::ResourceHandle<renderer::BufferTag>{73, 4}));
    (void)StaticObject();
    auto& secondOwner = m_scene.CreateGameObject("SecondSkinned");
    scene::SkinnedMeshRenderer secondRenderer;
    secondRenderer.model = &m_model;
    secondRenderer.submeshIndices = {1};
    auto& second = secondOwner.AddComponent<scene::SkinnedMeshRenderer>(secondRenderer);
    second.gpuSkinnedThisFrame = true;
    second.gpuSkinningFrame = Time::frameCount;
    second.skinnedBufferModel = &m_model;
    second.skinnedVertexBuffers = testkit::MakeOwnedResources<renderer::BufferTag>({{81, 5}, {82, 5}, {83, 5}});
    ASSERT_EQ(second.ResolveSlotSkinnedVertexBuffer(0), (renderer::ResourceHandle<renderer::BufferTag>{82, 5}));
    (void)StaticObject();

    scene::RenderFrameGeometryCache cache;
    const auto first = ExtractView({}, cache);
    ASSERT_EQ(first->Objects().size(), 6u);
    ASSERT_EQ(first->items.size(), 7u);
    EXPECT_FALSE(first->Objects()[1].skinned);
    EXPECT_TRUE(first->Objects()[2].skinned);
    EXPECT_FALSE(first->Objects()[3].skinned);
    EXPECT_TRUE(first->Objects()[4].skinned);
    EXPECT_FALSE(first->Objects()[5].skinned);
    mixed.skinnedVertexBuffers = testkit::MakeOwnedResources<renderer::BufferTag>({{91, 6}, {92, 6}, {93, 6}});
    second.skinnedVertexBuffers = testkit::MakeOwnedResources<renderer::BufferTag>({{101, 7}, {102, 7}, {103, 7}});
    const auto current = ExtractView({}, cache);
    EXPECT_EQ(current->Objects().data(), first->Objects().data());
    EXPECT_EQ(current->items[2].deformedVertexBuffer, (renderer::ResourceHandle<renderer::BufferTag>{93, 6}));
    EXPECT_EQ(current->items[3].deformedVertexBuffer, (renderer::ResourceHandle<renderer::BufferTag>{91, 6}));
    EXPECT_EQ(current->items[5].deformedVertexBuffer, (renderer::ResourceHandle<renderer::BufferTag>{102, 7}));
    EXPECT_EQ(first->items[2].deformedVertexBuffer, (renderer::ResourceHandle<renderer::BufferTag>{73, 4}));
    EXPECT_EQ(first->items[3].deformedVertexBuffer, (renderer::ResourceHandle<renderer::BufferTag>{71, 4}));
    EXPECT_EQ(first->items[5].deformedVertexBuffer, (renderer::ResourceHandle<renderer::BufferTag>{82, 5}));
}

TEST_F(RenderSceneExtractorMaterialTest, FusedScansFallBackForLegacyReplacedAndInvalidFrameCacheMetadata)
{
    ExtractionScanEnvironment mode("legacy");
    (void)StaticObject();
    auto& owner = SkinnedObject();
    auto& skinned = *owner.GetComponent<scene::SkinnedMeshRenderer>();
    skinned.gpuSkinnedThisFrame = true;
    skinned.gpuSkinningFrame = Time::frameCount;
    skinned.skinnedBufferModel = &m_model;
    skinned.skinnedVertexBuffers = testkit::MakeOwnedResources<renderer::BufferTag>({{71, 4}, {72, 4}, {73, 4}});
    ASSERT_EQ(skinned.ResolveSlotSkinnedVertexBuffer(0), (renderer::ResourceHandle<renderer::BufferTag>{73, 4}));
    scene::RenderFrameGeometryCache cache;
    const auto legacy = ExtractView({}, cache);
    _putenv_s("FBZZ_RENDER_EXTRACTION_SCAN_MODE", "fused");
    skinned.skinnedVertexBuffers[2] = renderer::ResourceHandle<renderer::BufferTag>{91, 6};
    const auto fromLegacyCache = ExtractView({}, cache);
    EXPECT_EQ(fromLegacyCache->Objects().data(), legacy->Objects().data());
    EXPECT_EQ(fromLegacyCache->items[1].deformedVertexBuffer, (renderer::ResourceHandle<renderer::BufferTag>{91, 6}));

    ++Time::frameCount;
    skinned.gpuSkinningFrame = Time::frameCount;
    (void)ExtractView({}, cache);
    /// @note A caller-replaced immutable geometry owner does not inherit the original producer's range certificate.
    cache.geometry = std::make_shared<renderer::RenderScene>(*cache.geometry);
    skinned.skinnedVertexBuffers[2] = renderer::ResourceHandle<renderer::BufferTag>{101, 7};
    const auto replaced = ExtractView({}, cache);
    EXPECT_EQ(replaced->items[1].deformedVertexBuffer, (renderer::ResourceHandle<renderer::BufferTag>{101, 7}));

    ++Time::frameCount;
    skinned.gpuSkinningFrame = Time::frameCount;
    (void)ExtractView({}, cache);
    cache.skinnedObjectBegin = cache.geometry->Objects().size();
    cache.skinnedObjectEnd = cache.skinnedObjectBegin + 1;
    skinned.skinnedVertexBuffers[2] = renderer::ResourceHandle<renderer::BufferTag>{111, 8};
    const auto invalid = ExtractView({}, cache);
    EXPECT_EQ(invalid->items[1].deformedVertexBuffer, (renderer::ResourceHandle<renderer::BufferTag>{111, 8}));
    EXPECT_EQ(legacy->items[1].deformedVertexBuffer, (renderer::ResourceHandle<renderer::BufferTag>{73, 4}));
}

TEST_F(RenderSceneExtractorMaterialTest, FusedColorEligibilityKeepsMissingDisabledAndHiddenMaterialsIndependent)
{
    ExtractionScanEnvironment mode("fused");
    const auto missingId = StaticObject().GetID();
    auto& mixedOwner = SkinnedObject();
    scene::MeshRenderer staticRenderer;
    staticRenderer.mesh = &m_mesh;
    mixedOwner.AddComponent<scene::MeshRenderer>(staticRenderer);
    auto& disabled = mixedOwner.AddComponent<scene::MaterialComponent>();
    disabled.enabled = false;
    disabled.SetVisible(false);
    auto& hidden = StaticObject().AddComponent<scene::MaterialComponent>();
    hidden.SetVisible(false);
    scene::RenderFrameGeometryCache cache;
    const auto first = ExtractView({}, cache);
    ASSERT_EQ(first->Objects().size(), 4u);
    ASSERT_EQ(first->items.size(), 5u);
    EXPECT_TRUE(first->Objects()[0].colorEligible);
    EXPECT_FALSE(first->Objects()[1].colorEligible);
    EXPECT_FALSE(first->Objects()[2].colorEligible);
    EXPECT_TRUE(first->Objects()[3].colorEligible);
    for (size_t index = 1; index < first->items.size(); ++index) EXPECT_FALSE(first->items[index].visible);

    disabled.enabled = true;
    hidden.enabled = false;
    const auto current = ExtractView({}, cache);
    EXPECT_NE(current->Objects().data(), first->Objects().data());
    EXPECT_TRUE(current->Objects()[1].colorEligible);
    EXPECT_TRUE(current->Objects()[2].colorEligible);
    EXPECT_FALSE(current->Objects()[3].colorEligible);
    EXPECT_FALSE(first->Objects()[1].colorEligible);
    EXPECT_TRUE(first->Objects()[3].colorEligible);
    const auto standalone = scene::ExtractRenderSceneGeometry(m_scene, Time::frameCount);
    ASSERT_EQ(standalone.Objects().size(), current->Objects().size());
    for (size_t index = 0; index < standalone.Objects().size(); ++index)
        EXPECT_EQ(standalone.Objects()[index].colorEligible, current->Objects()[index].colorEligible);
    EXPECT_EQ(m_scene.GetGameObject(missingId)->GetComponent<scene::MaterialComponent>(), nullptr);
}

TEST_F(RenderSceneExtractorMaterialTest, ScanModesKeepGeometryAndSkinnedInputsIndependentDuringReentrantMaterialLoad)
{
    struct RestoreImporter {
        std::unique_ptr<asset::IAssetImporter<asset::MaterialAsset>> previous;
        ~RestoreImporter() { asset::AssetManager::RegisterImporter<asset::MaterialAsset>(std::move(previous)); }
    } restore{std::move(asset::AssetStore<asset::MaterialAsset>::Get().importer)};
    for (const char* selectedMode : {"legacy", "fused"}) {
        ExtractionScanEnvironment mode(selectedMode);
        m_scene.Clear();
        auto& material = StaticObject().AddComponent<scene::MaterialComponent>();
        material.SetMaterialPath(m_temp.File(std::string(selectedMode) + "-reentrant.mat").generic_string());
        material.SetVisible(false);
        auto& owner = SkinnedObject();
        auto& skinned = *owner.GetComponent<scene::SkinnedMeshRenderer>();
        skinned.gpuSkinnedThisFrame = true;
        skinned.gpuSkinningFrame = Time::frameCount;
        skinned.skinnedBufferModel = &m_model;
        skinned.skinnedVertexBuffers = testkit::MakeOwnedResources<renderer::BufferTag>({{71, 4}, {72, 4}, {73, 4}});
        ASSERT_EQ(skinned.ResolveSlotSkinnedVertexBuffer(0), (renderer::ResourceHandle<renderer::BufferTag>{73, 4}));
        auto importer = std::make_unique<ExtractionCallbackMaterialImporter>();
        auto* callback = importer.get();
        asset::AssetManager::RegisterImporter<asset::MaterialAsset>(std::move(importer));
        scene::RenderFrameGeometryCache cache;
        std::shared_ptr<const renderer::RenderScene> nested;
        callback->beforeImport = [&] {
            material.enabled = false;
            skinned.skinnedVertexBuffers[2] = renderer::ResourceHandle<renderer::BufferTag>{91, 6};
            nested = ExtractView({}, cache);
        };
        const auto outer = ExtractView({}, cache);
        ASSERT_NE(nested, nullptr);
        ASSERT_EQ(outer->Objects().size(), 2u);
        ASSERT_EQ(nested->Objects().size(), 2u);
        ASSERT_EQ(outer->items.size(), 3u);
        EXPECT_NE(outer->Objects().data(), nested->Objects().data());
        EXPECT_TRUE(outer->Objects()[0].colorEligible);
        EXPECT_FALSE(nested->Objects()[0].colorEligible);
        EXPECT_FALSE(outer->items[0].visible);
        EXPECT_EQ(outer->items[1].deformedVertexBuffer, (renderer::ResourceHandle<renderer::BufferTag>{73, 4}));
        EXPECT_EQ(nested->items[1].deformedVertexBuffer, (renderer::ResourceHandle<renderer::BufferTag>{91, 6}));
        const auto current = ExtractView({}, cache);
        EXPECT_FALSE(current->Objects()[0].colorEligible);
        EXPECT_TRUE(outer->Objects()[0].colorEligible);
        EXPECT_EQ(outer->items[1].deformedVertexBuffer, (renderer::ResourceHandle<renderer::BufferTag>{73, 4}));
    }
}

TEST_F(RenderSceneExtractorMaterialTest, ManagedMeshWritesInvalidateSharedViewsWithoutRebuildingUnchangedMath)
{
    auto& owner = StaticObject();
    scene::RenderFrameGeometryCache cache;
    (void)ExtractView({}, cache);
    ++Time::frameCount;
    const auto retained = ExtractView({}, cache);
    const bool previousPlay = scene::Script::IsInPlayMode();
    scene::Script::SetInPlayMode(true);
    scene::Script view;
    view.SetContext(&m_scene, &owner);
    scene::ManagedBridgeRequest request;
    request.op = scene::ManagedBridgeOp::SERVICE_MESH; request.index = 3;
    scene::managed_detail::ManagedServiceResult result;
    const auto status = scene::managed_detail::RunRenderComponentServices(m_scene, view, owner, nullptr,
        request, 0, {}, {}, result);
    view.SetContext(nullptr, nullptr);
    scene::Script::SetInPlayMode(previousPlay);
    ASSERT_EQ(status, scene::ManagedBridgeStatus::OK);
    profiler::Profiler::BeginFrame();
    const auto changed = ExtractView({}, cache);
    profiler::Profiler::EndFrame();
    ASSERT_EQ(changed->Objects().size(), 1u);
    EXPECT_FALSE(changed->Objects()[0].castShadows);
    EXPECT_TRUE(retained->Objects()[0].castShadows);
    EXPECT_NE(changed->Objects().data(), retained->Objects().data());
    const auto& records = profiler::Profiler::GetLastFrameRecords();
    EXPECT_TRUE(std::none_of(records.begin(), records.end(), [](const auto& record) {
        return std::string_view(record.name) == "RenderSceneExtractor::StaticGeometryRebuild";
    }));
}

TEST_F(RenderSceneExtractorMaterialTest, MeshNotificationsInvalidateSameFrameGeometryWithoutMutatingPublishedViews)
{
    auto& owner = StaticObject();
    scene::RenderFrameGeometryCache cache;
    const auto first = ExtractView({}, cache);
    const auto shared = ExtractView({}, cache);
    ASSERT_EQ(first->Objects().size(), 1u);
    EXPECT_EQ(first->Objects().data(), shared->Objects().data());

    owner.transform.position = owner.transform.worldPosition = {13, 2, 3};
    scene::NotifyMeshRendererChanged(m_scene, owner.GetID());
    const auto moved = ExtractView({}, cache);
    ASSERT_EQ(moved->Objects().size(), 1u);
    EXPECT_NE(moved->Objects().data(), first->Objects().data());
    EXPECT_FLOAT_EQ(moved->Objects()[0].world.m[0][3], 13);
    EXPECT_FLOAT_EQ(first->Objects()[0].world.m[0][3], 0);

    owner.GetComponent<scene::MeshRenderer>()->enabled = false;
    scene::NotifyMeshRendererChanged(m_scene, owner.GetID());
    EXPECT_TRUE(ExtractView({}, cache)->Objects().empty());
    owner.GetComponent<scene::MeshRenderer>()->enabled = true;
    scene::NotifyMeshRendererChanged(m_scene, owner.GetID());
    EXPECT_EQ(ExtractView({}, cache)->Objects().size(), 1u);
}

TEST_F(RenderSceneExtractorMaterialTest, CompactFrameGeometryRebuildsMovedBoundsAndRendererMembershipEveryFrame)
{
    m_mesh.vertexBuffer = {11, 2};
    m_mesh.boundsRadius = 1;
    auto& owner = StaticObject();
    const auto ownerId = owner.GetID();
    scene::RenderFrameGeometryCache cache;
    const auto first = ExtractView({}, cache);
    ASSERT_EQ(first->Objects().size(), 1u);
    owner.transform.position = owner.transform.worldPosition = {75, 2, 3};
    owner.layer = 7;
    owner.GetComponent<scene::MeshRenderer>()->castShadows = false;
    m_mesh.boundsRadius = 7;
    m_mesh.vertexBuffer = {99, 3};
    ++Time::frameCount;
    const auto moved = ExtractView({}, cache);
    ASSERT_EQ(moved->Objects().size(), 1u);
    EXPECT_NE(moved->Objects().data(), first->Objects().data());
    EXPECT_VEC3_NEAR(moved->items[0].boundsCenter, (math::Vector3{75, 2, 3}), testkit::kTolerance);
    EXPECT_FLOAT_EQ(moved->items[0].boundsRadius, 7);
    EXPECT_EQ(moved->items[0].vertexBuffer, (renderer::ResourceHandle<renderer::BufferTag>{99, 3}));
    EXPECT_EQ(moved->Objects()[0].layer, 7u);
    EXPECT_FALSE(moved->Objects()[0].castShadows);
    EXPECT_FLOAT_EQ(first->items[0].boundsRadius, 1);
    EXPECT_EQ(first->items[0].vertexBuffer, (renderer::ResourceHandle<renderer::BufferTag>{11, 2}));

    owner.GetComponent<scene::MeshRenderer>()->enabled = false;
    ++Time::frameCount;
    EXPECT_TRUE(ExtractView({}, cache)->Objects().empty());
    owner.GetComponent<scene::MeshRenderer>()->enabled = true;
    owner.SetActive(false);
    ++Time::frameCount;
    EXPECT_TRUE(ExtractView({}, cache)->Objects().empty());
    owner.SetActive(true);
    renderer::Mesh replacement = m_mesh;
    replacement.vertexBuffer = {98, 4};
    owner.GetComponent<scene::MeshRenderer>()->mesh = &replacement;
    ++Time::frameCount;
    const auto replaced = ExtractView({}, cache);
    ASSERT_EQ(replaced->items.size(), 1u);
    EXPECT_EQ(replaced->items[0].vertexBuffer, replacement.vertexBuffer);
    ASSERT_TRUE(m_scene.DestroyGameObject(ownerId));
    auto& reused = StaticObject();
    reused.transform.position = reused.transform.worldPosition = {-4, 0, 1};
    ++Time::frameCount;
    const auto recreated = ExtractView({}, cache);
    ASSERT_EQ(recreated->Objects().size(), 1u);
    EXPECT_EQ(recreated->Objects()[0].sourceIndex, reused.GetID().index);
    EXPECT_EQ(recreated->Objects()[0].sourceGeneration, reused.GetID().generation);
    EXPECT_VEC3_NEAR(recreated->items[0].boundsCenter, (math::Vector3{-4, 0, 1}), testkit::kTolerance);
    EXPECT_VEC3_NEAR(replaced->items[0].boundsCenter, (math::Vector3{75, 2, 3}), testkit::kTolerance);
}

TEST_F(RenderSceneExtractorMaterialTest, CompactCachedItemsRetainOffCameraShadowProbeAndRayDemand)
{
    m_mesh.vertexBuffer = {11, 2};
    m_mesh.indexBuffer = {12, 3};
    m_mesh.boundsRadius = 1;
    auto& owner = StaticObject();
    owner.layer = 2;
    owner.transform.position = owner.transform.worldPosition = {0, -10, 10};
    scene::RenderFrameGeometryCache cache;
    const auto first = ExtractView({}, cache);
    const auto cached = ExtractView({}, cache);
    ASSERT_EQ(cached->items.size(), 1u);
    const auto& object = cached->Objects()[0];
    const auto& item = cached->items[0];
    scene::material_demand_detail::View view;
    view.consumerScopeCertified = true;
    view.materialCoverageKnown = true;
    math::Frustum frustum;
    view.culling.frustum = &frustum;
    auto result = scene::material_demand_detail::Evaluate(view, object, item);
    EXPECT_EQ(result.cameraResult, renderer::VisibilityResult::FRUSTUM_CULLED);
    view.directionalShadowPotential = true;
    EXPECT_NE(scene::material_demand_detail::Evaluate(view, object, item).reasons
        & scene::material_demand_detail::DIRECTIONAL_SHADOW_POTENTIAL, 0u);
    view.directionalShadowPotential = false;
    view.punctualShadowPotential = true;
    EXPECT_NE(scene::material_demand_detail::Evaluate(view, object, item).reasons
        & scene::material_demand_detail::PUNCTUAL_SHADOW_POTENTIAL, 0u);
    view.punctualShadowPotential = false;
    view.reflectionProbePotential = true;
    view.lightProbePotential = true;
    result = scene::material_demand_detail::Evaluate(view, object, item);
    EXPECT_NE(result.reasons & scene::material_demand_detail::REFLECTION_PROBE_POTENTIAL, 0u);
    EXPECT_NE(result.reasons & scene::material_demand_detail::LIGHT_PROBE_POTENTIAL, 0u);
    view.reflectionProbePotential = view.lightProbePotential = false;
    view.rayPotential = true;
    view.mask = 0;
    EXPECT_NE(scene::material_demand_detail::Evaluate(view, object, item).reasons
        & scene::material_demand_detail::RAY_POTENTIAL, 0u);
    EXPECT_EQ(first->Objects().data(), cached->Objects().data());
    EXPECT_VEC3_NEAR(first->items[0].boundsCenter, (math::Vector3{0, -10, 10}), testkit::kTolerance);
}

TEST_F(RenderSceneExtractorMaterialTest, IndexedMasksRetainBothRendererKindsAndRespectEachViewsLayerAndChildren)
{
    auto& root = StaticObject();
    auto& child = SkinnedObject();
    child.SetParent(root);
    child.layer = 7;
    scene::SkinnedMeshRenderer skinned;
    skinned.model = &m_model;
    skinned.submeshIndices = {0};
    root.AddComponent<scene::SkinnedMeshRenderer>(skinned);
    scene::RenderFrameGeometryCache cache;
    renderer::RenderSettings settings;
    const auto initial = ExtractView(settings, cache);
    renderer::RenderObjectMaskRequest request;
    request.id = {root.GetID().index, root.GetID().generation};
    request.frame = initial->frameStamp;
    settings.objectMaskRequests.push_back(request);
    const auto all = ExtractView(settings, cache);
    ASSERT_EQ(all->Objects().size(), 3u);
    ASSERT_EQ(all->maskGroups.size(), 1u);
    EXPECT_EQ(all->maskGroups[0].objectIndices, (std::vector<uint32_t>{0, 1, 2}));
    const auto onlyLayerZero = ExtractView(settings, cache, 1u);
    ASSERT_EQ(onlyLayerZero->maskGroups.size(), 1u);
    EXPECT_EQ(onlyLayerZero->maskGroups[0].objectIndices, (std::vector<uint32_t>{0, 1}));
    settings.objectMaskRequests[0].includeChildren = false;
    const auto onlyRoot = ExtractView(settings, cache);
    ASSERT_EQ(onlyRoot->maskGroups.size(), 1u);
    EXPECT_EQ(onlyRoot->maskGroups[0].objectIndices, (std::vector<uint32_t>{0, 1}));
    EXPECT_EQ(all->maskGroups[0].objectIndices, (std::vector<uint32_t>{0, 1, 2}));
    settings.objectMaskRequests[0].id.generation += 1;
    EXPECT_TRUE(ExtractView(settings, cache)->maskGroups.empty());
}

TEST_F(RenderSceneExtractorMaterialTest, KeepsSharedPosesAliveAfterCacheRetirementAndSceneGenerationChanges)
{
    auto& original = StaticObject();
    original.transform.worldPosition = {2, 3, 4};
    const auto originalId = original.GetID();
    scene::RenderFrameGeometryCache cache;
    renderer::RenderSettings settings;
    const auto first = ExtractView(settings, cache);
    m_scene.Clear();
    auto& replacement = StaticObject();
    replacement.transform.worldPosition = {8, 9, 10};
    const auto second = ExtractView(settings, cache);
    EXPECT_NE(first->sceneGeneration, second->sceneGeneration);
    EXPECT_NE(first->Objects().data(), second->Objects().data());
    cache = {};
    ASSERT_EQ(first->Objects().size(), 1u);
    EXPECT_EQ(first->Objects()[0].sourceIndex, originalId.index);
    EXPECT_FLOAT_EQ(first->Objects()[0].world.m[0][3], 2);
    EXPECT_FLOAT_EQ(second->Objects()[0].world.m[0][3], 8);
}

} /// @note namespace
TEST_F(RenderSceneExtractorMaterialTest, EmptyEffectExtractionObservesComponentsAddedAndRemovedWithinTheSameFrame)
{
    renderer::RenderSettings settings;
    scene::RenderFrameGeometryCache cache;
    const auto empty = ExtractView(settings, cache);
    ASSERT_NE(empty, nullptr);
    EXPECT_TRUE(empty->trails.empty());
    EXPECT_TRUE(empty->decals.empty());
    EXPECT_TRUE(empty->fibers.empty());
    EXPECT_FALSE(empty->environment.sky.has_value());
    EXPECT_FALSE(empty->environment.sunMoon.has_value());

    auto& owner = m_scene.CreateGameObject("Effects");
    owner.AddComponent<scene::SkyRenderer>();
    auto& trail = owner.AddComponent<scene::TrailComponent>();
    trail.beamMode = true;
    trail.beamWorldSpace = true;
    trail.beamStart = {1.0f, 2.0f, 3.0f};
    trail.beamEnd = {4.0f, 5.0f, 6.0f};
    const auto added = ExtractView(settings, cache);
    ASSERT_NE(added, nullptr);
    ASSERT_EQ(added->trails.size(), 1u);
    ASSERT_EQ(added->trails[0].points.size(), 2u);
    EXPECT_VEC3_NEAR(added->trails[0].points[0].position, trail.beamStart, testkit::kTolerance);
    EXPECT_TRUE(added->environment.sky.has_value());
    EXPECT_TRUE(empty->trails.empty());
    EXPECT_FALSE(empty->environment.sky.has_value());

    owner.RemoveComponent<scene::SkyRenderer>();
    owner.RemoveComponent<scene::TrailComponent>();
    owner.AddComponent<scene::SunMoonRenderer>();
    const auto replaced = ExtractView(settings, cache);
    ASSERT_NE(replaced, nullptr);
    EXPECT_TRUE(replaced->trails.empty());
    EXPECT_FALSE(replaced->environment.sky.has_value());
    EXPECT_TRUE(replaced->environment.sunMoon.has_value());
    EXPECT_EQ(added->trails.size(), 1u);
    EXPECT_TRUE(added->environment.sky.has_value());
}

TEST_F(RenderSceneExtractorMaterialTest, TypedEnvironmentCandidatesKeepScenePrecedenceAndRefreshWithinOneFrame)
{
    auto& first = m_scene.CreateGameObject("First environment");
    const auto firstId = first.GetID();
    auto& second = m_scene.CreateGameObject("Second environment");
    const auto secondId = second.GetID();
    second.AddComponent<scene::SkyRenderer>().skyScatterIntensity = 22.0f;
    second.AddComponent<scene::SunMoonRenderer>().sunDiskIntensity = 44.0f;
    first.AddComponent<scene::SkyRenderer>().skyScatterIntensity = 11.0f;
    first.AddComponent<scene::SunMoonRenderer>().sunDiskIntensity = 33.0f;
    for (int i = 0; i < 64; ++i) m_scene.CreateGameObject("Bone");
    renderer::RenderSettings settings;
    scene::RenderFrameGeometryCache cache;
    const auto initial = ExtractView(settings, cache);
    ASSERT_TRUE(initial->environment.sky);
    ASSERT_TRUE(initial->environment.sunMoon);
    EXPECT_FLOAT_EQ(initial->environment.sky->skyScatterIntensity, 11.0f);
    EXPECT_FLOAT_EQ(initial->environment.sunMoon->sunDiskIntensity, 33.0f);

    first.layer = 2;
    const auto masked = ExtractView(settings, cache, ~(1u << 2));
    ASSERT_TRUE(masked->environment.sky);
    EXPECT_FLOAT_EQ(masked->environment.sky->skyScatterIntensity, 22.0f);
    first.layer = 0;
    first.SetActive(false);
    const auto inactive = ExtractView(settings, cache);
    ASSERT_TRUE(inactive->environment.sunMoon);
    EXPECT_FLOAT_EQ(inactive->environment.sunMoon->sunDiskIntensity, 44.0f);

    first.SetActive(true);
    first.RemoveComponent<scene::SkyRenderer>();
    first.RemoveComponent<scene::SunMoonRenderer>();
    first.AddComponent<scene::SkyRenderer>().skyScatterIntensity = 55.0f;
    first.AddComponent<scene::SunMoonRenderer>().sunDiskIntensity = 66.0f;
    const auto replaced = ExtractView(settings, cache);
    ASSERT_TRUE(replaced->environment.sky);
    EXPECT_FLOAT_EQ(replaced->environment.sky->skyScatterIntensity, 55.0f);
    EXPECT_FLOAT_EQ(initial->environment.sky->skyScatterIntensity, 11.0f);
    m_scene.DestroyGameObject(firstId);
    const auto destroyed = ExtractView(settings, cache);
    ASSERT_TRUE(destroyed->environment.sunMoon);
    EXPECT_FLOAT_EQ(destroyed->environment.sunMoon->sunDiskIntensity, 44.0f);
    EXPECT_NE(m_scene.GetGameObject(secondId), nullptr);
}

namespace {

namespace demand = scene::material_demand_detail;

void ExpectSameMaterialDemand(const demand::Result& actual, const demand::Result& expected)
{
    EXPECT_EQ(actual.reasons, expected.reasons);
    EXPECT_EQ(actual.cameraResult, expected.cameraResult);
    EXPECT_EQ(actual.matchesView, expected.matchesView);
    EXPECT_EQ(actual.cameraEvaluated, expected.cameraEvaluated);
    EXPECT_EQ(actual.numericUnproven, expected.numericUnproven);
}

class MaterialDemandContractTest : public testkit::Fixture {
protected:
    void SetUp() override
    {
        m_object.colorEligible = true;
        m_object.lodVisible = true;
        m_object.castShadows = true;
        m_object.layer = 2;
        m_object.boundsCenter = {0, -10, 10};
        m_object.boundsRadius = 1;
        m_item.visible = true;
        m_item.slotVisible = true;
        m_item.vertexBuffer = {1, 1};
        m_item.indexBuffer = {2, 1};
        m_item.material.capabilities.blend = renderer::BlendMode::OPAQUE_BLEND;
        /// @note Controlled value tests isolate known-consumer predicates; the production collector never certifies user-pass absence.
        m_view.consumerScopeCertified = true;
        m_view.materialCoverageKnown = true;
        m_view.culling.frustum = &m_frustum;
    }
    math::Frustum m_frustum;
    scene::material_demand_detail::View m_view;
    renderer::RenderObject m_object;
    renderer::RenderMeshItem m_item;
};

TEST_F(MaterialDemandContractTest, FreshViewMasksFrustaAndPaddingDoNotMutateGeometry)
{
    const auto outside = scene::material_demand_detail::Evaluate(m_view, m_object, m_item);
    EXPECT_EQ(outside.cameraResult, renderer::VisibilityResult::FRUSTUM_CULLED);
    EXPECT_FALSE(outside.KnownConsumerPotential());
    m_view.boundsPadding = 10;
    EXPECT_TRUE(scene::material_demand_detail::Evaluate(m_view, m_object, m_item).KnownConsumerPotential());
    m_view.mask = uint32_t{1} << 1;
    EXPECT_FALSE(scene::material_demand_detail::Evaluate(m_view, m_object, m_item).matchesView);
    m_view.mask = uint32_t{1} << 2;
    m_view.boundsPadding = 0;
    m_view.culling.frustum = nullptr;
    EXPECT_TRUE(scene::material_demand_detail::Evaluate(m_view, m_object, m_item).KnownConsumerPotential());
    m_object.lodVisible = false;
    EXPECT_FALSE(scene::material_demand_detail::Evaluate(m_view, m_object, m_item).KnownConsumerPotential());
    EXPECT_VEC3_NEAR(m_object.boundsCenter, (math::Vector3{0, -10, 10}), testkit::kTolerance);
    EXPECT_EQ(m_item.vertexBuffer, (renderer::ResourceHandle<renderer::BufferTag>{1, 1}));
    EXPECT_TRUE(m_item.visible);
    EXPECT_TRUE(m_item.slotVisible);
}

TEST_F(MaterialDemandContractTest, LayerDistanceAndOrthographicSmallObjectPolicyUseCurrentValues)
{
    m_view.culling.frustum = nullptr;
    m_object.boundsCenter = {0, 0, 10};
    m_view.maxDistance = 5;
    EXPECT_EQ(scene::material_demand_detail::Evaluate(m_view, m_object, m_item).cameraResult,
        renderer::VisibilityResult::DISTANCE_CULLED);
    m_view.maxDistance = 20;
    m_view.hasLayerDistances = true;
    m_view.layerDistances[2] = 4;
    EXPECT_EQ(scene::material_demand_detail::Evaluate(m_view, m_object, m_item).cameraResult,
        renderer::VisibilityResult::DISTANCE_CULLED);
    m_view.hasLayerDistances = false;
    m_view.culling.projectionScaleY = 1;
    m_view.culling.smallObjectScreenHeight = 0.2f;
    EXPECT_EQ(scene::material_demand_detail::Evaluate(m_view, m_object, m_item).cameraResult,
        renderer::VisibilityResult::SMALL_OBJECT_CULLED);
    m_view.culling.orthographic = true;
    EXPECT_EQ(scene::material_demand_detail::Evaluate(m_view, m_object, m_item).cameraResult,
        renderer::VisibilityResult::VISIBLE);
}

TEST_F(MaterialDemandContractTest, OffCameraDirectionalAndPunctualShadowPotentialRemainRequired)
{
    m_view.directionalShadowPotential = true;
    auto result = scene::material_demand_detail::Evaluate(m_view, m_object, m_item);
    EXPECT_EQ(result.cameraResult, renderer::VisibilityResult::FRUSTUM_CULLED);
    EXPECT_NE(result.reasons & scene::material_demand_detail::DIRECTIONAL_SHADOW_POTENTIAL, 0u);
    m_view.directionalShadowPotential = false;
    m_view.punctualShadowPotential = true;
    result = scene::material_demand_detail::Evaluate(m_view, m_object, m_item);
    EXPECT_NE(result.reasons & scene::material_demand_detail::PUNCTUAL_SHADOW_POTENTIAL, 0u);
    m_object.castShadows = false;
    EXPECT_FALSE(scene::material_demand_detail::Evaluate(m_view, m_object, m_item).KnownConsumerPotential());
    m_object.castShadows = true;
    m_view.maxDistance = 2;
    EXPECT_FALSE(scene::material_demand_detail::Evaluate(m_view, m_object, m_item).KnownConsumerPotential());
}

TEST_F(MaterialDemandContractTest, StaticProbePotentialIgnoresMainCameraFrustumAndSkinnedInputs)
{
    m_view.reflectionProbePotential = true;
    m_view.lightProbePotential = true;
    auto result = scene::material_demand_detail::Evaluate(m_view, m_object, m_item);
    EXPECT_NE(result.reasons & scene::material_demand_detail::REFLECTION_PROBE_POTENTIAL, 0u);
    EXPECT_NE(result.reasons & scene::material_demand_detail::LIGHT_PROBE_POTENTIAL, 0u);
    m_item.material.capabilities.blend = renderer::BlendMode::ALPHA_BLEND;
    result = scene::material_demand_detail::Evaluate(m_view, m_object, m_item);
    EXPECT_NE(result.reasons & scene::material_demand_detail::REFLECTION_PROBE_POTENTIAL, 0u);
    EXPECT_EQ(result.reasons & scene::material_demand_detail::LIGHT_PROBE_POTENTIAL, 0u);
    m_object.skinned = true;
    EXPECT_FALSE(scene::material_demand_detail::Evaluate(m_view, m_object, m_item).KnownConsumerPotential());
}

TEST_F(MaterialDemandContractTest, RayAndUncertifiedUserPassScopeNeverAuthorizeCameraOnlyOmission)
{
    m_view.rayPotential = true;
    m_view.mask = 0;
    m_object.rayVisible = true;
    auto result = scene::material_demand_detail::Evaluate(m_view, m_object, m_item);
    EXPECT_NE(result.reasons & scene::material_demand_detail::RAY_POTENTIAL, 0u);
    m_object.rayVisible = false;
    m_object.rayLodSelectionRequired = true;
    EXPECT_NE(scene::material_demand_detail::Evaluate(m_view, m_object, m_item).reasons
        & scene::material_demand_detail::RAY_POTENTIAL, 0u);
    m_view.rayPotential = false;
    m_view.consumerScopeCertified = false;
    result = scene::material_demand_detail::Evaluate(m_view, m_object, m_item);
    EXPECT_NE(result.reasons & scene::material_demand_detail::UNKNOWN_CONSUMER, 0u);
    EXPECT_FALSE(result.KnownConsumerPotential());
    m_item.visible = false;
    EXPECT_EQ(scene::material_demand_detail::Evaluate(m_view, m_object, m_item).reasons, 0u);
}

TEST_F(MaterialDemandContractTest, NonfiniteOrExtremeValuesAreConservativeBeforeMathVisibility)
{
    m_object.boundsCenter.x = std::numeric_limits<float>::quiet_NaN();
    auto result = scene::material_demand_detail::Evaluate(m_view, m_object, m_item);
    EXPECT_TRUE(result.numericUnproven);
    EXPECT_FALSE(result.cameraEvaluated);
    EXPECT_NE(result.reasons & scene::material_demand_detail::NUMERIC_FALLBACK, 0u);
    m_object.boundsCenter = {1.0e30f, 0, 1};
    result = scene::material_demand_detail::Evaluate(m_view, m_object, m_item);
    EXPECT_TRUE(result.numericUnproven);
    EXPECT_FALSE(result.cameraEvaluated);
    m_object.boundsCenter = {0, -10, 10};
    m_object.boundsRadius = 0;
    result = scene::material_demand_detail::Evaluate(m_view, m_object, m_item);
    EXPECT_EQ(result.cameraResult, renderer::VisibilityResult::VISIBLE);
    EXPECT_TRUE(result.KnownConsumerPotential());
}

TEST_F(MaterialDemandContractTest, SharedObjectVisibilityMatchesLegacyForMixedSlotsAndSkinning)
{
    m_view.staticGpuFrustumPossible = true;
    m_view.directionalShadowPotential = true;
    m_view.punctualShadowPotential = true;
    m_view.reflectionProbePotential = true;
    m_view.lightProbePotential = true;
    m_view.rayPotential = true;
    m_object.rayVisible = false;
    m_object.rayLodSelectionRequired = true;
    std::array<renderer::RenderMeshItem, 9> slots;
    slots.fill(m_item);
    slots[0].visible = false;
    slots[1].slotVisible = false;
    slots[2].vertexBuffer = {};
    slots[4].material.capabilities.blend = renderer::BlendMode::ALPHA_BLEND;
    slots[5].indexBuffer = {};
    slots[6].vertexBuffer = {};
    slots[6].skinningVertexBuffer = {3, 1};
    slots[7].vertexBuffer = {};
    slots[7].deformedVertexBuffer = {4, 1};
    slots[8].vertexBuffer = {};

    for (bool skinned : {false, true}) {
        m_object.skinned = skinned;
        for (bool previousSkinning : {false, true}) {
            m_object.previousSkinningValid = previousSkinning;
            for (bool coverageKnown : {false, true}) {
                m_view.materialCoverageKnown = coverageKnown;
                for (bool cameraVisible : {false, true}) {
                    m_object.boundsCenter.y = cameraVisible ? 0.0f : -10.0f;
                    const demand::PreparedView prepared(m_view);
                    demand::ObjectEvaluator evaluator(prepared, m_object);
                    for (const auto& slot : slots)
                        ExpectSameMaterialDemand(evaluator.Evaluate(slot), demand::Evaluate(m_view, m_object, slot));
                }
            }
        }
    }
}

TEST_F(MaterialDemandContractTest, PreparedViewKeepsNumericFallbackLocalToTheSelectedLayer)
{
    const std::array invalidValues{std::numeric_limits<float>::quiet_NaN(),
        std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(), 1.0e30f, -1.0e30f};
    for (float* field : {&m_view.boundsPadding, &m_view.culling.position.x, &m_view.culling.position.y,
            &m_view.culling.position.z, &m_view.culling.forward.x, &m_view.culling.forward.y,
            &m_view.culling.forward.z, &m_view.culling.projectionScaleY,
            &m_view.culling.smallObjectScreenHeight, &m_view.maxDistance, &m_view.layerDistances[2]}) {
        const float original = *field;
        for (float invalid : invalidValues) {
            *field = invalid;
            const demand::PreparedView prepared(m_view);
            demand::ObjectEvaluator evaluator(prepared, m_object);
            EXPECT_EQ(demand::SafeObjectCulling(prepared, m_object), demand::SafeCulling(m_view, m_object));
            for (int reuse = 0; reuse < 2; ++reuse) {
                const auto actual = evaluator.Evaluate(m_item);
                ExpectSameMaterialDemand(actual, demand::Evaluate(m_view, m_object, m_item));
                EXPECT_TRUE(actual.numericUnproven);
                EXPECT_FALSE(actual.cameraEvaluated);
            }
            auto missingGeometry = m_item;
            missingGeometry.vertexBuffer = {};
            const auto missing = evaluator.Evaluate(missingGeometry);
            ExpectSameMaterialDemand(missing, demand::Evaluate(m_view, m_object, missingGeometry));
            EXPECT_FALSE(missing.numericUnproven);
            EXPECT_FALSE(missing.cameraEvaluated);
        }
        *field = original;
    }

    m_view.layerDistances[1] = std::numeric_limits<float>::quiet_NaN();
    const demand::PreparedView prepared(m_view);
    demand::ObjectEvaluator evaluator(prepared, m_object);
    const auto actual = evaluator.Evaluate(m_item);
    ExpectSameMaterialDemand(actual, demand::Evaluate(m_view, m_object, m_item));
    EXPECT_FALSE(actual.numericUnproven);
    EXPECT_TRUE(actual.cameraEvaluated);
}

TEST_F(MaterialDemandContractTest, PreparedViewChecksEveryFrustumPlaneBeforeSharingVisibility)
{
    for (int axis = 0; axis < 3; ++axis) {
        auto projection = math::Matrix4::Identity();
        projection.m[axis][3] = std::numeric_limits<float>::quiet_NaN();
        m_frustum = math::Frustum::FromViewProjection(projection);
        const demand::PreparedView prepared(m_view);
        demand::ObjectEvaluator evaluator(prepared, m_object);

        EXPECT_EQ(demand::SafeObjectCulling(prepared, m_object), demand::SafeCulling(m_view, m_object));
        const auto actual = evaluator.Evaluate(m_item);
        ExpectSameMaterialDemand(actual, demand::Evaluate(m_view, m_object, m_item));
        EXPECT_TRUE(actual.numericUnproven);
        EXPECT_EQ(actual.reasons, static_cast<uint32_t>(demand::NUMERIC_FALLBACK));
    }
}

TEST_F(MaterialDemandContractTest, SharedObjectVisibilityMatchesLegacyDepthBoundsAndRejectionPolicies)
{
    m_view.culling.frustum = nullptr;
    m_view.culling.projectionScaleY = 1.0f;
    m_view.culling.smallObjectScreenHeight = 0.2f;
    m_object.boundsCenter = {0, 0, 10};
    const auto check = [&] {
        const demand::PreparedView prepared(m_view);
        demand::ObjectEvaluator evaluator(prepared, m_object);
        EXPECT_EQ(demand::SafeObjectCulling(prepared, m_object), demand::SafeCulling(m_view, m_object));
        for (int reuse = 0; reuse < 2; ++reuse)
            ExpectSameMaterialDemand(evaluator.Evaluate(m_item), demand::Evaluate(m_view, m_object, m_item));
    };

    for (bool spherical : {false, true}) {
        m_view.culling.distanceSpherical = spherical;
        for (float depth : {-1.0f, -1.0e-12f, 0.0f, 1.0e-12f, 1.0e-10f, 10.0f, 1.0e10f,
                std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
            m_object.boundsCenter.z = depth;
            check();
        }
    }
    m_object.boundsCenter = {0, 0, 10};
    for (float radius : {-1.0f, 0.0f, 1.0f, 1.0e10f, 1.0e30f, std::numeric_limits<float>::quiet_NaN()}) {
        m_object.boundsRadius = radius;
        check();
    }
    m_object.boundsRadius = 1.0f;
    m_view.maxDistance = 5.0f;
    check();
    m_view.hasLayerDistances = true;
    m_view.layerDistances[2] = 20.0f;
    check();
    m_view.culling.orthographic = true;
    check();
    m_view.culling.frustum = &m_frustum;
    m_object.boundsCenter.y = -10.0f;
    m_view.staticGpuFrustumPossible = true;
    check();
    m_object.lodDither = 0.5f;
    check();
    m_object.lodVisible = false;
    check();
    m_object.lodVisible = true;
    m_view.mask = 0;
    m_view.rayPotential = true;
    check();
    m_view.consumerScopeCertified = false;
    check();
    m_view.mask = UINT32_MAX;
    for (uint32_t layer : {0u, 2u, 31u, 32u, UINT32_MAX}) {
        m_object.layer = layer;
        check();
    }
    m_object.colorEligible = false;
    check();
}

TEST_F(MaterialDemandContractTest, IndependentPreparationsObserveDirectChangesAndKeepFrozenOuterView)
{
    const auto frozenFrustum = m_frustum;
    auto frozenView = m_view;
    frozenView.culling.frustum = &frozenFrustum;
    const auto frozenObject = m_object;
    const demand::PreparedView outerView(frozenView);
    demand::ObjectEvaluator outer(outerView, frozenObject);
    const auto before = outer.Evaluate(m_item);
    EXPECT_EQ(before.reasons, 0u);
    EXPECT_TRUE(before.cameraEvaluated);

    m_frustum = math::Frustum::FromViewProjection(math::Matrix4::Identity());
    m_object.boundsCenter = {0, 0, 0.5f};
    {
        const demand::PreparedView currentView(m_view);
        demand::ObjectEvaluator current(currentView, m_object);
        const auto actual = current.Evaluate(m_item);
        ExpectSameMaterialDemand(actual, demand::Evaluate(m_view, m_object, m_item));
        EXPECT_NE(actual.reasons & demand::COLOR, 0u);
    }
    ExpectSameMaterialDemand(outer.Evaluate(m_item), before);
    ExpectSameMaterialDemand(outer.Evaluate(m_item), demand::Evaluate(frozenView, frozenObject, m_item));

    m_view.mask = 0;
    const demand::PreparedView nextView(m_view);
    demand::ObjectEvaluator next(nextView, m_object);
    const auto changed = next.Evaluate(m_item);
    ExpectSameMaterialDemand(changed, demand::Evaluate(m_view, m_object, m_item));
    EXPECT_FALSE(changed.matchesView);
    EXPECT_FALSE(changed.cameraEvaluated);
}

} /// @note namespace

TEST_F(RenderSceneExtractorMaterialTest, DemandDiagnosticCandidatePreservesMaskRejectedItemsAndMutableSlots)
{
    ASSERT_TRUE(WriteShader("Demand.hlsl", true));
    asset::MaterialAssetAuthoring asset;
    asset.shaderPath = ShaderPath("Demand.hlsl");
    const auto handle = LoadMaterial("Demand.mat", asset);
    ASSERT_TRUE(handle.IsValid());
    auto& slot = AddMaterialObject(handle);
    EditMaterialAuthoring(slot, [&](auto& values) { values.hasBlendModeOverride = true; });
    EditMaterialAuthoring(slot, [&](auto& values) { values.blendModeOverride = renderer::BlendMode::ALPHA_BLEND; });
    renderer::RenderSettings settings;
    scene::RenderFrameGeometryCache cache;
    const auto first = ExtractView(settings, cache, 0);
    ASSERT_EQ(first->Objects().size(), 1u);
    ASSERT_EQ(first->items.size(), 1u);
    EXPECT_TRUE(first->items[0].visible);
    EXPECT_TRUE(first->items[0].slotVisible);
    const auto identity = first->Objects()[0].sourceIndex;
    slot.visible = false;
    const auto second = ExtractView(settings, cache, UINT32_MAX);
    ASSERT_EQ(second->Objects().size(), 1u);
    ASSERT_EQ(second->items.size(), 1u);
    EXPECT_EQ(second->Objects()[0].sourceIndex, identity);
    EXPECT_FALSE(second->items[0].slotVisible);
    EXPECT_TRUE(first->items[0].slotVisible);
    EXPECT_TRUE(slot.Authoring().hasBlendModeOverride);
    EXPECT_EQ(slot.Authoring().blendModeOverride, renderer::BlendMode::ALPHA_BLEND);
}


namespace {
class GuidMeshResolutionTest : public testkit::EngineFixture {
protected:
    void SetUp() override
    {
        EngineFixture::SetUp(); ASSERT_TRUE(m_temp.IsValid());
        m_resources = std::make_unique<renderer::ResourceManager>(m_renderer);
        asset::AssetManager::Init(*m_resources, m_temp.Path().generic_string() + "/");
    }
    void TearDown() override
    {
        asset::AssetManager::UnloadAll(); m_resources.reset(); EngineFixture::TearDown();
    }
    std::string MakeReference(const std::string& name)
    {
        const auto path = m_temp.File(name).generic_string();
        WriteModel(path);
        const auto guid = asset::AssetDatabase::GuidFromPath(path); EXPECT_FALSE(guid.empty());
        return "guid:" + guid;
    }
    void WriteModel(const std::string& path)
    {
        const int faces = m_marker / 3;
        std::string ply = "ply\nformat ascii 1.0\nelement vertex " + std::to_string(m_marker)
            + "\nproperty float x\nproperty float y\nproperty float z\nelement face " + std::to_string(faces)
            + "\nproperty list uchar int vertex_indices\nend_header\n";
        for (int i = 0; i < faces; ++i) {
            const auto x = std::to_string(i * 2);
            ply += x + " 0 0\n" + x + " 1 0\n" + std::to_string(i * 2 + 1) + " 0 0\n";
        }
        for (int i = 0; i < faces; ++i)
            ply += "3 " + std::to_string(i * 3) + " " + std::to_string(i * 3 + 1) + " " + std::to_string(i * 3 + 2) + "\n";
        EXPECT_TRUE(util::FileSystem::WriteText(path, ply));
    }
    testkit::TempDir m_temp{ "guid-mesh-resolution" };
    ExtractionCpuRenderer m_renderer;
    std::unique_ptr<renderer::ResourceManager> m_resources;
    int m_marker = 3;
};
} /// @note namespace

TEST_F(GuidMeshResolutionTest, RepeatedGuidLoadsObserveExplicitUnloadAndCurrentModelContents)
{
    const auto reference = MakeReference("first.ply");
    auto* first = scene::ResolveMeshPath(reference, *m_resources); ASSERT_NE(first, nullptr);
    EXPECT_EQ(first->indexCount, 3u); EXPECT_EQ(scene::ResolveMeshPath(reference, *m_resources), first);
    m_marker = 6; WriteModel(m_temp.File("first.ply").generic_string()); asset::AssetManager::Unload<asset::Model>(reference);
    auto* second = scene::ResolveMeshPath(reference, *m_resources); ASSERT_NE(second, nullptr);
    EXPECT_EQ(second->indexCount, 6u);
    auto* model = asset::AssetManager::LoadAndGet<asset::Model>(reference); ASSERT_NE(model, nullptr);
    auto replacement = std::make_unique<renderer::Mesh>(); replacement->indexCount = 9;
    model->meshes[0] = std::move(replacement);
    auto* third = scene::ResolveMeshPath(reference, *m_resources); ASSERT_NE(third, nullptr); EXPECT_EQ(third->indexCount, 9u);
}

TEST_F(GuidMeshResolutionTest, StableGuidFollowsIndexMovesAndOldPathHints)
{
    const auto reference = MakeReference("before.ply");
    auto* before = scene::ResolveMeshPath(reference, *m_resources); ASSERT_NE(before, nullptr); EXPECT_EQ(before->indexCount, 3u);
    const auto oldPath = m_temp.File("before.ply").generic_string();
    const auto newPath = m_temp.File("after.ply").generic_string();
    std::error_code error; std::filesystem::rename(oldPath, newPath, error); ASSERT_FALSE(error);
    std::filesystem::rename(oldPath + ".meta", newPath + ".meta", error); ASSERT_FALSE(error);
    asset::AssetDatabase::OnAssetMoved(oldPath, newPath); m_marker = 12; WriteModel(newPath);
    auto* after = scene::ResolveMeshPath(reference + "|" + oldPath, *m_resources); ASSERT_NE(after, nullptr); EXPECT_EQ(after->indexCount, 12u);
    EXPECT_EQ(scene::ResolveMeshPath(reference, *m_resources), after);
}

TEST_F(GuidMeshResolutionTest, ProjectRetirementDoesNotReviveCachedModelHandles)
{
    const auto reference = MakeReference("retired.ply");
    ASSERT_NE(scene::ResolveMeshPath(reference, *m_resources), nullptr);
    asset::AssetManager::UnloadAll();
    asset::AssetManager::Init(*m_resources, m_temp.Path().generic_string() + "/");
    m_marker = 15; WriteModel(m_temp.File("retired.ply").generic_string());
    auto* current = scene::ResolveMeshPath(reference, *m_resources); ASSERT_NE(current, nullptr); EXPECT_EQ(current->indexCount, 15u);
}

TEST_F(RenderSceneExtractorTest, DenseGeometryKeepsChunkEdgesSharedMeshEditsAndSnapshotOwnership)
{
    m_mesh.vertexBuffer = {3, 2}; m_mesh.indexBuffer = {4, 2};
    m_mesh.vertexCount = 42; m_mesh.indexCount = 63;
    m_mesh.boundsCenter = {1, 2, 3}; m_mesh.boundsRadius = 4; m_mesh.boundsExtents = {2, 2, 2};
    std::vector<scene::EntityID> ids;
    for (int i = 0; i < 131; ++i) {
        auto& owner = StaticObject(); ids.push_back(owner.GetID());
        owner.transform.position = {static_cast<float>(i), 0, 0};
        owner.transform.scale = {-2, 3, 1};
    }
    scene::FlushWorldTransforms(m_scene);
    const auto first = scene::ExtractRenderSceneGeometry(m_scene, 20);
    ASSERT_EQ(first.Objects().size(), ids.size()); ASSERT_EQ(first.items.size(), ids.size());
    for (size_t i = 0; i < ids.size(); ++i) {
        EXPECT_EQ(first.Objects()[i].sourceIndex, ids[i].index);
        EXPECT_EQ(first.items[i].objectIndex, i);
        EXPECT_EQ(first.items[i].vertexCount, 42u);
        EXPECT_EQ(first.items[i].boundsRadius, 12.0f);
        EXPECT_VEC3_NEAR(first.items[i].boundsCenter, (math::Vector3{static_cast<float>(i)-2, 6, 3}), 0.0f);
    }
    m_mesh.vertexCount = 87; m_mesh.indexCount = 120; m_mesh.vertexBuffer = {9, 4};
    m_mesh.boundsRadius = 2; m_mesh.boundsCenter = {2, 0, 0};
    m_scene.GetGameObject(ids[64])->SetActive(false);
    const auto second = scene::ExtractRenderSceneGeometry(m_scene, 20);
    ASSERT_EQ(second.items.size(), ids.size()-1);
    EXPECT_EQ(second.Objects()[64].sourceIndex, ids[65].index);
    EXPECT_EQ(second.items[64].objectIndex, 64u);
    EXPECT_EQ(second.items[64].vertexBuffer, m_mesh.vertexBuffer);
    EXPECT_EQ(second.items[64].vertexCount, 87u);
    EXPECT_EQ(second.items[64].boundsRadius, 6.0f);
    EXPECT_EQ(first.items[64].vertexCount, 42u);
    EXPECT_EQ(first.items[64].vertexBuffer, (renderer::ResourceHandle<renderer::BufferTag>{3, 2}));
    EXPECT_EQ(first.items[64].boundsRadius, 12.0f);
}

TEST_F(RenderSpatialPublicationTest, KeepsNoChangePublicationsImmutableAndMarksDirectWorldEdits)
{
    StaticObject();
    auto& moving = StaticObject();
    StaticObject();
    const auto warm = Warm();
    ASSERT_NE(warm.spatialUpdate, nullptr);
    EXPECT_FALSE(warm.spatialUpdate->fullReplacement);
    EXPECT_TRUE(warm.spatialUpdate->changedObjectOrdinals.empty());
    const auto unchanged = scene::ExtractRenderSceneGeometry(m_scene, 11);
    ASSERT_NE(unchanged.spatialUpdate, nullptr);
    EXPECT_EQ(unchanged.spatialUpdate->baseVersion, warm.spatialUpdate->version);
    EXPECT_NE(unchanged.spatialUpdate->version, warm.spatialUpdate->version);
    EXPECT_EQ(unchanged.spatialUpdate->objectCount, 3u);
    EXPECT_TRUE(unchanged.spatialUpdate->changedObjectOrdinals.empty());

    moving.transform.worldPosition = {19, 2, 3};
    const auto moved = scene::ExtractRenderSceneGeometry(m_scene, 12);
    ASSERT_NE(moved.spatialUpdate, nullptr);
    EXPECT_FALSE(moved.spatialUpdate->fullReplacement);
    EXPECT_EQ(moved.spatialUpdate->baseVersion, unchanged.spatialUpdate->version);
    EXPECT_EQ(moved.spatialUpdate->changedObjectOrdinals, (std::vector<uint32_t>{1}));
    EXPECT_VEC3_NEAR(moved.Objects()[1].boundsCenter, (math::Vector3{19, 2, 3}), 0.0f);
    EXPECT_TRUE(unchanged.spatialUpdate->changedObjectOrdinals.empty());
    EXPECT_VEC3_NEAR(warm.Objects()[1].boundsCenter, math::Vector3::ZERO, 0.0f);
}

TEST_F(RenderSpatialPublicationTest, IncludesParentPropagationAndRawTransformRestoreWithoutNotifications)
{
    auto& parent = StaticObject();
    auto& child = StaticObject();
    child.SetParent(parent);
    child.transform.position = {2, 0, 0};
    scene::FlushWorldTransforms(m_scene);
    const auto saved = parent.transform;
    const auto warm = Warm();
    ASSERT_NE(warm.spatialUpdate, nullptr);
    parent.transform.position = {5, 0, 0};
    scene::FlushWorldTransforms(parent);
    const auto moved = scene::ExtractRenderSceneGeometry(m_scene, 11);
    ASSERT_NE(moved.spatialUpdate, nullptr);
    EXPECT_FALSE(moved.spatialUpdate->fullReplacement);
    EXPECT_EQ(moved.spatialUpdate->changedObjectOrdinals, (std::vector<uint32_t>{0, 1}));
    EXPECT_VEC3_NEAR(moved.Objects()[1].boundsCenter, (math::Vector3{7, 0, 0}), 0.0f);

    /// @note Undo restores the public Transform value before the existing hierarchy flush.
    parent.transform = saved;
    scene::FlushWorldTransforms(m_scene);
    const auto restored = scene::ExtractRenderSceneGeometry(m_scene, 12);
    ASSERT_NE(restored.spatialUpdate, nullptr);
    EXPECT_EQ(restored.spatialUpdate->baseVersion, moved.spatialUpdate->version);
    EXPECT_EQ(restored.spatialUpdate->changedObjectOrdinals, (std::vector<uint32_t>{0, 1}));
    EXPECT_VEC3_NEAR(restored.Objects()[1].boundsCenter, (math::Vector3{2, 0, 0}), 0.0f);
    EXPECT_VEC3_NEAR(moved.Objects()[1].boundsCenter, (math::Vector3{7, 0, 0}), 0.0f);
}

TEST_F(RenderSpatialPublicationTest, IncludesEveryOwnerOfDirectlyEditedSharedMeshBounds)
{
    m_mesh.boundsRadius = 2;
    StaticObject();
    auto& second = StaticObject();
    second.transform.worldPosition = {10, 0, 0};
    const auto warm = Warm();
    m_mesh.boundsCenter = {3, 1, 0};
    m_mesh.boundsRadius = 7;
    const auto edited = scene::ExtractRenderSceneGeometry(m_scene, 11);
    ASSERT_NE(edited.spatialUpdate, nullptr);
    EXPECT_FALSE(edited.spatialUpdate->fullReplacement);
    EXPECT_EQ(edited.spatialUpdate->changedObjectOrdinals, (std::vector<uint32_t>{0, 1}));
    EXPECT_VEC3_NEAR(edited.Objects()[0].boundsCenter, (math::Vector3{3, 1, 0}), 0.0f);
    EXPECT_VEC3_NEAR(edited.Objects()[1].boundsCenter, (math::Vector3{13, 1, 0}), 0.0f);
    EXPECT_FLOAT_EQ(edited.Objects()[1].boundsRadius, 7);
    EXPECT_FLOAT_EQ(warm.Objects()[1].boundsRadius, 2);
}

TEST_F(RenderSpatialPublicationTest, ResetsLineageForOrderComponentLifetimeEntityReuseAndEmptyTransitions)
{
    auto& first = StaticObject();
    auto& second = StaticObject();
    const auto secondId = second.GetID();
    const auto warm = Warm();
    ASSERT_TRUE(second.SetSiblingIndex(0));
    const auto reordered = scene::ExtractRenderSceneGeometry(m_scene, 11);
    ASSERT_NE(reordered.spatialUpdate, nullptr);
    EXPECT_TRUE(reordered.spatialUpdate->fullReplacement);
    EXPECT_EQ(reordered.spatialUpdate->baseVersion, 0u);
    EXPECT_EQ(reordered.Objects()[0].sourceIndex, secondId.index);

    second.RemoveComponent<scene::MeshRenderer>();
    second.AddComponent<scene::MeshRenderer>().mesh = &m_mesh;
    const auto replaced = scene::ExtractRenderSceneGeometry(m_scene, 12);
    ASSERT_NE(replaced.spatialUpdate, nullptr);
    EXPECT_TRUE(replaced.spatialUpdate->fullReplacement);
    EXPECT_EQ(replaced.spatialUpdate->baseVersion, 0u);
    ASSERT_TRUE(m_scene.DestroyGameObject(secondId));
    auto& reused = StaticObject();
    EXPECT_EQ(reused.GetID().index, secondId.index);
    EXPECT_NE(reused.GetID().generation, secondId.generation);
    const auto recreated = scene::ExtractRenderSceneGeometry(m_scene, 13);
    ASSERT_NE(recreated.spatialUpdate, nullptr);
    EXPECT_TRUE(recreated.spatialUpdate->fullReplacement);
    first.SetActive(false);
    reused.SetActive(false);
    const auto empty = scene::ExtractRenderSceneGeometry(m_scene, 14);
    ASSERT_NE(empty.spatialUpdate, nullptr);
    EXPECT_TRUE(empty.spatialUpdate->fullReplacement);
    EXPECT_EQ(empty.spatialUpdate->objectCount, 0u);
    const auto stillEmpty = scene::ExtractRenderSceneGeometry(m_scene, 15);
    ASSERT_NE(stillEmpty.spatialUpdate, nullptr);
    EXPECT_FALSE(stillEmpty.spatialUpdate->fullReplacement);
    EXPECT_EQ(stillEmpty.spatialUpdate->baseVersion, empty.spatialUpdate->version);
    EXPECT_TRUE(stillEmpty.spatialUpdate->changedObjectOrdinals.empty());
    EXPECT_EQ(warm.spatialUpdate->objectCount, 2u);
}

TEST_F(RenderSpatialPublicationTest, StartsAFullPublicationAfterSerializerRestorationAndSceneGenerationChange)
{
    auto& owner = StaticObject();
    owner.transform.position = {3, 4, 5};
    scene::FlushWorldTransforms(m_scene);
    const auto warm = Warm();
    const auto text = scene::SceneSerializer::SaveToText(m_scene);
    ASSERT_FALSE(text.empty());
    auto loaded = scene::SceneSerializer::LoadDataFromText(text, "");
    ASSERT_NE(loaded, nullptr);
    auto* restoredOwner = loaded->Find("Mesh");
    ASSERT_NE(restoredOwner, nullptr);
    ASSERT_NE(restoredOwner->GetComponent<scene::MeshRenderer>(), nullptr);
    restoredOwner->GetComponent<scene::MeshRenderer>()->mesh = &m_mesh;
    m_scene = std::move(*loaded);
    scene::FlushWorldTransforms(m_scene);
    const auto restored = scene::ExtractRenderSceneGeometry(m_scene, 11);
    ASSERT_NE(restored.spatialUpdate, nullptr);
    EXPECT_NE(restored.sceneGeneration, warm.sceneGeneration);
    EXPECT_TRUE(restored.spatialUpdate->fullReplacement);
    EXPECT_EQ(restored.spatialUpdate->baseVersion, 0u);
    ASSERT_EQ(restored.Objects().size(), 1u);
    EXPECT_VEC3_NEAR(restored.Objects()[0].boundsCenter, (math::Vector3{3, 4, 5}), 0.0f);
    EXPECT_VEC3_NEAR(warm.Objects()[0].boundsCenter, (math::Vector3{3, 4, 5}), 0.0f);
}

TEST_F(RenderSpatialPublicationTest, ConservativelyMarksSkinnedBoundsOnEveryPublication)
{
    StaticObject();
    auto& owner = SkinnedObject();
    auto& animator = owner.AddComponent<scene::AnimatorComponent>();
    animator.skinnedBoundsCenter = {1, 0, 0};
    animator.skinnedBoundsRadius = 3;
    const auto warm = Warm();
    ASSERT_NE(warm.spatialUpdate, nullptr);
    EXPECT_EQ(warm.spatialUpdate->changedObjectOrdinals, (std::vector<uint32_t>{1}));
    animator.skinnedBoundsCenter = {9, 2, 3};
    animator.skinnedBoundsRadius = 8;
    const auto animated = scene::ExtractRenderSceneGeometry(m_scene, 11);
    ASSERT_NE(animated.spatialUpdate, nullptr);
    EXPECT_FALSE(animated.spatialUpdate->fullReplacement);
    EXPECT_EQ(animated.spatialUpdate->changedObjectOrdinals, (std::vector<uint32_t>{1}));
    EXPECT_VEC3_NEAR(animated.Objects()[1].boundsCenter, (math::Vector3{9, 2, 3}), 0.0f);
    EXPECT_FLOAT_EQ(animated.Objects()[1].boundsRadius, 11);
    EXPECT_FLOAT_EQ(warm.Objects()[1].boundsRadius, 6);
}

TEST_F(RenderSpatialPublicationTest, IncludesConsecutiveMotionAndDenseFallbackBeforeReturningToEmptyDeltas)
{
    std::array<scene::EntityID, 128> ids;
    for (auto& id : ids) id = StaticObject().GetID();
    (void)Warm(1);
    for (uint64_t frame = 2; frame <= 6; ++frame) {
        for (auto id : ids) m_scene.GetGameObject(id)->transform.worldPosition.x = static_cast<float>(frame);
        const auto moving = scene::ExtractRenderSceneGeometry(m_scene, frame);
        ASSERT_NE(moving.spatialUpdate, nullptr);
        EXPECT_FALSE(moving.spatialUpdate->fullReplacement);
        ASSERT_EQ(moving.spatialUpdate->changedObjectOrdinals.size(), ids.size());
        EXPECT_EQ(moving.spatialUpdate->changedObjectOrdinals.front(), 0u);
        EXPECT_EQ(moving.spatialUpdate->changedObjectOrdinals.back(), 127u);
        EXPECT_FLOAT_EQ(moving.Objects().back().boundsCenter.x, static_cast<float>(frame));
    }
    renderer::RenderScene stopped;
    for (uint64_t frame = 7; frame <= 26; ++frame)
        stopped = scene::ExtractRenderSceneGeometry(m_scene, frame);
    ASSERT_NE(stopped.spatialUpdate, nullptr);
    EXPECT_FALSE(stopped.spatialUpdate->fullReplacement);
    EXPECT_TRUE(stopped.spatialUpdate->changedObjectOrdinals.empty());
    EXPECT_FLOAT_EQ(stopped.Objects().back().boundsCenter.x, 6);
}

TEST_F(RenderSpatialPublicationTest, LeavesLegacyAndUnknownModesUncertifiedAndRestartsJournalLineage)
{
    StaticObject();
    const auto warm = Warm();
    ASSERT_NE(warm.spatialUpdate, nullptr);
    for (const char* mode : std::array<const char*, 5>{nullptr, "legacy", "Journal", "journal ", "1"}) {
        const SpatialProducerEnvironment legacy(mode);
        const auto snapshot = scene::ExtractRenderSceneGeometry(m_scene, 11);
        EXPECT_EQ(snapshot.spatialUpdate, nullptr);
    }
    const auto restarted = scene::ExtractRenderSceneGeometry(m_scene, 12);
    ASSERT_NE(restarted.spatialUpdate, nullptr);
    EXPECT_TRUE(restarted.spatialUpdate->fullReplacement);
    EXPECT_EQ(restarted.spatialUpdate->baseVersion, 0u);
    EXPECT_NE(restarted.spatialUpdate->version, warm.spatialUpdate->version);
}

TEST_F(RenderSceneExtractorMaterialTest, SharesSpatialJournalAcrossViewsAndPreservesItAfterNotification)
{
    const SpatialProducerEnvironment journal;
    auto& owner = StaticObject();
    (void)scene::ExtractRenderSceneGeometry(m_scene, Time::frameCount);
    (void)scene::ExtractRenderSceneGeometry(m_scene, Time::frameCount);
    scene::RenderFrameGeometryCache cache;
    const auto first = ExtractView({}, cache);
    const auto second = ExtractView({}, cache);
    ASSERT_NE(first->spatialUpdate, nullptr);
    EXPECT_EQ(first->spatialUpdate, second->spatialUpdate);
    EXPECT_EQ(first->spatialUpdate, cache.geometry->spatialUpdate);
    EXPECT_NE(first->snapshotSerial, second->snapshotSerial);
    EXPECT_TRUE(first->spatialUpdate->changedObjectOrdinals.empty());
    owner.GetComponent<scene::MeshRenderer>()->castShadows = false;
    scene::NotifyMeshRendererChanged(m_scene, owner.GetID());
    const auto notified = ExtractView({}, cache);
    ASSERT_NE(notified->spatialUpdate, nullptr);
    EXPECT_NE(first->spatialUpdate, notified->spatialUpdate);
    EXPECT_EQ(notified->spatialUpdate->baseVersion, first->spatialUpdate->version);
    EXPECT_FALSE(notified->spatialUpdate->fullReplacement);
    EXPECT_TRUE(notified->spatialUpdate->changedObjectOrdinals.empty());
    EXPECT_TRUE(first->Objects()[0].castShadows);
    EXPECT_FALSE(notified->Objects()[0].castShadows);
}

} /// @note namespace fbzz::tests
