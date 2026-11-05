/// @file    ShaderReloadTests.cpp
/// @brief   シェーダーの一括差し替え・失敗時保持と定数バッファ容量の契約を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-15
#include <Engine/Scene/ScriptComponent.hpp>
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/OwnedResources.hpp>
#include <Graphics/Renderer/BuiltinShader.hpp>
#include <Graphics/Renderer/ShaderDependencyTracker.hpp>
#include <Graphics/Passes/Geometry/ComputedSkinningDemand.hpp>
#include <limits>
#include <Engine/Scene/Systems/RenderParticleExtractor.hpp>
#include <Engine/Scene/Components/ParticleEmitter.hpp>
#include <Engine/Renderer/IBuffer.hpp>
#include <Engine/Renderer/IConstantBuffer.hpp>
#include <Engine/Renderer/IPipelineState.hpp>
#include <Engine/Renderer/IRenderTarget.hpp>
#include <Engine/Renderer/IShader.hpp>
#include <Engine/Renderer/IStructuredBuffer.hpp>
#include <Engine/Renderer/ITexture.hpp>
#include <Engine/Renderer/IRenderer.hpp>
#include <Engine/Renderer/Material.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <functional>
#include <utility>
#include "../../../Engine/src/Scene/Systems/RenderPasses/Geometry/GeometryPasses.hpp"
#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <Engine/Asset/MaterialParamBinding.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/ShaderCapabilities.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <chrono>
#include <filesystem>
#include <string_view>
#include <system_error>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/ScriptProxy/ScriptMaterialProxy.hpp>
#include <Engine/Scene/Systems/RenderSceneExtractor.hpp>
#include <Engine/Scene/Systems/RenderSystem.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Asset/Model.hpp>
#include <Engine/Asset/InstanceSetAsset.hpp>
#include <Engine/Asset/DataAssetRegistry.hpp>
#include <Engine/Scene/Components/InstancedMeshComponent.hpp>
#include <Engine/Renderer/Camera.hpp>
#include <Engine/Renderer/RenderSettings.hpp>
#include <cstring>
#include <array>
#include <cmath>
#include <Engine/Scene/Systems/RenderSystem.hpp>
#include <Engine/Scene/Managed/ManagedScriptAdapter.hpp>
#include <Engine/Scene/Managed/ManagedScriptRuntime.hpp>
#include <Engine/Scene/ScriptFactory.hpp>
#include <Engine/Scene/Components/UICanvas.hpp>
#include <Engine/Scene/Components/UIText.hpp>
#include <Engine/Scene/Components/UILayoutGroup.hpp>
#include <Engine/Core/Time.hpp>
#include "../../../Engine/src/Scene/Systems/MaterialDemandTrace.hpp"
#include <Engine/Scene/Components/ReflectionProbeComponent.hpp>
#include <Engine/Scene/Components/LightProbeVolumeComponent.hpp>
#include <Engine/Scene/Components/DecalComponent.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include "../../../Engine/src/Scene/Systems/RenderShadowPreparation.hpp"

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



namespace r = renderer;

class TestShader final : public r::IShader {
public:
    TestShader(std::string path, uint32_t size, const std::vector<r::ShaderVarDesc>& variables = {})
        : m_path(std::move(path)) { m_descriptor.cbufferSize = size; m_descriptor.vars = variables; }
    const std::string& GetPath() const override { return m_path; }
    const r::ShaderDescriptor& GetDescriptor() const override { return m_descriptor; }
    void SetVariables(const std::vector<r::ShaderVarDesc>& variables) { m_descriptor.vars = variables; }
private:
    std::string m_path;
    r::ShaderDescriptor m_descriptor;
};

class TestConstantBuffer final : public r::IConstantBuffer {
public:
    TestConstantBuffer(size_t size, uint32_t& updates, std::function<void(size_t)>& callback)
        : m_size((size + 255) & ~size_t(255)), m_updates(updates), m_callback(callback) {}
    size_t GetSize() const override { return m_size; }
    void Update(const void*, size_t size) override {
        EXPECT_LE(size, m_size);
        ++m_updates;
        auto callback = m_callback;
        if (callback) callback(size);
    }
private:
    size_t m_size;
    uint32_t& m_updates;
    std::function<void(size_t)>& m_callback;
};

class ExtractionBuffer final : public r::IBuffer {
public:
    ExtractionBuffer(size_t size, uint32_t stride) : m_size(size), m_stride(stride) {}
    void Update(const void*, size_t size) override { EXPECT_LE(size, m_size); }
    size_t GetSize() const override { return m_size; }
    uint32_t GetStride() const override { return m_stride; }
private:
    size_t m_size;
    uint32_t m_stride;
};

class TestPipeline final : public r::IPipelineState {
public:
    explicit TestPipeline(const r::PipelineStateDesc& desc) : m_desc(desc) {}
    const r::PipelineStateDesc& GetDesc() const override { return m_desc; }
private:
    r::PipelineStateDesc m_desc;
};

class ExtractionRenderTarget final : public r::IRenderTarget {
public:
    ExtractionRenderTarget(uint32_t width, uint32_t height, uint32_t colors)
        : m_width(width), m_height(height), m_colors(colors) {}
    uint32_t GetWidth() const override { return m_width; }
    uint32_t GetHeight() const override { return m_height; }
    uint32_t GetColorCount() const override { return m_colors; }
    void* GetNativeSRV(int = 0) const override { return nullptr; }
private:
    uint32_t m_width, m_height, m_colors;
};

class ExtractionTexture final : public r::ITexture {
public:
    ExtractionTexture(uint32_t width, uint32_t height) : m_width(width), m_height(height) {}
    uint32_t GetWidth() const override { return m_width; }
    uint32_t GetHeight() const override { return m_height; }
private:
    uint32_t m_width, m_height;
};
class ReloadRenderer final : public r::IRenderer {
public:
    int shaderCreates = 0;
    int failAt = -1;
    int prepareCalls = 0;
    uint32_t shaderSize = 16;
    std::vector<r::ShaderVarDesc> shaderVars;
    bool allowReload = true;
    std::function<void()> beforeReplace;
    std::string failedShaderPath;
    bool failConstantBuffers = false;
    bool allowGeometry = false;
    bool allowRenderTargets = false;
    uint32_t constantBufferUpdates = 0;
    std::function<void()> beforeConstantBufferCreate;
    std::function<void(size_t)> afterConstantBufferUpdate;
    TestShader* lastCreatedShader = nullptr;

    bool PrepareShaderReload() override {
        ++prepareCalls;
        if (beforeReplace) beforeReplace();
        return allowReload;
    }
    std::unique_ptr<r::IShader> CreateNativeShader(const std::string& path) override {
        if (++shaderCreates == failAt || path == failedShaderPath) return {};
        if (r::IsErrorShader(path)) return std::make_unique<TestShader>(path, 0);
        auto shader = std::make_unique<TestShader>(path, shaderSize, shaderVars);
        lastCreatedShader = shader.get();
        return shader;
    }
    std::unique_ptr<r::IConstantBuffer> CreateNativeConstantBuffer(size_t size) override {
        auto callback = std::exchange(beforeConstantBufferCreate, {});
        if (callback) callback();
        if (failConstantBuffers) return {};
        return std::make_unique<TestConstantBuffer>(size, constantBufferUpdates, afterConstantBufferUpdate);
    }
    void Shutdown() override {}
    void BeginFrame() override {}
    void EndFrame() override {}
    void Clear(const math::Vector4&) override {}
    void Submit(const r::DrawCall&, r::ResourceManager&) override {}
    void Dispatch(const r::ComputeCall&, r::ResourceManager&) override {}
    void Resize(uint32_t, uint32_t) override {}
    uint32_t GetWidth() const override { return 1; }
    uint32_t GetHeight() const override { return 1; }
    void SetRenderTarget(r::ResourceHandle<r::RenderTargetTag>, r::ResourceManager&) override {}
    void ClearDepth() override {}
    void SetViewport(uint32_t, uint32_t, uint32_t, uint32_t) override {}
    std::unique_ptr<r::IBuffer> CreateNativeVertexBuffer(const void*, size_t size, uint32_t stride) override {
        return allowGeometry ? std::make_unique<ExtractionBuffer>(size, stride) : nullptr;
    }
    std::unique_ptr<r::IBuffer> CreateNativeIndexBuffer(const void*, uint32_t count) override {
        return allowGeometry ? std::make_unique<ExtractionBuffer>(count * sizeof(uint32_t), 0) : nullptr;
    }
    std::unique_ptr<r::ITexture> CreateNativeTexture(const std::string&) override { return {}; }
    std::unique_ptr<r::ITexture> CreateNativeTextureFromData(const uint8_t*, uint32_t width, uint32_t height) override {
        return allowRenderTargets ? std::make_unique<ExtractionTexture>(width, height) : nullptr;
    }
    std::unique_ptr<r::ITexture> CreateNativeTexture3DFromData(const uint8_t*, uint32_t, uint32_t, uint32_t) override { return {}; }
    std::unique_ptr<r::ITexture> CreateNativeTextureFromRenderTarget(r::IRenderTarget&, uint32_t, r::RenderTargetTextureKind) override { return {}; }
    std::unique_ptr<r::IPipelineState> CreateNativePipelineState(const r::PipelineStateDesc& desc) override { return std::make_unique<TestPipeline>(desc); }
    std::unique_ptr<r::IRenderTarget> CreateNativeRenderTarget(uint32_t width, uint32_t height, const r::RenderTargetDesc& desc) override {
        return allowRenderTargets ? std::make_unique<ExtractionRenderTarget>(width, height, desc.colorCount) : nullptr;
    }
    std::unique_ptr<r::ITexture> CreateNativeComputeTexture(uint32_t, uint32_t) override { return {}; }
    std::unique_ptr<r::IStructuredBuffer> CreateNativeStructuredBuffer(const void*, uint32_t, uint32_t) override { return {}; }
    std::unique_ptr<r::IStructuredBuffer> CreateNativeRWStructuredBuffer(const void*, uint32_t, uint32_t) override { return {}; }
};

constexpr std::string_view kSurfaceShaderDeclaration =
    "version=1\nvertex='standard_surface_v1'\nsurface='metallic_roughness_v1'\n"
    "opacity='alpha_clip_v1'\nvariants='standard_surface_v1'\n";
constexpr std::string_view kSkinnedShaderDeclaration =
    "version=1\nvertex='standard_skinned_v1'\nsurface='metallic_roughness_v1'\n"
    "opacity='alpha_clip_v1'\nvariants='standard_skinned_v1'\n";
constexpr std::string_view kLambertShaderDeclaration =
    "version=1\nvertex='standard_surface_v1'\nsurface='lambert_v1'\n"
    "opacity='opaque_v1'\nvariants='standard_surface_v1'\n";

std::vector<r::ShaderVarDesc> StandardPbrVariables()
{
    return {
        {"albedo", 0, 16, 1, 4, r::ShaderVarClass::Vector, r::ShaderVarType::Float},
        {"metallic", 16, 4, 1, 1, r::ShaderVarClass::Scalar, r::ShaderVarType::Float},
        {"roughness", 20, 4, 1, 1, r::ShaderVarClass::Scalar, r::ShaderVarType::Float},
        {"uvTiling", 24, 8, 1, 2, r::ShaderVarClass::Vector, r::ShaderVarType::Float},
        {"uvOffset", 32, 8, 1, 2, r::ShaderVarClass::Vector, r::ShaderVarType::Float},
        {"alphaCutoff", 64, 4, 1, 1, r::ShaderVarClass::Scalar, r::ShaderVarType::Float}
    };
}

void ExpectStandardShaderCapabilities(const r::ShaderCapabilities& capabilities, bool skinned)
{
    EXPECT_EQ(capabilities.vertex, skinned ? r::ShaderVertexContract::STANDARD_SKINNED
                                          : r::ShaderVertexContract::STANDARD_SURFACE);
    EXPECT_EQ(capabilities.surface, r::ShaderSurfaceContract::STANDARD_PBR);
    EXPECT_EQ(capabilities.opacity, r::ShaderOpacityContract::ALPHA_CLIP);
    EXPECT_EQ(capabilities.variants, skinned ? r::ShaderVariantSet::STANDARD_SKINNED
                                            : r::ShaderVariantSet::STANDARD_SURFACE);
    EXPECT_EQ(capabilities.preview, skinned ? r::ShaderPreviewKind::SKINNED
                                           : r::ShaderPreviewKind::SURFACE);
    EXPECT_TRUE(capabilities.SupportsGBuffer());
    EXPECT_EQ(capabilities.SupportsSkinning(), skinned);
}

void ExpectUnknownShaderCapabilities(const r::ShaderCapabilities& capabilities)
{
    EXPECT_EQ(capabilities.vertex, r::ShaderVertexContract::UNKNOWN);
    EXPECT_EQ(capabilities.surface, r::ShaderSurfaceContract::UNKNOWN);
    EXPECT_EQ(capabilities.opacity, r::ShaderOpacityContract::UNKNOWN);
    EXPECT_EQ(capabilities.variants, r::ShaderVariantSet::NONE);
    EXPECT_EQ(capabilities.preview, r::ShaderPreviewKind::UNKNOWN);
    EXPECT_FALSE(capabilities.SupportsGBuffer());
    EXPECT_FALSE(capabilities.SupportsSkinning());
}

} /// @note namespace

class ShaderReloadTest : public testkit::Fixture {};

class ShaderDependencyMacroTest : public testkit::Fixture {
protected:
    void SetUp() override
    {
        Fixture::SetUp();
        ASSERT_TRUE(m_temp.IsValid());
        m_time = std::filesystem::file_time_type::clock::now();
    }
    void Write(const std::string& name, const std::string& source, bool newer = false)
    {
        const auto path = m_temp.File(name);
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        ASSERT_FALSE(error);
        ASSERT_TRUE(util::FileSystem::WriteText(path, source));
        std::filesystem::last_write_time(path, m_time + std::chrono::seconds(newer ? 2 : -2), error);
        ASSERT_FALSE(error);
    }
    bool Stale()
    {
        std::unordered_set<std::string> visited;
        return renderer::shader_dependency::HasNewerDependency(m_temp.File("Root.hlsl"),
            m_temp.Path(), m_time, visited);
    }
    testkit::TempDir m_temp{"shader-dependency-macros"};
    std::filesystem::file_time_type m_time;
};

TEST_F(ShaderDependencyMacroTest, ProjectAppearanceObjectMacroReloadsWithoutChangingTheWrapper)
{
    Write("Root.hlsl", "#define APPEARANCE \"Appearance.hlsli\"\n#include APPEARANCE\n");
    Write("Appearance.hlsli", "float3 Color() { return 1; }\n");
    EXPECT_FALSE(Stale());
    Write("Appearance.hlsli", "float3 Color() { return 0; }\n", true);
    EXPECT_TRUE(Stale());
}

TEST_F(ShaderDependencyMacroTest, NestedIncludesShareObjectMacroDefinitionsAndResolveShaderRootPaths)
{
    Write("Root.hlsl", "#include \"Config.hlsli\"\n#include \"Fiber/Surface.hlsli\"\n");
    Write("Config.hlsli", "#define APPEARANCE_ALIAS APPEARANCE\n#define APPEARANCE \"Examples/Look.hlsli\"\n");
    Write("Fiber/Surface.hlsli", "#ifdef APPEARANCE\n#include APPEARANCE_ALIAS\n#endif\n");
    Write("Examples/Look.hlsli", "float3 Color() { return 1; }\n");
    EXPECT_FALSE(Stale());
    Write("Examples/Look.hlsli", "float3 Color() { return 0; }\n", true);
    EXPECT_TRUE(Stale());
}

TEST_F(ShaderDependencyMacroTest, OptionalUndefinedAppearancePreservesBuiltinShaderCaching)
{
    Write("Root.hlsl", "#include \"Surface.hlsli\"\n");
    Write("Surface.hlsli", "#ifdef APPEARANCE\n#include APPEARANCE\n#else\n#include \"Builtin.hlsli\"\n#endif\n");
    Write("Builtin.hlsli", "float3 Color() { return 1; }\n");
    EXPECT_FALSE(Stale());
    Write("Builtin.hlsli", "float3 Color() { return 0; }\n", true);
    EXPECT_TRUE(Stale());
}

TEST_F(ShaderDependencyMacroTest, LiteralIncludesBehindCompilerDefinedGuardsRetainConservativeTracking)
{
    Write("Root.hlsl", "#ifdef COMPILER_PLATFORM\n#include \"Platform.hlsli\"\n#endif\n");
    Write("Platform.hlsli", "float platform;\n");
    EXPECT_FALSE(Stale());
    Write("Platform.hlsli", "float platformChanged;\n", true);
    EXPECT_TRUE(Stale());
}

TEST_F(ShaderDependencyMacroTest, UnknownAndCyclicIncludeMacrosRequireRecompilation)
{
    Write("Root.hlsl", "#include UNKNOWN_APPEARANCE\n");
    EXPECT_TRUE(Stale());
    Write("Root.hlsl", "#define FIRST SECOND\n#define SECOND FIRST\n#include FIRST\n");
    EXPECT_TRUE(Stale());
    Write("Root.hlsl", "#define PICK(x) x\n#include PICK(\"Look.hlsli\")\n");
    EXPECT_TRUE(Stale());
}

TEST_F(ShaderDependencyMacroTest, UnknownConditionalBranchesKeepEveryPossibleAppearanceDependency)
{
    Write("Root.hlsl", "#if USER_SETTING\n#define LOOK \"First.hlsli\"\n#else\n#define LOOK \"Second.hlsli\"\n#endif\n#include LOOK\n");
    Write("First.hlsli", "float first;\n");
    Write("Second.hlsli", "float second;\n");
    EXPECT_FALSE(Stale());
    Write("First.hlsli", "float firstChanged;\n", true);
    EXPECT_TRUE(Stale());
    Write("First.hlsli", "float first;\n");
    Write("Second.hlsli", "float secondChanged;\n", true);
    EXPECT_TRUE(Stale());
}

TEST_F(ShaderDependencyMacroTest, RepeatedIncludeRescansAfterMacroChangesAndLiteralCyclesTerminate)
{
    Write("Root.hlsl", "#define LOOK \"First.hlsli\"\n#include \"Surface.hlsli\"\n#undef LOOK\n#define LOOK \"Second.hlsli\"\n#include \"Surface.hlsli\"\n");
    Write("Surface.hlsli", "#include LOOK\n");
    Write("First.hlsli", "float first;\n");
    Write("Second.hlsli", "float second;\n");
    EXPECT_FALSE(Stale());
    Write("Second.hlsli", "float secondChanged;\n", true);
    EXPECT_TRUE(Stale());
    Write("Root.hlsl", "#include \"Cycle.hlsli\"\n");
    Write("Cycle.hlsli", "#include \"Root.hlsl\"\n");
    EXPECT_FALSE(Stale());
    Write("Cycle.hlsli", "#include \"Missing.hlsli\"\n");
    EXPECT_TRUE(Stale());
}



TEST_F(ShaderReloadTest, ShadowBoundsKeepSceneOrderAndEveryRendererKindOnADualOwner)
{
    scene::Scene testScene;
    auto& mixed = testScene.CreateGameObject("Mixed renderer owner registered last");
    auto& second = testScene.CreateGameObject("Second static owner");
    for (int i = 0; i < 24; ++i) testScene.CreateGameObject("Bone only");
    r::Mesh firstMesh;
    firstMesh.boundsRadius = 1;
    r::Mesh secondMesh;
    secondMesh.boundsRadius = 2;
    second.AddComponent<scene::MeshRenderer>().mesh = &secondMesh;
    second.transform.position = second.transform.worldPosition = {0, 0, -15};
    mixed.AddComponent<scene::MeshRenderer>().mesh = &firstMesh;
    asset::Model model;
    auto skin = std::make_unique<r::Mesh>();
    skin->boundsRadius = 1;
    skin->boundsCenter = {4, 0, 0};
    model.meshes.push_back(std::move(skin));
    mixed.AddComponent<scene::SkinnedMeshRenderer>().model = &model;
    auto& terrain = mixed.AddComponent<scene::TerrainComponent>();
    terrain.columns = terrain.rows = 2;
    terrain.cellSize = 10;
    terrain.heightData.assign(4, 0);
    auto& inactive = testScene.CreateGameObject("Inactive distant renderer");
    inactive.AddComponent<scene::MeshRenderer>().mesh = &firstMesh;
    inactive.transform.worldPosition = {10000, 0, 0};
    inactive.SetActive(false);
    const auto bounds = scene::shadow_preparation_detail::ComputeSceneBounds(testScene, UINT32_MAX);
    ASSERT_TRUE(bounds.valid);
    const math::Vector3 expectedCenter{2.9578432f, 0, -2.4815008f};
    EXPECT_VEC3_NEAR(bounds.center, expectedCenter, 0.0001f);
    EXPECT_NEAR(bounds.radius, 14.8631901f, 0.0001f);
    mixed.RemoveComponent<scene::SkinnedMeshRenderer>();
    mixed.RemoveComponent<scene::TerrainComponent>();
    second.RemoveComponent<scene::MeshRenderer>();
    mixed.transform.position = mixed.transform.worldPosition = {20, 0, 0};
    const auto current = scene::shadow_preparation_detail::ComputeSceneBounds(testScene, UINT32_MAX);
    EXPECT_VEC3_NEAR(current.center, mixed.transform.worldPosition, testkit::kTolerance);
    EXPECT_FLOAT_EQ(current.radius, 1);
    testScene.Clear();
}

