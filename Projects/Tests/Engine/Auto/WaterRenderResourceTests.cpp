/// @file    WaterRenderResourceTests.cpp
/// @brief   本番の水面抽出を通して Scene・Entity 世代と GPU 資源の返却境界を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-07
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <TestKit/TempDir.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Core/Scheduler/SystemContext.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Scene/Components/WaterComponent.hpp>
#include <Engine/Scene/Components/RigidBodyComponent.hpp>
#include <Engine/Scene/Components/WaterExclusionComponent.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Engine/Scene/Components/WaterZoneComponent.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Script.hpp>
#include <Engine/Scene/WaterSurfaceQuery.hpp>
#include <Engine/Scene/WaterInteraction.hpp>
#include <Engine/Scene/WaterSpline.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Systems/PhysicsSystem.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Engine/Scene/Systems/WaterInteractionSystem.hpp>
#include <Engine/Scene/Systems/WaterSystem.hpp>
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <Engine/Scene/Systems/RenderPasses/Geometry/WaterRenderPass.hpp>
#include <Engine/Scene/Systems/RenderWaterExtractor.hpp>
#include <Engine/Scene/Systems/RenderEnvironmentExtractor.hpp>
#include <Graphics/Renderer/IBuffer.hpp>
#include <Graphics/Renderer/IRenderer.hpp>
#include <Graphics/Renderer/ITexture.hpp>
#include <Graphics/Renderer/RenderScene.hpp>
#include <Graphics/Renderer/ResourceManager.hpp>
#include <Math/MathUtils.hpp>
#include <Math/Quaternion.hpp>
#include <Physics/World.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

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


class WaterCpuBuffer final : public renderer::IBuffer {
public:
    WaterCpuBuffer(const void* data, size_t bytes, uint32_t stride, size_t& alive)
        : m_data(bytes), m_stride(stride), m_alive(alive)
    {
        if (data) std::memcpy(m_data.data(), data, bytes);
        ++m_alive;
    }
    ~WaterCpuBuffer() override { --m_alive; }
    void Update(const void* data, size_t bytes) override
    {
        if (data && bytes <= m_data.size()) std::memcpy(m_data.data(), data, bytes);
    }
    size_t GetSize() const override { return m_data.size(); }
    uint32_t GetStride() const override { return m_stride; }
    bool CopyData(size_t offset, size_t bytes, void* output) const override
    {
        if (!output || offset > m_data.size() || bytes > m_data.size() - offset) return false;
        std::memcpy(output, m_data.data() + offset, bytes);
        return true;
    }
private:
    std::vector<uint8_t> m_data;
    uint32_t m_stride;
    size_t& m_alive;
};

class WaterCpuTexture final : public renderer::ITexture {
public:
    WaterCpuTexture(uint32_t width, uint32_t height, uint32_t depth = 1)
        : m_width(width), m_height(height), m_depth(depth) {}
    uint32_t GetWidth() const override { return m_width; }
    uint32_t GetHeight() const override { return m_height; }
    uint32_t GetDepth() const override { return m_depth; }
private:
    uint32_t m_width, m_height, m_depth;
};

/// @note The opt-in dynamic backend observes only successful writes to its own texture; terrain-info uploads are separate.
class WaterCpuDynamicTexture final : public renderer::ITexture {
public:
    WaterCpuDynamicTexture(uint32_t width, uint32_t height,
        std::unordered_map<const renderer::ITexture*, std::vector<uint8_t>>& pixels,
        std::shared_ptr<size_t> uploads)
        : m_width(width), m_height(height), m_pixels(pixels), m_uploads(std::move(uploads))
    {
        m_pixels[this].assign(static_cast<size_t>(width) * height * 4u, 0u);
    }
    uint32_t GetWidth() const override { return m_width; }
    uint32_t GetHeight() const override { return m_height; }
    bool UpdateRegion(uint32_t x, uint32_t y, uint32_t width, uint32_t height,
        const void* pixels, uint32_t srcRowPitch) override
    {
        if (!pixels || width == 0 || height == 0 || x > m_width || y > m_height
            || width > m_width - x || height > m_height - y || srcRowPitch < width * 4u) return false;
        auto& destination = m_pixels[this];
        destination.resize(static_cast<size_t>(m_width) * m_height * 4u);
        const auto* source = static_cast<const uint8_t*>(pixels);
        for (uint32_t row = 0; row < height; ++row)
            std::memcpy(destination.data() + (static_cast<size_t>(y + row) * m_width + x) * 4u,
                source + static_cast<size_t>(row) * srcRowPitch, static_cast<size_t>(width) * 4u);
        ++*m_uploads;
        return true;
    }
private:
    uint32_t m_width, m_height;
    std::unordered_map<const renderer::ITexture*, std::vector<uint8_t>>& m_pixels;
    /// @note The fixture keeps the counter alive independently of texture destruction, so replacement failures remain observable.
    std::shared_ptr<size_t> m_uploads;
};

/// @note 資源生成・返却だけを提供し、Shader と描画は実行しない CPU のバックエンド境界。
class WaterCpuRenderer final : public renderer::IRenderer {
public:
    size_t buffersAlive = 0;
    size_t textureUploads = 0;
    bool recordDynamicRippleUploads = false;
    size_t rippleTextureCreations = 0;
    std::shared_ptr<size_t> rippleTextureUploads;
    std::function<void()> beforeVertexBuffer;
    std::unordered_map<const renderer::ITexture*, std::vector<uint8_t>> texturePixels;
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
    std::unique_ptr<renderer::IBuffer> CreateNativeVertexBuffer(const void* data, size_t bytes, uint32_t stride) override
    {
        if (beforeVertexBuffer) {
            const auto callback = std::move(beforeVertexBuffer);
            beforeVertexBuffer = {};
            callback();
        }
        return std::make_unique<WaterCpuBuffer>(data, bytes, stride, buffersAlive);
    }
    std::unique_ptr<renderer::IBuffer> CreateNativeIndexBuffer(const void* data, uint32_t count) override
    { return std::make_unique<WaterCpuBuffer>(data, static_cast<size_t>(count) * sizeof(uint32_t), 0, buffersAlive); }
    std::unique_ptr<renderer::ITexture> CreateNativeTextureFromData(const uint8_t* pixels, uint32_t width, uint32_t height) override
    {
        ++textureUploads;
        auto texture = std::make_unique<WaterCpuTexture>(width, height);
        if (pixels) texturePixels[texture.get()] = std::vector<uint8_t>(pixels, pixels + static_cast<size_t>(width) * height * 4u);
        return texture;
    }
    std::unique_ptr<renderer::ITexture> CreateNativeDynamicTexture(
        uint32_t width, uint32_t height, renderer::DynamicTextureFormat format) override
    {
        if (!recordDynamicRippleUploads || format != renderer::DynamicTextureFormat::RGBA8
            || width != scene::kWaterRippleTextureSize || height != scene::kWaterRippleTextureSize) return {};
        ++rippleTextureCreations;
        rippleTextureUploads = std::make_shared<size_t>(0);
        return std::make_unique<WaterCpuDynamicTexture>(width, height, texturePixels, rippleTextureUploads);
    }
    std::unique_ptr<renderer::ITexture> CreateNativeTextureFromDataMips(const renderer::TextureMipData* mips, uint32_t count) override
    { return mips && count ? std::make_unique<WaterCpuTexture>(mips[0].width, mips[0].height) : nullptr; }
    std::unique_ptr<renderer::ITexture> CreateNativeTexture3DFromData(const uint8_t*, uint32_t width, uint32_t height, uint32_t depth) override
    { return std::make_unique<WaterCpuTexture>(width, height, depth); }
    std::unique_ptr<renderer::IConstantBuffer> CreateNativeConstantBuffer(size_t) override { return {}; }
    std::unique_ptr<renderer::IShader> CreateNativeShader(const std::string&) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeTexture(const std::string&) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeTextureFromRenderTarget(renderer::IRenderTarget&, uint32_t, renderer::RenderTargetTextureKind) override { return {}; }
    std::unique_ptr<renderer::IPipelineState> CreateNativePipelineState(const renderer::PipelineStateDesc&) override { return {}; }
    std::unique_ptr<renderer::IRenderTarget> CreateNativeRenderTarget(uint32_t, uint32_t, const renderer::RenderTargetDesc&) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeComputeTexture(uint32_t, uint32_t) override { return {}; }
    std::unique_ptr<renderer::IStructuredBuffer> CreateNativeStructuredBuffer(const void*, uint32_t, uint32_t) override { return {}; }
    std::unique_ptr<renderer::IStructuredBuffer> CreateNativeRWStructuredBuffer(const void*, uint32_t, uint32_t) override { return {}; }
};
} /// @note namespace

class WaterRenderResourceTest : public testkit::EngineFixture {
protected:
    void SetUp() override
    {
        EngineFixture::SetUp();
        ASSERT_TRUE(m_temp.IsValid());
        ASSERT_EQ(renderer::ResourceManager::Active(), nullptr);
        m_resources = std::make_unique<renderer::ResourceManager>(m_renderer);
        asset::AssetManager::Init(*m_resources, m_temp.Path().generic_string() + "/");
        m_assetsInitialized = true;
    }
    void TearDown() override
    {
        /// @note 各テストの Scene は先に破棄し、共有 Noise / Atlas を含む manager 自身は最後に畳む。
        if (m_assetsInitialized) asset::AssetManager::UnloadAll();
        if (m_resources) m_resources->Reset();
        EXPECT_EQ(m_renderer.buffersAlive, 0u);
        m_resources.reset();
        EXPECT_EQ(renderer::ResourceManager::Active(), nullptr);
        EngineFixture::TearDown();
    }
    scene::EntityID AddWater(scene::Scene& scene, bool adaptive = false)
    {
        auto& object = scene.CreateGameObject("Water");
        auto& water = object.AddComponent<scene::WaterComponent>();
        water.enableGerstnerWaves = false;
        water.resolutionX = water.resolutionZ = 4;
        water.chunkCount = 1;
        water.lodPatchResolution = 4;
        water.adaptivePatchLod = adaptive;
        return object.GetID();
    }
    renderer::RenderWaterInput Extract(scene::Scene& scene, uint64_t frameStamp = 0,
        const renderer::Camera* cameraOverride = nullptr)
    {
        renderer::Camera camera;
        if (cameraOverride) camera = *cameraOverride;
        renderer::RenderSettings settings;
        renderer::RenderPassHandles handles;
        scene::RenderPassContext context(scene, m_renderer, *m_resources, camera, settings, {}, UINT32_MAX, handles);
        context.frameStamp = frameStamp;
        renderer::RenderScene output;
        scene::ExtractRenderWater(context, output);
        EXPECT_EQ(output.water.size(), 1u);
        return output.water.empty() ? renderer::RenderWaterInput{} : std::move(output.water.front());
    }
    renderer::RenderEnvironmentInput ExtractEnvironment(scene::Scene& scene, renderer::Camera& camera,
        uint32_t cullingMask = UINT32_MAX)
    {
        renderer::RenderSettings settings;
        renderer::RenderPassHandles handles;
        scene::RenderPassContext context(scene, m_renderer, *m_resources, camera, settings, {}, cullingMask, handles);
        return scene::ExtractRenderEnvironment(context);
    }
    std::string SaveMaterial(const std::string& name, const asset::MaterialAssetAuthoring& material)
    {
        const auto path = m_temp.File(name);
        if (!asset::SaveMaterialAssetToFile(path.generic_string(), material)) return {};
        return path.generic_string();
    }

