/// @file    HostResourceBindingTests.cpp
/// @brief   ホスト・Scene・Script の更新先が他 Manager の生成・破棄で切り替わらないことを検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-31
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <TestKit/Engine/ResourceTrackingRenderer.hpp>
#include <Engine/Core/Application.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/SceneManager.hpp>
#include <Engine/Scene/ProjectRuntime.hpp>
#include <Engine/Scene/SceneSerializer.hpp>
#include <Engine/Scene/Script.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Scene/Components/ProceduralMeshComponent.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Graphics/Renderer/ResourceManager.hpp>
#include <Physics/World.hpp>

namespace fbzz::tests {
class HostResourceBindingTest : public testkit::EngineFixture {
protected:
    void TearDown() override
    {
        core::Application::Get().SetResourceManager(nullptr);
        EngineFixture::TearDown();
    }
};

TEST_F(HostResourceBindingTest, ApplicationKeepsExplicitOwnerWhenAnotherManagerEnds)
{
    testkit::ResourceTrackingRenderer device, otherDevice;
    auto resources = std::make_unique<renderer::ResourceManager>(device);
    auto& app = core::Application::Get();
    app.SetResourceManager(resources.get());
    {
        renderer::ResourceManager other(otherDevice);
        EXPECT_EQ(app.GetResourceManager(), resources.get());
    }
    EXPECT_EQ(app.GetResourceManager(), resources.get());
    resources.reset();
    renderer::ResourceManager replacement(device);
    EXPECT_EQ(app.GetResourceManager(), nullptr);
}

TEST_F(HostResourceBindingTest, SchedulerUploadsOnlyToItsExplicitManager)
{
    testkit::ResourceTrackingRenderer device, otherDevice;
    renderer::ResourceManager resources(device), other(otherDevice);
    scene::Scene scene;
    physics::World world;
    auto& object = scene.CreateGameObject("Procedural");
    auto& procedural = object.AddComponent<scene::ProceduralMeshComponent>();
    procedural.builder.AddQuad(math::Vector3::ZERO, math::Vector3::RIGHT, math::Vector3::UP);
    procedural.dirty = scene::MeshDirty::All; procedural.materialPath.clear();
    scene::SceneManager manager;
    manager.SetResourceManager(&resources); manager.SetScene(&scene); manager.SetSimulating(false);
    manager.LateUpdate(0, world);
    const auto* view = object.GetComponent<scene::MeshRenderer>();
    ASSERT_NE(view, nullptr); ASSERT_NE(view->mesh, nullptr);
    EXPECT_EQ(view->mesh->vertexBuffer.owner, resources.GetInstanceId());
    EXPECT_EQ(device.m_buffersAlive, 2u); EXPECT_EQ(otherDevice.m_buffersAlive, 0u);
    EXPECT_EQ(scene.GetResourceManager(), &resources);
    manager.SetResourceManager(nullptr);
    EXPECT_EQ(scene.GetResourceManager(), nullptr);
    manager.SetScene(nullptr);
}

TEST_F(HostResourceBindingTest, ScriptUsesItsScenesManagerAndNeverAnUnrelatedFallback)
{
    testkit::ResourceTrackingRenderer device, otherDevice;
    auto resources = std::make_unique<renderer::ResourceManager>(device);
    renderer::ResourceManager other(otherDevice);
    scene::Scene scene;
    auto& owner = scene.CreateGameObject("Script");
    scene::Script script; script.SetContext(&scene, &owner);
    EXPECT_EQ(script.GetShaderDescriptor("owned.hlsl"), nullptr);
    EXPECT_EQ(otherDevice.m_shadersCreated, 0u);
    scene.SetResourceManager(resources.get());
    ASSERT_NE(script.GetShaderDescriptor("owned.hlsl"), nullptr);
    EXPECT_EQ(device.m_shadersCreated, 1u); EXPECT_EQ(otherDevice.m_shadersCreated, 0u);
    resources.reset();
    EXPECT_EQ(script.GetShaderDescriptor("expired.hlsl"), nullptr);
    EXPECT_EQ(otherDevice.m_shadersCreated, 0u);
    script.SetContext(nullptr, nullptr);
}

TEST_F(HostResourceBindingTest, RuntimeBindsExternalAndLoadedScenesBeforeCallbacks)
{
    testkit::ResourceTrackingRenderer device, otherDevice;
    renderer::ResourceManager resources(device), other(otherDevice);
    testkit::TempDir root{"host-resources"};
    ASSERT_TRUE(root.IsValid());
    scene::Scene external;
    scene::ProjectRuntime runtime;
    runtime.BindExternalScene(&external);
    runtime.RegisterScenes(root.Path(), resources);
    EXPECT_EQ(external.GetResourceManager(), &resources);
    const std::string text = "version = 1\n[[gameobjects]]\nname = 'Loaded'\n";
    auto loaded = scene::SceneSerializer::LoadFromText(text, &resources, root.File("loaded.scene").generic_string());
    ASSERT_NE(loaded, nullptr); EXPECT_EQ(loaded->GetResourceManager(), &resources);
    scene::Scene moved = std::move(*loaded);
    EXPECT_EQ(moved.GetResourceManager(), &resources);
    EXPECT_EQ(loaded->GetResourceManager(), nullptr);
    moved.Clear();
    EXPECT_EQ(moved.GetResourceManager(), &resources);
    runtime.BindExternalScene(nullptr);
}

TEST_F(HostResourceBindingTest, RegisteredSceneFilesUseTheCurrentExplicitManagerAtLoadTime)
{
    testkit::ResourceTrackingRenderer device, otherDevice;
    renderer::ResourceManager first(device), second(otherDevice);
    testkit::TempDir root{"bound-scene-file"}; ASSERT_TRUE(root.IsValid());
    const auto path = root.File("bound.scene").generic_string();
    ASSERT_TRUE(util::FileSystem::WriteText(path,
        "version = 1\n[[gameobjects]]\nname = 'Cube'\n[gameobjects.MeshRenderer]\nmesh = 'primitive:cube'\n"));
    physics::World world;
    scene::SceneManager manager;
    manager.RegisterFromFile("Bound", path, first);
    manager.SetResourceManager(&second); manager.SetSimulating(false);
    ASSERT_TRUE(manager.LoadScene("Bound")); manager.Update(0, world);
    auto* active = manager.GetActive(); ASSERT_NE(active, nullptr);
    auto* object = active->Find("Cube"); ASSERT_NE(object, nullptr);
    const auto* mesh = object->GetComponent<scene::MeshRenderer>(); ASSERT_NE(mesh, nullptr); ASSERT_NE(mesh->mesh, nullptr);
    EXPECT_EQ(mesh->mesh->vertexBuffer.owner, second.GetInstanceId());
    EXPECT_EQ(active->GetResourceManager(), &second);
    EXPECT_EQ(device.m_buffersAlive, 0u);
    manager.ClearScenes();
}
} /// @note namespace fbzz::tests