TEST_F(ShaderReloadTest, ShadowTerrainBoundsObserveDirectHeightAndScaleChangesWithoutARevisionBump)
{
    scene::Scene testScene;
    auto& owner = testScene.CreateGameObject("Current terrain heights");
    auto& terrain = owner.AddComponent<scene::TerrainComponent>();
    terrain.columns = terrain.rows = 2;
    terrain.cellSize = 2;
    terrain.maxHeight = 10;
    terrain.heightData = {-1, 0.25f, 0.5f, 1};
    owner.transform.position = owner.transform.worldPosition = {3, 4, 5};
    owner.transform.scale = owner.transform.worldScale = {-2, 3, 4};
    const auto first = scene::shadow_preparation_detail::ComputeSceneBounds(testScene, UINT32_MAX);
    const math::Vector3 firstCenter{1, 4, 9};
    EXPECT_VEC3_NEAR(first.center, firstCenter, testkit::kTolerance);
    EXPECT_NEAR(first.radius, std::sqrt(102.0f) * 4, testkit::kTolerance);
    const auto revision = terrain.surfaceRevision;
    terrain.heightData.assign(4, 0.5f);
    const auto second = scene::shadow_preparation_detail::ComputeSceneBounds(testScene, UINT32_MAX);
    const math::Vector3 secondCenter{1, 19, 9};
    EXPECT_EQ(terrain.surfaceRevision, revision);
    EXPECT_VEC3_NEAR(second.center, secondCenter, testkit::kTolerance);
    EXPECT_NEAR(second.radius, std::sqrt(2.0f) * 4, testkit::kTolerance);
}

class ShaderCapabilityReloadTest : public ShaderReloadTest {
protected:
    void SetUp() override
    {
        ShaderReloadTest::SetUp();
        ASSERT_TRUE(m_temp.IsValid());
    }

    std::string ShaderPath(const std::string& name) const { return m_temp.File(name).generic_string(); }

    bool WriteShader(const std::string& name, std::string_view declaration)
    {
        const std::string path = ShaderPath(name);
        const auto metaPath = std::filesystem::path(path + ".meta");
        std::error_code error;
        const bool hadMeta = std::filesystem::exists(metaPath, error);
        if (error) return false;
        const auto previousTime = hadMeta ? std::filesystem::last_write_time(metaPath, error)
                                          : std::filesystem::file_time_type{};
        if (error) return false;
        if (!util::FileSystem::WriteText(path, "float4 PSMain() : SV_Target { return 1; }\n")) return false;
        uint64_t hash = 14695981039346656037ull;
        for (const unsigned char character : name) { hash ^= character; hash *= 1099511628211ull; }
        const std::string suffix = std::to_string(hash);
        const std::string guid = std::string(32 - suffix.size(), '0') + suffix;
        const std::string meta = "[meta]\nguid='" + guid + "'\n[shader]\n"
                               + std::string(declaration);
        if (!util::FileSystem::WriteText(path + ".meta", meta)) return false;
        if (hadMeta) {
            /// @note sleep や現在時刻に依存せず、再宣言を metadata cache に反映させる。
            std::filesystem::last_write_time(metaPath, previousTime + std::chrono::seconds(2), error);
        }
        return !error;
    }

    testkit::TempDir m_temp{"shader-capability-reload"};
};

TEST_F(ShaderCapabilityReloadTest, FailedReloadsFreezeCapabilitiesTogetherWithEveryLoadedShader)
{
    ASSERT_TRUE(WriteShader("First.hlsl", kSurfaceShaderDeclaration));
    ASSERT_TRUE(WriteShader("Second.hlsl", kSurfaceShaderDeclaration));
    ReloadRenderer backend;
    backend.shaderSize = 96;
    backend.shaderVars = StandardPbrVariables();
    r::ResourceManager resources(backend);
    const std::string firstPath = ShaderPath("First.hlsl");
    const std::string secondPath = ShaderPath("Second.hlsl");
    const auto first = resources.LoadShader(firstPath);
    const auto second = resources.LoadShader(secondPath);
    ASSERT_NE(resources.Get(first), nullptr);
    ASSERT_NE(resources.Get(second), nullptr);
    const auto* firstShader = resources.Get(first);
    const auto* secondShader = resources.Get(second);
    const auto version = resources.GetShaderVersion();
    ExpectStandardShaderCapabilities(resources.GetShaderCapabilities(first), false);
    ExpectStandardShaderCapabilities(resources.GetShaderCapabilities(second), false);
    ASSERT_TRUE(WriteShader("First.hlsl", kSkinnedShaderDeclaration));
    ASSERT_TRUE(WriteShader("Second.hlsl", kSkinnedShaderDeclaration));
    ExpectStandardShaderCapabilities(asset::ResolveShaderCapabilities(firstPath), true);
    ExpectStandardShaderCapabilities(asset::ResolveShaderCapabilities(secondPath), true);
    backend.shaderSize = 112;

    const auto expectFrozen = [&] {
        EXPECT_EQ(resources.Get(first), firstShader);
        EXPECT_EQ(resources.Get(second), secondShader);
        EXPECT_EQ(resources.GetShaderVersion(), version);
        ExpectStandardShaderCapabilities(resources.GetShaderCapabilities(first), false);
        ExpectStandardShaderCapabilities(resources.GetShaderCapabilities(second), false);
    };
    backend.failAt = backend.shaderCreates + 1;
    EXPECT_EQ(resources.ReloadShader(firstPath), first);
    expectFrozen();
    EXPECT_EQ(backend.prepareCalls, 0);

    backend.failAt = backend.shaderCreates + 2;
    EXPECT_FALSE(resources.ReloadAllShaders());
    expectFrozen();
    EXPECT_EQ(backend.prepareCalls, 0);

    backend.failAt = -1;
    backend.allowReload = false;
    EXPECT_FALSE(resources.ReloadAllShaders());
    expectFrozen();
    EXPECT_EQ(resources.ReloadShader(firstPath), first);
    expectFrozen();
    EXPECT_EQ(backend.prepareCalls, 2);
}

TEST_F(ShaderCapabilityReloadTest, SuccessfulReloadsCommitCapabilitiesAndReleasedHandlesCannotKeepThem)
{
    ASSERT_TRUE(WriteShader("First.hlsl", kSurfaceShaderDeclaration));
    ASSERT_TRUE(WriteShader("Second.hlsl", kSurfaceShaderDeclaration));
    ReloadRenderer backend;
    backend.shaderSize = 96;
    backend.shaderVars = StandardPbrVariables();
    r::ResourceManager resources(backend);
    const std::string firstPath = ShaderPath("First.hlsl");
    const std::string secondPath = ShaderPath("Second.hlsl");
    const auto first = resources.LoadShader(firstPath);
    const auto second = resources.LoadShader(secondPath);
    ASSERT_NE(resources.Get(first), nullptr);
    ASSERT_NE(resources.Get(second), nullptr);
    const auto* firstShader = resources.Get(first);
    const auto* secondShader = resources.Get(second);
    const auto version = resources.GetShaderVersion();
    ASSERT_TRUE(WriteShader("First.hlsl", kSkinnedShaderDeclaration));
    ASSERT_TRUE(WriteShader("Second.hlsl", kSkinnedShaderDeclaration));
    backend.shaderSize = 112;
    backend.beforeReplace = [&] {
        EXPECT_EQ(resources.Get(first), firstShader);
        EXPECT_EQ(resources.Get(second), secondShader);
        ExpectStandardShaderCapabilities(resources.GetShaderCapabilities(first), false);
        ExpectStandardShaderCapabilities(resources.GetShaderCapabilities(second), false);
    };

    ASSERT_TRUE(resources.ReloadAllShaders());
    EXPECT_EQ(resources.GetShaderVersion(), version + 1);
    EXPECT_EQ(backend.prepareCalls, 1);
    EXPECT_EQ(resources.LoadShader(firstPath), first);
    EXPECT_EQ(resources.LoadShader(secondPath), second);
    ASSERT_NE(resources.Get(first), nullptr);
    ASSERT_NE(resources.Get(second), nullptr);
    EXPECT_EQ(resources.Get(first)->GetDescriptor().cbufferSize, 112u);
    EXPECT_EQ(resources.Get(second)->GetDescriptor().cbufferSize, 112u);
    ExpectStandardShaderCapabilities(resources.GetShaderCapabilities(first), true);
    ExpectStandardShaderCapabilities(resources.GetShaderCapabilities(second), true);

    ASSERT_TRUE(WriteShader("First.hlsl", kLambertShaderDeclaration));
    backend.shaderSize = 16;
    backend.shaderVars.clear();
    backend.beforeReplace = [&] {
        ExpectStandardShaderCapabilities(resources.GetShaderCapabilities(first), true);
        ExpectStandardShaderCapabilities(resources.GetShaderCapabilities(second), true);
    };
    ASSERT_EQ(resources.ReloadShader(firstPath), first);
    const auto current = resources.GetShaderCapabilities(first);
    EXPECT_EQ(current.surface, r::ShaderSurfaceContract::LAMBERT);
    EXPECT_TRUE(current.HasOpaqueOutput());
    EXPECT_FALSE(current.SupportsGBuffer());
    ExpectStandardShaderCapabilities(resources.GetShaderCapabilities(second), true);
    EXPECT_EQ(resources.GetShaderVersion(), version + 2);
    EXPECT_EQ(backend.prepareCalls, 2);

    resources.Release(first);
    EXPECT_EQ(resources.Get(first), nullptr);
    ExpectUnknownShaderCapabilities(resources.GetShaderCapabilities(first));
    const auto replacement = resources.LoadShader(firstPath);
    ASSERT_NE(resources.Get(replacement), nullptr);
    EXPECT_EQ(replacement.id, first.id);
    EXPECT_NE(replacement.gen, first.gen);
    EXPECT_EQ(resources.GetShaderCapabilities(replacement).surface, r::ShaderSurfaceContract::LAMBERT);
    ExpectUnknownShaderCapabilities(resources.GetShaderCapabilities(first));
    resources.Reset();
    ExpectUnknownShaderCapabilities(resources.GetShaderCapabilities(replacement));
    ExpectUnknownShaderCapabilities(resources.GetShaderCapabilities(second));
}

TEST_F(ShaderCapabilityReloadTest, StandardPbrDeclarationRequiresCompatibleWritableLoadedConstants)
{
    ReloadRenderer backend;
    backend.shaderSize = 96;
    backend.shaderVars = StandardPbrVariables();
    r::ResourceManager resources(backend);
    ASSERT_TRUE(WriteShader("Valid.hlsl", kSurfaceShaderDeclaration));
    const auto valid = resources.LoadShader(ShaderPath("Valid.hlsl"));
    ASSERT_NE(resources.Get(valid), nullptr);
    ExpectStandardShaderCapabilities(resources.GetShaderCapabilities(valid), false);

    const std::vector<std::pair<const char*, std::function<void()>>> invalidLayouts = {
        {"missing_alpha", [&] { backend.shaderVars.pop_back(); }},
        {"wrong_type", [&] { backend.shaderVars[1].varType = r::ShaderVarType::Int; }},
        {"read_only", [&] { backend.shaderVars[0].unsupportedReason = "unsupported layout"; }},
        {"wrong_vector_width", [&] { backend.shaderVars[3].columns = 1; }},
        {"matrix_shape", [&] { backend.shaderVars[0].rows = 2; }},
        {"matrix_class", [&] { backend.shaderVars[0].varClass = r::ShaderVarClass::Matrix; }},
        {"wrong_size", [&] { backend.shaderVars[0].size = 4; }},
        {"array_shape", [&] { backend.shaderVars[0].elements = 1; }},
        {"offset_out_of_bounds", [&] { backend.shaderVars.back().offset = 97; }},
        {"value_out_of_bounds", [&] { backend.shaderVars.back().offset = 95; }}
    };
    for (const auto& [name, invalidate] : invalidLayouts) {
        SCOPED_TRACE(name);
        backend.shaderVars = StandardPbrVariables();
        invalidate();
        const std::string file = std::string(name) + ".hlsl";
        ASSERT_TRUE(WriteShader(file, kSurfaceShaderDeclaration));
        ExpectStandardShaderCapabilities(asset::ResolveShaderCapabilities(ShaderPath(file)), false);
        const auto shader = resources.LoadShader(ShaderPath(file));
        ASSERT_NE(resources.Get(shader), nullptr);
        ExpectUnknownShaderCapabilities(resources.GetShaderCapabilities(shader));
        ExpectStandardShaderCapabilities(resources.GetShaderCapabilities(valid), false);
    }

    backend.shaderVars = StandardPbrVariables();
    backend.shaderVars.pop_back();
    EXPECT_EQ(resources.ReloadShader(ShaderPath("Valid.hlsl")), valid);
    ASSERT_NE(resources.Get(valid), nullptr);
    ExpectUnknownShaderCapabilities(resources.GetShaderCapabilities(valid));
}

TEST_F(ShaderReloadTest, WireframePreservesMaterialSidednessBlendAndDepthBias)
{
    ReloadRenderer backend;
    r::ResourceManager resources(backend);
    scene::MaterialSlot slot;
    EditMaterialAuthoring(slot, [&](auto& values) { values.hasDoubleSidedOverride = true; });
    EditMaterialAuthoring(slot, [&](auto& values) { values.doubleSidedOverride = true; });
    for (const auto blend : {r::BlendMode::OPAQUE_BLEND, r::BlendMode::ALPHA_BLEND, r::BlendMode::ADDITIVE}) {
        const auto solid = scene::GetOrCreateMaterialPSO(resources, blend, true, 3, 1.5f);
        const auto wire = scene::GetOrCreateMaterialPSO(resources, blend, true, 3, 1.5f, true);
        ASSERT_NE(resources.Get(solid), nullptr);
        ASSERT_NE(resources.Get(wire), nullptr);
        EXPECT_NE(solid, wire);
        const auto& before = resources.Get(solid)->GetDesc();
        const auto& after = resources.Get(wire)->GetDesc();
        EXPECT_EQ(before.rasterizer, r::RasterizerMode::SOLID_NOCULL);
        EXPECT_EQ(after.rasterizer, r::RasterizerMode::WIREFRAME_NOCULL);
        EXPECT_EQ(after.blend, before.blend);
        EXPECT_EQ(after.depth, before.depth);
        EXPECT_EQ(after.depthBias, before.depthBias);
        EXPECT_FLOAT_EQ(after.depthBiasSlope, before.depthBiasSlope);
        EXPECT_EQ(solid, scene::GetOrCreateMaterialPSO(resources, blend, true, 3, 1.5f));
    }
    auto wire = scene::GetOrCreateMaterialPSO(resources, slot, true);
    ASSERT_NE(resources.Get(wire), nullptr);
    EXPECT_EQ(resources.Get(wire)->GetDesc().rasterizer, r::RasterizerMode::WIREFRAME_NOCULL);
    EditMaterialAuthoring(slot, [&](auto& values) { values.doubleSidedOverride = false; });
    wire = scene::GetOrCreateMaterialPSO(resources, slot, true);
    ASSERT_NE(resources.Get(wire), nullptr);
    EXPECT_EQ(resources.Get(wire)->GetDesc().rasterizer, r::RasterizerMode::WIREFRAME);
    resources.Reset();
    wire = scene::GetOrCreateMaterialPSO(resources, slot, true);
    ASSERT_NE(resources.Get(wire), nullptr);
    EXPECT_EQ(resources.Get(wire)->GetDesc().rasterizer, r::RasterizerMode::WIREFRAME);
}

TEST_F(ShaderReloadTest, FailedBatchKeepsEveryOldShaderAndVersion)
{
    ReloadRenderer backend;
    r::ResourceManager resources(backend);
    const auto first = resources.LoadShader("first.hlsl");
    const auto second = resources.LoadShader("second.hlsl");
    const auto version = resources.GetShaderVersion();
    backend.shaderSize = 32;
    backend.failAt = backend.shaderCreates + 2;

    EXPECT_FALSE(resources.ReloadAllShaders());

    ASSERT_NE(resources.Get(first), nullptr);
    ASSERT_NE(resources.Get(second), nullptr);
    EXPECT_EQ(resources.Get(first)->GetDescriptor().cbufferSize, 16u);
    EXPECT_EQ(resources.Get(second)->GetDescriptor().cbufferSize, 16u);
    EXPECT_EQ(resources.GetShaderVersion(), version);
    EXPECT_EQ(backend.prepareCalls, 0);
}

TEST_F(ShaderReloadTest, SuccessfulBatchSynchronizesBeforeReplacingAndKeepsHandles)
{
    ReloadRenderer backend;
    r::ResourceManager resources(backend);
    const auto first = resources.LoadShader("first.hlsl");
    const auto second = resources.LoadShader("second.hlsl");
    const auto version = resources.GetShaderVersion();
    backend.shaderSize = 32;
    backend.beforeReplace = [&] {
        EXPECT_EQ(resources.Get(first)->GetDescriptor().cbufferSize, 16u);
        EXPECT_EQ(resources.Get(second)->GetDescriptor().cbufferSize, 16u);
    };

    ASSERT_TRUE(resources.ReloadAllShaders());

    EXPECT_EQ(backend.prepareCalls, 1);
    EXPECT_EQ(resources.LoadShader("first.hlsl"), first);
    EXPECT_EQ(resources.Get(first)->GetDescriptor().cbufferSize, 32u);
    EXPECT_EQ(resources.Get(second)->GetDescriptor().cbufferSize, 32u);
    EXPECT_EQ(resources.GetShaderVersion(), version + 1);
}

TEST_F(ShaderReloadTest, RendererRejectionKeepsPreviousState)
{
    ReloadRenderer backend;
    r::ResourceManager resources(backend);
    const auto shader = resources.LoadShader("first.hlsl");
    const auto version = resources.GetShaderVersion();
    backend.shaderSize = 32;
    backend.allowReload = false;

    EXPECT_FALSE(resources.ReloadAllShaders());
    EXPECT_EQ(resources.ReloadShader("first.hlsl"), shader);

    EXPECT_EQ(resources.Get(shader)->GetDescriptor().cbufferSize, 16u);
    EXPECT_EQ(resources.GetShaderVersion(), version);
}

TEST_F(ShaderReloadTest, SingleReloadNormalizesPathSeparators)
{
    ReloadRenderer backend;
    r::ResourceManager resources(backend);
    const auto shader = resources.LoadShader("UI/Text.hlsl");
    backend.shaderSize = 32;

    EXPECT_EQ(resources.ReloadShader("UI\\Text.hlsl"), shader);

    EXPECT_EQ(backend.prepareCalls, 1);
    EXPECT_EQ(resources.Get(shader)->GetDescriptor().cbufferSize, 32u);
}

TEST_F(ShaderReloadTest, ReleasedShaderIsLoadedWithANewGenerationAndStaleReleaseKeepsCurrentCache)
{
    ReloadRenderer backend;
    r::ResourceManager resources(backend);
    const auto released = resources.LoadShader("UI/Text.hlsl");
    const auto other = resources.LoadShader("other.hlsl");
    resources.Release(released);
    EXPECT_EQ(resources.Get(released), nullptr);

    const auto reusedSlot = resources.LoadShader("reused.hlsl");
    ASSERT_NE(resources.Get(reusedSlot), nullptr);
    EXPECT_EQ(reusedSlot.id, released.id);
    EXPECT_NE(reusedSlot.gen, released.gen);
    const auto current = resources.LoadShader("UI\\Text.hlsl");
    ASSERT_NE(resources.Get(current), nullptr);
    EXPECT_NE(current, released);
    EXPECT_NE(current, reusedSlot);
    EXPECT_EQ(resources.LoadShader("UI/Text.hlsl"), current);
    EXPECT_EQ(backend.shaderCreates, 4);

    resources.Release(released);
    EXPECT_EQ(resources.LoadShader("UI\\Text.hlsl"), current);
    EXPECT_EQ(resources.LoadShader("reused.hlsl"), reusedSlot);
    EXPECT_EQ(resources.LoadShader("other.hlsl"), other);
    EXPECT_EQ(backend.shaderCreates, 4);
    EXPECT_EQ(backend.prepareCalls, 0);
}