    /// @note Compare production texture bytes against the full canonical scan, without spatial or zero-tail rejection.
    void ExpectCanonicalRipplePixels(scene::Scene& scene, scene::EntityID id, uint64_t frameStamp = 0)
    {
        const auto* water = scene.GetComponent<scene::WaterComponent>(id);
        ASSERT_NE(water, nullptr);
        const auto input = Extract(scene, frameStamp);
        const auto* texture = m_resources->Get(input.ripple);
        ASSERT_NE(texture, nullptr);
        const auto found = m_renderer.texturePixels.find(texture);
        ASSERT_NE(found, m_renderer.texturePixels.end());
        const auto& pixels = found->second;
        const auto worldMatrix = scene.GetGameObject(id)->transform.GetWorldMatrix();
        const float inverseHeightScale = 1.0f / scene::kWaterRippleHeightScale;
        size_t differentBytes = 0;
        for (uint32_t y = 0; y < texture->GetHeight(); ++y) {
            for (uint32_t x = 0; x < texture->GetWidth(); ++x) {
                const math::Vector2 uv{(static_cast<float>(x) + 0.5f) / texture->GetWidth(),
                    (static_cast<float>(y) + 0.5f) / texture->GetHeight()};
                const auto point = worldMatrix * math::Vector4{(uv.x - 0.5f) * water->extentX,
                    0.0f, (uv.y - 0.5f) * water->extentZ, 1.0f};
                math::Vector2 offset = math::Vector2::ZERO;
                float height = 0.0f;
                for (const auto& ripple : water->ripples) {
                    if (ripple.amplitude <= 0.0f) continue;
                    const math::Vector2 delta{point.x - ripple.center.x, point.z - ripple.center.y};
                    const float distance = std::sqrt(delta.LengthSq());
                    if (distance <= math::EPSILON) continue;
                    const float wave = scene::WaterComponent::WaterRippleWave(ripple, distance) * ripple.amplitude;
                    offset += (delta / distance) * wave;
                    height += wave * ripple.meshFade * scene::kWaterRippleHeightAmplitude;
                }
                const uint8_t expected[]{
                    static_cast<uint8_t>(math::Clamp01(0.5f + offset.x * 0.5f) * 255.0f),
                    static_cast<uint8_t>(math::Clamp01(0.5f + offset.y * 0.5f) * 255.0f),
                    static_cast<uint8_t>(math::Clamp01(0.5f + height * inverseHeightScale * 0.5f) * 255.0f), 255u};
                const size_t pixel = (static_cast<size_t>(y) * texture->GetWidth() + x) * 4u;
                for (size_t channel = 0; channel < 4; ++channel)
                    differentBytes += pixels[pixel + channel] != expected[channel];
            }
        }
        EXPECT_EQ(differentBytes, 0u);
    }

    WaterCpuRenderer m_renderer;
    std::unique_ptr<renderer::ResourceManager> m_resources;
    testkit::TempDir m_temp{"water-render-resources"};
    bool m_assetsInitialized = false;
};

TEST_F(WaterRenderResourceTest, KeepsSameIndexScenesIndependentThroughClearAndDestruction)
{
    auto first = std::make_unique<scene::Scene>();
    scene::Scene second;
    const auto firstId = AddWater(*first);
    const auto secondId = AddWater(second);
    ASSERT_EQ(firstId, secondId);
    const auto firstInput = Extract(*first);
    const auto secondInput = Extract(second);
    ASSERT_EQ(firstInput.patches.size(), 1u);
    ASSERT_EQ(secondInput.patches.size(), 1u);
    const auto firstVb = firstInput.patches[0].vertexBuffer;
    const auto secondVb = secondInput.patches[0].vertexBuffer;
    EXPECT_NE(firstVb, secondVb);
    ASSERT_NE(m_resources->Get(firstVb), nullptr);
    ASSERT_NE(m_resources->Get(secondVb), nullptr);
    ASSERT_NE(m_resources->Get(firstInput.detailNoise), nullptr);
    EXPECT_EQ(firstInput.detailNoise, secondInput.detailNoise);

    first->Clear();
    EXPECT_EQ(m_resources->Get(firstVb), nullptr);
    EXPECT_EQ(m_resources->Get(firstInput.patches[0].indexBuffer), nullptr);
    EXPECT_EQ(m_resources->Get(firstInput.foam), nullptr);
    EXPECT_EQ(m_resources->Get(firstInput.ripple), nullptr);
    EXPECT_NE(m_resources->Get(secondVb), nullptr);
    EXPECT_NE(m_resources->Get(secondInput.foam), nullptr);
    EXPECT_NE(m_resources->Get(secondInput.ripple), nullptr);
    EXPECT_NE(m_resources->Get(secondInput.detailNoise), nullptr);
    EXPECT_NE(m_resources->Get(secondInput.velocityField), nullptr);

    EXPECT_EQ(AddWater(*first), firstId);
    const auto replaced = Extract(*first);
    ASSERT_EQ(replaced.patches.size(), 1u);
    EXPECT_NE(replaced.patches[0].vertexBuffer, firstVb);
    first.reset();
    EXPECT_EQ(m_resources->Get(replaced.patches[0].vertexBuffer), nullptr);
    EXPECT_EQ(m_resources->Get(replaced.foam), nullptr);
    EXPECT_NE(m_resources->Get(secondVb), nullptr);
    EXPECT_NE(m_resources->Get(secondInput.detailNoise), nullptr);
    EXPECT_EQ(m_renderer.buffersAlive, 2u);
}

TEST_F(WaterRenderResourceTest, SharedMaterialPreparationReappliesCurrentWithoutChangingSimulationPublication)
{
    scene::Scene source;
    const auto id = AddWater(source);
    auto* water = source.GetComponent<scene::WaterComponent>(id);
    asset::MaterialAssetAuthoring authoring;
    authoring.params[scene::water_keys::kFlowDirection] = {3.0f, 4.0f};
    authoring.params[scene::water_keys::kCurrentSpeed] = {5.0f};
    const auto path = SaveMaterial("SharedCurrent.mat", authoring);
    ASSERT_FALSE(path.empty());
    water->materialPath = path;
    scene::FlushWorldTransforms(source);
    scene::PrepareWaterSimulationSurfaces(source);
    ASSERT_TRUE(water->simulationSurfacePublished);
    EXPECT_NEAR(water->current.x, 3.0f, testkit::kTolerance);
    EXPECT_NEAR(water->current.y, 4.0f, testkit::kTolerance);
    source.GetGameObject(id)->transform.position = {4.0f, 2.0f, 0.0f};
    scene::FlushWorldTransforms(source);
    water->current = {99.0f, 99.0f};
    scene::PrepareWaterSceneSurfaces(source);
    EXPECT_NEAR(water->current.x, 3.0f, testkit::kTolerance);
    EXPECT_VEC3_NEAR(scene::WaterSurfaceTransform(*water, source.GetGameObject(id)->transform,
        scene::WaterSurfaceReadView::SIMULATION).worldPosition, math::Vector3::ZERO, testkit::kTolerance);
    EXPECT_VEC3_NEAR(scene::WaterSurfaceTransform(*water, source.GetGameObject(id)->transform,
        scene::WaterSurfaceReadView::LIVE_QUERY).worldPosition, (math::Vector3{4.0f, 2.0f, 0.0f}), testkit::kTolerance);

    const auto handle = asset::AssetManager::Load<asset::MaterialAsset>(path);
    auto* material = asset::AssetManager::Get(handle);
    ASSERT_NE(material, nullptr);
    const auto previous = material->ReadAuthoring();
    EditMaterialAuthoring(*material, [](auto& values) {
        values.params[scene::water_keys::kFlowDirection] = {-1.0f, 0.0f};
        values.params[scene::water_keys::kCurrentSpeed] = {2.0f};
    });
    renderer::Camera camera;
    camera.m_position = {0.0f, -1.0f, 0.0f};
    ExtractEnvironment(source, camera);
    EXPECT_NEAR(water->current.x, -2.0f, testkit::kTolerance);
    EXPECT_FLOAT_EQ(previous->Data().params.at(scene::water_keys::kCurrentSpeed)[0], 5.0f);
    Extract(source);
    EXPECT_NEAR(water->current.x, -2.0f, testkit::kTolerance);
    asset::AssetManager::Unload<asset::MaterialAsset>(path);
    authoring.params[scene::water_keys::kCurrentSpeed] = {10.0f};
    ASSERT_EQ(SaveMaterial("SharedCurrent.mat", authoring), path);
    scene::PrepareWaterSceneSurfaces(source);
    EXPECT_NEAR(water->current.x, 6.0f, testkit::kTolerance);
    EXPECT_NEAR(water->current.y, 8.0f, testkit::kTolerance);
    EXPECT_TRUE(water->simulationSurfacePublished);
}

TEST_F(WaterRenderResourceTest, RepeatedMaterialPreparationRetainsZoneAndExclusionSideEffects)
{
    scene::Scene source;
    const auto ownerId = AddWater(source);
    const auto memberId = AddWater(source);
    auto* owner = source.GetComponent<scene::WaterComponent>(ownerId);
    auto* member = source.GetComponent<scene::WaterComponent>(memberId);
    owner->extentX = owner->extentZ = 8.0f;
    member->extentX = member->extentZ = 4.0f;
    auto* memberObject = source.GetGameObject(memberId);
    memberObject->transform.position = {4.0f, 0.0f, 0.0f};
    auto& zone = source.GetGameObject(ownerId)->AddComponent<scene::WaterZoneComponent>();
    zone.members.push_back({scene::EntityRef{memberId}, 0});
    auto& dryObject = source.CreateGameObject("Dry box");
    dryObject.transform.position = {4.0f, 0.0f, 0.0f};
    auto& exclusion = dryObject.AddComponent<scene::WaterExclusionComponent>();
    exclusion.size = {2.0f, 2.0f, 2.0f};
    scene::FlushWorldTransforms(source);
    scene::PrepareWaterSceneSurfaces(source);
    ASSERT_NE(zone.snapshot, nullptr) << zone.diagnostic;
    const auto firstZone = zone.snapshot;
    ASSERT_EQ(member->zoneOwner, ownerId);
    ASSERT_NE(owner->exclusionSnapshot, nullptr);
    member->ripples.emplace_back();
    member->surfaceFlowCount = 1;
    member->zoneOwner = scene::EntityID::INVALID;
    owner->zoneSnapshot.reset();
    scene::PrepareWaterSceneSurfaces(source);
    EXPECT_EQ(zone.snapshot, firstZone);
    EXPECT_EQ(owner->zoneSnapshot, firstZone);
    EXPECT_EQ(member->zoneOwner, ownerId);
    EXPECT_TRUE(member->ripples.empty());
    EXPECT_EQ(member->surfaceFlowCount, 0);
    exclusion.enabled = false;
    scene::PrepareWaterSceneSurfaces(source);
    EXPECT_EQ(owner->exclusionSnapshot, nullptr);
    memberObject->transform.position = {6.0f, 0.0f, 0.0f};
    scene::FlushWorldTransforms(source);
    scene::PrepareWaterSceneSurfaces(source);
    EXPECT_NE(zone.snapshot, firstZone);
}

TEST_F(WaterRenderResourceTest, NestedWaterExtractionKeepsOuterTypedCandidatesAndComponentOrder)
{
    scene::Scene source;
    const auto earlyId = source.CreateGameObject("Early owner").GetID();
    const auto laterId = AddWater(source);
    source.GetGameObject(laterId)->layer = 2;
    auto& earlyWater = source.GetGameObject(earlyId)->AddComponent<scene::WaterComponent>();
    earlyWater.enableGerstnerWaves = false;
    earlyWater.resolutionX = earlyWater.resolutionZ = 4;
    earlyWater.chunkCount = 1;
    source.GetGameObject(earlyId)->layer = 3;
    scene::Scene nested;
    AddWater(nested);
    bool nestedCalled = false;
    m_renderer.beforeVertexBuffer = [&] {
        nestedCalled = true;
        const auto inner = Extract(nested);
        EXPECT_EQ(inner.patches.size(), 1u);
    };
    renderer::Camera camera;
    renderer::RenderSettings settings;
    renderer::RenderPassHandles handles;
    scene::RenderPassContext context(source, m_renderer, *m_resources, camera, settings, {}, UINT32_MAX, handles);
    renderer::RenderScene output;
    scene::ExtractRenderWater(context, output);
    EXPECT_TRUE(nestedCalled);
    ASSERT_EQ(output.water.size(), 2u);
    EXPECT_EQ(output.water[0].layer, 2u);
    EXPECT_EQ(output.water[1].layer, 3u);
    source.RemoveComponent<scene::WaterComponent>(laterId);
    const auto remaining = Extract(source);
    EXPECT_EQ(remaining.layer, 3u);
}

