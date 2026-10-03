/// @file    ShaderReloadTests.cpp
/// @brief   シェーダーの一括差し替え・失敗時保持と定数バッファ容量の契約を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-15
#include <TestKit/TestKit.hpp>
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
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Renderer/Camera.hpp>
#include <Engine/Renderer/RenderSettings.hpp>
#include <cstring>

namespace fbzz::tests {
namespace {

namespace r = renderer;

class TestShader final : public r::IShader {
public:
    TestShader(std::string path, uint32_t size, const std::vector<r::ShaderVarDesc>& variables = {})
        : m_path(std::move(path)) { m_descriptor.cbufferSize = size; m_descriptor.vars = variables; }
    const std::string& GetPath() const override { return m_path; }
    const r::ShaderDescriptor& GetDescriptor() const override { return m_descriptor; }
private:
    std::string m_path;
    r::ShaderDescriptor m_descriptor;
};

class TestConstantBuffer final : public r::IConstantBuffer {
public:
    explicit TestConstantBuffer(size_t size) : m_size((size + 255) & ~size_t(255)) {}
    size_t GetSize() const override { return m_size; }
    void Update(const void*, size_t size) override { EXPECT_LE(size, m_size); }
private:
    size_t m_size;
};

class TestPipeline final : public r::IPipelineState {
public:
    explicit TestPipeline(const r::PipelineStateDesc& desc) : m_desc(desc) {}
    const r::PipelineStateDesc& GetDesc() const override { return m_desc; }
private:
    r::PipelineStateDesc m_desc;
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

    bool PrepareShaderReload() override {
        ++prepareCalls;
        if (beforeReplace) beforeReplace();
        return allowReload;
    }
    std::unique_ptr<r::IShader> CreateNativeShader(const std::string& path) override {
        if (++shaderCreates == failAt) return {};
        return std::make_unique<TestShader>(path, shaderSize, shaderVars);
    }
    std::unique_ptr<r::IConstantBuffer> CreateNativeConstantBuffer(size_t size) override {
        return std::make_unique<TestConstantBuffer>(size);
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
    std::unique_ptr<r::IBuffer> CreateNativeVertexBuffer(const void*, size_t, uint32_t) override { return {}; }
    std::unique_ptr<r::IBuffer> CreateNativeIndexBuffer(const void*, uint32_t) override { return {}; }
    std::unique_ptr<r::ITexture> CreateNativeTexture(const std::string&) override { return {}; }
    std::unique_ptr<r::ITexture> CreateNativeTextureFromData(const uint8_t*, uint32_t, uint32_t) override { return {}; }
    std::unique_ptr<r::ITexture> CreateNativeTexture3DFromData(const uint8_t*, uint32_t, uint32_t, uint32_t) override { return {}; }
    std::unique_ptr<r::ITexture> CreateNativeTextureFromRenderTarget(r::IRenderTarget&, uint32_t, r::RenderTargetTextureKind) override { return {}; }
    std::unique_ptr<r::IPipelineState> CreateNativePipelineState(const r::PipelineStateDesc& desc) override { return std::make_unique<TestPipeline>(desc); }
    std::unique_ptr<r::IRenderTarget> CreateNativeRenderTarget(uint32_t, uint32_t, const r::RenderTargetDesc&) override { return {}; }
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
    slot.hasDoubleSidedOverride = true;
    slot.doubleSidedOverride = true;
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
    slot.doubleSidedOverride = false;
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
        asset::AssetManager::Init(*m_resources, m_temp.Path().generic_string() + "/");
        asset::MaterialAsset material;
        material.shaderPath = "typed.hlsl";
        const auto path = m_temp.File("typed.mat").generic_string();
        ASSERT_TRUE(asset::SaveMaterialAssetToFile(path, material));
        auto& object = m_scene.CreateGameObject("Typed material");
        m_material = &object.AddComponent<scene::MaterialComponent>();
        m_material->materialPath = path;
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
    asset::ApplyMaterialParamOverrides(m_material->paramOverrides, descriptor, bytes);
    asset::ApplyMaterialIntegerOverrides(m_material->integerParamOverrides, descriptor, bytes);
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
    EXPECT_FALSE(m_material->integerParamOverrides.contains("mode"));
    ASSERT_TRUE(instance.ClearAllOverrides());
    EXPECT_TRUE(m_material->integerParamOverrides.empty());
    EXPECT_TRUE(m_material->paramOverrides.empty());
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
    EXPECT_FALSE(m_material->integerParamOverrides.contains("mode"));
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
    asset::MaterialAsset authored;
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
    object.AddComponent<scene::MaterialComponent>().materialPath = path;
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
    asset::MaterialAsset glass;
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
    slot.materialPath = path;
    renderer::Camera camera;
    renderer::RenderSettings settings;
    renderer::RenderPassHandles handles;
    scene::RenderPassContext context(m_scene, m_backend, *m_resources, camera, settings, {}, UINT32_MAX, handles);
    scene::ExtractRenderScene(context);
    ASSERT_NE(context.renderScene, nullptr);
    ASSERT_EQ(context.renderScene->items.size(), 1u);
    const auto first = context.renderScene->items[0].material.surface;
    EXPECT_EQ(first.dielectric, glass.dielectric);
    EXPECT_TRUE(first.solidDielectricSupported);
    EXPECT_FALSE(first.standardSurfaceSupported);
    ASSERT_NE(slot.material, nullptr);
    EXPECT_EQ(slot.material->dielectric, glass.dielectric);
    auto* shared = asset::AssetManager::Get<asset::MaterialAsset>(slot.materialAsset);
    ASSERT_NE(shared, nullptr);
    shared->dielectric.ior = 1.7f;
    scene::ExtractRenderScene(context);
    const auto edited = context.renderScene->items[0].material.surface;
    EXPECT_FLOAT_EQ(edited.dielectric.ior, 1.7f);
    EXPECT_NE(first, edited);
    slot.paramOverrides["roughness"] = {0.1f};
    scene::ExtractRenderScene(context);
    EXPECT_TRUE(context.renderScene->items[0].material.surface.solidDielectricSupported);
    EXPECT_FLOAT_EQ(context.renderScene->items[0].material.surface.roughness, 0.1f);
    shared->dielectric.thinWalled = true;
    shared->dielectric.attenuationColor = {1, 1, 1};
    scene::ExtractRenderScene(context);
    EXPECT_EQ(context.renderScene->items[0].material.surface.issue,
        renderer::SurfaceMaterialIssue::SOLID_DIELECTRIC_UNSUPPORTED);
    slot.paramOverrides.erase("roughness");
    scene::ExtractRenderScene(context);
    EXPECT_TRUE(context.renderScene->items[0].material.surface.solidDielectricSupported);
    EXPECT_TRUE(context.renderScene->items[0].material.surface.dielectric.thinWalled);
    shared->dielectric.thinWalled = false;
    slot.paramOverrides["albedo"] = {1, 1, 1, 0.75f};
    scene::ExtractRenderScene(context);
    EXPECT_FALSE(context.renderScene->items[0].material.surface.solidDielectricSupported);
    EXPECT_FLOAT_EQ(context.renderScene->items[0].material.surface.dielectric.transmission, 1);
    slot.paramOverrides.erase("albedo");
    slot.hasBlendModeOverride = true;
    slot.blendModeOverride = renderer::BlendMode::ALPHA_BLEND;
    scene::ExtractRenderScene(context);
    EXPECT_NE(context.renderScene->items[0].material.rayCapabilities.opacity, renderer::RayOpacity::OPAQUE_SURFACE);
}

} /// @note namespace fbzz::tests