TEST_F(ShaderReloadTest, ReleasedShaderIsExcludedFromBatchReloadAndSingleReloadCreatesALiveHandle)
{
    ReloadRenderer backend;
    r::ResourceManager resources(backend);
    const auto released = resources.LoadShader("UI/Text.hlsl");
    const auto other = resources.LoadShader("other.hlsl");
    const auto version = resources.GetShaderVersion();
    resources.Release(released);
    backend.shaderSize = 32;

    ASSERT_TRUE(resources.ReloadAllShaders());
    EXPECT_EQ(backend.shaderCreates, 3);
    EXPECT_EQ(backend.prepareCalls, 1);
    EXPECT_EQ(resources.Get(released), nullptr);
    ASSERT_NE(resources.Get(other), nullptr);
    EXPECT_EQ(resources.Get(other)->GetDescriptor().cbufferSize, 32u);
    EXPECT_EQ(resources.LoadShader("other.hlsl"), other);
    EXPECT_EQ(resources.GetShaderVersion(), version + 1);

    const auto current = resources.ReloadShader("UI\\Text.hlsl");
    ASSERT_NE(resources.Get(current), nullptr);
    EXPECT_NE(current, released);
    EXPECT_EQ(resources.Get(current)->GetDescriptor().cbufferSize, 32u);
    EXPECT_EQ(resources.LoadShader("UI/Text.hlsl"), current);
    EXPECT_EQ(backend.shaderCreates, 4);
    EXPECT_EQ(backend.prepareCalls, 1);
    EXPECT_EQ(resources.GetShaderVersion(), version + 1);
}

TEST_F(ShaderReloadTest, MaterialGrowsGpuBufferAfterCpuLayoutHasAlreadyChanged)
{
    ReloadRenderer backend;
    r::ResourceManager resources(backend);
    r::Material material;
    r::ShaderDescriptor descriptor;
    descriptor.cbufferSize = 16;
    material.paramData.resize(16);
    material.Upload(resources, descriptor);
    const auto initial = material.paramsBuffer;
    material.Upload(resources, descriptor);
    EXPECT_EQ(material.paramsBuffer, initial);
    material.Init(resources, 32);
    EXPECT_EQ(material.paramsBuffer, initial);
    EXPECT_EQ(material.paramData.size(), 32u);

    descriptor.cbufferSize = 512;
    material.paramData.resize(512);
    material.Upload(resources, descriptor);

    ASSERT_NE(resources.Get(material.paramsBuffer), nullptr);
    EXPECT_GE(resources.Get(material.paramsBuffer)->GetSize(), 512u);
    EXPECT_EQ(resources.Get(initial), nullptr);
}


class MaterialScriptIntegrationTest : public testkit::Fixture {
protected:
    void SetUp() override
    {
        Fixture::SetUp();
        m_backend.shaderVars = {
            {"mode", 0, 4, 1, 1, r::ShaderVarClass::Scalar, r::ShaderVarType::Int},
            {"mask", 4, 4, 1, 1, r::ShaderVarClass::Scalar, r::ShaderVarType::UInt},
            {"enabled", 8, 4, 1, 1, r::ShaderVarClass::Scalar, r::ShaderVarType::Bool},
            {"uv", 16, 8, 1, 2, r::ShaderVarClass::Vector, r::ShaderVarType::Float},
            {"matrix", 32, 64, 4, 4, r::ShaderVarClass::Matrix, r::ShaderVarType::Float}
        };
        m_backend.shaderSize = 96;
        m_resources = std::make_unique<r::ResourceManager>(m_backend);
        m_scene.SetResourceManager(m_resources.get());
        asset::AssetManager::Init(*m_resources, m_temp.Path().generic_string() + "/");
        asset::MaterialAssetAuthoring material;
        material.shaderPath = "typed.hlsl";
        const auto path = m_temp.File("typed.mat").generic_string();
        ASSERT_TRUE(asset::SaveMaterialAssetToFile(path, material));
        auto& object = m_scene.CreateGameObject("Typed material");
        m_material = &object.AddComponent<scene::MaterialComponent>();
        m_material->SetMaterialPath(path);
        m_script.SetContext(&m_scene, &object);
        m_proxy.script = &m_script;
    }
    void TearDown() override
    {
        asset::AssetManager::UnloadAll();
        m_resources.reset();
        Fixture::TearDown();
    }
    testkit::TempDir m_temp{"material-script"};
    ReloadRenderer m_backend;
    std::unique_ptr<r::ResourceManager> m_resources;
    scene::Scene m_scene;
    scene::Script m_script;
    scene::ScriptMaterialProxy m_proxy;
    scene::MaterialComponent* m_material = nullptr;
};

TEST_F(MaterialScriptIntegrationTest, MissingAndUnassignedMaterialsUseEmbeddedFallbackWithoutChangingReferences)
{
    const auto fallback = m_resources->GetErrorMaterial(false);
    ASSERT_NE(fallback, nullptr);
    EXPECT_EQ(fallback->shaderPath, r::ERROR_SURFACE_SHADER);
    EXPECT_TRUE(fallback->textures.empty());
    EXPECT_FALSE(fallback->paramsBuffer.IsValid());
    for (const std::string path : {std::string{}, m_temp.File("missing.mat").generic_string()}) {
        m_material->SetMaterialPath(path);
        EditMaterialAuthoring(*m_material, [&](auto& values) { values.paramOverrides["albedo"] = {0, 0, 0, 0}; });
        EXPECT_EQ(scene::SyncMaterial(*m_material, *m_resources), fallback);
        EXPECT_EQ(m_material->GetMaterialPath(), path);
        EXPECT_FALSE(m_material->GetMaterialAsset().IsValid());
        EXPECT_EQ(m_material->Authoring().paramOverrides.at("albedo").at(3), 0);
    }
    m_material->enabled = false;
    EXPECT_EQ(scene::SyncMaterial(*m_material, *m_resources), nullptr);
    m_material->enabled = true;
    m_material->visible = false;
    EXPECT_EQ(scene::SyncMaterial(*m_material, *m_resources), nullptr);
}

TEST_F(MaterialScriptIntegrationTest, MalformedMaterialRecoversAfterReimportAndEmptyShaderUsesFallback)
{
    const auto path = m_temp.File("broken.mat").generic_string();
    ASSERT_TRUE(util::FileSystem::WriteText(path, "[params\n"));
    m_material->SetMaterialPath(path);
    EXPECT_EQ(scene::SyncMaterial(*m_material, *m_resources), m_resources->GetErrorMaterial(false));
    asset::MaterialAssetAuthoring authored;
    authored.shaderPath = "repaired.hlsl";
    ASSERT_TRUE(asset::SaveMaterialAssetToFile(path, authored));
    asset::AssetManager::FlushFailed();
    const auto* repaired = scene::SyncMaterial(*m_material, *m_resources);
    ASSERT_NE(repaired, nullptr);
    EXPECT_EQ(repaired->shaderPath, "repaired.hlsl");
    authored.shaderPath.clear();
    const auto emptyPath = m_temp.File("empty.mat").generic_string();
    ASSERT_TRUE(asset::SaveMaterialAssetToFile(emptyPath, authored));
    m_material->SetMaterialPath(emptyPath);
    EXPECT_EQ(scene::SyncMaterial(*m_material, *m_resources), m_resources->GetErrorMaterial(false));
}

TEST_F(MaterialScriptIntegrationTest, MissingComponentIsVisibleWithoutAddingItToTheScene)
{
    auto& go = m_scene.CreateGameObject("No material component");
    r::Mesh mesh;
    scene::MeshRenderer meshRenderer;
    meshRenderer.mesh = &mesh;
    go.AddComponent<scene::MeshRenderer>(meshRenderer);
    r::Camera camera;
    r::RenderSettings settings;
    r::RenderPassHandles handles;
    scene::RenderPassContext context(m_scene, m_backend, *m_resources, camera, settings, {}, UINT32_MAX, handles);
    scene::ExtractRenderScene(context);
    ASSERT_EQ(context.renderScene->objects.size(), 1u);
    ASSERT_EQ(context.renderScene->items.size(), 1u);
    EXPECT_TRUE(context.renderScene->objects[0].colorEligible);
    EXPECT_TRUE(context.renderScene->items[0].material.valid);
    EXPECT_EQ(m_resources->Get(context.renderScene->items[0].material.shader)->GetPath(), r::ERROR_SURFACE_SHADER);
    EXPECT_EQ(go.GetComponent<scene::MaterialComponent>(), nullptr);
}

TEST_F(MaterialScriptIntegrationTest, WrongMeshTypeOrRenderPathUsesErrorMaterial)
{
    ASSERT_TRUE(m_material->EnsureMaterialAsset());
    auto* authored = asset::AssetManager::Get<asset::MaterialAsset>(m_material->GetMaterialAsset());
    ASSERT_NE(authored, nullptr);
    EditMaterialAuthoring(*authored, [&](auto& values) { values.meshType = asset::MeshType::Skinned; });
    EXPECT_EQ(scene::SyncMaterial(*m_material, *m_resources), m_resources->GetErrorMaterial(false));
    EditMaterialAuthoring(*authored, [&](auto& values) { values.meshType = asset::MeshType::Any; });
    for (const auto path : {asset::RenderPath::UI, asset::RenderPath::Decal, asset::RenderPath::Particle,
                           asset::RenderPath::PostProcess, asset::RenderPath::Trail}) {
        EditMaterialAuthoring(*authored, [&](auto& values) { values.renderPath = path; });
        EXPECT_EQ(scene::SyncMaterial(*m_material, *m_resources), m_resources->GetErrorMaterial(false));
    }
}

TEST_F(MaterialScriptIntegrationTest, ConstantBufferFailureUsesErrorShaderWithoutRequiringAConstantBuffer)
{
    m_backend.failConstantBuffers = true;
    const auto* fallback = scene::SyncMaterial(*m_material, *m_resources);
    ASSERT_NE(fallback, nullptr);
    EXPECT_EQ(fallback->shaderPath, r::ERROR_SURFACE_SHADER);
    EXPECT_FALSE(fallback->paramsBuffer.IsValid());
    m_backend.failConstantBuffers = false;
    const auto* repaired = scene::SyncMaterial(*m_material, *m_resources);
    ASSERT_NE(repaired, nullptr);
    EXPECT_EQ(repaired->shaderPath, "typed.hlsl");
}

TEST_F(MaterialScriptIntegrationTest, FailedShaderRecoversWithTheSameMaterialReference)
{
    m_backend.failedShaderPath = "typed.hlsl";
    const auto reference = m_material->GetMaterialPath();
    EXPECT_EQ(scene::SyncMaterial(*m_material, *m_resources), m_resources->GetErrorMaterial(false));
    EXPECT_EQ(m_material->GetMaterialPath(), reference);
    m_backend.failedShaderPath.clear();
    const auto* repaired = scene::SyncMaterial(*m_material, *m_resources);
    ASSERT_NE(repaired, nullptr);
    EXPECT_EQ(repaired->shaderPath, "typed.hlsl");
    EXPECT_NE(m_resources->Get(repaired->paramsBuffer), nullptr);
}

TEST_F(MaterialScriptIntegrationTest, BrokenPrimarySlotDoesNotHideOtherSubmeshesOrCarryInvisibleOverrides)
{
    asset::Model model;
    for (uint32_t i = 0; i < 2; ++i) {
        auto mesh = std::make_unique<r::Mesh>();
        mesh->vertexBuffer = {10 + i, 1};
        mesh->indexBuffer = {20 + i, 1};
        model.meshes.push_back(std::move(mesh));
    }
    auto& go = m_scene.CreateGameObject("Two slots");
    scene::SkinnedMeshRenderer meshRenderer;
    meshRenderer.model = &model;
    go.AddComponent<scene::SkinnedMeshRenderer>(meshRenderer);
    auto& slots = go.AddComponent<scene::MaterialComponent>();
    slots.SetMaterialPath(m_temp.File("missing-primary.mat").generic_string());
    slots.ResizeSlots(2);
    slots.RawSlotAt(1).SetMaterialPath(m_material->GetMaterialPath());
    EditMaterialAuthoring(slots, [&](auto& values) { values.paramOverrides["albedo"] = {0, 0, 0, 0}; });
    EditMaterialAuthoring(slots, [&](auto& values) { values.hasBlendModeOverride = true; });
    EditMaterialAuthoring(slots, [&](auto& values) { values.blendModeOverride = r::BlendMode::ALPHA_BLEND; });
    EditMaterialAuthoring(slots, [&](auto& values) { values.hasRenderQueueOverride = true; });
    EditMaterialAuthoring(slots, [&](auto& values) { values.renderQueueOverride = 5000; });
    r::Camera camera;
    r::RenderSettings settings;
    r::RenderPassHandles handles;
    scene::RenderPassContext context(m_scene, m_backend, *m_resources, camera, settings, {}, UINT32_MAX, handles);
    scene::ExtractRenderScene(context);
    ASSERT_EQ(context.renderScene->objects.size(), 1u);
    EXPECT_TRUE(context.renderScene->objects[0].colorEligible);
    ASSERT_EQ(context.renderScene->items.size(), 2u);
    const auto& first = context.renderScene->items[0].material;
    const auto& second = context.renderScene->items[1].material;
    EXPECT_TRUE(first.valid && second.valid);
    ASSERT_NE(m_resources->Get(first.shader), nullptr);
    EXPECT_EQ(m_resources->Get(first.shader)->GetPath(), r::ERROR_SKINNED_SHADER);
    EXPECT_EQ(first.capabilities.blend, r::BlendMode::OPAQUE_BLEND);
    EXPECT_EQ(first.renderQueue, r::RenderQueue::GEOMETRY);
    EXPECT_TRUE(first.doubleSided);
    float color[4]{};
    std::memcpy(color, first.gbufferParams.data(), sizeof(color));
    EXPECT_FLOAT_EQ(color[0], 1);
    EXPECT_FLOAT_EQ(color[1], 0);
    EXPECT_FLOAT_EQ(color[2], 1);
    EXPECT_FLOAT_EQ(color[3], 1);
    EXPECT_EQ(m_resources->Get(second.shader)->GetPath(), "typed.hlsl");
    slots.RawSlotAt(1).visible = false;
    scene::ExtractRenderScene(context);
    EXPECT_FALSE(context.renderScene->items[1].visible);
    slots.enabled = false;
    scene::ExtractRenderScene(context);
    EXPECT_FALSE(context.renderScene->objects[0].colorEligible);
}

TEST_F(MaterialScriptIntegrationTest, SurfaceMaterialOnSkinnedMeshUsesEmbeddedSkinnedShader)
{
    ASSERT_TRUE(m_material->EnsureMaterialAsset());
    auto* authored = asset::AssetManager::Get<asset::MaterialAsset>(m_material->GetMaterialAsset());
    ASSERT_NE(authored, nullptr);
    EditMaterialAuthoring(*authored, [&](auto& values) { values.meshType = asset::MeshType::Surface; });
    const auto* material = scene::SyncMaterial(*m_material, *m_resources, true);
    ASSERT_NE(material, nullptr);
    EXPECT_EQ(material->shaderPath, r::ERROR_SKINNED_SHADER);
    EXPECT_TRUE(m_resources->GetShaderCapabilities(material->shader).SupportsSkinning());
    EXPECT_FALSE(m_resources->GetShaderCapabilities(material->shader).SupportsGBuffer());
}

TEST_F(MaterialScriptIntegrationTest, EmbeddedMaterialsAreIndependentPerManagerAndRecreatedAfterReset)
{
    auto* first = m_resources->GetErrorMaterial(false);
    const auto previous = first->shader;
    EXPECT_EQ(m_resources->GetErrorMaterial(false), first);
    EXPECT_NE(m_resources->GetErrorMaterial(true), first);
    m_resources->Reset();
    EXPECT_EQ(m_resources->Get(previous), nullptr);
    auto* restored = m_resources->GetErrorMaterial(false);
    ASSERT_NE(restored, nullptr);
    EXPECT_NE(m_resources->Get(restored->shader), nullptr);
    ReloadRenderer otherBackend;
    r::ResourceManager other(otherBackend);
    EXPECT_NE(other.GetErrorMaterial(false), restored);
}

TEST_F(MaterialScriptIntegrationTest, MissingParticleMaterialClearsPreviousShaderTintAndAuxiliaryTextures)
{
    auto& emitter = m_scene.CreateGameObject("Particle").AddComponent<scene::ParticleEmitter>();
    emitter.settings.materialPath = m_temp.File("missing-particle.mat").generic_string();
    emitter.settings.emitRate = 0;
    emitter.settings.culling.cullingEnabled = false;
    emitter.runtime.customShader = m_resources->LoadShader("old-particle.hlsl");
    emitter.runtime.loadedShaderPath = "old-particle.hlsl";
    emitter.runtime.materialParamsCB = m_resources->CreateConstantBuffer(16);
    emitter.runtime.materialTint = {0, 0, 0, 0};
    emitter.runtime.sixWayAlbedoColorTexture = {4, 1};
    emitter.runtime.sixWayEmissionColorTexture = {5, 1};
    emitter.runtime.loadedSixWayAlbedoColorTexturePath = "old-color.png";
    emitter.runtime.loadedSixWayEmissionColorTexturePath = "old-emission.png";
    r::Camera camera;
    r::RenderSettings settings;
    r::RenderPassHandles handles;
    handles.particleShader = m_resources->LoadShader("Particle.hlsl");
    handles.particleIB = {1, 1};
    scene::RenderPassContext context(m_scene, m_backend, *m_resources, camera, settings, {}, UINT32_MAX, handles);
    r::RenderScene output;
    scene::ExtractRenderParticles(context, output);
    EXPECT_FALSE(emitter.runtime.customShader.IsValid());
    EXPECT_TRUE(emitter.runtime.loadedShaderPath.empty());
    EXPECT_FALSE(emitter.runtime.materialParamsCB.IsValid());
    EXPECT_VEC4_NEAR(emitter.runtime.materialTint, (math::Vector4{1, 1, 1, 1}), 0.0f);
    EXPECT_FALSE(emitter.runtime.sixWayAlbedoColorTexture.IsValid());
    EXPECT_FALSE(emitter.runtime.sixWayEmissionColorTexture.IsValid());
    EXPECT_TRUE(emitter.runtime.loadedSixWayAlbedoColorTexturePath.empty());
    EXPECT_TRUE(emitter.runtime.loadedSixWayEmissionColorTexturePath.empty());
}

TEST_F(MaterialScriptIntegrationTest, ParticleShaderRetriesFailedLoadWithAnUnchangedPath)
{
    asset::MaterialAssetAuthoring authored;
    authored.shaderPath = "custom-particle.hlsl";
    authored.renderPath = asset::RenderPath::Particle;
    const auto path = m_temp.File("particle.mat").generic_string();
    ASSERT_TRUE(asset::SaveMaterialAssetToFile(path, authored));
    auto& emitter = m_scene.CreateGameObject("Particle").AddComponent<scene::ParticleEmitter>();
    emitter.settings.materialPath = path;
    emitter.settings.emitRate = 0;
    emitter.settings.culling.cullingEnabled = false;
    r::Camera camera;
    r::RenderSettings settings;
    r::RenderPassHandles handles;
    handles.particleShader = m_resources->LoadShader("Particle.hlsl");
    handles.particleIB = {1, 1};
    scene::RenderPassContext context(m_scene, m_backend, *m_resources, camera, settings, {}, UINT32_MAX, handles);
    r::RenderScene output;
    m_backend.failedShaderPath = authored.shaderPath;
    scene::ExtractRenderParticles(context, output);
    EXPECT_FALSE(emitter.runtime.customShader.IsValid());
    m_backend.failedShaderPath.clear();
    scene::ExtractRenderParticles(context, output);
    EXPECT_NE(m_resources->Get(emitter.runtime.customShader), nullptr);
    EXPECT_EQ(emitter.runtime.loadedShaderPath, authored.shaderPath);
}