TEST_F(WaterRenderResourceTest, ReleasesDestroyedEntityGenerationBeforeReusingItsIndex)
{
    scene::Scene scene;
    const auto oldId = AddWater(scene);
    const auto oldInput = Extract(scene);
    ASSERT_EQ(oldInput.patches.size(), 1u);
    ASSERT_NE(m_resources->Get(oldInput.patches[0].vertexBuffer), nullptr);
    ASSERT_NE(m_resources->Get(oldInput.foam), nullptr);
    ASSERT_TRUE(scene.DestroyGameObject(oldId));
    EXPECT_EQ(m_resources->Get(oldInput.patches[0].vertexBuffer), nullptr);
    EXPECT_EQ(m_resources->Get(oldInput.patches[0].indexBuffer), nullptr);
    EXPECT_EQ(m_resources->Get(oldInput.foam), nullptr);
    EXPECT_EQ(m_resources->Get(oldInput.ripple), nullptr);
    const auto newId = AddWater(scene);
    EXPECT_EQ(newId.index, oldId.index);
    EXPECT_NE(newId.generation, oldId.generation);
    const auto newInput = Extract(scene);
    ASSERT_EQ(newInput.patches.size(), 1u);
    EXPECT_NE(newInput.patches[0].vertexBuffer, oldInput.patches[0].vertexBuffer);
    scene::ReleaseWaterRenderResources(scene, oldId, *m_resources);
    EXPECT_NE(m_resources->Get(newInput.patches[0].vertexBuffer), nullptr);
    EXPECT_NE(m_resources->Get(newInput.foam), nullptr);
    EXPECT_NE(m_resources->Get(newInput.ripple), nullptr);
    EXPECT_NE(m_resources->Get(newInput.detailNoise), nullptr);
    EXPECT_EQ(m_renderer.buffersAlive, 2u);
}

TEST_F(WaterRenderResourceTest, ReleasesSharedAdaptiveBuffersAcrossGridRebuildsWithoutAffectingOtherResources)
{
    scene::Scene scene;
    const auto id = AddWater(scene, true);
    const auto adaptive = Extract(scene);
    ASSERT_TRUE(adaptive.patches.empty());
    ASSERT_NE(m_resources->Get(adaptive.patchVertexBuffer), nullptr);
    for (const auto buffer : adaptive.patchIndexBuffers) ASSERT_NE(m_resources->Get(buffer), nullptr);
    EXPECT_EQ(m_renderer.buffersAlive, 17u);
    auto* water = scene.GetComponent<scene::WaterComponent>(id);
    ASSERT_NE(water, nullptr);
    water->adaptivePatchLod = false;
    water->chunkCount = 2;
    water->meshDirty = true;
    const auto grid = Extract(scene);
    ASSERT_EQ(grid.patches.size(), 4u);
    EXPECT_EQ(m_resources->Get(adaptive.patchVertexBuffer), nullptr);
    for (const auto buffer : adaptive.patchIndexBuffers) EXPECT_EQ(m_resources->Get(buffer), nullptr);
    EXPECT_EQ(m_renderer.buffersAlive, 8u);
    const uint32_t canaryData = 0;
    const auto canary = m_resources->CreateVertexBuffer(&canaryData, sizeof(canaryData), sizeof(canaryData));
    ASSERT_NE(m_resources->Get(canary), nullptr);
    water->adaptivePatchLod = true;
    water->meshDirty = true;
    const auto rebuilt = Extract(scene);
    EXPECT_TRUE(rebuilt.patches.empty());
    for (const auto& patch : grid.patches) {
        EXPECT_EQ(m_resources->Get(patch.vertexBuffer), nullptr);
        EXPECT_EQ(m_resources->Get(patch.indexBuffer), nullptr);
    }
    EXPECT_EQ(m_renderer.buffersAlive, 18u);
    scene::ReleaseWaterRenderResources(scene, *m_resources);
    EXPECT_EQ(m_resources->Get(rebuilt.patchVertexBuffer), nullptr);
    for (const auto buffer : rebuilt.patchIndexBuffers) EXPECT_EQ(m_resources->Get(buffer), nullptr);
    EXPECT_EQ(m_resources->Get(rebuilt.foam), nullptr);
    EXPECT_EQ(m_resources->Get(rebuilt.ripple), nullptr);
    EXPECT_NE(m_resources->Get(rebuilt.detailNoise), nullptr);
    EXPECT_NE(m_resources->Get(rebuilt.velocityField), nullptr);
    EXPECT_NE(m_resources->Get(canary), nullptr);
    scene::ReleaseWaterRenderResources(scene, *m_resources);
    scene.Clear();
    EXPECT_NE(m_resources->Get(canary), nullptr);
    EXPECT_EQ(m_renderer.buffersAlive, 1u);
    m_resources->Release(canary);
}
TEST_F(WaterRenderResourceTest, AuxiliaryDepthTextureWithoutTerrainPreservesUniformPhysicalDepth)
{
    scene::Scene scene;
    const auto id = AddWater(scene);
    auto* water = scene.GetComponent<scene::WaterComponent>(id);
    water->buoyancyDepth = 24.0f;
    const auto input = Extract(scene);
    EXPECT_FLOAT_EQ(input.constants.terrainInfoParams.x, 1.0f);
    EXPECT_TRUE(input.medium.depthIsUniform);
}

TEST_F(WaterRenderResourceTest, UnchangedRippleInputsReuseTheTextureAcrossViews)
{
    scene::Scene scene;
    const auto id = AddWater(scene);
    auto* water = scene.GetComponent<scene::WaterComponent>(id);
    scene::WaterRipple ripple; ripple.radius = 1; ripple.amplitude = 0.5f; ripple.meshFade = 1;
    water->ripples.push_back(ripple);
    const auto first = Extract(scene, 1);
    const auto repeated = Extract(scene, 1);
    EXPECT_TRUE(first.ripple.IsValid());
    EXPECT_EQ(first.ripple, repeated.ripple);
}

TEST_F(WaterRenderResourceTest, RippleZeroTailRejectionPreservesTheCanonicalBakedPixels)
{
    scene::Scene scene;
    const auto id = AddWater(scene);
    auto* water = scene.GetComponent<scene::WaterComponent>(id);
    water->extentX = 32.0f; water->extentZ = 20.0f;
    scene::WaterRipple first; first.center = {2.3f, -1.7f}; first.radius = 4.1f;
    first.width = 0.08f; first.amplitude = 0.7f; first.meshFade = 0.8f;
    scene::WaterRipple second = first; second.center = {-3.1f, 2.8f}; second.radius = 1.7f;
    second.width = 0.3f; second.amplitude = 0.4f; second.meshFade = 0.2f;
    water->ripples = {first, second};
    const auto input = Extract(scene);
    const auto* texture = m_resources->Get(input.ripple);
    ASSERT_NE(texture, nullptr);
    const auto& pixels = m_renderer.texturePixels.at(texture);
    const auto& transform = scene.GetGameObject(id)->transform;
    const auto worldMatrix = transform.GetWorldMatrix();
    uint32_t difference = 0;
    for (uint32_t y = 0; y < texture->GetHeight(); ++y) for (uint32_t x = 0; x < texture->GetWidth(); ++x) {
        const math::Vector2 uv{(static_cast<float>(x) + 0.5f) / texture->GetWidth(),
                              (static_cast<float>(y) + 0.5f) / texture->GetHeight()};
        const auto point = worldMatrix * math::Vector4{(uv.x - 0.5f) * water->extentX, 0,
                                                       (uv.y - 0.5f) * water->extentZ, 1};
        math::Vector2 offset{}; float height = 0;
        for (const auto& ripple : water->ripples) {
            const math::Vector2 delta{point.x - ripple.center.x, point.z - ripple.center.y};
            const float distance = delta.Length();
            if (distance <= math::EPSILON) continue;
            const float wave = scene::WaterComponent::WaterRippleWave(ripple, distance) * ripple.amplitude;
            offset += delta.Normalized() * wave;
            height += wave * ripple.meshFade * scene::kWaterRippleHeightAmplitude;
        }
        const uint8_t expected[]{static_cast<uint8_t>(math::Clamp01(0.5f + offset.x * 0.5f) * 255),
            static_cast<uint8_t>(math::Clamp01(0.5f + offset.y * 0.5f) * 255),
            static_cast<uint8_t>(math::Clamp01(0.5f + height / scene::kWaterRippleHeightScale * 0.5f) * 255), 255};
        const size_t pixel = (static_cast<size_t>(y) * texture->GetWidth() + x) * 4u;
        for (size_t channel = 0; channel < 4; ++channel) difference += pixels[pixel + channel] != expected[channel];
    }
    EXPECT_EQ(difference, 0u);
}

TEST_F(WaterRenderResourceTest, RippleBakePreservesCanonicalPixelsAcrossCountsTransformsAndBoundaries)
{
    struct Scenario {
        size_t rippleCount;
        math::Vector2 extent;
        math::Vector3 scale;
        float yaw;
        math::Vector3 position;
        bool fullSupport = false;
    };
    const Scenario scenarios[]{
        {0u, {32.0f, 20.0f}, math::Vector3::ONE, 0.0f, math::Vector3::ZERO},
        {1u, {32.0f, 20.0f}, math::Vector3::ONE, 0.0f, math::Vector3::ZERO},
        {scene::kMaxWaterRipples, {32.0f, 20.0f}, math::Vector3::ONE, 0.0f, math::Vector3::ZERO},
        {scene::kMaxWaterRipples, {128.0f, 7.0f}, {-0.5f, 1.0f, 3.0f}, 0.37f, {17.0f, 2.0f, -9.0f}},
        {scene::kMaxWaterRipples, {0.1f, 1024.0f}, {3.0f, 1.0f, -0.25f}, -0.81f, {-3.0f, 0.0f, 4.0f}},
        {scene::kMaxWaterRipples, {8192.0f, 4096.0f}, {-2.0f, -1.0f, -0.5f}, 1.2f, {1048576.0f, 0.0f, -524288.0f}},
        {scene::kMaxWaterRipples, {8192.0f, 4096.0f}, {256.0f, 1.0f, -256.0f}, 0.0f, {1048576.0f, 0.0f, -524288.0f}},
        {scene::kMaxWaterRipples, {8.0f, 8.0f}, math::Vector3::ONE, 0.0f, math::Vector3::ZERO, true}
    };
    const float widths[]{-0.05f, 0.0f, 1.0e-6f, 0.02f, 0.12f, 2.0f};
    for (size_t scenarioIndex = 0; scenarioIndex < std::size(scenarios); ++scenarioIndex) {
        SCOPED_TRACE(scenarioIndex);
        const auto& scenario = scenarios[scenarioIndex];
        scene::Scene scene;
        const auto id = AddWater(scene);
        auto* water = scene.GetComponent<scene::WaterComponent>(id);
        auto* object = scene.GetGameObject(id);
        water->extentX = scenario.extent.x; water->extentZ = scenario.extent.y;
        object->transform.position = scenario.position;
        object->transform.rotation = math::Quaternion::FromAxisAngle(math::Vector3::UP, scenario.yaw);
        object->transform.scale = scenario.scale;
        scene::FlushWorldTransforms(scene);
        const auto matrix = object->transform.GetWorldMatrix();
        for (size_t index = 0; index < scenario.rippleCount; ++index) {
            scene::WaterRipple ripple;
            const float u = index == 0u ? 16.5f / 256.0f
                : (static_cast<float>(index % 6u) - 0.5f) / 4.0f;
            const float v = index == 0u ? 15.5f / 256.0f
                : (static_cast<float>(index / 6u) - 0.5f) / 2.0f;
            const auto center = matrix * math::Vector4{(u - 0.5f) * water->extentX,
                0.0f, (v - 0.5f) * water->extentZ, 1.0f};
            ripple.center = {center.x, center.z};
            ripple.radius = index == 0u ? 0.0f : (index == 23u ? 1.0e20f : 0.1f + static_cast<float>(index % 7u));
            ripple.width = index == 0u ? 0.2f : widths[index % std::size(widths)];
            if (index >= 6u && index < 12u) {
                const auto boundary = matrix * math::Vector4{(16.5f / 256.0f - 0.5f) * water->extentX,
                    0.0f, (15.5f / 256.0f - 0.5f) * water->extentZ, 1.0f};
                const float distance = math::Vector2{boundary.x - center.x, boundary.z - center.z}.Length();
                const float width = (std::max)(ripple.width, 1.0e-4f);
                const float radius = index < 9u ? distance - width * 16.0f : distance + width * 22.0f;
                ripple.radius = std::nextafter(radius, index % 2u == 0u
                    ? std::numeric_limits<float>::infinity() : -std::numeric_limits<float>::infinity());
            }
            ripple.amplitude = 0.1f + static_cast<float>(index % 5u) * 0.15f;
            ripple.meshFade = static_cast<float>(index % 4u) / 3.0f;
            if (scenario.fullSupport) {
                ripple.center = {scenario.position.x, scenario.position.z};
                ripple.radius = 0.5f; ripple.width = 2.0f;
            }
            water->ripples.push_back(ripple);
        }
        ExpectCanonicalRipplePixels(scene, id);
    }
}

