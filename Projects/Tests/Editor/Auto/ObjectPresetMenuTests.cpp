/// @file    ObjectPresetMenuTests.cpp
/// @brief   常用 Create と音声プリセットの初期設定・親付け・命名・選択・Undo/Redo の契約。
/// @author  Hasegawa Jin
/// @date    2026-10-12
#include <Engine/Scene/Components/AudioListenerComponent.hpp>
#include <Engine/Scene/Components/CameraComponent.hpp>
#include <Engine/Scene/Components/ColliderComponent.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Components/PresentationComponents.hpp>
#include <TestKit/TestKit.hpp>
#include <TestKit/Editor/EditorFixture.hpp>
#include <TestKit/Engine/ResourceTrackingRenderer.hpp>
#include <Editor/Util/ComponentDefaults.hpp>
#include <Editor/Util/SceneIO.hpp>
#include <Graphics/Renderer/ResourceManager.hpp>
#include <Editor/Util/ObjectCreation.hpp>
#include <Editor/Util/ObjectPresets.hpp>
#include <Engine/Scene/ComponentRegistry.hpp>
#include <Engine/Scene/Components/AudioSourceComponent.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/ScriptComponent.hpp>
#include <set>
#include <string>

namespace fbzz::tests {

class ObjectPresetMenuTest : public testkit::EditorFixture {};

TEST_F(ObjectPresetMenuTest, KeepsCommonCreationShallowAndAllPresetIdsUnique)
{
    for (const char* id : {"empty", "3d.cube", "3d.sphere", "3d.plane", "camera"}) {
        const auto* preset = editor::FindObjectPreset(id);
        ASSERT_NE(preset, nullptr) << id;
        EXPECT_TRUE(preset->category.empty()) << id;
        EXPECT_NE(preset->create, nullptr);
    }
    std::set<std::string> ids;
    std::set<std::string> closedCategories;
    std::string currentCategory;
    for (const auto& preset : editor::ObjectPresetCatalog()) {
        EXPECT_TRUE(ids.insert(std::string(preset.id)).second) << preset.id;
        EXPECT_NE(preset.create, nullptr) << preset.id;
        if (preset.category != currentCategory) {
            closedCategories.insert(currentCategory);
            currentCategory = preset.category;
            EXPECT_FALSE(closedCategories.contains(currentCategory)) << currentCategory;
        }
    }
}

TEST_F(ObjectPresetMenuTest, CreatesAFixedQuadAndKeepsBillboardAsAnExplicitChoice)
{
    AttachScene();
    const auto* quadPreset = editor::FindObjectPreset("3d.quad");
    ASSERT_NE(quadPreset, nullptr);
    auto* quad = quadPreset->create(Context());
    ASSERT_NE(quad, nullptr);
    ASSERT_NE(quad->GetComponent<scene::MeshRenderer>(), nullptr);
    EXPECT_EQ(quad->GetComponent<scene::MeshRenderer>()->meshPath, "primitive:quad");
    EXPECT_NE(quad->GetComponent<scene::BoxColliderComponent>(), nullptr);
    EXPECT_EQ(quad->GetComponent<scene::ScriptComponent>(), nullptr);
    EXPECT_EQ(quad->GetComponent<scene::BillboardComponent>(), nullptr);
    const auto* billboardPreset = editor::FindObjectPreset("rendering.billboard");
    ASSERT_NE(billboardPreset, nullptr);
    auto* billboard = billboardPreset->create(Context());
    ASSERT_NE(billboard, nullptr);
    EXPECT_NE(billboard->GetComponent<scene::BillboardComponent>(), nullptr);
}

TEST_F(ObjectPresetMenuTest, CreatesUsefulSpatialAmbientAndMusicAudioDefaultsWithoutGuessingAnAsset)
{
    AttachScene();
    const auto createAudio = [this](const char* id) {
        const auto* preset = editor::FindObjectPreset(id);
        if (!preset) return static_cast<scene::AudioSourceComponent*>(nullptr);
        auto* go = preset->create(Context());
        return go ? go->GetComponent<scene::AudioSourceComponent>() : nullptr;
    };
    auto* spatial = createAudio("audio.spatialSource");
    ASSERT_NE(spatial, nullptr);
    EXPECT_FLOAT_EQ(spatial->spatialBlend, 1.0f);
    EXPECT_FALSE(spatial->loop);
    EXPECT_FALSE(spatial->playOnAwake);
    EXPECT_TRUE(spatial->clipPath.empty());
    auto* ambient = createAudio("audio.ambient");
    ASSERT_NE(ambient, nullptr);
    EXPECT_FLOAT_EQ(ambient->spatialBlend, 1.0f);
    EXPECT_TRUE(ambient->loop);
    EXPECT_TRUE(ambient->playOnAwake);
    EXPECT_FALSE(ambient->streaming);
    EXPECT_TRUE(ambient->clipPath.empty());
    auto* music = createAudio("audio.music");
    ASSERT_NE(music, nullptr);
    EXPECT_FLOAT_EQ(music->spatialBlend, 0.0f);
    EXPECT_TRUE(music->loop);
    EXPECT_TRUE(music->playOnAwake);
    EXPECT_TRUE(music->streaming);
    EXPECT_EQ(music->busName, "BGM");
    EXPECT_TRUE(music->clipPath.empty());
}

TEST_F(ObjectPresetMenuTest, CreatesAtTheSelectedParentsLocalOriginAndRestoresSelectionAndGuidThroughUndoRedo)
{
    auto& target = AttachScene();
    auto& parent = target.CreateGameObject("Parent");
    parent.transform.position = {12.0f, 8.0f, -3.0f};
    parent.transform.scale = {2.0f, 3.0f, 4.0f};
    auto& existing = target.CreateGameObject("Camera");
    existing.SetParent(parent);
    Context().selectedEntities = {parent.GetID()};
    editor::CreateObjectRequest request;
    request.key = "camera";
    request.parentGuid = parent.instanceId;
    request.placeInView = true;
    auto command = editor::MakeCreateObjectCommand(Context(), request, "Create Camera", true);
    ASSERT_NE(command, nullptr);
    ASSERT_EQ(Context().selectedEntities.size(), 1u);
    auto* created = target.GetGameObject(Context().selectedEntities.front());
    ASSERT_NE(created, nullptr);
    EXPECT_EQ(created->name, "Camera (1)");
    EXPECT_EQ(created->GetParent(), &parent);
    EXPECT_VEC3_NEAR(created->transform.position, math::Vector3{}, 1e-6f);
    EXPECT_VEC3_NEAR(created->transform.scale, math::Vector3(1.0f, 1.0f, 1.0f), 1e-6f);
    EXPECT_NE(created->GetComponent<scene::CameraComponent>(), nullptr);
    EXPECT_NE(created->GetComponent<scene::AudioListenerComponent>(), nullptr);
    const std::string guid = created->instanceId;
    command->Undo();
    EXPECT_EQ(target.FindByGuid(guid), nullptr);
    ASSERT_EQ(Context().selectedEntities.size(), 1u);
    EXPECT_EQ(Context().selectedEntities.front(), parent.GetID());
    command->Execute();
    created = target.FindByGuid(guid);
    ASSERT_NE(created, nullptr);
    EXPECT_EQ(created->name, "Camera (1)");
    EXPECT_EQ(created->GetParent(), &parent);
    EXPECT_VEC3_NEAR(created->transform.position, math::Vector3{}, 1e-6f);
    ASSERT_EQ(Context().selectedEntities.size(), 1u);
    EXPECT_EQ(Context().selectedEntities.front(), created->GetID());
}


TEST_F(ObjectPresetMenuTest, PrimitiveCreationAndDefaultsUseTheirExplicitResourceContext)
{
    auto& target = AttachScene();
    testkit::ResourceTrackingRenderer device, otherDevice;
    renderer::ResourceManager resources(device), other(otherDevice);
    Context().resources = &resources; target.SetResourceManager(&resources);
    const auto* preset = editor::FindObjectPreset("3d.cube"); ASSERT_NE(preset, nullptr);
    auto* object = preset->create(Context()); ASSERT_NE(object, nullptr);
    const auto* rendered = object->GetComponent<scene::MeshRenderer>();
    ASSERT_NE(rendered, nullptr); ASSERT_NE(rendered->mesh, nullptr);
    EXPECT_EQ(rendered->mesh->vertexBuffer.owner, resources.GetInstanceId());
    auto& defaultsOwner = target.CreateGameObject("Default mesh");
    const auto defaults = editor::MakeDefaultComponent<scene::MeshRenderer>(defaultsOwner);
    ASSERT_NE(defaults.mesh, nullptr);
    EXPECT_EQ(defaults.mesh->vertexBuffer.owner, resources.GetInstanceId());
    EXPECT_EQ(otherDevice.m_buffersAlive, 0u);
    Context().resources = nullptr; target.Clear();
}

TEST_F(ObjectPresetMenuTest, SceneReloadResolvesMeshesThroughTheScenesBoundManager)
{
    auto& target = AttachScene();
    testkit::ResourceTrackingRenderer device, otherDevice;
    renderer::ResourceManager resources(device), other(otherDevice);
    target.SetResourceManager(&resources);
    auto& object = target.CreateGameObject("Reloaded cube");
    object.AddComponent<scene::MeshRenderer>().meshPath = "primitive:cube";
    const auto path = File("resources.scene").generic_string();
    ASSERT_TRUE(editor::SceneIO::Save(target, path));
    ASSERT_TRUE(editor::SceneIO::Load(target, path));
    auto* loaded = target.Find("Reloaded cube"); ASSERT_NE(loaded, nullptr);
    const auto* rendered = loaded->GetComponent<scene::MeshRenderer>();
    ASSERT_NE(rendered, nullptr); ASSERT_NE(rendered->mesh, nullptr);
    EXPECT_EQ(rendered->mesh->vertexBuffer.owner, resources.GetInstanceId());
    EXPECT_EQ(target.GetResourceManager(), &resources);
    EXPECT_EQ(otherDevice.m_buffersAlive, 0u);
    target.Clear();
}

TEST_F(ObjectPresetMenuTest, HeadlessSceneLoadDoesNotBorrowAnUnrelatedManagersResources)
{
    auto& target = AttachScene();
    testkit::ResourceTrackingRenderer otherDevice;
    renderer::ResourceManager other(otherDevice);
    auto& object = target.CreateGameObject("Data only");
    object.AddComponent<scene::MeshRenderer>().meshPath = "primitive:cube";
    const auto path = File("headless.scene").generic_string();
    ASSERT_TRUE(editor::SceneIO::Save(target, path));
    ASSERT_TRUE(editor::SceneIO::Load(target, path));
    ASSERT_NE(target.Find("Data only"), nullptr);
    EXPECT_EQ(target.Find("Data only")->GetComponent<scene::MeshRenderer>()->mesh, nullptr);
    EXPECT_EQ(target.GetResourceManager(), nullptr);
    EXPECT_EQ(otherDevice.m_buffersAlive, 0u);
}

} /// @note namespace fbzz::tests