TEST_F(MaterialScriptIntegrationTest, TypedSettersAndGettersMatchUploadedValues)
{
    const auto instance = m_proxy.Instance();
    const scene::MaterialPropertyId mode("mode"), mask("mask"), enabled("enabled"), uv("uv"), matrix("matrix");
    ASSERT_TRUE(instance.SetInt(mode, -16777217));
    ASSERT_TRUE(instance.SetUInt(mask, UINT32_MAX));
    ASSERT_TRUE(instance.SetBool(enabled, true));
    ASSERT_TRUE(instance.SetVector2(uv, {2, 3}));
    auto transform = math::Matrix4::Identity();
    transform.m[0][3] = 7;
    ASSERT_TRUE(instance.SetMatrix(matrix, transform));
    int signedValue = 0;
    uint32_t unsignedValue = 0;
    ASSERT_TRUE(instance.TryGetInt(mode, signedValue));
    ASSERT_TRUE(instance.TryGetUInt(mask, unsignedValue));
    EXPECT_EQ(signedValue, -16777217);
    EXPECT_EQ(unsignedValue, UINT32_MAX);
    EXPECT_FALSE(instance.SetInt(mask, -1));
    EXPECT_FALSE(instance.SetFloat(mode, 3));
    EXPECT_FALSE(instance.SetVector4(matrix, {1, 2, 3, 4}));
    renderer::ShaderDescriptor descriptor;
    descriptor.vars = m_backend.shaderVars;
    std::vector<uint8_t> bytes(96);
    asset::ApplyMaterialParamOverrides(m_material->Authoring().paramOverrides, descriptor, bytes);
    asset::ApplyMaterialIntegerOverrides(m_material->Authoring().integerParamOverrides, descriptor, bytes);
    int32_t uploadedMode = 0;
    uint32_t uploadedMask = 0;
    float uploadedTranslation = 0;
    std::memcpy(&uploadedMode, bytes.data(), 4);
    std::memcpy(&uploadedMask, bytes.data() + 4, 4);
    std::memcpy(&uploadedTranslation, bytes.data() + 80, 4);
    EXPECT_EQ(uploadedMode, -16777217);
    EXPECT_EQ(uploadedMask, UINT32_MAX);
    EXPECT_FLOAT_EQ(uploadedTranslation, 7);
    ASSERT_TRUE(instance.ClearOverride(mode));
    EXPECT_FALSE(m_material->Authoring().integerParamOverrides.contains("mode"));
    ASSERT_TRUE(instance.ClearAllOverrides());
    EXPECT_TRUE(m_material->Authoring().integerParamOverrides.empty());
    EXPECT_TRUE(m_material->Authoring().paramOverrides.empty());
}

TEST_F(MaterialScriptIntegrationTest, LegacyFiberPropertyNamesReadWriteAndClearTheSameRuntimeValue)
{
    bool reload = false;
    for (const char* shaderName : {"grassShading", "planarShading"}) {
        SCOPED_TRACE(shaderName);
        m_backend.shaderVars = {{shaderName, 0, 4, 1, 1, r::ShaderVarClass::Scalar, r::ShaderVarType::Float}};
        m_backend.shaderSize = 4;
        if (reload) ASSERT_TRUE(m_resources->ReloadShader("typed.hlsl"));
        const auto instance = m_proxy.Instance();
        const scene::MaterialPropertyId legacy("grassShading"), canonical("planarShading");
        EXPECT_TRUE(instance.HasProperty(legacy));
        EXPECT_TRUE(instance.HasProperty(canonical));
        EditMaterialAuthoring(*m_material, [&](auto& values) { values.paramOverrides["grassShading"] = {0.9f}; });
        float value = 0;
        ASSERT_TRUE(instance.TryGetFloat(canonical, value));
        EXPECT_FLOAT_EQ(value, 0.9f);
        ASSERT_TRUE(instance.SetFloat(legacy, 0.7f));
        ASSERT_TRUE(instance.TryGetFloat(canonical, value));
        EXPECT_FLOAT_EQ(value, 0.7f);
        ASSERT_TRUE(instance.SetFloat(canonical, 0.2f));
        ASSERT_TRUE(instance.TryGetFloat(legacy, value));
        EXPECT_FLOAT_EQ(value, 0.2f);
        r::ShaderDescriptor descriptor;
        descriptor.vars = m_backend.shaderVars;
        std::vector<uint8_t> bytes(4);
        asset::ApplyMaterialParamOverrides(m_material->Authoring().paramOverrides, descriptor, bytes);
        std::memcpy(&value, bytes.data(), 4);
        EXPECT_FLOAT_EQ(value, 0.2f);
        ASSERT_TRUE(instance.ClearOverride(legacy));
        EXPECT_FALSE(instance.TryGetFloat(canonical, value));
        reload = true;
    }
}

TEST_F(MaterialScriptIntegrationTest, ShaderReloadInvalidatesPreviouslyValidPropertyTypes)
{
    const auto instance = m_proxy.Instance();
    const scene::MaterialPropertyId mode("mode");
    ASSERT_TRUE(instance.SetInt(mode, 1));
    m_backend.shaderVars[0].varType = r::ShaderVarType::Float;
    ASSERT_TRUE(m_resources->ReloadShader("typed.hlsl"));
    EXPECT_FALSE(instance.SetInt(mode, 2));
    EXPECT_TRUE(instance.SetFloat(mode, 2));
    EXPECT_FALSE(m_material->Authoring().integerParamOverrides.contains("mode"));
}

TEST_F(MaterialScriptIntegrationTest, ExplicitCapabilityNormalizesDifferent96ByteConstantLayouts)
{
    const auto shaderPath = m_temp.File("RenamedSurface.hlsl").generic_string();
    ASSERT_TRUE(util::FileSystem::WriteText(shaderPath, "float4 PSMain() : SV_Target { return 1; }\n"));
    ASSERT_TRUE(util::FileSystem::WriteText(shaderPath + ".meta", "[shader]\n" + std::string(kSurfaceShaderDeclaration)));
    m_backend.shaderSize = 96;
    m_backend.shaderVars = {
        {"albedo", 32, 16, 1, 4, r::ShaderVarClass::Vector, r::ShaderVarType::Float},
        {"metallic", 0, 4, 1, 1, r::ShaderVarClass::Scalar, r::ShaderVarType::Float},
        {"roughness", 4, 4, 1, 1, r::ShaderVarClass::Scalar, r::ShaderVarType::Float},
        {"uvTiling", 8, 8, 1, 2, r::ShaderVarClass::Vector, r::ShaderVarType::Float},
        {"uvOffset", 16, 8, 1, 2, r::ShaderVarClass::Vector, r::ShaderVarType::Float},
        {"alphaCutoff", 24, 4, 1, 1, r::ShaderVarClass::Scalar, r::ShaderVarType::Float}
    };
    asset::MaterialAssetAuthoring authored;
    authored.shaderPath = shaderPath;
    authored.params["albedo"] = {0.1f, 0.2f, 0.3f, 1.0f};
    authored.params["metallic"] = {0.25f};
    authored.params["roughness"] = {0.6f};
    authored.params["uvTiling"] = {2.0f, 3.0f};
    authored.params["uvOffset"] = {0.4f, 0.5f};
    authored.params["alphaCutoff"] = {0.7f};
    const auto path = m_temp.File("renamed.mat").generic_string();
    ASSERT_TRUE(asset::SaveMaterialAssetToFile(path, authored));
    auto& object = m_scene.CreateGameObject("Surface");
    r::Mesh mesh;
    scene::MeshRenderer meshRenderer;
    meshRenderer.mesh = &mesh;
    object.AddComponent<scene::MeshRenderer>(meshRenderer);
    object.AddComponent<scene::MaterialComponent>().SetMaterialPath(path);
    r::Camera camera;
    r::RenderSettings settings;
    r::RenderPassHandles handles;
    scene::RenderPassContext context(m_scene, m_backend, *m_resources, camera, settings, {}, UINT32_MAX, handles);

    scene::ExtractRenderScene(context);

    ASSERT_NE(context.renderScene, nullptr);
    ASSERT_EQ(context.renderScene->items.size(), 1u);
    const auto& material = context.renderScene->items.front().material;
    EXPECT_TRUE(material.shaderCapabilities.SupportsGBuffer());
    EXPECT_TRUE(material.capabilities.gbufferEquivalentShader);
    EXPECT_FALSE(material.directGBufferParams);
    const auto value = [&](std::size_t offset) {
        float result = 0;
        std::memcpy(&result, material.gbufferParams.data() + offset, sizeof(result));
        return result;
    };
    EXPECT_FLOAT_EQ(value(0), 0.1f);
    EXPECT_FLOAT_EQ(value(12), 1.0f);
    EXPECT_FLOAT_EQ(value(16), 0.25f);
    EXPECT_FLOAT_EQ(value(20), 0.6f);
    EXPECT_FLOAT_EQ(value(48), 2.0f);
    EXPECT_FLOAT_EQ(value(52), 3.0f);
    EXPECT_FLOAT_EQ(value(56), 0.4f);
    EXPECT_FLOAT_EQ(value(60), 0.5f);
    EXPECT_FLOAT_EQ(value(64), 0.7f);
}

TEST_F(MaterialScriptIntegrationTest, ExtractsRoughSolidAndSmoothThinRuntimeValuesWhileRejectingRoughThinAndAlphaOverrides)
{
    m_backend.shaderVars = StandardPbrVariables();
    asset::MaterialAssetAuthoring glass;
    glass.shaderPath = m_temp.File("GlassSurface.hlsl").generic_string();
    ASSERT_TRUE(util::FileSystem::WriteText(glass.shaderPath, "float4 PSMain() : SV_Target { return 1; }\n"));
    ASSERT_TRUE(util::FileSystem::WriteText(glass.shaderPath + ".meta", "[shader]\n" + std::string(kSurfaceShaderDeclaration)));
    glass.params["albedo"] = {1, 1, 1, 1};
    glass.params["metallic"] = {0};
    glass.params["roughness"] = {0};
    glass.params["alphaCutoff"] = {0.5f};
    glass.dielectric.transmission = 1;
    glass.dielectric.ior = 1.6f;
    glass.dielectric.attenuationColor = {0.25f, 0.5f, 1};
    glass.dielectric.attenuationDistance = 2;
    const auto path = m_temp.File("glass.mat").generic_string();
    ASSERT_TRUE(asset::SaveMaterialAssetToFile(path, glass));
    auto& object = m_scene.CreateGameObject("Glass");
    renderer::Mesh mesh;
    scene::MeshRenderer meshRenderer;
    meshRenderer.mesh = &mesh;
    object.AddComponent<scene::MeshRenderer>(meshRenderer);
    auto& slot = object.AddComponent<scene::MaterialComponent>();
    slot.SetMaterialPath(path);
    renderer::Camera camera;
    renderer::RenderSettings settings;
    renderer::RenderPassHandles handles;
    scene::RenderPassContext context(m_scene, m_backend, *m_resources, camera, settings, {}, UINT32_MAX, handles);
    settings.modeRequest.mode = renderer::RenderMode::PATH_TRACING;
    scene::ExtractRenderScene(context);
    ASSERT_NE(context.renderScene, nullptr);
    ASSERT_EQ(context.renderScene->items.size(), 1u);
    EXPECT_FLOAT_EQ(context.renderScene->items[0].material.surface.dielectric.transmission, 0);
    context.experimentalRayTracingEnabled = true;
    scene::ExtractRenderScene(context);
    ASSERT_NE(context.renderScene, nullptr);
    ASSERT_EQ(context.renderScene->items.size(), 1u);
    const auto first = context.renderScene->items[0].material.surface;
    EXPECT_EQ(first.dielectric, glass.dielectric);
    EXPECT_TRUE(first.solidDielectricSupported);
    EXPECT_FALSE(first.standardSurfaceSupported);
    ASSERT_NE(slot.material, nullptr);
    EXPECT_EQ(slot.material->dielectric, glass.dielectric);
    auto* shared = asset::AssetManager::Get<asset::MaterialAsset>(slot.GetMaterialAsset());
    ASSERT_NE(shared, nullptr);
    EditMaterialAuthoring(*shared, [&](auto& values) { values.dielectric.ior = 1.7f; });
    scene::ExtractRenderScene(context);
    const auto edited = context.renderScene->items[0].material.surface;
    EXPECT_FLOAT_EQ(edited.dielectric.ior, 1.7f);
    EXPECT_NE(first, edited);
    EditMaterialAuthoring(slot, [&](auto& values) { values.paramOverrides["roughness"] = {0.1f}; });
    scene::ExtractRenderScene(context);
    EXPECT_TRUE(context.renderScene->items[0].material.surface.solidDielectricSupported);
    EXPECT_FLOAT_EQ(context.renderScene->items[0].material.surface.roughness, 0.1f);
    EditMaterialAuthoring(*shared, [&](auto& values) { values.dielectric.thinWalled = true; });
    EditMaterialAuthoring(*shared, [&](auto& values) { values.dielectric.attenuationColor = {1, 1, 1}; });
    scene::ExtractRenderScene(context);
    EXPECT_EQ(context.renderScene->items[0].material.surface.issue,
        renderer::SurfaceMaterialIssue::SOLID_DIELECTRIC_UNSUPPORTED);
    EditMaterialAuthoring(slot, [&](auto& values) { values.paramOverrides.erase("roughness"); });
    scene::ExtractRenderScene(context);
    EXPECT_TRUE(context.renderScene->items[0].material.surface.solidDielectricSupported);
    EXPECT_TRUE(context.renderScene->items[0].material.surface.dielectric.thinWalled);
    EditMaterialAuthoring(*shared, [&](auto& values) { values.dielectric.thinWalled = false; });
    EditMaterialAuthoring(slot, [&](auto& values) { values.paramOverrides["albedo"] = {1, 1, 1, 0.75f}; });
    scene::ExtractRenderScene(context);
    EXPECT_FALSE(context.renderScene->items[0].material.surface.solidDielectricSupported);
    EXPECT_FLOAT_EQ(context.renderScene->items[0].material.surface.dielectric.transmission, 1);
    EditMaterialAuthoring(slot, [&](auto& values) { values.paramOverrides.erase("albedo"); });
    EditMaterialAuthoring(slot, [&](auto& values) { values.hasBlendModeOverride = true; });
    EditMaterialAuthoring(slot, [&](auto& values) { values.blendModeOverride = renderer::BlendMode::ALPHA_BLEND; });
    scene::ExtractRenderScene(context);
    EXPECT_NE(context.renderScene->items[0].material.rayCapabilities.opacity, renderer::RayOpacity::OPAQUE_SURFACE);
}

class MaterialExtractionSharingTest : public ShaderCapabilityReloadTest {
protected:
    void SetUp() override
    {
        ShaderCapabilityReloadTest::SetUp();
        ASSERT_TRUE(WriteShader("Shared.hlsl", kSurfaceShaderDeclaration));
        m_backend.shaderSize = 96;
        m_backend.shaderVars = StandardPbrVariables();
        m_backend.shaderVars.push_back({"flags", 88, 4, 1, 1, r::ShaderVarClass::Scalar, r::ShaderVarType::UInt});
        m_backend.allowGeometry = true;
        m_resources = std::make_unique<r::ResourceManager>(m_backend);
        m_scene.SetResourceManager(m_resources.get());
        asset::AssetManager::Init(*m_resources, m_temp.Path().generic_string() + "/");
        asset::DataAssetRegistry::ClearCache();
        /// @note Retire the previous fixture's function-static instance materials before this renderer owns any constant buffers.
        Extract();
        m_backend.constantBufferUpdates = 0;
        asset::MaterialAssetAuthoring material;
        material.shaderPath = ShaderPath("Shared.hlsl");
        m_materialPath = m_temp.File("Shared.mat").generic_string();
        ASSERT_TRUE(asset::SaveMaterialAssetToFile(m_materialPath, material));
        m_asset = asset::AssetManager::Load<asset::MaterialAsset>(m_materialPath);
        ASSERT_TRUE(m_asset.IsValid());
        m_mesh.indexCount = 3;
        m_mesh.boundsRadius = 1;
    }

    void TearDown() override
    {
        m_scene.Clear();
        m_resources->Reset();
        Extract();
        asset::DataAssetRegistry::ClearCache();
        asset::AssetManager::UnloadAll();
        m_resources.reset();
        ShaderCapabilityReloadTest::TearDown();
    }

    scene::GameObject& AddObject(float z = 0)
    {
        auto& go = m_scene.CreateGameObject("Shared material");
        scene::MeshRenderer mesh;
        mesh.mesh = &m_mesh;
        go.AddComponent<scene::MeshRenderer>(mesh);
        go.AddComponent<scene::MaterialComponent>().SetMaterialAsset(m_asset);
        go.transform.position = go.transform.worldPosition = {0, 0, z};
        return go;
    }

    std::shared_ptr<const r::RenderScene> Extract(bool ray = false)
    {
        r::Camera camera;
        r::RenderSettings settings;
        if (ray) settings.modeRequest.mode = r::RenderMode::PATH_TRACING;
        r::RenderPassHandles handles;
        scene::RenderPassContext context(m_scene, m_backend, *m_resources, camera, settings, {}, UINT32_MAX, handles);
        context.isDeferred = true;
        context.cullProjScaleY = 1;
        context.height = 600;
        context.experimentalRayTracingEnabled = ray;
        scene::ExtractRenderScene(context);
        return context.renderScene;
    }

    static float Roughness(const r::RenderMaterial& material)
    {
        float value = 0;
        std::memcpy(&value, material.gbufferParams.data() + 20, sizeof(value));
        return value;
    }

    std::shared_ptr<const r::RenderScene> ExtractDemand(const r::RenderSettings& settings,
        const std::function<void(scene::RenderPassContext&)>& configure = {}, bool scoped = true, bool nested = false)
    {
        r::Camera camera;
        r::RenderPassHandles handles;
        scene::RenderPassContext context(m_scene, m_backend, *m_resources, camera, settings, {}, UINT32_MAX, handles);
        context.isDeferred = true;
        context.cullProjScaleY = 1;
        context.height = 600;
        if (configure) configure(context);
        if (scoped) {
            scene::material_demand_detail::BindingDemandScope demand(context);
            if (nested) {
                scene::material_demand_detail::BindingDemandScope reentry(context);
                scene::ExtractRenderScene(context);
            } else scene::ExtractRenderScene(context);
        } else scene::ExtractRenderScene(context);
        return context.renderScene;
    }

    ReloadRenderer m_backend;
    std::unique_ptr<r::ResourceManager> m_resources;
    scene::Scene m_scene;
    r::Mesh m_mesh;
    asset::AssetHandle<asset::MaterialAsset> m_asset;
    std::string m_materialPath;
};

/// @note This hook is deliberately unregistered and does not opt into edit-mode execution. Native render
/// @note hooks still run under those conditions, even when the entry has been marked for pending removal.
class GeometryRenderWriter final : public scene::Script {
public:
    std::vector<std::shared_ptr<const r::RenderScene>> views;
    std::function<void()> afterFirstView;
    void OnSetupRenderPasses(scene::RenderPipeline&, scene::RenderPassContext& context) override {
        views.push_back(context.renderScene);
    }
    void OnPostRender() override {
        if (views.size() == 1 && afterFirstView) afterFirstView();
    }
};

class ShadowContextObserver final : public scene::Script {
public:
    int calls = 0;
    r::RenderLightingInput lighting;
    std::shared_ptr<const r::RenderScene> snapshot;
    void OnSetupRenderPasses(scene::RenderPipeline&, scene::RenderPassContext& context) override {
        ++calls;
        lighting = static_cast<const r::RenderLightingInput&>(context);
        snapshot = context.renderScene;
    }
};

class ShadowPreparationRenderTest : public MaterialExtractionSharingTest {
protected:
    void SetUp() override {
        MaterialExtractionSharingTest::SetUp();
        m_backend.allowRenderTargets = true;
    }
    ShadowContextObserver& Observe(scene::GameObject& owner) {
        auto& component = owner.AddComponent<scene::ScriptComponent>();
        auto observer = std::make_unique<ShadowContextObserver>();
        auto* result = observer.get();
        component.scripts.emplace_back().script = std::move(observer);
        return *result;
    }
    void Render(const r::RenderSettings& settings) {
        r::Camera camera;
        scene::RenderSystem(m_scene, m_backend, *m_resources, camera, {}, &settings);
    }
};

TEST_F(ShadowPreparationRenderTest, ShadowsOffStillPublishesTheOriginalFitToANativeConsumer)
{
    auto& object = AddObject(0);
    auto& observer = Observe(m_scene.CreateGameObject("Native shadow input reader"));
    r::RenderSettings settings;
    settings.shadowEnabled = false;
    Render(settings);
    ASSERT_EQ(observer.calls, 1);
    r::Camera camera;
    const r::ShadowBounds firstBounds{{0, 0, 0}, 1, true};
    const auto expected = r::PrepareShadows(camera, settings, firstBounds,
        observer.lighting.lightData.lightDir.Normalized(), 0, 1);
    EXPECT_MAT4_NEAR(observer.lighting.lightVP, expected.lightVP, testkit::kTolerance);
    EXPECT_MAT4_NEAR(observer.lighting.lightView, expected.lightView, testkit::kTolerance);
    EXPECT_EQ(observer.lighting.shadowCascadeCount, expected.count);
    object.transform.position = object.transform.worldPosition = {90, 0, 0};
    Render(settings);
    ASSERT_EQ(observer.calls, 2);
    const r::ShadowBounds currentBounds{{90, 0, 0}, 1, true};
    const auto current = r::PrepareShadows(camera, settings, currentBounds,
        observer.lighting.lightData.lightDir.Normalized(), 0, 1);
    EXPECT_MAT4_NEAR(observer.lighting.lightVP, current.lightVP, testkit::kTolerance);
    ASSERT_NE(observer.snapshot, nullptr);
    EXPECT_MAT4_NEAR(observer.snapshot->lighting.lightVP, current.lightVP, testkit::kTolerance);
}