TEST_F(WaterRenderResourceTest, InitializesNeutralRippleTextureOnceWithoutActiveRipples)
{
    m_renderer.recordDynamicRippleUploads = true;
    scene::Scene scene;
    const auto id = AddWater(scene);
    ExpectCanonicalRipplePixels(scene, id, 1u);
    ASSERT_EQ(m_renderer.rippleTextureCreations, 1u);
    const auto rippleUploads = m_renderer.rippleTextureUploads;
    ASSERT_NE(rippleUploads, nullptr);
    EXPECT_EQ(*rippleUploads, 1u);
    ExpectCanonicalRipplePixels(scene, id, 2u);
    EXPECT_EQ(*rippleUploads, 1u);
    EXPECT_EQ(m_renderer.rippleTextureCreations, 1u);
}

TEST_F(WaterRenderResourceTest, RippleSignatureTracksInputsAndClearsOnceAfterTheLastRipple)
{
    m_renderer.recordDynamicRippleUploads = true;
    scene::Scene scene;
    const auto id = AddWater(scene);
    auto* water = scene.GetComponent<scene::WaterComponent>(id);
    scene::WaterRipple ripple;
    ripple.radius = 3.0f; ripple.width = 0.4f; ripple.amplitude = 0.8f; ripple.meshFade = 1.0f;
    water->ripples.push_back(ripple);
    ExpectCanonicalRipplePixels(scene, id, 1u);
    ASSERT_EQ(m_renderer.rippleTextureCreations, 1u);
    const auto rippleUploads = m_renderer.rippleTextureUploads;
    ASSERT_NE(rippleUploads, nullptr);
    ASSERT_EQ(*rippleUploads, 1u);
    const auto first = Extract(scene, 1u);
    renderer::Camera otherCamera;
    otherCamera.m_position = {19.0f, 7.0f, -13.0f};
    const float previousTime = Time::time;
    Time::time += 7.0f;
    const auto otherView = Extract(scene, 2u, &otherCamera);
    Time::time = previousTime;
    EXPECT_EQ(otherView.ripple, first.ripple);
    EXPECT_EQ(*rippleUploads, 1u);

    water->ripples.front().radius += 0.125f;
    ExpectCanonicalRipplePixels(scene, id, 3u);
    EXPECT_EQ(*rippleUploads, 2u);
    water->ripples.front().center.x += 0.25f;
    water->ripples.front().width += 0.1f;
    water->ripples.front().amplitude *= 0.5f;
    water->ripples.front().meshFade *= 0.5f;
    ExpectCanonicalRipplePixels(scene, id, 4u);
    EXPECT_EQ(*rippleUploads, 3u);
    scene.GetGameObject(id)->transform.position.x += 1.0f;
    scene::FlushWorldTransforms(scene);
    ExpectCanonicalRipplePixels(scene, id, 5u);
    EXPECT_EQ(*rippleUploads, 4u);
    water->extentX *= 0.5f;
    ExpectCanonicalRipplePixels(scene, id, 6u);
    EXPECT_EQ(*rippleUploads, 5u);

    water->ripples.clear();
    ExpectCanonicalRipplePixels(scene, id, 7u);
    EXPECT_EQ(*rippleUploads, 6u);
    ExpectCanonicalRipplePixels(scene, id, 8u);
    EXPECT_EQ(*rippleUploads, 6u);
    EXPECT_EQ(m_renderer.rippleTextureCreations, 1u);
    EXPECT_EQ(Extract(scene, 8u).ripple, first.ripple);
}

TEST_F(WaterRenderResourceTest, NonFiniteActiveRipplesRemainUnsupportedAndInactiveRipplesStayNeutral)
{
    const float nonFinite = std::numeric_limits<float>::quiet_NaN();
    for (size_t field = 0; field < 5u; ++field) {
        SCOPED_TRACE(field);
        scene::Scene scene;
        const auto id = AddWater(scene);
        auto* water = scene.GetComponent<scene::WaterComponent>(id);
        scene::WaterRipple ripple;
        ripple.amplitude = 0.5f;
        if (field == 0u) ripple.center.x = nonFinite;
        if (field == 1u) ripple.radius = nonFinite;
        if (field == 2u) ripple.width = nonFinite;
        if (field == 3u) ripple.amplitude = nonFinite;
        if (field == 4u) ripple.meshFade = nonFinite;
        water->ripples.push_back(ripple);
        renderer::Camera camera;
        renderer::RenderSettings settings;
        renderer::RenderPassHandles handles;
        scene::RenderPassContext context(scene, m_renderer, *m_resources, camera, settings, {}, UINT32_MAX, handles);
        renderer::RenderScene output;
        scene::ExtractRenderWater(context, output);
        EXPECT_TRUE(output.water.empty());
    }
    scene::Scene scene;
    const auto id = AddWater(scene);
    auto* water = scene.GetComponent<scene::WaterComponent>(id);
    scene::WaterRipple inactive;
    inactive.amplitude = 0.0f; inactive.center.x = nonFinite; inactive.width = nonFinite;
    water->ripples.push_back(inactive);
    ExpectCanonicalRipplePixels(scene, id);
    inactive.amplitude = -0.5f;
    water->ripples.front() = inactive;
    ExpectCanonicalRipplePixels(scene, id, 1u);
}

TEST_F(WaterRenderResourceTest, TerrainEditsRebakeInfoWithoutChangingWaterGeometryOrRebakingUnchangedViews)
{
    scene::Scene scene;
    const auto waterId = AddWater(scene);
    auto* water = scene.GetComponent<scene::WaterComponent>(waterId);
    ASSERT_NE(water, nullptr);
    water->extentX = water->extentZ = 4.0f;
    auto& ground = scene.CreateGameObject("Shore");
    auto& terrain = ground.AddComponent<scene::TerrainComponent>();
    terrain.columns = terrain.rows = 3;
    terrain.cellSize = 2.0f;
    terrain.InitFlat(-0.1f);
    ground.transform.worldPosition = {-2.0f, 0.0f, -2.0f};
    const auto first = Extract(scene);
    EXPECT_FLOAT_EQ(first.constants.terrainInfoParams.x, 1.0f);
    EXPECT_FALSE(first.medium.depthIsUniform);
    const auto repeated = Extract(scene);
    EXPECT_EQ(repeated.foam, first.foam);
    terrain.InitFlat(-1.0f);
    terrain.RequestHeightRebuild();
    const auto changed = Extract(scene);
    EXPECT_NE(changed.foam, first.foam);
    EXPECT_EQ(m_resources->Get(first.foam), nullptr);
    EXPECT_EQ(changed.patches[0].vertexBuffer, first.patches[0].vertexBuffer);
    EXPECT_EQ(changed.ripple, first.ripple);
    terrain.enabled = false;
    const auto hidden = Extract(scene);
    EXPECT_NE(hidden.foam, changed.foam);
    EXPECT_EQ(m_resources->Get(changed.foam), nullptr);
    EXPECT_EQ(Extract(scene).foam, hidden.foam);
}

TEST_F(WaterRenderResourceTest, HorizonUsesSharedGeometryAndExtendsThePhysicalWaterRegion)
{
    scene::Scene scene;
    const auto id = AddWater(scene);
    auto* water = scene.GetComponent<scene::WaterComponent>(id);
    auto* object = scene.GetGameObject(id);
    ASSERT_NE(water, nullptr);
    ASSERT_NE(object, nullptr);
    water->farOceanEnabled = true;
    water->lodMorphEnabled = true;
    water->farOceanCoverageScale = 1.2f;
    water->farOceanRippleFadeDistance = 6.0f;
    const auto ocean = Extract(scene);
    EXPECT_TRUE(ocean.adaptivePatchLod);
    EXPECT_TRUE(ocean.patchMorphEnabled);
    EXPECT_TRUE(ocean.patchLod.morphEnabled);
    EXPECT_TRUE(ocean.farOceanEnabled);
    EXPECT_FLOAT_EQ(ocean.farOceanCoverageScale, 1.2f);
    EXPECT_FLOAT_EQ(ocean.farOceanRippleFadeDistance, 6.0f);
    EXPECT_TRUE(ocean.patches.empty());
    EXPECT_NE(m_resources->Get(ocean.patchVertexBuffer), nullptr);
    scene::WaterSurfaceHeightSample sample;
    ASSERT_TRUE(scene::TrySampleWaterSurfaceHeightWorld(*water, object->transform,
        {water->extentX * 2.0f, -0.1f, 0.0f}, 0.0f, sample));
    EXPECT_TRUE(sample.insideSurface);
    EXPECT_TRUE(sample.underwater);
    water->farOceanEnabled = false;
    water->meshDirty = true;
    const auto restored = Extract(scene);
    EXPECT_FALSE(restored.adaptivePatchLod);
    EXPECT_FALSE(restored.farOceanEnabled);
    EXPECT_EQ(restored.patches.size(), 1u);
    EXPECT_EQ(m_resources->Get(ocean.patchVertexBuffer), nullptr);
}
TEST_F(WaterRenderResourceTest, HorizonUnderwaterCameraTracksToggleDepthAndRemoteExclusion)
{
    scene::Scene scene;
    const auto id = AddWater(scene);
    auto& water = *scene.GetComponent<scene::WaterComponent>(id);
    water.extentX = water.extentZ = 20;
    water.buoyancyDepth = 5;
    asset::MaterialAssetAuthoring material;
    material.params["opticalMode"] = {1};
    water.materialPath = SaveMaterial("HorizonOptics.mat", material);
    renderer::Camera camera;
    for (bool horizon : {false, true, false, true}) {
        water.farOceanEnabled = horizon;
        for (const auto& position : {math::Vector3{0, -2, 0}, math::Vector3{360, -2, 360},
                math::Vector3{-360, -2, -360}}) {
            camera.m_position = position;
            const auto environment = ExtractEnvironment(scene, camera);
            EXPECT_EQ(environment.cameraUnderwater, horizon || position.x == 0);
            if (horizon) {
                EXPECT_FLOAT_EQ(environment.underwater.halfExtent.x, 0);
                EXPECT_FLOAT_EQ(environment.underwater.halfExtent.y, 0);
                EXPECT_FLOAT_EQ(environment.underwater.bottomY, -5);
            }
        }
    }
    camera.m_position = {360, -6, 360};
    EXPECT_FALSE(ExtractEnvironment(scene, camera).cameraUnderwater);
    auto& box = scene.CreateGameObject("Remote Dry Room");
    box.transform.position = {360, -2, 360};
    box.AddComponent<scene::WaterExclusionComponent>().size = {4, 4, 4};
    scene::FlushWorldTransforms(scene);
    scene::PrepareWaterSceneSurfaces(scene);
    camera.m_position = {360, -2, 360};
    EXPECT_FALSE(ExtractEnvironment(scene, camera).cameraUnderwater);
    camera.m_position.x = 366;
    EXPECT_TRUE(ExtractEnvironment(scene, camera).cameraUnderwater);
}

