/// @file    ClothSystemTests.cpp
/// @brief   布の固定更新・Transform・保存と実行状態の分離を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-17
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Engine/Core/Scheduler/SystemContext.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/SceneSerializer.hpp>
#include <Engine/Scene/Components/ClothComponent.hpp>
#include <Engine/Scene/Systems/ClothSystem.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Asset/ClothAsset.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Renderer/IRenderer.hpp>
#include <Engine/Renderer/IBuffer.hpp>
#include <Engine/Renderer/ITexture.hpp>
#include <Engine/Renderer/IShader.hpp>
#include <Engine/Renderer/IConstantBuffer.hpp>
#include <Engine/Renderer/IPipelineState.hpp>
#include <Engine/Renderer/IRenderTarget.hpp>
#include <Engine/Renderer/IStructuredBuffer.hpp>
#include <Physics/World.hpp>
#include <cmath>

namespace fbzz::tests {
namespace {
class ClothCpuBuffer final : public renderer::IBuffer {
public:
    ClothCpuBuffer(size_t size, uint32_t stride) : m_size(size), m_stride(stride) {}
    void Update(const void*, size_t size) override { EXPECT_LE(size,m_size); }
    size_t GetSize() const override { return m_size; }
    uint32_t GetStride() const override { return m_stride; }
private:
    size_t m_size;
    uint32_t m_stride;
};
/// @note CPU アセットの実ロードに必要な ResourceManager を作る。デバイスもウィンドウも生成しない。
class ClothCpuRenderer final : public renderer::IRenderer {
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
    std::unique_ptr<renderer::IShader> CreateNativeShader(const std::string&) override { return {}; }
    std::unique_ptr<renderer::IConstantBuffer> CreateNativeConstantBuffer(size_t) override { return {}; }
    std::unique_ptr<renderer::IBuffer> CreateNativeVertexBuffer(const void*, size_t size, uint32_t stride) override
    { return std::make_unique<ClothCpuBuffer>(size,stride); }
    std::unique_ptr<renderer::IBuffer> CreateNativeIndexBuffer(const void*, uint32_t count) override
    { return std::make_unique<ClothCpuBuffer>(count*sizeof(uint32_t),0); }
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
}
class ClothSystemTest : public testkit::EngineFixture {
protected:
    ClothCpuRenderer m_cpuRenderer;
    std::unique_ptr<renderer::ResourceManager> m_skinResources;
    asset::Model m_skinModel;
    testkit::TempDir m_skinTemp{"clothsystemskin"};
    scene::Scene m_scene;
    physics::World m_world;
    scene::EntityID m_id{};
    std::string m_skinPath;
    void SetUp() override
    {
        testkit::EngineFixture::SetUp();
        auto& go = m_scene.CreateGameObject("Curtain");
        m_id = go.GetID();
        go.transform.position = go.transform.worldPosition = {0, 3, 0};
        auto& cloth = go.AddComponent<scene::ClothComponent>();
        cloth.segments = 8;
        cloth.settings.windVelocity = {0, 0, 2};
    }
    void TearDown() override
    {
        if (m_skinResources) asset::AssetManager::UnloadAll();
        m_skinResources.reset();
        testkit::EngineFixture::TearDown();
    }
    void SetSkinSource(scene::EntityID& owner, scene::EntityID& left, scene::EntityID& right, bool bindRender = false)
    {
        ASSERT_TRUE(m_skinTemp.IsValid());
        m_skinModel.skeleton = std::make_unique<asset::Skeleton>();
        auto& skeleton = *m_skinModel.skeleton;
        /// @note ソースと異なるパレット順で、添字ではなく名前で解決することを検証する。
        skeleton.bones = {{"Right", 1, math::Matrix4::Translate({-1,0,0})}, {"Left", 0, math::Matrix4::Identity()}};
        owner = m_scene.CreateGameObject("SkinOwner").GetID();
        left = m_scene.CreateGameObject("LeftBone").GetID();
        right = m_scene.CreateGameObject("RightBone").GetID();
        auto& renderer = m_scene.GetGameObject(owner)->AddComponent<scene::SkinnedMeshRenderer>();
        renderer.model = &m_skinModel;
        renderer.nodeEntities = {left, right};
        m_scene.GetGameObject(left)->transform.position = m_scene.GetGameObject(left)->transform.worldPosition = {0,3,0};
        m_scene.GetGameObject(right)->transform.position = m_scene.GetGameObject(right)->transform.worldPosition = {1,3,0};
        renderer::Mesh mesh;
        mesh.cpuVertices = {{{0,0,0}, {0,0,1}, {1,0,0}, {}}, {{0,-1,0}, {0,0,1}, {1,0,0}, {}},
            {{1,0,0}, {0,0,1}, {1,0,0}, {}}};
        mesh.cpuIndices = {0,1,2};
        asset::ClothAsset source;
        ASSERT_TRUE(asset::CreateClothAsset(mesh, {}, source));
        source.skinBones = {{"Left", math::Matrix4::Identity()}, {"Right", math::Matrix4::Translate({-1,0,0})}};
        for (const auto& p : source.particles) source.skinWeights.push_back({p, {0,1,0,0}, {0.25f,0.75f,0,0}});
        source.pins = {0};
        if (bindRender) {
            mesh.cpuVertices.push_back({{0.25f,-0.25f,0.05f},{0,0,1},{1,0,0},{0.25f,0.25f}});
            mesh.cpuIndices = {0,1,3,1,2,3,2,0,3};
            asset::ClothAsset bound;
            ASSERT_TRUE(asset::BindClothRenderMesh(source,mesh,0.1f,bound));
            source = std::move(bound);
        }
        m_skinPath = m_skinTemp.File("skin.cloth").generic_string();
        ASSERT_TRUE(asset::SaveClothAssetToFile(m_skinPath, source));
        m_skinResources = std::make_unique<renderer::ResourceManager>(m_cpuRenderer);
        asset::AssetManager::Init(*m_skinResources, m_skinTemp.Path().generic_string() + "/");
        Cloth().clothAssetPath = m_skinPath;
        Cloth().useSkinning = true;
        Cloth().skinTarget = owner;
        Cloth().skinMaxDistance = 0.1f;
        Cloth().settings.gravity = {};
        Cloth().settings.windVelocity = {};
    }
    scene::ClothComponent& Cloth() { return *m_scene.GetComponent<scene::ClothComponent>(m_id); }
    void Tick(bool simulate = true)
    {
        SystemContext ctx{m_scene, m_world, nullptr, nullptr, 1.0f / 60.0f, 1.0f / 60.0f, simulate, true};
        scene::ClothSystem system;
        system.Update(ctx);
    }
};

TEST_F(ClothSystemTest, GridMovesInWindButTopEdgeStaysAttached)
{
    for (int i = 0; i < 120; ++i) Tick();
    ASSERT_TRUE(Cloth().runtime.initialized);
    EXPECT_FALSE(Cloth().runtime.failed);
    const auto& positions = Cloth().runtime.solver.Positions();
    ASSERT_EQ(positions.size(), 81u);
    EXPECT_VEC3_NEAR(positions[0], (math::Vector3{-1, 3, 0}), 1.0e-5f);
    EXPECT_GT(positions.back().z, 0.01f);
    for (const auto& position : positions) {
        EXPECT_TRUE(std::isfinite(position.x));
        EXPECT_TRUE(std::isfinite(position.y));
        EXPECT_TRUE(std::isfinite(position.z));
    }
}
TEST_F(ClothSystemTest, PaintedDistanceLimitsStaticClothAndInvalidEntriesFreezeStep)
{
    Cloth().maxDistances = {{80,0.0f},{79,0.02f}};
    for (int i = 0; i < 20; ++i) Tick();
    ASSERT_FALSE(Cloth().runtime.failed);
    const auto& state = Cloth().runtime;
    EXPECT_VEC3_NEAR(state.solver.Positions()[80],(state.restLocal[80]+math::Vector3{0,3,0}),1.0e-5f);
    EXPECT_LE((state.solver.Positions()[79]-state.restLocal[79]-math::Vector3{0,3,0}).Length(),0.02001f);
    const auto before = state.solver.Positions();
    Cloth().maxDistances.push_back({80,0.5f});
    Tick();
    EXPECT_TRUE(state.failed);
    for (size_t i = 0; i < before.size(); ++i) EXPECT_VEC3_NEAR(state.solver.Positions()[i],before[i],1.0e-6f);
    Cloth().maxDistances.clear();
    for (int i = 0; i < 20; ++i) Tick();
    EXPECT_FALSE(state.failed);
    EXPECT_GT((state.solver.Positions()[80]-before[80]).Length(),0.001f);
}
TEST_F(ClothSystemTest, PaintedDistanceOverridesSkinDefaultAndAttachmentsOverridePaint)
{
    scene::EntityID owner,left,right;
    SetSkinSource(owner,left,right);
    Cloth().maxDistances = {{1,0.0f},{2,0.02f}};
    Tick();
    m_scene.GetGameObject(right)->transform.worldPosition.z = 0.4f;
    Tick();
    ASSERT_FALSE(Cloth().runtime.failed);
    EXPECT_NEAR(Cloth().runtime.solver.Positions()[1].z,0.3f,1.0e-5f);
    EXPECT_LE((Cloth().runtime.solver.Positions()[2]-math::Vector3{1,3,0.3f}).Length(),0.02001f);
    Cloth().attachments = {{1,left,{0,-1,0},0.0f}};
    Tick();
    EXPECT_NEAR(Cloth().runtime.solver.Positions()[1].z,0.0f,1.0e-5f);
}
TEST_F(ClothSystemTest, LowResolutionPhysicsTransfersInterpolatedPoseToRenderMesh)
{
    scene::EntityID owner,left,right;
    SetSkinSource(owner,left,right,true);
    Cloth().skinMaxDistance = 0;
    Cloth().materialPath.clear();
    Tick();
    ASSERT_EQ(Cloth().runtime.solver.Positions().size(),3u);
    ASSERT_EQ(Cloth().runtime.mesh.Vertices().size(),4u);
    m_scene.GetGameObject(right)->transform.worldPosition.z = 0.4f;
    Tick();
    ASSERT_FALSE(Cloth().runtime.failed);
    SystemContext ctx{m_scene,m_world,m_skinResources.get(),nullptr,1.0f/60,1.0f/60,true,true,0.5f};
    scene::ClothRenderSystem{}.Update(ctx);
    ASSERT_FALSE(Cloth().runtime.failed);
    ASSERT_TRUE(Cloth().runtimeMesh.HasMesh());
    const auto* mesh = Cloth().runtimeMesh.Current();
    ASSERT_EQ(mesh->cpuVertices.size(),4u);
    EXPECT_EQ(mesh->cpuIndices.size(),9u);
    EXPECT_VEC3_NEAR(mesh->cpuVertices[3].position,(math::Vector3{0.25f,-0.25f,0.2f}),1.0e-5f);
    EXPECT_FLOAT_EQ(mesh->cpuVertices[3].uv.x,0.25f);
    EXPECT_NEAR(mesh->boundsCenter.z+mesh->boundsExtents.z,0.2f,1.0e-5f);
}

TEST_F(ClothSystemTest, PauseDoesNotAdvanceAndTeleportRebuildsAtNewOrigin)
{
    Tick();
    const auto before = Cloth().runtime.solver.Positions();
    Tick(false);
    EXPECT_VEC3_NEAR(Cloth().runtime.solver.Positions().back(), before.back(), 1.0e-6f);
    auto& transform = m_scene.GetGameObject(m_id)->transform;
    transform.position = transform.worldPosition = {100, 3, 0};
    Tick();
    EXPECT_VEC3_NEAR(Cloth().runtime.solver.Positions()[0], (math::Vector3{99, 3, 0}), 1.0e-5f);
    EXPECT_GT(Cloth().runtime.solver.Positions().back().x, 98.0f);
}

TEST_F(ClothSystemTest, GroundContactSupportsTheFreeEdge)
{
    Cloth().groundEnabled = true;
    Cloth().groundHeight = 1.5f;
    for (int i = 0; i < 30; ++i) Tick();
    EXPECT_FALSE(Cloth().runtime.failed);
    for (const auto& p : Cloth().runtime.solver.Positions())
        EXPECT_GE(p.y, 1.5f + Cloth().settings.thickness - 1.0e-4f);
}

TEST_F(ClothSystemTest, CopyKeepsSettingsButDropsSimulationState)
{
    Tick();
    scene::ClothComponent copy = Cloth();
    EXPECT_EQ(copy.segments, 8);
    EXPECT_VEC3_NEAR(copy.settings.windVelocity, (math::Vector3{0, 0, 2}), 1.0e-6f);
    EXPECT_FALSE(copy.runtime.initialized);
    EXPECT_TRUE(copy.runtime.solver.Positions().empty());
    EXPECT_EQ(copy.runtime.ownerScene, nullptr);
    EXPECT_FALSE(copy.runtimeMesh.HasMesh());
}

TEST_F(ClothSystemTest, SceneRoundTripKeepsSettingsWithoutRuntimeState)
{
    const auto target = m_scene.CreateGameObject("BoneTarget").GetID();
    Cloth().attachments.push_back({0, target, {0.1f, 0.2f, 0.3f}, 0.0f});
    Cloth().skinTarget = target;
    Cloth().skinMaxDistance = 0.25f;
    Cloth().maxDistances = {{1,0.0f},{2,0.2f}};
    Cloth().settings.selfCollisionDistance = 0.05f;
    Cloth().settings.dihedralBending = true;
    Cloth().collisionMask = 0x80000001u;
    Cloth().twoWayCoupling = true;
    Cloth().interCollisionDistance = 0.03f;
    Cloth().interCollisionFaces = true;
    Cloth().interContinuousCollision = true;
    Cloth().receiveFlowFields = true;
    Cloth().flowFieldChannels = 0x80000001u;
    Cloth().overridePins = true;
    Cloth().pinnedParticles = {0, 8};
    Tick();
    Cloth().useSkinning = true;
    Cloth().clothAssetPath = "guid:0123456789abcdef0123456789abcdef";
    testkit::TempDir temp{"cloth"};
    ASSERT_TRUE(temp.IsValid());
    const std::string path = temp.File("cloth.scene").generic_string();
    ASSERT_TRUE(scene::SceneSerializer::Save(m_scene, path));
    const auto restored = scene::SceneSerializer::LoadData(path);
    ASSERT_NE(restored, nullptr);
    auto* go = restored->Find("Curtain");
    ASSERT_NE(go, nullptr);
    const auto* cloth = go->GetComponent<scene::ClothComponent>();
    ASSERT_NE(cloth, nullptr);
    EXPECT_EQ(cloth->segments, 8);
    EXPECT_TRUE(cloth->settings.dihedralBending);
    EXPECT_EQ(cloth->collisionMask,0x80000001u);
    EXPECT_TRUE(cloth->twoWayCoupling);
    EXPECT_FLOAT_EQ(cloth->interCollisionDistance,0.03f);
    EXPECT_TRUE(cloth->interCollisionFaces);
    EXPECT_TRUE(cloth->interContinuousCollision);
    EXPECT_TRUE(cloth->receiveFlowFields);
    EXPECT_EQ(cloth->flowFieldChannels,0x80000001u);
    EXPECT_EQ(cloth->materialPath, Cloth().materialPath);
    EXPECT_EQ(cloth->clothAssetPath, Cloth().clothAssetPath);
    EXPECT_TRUE(cloth->overridePins);
    EXPECT_EQ(cloth->pinnedParticles, Cloth().pinnedParticles);
    EXPECT_VEC3_NEAR(cloth->settings.windVelocity, (math::Vector3{0, 0, 2}), 1.0e-6f);
    EXPECT_FALSE(cloth->runtime.initialized);
    EXPECT_TRUE(cloth->runtime.solver.Positions().empty());
    ASSERT_EQ(cloth->attachments.size(), 1u);
    const auto* restoredTarget = restored->Find("BoneTarget");
    ASSERT_NE(restoredTarget, nullptr);
    EXPECT_EQ(cloth->attachments[0].target, restoredTarget->GetID());
    EXPECT_VEC3_NEAR(cloth->attachments[0].localPosition, (math::Vector3{0.1f, 0.2f, 0.3f}), 1.0e-6f);
    EXPECT_FLOAT_EQ(cloth->attachments[0].maxDistance, 0.0f);
    EXPECT_TRUE(cloth->runtime.attachmentCenters.empty());
    EXPECT_EQ(cloth->skinTarget, restoredTarget->GetID());
    EXPECT_TRUE(cloth->useSkinning);
    EXPECT_FLOAT_EQ(cloth->skinMaxDistance, 0.25f);
    EXPECT_EQ(cloth->maxDistances, Cloth().maxDistances);
    EXPECT_FLOAT_EQ(cloth->settings.selfCollisionDistance, 0.05f);
}

TEST_F(ClothSystemTest, PinOverrideRebuildsAndEmptyOverrideReleasesAll)
{
    Cloth().overridePins = true;
    Cloth().pinnedParticles = {0, 8, 0};
    for (int i = 0; i < 30; ++i) Tick();
    ASSERT_EQ(Cloth().runtime.pins, (std::vector<uint32_t>{0,8}));
    EXPECT_VEC3_NEAR(Cloth().runtime.solver.Positions()[0], (math::Vector3{-1,3,0}), 1.0e-5f);
    Cloth().pinnedParticles.clear();
    Tick();
    EXPECT_TRUE(Cloth().runtime.pins.empty());
    EXPECT_LT(Cloth().runtime.solver.Positions()[0].y, 3.0f);
}

TEST_F(ClothSystemTest, InvalidPinStopsAndValidEditRecovers)
{
    Tick();
    Cloth().overridePins = true;
    Cloth().pinnedParticles = {-1};
    Tick();
    EXPECT_FALSE(Cloth().runtime.initialized);
    EXPECT_TRUE(Cloth().runtime.failed);
    Cloth().pinnedParticles = {0};
    Tick();
    EXPECT_TRUE(Cloth().runtime.initialized);
    EXPECT_FALSE(Cloth().runtime.failed);
    EXPECT_EQ(Cloth().runtime.renderToParticle.size(), 81u);
}

TEST_F(ClothSystemTest, BoneAttachmentOverridesOwnerPinAndPauses)
{
    const auto targetId = m_scene.CreateGameObject("BoneTarget").GetID();
    auto& target = m_scene.GetGameObject(targetId)->transform;
    target.position = target.worldPosition = {-1, 3, 0};
    target.scale = target.worldScale = {2, 2, 2};
    Cloth().attachments.push_back({0, targetId, {0.1f, 0, 0}, 0.0f});
    Tick();
    ASSERT_FALSE(Cloth().runtime.failed);
    EXPECT_VEC3_NEAR(Cloth().runtime.solver.Positions()[0], (math::Vector3{-0.8f, 3, 0}), 1.0e-5f);
    target.position = target.worldPosition = {-1, 3.1f, 0};
    Tick(false);
    EXPECT_NEAR(Cloth().runtime.solver.Positions()[0].y, 3.0f, 1.0e-5f);
    Tick();
    EXPECT_NEAR(Cloth().runtime.solver.Positions()[0].y, 3.1f, 1.0e-5f);
    EXPECT_NEAR(Cloth().runtime.solver.Velocities()[0].y, 6.0f, 0.002f);
    target.rotation = target.worldRotation = {0, 0, 1, 0};
    Tick();
    EXPECT_VEC3_NEAR(Cloth().runtime.solver.Positions()[0], (math::Vector3{-1.2f, 3.1f, 0}), 1.0e-5f);
    Cloth().attachments.clear();
    Tick();
    EXPECT_VEC3_NEAR(Cloth().runtime.solver.Positions()[0], (math::Vector3{-1, 3, 0}), 1.0e-5f);
}

TEST_F(ClothSystemTest, PositiveDistanceBoundsParticleAroundMovingTarget)
{
    const auto targetId = m_scene.CreateGameObject("BoneTarget").GetID();
    auto& target = m_scene.GetGameObject(targetId)->transform;
    target.position = target.worldPosition = {1, 1, 0};
    Cloth().attachments.push_back({80, targetId, {}, 0.05f});
    for (int i = 0; i < 20; ++i) {
        target.position = target.worldPosition = {1, 1, i * 0.002f};
        Tick();
        ASSERT_FALSE(Cloth().runtime.failed);
        EXPECT_LE((Cloth().runtime.solver.Positions()[80] - target.worldPosition).Length(), 0.05001f);
    }
}

TEST_F(ClothSystemTest, InvalidAttachmentStopsAtomicallyAndCorrectionRecovers)
{
    Tick();
    const auto before = Cloth().runtime.solver.Positions();
    Cloth().attachments.push_back({0, {}, {}, 0.0f});
    Tick();
    EXPECT_TRUE(Cloth().runtime.failed);
    EXPECT_VEC3_NEAR(Cloth().runtime.solver.Positions().back(), before.back(), 1.0e-6f);
    const auto targetId = m_scene.CreateGameObject("BoneTarget").GetID();
    auto& target = m_scene.GetGameObject(targetId)->transform;
    target.position = target.worldPosition = {-1, 3, 0};
    Cloth().attachments[0].target = targetId;
    Cloth().attachments.push_back(Cloth().attachments[0]);
    Tick();
    EXPECT_TRUE(Cloth().runtime.failed);
    Cloth().attachments.pop_back();
    Tick();
    EXPECT_FALSE(Cloth().runtime.failed);
    EXPECT_VEC3_NEAR(Cloth().runtime.solver.Positions()[0], target.worldPosition, 1.0e-5f);
}

TEST_F(ClothSystemTest, SkinningResolvesNamesBlendsBonesAndAllowsExplicitOverride)
{
    scene::EntityID owner, left, right;
    SetSkinSource(owner, left, right);
    Tick();
    ASSERT_FALSE(Cloth().runtime.failed);
    ASSERT_EQ(Cloth().runtime.solver.Positions().size(), 3u);
    EXPECT_VEC3_NEAR(Cloth().runtime.solver.Positions()[0], (math::Vector3{0,3,0}), 1.0e-5f);
    auto& transform = m_scene.GetGameObject(right)->transform;
    transform.position = transform.worldPosition = {1,3,0.4f};
    Tick();
    ASSERT_FALSE(Cloth().runtime.failed);
    EXPECT_VEC3_NEAR(Cloth().runtime.solver.Positions()[0], (math::Vector3{0,3,0.3f}), 1.0e-5f);
    EXPECT_LE((Cloth().runtime.solver.Positions()[1] - math::Vector3{0,2,0.3f}).Length(), 0.10001f);
    Cloth().attachments.push_back({0, left, {}, 0.0f});
    Tick();
    EXPECT_FALSE(Cloth().runtime.failed);
    EXPECT_VEC3_NEAR(Cloth().runtime.solver.Positions()[0], (math::Vector3{0,3,0}), 1.0e-5f);
    Cloth().attachments.clear();
    Tick();
    EXPECT_FALSE(Cloth().runtime.failed);
    EXPECT_NEAR(Cloth().runtime.solver.Positions()[0].z, 0.3f, 1.0e-5f);
    EXPECT_NEAR(Cloth().runtime.solver.Velocities()[0].z, 18.0f, 0.002f);
}

TEST_F(ClothSystemTest, MissingSkinBoneStopsAndValidMappingRecovers)
{
    scene::EntityID owner, left, right;
    SetSkinSource(owner, left, right);
    Tick();
    ASSERT_FALSE(Cloth().runtime.failed);
    const auto before = Cloth().runtime.solver.Positions();
    m_skinModel.skeleton->bones[0].name = "Missing";
    Tick();
    EXPECT_TRUE(Cloth().runtime.failed);
    for (size_t i = 0; i < before.size(); ++i) EXPECT_VEC3_NEAR(Cloth().runtime.solver.Positions()[i], before[i], 1.0e-6f);
    m_skinModel.skeleton->bones[0].name = "Right";
    Tick();
    EXPECT_FALSE(Cloth().runtime.failed);
    Cloth().useSkinning = false;
    Tick();
    EXPECT_TRUE(Cloth().runtime.skinCenters.empty());
}
}