TEST_F(ShadowPreparationRenderTest, LateNativeConsumerKeepsTheInitialFitWithoutReextractingGeometry)
{
    r::RenderSettings settings;
    settings.shadowEnabled = false;
    settings.volumetricLight.enabled = false;
    settings.froxelFog.enabled = false;
    auto& initialObserver = Observe(m_scene.CreateGameObject("Initial full-fit reference"));
    Render(settings);
    ASSERT_EQ(initialObserver.calls, 1);
    const auto expected = initialObserver.lighting;
    m_scene.Clear();
    ShadowContextObserver* lateObserver = nullptr;
    int callbacks = 0;
    m_backend.afterConstantBufferUpdate = [&](size_t bytes) {
        if (bytes != sizeof(r::AdvancedGraphicsCB)) return;
        m_backend.afterConstantBufferUpdate = {};
        ++callbacks;
        AddObject(10000);
        lateObserver = &Observe(m_scene.CreateGameObject("Late native shadow reader"));
    };
    Render(settings);
    m_backend.afterConstantBufferUpdate = {};
    ASSERT_EQ(callbacks, 1);
    ASSERT_NE(lateObserver, nullptr);
    ASSERT_EQ(lateObserver->calls, 1);
    EXPECT_MAT4_NEAR(lateObserver->lighting.lightVP, expected.lightVP, testkit::kTolerance);
    EXPECT_MAT4_NEAR(lateObserver->lighting.lightView, expected.lightView, testkit::kTolerance);
    EXPECT_VEC3_NEAR(lateObserver->lighting.lightEyePos, expected.lightEyePos, testkit::kTolerance);
    EXPECT_EQ(lateObserver->lighting.shadowCascadeCount, expected.shadowCascadeCount);
    ASSERT_NE(lateObserver->snapshot, nullptr);
    EXPECT_TRUE(lateObserver->snapshot->Objects().empty());
    EXPECT_MAT4_NEAR(lateObserver->snapshot->lighting.lightVP, expected.lightVP, testkit::kTolerance);
}

class ManagedFrameGeometryTest : public MaterialExtractionSharingTest {
protected:
    void SetUp() override
    {
        MaterialExtractionSharingTest::SetUp();
        m_previousFrame = Time::frameCount;
        m_previousPlay = scene::Script::IsInPlayMode();
        scene::Script::SetInPlayMode(false);
        Time::frameCount = 1701;
        m_backend.allowRenderTargets = true;
        auto& runtime = scene::ManagedScriptRuntime::Instance();
        ASSERT_TRUE(runtime.Initialize(FBZZ_MANAGED_HOST_TEST_DIRECTORY)) << runtime.LastDiagnostic();
        ASSERT_TRUE(runtime.Load(FBZZ_MANAGED_FIXTURE_TEST_ASSEMBLY, true)) << runtime.LastDiagnostic();
    }

    void TearDown() override
    {
        scene::ManagedScriptRuntime::Instance().Unload(&m_scene);
        scene::UISystemReleaseGpuResources(m_ui, *m_resources);
        scene::UISystemFlushCache(m_ui, m_resources.get());
        Time::frameCount = m_previousFrame;
        scene::Script::SetInPlayMode(m_previousPlay);
        MaterialExtractionSharingTest::TearDown();
    }

    scene::ManagedScriptAdapter* AttachManaged(scene::GameObject& go)
    {
        auto& component = go.AddComponent<scene::ScriptComponent>();
        auto& entry = component.scripts.emplace_back();
        entry.script = scene::ScriptFactory::Create("cs-internal:ManagedFixtures.SettingsComponent", entry.savedScriptId);
        if (!entry.script) return nullptr;
        entry.script->SetContext(&m_scene, &go);
        auto* adapter = scene::ManagedScriptRuntime::Instance().FindAdapter(entry.script.get());
        return adapter && adapter->RefreshOwnerBinding() ? adapter : nullptr;
    }

    GeometryRenderWriter& AttachNative(scene::GameObject& go)
    {
        auto& component = go.AddComponent<scene::ScriptComponent>();
        auto& entry = component.scripts.emplace_back();
        auto writer = std::make_unique<GeometryRenderWriter>();
        auto* result = writer.get();
        entry.script = std::move(writer);
        return *result;
    }

    void RenderView(scene::RenderFrameGeometryCache& cache, const r::RenderSettings& settings = {},
        float width = 800, float height = 600, scene::UIRenderTargetView target = scene::UIRenderTargetView::SceneViewport)
    {
        scene::RenderSystemUIOptions ui;
        ui.enabled = true;
        ui.viewportWidth = width;
        ui.viewportHeight = height;
        ui.targetView = target;
        ui.context = &m_ui;
        r::Camera camera;
        scene::RenderSystem(m_scene, m_backend, *m_resources, camera, {}, &settings, UINT32_MAX,
            &ui, nullptr, nullptr, nullptr, &cache);
    }

    std::shared_ptr<const r::RenderScene> ExtractView(const r::RenderSettings& settings, scene::RenderFrameGeometryCache* cache)
    {
        r::Camera camera;
        r::RenderPassHandles handles;
        if (cache) handles.bindPoseSkinningCB = cache->identityPalette;
        scene::RenderPassContext context(m_scene, m_backend, *m_resources, camera, settings, {}, UINT32_MAX, handles);
        context.isDeferred = true;
        context.height = 600;
        context.cullProjScaleY = 1;
        scene::ExtractRenderScene(context, cache);
        return context.renderScene;
    }

    static const r::RenderObject* Object(const r::RenderScene& view, scene::EntityID id)
    {
        for (const auto& object : view.Objects())
            if (object.sourceIndex == id.index && object.sourceGeneration == id.generation) return &object;
        return nullptr;
    }

    uint64_t m_previousFrame = 0;
    bool m_previousPlay = false;
    scene::UISystemContext m_ui;
    asset::Model m_skinnedModel;
};

TEST_F(ManagedFrameGeometryTest, KeepsManagedViewsIndependentWithoutPublishingFrameGeometry)
{
    auto& mesh = AddObject(2);
    ASSERT_NE(AttachManaged(mesh), nullptr);
    auto& hud = m_scene.CreateGameObject("OceanHUD");
    hud.AddComponent<scene::UICanvas>();
    auto& label = m_scene.CreateGameObject("OceanHUD_Controls");
    label.SetParent(hud);
    label.AddComponent<scene::UIText>().text = "Controls";
    m_skinnedModel.meshes.push_back(std::make_unique<r::Mesh>(m_mesh));
    auto& skinnedOwner = m_scene.CreateGameObject("Raster hidden skinned mesh");
    auto& skinned = skinnedOwner.AddComponent<scene::SkinnedMeshRenderer>();
    skinned.model = &m_skinnedModel;
    skinned.lodVisible = false;
    skinned.lodDither = 0.25f;
    skinnedOwner.AddComponent<scene::MaterialComponent>().SetMaterialAsset(m_asset);
    scene::RenderFrameGeometryCache cache;
    RenderView(cache);
    EXPECT_EQ(cache.geometry, nullptr);
    const auto first = ExtractView({}, nullptr);
    auto* material = asset::AssetManager::Get<asset::MaterialAsset>(m_asset);
    ASSERT_NE(material, nullptr);
    EditMaterialAuthoring(*material, [&](auto& values) { values.params["roughness"] = {0.81f}; });
    r::RenderSettings settings;
    settings.selectedObjects.push_back({mesh.GetID().index, mesh.GetID().generation});
    RenderView(cache, settings, 1200, 700, scene::UIRenderTargetView::GameViewport);
    EXPECT_EQ(cache.geometry, nullptr);
    const auto second = ExtractView(settings, nullptr);
    EXPECT_FALSE(first->sharedObjects);
    EXPECT_FALSE(second->sharedObjects);
    EXPECT_NE(first->Objects().data(), second->Objects().data());
    ASSERT_EQ(first->items.size(), 2u);
    ASSERT_EQ(second->items.size(), 2u);
    EXPECT_FLOAT_EQ(Roughness(first->items[0].material), 0.65f);
    EXPECT_FLOAT_EQ(Roughness(second->items[0].material), 0.81f);
    EXPECT_FALSE(first->IsObjectSelected(0));
    EXPECT_TRUE(second->IsObjectSelected(0));
    const auto* skinnedObject = Object(*second, skinnedOwner.GetID());
    ASSERT_NE(skinnedObject, nullptr);
    EXPECT_FALSE(skinnedObject->lodVisible);
    EXPECT_FLOAT_EQ(skinnedObject->lodDither, 0.25f);
    const std::array<r::Vertex, 3> vertices{};
    const auto firstBuffer = m_resources->CreateVertexBuffer(vertices.data(), sizeof(vertices), sizeof(r::Vertex));
    const auto secondBuffer = m_resources->CreateVertexBuffer(vertices.data(), sizeof(vertices), sizeof(r::Vertex));
    ASSERT_TRUE(firstBuffer.IsValid());
    ASSERT_TRUE(secondBuffer.IsValid());
    skinned.gpuSkinnedThisFrame = true;
    skinned.gpuSkinningFrame = Time::frameCount;
    skinned.skinnedBufferModel = skinned.model;
    skinned.skinnedVertexBuffers = testkit::MakeOwnedResources<renderer::BufferTag>({firstBuffer});
    const auto firstDeformed = ExtractView(settings, nullptr);
    skinned.skinnedVertexBuffers = testkit::MakeOwnedResources<renderer::BufferTag>({secondBuffer});
    const auto secondDeformed = ExtractView(settings, nullptr);
    EXPECT_NE(firstDeformed->Objects().data(), secondDeformed->Objects().data());
    EXPECT_EQ(firstDeformed->items[skinnedObject->firstItem].deformedVertexBuffer, firstBuffer);
    EXPECT_EQ(secondDeformed->items[skinnedObject->firstItem].deformedVertexBuffer, secondBuffer);
}

TEST_F(ManagedFrameGeometryTest, RetiresNoScriptGeometryAcrossNativeMutationAndRemovalWithinTheSameFrame)
{
    auto& firstOwner = AddObject(2);
    const auto firstId = firstOwner.GetID();
    auto& hidden = AddObject(4);
    const auto hiddenId = hidden.GetID();
    auto& removed = AddObject(6);
    const auto removedId = removed.GetID();
    scene::RenderFrameGeometryCache cache;
    /// @note Prime from a no-script view, then introduce a writer without changing the frame or Scene generation.
    RenderView(cache);
    ASSERT_NE(cache.geometry, nullptr);
    const auto earlier = cache.geometry;
    auto& hookOwner = m_scene.CreateGameObject("Unregistered native render hook");
    auto& writer = AttachNative(hookOwner);
    hookOwner.GetComponent<scene::ScriptComponent>()->scripts.front().m_pendingRemoval = true;
    scene::EntityID addedId;
    writer.afterFirstView = [&]() {
        firstOwner.transform.position = firstOwner.transform.worldPosition = {10, 0, 7};
        hidden.SetActive(false);
        removed.RemoveComponent<scene::MeshRenderer>();
        auto* material = asset::AssetManager::Get<asset::MaterialAsset>(m_asset);
        ASSERT_NE(material, nullptr);
        EditMaterialAuthoring(*material, [&](auto& values) { values.params["roughness"] = {0.92f}; });
        addedId = AddObject(9).GetID();
    };
    RenderView(cache);
    EXPECT_EQ(cache.geometry, nullptr);
    RenderView(cache, {}, 1200, 700, scene::UIRenderTargetView::GameViewport);
    ASSERT_EQ(writer.views.size(), 2u);
    const auto nativeFirst = writer.views[0];
    const auto nativeSecond = writer.views[1];
    const auto* initial = Object(*nativeFirst, firstId);
    const auto* changed = Object(*nativeSecond, firstId);
    ASSERT_NE(initial, nullptr);
    ASSERT_NE(changed, nullptr);
    EXPECT_FLOAT_EQ(initial->sortPosition.x, 0);
    EXPECT_FLOAT_EQ(changed->sortPosition.x, 10);
    EXPECT_NE(Object(*nativeFirst, hiddenId), nullptr);
    EXPECT_EQ(Object(*nativeSecond, hiddenId), nullptr);
    EXPECT_NE(Object(*nativeFirst, removedId), nullptr);
    EXPECT_EQ(Object(*nativeSecond, removedId), nullptr);
    EXPECT_EQ(Object(*nativeFirst, addedId), nullptr);
    EXPECT_NE(Object(*nativeSecond, addedId), nullptr);
    EXPECT_FLOAT_EQ(Roughness(nativeFirst->items[initial->firstItem].material), 0.65f);
    EXPECT_FLOAT_EQ(Roughness(nativeSecond->items[changed->firstItem].material), 0.92f);
    EXPECT_EQ(earlier->Objects().size(), 3u);
    ASSERT_NE(Object(*earlier, firstId), nullptr);
    EXPECT_FLOAT_EQ(Object(*earlier, firstId)->sortPosition.x, 0);

    /// @note Removing the last script permits sharing again, but must never resurrect the pre-writer pose.
    hookOwner.RemoveComponent<scene::ScriptComponent>();
    RenderView(cache);
    ASSERT_NE(cache.geometry, nullptr);
    EXPECT_NE(cache.geometry, earlier);
    ASSERT_NE(Object(*cache.geometry, firstId), nullptr);
    EXPECT_FLOAT_EQ(Object(*cache.geometry, firstId)->sortPosition.x, 10);
    EXPECT_EQ(Object(*cache.geometry, hiddenId), nullptr);
    EXPECT_EQ(Object(*cache.geometry, removedId), nullptr);
    EXPECT_NE(Object(*cache.geometry, addedId), nullptr);
    const auto afterRemoval = cache.geometry;
    RenderView(cache);
    EXPECT_EQ(cache.geometry, afterRemoval);
    EXPECT_FLOAT_EQ(initial->sortPosition.x, 0);
    EXPECT_FLOAT_EQ(changed->sortPosition.x, 10);
}

TEST_F(ManagedFrameGeometryTest, RejectsAnyScriptPresenceAndProtectsAllCanvasGeometryDescendants)
{
    auto& mesh = AddObject(2);
    ASSERT_NE(AttachManaged(mesh), nullptr);
    scene::RenderFrameGeometryCache cache;
    RenderView(cache);
    EXPECT_EQ(cache.geometry, nullptr);
    mesh.RemoveComponent<scene::ScriptComponent>();
    RenderView(cache);
    ASSERT_NE(cache.geometry, nullptr);
    auto& nativeOwner = m_scene.CreateGameObject("Disabled native hook");
    auto& native = AttachNative(nativeOwner);
    native.enabled = false;
    RenderView(cache);
    EXPECT_EQ(cache.geometry, nullptr);
    native.enabled = true;
    nativeOwner.SetActive(false);
    RenderView(cache);
    EXPECT_EQ(cache.geometry, nullptr);
    EXPECT_TRUE(native.views.empty());
    nativeOwner.SetActive(true);
    RenderView(cache);
    EXPECT_EQ(cache.geometry, nullptr);
    ASSERT_EQ(native.views.size(), 1u);
    native.enabled = false;
    RenderView(cache);
    EXPECT_EQ(cache.geometry, nullptr);
    nativeOwner.RemoveComponent<scene::ScriptComponent>();
    RenderView(cache);
    ASSERT_NE(cache.geometry, nullptr);
    auto& emptyOwner = m_scene.CreateGameObject("Empty ScriptComponent");
    emptyOwner.AddComponent<scene::ScriptComponent>();
    RenderView(cache);
    EXPECT_EQ(cache.geometry, nullptr);
    emptyOwner.RemoveComponent<scene::ScriptComponent>();
    RenderView(cache);
    ASSERT_NE(cache.geometry, nullptr);

    /// @note With all scripts removed, these assertions exercise the Canvas guard rather than script bypass.
    auto& canvasOwner = m_scene.CreateGameObject("Canvas root mesh");
    canvasOwner.AddComponent<scene::UICanvas>();
    canvasOwner.AddComponent<scene::MeshRenderer>().enabled = false;
    RenderView(cache);
    EXPECT_EQ(cache.geometry, nullptr);
    canvasOwner.RemoveComponent<scene::MeshRenderer>();
    auto& intermediate = m_scene.CreateGameObject("Non UI intermediate");
    intermediate.SetParent(canvasOwner);
    intermediate.AddComponent<scene::UICanvas>().enabled = false;
    auto& child = m_scene.CreateGameObject("Inactive skinned descendant");
    child.SetParent(intermediate);
    child.SetActive(false);
    child.AddComponent<scene::SkinnedMeshRenderer>().enabled = false;
    RenderView(cache);
    EXPECT_EQ(cache.geometry, nullptr);
    canvasOwner.GetComponent<scene::UICanvas>()->enabled = false;
    RenderView(cache);
    ASSERT_NE(cache.geometry, nullptr);
    const auto disabledCanvasGeometry = cache.geometry;
    canvasOwner.GetComponent<scene::UICanvas>()->enabled = true;
    canvasOwner.SetActive(false);
    RenderView(cache);
    EXPECT_EQ(cache.geometry, disabledCanvasGeometry);
}