TEST_F(WaterRenderResourceTest, HorizonPhysicsAppliesBuoyancyOutsideTheOriginalRectangle)
{
    for (bool horizon : {false, true}) {
        scene::Scene scene;
        const auto id = AddWater(scene);
        auto& water = *scene.GetComponent<scene::WaterComponent>(id);
        water.farOceanEnabled = horizon;
        water.extentX = water.extentZ = 20;
        water.buoyancyDepth = 5;
        auto& object = scene.CreateGameObject("Remote Body");
        object.transform.position = {360, -2, 360};
        auto& body = object.AddComponent<scene::RigidBodyComponent>();
        body.rigidBody = std::make_unique<physics::RigidBody>();
        body.rigidBody->SetPosition(object.transform.position);
        scene::FlushWorldTransforms(scene);
        physics::World world;
        world.SetGravity(math::Vector3::ZERO);
        scene::PhysicsSystem system;
        SystemContext context{scene, world, nullptr, nullptr, 0.01f, 0.01f, false, false};
        physics::World* previousWorld = scene::Script::GetPhysicsWorld();
        system.Update(context);
        scene::Script::SetPhysicsWorld(previousWorld);
        if (horizon) EXPECT_GT(body.rigidBody->GetVelocity().y, 0);
        else EXPECT_FLOAT_EQ(body.rigidBody->GetVelocity().y, 0);
    }
}

TEST_F(WaterRenderResourceTest, ZeroPhysicalDepthKeepsTheVisibleWaterAndDisablesOnlyTerrainInfo)
{
    scene::Scene scene;
    const auto id = AddWater(scene);
    auto* water = scene.GetComponent<scene::WaterComponent>(id);
    ASSERT_NE(water, nullptr);
    water->buoyancyDepth = 0.0f;
    const auto input = Extract(scene);
    ASSERT_EQ(input.patches.size(), 1u);
    EXPECT_NE(m_resources->Get(input.patches[0].vertexBuffer), nullptr);
    EXPECT_FLOAT_EQ(input.constants.terrainInfoParams.x, 0.0f);
}

TEST_F(WaterRenderResourceTest, HorizonSettingChangesReplaceWarmGeometryWithoutCallerDirtyFlags)
{
    scene::Scene scene;
    const auto id = AddWater(scene);
    auto* water = scene.GetComponent<scene::WaterComponent>(id);
    ASSERT_NE(water, nullptr);
    const auto finite = Extract(scene);
    ASSERT_FALSE(finite.patches.empty());
    ASSERT_FALSE(water->meshDirty);
    water->farOceanEnabled = true;
    const auto horizon = Extract(scene);
    EXPECT_TRUE(horizon.adaptivePatchLod);
    EXPECT_TRUE(horizon.farOceanEnabled);
    EXPECT_TRUE(horizon.patches.empty());
    EXPECT_NE(m_resources->Get(horizon.patchVertexBuffer), nullptr);
    water->farOceanEnabled = false;
    const auto restored = Extract(scene);
    EXPECT_FALSE(restored.adaptivePatchLod);
    EXPECT_FALSE(restored.patches.empty());
    EXPECT_EQ(m_resources->Get(horizon.patchVertexBuffer), nullptr);
}

TEST_F(WaterRenderResourceTest, WarmGeometryTracksExtentResolutionAndPatchSettings)
{
    scene::Scene scene;
    const auto id = AddWater(scene);
    auto* water = scene.GetComponent<scene::WaterComponent>(id);
    ASSERT_NE(water, nullptr);
    const auto initial = Extract(scene);
    ASSERT_EQ(initial.patches.size(), 1u);
    water->extentX = 24;
    water->resolutionX = 8;
    const auto resized = Extract(scene);
    EXPECT_FLOAT_EQ(resized.aabbMin.x, -12);
    EXPECT_FLOAT_EQ(resized.aabbMax.x, 12);
    ASSERT_EQ(resized.patches.size(), 1u);
    EXPECT_EQ(resized.patches[0].indexCount, 8u * 4u * 6u);
    EXPECT_EQ(m_resources->Get(initial.patches[0].vertexBuffer), nullptr);
    water->farOceanEnabled = true;
    const auto horizon = Extract(scene);
    water->lodPatchResolution = 8;
    const auto refined = Extract(scene);
    EXPECT_EQ(refined.patchIndexCounts[0], 8u * 8u * 6u);
    EXPECT_EQ(m_resources->Get(horizon.patchVertexBuffer), nullptr);
    water->adaptivePatchLod = true;
    water->farOceanEnabled = false;
    EXPECT_EQ(Extract(scene).patchVertexBuffer, refined.patchVertexBuffer);
}

TEST_F(WaterRenderResourceTest, TerrainRemovalAndReplacementInsideOneFrameReacquireTheCurrentSources)
{
    scene::Scene scene;
    AddWater(scene);
    const auto terrainId = scene.CreateGameObject("Ground").GetID();
    scene.GetGameObject(terrainId)->AddComponent<scene::TerrainComponent>().InitFlat(-0.1f);
    const auto first = Extract(scene, 42);
    scene.RemoveComponent<scene::TerrainComponent>(terrainId);
    const auto removed = Extract(scene, 42);
    EXPECT_NE(removed.foam, first.foam);
    EXPECT_EQ(m_resources->Get(first.foam), nullptr);
    scene.GetGameObject(terrainId)->AddComponent<scene::TerrainComponent>().InitFlat(-2.0f);
    const auto replaced = Extract(scene, 42);
    EXPECT_NE(replaced.foam, removed.foam);
    EXPECT_EQ(m_resources->Get(removed.foam), nullptr);
    EXPECT_EQ(Extract(scene, 42).foam, replaced.foam);
    ASSERT_TRUE(scene.DestroyGameObject(terrainId));
    const auto destroyed = Extract(scene, 42);
    EXPECT_NE(destroyed.foam, replaced.foam);
    EXPECT_EQ(m_resources->Get(replaced.foam), nullptr);
}
TEST_F(WaterRenderResourceTest, ExtractsExplicitWaveCountPhasesAndFiniteOpticalCoefficients)
{
    const auto materialPath = m_temp.File("WaterOptics.mat").generic_string();
    asset::MaterialAssetAuthoring material;
    ASSERT_TRUE(asset::SaveMaterialAssetToFile(materialPath, material));
    const auto handle = asset::AssetManager::Load<asset::MaterialAsset>(materialPath);
    ASSERT_TRUE(handle.IsValid());
    auto* shared = asset::AssetManager::Get<asset::MaterialAsset>(handle);
    ASSERT_NE(shared, nullptr);
    EditMaterialAuthoring(*shared, [&](auto& values) { values.params["opticalMode"] = {1.0f}; });
    EditMaterialAuthoring(*shared, [&](auto& values) {
        values.params["absorptionCoefficients"] = {
            -2.0f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()};
    });
    EditMaterialAuthoring(*shared, [&](auto& values) { values.params["scatteringCoefficients"] = {0.04f, 0.03f, 0.02f}; });
    EditMaterialAuthoring(*shared, [&](auto& values) { values.params["phaseG"] = {2.0f}; });
    scene::Scene scene;
    const auto id = AddWater(scene);
    auto* water = scene.GetComponent<scene::WaterComponent>(id);
    ASSERT_NE(water, nullptr);
    water->materialPath = materialPath;
    water->enableGerstnerWaves = true;
    water->waveCount = 5;
    water->waveGrouping = water->waveSpread = 0.0f;
    for (size_t i = 0; i < water->waveCount; ++i)
        water->waves[i].phaseOffsetRadians = static_cast<float>(i) + 0.25f;
    const auto input = Extract(scene);
    EXPECT_FLOAT_EQ(input.constants.waveControls.x, 5.0f);
    EXPECT_FLOAT_EQ(input.constants.waveControls.w, 1.0f);
    EXPECT_FLOAT_EQ(input.constants.wavePhasePacked[0].w, 3.25f);
    EXPECT_FLOAT_EQ(input.constants.wavePhasePacked[1].x, 4.25f);
    EXPECT_FLOAT_EQ(input.constants.absorptionCoefficients.x, 0.0f);
    EXPECT_FLOAT_EQ(input.constants.absorptionCoefficients.y, 0.0f);
    EXPECT_FLOAT_EQ(input.constants.absorptionCoefficients.z, 1000.0f);
    EXPECT_FLOAT_EQ(input.constants.scatteringCoefficients.x, 0.04f);
    EXPECT_FLOAT_EQ(input.constants.opticalParams.x, 1.0f);
    EXPECT_FLOAT_EQ(input.constants.opticalParams.y, 0.95f);
    water->waveCount = 0;
    EditMaterialAuthoring(*shared, [&](auto& values) { values.params["phaseG"] = {std::numeric_limits<float>::quiet_NaN()}; });
    const auto calm = Extract(scene);
    EXPECT_FLOAT_EQ(calm.constants.waveControls.x, 0.0f);
    EXPECT_FLOAT_EQ(calm.constants.waveControls.w, 1.0f);
    EXPECT_FLOAT_EQ(calm.constants.opticalParams.y, 0.0f);
}

