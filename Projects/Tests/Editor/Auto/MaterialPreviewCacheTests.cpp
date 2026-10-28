/// @file    MaterialPreviewCacheTests.cpp
/// @brief   未保存マテリアルとプレビュー描画条件・資源寿命のキャッシュ回帰検査。
/// @author  Hasegawa Jin
/// @date    2026-10-10
#include <TestKit/TestKit.hpp>

#include <Editor/Util/MaterialPreviewCache.hpp>
#include <Editor/Util/RevisionCache.hpp>
#include <Engine/Renderer/IRenderer.hpp>
#include <Engine/Renderer/Mesh.hpp>
#include <Engine/Renderer/ResourceManager.hpp>

#include <bit>
#include <cstdint>
#include <memory>

namespace fbzz::tests {
namespace mp = editor::matpreview;
namespace {

class PreviewLifetimeBuffer final : public renderer::IBuffer {
public:
    PreviewLifetimeBuffer(std::size_t size, std::uint32_t stride) : m_size(size), m_stride(stride) {}
    void Update(const void*, std::size_t) override {}
    std::size_t GetSize() const override { return m_size; }
    std::uint32_t GetStride() const override { return m_stride; }
private:
    std::size_t m_size;
    std::uint32_t m_stride;
};

/// @note ShapeMesh の本番資源生成を CPU オブジェクトで受け、Manager の生存判定を検証する。
class PreviewLifetimeRenderer final : public renderer::IRenderer {
public:
    void Shutdown() override {}
    void BeginFrame() override {}
    void EndFrame() override {}
    void Clear(const math::Vector4&) override {}
    void Submit(const renderer::DrawCall&, renderer::ResourceManager&) override {}
    void Dispatch(const renderer::ComputeCall&, renderer::ResourceManager&) override {}
    void Resize(std::uint32_t, std::uint32_t) override {}
    std::uint32_t GetWidth() const override { return 1; }
    std::uint32_t GetHeight() const override { return 1; }
    void SetRenderTarget(renderer::ResourceHandle<renderer::RenderTargetTag>, renderer::ResourceManager&) override {}
    void ClearDepth() override {}
    void SetViewport(std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t) override {}
    std::unique_ptr<renderer::IShader> CreateNativeShader(const std::string&) override { return {}; }
    std::unique_ptr<renderer::IConstantBuffer> CreateNativeConstantBuffer(std::size_t) override { return {}; }
    std::unique_ptr<renderer::IBuffer> CreateNativeVertexBuffer(const void*, std::size_t size, std::uint32_t stride) override
    { return std::make_unique<PreviewLifetimeBuffer>(size, stride); }
    std::unique_ptr<renderer::IBuffer> CreateNativeIndexBuffer(const void*, std::uint32_t count) override
    { return std::make_unique<PreviewLifetimeBuffer>(static_cast<std::size_t>(count) * sizeof(std::uint32_t), 0); }
    std::unique_ptr<renderer::ITexture> CreateNativeTexture(const std::string&) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeTextureFromData(const std::uint8_t*, std::uint32_t, std::uint32_t) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeTextureFromDataMips(const renderer::TextureMipData*, std::uint32_t) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeTexture3DFromData(const std::uint8_t*, std::uint32_t, std::uint32_t, std::uint32_t) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeTextureFromRenderTarget(renderer::IRenderTarget&, std::uint32_t, renderer::RenderTargetTextureKind) override { return {}; }
    std::unique_ptr<renderer::IPipelineState> CreateNativePipelineState(const renderer::PipelineStateDesc&) override { return {}; }
    std::unique_ptr<renderer::IRenderTarget> CreateNativeRenderTarget(std::uint32_t, std::uint32_t, const renderer::RenderTargetDesc&) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeComputeTexture(std::uint32_t, std::uint32_t) override { return {}; }
    std::unique_ptr<renderer::IStructuredBuffer> CreateNativeStructuredBuffer(const void*, std::uint32_t, std::uint32_t) override { return {}; }
    std::unique_ptr<renderer::IStructuredBuffer> CreateNativeRWStructuredBuffer(const void*, std::uint32_t, std::uint32_t) override { return {}; }
};

}

class MaterialPreviewCacheTest : public testkit::Fixture {};

TEST_F(MaterialPreviewCacheTest, DirectParameterEditsAndUndoAreComparedByTheirActualValues)
{
    asset::MaterialAssetAuthoring source;
    source.params["albedo"] = { 0.2f, 0.4f, 0.6f, 1.0f };
    source.integerParams["mode"] = { 16777217 };
    auto edited = source;
    ASSERT_TRUE(mp::MaterialPreviewValuesEqual(source, edited));

    edited.params["albedo"][1] = 0.5f;

    EXPECT_FALSE(mp::MaterialPreviewValuesEqual(source, edited));
    edited = source;
    EXPECT_TRUE(mp::MaterialPreviewValuesEqual(source, edited));
    edited.integerParams["mode"][0] = 16777216;
    EXPECT_FALSE(mp::MaterialPreviewValuesEqual(source, edited));
    edited = source;
    edited.params["uvTiling"] = { 2.0f, 2.0f };
    EXPECT_FALSE(mp::MaterialPreviewValuesEqual(source, edited));
}

TEST_F(MaterialPreviewCacheTest, HashMapOrderDoesNotChangeMaterialIdentity)
{
    asset::MaterialAssetAuthoring a, b;
    a.textures["albedo"] = "Assets/Albedo.png";
    a.textures["normal"] = "Assets/Normal.png";
    a.params["roughness"] = { 0.4f };
    a.params["metallic"] = { 0.2f };
    b.textures["normal"] = "Assets/Normal.png";
    b.textures["albedo"] = "Assets/Albedo.png";
    b.params["metallic"] = { 0.2f };
    b.params["roughness"] = { 0.4f };
    b.params.rehash(97);

    EXPECT_TRUE(mp::MaterialPreviewValuesEqual(a, b));
}

TEST_F(MaterialPreviewCacheTest, ShaderTextureRenderStateParticleAndDielectricEditsInvalidateIdentity)
{
    const asset::MaterialAssetAuthoring source;
    auto edited = source;
    edited.shaderPath = "Assets/Shaders/Material/Water.hlsl";
    EXPECT_FALSE(mp::MaterialPreviewValuesEqual(source, edited));
    edited = source;
    edited.textures["normal"] = "Assets/Normal.png";
    EXPECT_FALSE(mp::MaterialPreviewValuesEqual(source, edited));
    edited = source;
    edited.depthWrite = false;
    EXPECT_FALSE(mp::MaterialPreviewValuesEqual(source, edited));
    edited = source;
    edited.particle.sixWayEmissionScale = 3.0f;
    EXPECT_FALSE(mp::MaterialPreviewValuesEqual(source, edited));
    edited = source;
    edited.particle.flipbook.spriteEndFrame = 12;
    EXPECT_FALSE(mp::MaterialPreviewValuesEqual(source, edited));
    edited = source;
    edited.dielectric.transmission = 0.8f;
    EXPECT_FALSE(mp::MaterialPreviewValuesEqual(source, edited));
}

TEST_F(MaterialPreviewCacheTest, StableNanBitsDoNotForceUnchangedMaterialToRebuild)
{
    asset::MaterialAssetAuthoring source;
    source.params["custom"] = { std::bit_cast<float>(std::uint32_t{ 0x7FC00001 }) };
    source.particle.emissiveScale = source.params["custom"][0];
    auto edited = source;

    EXPECT_TRUE(mp::MaterialPreviewValuesEqual(source, edited));

    edited.params["custom"][0] = std::bit_cast<float>(std::uint32_t{ 0x7FC00002 });
    EXPECT_FALSE(mp::MaterialPreviewValuesEqual(source, edited));
}

TEST_F(MaterialPreviewCacheTest, ResourceResetReplacementShaderReloadAndProjectSwitchInvalidateGpuKey)
{
    mp::PreviewGpuKey source;
    source.resources.instanceId = 1;
    source.resources.resetVersion = 1;
    source.resources.shaderVersion = 1;
    source.resources.projectRoot = "C:/ProjectA";
    editor::RevisionCache<mp::PreviewGpuKey> cache;
    cache.Commit(source);
    ASSERT_TRUE(cache.IsCurrent(source));

    auto changed = source;
    ++changed.resources.resetVersion;
    EXPECT_FALSE(cache.IsCurrent(changed));
    changed = source;
    ++changed.resources.instanceId;
    EXPECT_FALSE(cache.IsCurrent(changed));
    changed = source;
    ++changed.resources.shaderVersion;
    EXPECT_FALSE(cache.IsCurrent(changed));
    changed = source;
    ++changed.resources.assetGeneration;
    EXPECT_FALSE(cache.IsCurrent(changed));
    changed = source;
    changed.resources.projectRoot = "C:/ProjectB";
    EXPECT_FALSE(cache.IsCurrent(changed));
}

TEST_F(MaterialPreviewCacheTest, TextureReloadKeepingItsHandleStillInvalidatesGpuKey)
{
    mp::PreviewGpuKey source;
    source.textures.push_back({ { 7, 2 }, nullptr, 1 });
    editor::RevisionCache<mp::PreviewGpuKey> cache;
    cache.Commit(source);
    auto reloaded = source;

    ++reloaded.textures[0].contentVersion;

    EXPECT_FALSE(cache.IsCurrent(reloaded));
    reloaded = source;
    ++reloaded.textures[0].handle.gen;
    EXPECT_FALSE(cache.IsCurrent(reloaded));
    reloaded.textures.clear();
    EXPECT_FALSE(cache.IsCurrent(reloaded));
}

TEST_F(MaterialPreviewCacheTest, CameraLightShapeChannelAndFiberInputsInvalidateTheRenderedFrame)
{
    mp::RenderDesc desc;
    desc.target = { 3, 1 };
    const auto source = mp::MakePreviewFrameKey(1, desc, mp::Shape::Sphere, true);
    editor::RevisionCache<mp::PreviewFrameKey> cache;
    cache.Commit(source);
    ASSERT_TRUE(cache.IsCurrent(source));

    auto changed = desc;
    changed.orbit.yaw += 0.2f;
    EXPECT_FALSE(cache.IsCurrent(mp::MakePreviewFrameKey(1, changed, mp::Shape::Sphere, true)));
    changed = desc;
    changed.rig.exposure = 2.0f;
    EXPECT_FALSE(cache.IsCurrent(mp::MakePreviewFrameKey(1, changed, mp::Shape::Sphere, true)));
    EXPECT_FALSE(cache.IsCurrent(mp::MakePreviewFrameKey(1, desc, mp::Shape::Cube, true)));
    changed = desc;
    changed.channel = mp::Channel::Normal;
    EXPECT_FALSE(cache.IsCurrent(mp::MakePreviewFrameKey(1, changed, mp::Shape::Sphere, true)));
    changed = desc;
    changed.fiberMode = mp::FiberMode::Blade;
    EXPECT_FALSE(cache.IsCurrent(mp::MakePreviewFrameKey(1, changed, mp::Shape::Sphere, true)));
    changed = desc;
    changed.fiberShellCount = 32;
    EXPECT_FALSE(cache.IsCurrent(mp::MakePreviewFrameKey(1, changed, mp::Shape::Sphere, true)));
    changed = desc;
    changed.fiberWind = 1.0f;
    EXPECT_FALSE(cache.IsCurrent(mp::MakePreviewFrameKey(1, changed, mp::Shape::Sphere, true)));
    EXPECT_FALSE(cache.IsCurrent(mp::MakePreviewFrameKey(2, desc, mp::Shape::Sphere, true)));
    EXPECT_FALSE(cache.IsCurrent(mp::MakePreviewFrameKey(1, desc, mp::Shape::Sphere, false)));
}

TEST_F(MaterialPreviewCacheTest, EveryTimeDependentPreviewRemainsDynamicUntilAnimationIsPaused)
{
    for (const auto flavor : { mp::Flavor::Water, mp::Flavor::Fiber,
                              mp::Flavor::Trail, mp::Flavor::PostProcess }) {
        EXPECT_TRUE(mp::UsesPreviewTime(flavor, true));
        EXPECT_FALSE(mp::UsesPreviewTime(flavor, false));
    }
    for (const auto flavor : { mp::Flavor::Surface, mp::Flavor::Skinned, mp::Flavor::Terrain,
                              mp::Flavor::Ui, mp::Flavor::Particle, mp::Flavor::Decal })
        EXPECT_FALSE(mp::UsesPreviewTime(flavor, true));
}

TEST_F(MaterialPreviewCacheTest, AnInvalidatedFrameCannotBeReusedBeforeAnotherSuccessfulCommit)
{
    mp::RenderDesc desc;
    const auto key = mp::MakePreviewFrameKey(1, desc, mp::Shape::Sphere, true);
    editor::RevisionCache<mp::PreviewFrameKey> cache;
    cache.Commit(key);
    ASSERT_TRUE(cache.IsCurrent(key));

    cache.Invalidate();

    EXPECT_FALSE(cache.IsCurrent(key));
    cache.Commit(key);
    EXPECT_TRUE(cache.IsCurrent(key));
}

TEST_F(MaterialPreviewCacheTest, SkinnedShapeRecreatesLiveBuffersAfterResourceReset)
{
    PreviewLifetimeRenderer renderer;
    renderer::ResourceManager resources(renderer);
    const auto* initial = mp::ShapeMesh(resources, mp::Shape::Cube, mp::Flavor::Skinned);
    ASSERT_NE(initial, nullptr);
    ASSERT_NE(resources.Get(initial->vertexBuffer), nullptr);
    const auto previousVertex = initial->vertexBuffer;

    resources.Reset();
    const auto* rebuilt = mp::ShapeMesh(resources, mp::Shape::Cube, mp::Flavor::Skinned);

    ASSERT_NE(rebuilt, nullptr);
    EXPECT_NE(resources.Get(rebuilt->vertexBuffer), nullptr);
    EXPECT_NE(resources.Get(rebuilt->indexBuffer), nullptr);
    EXPECT_NE(rebuilt->vertexBuffer, previousVertex);
}

TEST_F(MaterialPreviewCacheTest, SkinnedShapeDoesNotReuseAnotherManagersNumericHandles)
{
    PreviewLifetimeRenderer renderer;
    renderer::ResourceManager first(renderer);
    const auto* initial = mp::ShapeMesh(first, mp::Shape::Cube, mp::Flavor::Skinned);
    ASSERT_NE(initial, nullptr);
    const auto previousVertex = initial->vertexBuffer;
    renderer::ResourceManager second(renderer);
    ASSERT_EQ(first.GetResetVersion(), second.GetResetVersion());

    const auto* rebuilt = mp::ShapeMesh(second, mp::Shape::Cube, mp::Flavor::Skinned);

    ASSERT_NE(rebuilt, nullptr);
    EXPECT_NE(second.Get(rebuilt->vertexBuffer), nullptr);
    EXPECT_NE(second.Get(rebuilt->indexBuffer), nullptr);
    EXPECT_NE(first.Get(previousVertex), nullptr);
}

TEST_F(MaterialPreviewCacheTest, SurfaceShapeRecreatesLiveBuffersAfterResourceReset)
{
    PreviewLifetimeRenderer renderer;
    renderer::ResourceManager resources(renderer);
    const auto* initial = mp::ShapeMesh(resources, mp::Shape::Cube, mp::Flavor::Surface);
    ASSERT_NE(initial, nullptr);
    ASSERT_NE(resources.Get(initial->vertexBuffer), nullptr);
    const auto previousVertex = initial->vertexBuffer;

    resources.Reset();
    const auto* rebuilt = mp::ShapeMesh(resources, mp::Shape::Cube, mp::Flavor::Surface);

    ASSERT_NE(rebuilt, nullptr);
    EXPECT_NE(resources.Get(rebuilt->vertexBuffer), nullptr);
    EXPECT_NE(resources.Get(rebuilt->indexBuffer), nullptr);
    EXPECT_NE(rebuilt->vertexBuffer, previousVertex);
}

TEST_F(MaterialPreviewCacheTest, SurfaceShapeDoesNotReuseAnotherManagersNumericHandles)
{
    PreviewLifetimeRenderer renderer;
    renderer::ResourceManager first(renderer);
    const auto* initial = mp::ShapeMesh(first, mp::Shape::Cube, mp::Flavor::Surface);
    ASSERT_NE(initial, nullptr);
    const auto previousVertex = initial->vertexBuffer;
    renderer::ResourceManager second(renderer);
    ASSERT_EQ(first.GetResetVersion(), second.GetResetVersion());

    const auto* rebuilt = mp::ShapeMesh(second, mp::Shape::Cube, mp::Flavor::Surface);

    ASSERT_NE(rebuilt, nullptr);
    EXPECT_NE(second.Get(rebuilt->vertexBuffer), nullptr);
    EXPECT_NE(second.Get(rebuilt->indexBuffer), nullptr);
    EXPECT_NE(first.Get(previousVertex), nullptr);
}

TEST_F(MaterialPreviewCacheTest, TerrainAndWaterMeshCachesReuseThenRebuildAfterResourceReset)
{
    PreviewLifetimeRenderer renderer;
    renderer::ResourceManager resources(renderer);
    const std::array<mp::Flavor, 2> flavors{mp::Flavor::Terrain, mp::Flavor::Water};
    for (const auto flavor : flavors) {
        const auto* initial = mp::ShapeMesh(resources, mp::Shape::Cube, flavor);
        ASSERT_NE(initial, nullptr);
        ASSERT_NE(resources.Get(initial->vertexBuffer), nullptr);
        const auto vertexCount = initial->vertexCount;
        const auto indexCount = initial->indexCount;
        const auto previousVertex = initial->vertexBuffer;
        EXPECT_EQ(mp::ShapeMesh(resources, mp::Shape::Sphere, flavor), initial);
        resources.Reset();
        const auto* rebuilt = mp::ShapeMesh(resources, mp::Shape::Cube, flavor);
        ASSERT_NE(rebuilt, nullptr);
        EXPECT_NE(resources.Get(rebuilt->vertexBuffer), nullptr);
        EXPECT_NE(resources.Get(rebuilt->indexBuffer), nullptr);
        EXPECT_NE(rebuilt->vertexBuffer, previousVertex);
        EXPECT_EQ(rebuilt->vertexCount, vertexCount);
        EXPECT_EQ(rebuilt->indexCount, indexCount);
    }
}

TEST_F(MaterialPreviewCacheTest, TerrainAndWaterMeshCachesKeepEachManagersResourcesAlive)
{
    PreviewLifetimeRenderer renderer;
    renderer::ResourceManager first(renderer);
    renderer::ResourceManager second(renderer);
    ASSERT_EQ(first.GetResetVersion(), second.GetResetVersion());
    EXPECT_NE(mp::PreviewResourceLifetime::Read(first), mp::PreviewResourceLifetime::Read(second));
    const std::array<mp::Flavor, 2> flavors{mp::Flavor::Terrain, mp::Flavor::Water};
    for (const auto flavor : flavors) {
        const auto* initial = mp::ShapeMesh(first, mp::Shape::Cube, flavor);
        ASSERT_NE(initial, nullptr);
        const auto firstVertex = initial->vertexBuffer;
        const auto* other = mp::ShapeMesh(second, mp::Shape::Cube, flavor);
        ASSERT_NE(other, nullptr);
        EXPECT_NE(second.Get(other->vertexBuffer), nullptr);
        EXPECT_NE(second.Get(other->indexBuffer), nullptr);
        EXPECT_NE(first.Get(firstVertex), nullptr);
    }
}

} /// @note namespace fbzz::tests