TEST_F(ManagedFrameGeometryTest, RebuildsUiGeometryAfterLayoutRunsForAnOverlayOutsideItsTargetView)
{
    auto& canvasOwner = m_scene.CreateGameObject("Overlay layout");
    auto& canvas = canvasOwner.AddComponent<scene::UICanvas>();
    canvas.scaleMode = scene::UICanvasScaleMode::ScaleWithScreenSize;
    canvas.referenceWidth = 800;
    canvas.referenceHeight = 600;
    canvas.matchWidthOrHeight = 1;
    canvasOwner.AddComponent<scene::UILayoutGroup>().alignMain = scene::UILayoutAlign::End;
    auto& child = AddObject(2);
    child.SetParent(canvasOwner);
    child.transform.scale = {100, 40, 1};
    scene::RenderFrameGeometryCache cache;
    /// @note Seed the former no-Script cache path, then demonstrate its retirement and actual UI writes.
    const auto original = ExtractView({}, &cache);
    ASSERT_NE(cache.geometry, nullptr);
    RenderView(cache, {}, 800, 600, scene::UIRenderTargetView::SceneViewport);
    EXPECT_EQ(cache.geometry, nullptr);
    ASSERT_TRUE(m_ui.whiteTexture.IsValid());
    EXPECT_FLOAT_EQ(child.transform.position.x, 700);
    const auto first = ExtractView({}, nullptr);
    RenderView(cache, {}, 1200, 600, scene::UIRenderTargetView::GameViewport);
    EXPECT_EQ(cache.geometry, nullptr);
    EXPECT_FLOAT_EQ(child.transform.position.x, 1100);
    const auto second = ExtractView({}, nullptr);
    ASSERT_NE(Object(*original, child.GetID()), nullptr);
    ASSERT_NE(Object(*first, child.GetID()), nullptr);
    ASSERT_NE(Object(*second, child.GetID()), nullptr);
    EXPECT_FLOAT_EQ(Object(*original, child.GetID())->sortPosition.x, 0);
    EXPECT_FLOAT_EQ(Object(*first, child.GetID())->sortPosition.x, 700);
    EXPECT_FLOAT_EQ(Object(*second, child.GetID())->sortPosition.x, 1100);
}
TEST_F(ManagedFrameGeometryTest, RebuildsColorEligibilityForDirectMaterialEnabledChangesAndComponentLifetime)
{
    auto& owner = AddObject(2);
    owner.GetComponent<scene::MaterialComponent>()->enabled = false;
    scene::RenderFrameGeometryCache cache;
    RenderView(cache);
    const auto disabled = ExtractView({}, &cache);
    ASSERT_NE(cache.geometry, nullptr);
    const auto disabledGeometry = cache.geometry;
    ASSERT_EQ(disabled->Objects().size(), 1u);
    ASSERT_EQ(disabled->items.size(), 1u);
    EXPECT_FALSE(disabled->Objects()[0].colorEligible);
    EXPECT_FALSE(disabled->items[0].visible);
    EXPECT_FALSE(disabled->items[0].material.valid);

    /// @note Public component and asset writes between views require no scene generation notification.
    auto* material = asset::AssetManager::Get<asset::MaterialAsset>(m_asset);
    ASSERT_NE(material, nullptr);
    EditMaterialAuthoring(*material, [&](auto& values) { values.params["roughness"] = {0.87f}; });
    owner.GetComponent<scene::MaterialComponent>()->enabled = true;
    RenderView(cache, {}, 1200, 700, scene::UIRenderTargetView::GameViewport);
    EXPECT_NE(cache.geometry, disabledGeometry);
    const auto enabled = ExtractView({}, &cache);
    const auto enabledGeometry = cache.geometry;
    ASSERT_EQ(enabled->Objects().size(), 1u);
    ASSERT_EQ(enabled->items.size(), 1u);
    EXPECT_NE(enabled->sharedObjects, disabled->sharedObjects);
    EXPECT_TRUE(enabled->Objects()[0].colorEligible);
    EXPECT_TRUE(enabled->items[0].visible);
    ASSERT_TRUE(enabled->items[0].material.valid);
    EXPECT_FLOAT_EQ(Roughness(enabled->items[0].material), 0.87f);
    EXPECT_EQ(enabled->frameStamp, disabled->frameStamp);
    EXPECT_FLOAT_EQ(enabled->Objects()[0].world.m[2][3], disabled->Objects()[0].world.m[2][3]);
    EXPECT_FALSE(disabled->Objects()[0].colorEligible);
    EXPECT_FALSE(disabled->items[0].visible);
    EXPECT_FALSE(disabled->items[0].material.valid);
    RenderView(cache);
    EXPECT_EQ(cache.geometry, enabledGeometry);

    owner.GetComponent<scene::MaterialComponent>()->enabled = false;
    RenderView(cache);
    const auto disabledAgain = ExtractView({}, &cache);
    EXPECT_NE(cache.geometry, enabledGeometry);
    ASSERT_EQ(disabledAgain->Objects().size(), 1u);
    ASSERT_EQ(disabledAgain->items.size(), 1u);
    EXPECT_FALSE(disabledAgain->Objects()[0].colorEligible);
    EXPECT_FALSE(disabledAgain->items[0].visible);
    EXPECT_FALSE(disabledAgain->items[0].material.valid);
    EXPECT_TRUE(enabled->Objects()[0].colorEligible);
    EXPECT_TRUE(enabled->items[0].visible);
    EXPECT_FLOAT_EQ(Roughness(enabled->items[0].material), 0.87f);

    /// @note Exercise the public extractor directly as well: material lifetime changes affect eligibility,
    /// @note while the caller keeps geometry stable. Missing material remains an eligible unassigned surface.
    const auto disabledAgainGeometry = cache.geometry;
    owner.RemoveComponent<scene::MaterialComponent>();
    const auto missing = ExtractView({}, &cache);
    EXPECT_NE(cache.geometry, disabledAgainGeometry);
    ASSERT_EQ(missing->Objects().size(), 1u);
    ASSERT_EQ(missing->items.size(), 1u);
    EXPECT_TRUE(missing->Objects()[0].colorEligible);
    EXPECT_TRUE(missing->items[0].visible);
    EXPECT_EQ(owner.GetComponent<scene::MaterialComponent>(), nullptr);
    EXPECT_FALSE(disabledAgain->Objects()[0].colorEligible);
    const auto missingGeometry = cache.geometry;
    auto& added = owner.AddComponent<scene::MaterialComponent>();
    added.SetMaterialAsset(m_asset);
    added.enabled = false;
    const auto addedDisabled = ExtractView({}, &cache);
    EXPECT_NE(cache.geometry, missingGeometry);
    ASSERT_EQ(addedDisabled->Objects().size(), 1u);
    ASSERT_EQ(addedDisabled->items.size(), 1u);
    EXPECT_FALSE(addedDisabled->Objects()[0].colorEligible);
    EXPECT_FALSE(addedDisabled->items[0].visible);
    EXPECT_TRUE(missing->Objects()[0].colorEligible);
    const auto addedDisabledGeometry = cache.geometry;
    owner.GetComponent<scene::MaterialComponent>()->enabled = true;
    const auto directEnabled = ExtractView({}, &cache);
    EXPECT_NE(cache.geometry, addedDisabledGeometry);
    ASSERT_EQ(directEnabled->Objects().size(), 1u);
    ASSERT_EQ(directEnabled->items.size(), 1u);
    EXPECT_TRUE(directEnabled->Objects()[0].colorEligible);
    EXPECT_TRUE(directEnabled->items[0].visible);
    ASSERT_TRUE(directEnabled->items[0].material.valid);
    EXPECT_FLOAT_EQ(Roughness(directEnabled->items[0].material), 0.87f);
    EXPECT_FALSE(addedDisabled->Objects()[0].colorEligible);
}
TEST_F(MaterialExtractionSharingTest, SharesDefaultsOnceAndReevaluatesMaterialEditsInTheNextView)
{
    for (int i = 0; i < 8; ++i) AddObject(static_cast<float>(i + 2));

    const auto first = Extract();

    ASSERT_EQ(first->items.size(), 8u);
    EXPECT_EQ(m_backend.constantBufferUpdates, 1u);
    for (const auto& item : first->items) {
        EXPECT_TRUE(item.material.valid);
        EXPECT_EQ(item.material.paramsBuffer, first->items[0].material.paramsBuffer);
        EXPECT_EQ(item.forwardMaterial.gbufferParams, item.material.gbufferParams);
        EXPECT_FLOAT_EQ(Roughness(item.material), 0.65f);
    }
    auto* material = asset::AssetManager::Get<asset::MaterialAsset>(m_asset);
    ASSERT_NE(material, nullptr);
    EditMaterialAuthoring(*material, [&](auto& values) { values.params["roughness"] = {0.73f}; });

    const auto second = Extract();

    EXPECT_EQ(m_backend.constantBufferUpdates, 2u);
    for (const auto& item : second->items) EXPECT_FLOAT_EQ(Roughness(item.material), 0.73f);
    EXPECT_FLOAT_EQ(Roughness(first->items[0].material), 0.65f);
}

TEST_F(MaterialExtractionSharingTest, OverridesAndPresentationRemainIndependentOfSharedDefaults)
{
    AddObject(); AddObject();
    auto& tint = *AddObject().GetComponent<scene::MaterialComponent>();
    EditMaterialAuthoring(tint, [&](auto& values) { values.paramOverrides["roughness"] = {0.1f}; });
    auto& integer = *AddObject().GetComponent<scene::MaterialComponent>();
    EditMaterialAuthoring(integer, [&](auto& values) { values.integerParamOverrides["flags"] = {4294967295ll}; });
    EditMaterialAuthoring(*AddObject().GetComponent<scene::MaterialComponent>(), [&](auto& values) { values.textureOverrides["albedo"] = ""; });
    auto& blend = *AddObject().GetComponent<scene::MaterialComponent>();
    EditMaterialAuthoring(blend, [&](auto& values) { values.hasBlendModeOverride = true; }); EditMaterialAuthoring(blend, [&](auto& values) { values.blendModeOverride = r::BlendMode::ALPHA_BLEND; });
    auto& sided = *AddObject().GetComponent<scene::MaterialComponent>();
    EditMaterialAuthoring(sided, [&](auto& values) { values.hasDoubleSidedOverride = true; }); EditMaterialAuthoring(sided, [&](auto& values) { values.doubleSidedOverride = true; });
    auto& queue = *AddObject().GetComponent<scene::MaterialComponent>();
    EditMaterialAuthoring(queue, [&](auto& values) { values.hasRenderQueueOverride = true; }); EditMaterialAuthoring(queue, [&](auto& values) { values.renderQueueOverride = 2021; });
    auto& presentation = *AddObject().GetComponent<scene::MaterialComponent>();
    EditMaterialAuthoring(presentation, [&](auto& values) { values.hasPresentationDefaults = true; });
    EditMaterialAuthoring(presentation, [&](auto& values) { values.presentationParamDefaults["roughness"] = {0.2f}; });

    const auto output = Extract();

    ASSERT_EQ(output->items.size(), 9u);
    EXPECT_EQ(m_backend.constantBufferUpdates, 8u);
    EXPECT_EQ(output->items[0].material.paramsBuffer, output->items[1].material.paramsBuffer);
    for (size_t i = 2; i < output->items.size(); ++i)
        EXPECT_NE(output->items[i].material.paramsBuffer, output->items[0].material.paramsBuffer);
    EXPECT_FLOAT_EQ(Roughness(output->items[2].material), 0.1f);
    EXPECT_EQ(output->items[5].material.capabilities.blend, r::BlendMode::ALPHA_BLEND);
    EXPECT_TRUE(output->items[6].material.doubleSided);
    EXPECT_EQ(output->items[7].material.renderQueue, 2021);
    EXPECT_FLOAT_EQ(Roughness(output->items[8].material), 0.2f);
    ASSERT_NE(integer.material, nullptr);
    uint32_t flags = 0;
    std::memcpy(&flags, integer.material->paramData.data() + 88, sizeof(flags));
    EXPECT_EQ(flags, UINT32_MAX);
}

TEST_F(MaterialExtractionSharingTest, PackingPlanKeepsDistinctOverridesBeyondTheVariantLimitAcrossReloadAndReset)
{
    std::vector<scene::EntityID> owners;
    for (uint32_t index = 0; index < 40; ++index) {
        auto& object = AddObject();
        owners.push_back(object.GetID());
        EditMaterialAuthoring(*object.GetComponent<scene::MaterialComponent>(), [&](auto& values) {
            values.paramOverrides["roughness"] = {static_cast<float>(index) / 64.0f};
            values.paramOverrides["albedo"] = {static_cast<float>(index) / 64.0f, 0.25f, 0.5f, 1};
            values.paramOverrides["uvTiling"] = {2, 3};
            values.paramOverrides["alphaCutoff"] = {0.125f};
        });
    }
    const auto verify = [&](const std::shared_ptr<const r::RenderScene>& output, bool edited) {
        ASSERT_EQ(output->items.size(), owners.size());
        for (size_t index = 0; index < owners.size(); ++index) {
            const auto& material = output->items[index].material;
            ASSERT_TRUE(material.valid);
            EXPECT_FLOAT_EQ(Roughness(material), edited && index == 0 ? 0.875f : static_cast<float>(index) / 64.0f);
            const auto value = [&](size_t offset) {
                float result = 0;
                std::memcpy(&result, material.gbufferParams.data() + offset, sizeof(result));
                return result;
            };
            EXPECT_FLOAT_EQ(value(0), static_cast<float>(index) / 64.0f);
            EXPECT_FLOAT_EQ(value(48), 2);
            EXPECT_FLOAT_EQ(value(52), 3);
            EXPECT_FLOAT_EQ(value(64), 0.125f);
        }
    };
    const auto first = Extract();
    ASSERT_NO_FATAL_FAILURE(verify(first, false));
    const auto shader = first->items[0].material.shader;
    m_backend.shaderSize = 160;
    for (auto& variable : m_backend.shaderVars) {
        if (variable.name == "albedo") variable.offset = 112;
        if (variable.name == "metallic") variable.offset = 96;
        if (variable.name == "roughness") variable.offset = 100;
    }
    ASSERT_TRUE(m_resources->ReloadShader(ShaderPath("Shared.hlsl")));
    auto* firstMaterial = m_scene.GetComponent<scene::MaterialComponent>(owners[0]);
    ASSERT_NE(firstMaterial, nullptr);
    EditMaterialAuthoring(*firstMaterial, [&](auto& values) { values.paramOverrides["roughness"] = {0.875f}; });
    const auto reloaded = Extract();
    ASSERT_NO_FATAL_FAILURE(verify(reloaded, true));
    EXPECT_EQ(reloaded->items[0].material.shader, shader);
    EXPECT_FALSE(reloaded->items[0].material.directGBufferParams);
    EXPECT_FLOAT_EQ(Roughness(first->items[0].material), 0);
    m_resources->Reset();
    ASSERT_NO_FATAL_FAILURE(verify(Extract(), true));
}

TEST_F(MaterialExtractionSharingTest, PackingPlanRechecksInPlaceNativeTypeChangesAndSuccessfulReload)
{
    m_backend.shaderVars.push_back({"normalStrength", 68, 4, 1, 1, r::ShaderVarClass::Scalar, r::ShaderVarType::UInt});
    auto& object = AddObject();
    EditMaterialAuthoring(*object.GetComponent<scene::MaterialComponent>(), [&](auto& values) {
        values.paramOverrides["normalStrength"] = {0.75f};
        values.paramOverrides["roughness"] = {0.25f};
    });
    const auto authoring = asset::ReadMaterialAuthoring(m_asset);
    ASSERT_TRUE(authoring);
    /// @note Material import can change reference spelling; mutate the exact shader resource key used by extraction.
    const std::string shaderReference = authoring->Data().shaderPath;
    const auto shader = m_resources->LoadShader(shaderReference);
    ASSERT_TRUE(shader.IsValid());
    ASSERT_NE(m_backend.lastCreatedShader, nullptr);
    auto* native = m_backend.lastCreatedShader;
    const auto* descriptor = &native->GetDescriptor();
    const auto shaderVersion = m_resources->GetShaderVersion();
    bool changed = false;
    m_backend.beforeConstantBufferCreate = [&] {
        auto variables = m_backend.shaderVars;
        variables.back().varType = r::ShaderVarType::Float;
        native->SetVariables(variables);
        changed = true;
    };
    const auto output = Extract();
    ASSERT_TRUE(changed);
    EXPECT_EQ(&native->GetDescriptor(), descriptor);
    EXPECT_EQ(m_resources->GetShaderVersion(), shaderVersion);
    ASSERT_EQ(output->items.size(), 1u);
    ASSERT_TRUE(output->items[0].material.valid);
    /// @note The bounded synchronization retry derives current bytes; packing must reject its earlier UInt copy plan.
    EXPECT_EQ(output->items[0].material.shader, shader);
    EXPECT_FLOAT_EQ(Roughness(output->items[0].material), 0.25f);
    float strength = 0;
    std::memcpy(&strength, output->items[0].material.gbufferParams.data() + 24, sizeof(strength));
    EXPECT_FLOAT_EQ(strength, 0.75f);

    m_backend.shaderVars.back().varType = r::ShaderVarType::Float;
    ASSERT_TRUE(m_resources->ReloadShader(shaderReference));
    EXPECT_GT(m_resources->GetShaderVersion(), shaderVersion);
    const auto recovered = Extract();
    ASSERT_EQ(recovered->items.size(), 1u);
    ASSERT_TRUE(recovered->items[0].material.valid);
    EXPECT_EQ(recovered->items[0].material.shader, shader);
    EXPECT_FLOAT_EQ(Roughness(recovered->items[0].material), 0.25f);
    std::memcpy(&strength, recovered->items[0].material.gbufferParams.data() + 24, sizeof(strength));
    EXPECT_FLOAT_EQ(strength, 0.75f);
    EXPECT_EQ(output->items[0].material.shader, shader);
}

TEST_F(MaterialExtractionSharingTest, MaterialStateResolvesLazyAssetsAndKeepsMissingComponentOrdinalsIndependent)
{
    AddObject().RemoveComponent<scene::MaterialComponent>();
    asset::MaterialAssetAuthoring values;
    values.shaderPath = ShaderPath("Shared.hlsl");
    values.blendMode = r::BlendMode::ALPHA_BLEND;
    values.renderQueue = 2073;
    values.doubleSided = true;
    values.depthBias = -7;
    values.depthBiasSlope = 1.25f;
    values.params["clearcoat"] = {0.4f};
    const auto path = m_temp.File("LazyState.mat").generic_string();
    ASSERT_TRUE(asset::SaveMaterialAssetToFile(path, values));
    auto& lazy = *AddObject().GetComponent<scene::MaterialComponent>();
    lazy.SetMaterialPath(path);
    EXPECT_FALSE(lazy.GetMaterialAsset().IsValid());
    auto& presentation = *AddObject().GetComponent<scene::MaterialComponent>();
    presentation.visible = false;
    EditMaterialAuthoring(presentation, [&](auto& state) {
        state.hasPresentationDefaults = true;
        state.presentationRenderQueue = 3051;
        state.presentationDoubleSided = true;
        state.hasRenderQueueOverride = true;
        state.renderQueueOverride = 2019;
        state.hasDoubleSidedOverride = true;
        state.doubleSidedOverride = false;
    });
    auto& missing = *AddObject().GetComponent<scene::MaterialComponent>();
    missing.SetMaterialReference("", {});
    missing.visible = false;
    EditMaterialAuthoring(missing, [&](auto& state) {
        state.hasBlendModeOverride = true;
        state.blendModeOverride = r::BlendMode::ALPHA_BLEND;
        state.hasPresentationDefaults = true;
        state.presentationRenderQueue = 3037;
    });

    const auto output = Extract();

    ASSERT_EQ(output->items.size(), 4u);
    EXPECT_TRUE(output->items[0].material.valid);
    EXPECT_TRUE(output->items[0].material.doubleSided);
    const auto& loaded = output->items[1].material;
    EXPECT_TRUE(lazy.GetMaterialAsset().IsValid());
    EXPECT_TRUE(loaded.valid);
    EXPECT_EQ(loaded.capabilities.blend, r::BlendMode::ALPHA_BLEND);
    EXPECT_TRUE(loaded.capabilities.advancedLobe);
    EXPECT_EQ(loaded.renderQueue, 2073);
    EXPECT_TRUE(loaded.doubleSided);
    EXPECT_EQ(loaded.depthBias, -7);
    EXPECT_FLOAT_EQ(loaded.depthBiasSlope, 1.25f);
    EXPECT_EQ(output->items[2].material.renderQueue, 2019);
    EXPECT_FALSE(output->items[2].material.doubleSided);
    EXPECT_EQ(output->items[3].material.capabilities.blend, r::BlendMode::ALPHA_BLEND);
    EXPECT_FALSE(output->items[3].material.capabilities.gbufferEquivalentShader);
    EXPECT_EQ(output->items[3].material.renderQueue, 3037);
    EXPECT_TRUE(output->items[3].material.doubleSided);
    EXPECT_EQ(output->items[3].material.depthBias, 0);
    EXPECT_FLOAT_EQ(output->items[3].material.depthBiasSlope, 0.0f);
}

TEST_F(MaterialExtractionSharingTest, MaterialStateRejectsMovedSlotEditsAndSameHandleReplacementDuringEarlierSlotSynchronization)
{
    AddObject();
    asset::Model model;
    for (int i = 0; i < 2; ++i) {
        model.meshes.push_back(std::make_unique<r::Mesh>());
        model.meshes.back()->indexCount = 3;
    }
    auto& owner = m_scene.CreateGameObject("Moving material slots");
    scene::SkinnedMeshRenderer skinned;
    skinned.model = &model;
    owner.AddComponent<scene::SkinnedMeshRenderer>(skinned);
    auto& component = owner.AddComponent<scene::MaterialComponent>();
    component.SetMaterialAsset(m_asset);
    component.ResizeSlots(2);
    component.RawSlotAt(0).visible = false;
    component.RawSlotAt(1).SetMaterialReference("Selected.mat", m_asset);
    component.RawSlotAt(1).visible = false;
    const auto oldSlot = component.RawSlotAt(1).ReadAuthoring();
    const auto oldAsset = asset::ReadMaterialAuthoring(m_asset);
    bool changed = false;
    m_backend.beforeConstantBufferCreate = [&] {
        EditMaterialAuthoring(component.RawSlotAt(1), [&](auto& values) {
            values.paramOverrides["clearcoat"] = {0};
            values.hasBlendModeOverride = true;
            values.blendModeOverride = r::BlendMode::OPAQUE_BLEND;
            values.hasRenderQueueOverride = true;
            values.renderQueueOverride = 2081;
            values.hasDoubleSidedOverride = true;
            values.doubleSidedOverride = false;
        });
        component.extraSlots.reserve(component.extraSlots.capacity() + 8);
        auto replacement = oldAsset->Data();
        replacement.params["clearcoat"] = {0.6f};
        replacement.blendMode = r::BlendMode::ALPHA_BLEND;
        replacement.renderQueue = 2093;
        replacement.doubleSided = true;
        replacement.depthBias = 9;
        replacement.depthBiasSlope = -0.75f;
        ASSERT_TRUE(asset::SaveMaterialAssetToFile(m_materialPath, replacement));
        ASSERT_EQ(asset::AssetManager::ReloadPath(m_materialPath), 1);
        changed = true;
    };

    const auto output = Extract();

    ASSERT_TRUE(changed);
    ASSERT_EQ(output->items.size(), 3u);
    EXPECT_NE(component.RawSlotAt(1).ReadAuthoring(), oldSlot);
    EXPECT_EQ(asset::AssetManager::Load<asset::MaterialAsset>(m_materialPath), m_asset);
    EXPECT_NE(asset::ReadMaterialAuthoring(m_asset), oldAsset);
    const auto& assetState = output->items[1].material;
    EXPECT_TRUE(assetState.capabilities.advancedLobe);
    EXPECT_EQ(assetState.capabilities.blend, r::BlendMode::ALPHA_BLEND);
    EXPECT_EQ(assetState.renderQueue, 2093);
    EXPECT_TRUE(assetState.doubleSided);
    const auto& slotState = output->items[2].material;
    EXPECT_FALSE(slotState.capabilities.advancedLobe);
    EXPECT_EQ(slotState.capabilities.blend, r::BlendMode::OPAQUE_BLEND);
    EXPECT_EQ(slotState.renderQueue, 2081);
    EXPECT_FALSE(slotState.doubleSided);
    EXPECT_EQ(slotState.depthBias, 9);
    EXPECT_FLOAT_EQ(slotState.depthBiasSlope, -0.75f);
    EXPECT_EQ(assetState.depthBias, 9);
    EXPECT_FLOAT_EQ(assetState.depthBiasSlope, -0.75f);
}