TEST_F(WaterRenderResourceTest, ExtractsAuthoredOpticsExclusionsAndSedimentIntoTheSharedMedium)
{
    asset::MaterialAssetAuthoring material;
    material.params["opticalMode"] = {1.0f};
    material.params["absorptionCoefficients"] = {0.28f, 0.11f, 0.055f};
    material.params["scatteringCoefficients"] = {0.02f, 0.06f, 0.095f};
    material.params["phaseG"] = {0.45f};
    material.params["underwaterLightStrength"] = {2.0f};
    material.params["underwaterMaxDistance"] = {65.0f};
    material.params["underwaterLightSteps"] = {32.0f};
    const std::string materialPath = SaveMaterial("WaterLabOptics.mat", material);
    ASSERT_FALSE(materialPath.empty());

    scene::Scene scene;
    const auto waterID = AddWater(scene);
    auto* waterObject = scene.GetGameObject(waterID);
    auto* water = scene.GetComponent<scene::WaterComponent>(waterID);
    ASSERT_NE(waterObject, nullptr);
    ASSERT_NE(water, nullptr);
    waterObject->transform.position = {3.0f, 2.0f, -4.0f};
    waterObject->transform.scale = {2.0f, 1.0f, -0.5f};
    water->extentX = 40.0f;
    water->extentZ = 30.0f;
    water->buoyancyDepth = 24.0f;
    water->materialPath = materialPath;
    water->interactionSettings.sedimentScattering = {0.31f, 0.19f, 0.08f};

    auto& exclusionObject = scene.CreateGameObject("Dry room");
    exclusionObject.transform.position = {30.0f, 1.0f, -4.0f};
    exclusionObject.transform.scale = {2.0f, 3.0f, 4.0f};
    auto& exclusion = exclusionObject.AddComponent<scene::WaterExclusionComponent>();
    exclusion.size = {4.0f, 6.0f, 8.0f};
    exclusion.excludeAll = true;

    scene::FlushWorldTransforms(scene);
    scene::PrepareWaterSceneSurfaces(scene);
    const uint64_t frameCountBefore = Time::frameCount;
    scene::WaterInteractionImpulse sediment;
    sediment.kind = scene::WaterInteractionKind::SEDIMENT;
    sediment.worldPosition = {0.0f, -3.0f, 0.0f};
    sediment.worldVelocity = {0.4f, 0.0f, 0.0f};
    sediment.radius = 0.75f;
    sediment.strength = 0.8f;
    sediment.lifetime = 6.0f;
    ASSERT_TRUE(scene::QueueWaterInteraction(scene, waterID, sediment));
    physics::World world;
    scene::WaterInteractionSystem interactionSystem;
    ++Time::frameCount;
    SystemContext interactionContext{scene, world, nullptr, nullptr, 0.1f, 0.1f, true, true};
    interactionSystem.Update(interactionContext);
    water = scene.GetComponent<scene::WaterComponent>(waterID);
    ASSERT_NE(water, nullptr);
    const auto interactionFrame = scene::WaterInteractionSnapshot(*water);
    ASSERT_NE(interactionFrame, nullptr);
    ASSERT_EQ(interactionFrame->plumes.size(), 1u);

    const auto input = Extract(scene);
    const auto& medium = input.medium;
    EXPECT_TRUE(medium.opticalEnabled);
    EXPECT_TRUE(medium.analyticRectangle);
    EXPECT_EQ(medium.stableOwnerKey,
        (static_cast<uint64_t>(waterID.generation) << 32u) | waterID.index);
    EXPECT_NEAR(medium.absorptionPhaseG.x, 0.28f, 1.0e-6f);
    EXPECT_NEAR(medium.absorptionPhaseG.y, 0.11f, 1.0e-6f);
    EXPECT_NEAR(medium.absorptionPhaseG.z, 0.055f, 1.0e-6f);
    EXPECT_NEAR(medium.absorptionPhaseG.w, 0.45f, 1.0e-6f);
    EXPECT_NEAR(medium.scatteringLight.x, 0.02f, 1.0e-6f);
    EXPECT_NEAR(medium.scatteringLight.y, 0.06f, 1.0e-6f);
    EXPECT_NEAR(medium.scatteringLight.z, 0.095f, 1.0e-6f);
    EXPECT_NEAR(medium.scatteringLight.w, 2.0f, 1.0e-6f);
    EXPECT_NEAR(medium.surfaceY, 2.0f, 1.0e-6f);
    EXPECT_NEAR(medium.depthLimit, 24.0f, 1.0e-6f);
    EXPECT_NEAR(medium.bottomY, -22.0f, 1.0e-6f);
    EXPECT_NEAR(medium.maxDistance, 65.0f, 1.0e-6f);
    EXPECT_EQ(medium.lightSteps, 32);
    EXPECT_NEAR(medium.localHalfExtent.x, 20.0f, 1.0e-6f);
    EXPECT_NEAR(medium.localHalfExtent.y, 15.0f, 1.0e-6f);
    EXPECT_NEAR(medium.worldToLocalRows[0].x, 0.5f, 1.0e-6f);
    EXPECT_NEAR(medium.worldToLocalRows[0].w, -1.5f, 1.0e-6f);
    EXPECT_NEAR(medium.worldToLocalRows[1].y, 1.0f, 1.0e-6f);
    EXPECT_NEAR(medium.worldToLocalRows[1].w, -2.0f, 1.0e-6f);
    EXPECT_NEAR(medium.worldToLocalRows[2].z, -2.0f, 1.0e-6f);
    EXPECT_NEAR(medium.worldToLocalRows[2].w, -8.0f, 1.0e-6f);
    EXPECT_LE(medium.worldBoundsXZ.x, -37.0f);
    EXPECT_GE(medium.worldBoundsXZ.z, 43.0f);
    EXPECT_LE(medium.worldBoundsXZ.y, -11.5f);
    EXPECT_GE(medium.worldBoundsXZ.w, 3.5f);

    ASSERT_EQ(medium.exclusions.size(), 1u);
    EXPECT_NEAR(medium.exclusions[0].halfExtent.x, 2.0f, 1.0e-6f);
    EXPECT_NEAR(medium.exclusions[0].halfExtent.y, 3.0f, 1.0e-6f);
    EXPECT_NEAR(medium.exclusions[0].halfExtent.z, 4.0f, 1.0e-6f);
    EXPECT_NEAR(medium.exclusions[0].worldToLocalRows[0].x, 0.5f, 1.0e-6f);
    EXPECT_NEAR(medium.exclusions[0].worldToLocalRows[0].w, -15.0f, 1.0e-6f);
    EXPECT_NEAR(medium.exclusions[0].worldToLocalRows[1].y, 1.0f / 3.0f, 1.0e-6f);
    EXPECT_NEAR(medium.exclusions[0].worldToLocalRows[1].w, -1.0f / 3.0f, 1.0e-6f);
    EXPECT_NEAR(medium.exclusions[0].worldToLocalRows[2].z, 0.25f, 1.0e-6f);
    EXPECT_NEAR(medium.exclusions[0].worldToLocalRows[2].w, 1.0f, 1.0e-6f);

    ASSERT_EQ(medium.plumes.size(), 1u);
    EXPECT_NEAR(medium.plumes[0].worldPositionRadius.x, 0.0f, 1.0e-6f);
    EXPECT_NEAR(medium.plumes[0].worldPositionRadius.y, -3.0f, 1.0e-6f);
    EXPECT_NEAR(medium.plumes[0].worldPositionRadius.z, 0.0f, 1.0e-6f);
    EXPECT_NEAR(medium.plumes[0].worldPositionRadius.w, 0.75f, 1.0e-6f);
    EXPECT_NEAR(medium.plumes[0].scatteringDensity.x, 0.31f, 1.0e-6f);
    EXPECT_NEAR(medium.plumes[0].scatteringDensity.y, 0.19f, 1.0e-6f);
    EXPECT_NEAR(medium.plumes[0].scatteringDensity.z, 0.08f, 1.0e-6f);
    EXPECT_NEAR(medium.plumes[0].scatteringDensity.w, 0.8f, 1.0e-6f);
    EXPECT_EQ(Time::frameCount, frameCountBefore + 1);
}

TEST_F(WaterRenderResourceTest, SplineRiverUploadsTheSharedSnapshotAndInvalidatesOnAuthoringChanges)
{
    scene::Scene scene;
    auto& object = scene.CreateGameObject("Spline River");
    auto& water = object.AddComponent<scene::WaterComponent>();
    water.enableGerstnerWaves = false;
    water.surfaceShape = scene::WaterSurfaceShape::SPLINE_RIVER;
    water.splineSamplesPerSegment = 2;
    water.buoyancyDepth = 3.0f;
    water.splinePoints = {
        {1, {0.0f, 0.0f, 0.0f}, 2.0f, 1.0f, 1.25f},
        {2, {0.0f, 0.0f, 4.0f}, 4.0f, 8.0f, -2.0f},
        {3, {0.0f, 0.0f, 8.0f}, 2.0f, 5.0f, 0.5f},
    };
    water.cameraFocusedGrid = true;
    water.adaptivePatchLod = true;
    water.lodMorphEnabled = true;
    water.farOceanEnabled = true;

    const auto input = Extract(scene);
    ASSERT_NE(water.splineSnapshot, nullptr);
    const auto snapshot = water.splineSnapshot;
    ASSERT_EQ(input.patches.size(), 1u);
    EXPECT_FALSE(input.adaptivePatchLod);
    EXPECT_FALSE(input.patchMorphEnabled);
    EXPECT_FALSE(input.farOceanEnabled);
    EXPECT_FLOAT_EQ(input.constants.gridParams.x, 0.0f);
    EXPECT_FALSE(input.patchVertexBuffer.IsValid());
    EXPECT_FALSE(input.patchLod.morphEnabled);
    EXPECT_FLOAT_EQ(input.constants.timeParams.x, snapshot->maximumWorldEdgeLength);
    EXPECT_FLOAT_EQ(input.constants.timeParams.y, snapshot->maximumWorldEdgeLength);
    EXPECT_FLOAT_EQ(input.constants.farOceanParams.x, 0.0f);

    const auto& patch = input.patches.front();
    auto* vertexBuffer = m_resources->Get(patch.vertexBuffer);
    auto* indexBuffer = m_resources->Get(patch.indexBuffer);
    ASSERT_NE(vertexBuffer, nullptr);
    ASSERT_NE(indexBuffer, nullptr);
    EXPECT_EQ(vertexBuffer->GetStride(), sizeof(renderer::WaterVertex));
    ASSERT_EQ(vertexBuffer->GetSize(), snapshot->vertices.size() * sizeof(renderer::WaterVertex));
    ASSERT_EQ(patch.indexCount, snapshot->triangles.size() * 3u);
    ASSERT_EQ(indexBuffer->GetSize(), static_cast<size_t>(patch.indexCount) * sizeof(uint32_t));

    std::vector<renderer::WaterVertex> uploadedVertices(snapshot->vertices.size());
    ASSERT_TRUE(vertexBuffer->CopyData(0, vertexBuffer->GetSize(), uploadedVertices.data()));
    const auto extent = scene::WaterSurfaceLocalExtent(water);
    for (size_t i = 0; i < uploadedVertices.size(); ++i) {
        const auto& uploaded = uploadedVertices[i];
        const auto& source = snapshot->vertices[i];
        EXPECT_FLOAT_EQ(uploaded.position.x, source.localPosition.x);
        EXPECT_FLOAT_EQ(uploaded.position.y, source.localPosition.y);
        EXPECT_FLOAT_EQ(uploaded.position.z, source.localPosition.z);
        EXPECT_FLOAT_EQ(uploaded.uv.x, source.localPosition.x / extent.x + 0.5f);
        EXPECT_FLOAT_EQ(uploaded.uv.y, source.localPosition.z / extent.y + 0.5f);
        EXPECT_FLOAT_EQ(uploaded.splineData.x, (std::min)(source.depth, water.buoyancyDepth));
        EXPECT_FLOAT_EQ(uploaded.splineData.y, source.worldVelocity.x);
        EXPECT_FLOAT_EQ(uploaded.splineData.z, source.worldVelocity.z);
        EXPECT_FLOAT_EQ(uploaded.splineData.w, 1.0f);
    }
    std::vector<uint32_t> uploadedIndices(patch.indexCount);
    ASSERT_TRUE(indexBuffer->CopyData(0, indexBuffer->GetSize(), uploadedIndices.data()));
    for (size_t triangle = 0; triangle < snapshot->triangles.size(); ++triangle)
        for (size_t corner = 0; corner < 3; ++corner)
            EXPECT_EQ(uploadedIndices[triangle * 3u + corner], snapshot->triangles[triangle].indices[corner]);

    const auto oldVertexBuffer = patch.vertexBuffer;
    const auto oldSignature = snapshot->signature;
    water.splinePoints[1].flowSpeed = 4.0f;
    const auto changed = Extract(scene);
    ASSERT_NE(water.splineSnapshot, nullptr);
    EXPECT_NE(water.splineSnapshot->signature, oldSignature);
    ASSERT_EQ(changed.patches.size(), 1u);
    EXPECT_NE(changed.patches[0].vertexBuffer, oldVertexBuffer);
    EXPECT_EQ(m_resources->Get(oldVertexBuffer), nullptr);

    const auto authoredVertexBuffer = changed.patches[0].vertexBuffer;
    const auto authoredSignature = water.splineSnapshot->signature;
    object.transform.worldPosition = {3.0f, 1.0f, -2.0f};
    const auto moved = Extract(scene);
    ASSERT_NE(water.splineSnapshot, nullptr);
    EXPECT_NE(water.splineSnapshot->signature, authoredSignature);
    ASSERT_EQ(moved.patches.size(), 1u);
    EXPECT_NE(moved.patches[0].vertexBuffer, authoredVertexBuffer);
    EXPECT_EQ(m_resources->Get(authoredVertexBuffer), nullptr);
}

