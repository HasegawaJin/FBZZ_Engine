/// @file    MeshTrailRenderResourceTests.cpp
/// @brief   残像抽出の作成元分離、Reset 後の再転送と終了時の返却を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-31
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <TestKit/Engine/ResourceTrackingRenderer.hpp>
#include <TestKit/TempDir.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Components/MeshTrailComponent.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/Systems/RenderMeshTrailExtractor.hpp>
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <Graphics/Renderer/RenderScene.hpp>
#include <Graphics/Renderer/ResourceManager.hpp>
#include <cstring>

namespace fbzz::tests {
class MeshTrailRenderResourceTest : public testkit::EngineFixture {
protected:
    void SetUp() override
    {
        EngineFixture::SetUp();
        m_time = Time::time;
        m_playing = scene::Script::IsInPlayMode();
        scene::Script::SetInPlayMode(true);
        Time::time = 10.0f;
        ASSERT_TRUE(m_temp.IsValid());
        m_resources = std::make_unique<renderer::ResourceManager>(m_device);
        m_device.m_shaderDescriptor.cbufferSize = 16;
        asset::AssetManager::Init(*m_resources, m_temp.Path().generic_string() + "/");
        asset::MaterialAssetAuthoring material;
        material.renderPath = asset::RenderPath::Trail;
        material.shaderPath = "TrailOwnership.hlsl";
        m_materialPath = m_temp.File("Trail.mat").generic_string();
        ASSERT_TRUE(asset::SaveMaterialAssetToFile(m_materialPath, material));
        m_object = &m_scene.CreateGameObject("Trail");
        auto& trail = m_object->AddComponent<scene::MeshTrailComponent>();
        trail.materialPath = m_materialPath;
        trail.minVertexDist = 0.0f;
        m_object->AddComponent<scene::MeshRenderer>();
        SetMesh(*m_resources, false);
    }
    void TearDown() override
    {
        m_scene.Clear();
        m_model.reset();
        asset::AssetManager::UnloadAll();
        m_resources.reset();
        EXPECT_EQ(m_device.m_constantsAlive, 0u);
        EXPECT_EQ(m_device.m_buffersAlive, 0u);
        Time::time = m_time;
        scene::Script::SetInPlayMode(m_playing);
        EngineFixture::TearDown();
    }
    void SetMesh(renderer::ResourceManager& resources, bool skinned)
    {
        m_model = std::make_unique<asset::Model>();
        auto mesh = std::make_unique<renderer::Mesh>();
        const float vertices[9]{};
        const uint32_t indices[3]{0, 1, 2};
        mesh->vertexBuffer = resources.CreateVertexBuffer(vertices, sizeof(vertices), sizeof(float) * 3);
        mesh->indexBuffer = resources.CreateIndexBuffer(indices, 3);
        mesh->vertexCount = mesh->indexCount = 3;
        mesh->isSkinned = skinned;
        m_model->meshes.push_back(std::move(mesh));
        if (skinned) {
            m_object->GetComponent<scene::SkinnedMeshRenderer>()->model = m_model.get();
        } else {
            m_object->GetComponent<scene::MeshRenderer>()->mesh = m_model->meshes.front().get();
        }
    }
    renderer::RenderMeshTrailInput Extract(testkit::ResourceTrackingRenderer& device, renderer::ResourceManager& resources)
    {
        renderer::Camera camera;
        renderer::RenderSettings settings;
        renderer::RenderPassHandles handles;
        scene::RenderPassContext context(m_scene, device, resources, camera, settings, {}, UINT32_MAX, handles);
        renderer::RenderScene output;
        scene::ExtractRenderMeshTrails(context, output);
        EXPECT_EQ(output.meshTrails.size(), 1u);
        return output.meshTrails.empty() ? renderer::RenderMeshTrailInput{} : output.meshTrails.front();
    }
    testkit::TempDir m_temp{"mesh-trail-resources"};
    testkit::ResourceTrackingRenderer m_device;
    std::unique_ptr<renderer::ResourceManager> m_resources;
    std::unique_ptr<asset::Model> m_model;
    scene::Scene m_scene;
    scene::GameObject* m_object = nullptr;
    std::string m_materialPath;
    float m_time = 0;
    bool m_playing = false;
};

TEST_F(MeshTrailRenderResourceTest, AlternatingManagersRetainMaterialCachesAndReturnReplacedComponentBuffers)
{
    testkit::ResourceTrackingRenderer otherDevice;
    otherDevice.m_shaderDescriptor.cbufferSize = 16;
    renderer::ResourceManager other(otherDevice);
    const auto initial = Extract(m_device, *m_resources);
    ASSERT_NE(m_resources->Get(initial.material), nullptr);
    ASSERT_NE(m_resources->Get(initial.shader), nullptr);
    SetMesh(other, false);
    const auto switched = Extract(otherDevice, other);

    EXPECT_NE(other.Get(switched.shader), nullptr);
    EXPECT_NE(other.Get(switched.material), nullptr);
    EXPECT_NE(other.Get(switched.colorBuffer), nullptr);
    EXPECT_EQ(m_resources->Get(initial.colorBuffer), nullptr);
    EXPECT_NE(m_resources->Get(initial.material), nullptr);
    SetMesh(*m_resources, false);
    const auto repeated = Extract(m_device, *m_resources);
    EXPECT_EQ(repeated.shader, initial.shader);
    EXPECT_EQ(repeated.material, initial.material);
    EXPECT_EQ(other.Get(switched.colorBuffer), nullptr);
    EXPECT_EQ(m_device.m_shadersCreated, 1u);
    EXPECT_EQ(otherDevice.m_shadersCreated, 1u);
    m_scene.Clear();

    EXPECT_EQ(m_device.m_constantsAlive, 0u);
    EXPECT_EQ(otherDevice.m_constantsAlive, 0u);
    EXPECT_NE(m_resources->Get(initial.shader), nullptr);
    EXPECT_NE(other.Get(switched.shader), nullptr);
}

TEST_F(MeshTrailRenderResourceTest, ResetRecreatesColorMaterialAndCleanSkinningBuffersWithTheCapturedPose)
{
    m_object->RemoveComponent<scene::MeshRenderer>();
    m_object->AddComponent<scene::SkinnedMeshRenderer>();
    auto& animator = m_object->AddComponent<scene::AnimatorComponent>();
    const auto pose = math::Matrix4::TRS({2, 3, 4}, math::Quaternion::Identity(), math::Vector3::ONE);
    animator.boneMatrices.push_back(pose);
    SetMesh(*m_resources, true);
    const auto initial = Extract(m_device, *m_resources);
    ASSERT_NE(m_resources->Get(initial.skin), nullptr);
    const auto expected = m_device.m_constantBytes.at(m_resources->Get(initial.skin));
    ASSERT_GE(expected.size(), sizeof(pose));
    EXPECT_EQ(std::memcmp(expected.data(), &pose, sizeof(pose)), 0);

    m_resources->Reset();
    SetMesh(*m_resources, true);
    const auto recreated = Extract(m_device, *m_resources);

    EXPECT_NE(m_resources->Get(recreated.colorBuffer), nullptr);
    EXPECT_NE(m_resources->Get(recreated.material), nullptr);
    ASSERT_NE(m_resources->Get(recreated.skin), nullptr);
    EXPECT_EQ(m_device.m_constantBytes.at(m_resources->Get(recreated.skin)), expected);
    EXPECT_EQ(m_resources->Get(initial.skin), nullptr);
    EXPECT_NE(recreated.skin, initial.skin);
    m_object->RemoveComponent<scene::MeshTrailComponent>();
    EXPECT_EQ(m_resources->Get(recreated.colorBuffer), nullptr);
    EXPECT_EQ(m_resources->Get(recreated.skin), nullptr);
    m_scene.Clear();
    EXPECT_EQ(m_device.m_constantsAlive, 0u);
}

TEST_F(MeshTrailRenderResourceTest, ShaderReloadWithoutParametersReturnsTheOldMaterialBuffer)
{
    const auto initial = Extract(m_device, *m_resources);
    ASSERT_NE(m_resources->Get(initial.material), nullptr);
    m_device.m_shaderDescriptor = {};
    ASSERT_TRUE(m_resources->ReloadShader("TrailOwnership.hlsl").IsValid());

    const auto reloaded = Extract(m_device, *m_resources);

    EXPECT_TRUE(reloaded.shader.IsValid());
    EXPECT_FALSE(reloaded.material.IsValid());
    EXPECT_EQ(m_resources->Get(initial.material), nullptr);
    EXPECT_EQ(m_device.m_constantsAlive, 1u);
}

TEST_F(MeshTrailRenderResourceTest, SwitchingManagersReuploadsTheCapturedSkinAndReturnsItsPreviousBuffer)
{
    m_object->RemoveComponent<scene::MeshRenderer>();
    m_object->AddComponent<scene::SkinnedMeshRenderer>();
    m_object->AddComponent<scene::AnimatorComponent>().boneMatrices.push_back(
        math::Matrix4::TRS({2, 3, 4}, math::Quaternion::Identity(), math::Vector3::ONE));
    SetMesh(*m_resources, true);
    const auto initial = Extract(m_device, *m_resources);
    ASSERT_NE(m_resources->Get(initial.skin), nullptr);
    const auto expected = m_device.m_constantBytes.at(m_resources->Get(initial.skin));
    testkit::ResourceTrackingRenderer otherDevice;
    otherDevice.m_shaderDescriptor.cbufferSize = 16;
    renderer::ResourceManager other(otherDevice);

    SetMesh(other, true);
    const auto switched = Extract(otherDevice, other);

    ASSERT_NE(other.Get(switched.skin), nullptr);
    EXPECT_EQ(otherDevice.m_constantBytes.at(other.Get(switched.skin)), expected);
    EXPECT_EQ(m_resources->Get(initial.skin), nullptr);
    m_scene.Clear();
    EXPECT_EQ(other.Get(switched.skin), nullptr);
    EXPECT_EQ(otherDevice.m_constantsAlive, 0u);
}

TEST_F(MeshTrailRenderResourceTest, EarlyOwnerDestructionRebuildsForTheRemainingManager)
{
    const auto initial = Extract(m_device, *m_resources);
    ASSERT_TRUE(initial.colorBuffer.IsValid());
    m_resources.reset();
    EXPECT_EQ(m_device.m_constantsAlive, 0u);
    m_resources = std::make_unique<renderer::ResourceManager>(m_device);
    asset::AssetManager::Init(*m_resources, m_temp.Path().generic_string() + "/");
    SetMesh(*m_resources, false);

    const auto rebuilt = Extract(m_device, *m_resources);

    EXPECT_NE(m_resources->Get(rebuilt.colorBuffer), nullptr);
    EXPECT_NE(m_resources->Get(rebuilt.material), nullptr);
    EXPECT_NE(m_resources->Get(rebuilt.shader), nullptr);
    EXPECT_EQ(m_resources->Get(initial.colorBuffer), nullptr);
}
} /// @note namespace fbzz::tests