TEST_F(MaterialExtractionSharingTest, MaterialStateRechecksShaderCapabilitiesAfterNativeReloadWithinTheExtraction)
{
    AddObject();
    auto& hidden = *AddObject().GetComponent<scene::MaterialComponent>();
    hidden.visible = false;
    bool reloaded = false;
    m_backend.beforeConstantBufferCreate = [&] {
        ASSERT_TRUE(WriteShader("Shared.hlsl", kLambertShaderDeclaration));
        ASSERT_TRUE(m_resources->ReloadAllShaders());
        reloaded = true;
    };

    const auto output = Extract();

    ASSERT_TRUE(reloaded);
    ASSERT_EQ(output->items.size(), 2u);
    EXPECT_FALSE(output->items[1].visible);
    EXPECT_FALSE(output->items[1].material.capabilities.gbufferEquivalentShader);
    EXPECT_FALSE(output->items[1].material.valid);
}

TEST_F(MaterialExtractionSharingTest, KeepsProjectedTextureRequestsAndSkinContractsDistinct)
{
    auto* material = asset::AssetManager::Get<asset::MaterialAsset>(m_asset);
    ASSERT_NE(material, nullptr);
    EditMaterialAuthoring(*material, [&](auto& values) { values.textures["albedo"] = m_temp.File("Pending.png").generic_string(); });
    auto& first = AddObject(2);
    AddObject(20);
    AddObject(2);
    asset::Model model;
    model.meshes.push_back(std::make_unique<r::Mesh>());
    model.meshes[0]->indexCount = 3;
    scene::SkinnedMeshRenderer skinned;
    skinned.model = &model;
    first.AddComponent<scene::SkinnedMeshRenderer>(skinned);

    const auto output = Extract();

    ASSERT_EQ(output->items.size(), 4u);
    EXPECT_EQ(m_backend.constantBufferUpdates, 3u);
    EXPECT_NE(output->items[0].material.paramsBuffer, output->items[2].material.paramsBuffer);
    EXPECT_NE(output->items[0].material.paramsBuffer, output->items[3].material.paramsBuffer);
    ASSERT_NE(m_resources->Get(output->items[1].material.shader), nullptr);
    EXPECT_EQ(m_resources->Get(output->items[1].material.shader)->GetPath(), r::ERROR_SKINNED_SHADER);
}

TEST_F(MaterialExtractionSharingTest, RayInputsKeepPerObjectSynchronizationAndCanonicalSurfaceValues)
{
    AddObject(); AddObject();

    const auto output = Extract(true);

    ASSERT_EQ(output->items.size(), 2u);
    EXPECT_EQ(m_backend.constantBufferUpdates, 2u);
    EXPECT_NE(output->items[0].material.paramsBuffer, output->items[1].material.paramsBuffer);
    EXPECT_EQ(output->items[0].material.surface, output->items[1].material.surface);
    EXPECT_FLOAT_EQ(output->items[0].material.surface.roughness, 0.65f);
}

TEST_F(MaterialExtractionSharingTest, ShaderReloadAndResourceResetCannotReusePreviousExtractionBindings)
{
    AddObject(); AddObject();
    const auto first = Extract();
    m_backend.shaderSize = 112;
    m_backend.shaderVars[2].offset = 100;
    ASSERT_TRUE(m_resources->ReloadAllShaders());

    const auto reloaded = Extract();

    EXPECT_EQ(m_backend.constantBufferUpdates, 2u);
    EXPECT_FLOAT_EQ(Roughness(reloaded->items[0].material), 0.65f);
    m_resources->Reset();

    const auto reset = Extract();

    EXPECT_EQ(m_backend.constantBufferUpdates, 3u);
    ASSERT_NE(m_resources->Get(reset->items[0].material.paramsBuffer), nullptr);
    EXPECT_FLOAT_EQ(Roughness(first->items[0].material), 0.65f);
    EXPECT_FLOAT_EQ(Roughness(reset->items[0].material), 0.65f);
}

TEST_F(MaterialExtractionSharingTest, ShaderCapabilitiesReconsiderNonGuidProjectPriorityAfterRawFallbackWasPrepared)
{
    const auto rawRoot = m_temp.File("RawFallback");
    ASSERT_TRUE(util::FileSystem::EnsureDirectory(rawRoot));
    std::error_code error;
    const auto previousDirectory = std::filesystem::current_path(error);
    ASSERT_FALSE(error);
    struct RestoreDirectory {
        std::filesystem::path path;
        ~RestoreDirectory() {
            std::error_code restoreError;
            std::filesystem::current_path(path, restoreError);
            EXPECT_FALSE(restoreError);
        }
    } restore{previousDirectory};
    std::filesystem::current_path(rawRoot, error);
    ASSERT_FALSE(error);
    const std::string reference = "PreparedReferenceFallbackFixture.hlsl";
    const auto rawPath = rawRoot / reference;
    const auto projectPath = m_temp.File(reference);
    const auto writeShader = [](const std::filesystem::path& path, std::string_view declaration) {
        const std::string source = util::FileSystem::PathToUtf8(path);
        return util::FileSystem::WriteText(source, "float4 PSMain() : SV_Target { return 1; }\n")
            && util::FileSystem::WriteText(source + ".meta", "[shader]\n" + std::string(declaration));
    };
    ASSERT_TRUE(writeShader(rawPath, kSurfaceShaderDeclaration));
    ASSERT_EQ(asset::AssetManager::ResolveAssetPath(reference), reference);
    EXPECT_TRUE(asset::ResolveShaderCapabilities(reference).SupportsGBuffer());
    EXPECT_TRUE(asset::ResolveShaderCapabilities(reference).SupportsGBuffer());

    ASSERT_TRUE(writeShader(projectPath, kLambertShaderDeclaration));
    EXPECT_EQ(asset::AssetManager::ResolveAssetPath(reference), projectPath.generic_string());
    const auto project = asset::ResolveShaderCapabilities(reference);
    EXPECT_EQ(project.surface, r::ShaderSurfaceContract::LAMBERT);
    EXPECT_FALSE(project.SupportsGBuffer());
    ASSERT_TRUE(std::filesystem::remove(projectPath, error));
    ASSERT_FALSE(error);
    EXPECT_TRUE(asset::ResolveShaderCapabilities(reference).SupportsGBuffer());
    ASSERT_TRUE(writeShader(projectPath, kLambertShaderDeclaration));
    EXPECT_EQ(asset::ResolveShaderCapabilities(reference).surface, r::ShaderSurfaceContract::LAMBERT);
}

TEST_F(MaterialExtractionSharingTest, InstancePartsShareMaterialBindingsAndPreservePlacementAndLodIdentity)
{
    auto* materialAsset = asset::AssetManager::Get<asset::MaterialAsset>(m_asset);
    ASSERT_NE(materialAsset, nullptr);
    EditMaterialAuthoring(*materialAsset, [&](auto& values) { values.textures["albedo"] = m_temp.File("Pending.png").generic_string(); });
    const auto file = m_temp.File("Instances.fzdata").generic_string();
    ASSERT_TRUE(asset::DataAssetRegistry::Create(file, asset::InstanceSetAsset::TYPE_NAME));
    auto* base = asset::DataAssetRegistry::Resolve(file);
    ASSERT_NE(base, nullptr);
    ASSERT_EQ(std::string_view(base->GetTypeName()), asset::InstanceSetAsset::TYPE_NAME);
    auto* definition = static_cast<asset::InstanceSetAsset*>(base);
    const std::array<asset::InstanceMeshPart, 2> parts{{{"primitive:cube", m_materialPath, 0}, {"primitive:cube", m_materialPath, 1}}};
    const std::array<float, 2> heights{0.2f, 0.0f};
    const std::array<r::InstancePlacement, 1> placements{{{7, {1, 2, 3}, {}, {1, 1, 1}, true}}};
    ASSERT_TRUE(definition->Replace(parts, heights, placements));
    for (int i = 0; i < 4; ++i) {
        auto& go = m_scene.CreateGameObject("Instance batch");
        auto& component = go.AddComponent<scene::InstancedMeshComponent>();
        component.m_instanceSetRef.path = file;
        component.m_maxDistance = 90 + static_cast<float>(i);
    }

    const auto output = Extract();

    ASSERT_EQ(output->instances.size(), 4u);
    EXPECT_EQ(m_backend.constantBufferUpdates, 1u);
    const auto buffer = output->instances[0].parts[0].mesh.material.paramsBuffer;
    for (size_t i = 0; i < output->instances.size(); ++i) {
        const auto& batch = output->instances[i];
        EXPECT_EQ(batch.data, definition->Snapshot());
        EXPECT_FLOAT_EQ(batch.maxDistance, 90 + static_cast<float>(i));
        ASSERT_EQ(batch.parts.size(), 2u);
        EXPECT_EQ(batch.parts[0].lod, 0u); EXPECT_EQ(batch.parts[1].lod, 1u);
        EXPECT_EQ(batch.parts[0].mesh.material.paramsBuffer, buffer);
        EXPECT_EQ(batch.parts[1].mesh.forwardMaterial.paramsBuffer, buffer);
    }
    auto* material = asset::AssetManager::Get<asset::MaterialAsset>(m_asset);
    ASSERT_NE(material, nullptr);
    EditMaterialAuthoring(*material, [&](auto& values) { values.params["roughness"] = {0.8f}; });

    const auto changed = Extract();

    EXPECT_EQ(m_backend.constantBufferUpdates, 2u);
    EXPECT_FLOAT_EQ(Roughness(changed->instances[3].parts[1].mesh.material), 0.8f);
    EXPECT_FLOAT_EQ(Roughness(output->instances[0].parts[0].mesh.material), 0.65f);
}

TEST_F(MaterialExtractionSharingTest, PaintPreviewIsViewLocalAndLeavesSceneAndSharedGeometryUnchanged)
{
    const auto file = m_temp.File("Preview.fzdata").generic_string();
    ASSERT_TRUE(asset::DataAssetRegistry::Create(file, asset::InstanceSetAsset::TYPE_NAME));
    auto* definition = static_cast<asset::InstanceSetAsset*>(asset::DataAssetRegistry::Resolve(file));
    ASSERT_NE(definition, nullptr);
    const std::array<asset::InstanceMeshPart, 1> parts{{{"primitive:cube", m_materialPath, 0}}};
    const std::array<float, 1> heights{0.0f};
    ASSERT_TRUE(definition->Replace(parts, heights, {}));
    auto data = std::make_shared<r::InstanceSetData>();
    data->placements.push_back({9, {1, 2, 3}, {0, 0.3f, 0}, {2, 2, 2}, true});
    const std::array<scene::RenderInstancePreview, 1> previews{{{file, data, math::Matrix4::Identity(), 71, true}}};
    scene::RenderSystemUIOptions ui; ui.instancePreviews = previews;
    r::Camera camera; r::RenderSettings settings; r::RenderPassHandles handles;
    scene::RenderPassContext context(m_scene, m_backend, *m_resources, camera, settings, {}, UINT32_MAX, handles);
    scene::RenderFrameGeometryCache cache;
    context.uiOptions = &ui; scene::ExtractRenderScene(context, &cache);
    ASSERT_EQ(context.renderScene->instances.size(), 1u);
    EXPECT_EQ(context.renderScene->instances[0].data, data);
    EXPECT_TRUE(context.renderScene->instances[0].selectionOnly);
    EXPECT_TRUE(context.renderScene->instances[0].selected);
    EXPECT_FALSE(context.renderScene->instances[0].castShadows);
    ASSERT_EQ(context.renderScene->instances[0].parts.size(), 1u);
    bool empty = true;
    for (const auto& object : m_scene.GameObjects()) { (void)object; empty = false; }
    EXPECT_TRUE(empty);
    context.uiOptions = nullptr; scene::ExtractRenderScene(context, &cache);
    EXPECT_TRUE(context.renderScene->instances.empty());
}

TEST_F(MaterialExtractionSharingTest, LeafWindParametersPackIntoTheStandardPrefixAndDefaultToRigid)
{
    m_backend.shaderVars.push_back({"leafWindStrength", 68, 4, 1, 1, r::ShaderVarClass::Scalar, r::ShaderVarType::Float});
    m_backend.shaderVars.push_back({"leafWindFrequency", 72, 4, 1, 1, r::ShaderVarClass::Scalar, r::ShaderVarType::Float});
    m_backend.shaderVars.push_back({"leafWindRootHeight", 76, 4, 1, 1, r::ShaderVarClass::Scalar, r::ShaderVarType::Float});
    auto* material = asset::AssetManager::Get<asset::MaterialAsset>(m_asset);
    ASSERT_NE(material, nullptr);
    EditMaterialAuthoring(*material, [](auto& values) {
        values.params["leafWindStrength"] = {0.15f}; values.params["leafWindFrequency"] = {1.8f}; values.params["leafWindRootHeight"] = {1.6f};
    });
    AddObject();
    const auto output = Extract(); ASSERT_EQ(output->items.size(), 1u);
    const auto scalar = [](const auto& value, size_t offset) { float result = 0; std::memcpy(&result, value.gbufferParams.data() + offset, 4); return result; };
    EXPECT_FLOAT_EQ(scalar(output->items[0].material, 68), 0.15f);
    EXPECT_FLOAT_EQ(scalar(output->items[0].material, 72), 1.8f);
    EXPECT_FLOAT_EQ(scalar(output->items[0].material, 76), 1.6f);
    EditMaterialAuthoring(*material, [](auto& values) { values.params["leafWindStrength"] = {0.0f}; });
    EXPECT_FLOAT_EQ(scalar(Extract()->items[0].material, 68), 0.0f);
}

TEST_F(MaterialExtractionSharingTest, RetainedCpuTemplatesRestoreRuntimeBytesAndRejectSameHandleAssetReplacement)
{
    auto& component = *AddObject().GetComponent<scene::MaterialComponent>();
    const auto first = Extract();
    ASSERT_EQ(first->items.size(), 1u);
    ASSERT_NE(component.material, nullptr);
    std::fill(component.material->paramData.begin(), component.material->paramData.end(), uint8_t{0xCD});
    component.material->textures.clear();
    const auto* restored = scene::SyncMaterial(component, *m_resources);
    ASSERT_NE(restored, nullptr);
    float roughness = 0;
    std::memcpy(&roughness, restored->paramData.data() + 20, sizeof(roughness));
    EXPECT_FLOAT_EQ(roughness, 0.65f);
    EXPECT_EQ(restored->textures.size(), 8u);
    const auto oldAuthoring = asset::ReadMaterialAuthoring(m_asset);
    auto replacement = oldAuthoring->Data(); replacement.params["roughness"] = {0.83f};
    ASSERT_TRUE(asset::SaveMaterialAssetToFile(m_materialPath, replacement));
    ASSERT_EQ(asset::AssetManager::ReloadPath(m_materialPath), 1);
    EXPECT_EQ(asset::AssetManager::Load<asset::MaterialAsset>(m_materialPath), m_asset);
    EXPECT_NE(asset::ReadMaterialAuthoring(m_asset)->OwnerToken(), oldAuthoring->OwnerToken());
    const auto changed = Extract();
    EXPECT_FLOAT_EQ(Roughness(changed->items[0].material), 0.83f);
    EXPECT_FLOAT_EQ(Roughness(first->items[0].material), 0.65f);
    EXPECT_TRUE(changed->items[0].material.valid);
    EXPECT_NE(m_resources->Get(changed->items[0].material.paramsBuffer), nullptr);
}

TEST_F(MaterialExtractionSharingTest, LiveFallbackAndSlotRelocationUseTheCurrentSelectedTemplateOwner)
{
    auto& component = *AddObject().GetComponent<scene::MaterialComponent>();
    component.extraSlots.resize(1);
    component.extraSlots[0].SetMaterialReference("extra.mat", m_asset);
    EditMaterialAuthoring(component.extraSlots[0], [&](auto& values) { values.paramOverrides["roughness"] = {0.22f}; });
    const auto readRoughness = [](const r::Material& material) {
        float value = 0; std::memcpy(&value, material.paramData.data() + 20, sizeof(value)); return value;
    };
    const auto* first = scene::SyncMaterialSlot(component, 1, *m_resources);
    ASSERT_NE(first, nullptr); EXPECT_FLOAT_EQ(readRoughness(*first), 0.22f);
    component.extraSlots[0].visible = false;
    EXPECT_EQ(scene::SyncMaterialSlot(component, 1, *m_resources), nullptr);
    component.extraSlots[0].visible = true;
    component.extraSlots[0].SetMaterialPath("");
    const auto* fallback = scene::SyncMaterialSlot(component, 1, *m_resources);
    ASSERT_NE(fallback, nullptr); EXPECT_FLOAT_EQ(readRoughness(*fallback), 0.65f);
    scene::MaterialSlot replacement;
    replacement.SetMaterialReference("replacement.mat", m_asset);
    EditMaterialAuthoring(replacement, [&](auto& values) { values.paramOverrides["roughness"] = {0.91f}; });
    component.extraSlots[0] = replacement;
    component.extraSlots.reserve(component.extraSlots.capacity() + 8);
    const auto* replaced = scene::SyncMaterialSlot(component, 1, *m_resources);
    ASSERT_NE(replaced, nullptr); EXPECT_FLOAT_EQ(readRoughness(*replaced), 0.91f);
    component.enabled = false;
    EXPECT_EQ(scene::SyncMaterialSlot(component, 1, *m_resources), nullptr);
}


TEST_F(MaterialExtractionSharingTest, MaterialDemandSkipsMaskedBindingsAndRestoresThemForTheNextView)
{
    struct RestoreFrameCount {
        uint64_t value;
        ~RestoreFrameCount() { Time::frameCount = value; }
    } restoreFrame{Time::frameCount};
    /// @note Scene publication and resource retirement use independent frame clocks.
    const auto resourceFrame = m_resources->FrameStamp();
    Time::frameCount = resourceFrame + 17;
    ASSERT_NE(Time::frameCount, resourceFrame);
    auto& object = AddObject(10);
    auto* component = object.GetComponent<scene::MaterialComponent>();
    const auto authoring = component->SlotAt(0).ReadAuthoring();
    r::RenderSettings settings;
    settings.shadowEnabled = false;
    const auto skipped = ExtractDemand(settings, [](auto& context) { context.cullingMask = 0; });
    ASSERT_EQ(skipped->Objects().size(), 1u);
    ASSERT_EQ(skipped->items.size(), 1u);
    EXPECT_EQ(skipped->frameStamp, Time::frameCount);
    EXPECT_EQ(m_resources->FrameStamp(), resourceFrame);
    EXPECT_TRUE(skipped->items[0].visible);
    EXPECT_FALSE(skipped->items[0].material.valid);
    EXPECT_EQ(m_backend.shaderCreates, 0);
    EXPECT_EQ(m_backend.constantBufferUpdates, 0u);
    EXPECT_EQ(component->SlotAt(0).ReadAuthoring(), authoring);
    const auto visible = ExtractDemand(settings);
    ASSERT_EQ(visible->items.size(), 1u);
    EXPECT_TRUE(visible->items[0].material.valid);
    EXPECT_GT(m_backend.constantBufferUpdates, 0u);
    EXPECT_FALSE(skipped->items[0].material.valid);
}