TEST_F(WaterRenderResourceTest, WaterZoneRendersOneSharedUnionAndReleasesSuppressedResources)
{
    scene::Scene scene;
    auto& ownerObject = scene.CreateGameObject("Zone Owner");
    ownerObject.layer = 3;
    auto& ownerWater = ownerObject.AddComponent<scene::WaterComponent>();
    ownerWater.extentX = ownerWater.extentZ = 8.0f;
    ownerWater.enableGerstnerWaves = false;
    ownerWater.resolutionX = ownerWater.resolutionZ = 4;
    ownerWater.chunkCount = 1;

    auto& memberObject = scene.CreateGameObject("River Member");
    memberObject.layer = 4;
    auto& memberWater = memberObject.AddComponent<scene::WaterComponent>();
    memberWater.enableGerstnerWaves = false;
    memberWater.surfaceShape = scene::WaterSurfaceShape::SPLINE_RIVER;
    memberWater.splineSamplesPerSegment = 2;
    memberWater.buoyancyDepth = 4.0f;
    memberWater.splinePoints = {
        {1, {-8.0f, 0.0f, 0.0f}, 4.0f, 2.0f, 1.0f},
        {2, {-4.0f, 0.0f, 0.0f}, 4.0f, 3.0f, 2.0f},
        {3, { 0.0f, 0.0f, 0.0f}, 4.0f, 4.0f, 3.0f},
    };

    const auto extractAll = [&](bool selectMember = false) {
        renderer::Camera camera;
        renderer::RenderSettings settings;
        if (selectMember)
            settings.selectedObjects.push_back({memberObject.GetID().index, memberObject.GetID().generation});
        renderer::RenderPassHandles handles;
        scene::RenderPassContext context(scene, m_renderer, *m_resources,
            camera, settings, {}, UINT32_MAX, handles);
        renderer::RenderScene output;
        scene::ExtractRenderWater(context, output);
        return std::move(output.water);
    };

    const auto standalone = extractAll();
    ASSERT_EQ(standalone.size(), 2u);
    const auto standaloneMember = std::find_if(standalone.begin(), standalone.end(),
        [](const renderer::RenderWaterInput& input) { return input.layer == 4u; });
    ASSERT_NE(standaloneMember, standalone.end());
    ASSERT_EQ(standaloneMember->patches.size(), 1u);
    const auto suppressedMemberBuffer = standaloneMember->patches.front().vertexBuffer;
    ASSERT_NE(m_resources->Get(suppressedMemberBuffer), nullptr);

    auto& zone = ownerObject.AddComponent<scene::WaterZoneComponent>();
    zone.members.push_back({scene::EntityRef{memberObject.GetID()}, 0});
    const auto grouped = extractAll();
    ASSERT_EQ(grouped.size(), 1u);
    ASSERT_NE(zone.snapshot, nullptr) << zone.diagnostic;
    ASSERT_NE(ownerWater.zoneSnapshot, nullptr);
    ASSERT_TRUE(memberWater.zoneOwner.IsValid());
    EXPECT_EQ(ownerWater.surfaceShape, scene::WaterSurfaceShape::RECTANGLE);
    EXPECT_EQ(memberWater.surfaceShape, scene::WaterSurfaceShape::SPLINE_RIVER);
    EXPECT_EQ(m_resources->Get(suppressedMemberBuffer), nullptr);
    const auto selectedMember = extractAll(true);
    ASSERT_EQ(selectedMember.size(), 1u);
    EXPECT_TRUE(selectedMember.front().selected);

    const auto& geometry = ownerWater.zoneSnapshot->geometry;
    ASSERT_EQ(grouped.front().patches.size(), 1u);
    const auto& patch = grouped.front().patches.front();
    auto* vertexBuffer = m_resources->Get(patch.vertexBuffer);
    ASSERT_NE(vertexBuffer, nullptr);
    ASSERT_EQ(vertexBuffer->GetSize(), geometry.vertices.size() * sizeof(renderer::WaterVertex));
    std::vector<renderer::WaterVertex> uploaded(geometry.vertices.size());
    ASSERT_TRUE(vertexBuffer->CopyData(0, vertexBuffer->GetSize(), uploaded.data()));
    const auto extent = scene::WaterSurfaceLocalExtent(ownerWater);
    for (size_t i = 0; i < uploaded.size(); ++i) {
        EXPECT_FLOAT_EQ(uploaded[i].position.x, geometry.vertices[i].localPosition.x);
        EXPECT_FLOAT_EQ(uploaded[i].position.y, geometry.vertices[i].localPosition.y);
        EXPECT_FLOAT_EQ(uploaded[i].position.z, geometry.vertices[i].localPosition.z);
        EXPECT_FLOAT_EQ(uploaded[i].uv.x, geometry.vertices[i].localPosition.x / extent.x + 0.5f);
        EXPECT_FLOAT_EQ(uploaded[i].uv.y, geometry.vertices[i].localPosition.z / extent.y + 0.5f);
        EXPECT_FLOAT_EQ(uploaded[i].splineData.y, geometry.vertices[i].worldVelocity.x);
        EXPECT_FLOAT_EQ(uploaded[i].splineData.z, geometry.vertices[i].worldVelocity.z);
    }

    const auto sharedBuffer = patch.vertexBuffer;
    const auto sharedSignature = geometry.signature;
    memberWater.splinePoints[1].localPosition.x += 0.5f;
    const auto changed = extractAll();
    ASSERT_EQ(changed.size(), 1u);
    ASSERT_NE(ownerWater.zoneSnapshot, nullptr);
    EXPECT_NE(ownerWater.zoneSnapshot->geometry.signature, sharedSignature);
    EXPECT_NE(changed.front().patches.front().vertexBuffer, sharedBuffer);
    EXPECT_EQ(m_resources->Get(sharedBuffer), nullptr);

    zone.members.clear();
    const auto restored = extractAll();
    ASSERT_EQ(restored.size(), 2u);
    EXPECT_FALSE(memberWater.zoneOwner.IsValid());
    EXPECT_FALSE(ownerWater.zoneSnapshot);
    const auto restoredMember = std::find_if(restored.begin(), restored.end(),
        [](const renderer::RenderWaterInput& input) { return input.layer == 4u; });
    ASSERT_NE(restoredMember, restored.end());
    ASSERT_EQ(restoredMember->patches.size(), 1u);
    EXPECT_TRUE(restoredMember->patches.front().vertexBuffer.IsValid());
    EXPECT_NE(restoredMember->patches.front().vertexBuffer, suppressedMemberBuffer);
}

TEST_F(WaterRenderResourceTest, PhysicsCreatesOneZoneVolumeAndRestoresMemberVolumeWhenDisabled)
{
    scene::Scene scene;
    auto& ownerObject = scene.CreateGameObject("Zone Owner");
    auto& ownerWater = ownerObject.AddComponent<scene::WaterComponent>();
    ownerWater.enableGerstnerWaves = false;
    ownerWater.extentX = ownerWater.extentZ = 8.0f;

    auto& memberObject = scene.CreateGameObject("Zone Member");
    memberObject.transform.position.x = 6.0f;
    auto& memberWater = memberObject.AddComponent<scene::WaterComponent>();
    memberWater.enableGerstnerWaves = false;
    memberWater.extentX = memberWater.extentZ = 6.0f;

    auto& zone = ownerObject.AddComponent<scene::WaterZoneComponent>();
    zone.members.push_back({scene::EntityRef{memberObject.GetID()}, 0});
    scene::FlushWorldTransforms(scene);

    physics::World world;
    scene::PhysicsSystem physicsSystem;
    SystemContext context{scene, world, nullptr, nullptr, 0.01f, 0.01f, false, false};
    physics::World* previousPhysicsWorld = scene::Script::GetPhysicsWorld();
    physicsSystem.Update(context);

    EXPECT_TRUE(ownerWater.zoneSnapshot);
    EXPECT_TRUE(ownerWater.volumeHandle.IsValid());
    EXPECT_TRUE(memberWater.zoneOwner.IsValid());
    EXPECT_FALSE(memberWater.volumeHandle.IsValid());

    zone.enabled = false;
    physicsSystem.Update(context);
    EXPECT_FALSE(memberWater.zoneOwner.IsValid());
    EXPECT_TRUE(ownerWater.volumeHandle.IsValid());
    EXPECT_TRUE(memberWater.volumeHandle.IsValid());
    scene::Script::SetPhysicsWorld(previousPhysicsWorld);
}

TEST_F(WaterRenderResourceTest, ScriptWaterMemberKeepsLocalHeightAndRippleOnTheSharedOwner)
{
    scene::Scene scene;
    auto& ownerObject = scene.CreateGameObject("Zone Owner");
    ownerObject.transform.position.y = 10.0f;
    auto& ownerWater = ownerObject.AddComponent<scene::WaterComponent>();
    ownerWater.extentX = ownerWater.extentZ = 8.0f;
    ownerWater.enableGerstnerWaves = false;

    auto& memberObject = scene.CreateGameObject("Offset Member");
    memberObject.transform.position = {4.5f, 15.0f, 0.0f};
    memberObject.transform.rotation = math::Quaternion::FromAxisAngle(
        math::Vector3::UP, math::ToRad(90.0f));
    memberObject.transform.scale = {2.0f, 1.0f, 0.5f};
    auto& memberWater = memberObject.AddComponent<scene::WaterComponent>();
    memberWater.enableGerstnerWaves = false;
    memberWater.extentX = memberWater.extentZ = 4.0f;

    auto& zone = ownerObject.AddComponent<scene::WaterZoneComponent>();
    zone.members.push_back({scene::EntityRef{memberObject.GetID()}, 0});
    scene::FlushWorldTransforms(scene);

    scene::Script caller;
    caller.SetContext(&scene, &memberObject);
    const auto relativeSurfaceHeight = [&](const scene::Transform& coordinateTransform,
                                           float localX, float localZ) {
        const math::Vector3 world = coordinateTransform.worldPosition
            + coordinateTransform.worldRotation * math::Vector3{
                localX * coordinateTransform.worldScale.x, 0.0f,
                localZ * coordinateTransform.worldScale.z };
        scene::WaterSurfaceHeightSample sample;
        EXPECT_TRUE(scene::TrySampleWaterSurfaceHeightWorld(ownerWater, ownerObject.transform,
            {world.x, ownerObject.transform.worldPosition.y, world.z}, 0.0f, sample));
        return sample.position.y - memberObject.transform.worldPosition.y;
    };
    const float actualMemberHeight = caller.water.GetSurfaceHeightLocal(0.5f, 0.25f, 0.0f);
    const float expectedMemberHeight = relativeSurfaceHeight(memberObject.transform, 0.5f, 0.25f);
    const float ownerLocalHeight = relativeSurfaceHeight(ownerObject.transform, 0.5f, 0.25f);
    EXPECT_GT(std::abs(expectedMemberHeight - ownerLocalHeight), 0.1f);
    EXPECT_NEAR(actualMemberHeight, expectedMemberHeight, testkit::kTolerance);

    caller.water.AddRipple({4.625f, 15.0f, 1.0f}, 0.5f);
    ASSERT_EQ(ownerWater.ripples.size(), 1u);
    EXPECT_TRUE(memberWater.ripples.empty());
    caller.water.Splash({4.625f, 15.0f, 1.0f}, 0.5f);
    EXPECT_EQ(ownerWater.ripples.size(), 2u);
    EXPECT_TRUE(memberWater.ripples.empty());

    ownerObject.transform.position.y = 12.0f;
    memberObject.transform.position.y = 17.0f;
    scene::FlushWorldTransforms(scene);
    const float movedActual = caller.water.GetSurfaceHeightLocal(0.5f, 0.25f, 0.0f);
    const float movedExpected = relativeSurfaceHeight(memberObject.transform, 0.5f, 0.25f);
    EXPECT_NEAR(movedActual, movedExpected, testkit::kTolerance);
}

TEST_F(WaterRenderResourceTest, SplineUnderwaterEffectsPrepareTheSharedShapeBeforeGeometryExtraction)
{
    scene::Scene scene;
    const auto id = AddWater(scene);
    auto* water = scene.GetComponent<scene::WaterComponent>(id);
    ASSERT_NE(water, nullptr);
    water->surfaceShape = scene::WaterSurfaceShape::SPLINE_RIVER;
    water->splinePoints = {{1, {-10.0f, 0.0f, 0.0f}, 6.0f, 2.0f, 1.0f},
                          {2, { 10.0f, 0.0f, 0.0f}, 6.0f, 2.0f, 1.0f}};
    renderer::Camera camera;
    camera.m_position = {0.0f, -1.0f, 0.0f};
    renderer::RenderSettings settings;
    renderer::RenderPassHandles handles;
    scene::RenderPassContext context(scene, m_renderer, *m_resources, camera, settings, {}, UINT32_MAX, handles);
    const auto inside = scene::ExtractRenderEnvironment(context);
    EXPECT_TRUE(inside.underwater.enabled);
    EXPECT_NEAR(inside.underwater.depth, 1.0f, testkit::kTolerance);
    EXPECT_NE(water->splineSnapshot, nullptr);
    camera.m_position.y = -3.0f;
    EXPECT_FALSE(scene::ExtractRenderEnvironment(context).underwater.enabled);
    camera.m_position = {0.0f, -1.0f, 5.0f};
    EXPECT_FALSE(scene::ExtractRenderEnvironment(context).underwater.enabled);
}

TEST_F(WaterRenderResourceTest, UnderwaterOpticsUseResolvedMaterialAndYawLocalRectangleBounds)
{
    scene::Scene scene;
    const auto id = AddWater(scene);
    auto* water = scene.GetComponent<scene::WaterComponent>(id);
    auto* object = scene.GetGameObject(id);
    ASSERT_NE(water, nullptr);
    ASSERT_NE(object, nullptr);
    water->enableGerstnerWaves = false;
    water->extentX = 8.0f;
    water->extentZ = 4.0f;
    water->buoyancyDepth = 5.0f;
    object->transform.position = {10.0f, 3.0f, 20.0f};
    object->transform.rotation = math::Quaternion::FromAxisAngle(math::Vector3::UP, math::PI * 0.5f);
    object->transform.scale = {2.0f, 1.0f, 0.5f};
    scene::FlushWorldTransforms(scene);

    asset::MaterialAssetAuthoring material;
    material.params["opticalMode"] = {1.0f};
    material.params["absorptionCoefficients"] = {0.15f, 0.25f, 0.35f};
    material.params["scatteringCoefficients"] = {0.02f, 0.04f, 0.06f};
    material.params["phaseG"] = {0.65f};
    material.params["underwaterLightStrength"] = {3.0f};
    material.params["underwaterMaxDistance"] = {42.0f};
    material.params["underwaterLightSteps"] = {20.0f};
    material.params["underwaterTransitionDepth"] = {0.5f};
    material.params["underwaterDistortion"] = {0.004f};
    material.params["enableCaustics"] = {1.0f};
    material.params["causticsIntensity"] = {0.7f};
    water->materialPath = SaveMaterial("underwater-optics.mat", material);
    ASSERT_FALSE(water->materialPath.empty());

    renderer::Camera camera;
    camera.m_position = {10.0f, 2.0f, 20.0f};
    const auto environment = ExtractEnvironment(scene, camera);
    ASSERT_TRUE(environment.underwater.enabled);
    EXPECT_TRUE(environment.underwater.opticalEnabled);
    EXPECT_NEAR(environment.underwater.depth, 1.0f, testkit::kTolerance);
    EXPECT_NEAR(environment.underwater.strength, 1.0f, testkit::kTolerance);
    EXPECT_NEAR(environment.underwater.absorption.x, 0.15f, testkit::kTolerance);
    EXPECT_NEAR(environment.underwater.absorption.y, 0.25f, testkit::kTolerance);
    EXPECT_NEAR(environment.underwater.scattering.z, 0.06f, testkit::kTolerance);
    EXPECT_NEAR(environment.underwater.phaseG, 0.65f, testkit::kTolerance);
    EXPECT_NEAR(environment.underwater.surfaceY, 3.0f, testkit::kTolerance);
    EXPECT_NEAR(environment.underwater.bottomY, -2.0f, testkit::kTolerance);
    EXPECT_NEAR(environment.underwater.maxDistance, 42.0f, testkit::kTolerance);
    EXPECT_NEAR(environment.underwater.lightStrength, 3.0f, testkit::kTolerance);
    EXPECT_EQ(environment.underwater.lightSteps, 20);
    EXPECT_NEAR(environment.underwater.distortion, 0.004f, testkit::kTolerance);
    EXPECT_NEAR(environment.underwater.center.x, 10.0f, testkit::kTolerance);
    EXPECT_NEAR(environment.underwater.center.y, 20.0f, testkit::kTolerance);
    EXPECT_NEAR(environment.underwater.halfExtent.x, 8.0f, testkit::kTolerance);
    EXPECT_NEAR(environment.underwater.halfExtent.y, 1.0f, testkit::kTolerance);
    EXPECT_NEAR(environment.underwater.cosYaw, 0.0f, testkit::kTolerance);
    EXPECT_NEAR(environment.underwater.sinYaw, 1.0f, testkit::kTolerance);

    ASSERT_TRUE(environment.caustics.enabled);
    EXPECT_TRUE(environment.caustics.opticalEnabled);
    EXPECT_NEAR(environment.caustics.absorption.z, 0.35f, testkit::kTolerance);
    EXPECT_NEAR(environment.caustics.scattering.y, 0.04f, testkit::kTolerance);
    EXPECT_NEAR(environment.caustics.phaseG, 0.65f, testkit::kTolerance);
    EXPECT_NEAR(environment.caustics.halfExtentX, 8.0f, testkit::kTolerance);
    EXPECT_NEAR(environment.caustics.halfExtentZ, 1.0f, testkit::kTolerance);
    EXPECT_NEAR(environment.caustics.cosYaw, 0.0f, testkit::kTolerance);
    EXPECT_NEAR(environment.caustics.sinYaw, 1.0f, testkit::kTolerance);
}

TEST_F(WaterRenderResourceTest, LegacyUnderwaterModeAndCameraBoundaryStayCompatible)
{
    scene::Scene scene;
    const auto id = AddWater(scene);
    auto* water = scene.GetComponent<scene::WaterComponent>(id);
    ASSERT_NE(water, nullptr);
    water->enableGerstnerWaves = false;
    water->buoyancyDepth = 5.0f;

    asset::MaterialAssetAuthoring material;
    material.params["opticalMode"] = {0.0f};
    material.params["deepDepth"] = {5.0f};
    material.params["shallowColor"] = {0.2f, 0.6f, 0.7f};
    material.params["deepColor"] = {0.0f, 0.1f, 0.3f};
    water->materialPath = SaveMaterial("underwater-legacy.mat", material);
    ASSERT_FALSE(water->materialPath.empty());

    renderer::Camera camera;
    camera.m_position = {0.0f, -1.0f, 0.0f};
    const auto inside = ExtractEnvironment(scene, camera);
    ASSERT_TRUE(inside.underwater.enabled);
    EXPECT_FALSE(inside.underwater.opticalEnabled);
    EXPECT_NEAR(inside.underwater.strength, 0.5f, testkit::kTolerance);
    EXPECT_NEAR(inside.underwater.fogDensity, 0.102f, testkit::kTolerance);

    camera.m_position.y = 0.0f;
    EXPECT_FALSE(ExtractEnvironment(scene, camera).underwater.enabled);
    camera.m_position.y = -5.01f;
    EXPECT_FALSE(ExtractEnvironment(scene, camera).underwater.enabled);
    water->enabled = false;
    camera.m_position.y = -1.0f;
    EXPECT_FALSE(ExtractEnvironment(scene, camera).underwater.enabled);
}

TEST_F(WaterRenderResourceTest, UnderwaterOpticsSanitizeNonFiniteMaterialValuesAndUseSplineSlab)
{
    scene::Scene scene;
    const auto id = AddWater(scene);
    auto* water = scene.GetComponent<scene::WaterComponent>(id);
    ASSERT_NE(water, nullptr);
    water->enableGerstnerWaves = false;
    water->surfaceShape = scene::WaterSurfaceShape::SPLINE_RIVER;
    water->splinePoints = {{1, {-10.0f, 0.0f, 0.0f}, 6.0f, 2.0f, 0.0f},
                           {2, { 10.0f, 0.0f, 0.0f}, 6.0f, 2.0f, 0.0f}};

    asset::MaterialAssetAuthoring material;
    material.params["opticalMode"] = {1.0f};
    water->materialPath = SaveMaterial("underwater-nonfinite.mat", material);
    ASSERT_FALSE(water->materialPath.empty());
    const auto handle = asset::AssetManager::Load<asset::MaterialAsset>(water->materialPath);
    auto* loaded = asset::AssetManager::Get<asset::MaterialAsset>(handle);
    ASSERT_NE(loaded, nullptr);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float infinity = std::numeric_limits<float>::infinity();
    EditMaterialAuthoring(*loaded, [&](auto& values) { values.params["absorptionCoefficients"] = {nan, infinity, -1.0f}; });
    EditMaterialAuthoring(*loaded, [&](auto& values) { values.params["scatteringCoefficients"] = {infinity, nan, -2.0f}; });
    EditMaterialAuthoring(*loaded, [&](auto& values) { values.params["phaseG"] = {infinity}; });
    EditMaterialAuthoring(*loaded, [&](auto& values) { values.params["underwaterLightStrength"] = {nan}; });
    EditMaterialAuthoring(*loaded, [&](auto& values) { values.params["underwaterMaxDistance"] = {infinity}; });
    EditMaterialAuthoring(*loaded, [&](auto& values) { values.params["underwaterLightSteps"] = {nan}; });
    EditMaterialAuthoring(*loaded, [&](auto& values) { values.params["underwaterTransitionDepth"] = {nan}; });
    EditMaterialAuthoring(*loaded, [&](auto& values) { values.params["underwaterDistortion"] = {infinity}; });

    renderer::Camera camera;
    camera.m_position = {0.0f, -0.05f, 0.0f};
    const auto environment = ExtractEnvironment(scene, camera);
    ASSERT_TRUE(environment.underwater.enabled);
    EXPECT_TRUE(environment.underwater.opticalEnabled);
    EXPECT_NEAR(environment.underwater.absorption.x, 0.0f, testkit::kTolerance);
    EXPECT_NEAR(environment.underwater.absorption.y, 1000.0f, testkit::kTolerance);
    EXPECT_NEAR(environment.underwater.absorption.z, 0.0f, testkit::kTolerance);
    EXPECT_NEAR(environment.underwater.scattering.x, 1000.0f, testkit::kTolerance);
    EXPECT_NEAR(environment.underwater.scattering.y, 0.0f, testkit::kTolerance);
    EXPECT_NEAR(environment.underwater.phaseG, 0.0f, testkit::kTolerance);
    EXPECT_NEAR(environment.underwater.strength, 0.259259f, 0.001f);
    EXPECT_NEAR(environment.underwater.lightStrength, 1.0f, testkit::kTolerance);
    EXPECT_NEAR(environment.underwater.maxDistance, 120.0f, testkit::kTolerance);
    EXPECT_EQ(environment.underwater.lightSteps, 48);
    EXPECT_NEAR(environment.underwater.distortion, 0.001f, testkit::kTolerance);
    EXPECT_NEAR(environment.underwater.bottomY, -2.0f, testkit::kTolerance);
    EXPECT_NEAR(environment.underwater.halfExtent.x, 0.0f, testkit::kTolerance);
    EXPECT_NEAR(environment.underwater.halfExtent.y, 0.0f, testkit::kTolerance);
}
TEST_F(WaterRenderResourceTest, ViewHiddenWaterCannotProvideUnderwaterOrLegacyCausticEffects)
{
    for (float opticalMode : {0.0f, 1.0f}) {
        SCOPED_TRACE(opticalMode);
        scene::Scene scene;
        const auto id = AddWater(scene);
        auto* water = scene.GetComponent<scene::WaterComponent>(id);
        auto* object = scene.GetGameObject(id);
        ASSERT_NE(water, nullptr);
        ASSERT_NE(object, nullptr);
        object->layer = 7;
        water->buoyancyDepth = 5;
        asset::MaterialAssetAuthoring material;
        material.params["opticalMode"] = {opticalMode};
        material.params["enableCaustics"] = {1};
        material.params["causticsIntensity"] = {0.7f};
        water->materialPath = SaveMaterial(opticalMode > 0.5f ? "layer-optical.mat" : "layer-legacy.mat", material);
        ASSERT_FALSE(water->materialPath.empty());
        renderer::Camera camera;
        camera.m_position = {0, -1, 0};
        const auto visible = ExtractEnvironment(scene, camera);
        EXPECT_TRUE(visible.underwater.enabled);
        EXPECT_TRUE(visible.caustics.enabled);
        const auto hidden = ExtractEnvironment(scene, camera, ~(1u << 7u));
        EXPECT_FALSE(hidden.underwater.enabled);
        EXPECT_FALSE(hidden.caustics.enabled);
        const auto visibleAgain = ExtractEnvironment(scene, camera, 1u << 7u);
        EXPECT_TRUE(visibleAgain.underwater.enabled);
        EXPECT_TRUE(visibleAgain.caustics.enabled);
    }
}
} /// @note namespace fbzz::tests