TEST_F(MaterialExtractionSharingTest, MaterialDemandRestoresOmissionsWhenTheResourceFrameAdvancesDuringExtraction)
{
    auto& masked = AddObject(10);
    masked.layer = 1;
    AddObject(10);
    r::RenderSettings settings;
    settings.shadowEnabled = false;
    const auto resourceFrame = m_resources->FrameStamp();
    const auto sceneFrame = Time::frameCount;
    bool advanced = false;
    const auto output = ExtractDemand(settings, [&](auto& context) {
        context.cullingMask = 1;
        m_backend.beforeConstantBufferCreate = [&] {
            m_resources->AdvanceFrame();
            advanced = true;
        };
    });
    ASSERT_TRUE(advanced);
    ASSERT_EQ(output->items.size(), 2u);
    EXPECT_EQ(m_resources->FrameStamp(), resourceFrame + 1);
    EXPECT_EQ(Time::frameCount, sceneFrame);
    EXPECT_EQ(output->frameStamp, sceneFrame);
    EXPECT_TRUE(output->items[0].material.valid);
    EXPECT_TRUE(output->items[1].material.valid);
}

TEST_F(MaterialExtractionSharingTest, MaterialDemandPreservesOffCameraShadowBindings)
{
    auto& object = AddObject(10);
    object.transform.position = object.transform.worldPosition = {0, -10, 10};
    object.GetComponent<scene::MeshRenderer>()->castShadows = true;
    m_mesh.vertexBuffer = {10, 1};
    m_mesh.indexBuffer = {20, 1};
    math::Frustum frustum;
    const auto configure = [&](auto& context) {
        context.cameraFrustum = &frustum;
        context.frustumCullingEnabled = true;
    };
    r::RenderSettings settings;
    settings.shadowEnabled = false;
    const auto skipped = ExtractDemand(settings, configure);
    ASSERT_EQ(skipped->items.size(), 1u);
    EXPECT_FALSE(skipped->items[0].material.valid);
    EXPECT_EQ(m_backend.constantBufferUpdates, 0u);
    settings.shadowEnabled = true;
    const auto shadow = ExtractDemand(settings, configure);
    ASSERT_EQ(shadow->items.size(), 1u);
    EXPECT_TRUE(shadow->items[0].material.valid);
    EXPECT_GT(m_backend.constantBufferUpdates, 0u);
}

TEST_F(MaterialExtractionSharingTest, MaterialDemandPreservesStaticReflectionAndLightProbeBindings)
{
    auto& object = AddObject(10);
    object.transform.position = object.transform.worldPosition = {0, -10, 10};
    object.GetComponent<scene::MeshRenderer>()->castShadows = false;
    m_mesh.vertexBuffer = {10, 1};
    m_mesh.indexBuffer = {20, 1};
    math::Frustum frustum;
    const auto configure = [&](auto& context) {
        context.cameraFrustum = &frustum;
        context.frustumCullingEnabled = true;
    };
    r::RenderSettings settings;
    settings.shadowEnabled = false;
    auto& probeOwner = m_scene.CreateGameObject("Capture");
    auto& probe = probeOwner.AddComponent<scene::ReflectionProbeComponent>();
    probe.captureMode = scene::ReflectionProbeCaptureMode::DynamicScene;
    const auto reflection = ExtractDemand(settings, configure);
    ASSERT_EQ(reflection->items.size(), 1u);
    EXPECT_TRUE(reflection->items[0].material.valid);
    probe.enabled = false;
    probeOwner.AddComponent<scene::LightProbeVolumeComponent>();
    const auto light = ExtractDemand(settings, configure);
    ASSERT_EQ(light->items.size(), 1u);
    EXPECT_TRUE(light->items[0].material.valid);
}

TEST_F(MaterialExtractionSharingTest, MaterialDemandKeepsUnscopedAndNestedExtractionBindings)
{
    AddObject(10);
    r::RenderSettings settings;
    settings.shadowEnabled = false;
    const auto mask = [](auto& context) { context.cullingMask = 0; };
    const auto unscoped = ExtractDemand(settings, mask, false);
    ASSERT_EQ(unscoped->items.size(), 1u);
    EXPECT_TRUE(unscoped->items[0].material.valid);
    const auto nested = ExtractDemand(settings, mask, true, true);
    ASSERT_EQ(nested->items.size(), 1u);
    EXPECT_TRUE(nested->items[0].material.valid);
}

TEST_F(MaterialExtractionSharingTest, MaterialDemandKeepsNativeSetupHooksAndObjectMaskFallback)
{
    auto& object = AddObject(10);
    auto& scripts = object.AddComponent<scene::ScriptComponent>();
    scripts.scripts.emplace_back().script = std::make_unique<GeometryRenderWriter>();
    scripts.scripts.front().script->enabled = false;
    r::RenderSettings settings;
    settings.shadowEnabled = false;
    const auto native = ExtractDemand(settings, [](auto& context) { context.cullingMask = 0; });
    ASSERT_EQ(native->items.size(), 1u);
    EXPECT_TRUE(native->items[0].material.valid);
    scripts.scripts.clear();
    const auto mask = ExtractDemand(settings, [](auto& context) {
        context.cullingMask = 0;
        context.objectMaskEnabled = true;
    });
    ASSERT_EQ(mask->items.size(), 1u);
    EXPECT_TRUE(mask->items[0].material.valid);
}

TEST_F(MaterialExtractionSharingTest, MaterialDemandKeepsRayInputsOutsideTheRasterMask)
{
    AddObject(10);
    r::RenderSettings settings;
    settings.shadowEnabled = false;
    settings.modeRequest.mode = r::RenderMode::PATH_TRACING;
    const auto ray = ExtractDemand(settings, [](auto& context) {
        context.cullingMask = 0;
        context.experimentalRayTracingEnabled = true;
    });
    ASSERT_EQ(ray->items.size(), 1u);
    EXPECT_TRUE(ray->items[0].material.valid);
}

TEST_F(MaterialExtractionSharingTest, MaterialDemandRestoresEarlierMaskedBindingAfterLaterSlotChangesTheView)
{
    auto& masked=AddObject(10);masked.layer=1;AddObject(10);
    r::RenderSettings settings;settings.shadowEnabled=false;
    bool changed=false;
    const auto output=ExtractDemand(settings,[&](auto& context){
        context.cullingMask=1;
        m_backend.beforeConstantBufferCreate=[&context,&changed]{context.cullingMask=3;changed=true;};
    });
    ASSERT_TRUE(changed);ASSERT_EQ(output->items.size(),2u);
    EXPECT_TRUE(output->items[0].material.valid);EXPECT_TRUE(output->items[1].material.valid);
}

TEST_F(MaterialExtractionSharingTest, MaterialDemandRestoresEarlierOffCameraBindingAfterLaterSlotAddsAProbe)
{
    auto& outside=AddObject(10);outside.transform.position=outside.transform.worldPosition={0,-10,10};AddObject(10);
    m_mesh.vertexBuffer={10,1};m_mesh.indexBuffer={20,1};
    math::Frustum frustum;r::RenderSettings settings;settings.shadowEnabled=false;bool changed=false;
    const auto output=ExtractDemand(settings,[&](auto& context){
        context.cameraFrustum=&frustum;context.frustumCullingEnabled=true;
        m_backend.beforeConstantBufferCreate=[&]{
            m_scene.CreateGameObject("Late probe").AddComponent<scene::ReflectionProbeComponent>().captureMode=scene::ReflectionProbeCaptureMode::DynamicScene;
            changed=true;
        };
    });
    ASSERT_TRUE(changed);ASSERT_EQ(output->items.size(),2u);
    EXPECT_TRUE(output->items[0].material.valid);EXPECT_TRUE(output->items[1].material.valid);
}

TEST_F(MaterialExtractionSharingTest, MaterialDemandRestoresEarlierBindingAfterLaterSlotAddsANativeSetupHook)
{
    auto& masked=AddObject(10);masked.layer=1;auto& visible=AddObject(10);
    r::RenderSettings settings;settings.shadowEnabled=false;bool changed=false;
    const auto output=ExtractDemand(settings,[&](auto& context){
        context.cullingMask=1;
        m_backend.beforeConstantBufferCreate=[&]{
            visible.AddComponent<scene::ScriptComponent>().scripts.emplace_back().script=std::make_unique<GeometryRenderWriter>();
            changed=true;
        };
    });
    ASSERT_TRUE(changed);ASSERT_EQ(output->items.size(),2u);
    EXPECT_TRUE(output->items[0].material.valid);EXPECT_TRUE(output->items[1].material.valid);
}

TEST_F(MaterialExtractionSharingTest, MaterialDemandRestoresBindingAfterADifferentExtractorChangesTheView)
{
    auto& masked=AddObject(10);masked.layer=1;
    asset::MaterialAssetAuthoring decalMaterial;decalMaterial.shaderPath=ShaderPath("Shared.hlsl");decalMaterial.renderPath=asset::RenderPath::Decal;
    const auto path=m_temp.File("LateDecal.mat").generic_string();ASSERT_TRUE(asset::SaveMaterialAssetToFile(path,decalMaterial));
    m_scene.CreateGameObject("Late decal").AddComponent<scene::DecalComponent>().materialPath=path;
    r::RenderSettings settings;settings.shadowEnabled=false;bool changed=false;
    const auto output=ExtractDemand(settings,[&](auto& context){
        context.cullingMask=1;
        m_backend.beforeConstantBufferCreate=[&context,&changed]{context.cullingMask=3;changed=true;};
    });
    ASSERT_TRUE(changed);ASSERT_EQ(output->items.size(),1u);EXPECT_FALSE(output->decals.empty());
    EXPECT_TRUE(output->items[0].material.valid);
}

TEST_F(MaterialExtractionSharingTest, MaterialDemandLateCompletionCopiesThePublishedSnapshotAndRestoresAllOmissionsOnce)
{
    AddObject(10);AddObject(20);
    r::RenderSettings settings;settings.shadowEnabled=false;r::Camera camera;r::RenderPassHandles handles;
    scene::RenderPassContext context(m_scene,m_backend,*m_resources,camera,settings,{},UINT32_MAX,handles);
    context.isDeferred=true;context.height=600;context.cullProjScaleY=1;context.cullingMask=0;
    scene::material_demand_detail::BindingDemandScope demand(context);
    scene::RenderFrameGeometryCache geometry;
    scene::ExtractRenderScene(context,&geometry);
    const auto original=context.renderScene;ASSERT_EQ(original->items.size(),2u);
    ASSERT_FALSE(original->items[0].material.valid);ASSERT_FALSE(original->items[1].material.valid);
    scene::material_demand_detail::CompleteBindings(context);EXPECT_EQ(context.renderScene,original);
    context.cullingMask=UINT32_MAX;
    std::shared_ptr<const r::RenderScene> nested;
    int callbacks=0;
    m_backend.beforeConstantBufferCreate=[&]{
        ++callbacks;context.cullingMask=0;
        scene::material_demand_detail::CompleteBindings(context);
        scene::ExtractRenderScene(context);nested=context.renderScene;
    };
    scene::material_demand_detail::CompleteBindings(context);
    ASSERT_NE(context.renderScene,original);ASSERT_NE(nested,nullptr);EXPECT_EQ(callbacks,1);
    EXPECT_EQ(context.renderScene->sharedObjects,original->sharedObjects);
    EXPECT_NE(context.renderScene->snapshotSerial,original->snapshotSerial);
    EXPECT_EQ(context.renderScene->Objects().size(),original->Objects().size());
    for(size_t index=0;index<original->items.size();++index){
        EXPECT_FALSE(original->items[index].material.valid);
        EXPECT_TRUE(context.renderScene->items[index].material.valid);EXPECT_TRUE(nested->items[index].material.valid);
        EXPECT_EQ(context.renderScene->items[index].objectIndex,original->items[index].objectIndex);
    }
    const auto completed=context.renderScene;const auto updates=m_backend.constantBufferUpdates;
    context.cullingMask=UINT32_MAX;scene::material_demand_detail::CompleteBindings(context);
    EXPECT_EQ(context.renderScene,completed);EXPECT_EQ(m_backend.constantBufferUpdates,updates);
}

TEST_F(MaterialExtractionSharingTest, MaterialDemandLateNativeSetupHookCompletesAPreviouslyPublishedSnapshot)
{
    auto& object=AddObject(10);
    r::RenderSettings settings;settings.shadowEnabled=false;r::Camera camera;r::RenderPassHandles handles;
    scene::RenderPassContext context(m_scene,m_backend,*m_resources,camera,settings,{},UINT32_MAX,handles);
    context.cullingMask=0;scene::material_demand_detail::BindingDemandScope demand(context);
    scene::ExtractRenderScene(context);const auto original=context.renderScene;
    ASSERT_FALSE(original->items[0].material.valid);
    scene::material_demand_detail::CompleteBindings(context);
    scene::material_demand_detail::CompleteBindings(context);
    object.AddComponent<scene::ScriptComponent>().scripts.emplace_back().script=std::make_unique<GeometryRenderWriter>();
    scene::material_demand_detail::CompleteBindings(context);
    ASSERT_NE(context.renderScene,original);EXPECT_TRUE(context.renderScene->items[0].material.valid);
    EXPECT_FALSE(original->items[0].material.valid);
}

TEST_F(MaterialExtractionSharingTest, MaterialDemandRechecksInPlaceFrustumEditsAfterRepeatedNoDemandBoundaries)
{
    AddObject(20);
    m_mesh.vertexBuffer = {10, 1}; m_mesh.indexBuffer = {20, 1};
    r::RenderSettings settings; settings.shadowEnabled = false;
    r::Camera camera; r::RenderPassHandles handles;
    auto frustum = math::Frustum::FromViewProjection(math::Matrix4::Identity());
    scene::RenderPassContext context(m_scene, m_backend, *m_resources, camera, settings, {}, UINT32_MAX, handles);
    context.frustumCullingEnabled = true; context.cameraFrustum = &frustum;
    context.cullProjScaleY = 1; context.height = 600;
    scene::material_demand_detail::BindingDemandScope demand(context);
    scene::ExtractRenderScene(context);
    const auto original = context.renderScene;
    ASSERT_EQ(original->items.size(), 1u);
    ASSERT_FALSE(original->items[0].material.valid);

    scene::material_demand_detail::CompleteBindings(context);
    scene::material_demand_detail::CompleteBindings(context);
    EXPECT_EQ(context.renderScene, original);
    auto wider = math::Matrix4::Identity(); wider.m[2][2] = 0.01f;
    frustum = math::Frustum::FromViewProjection(wider);
    scene::material_demand_detail::CompleteBindings(context);

    ASSERT_NE(context.renderScene, original);
    EXPECT_TRUE(context.renderScene->items[0].material.valid);
    EXPECT_FALSE(original->items[0].material.valid);
}

TEST_F(MaterialExtractionSharingTest, MaterialDemandRechecksLayerDistanceAfterNoDemandBoundaries)
{
    AddObject(20);
    m_mesh.vertexBuffer = {10, 1}; m_mesh.indexBuffer = {20, 1};
    r::RenderSettings settings; settings.shadowEnabled = false;
    r::Camera camera; r::RenderPassHandles handles;
    scene::RenderPassContext context(m_scene, m_backend, *m_resources, camera, settings, {}, UINT32_MAX, handles);
    context.frustumCullingEnabled = false; context.hasLayerCullDistances = true;
    context.cullLayerDistances[0] = 5; context.cullProjScaleY = 1; context.height = 600;
    scene::material_demand_detail::BindingDemandScope demand(context);
    scene::ExtractRenderScene(context);
    const auto original = context.renderScene;
    ASSERT_EQ(original->items.size(), 1u);
    ASSERT_FALSE(original->items[0].material.valid);

    scene::material_demand_detail::CompleteBindings(context);
    scene::material_demand_detail::CompleteBindings(context);
    context.cullLayerDistances[0] = 30;
    scene::material_demand_detail::CompleteBindings(context);
    ASSERT_NE(context.renderScene, original);
    EXPECT_TRUE(context.renderScene->items[0].material.valid);
    EXPECT_FALSE(original->items[0].material.valid);
}

TEST_F(MaterialExtractionSharingTest, MaterialDemandRechecksResourceFrameAfterNoDemandBoundaries)
{
    AddObject(20);
    r::RenderSettings settings; settings.shadowEnabled = false;
    r::Camera camera; r::RenderPassHandles handles;
    scene::RenderPassContext context(m_scene, m_backend, *m_resources, camera, settings, {}, UINT32_MAX, handles);
    context.cullingMask = 0;
    scene::material_demand_detail::BindingDemandScope demand(context);
    scene::ExtractRenderScene(context);
    const auto original = context.renderScene;
    ASSERT_EQ(original->items.size(), 1u);
    ASSERT_FALSE(original->items[0].material.valid);

    scene::material_demand_detail::CompleteBindings(context);
    scene::material_demand_detail::CompleteBindings(context);
    m_resources->AdvanceFrame();
    scene::material_demand_detail::CompleteBindings(context);

    ASSERT_NE(context.renderScene, original);
    EXPECT_TRUE(context.renderScene->items[0].material.valid);
    EXPECT_FALSE(original->items[0].material.valid);
}

TEST_F(MaterialExtractionSharingTest, ComputedSkinningDemandKeepsOffCameraShadowCasters)
{
    r::RenderSettings settings;
    settings.shadowEnabled = true;
    r::Camera camera;
    r::RenderPassHandles handles;
    scene::RenderPassContext context(m_scene, m_backend, *m_resources, camera, settings, {}, UINT32_MAX, handles);
    const auto cameraFrustum = math::Frustum::FromViewProjection(math::Matrix4::Identity());
    context.cameraFrustum = &cameraFrustum;
    auto wide = math::Matrix4::Identity();
    wide.m[0][0] = wide.m[1][1] = 0.1f;
    context.shadowCascades[0].frustum = math::Frustum::FromViewProjection(wide);
    context.shadowCascades[0].texelWorldSize = 0.01f;
    r::RenderObject object;
    object.boundsCenter = {3, 0, 0.5f};
    object.boundsRadius = 0.25f;
    object.castShadows = true;
    EXPECT_TRUE(r::RequiresComputedSkinning(context, object));
    object.castShadows = false;
    EXPECT_FALSE(r::RequiresComputedSkinning(context, object));
    object.castShadows = true;
    settings.shadowEnabled = false;
    EXPECT_FALSE(r::RequiresComputedSkinning(context, object));
}

TEST_F(MaterialExtractionSharingTest, ComputedSkinningDemandKeepsPunctualShadowsWhenDirectionalStrengthIsZero)
{
    r::RenderSettings settings;
    settings.shadowEnabled = true;
    r::Camera camera;
    r::RenderPassHandles handles;
    scene::RenderPassContext context(m_scene, m_backend, *m_resources, camera, settings, {}, UINT32_MAX, handles);
    const auto cameraFrustum = math::Frustum::FromViewProjection(math::Matrix4::Identity());
    context.cameraFrustum = &cameraFrustum;
    context.shadowStrength = 0;
    context.punctualShadowViewCount = 1;
    auto wide = math::Matrix4::Identity();
    wide.m[0][0] = wide.m[1][1] = 0.1f;
    context.punctualShadowViews[0].frustum = math::Frustum::FromViewProjection(wide);
    context.punctualShadowViews[0].texelAngularSize = 0;
    r::RenderObject object;
    object.boundsCenter = {3, 0, 0.5f};
    object.boundsRadius = 0.25f;
    object.castShadows = true;
    EXPECT_TRUE(r::RequiresComputedSkinning(context, object));
    context.cullMaxDistance = 1;
    EXPECT_FALSE(r::RequiresComputedSkinning(context, object));
}

TEST_F(MaterialExtractionSharingTest, ComputedSkinningDemandPreservesUnknownConsumersAndUnavailableBounds)
{
    r::RenderSettings settings;
    settings.shadowEnabled = false;
    r::Camera camera;
    r::RenderPassHandles handles;
    scene::RenderPassContext context(m_scene, m_backend, *m_resources, camera, settings, {}, 0, handles);
    r::RenderObject object;
    object.boundsCenter = {3, 0, 0.5f};
    object.boundsRadius = 0.25f;
    EXPECT_FALSE(r::RequiresComputedSkinning(context, object));
    EXPECT_TRUE(r::RequiresComputedSkinning(context, object, true));
    context.objectMaskEnabled = true;
    EXPECT_TRUE(r::RequiresComputedSkinning(context, object));
    context.objectMaskEnabled = false;
    context.selectionOutlineEnabled = true;
    object.selected = true;
    EXPECT_TRUE(r::RequiresComputedSkinning(context, object));
    object.selected = false;
    context.experimentalRayTracingEnabled = true;
    settings.modeRequest.mode = r::RenderMode::PATH_TRACING;
    EXPECT_TRUE(r::RequiresComputedSkinning(context, object));
    context.experimentalRayTracingEnabled = false;
    object.boundsRadius = std::numeric_limits<float>::quiet_NaN();
    EXPECT_TRUE(r::RequiresComputedSkinning(context, object));
}

} /// @note namespace fbzz::tests
