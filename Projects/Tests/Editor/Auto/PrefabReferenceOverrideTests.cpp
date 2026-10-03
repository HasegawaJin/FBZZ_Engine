/// @file    PrefabReferenceOverrideTests.cpp
/// @brief   内部参照の名前とアセット参照の表示形式を個別変更と誤認せず、参照先変更は保持する。
/// @author  Hasegawa Jin
/// @date    2026-10-02
#include <TestKit/TestKit.hpp>
#include <TestKit/Editor/EditorFixture.hpp>
#include <Editor/Util/PrefabOverrides.hpp>
#include <Editor/Util/PrefabSerializer.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Asset/GuidRefCodec.hpp>
#include <Engine/Scene/Components/BoneComponent.hpp>
#include <Engine/Scene/Components/IKSolverComponent.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/SceneSerializer.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <memory>
#include <sstream>

namespace fbzz::tests {

class PrefabReferenceOverrideTest : public testkit::EditorFixture {
protected:
    void SetUp() override
    {
        EditorFixture::SetUp();
        asset::AssetDatabase::Init(File("Assets").generic_string());
    }

    void TearDown() override
    {
        asset::AssetDatabase::Shutdown();
        EditorFixture::TearDown();
    }

    std::string PrefabPath() const { return File("Assets/Rig.prefab").generic_string(); }

    std::string CreateRegisteredModel(const std::string& name)
    {
        const std::string path = File("Assets/" + name).generic_string();
        if (!util::FileSystem::WriteText(path, "CPU comparison fixture")) return {};
        if (asset::AssetDatabase::GuidFromPath(path).empty()) return {};
        return path;
    }

    std::unique_ptr<scene::Scene> CreateMeshScene(const std::string& currentReference,
                                                 const std::string& baselineReference)
    {
        scene::Scene source;
        auto& mesh = source.CreateGameObject("Mesh");
        mesh.AddComponent<scene::MeshRenderer>().meshPath = currentReference;
        std::vector<scene::EntityID> roots;
        if (!editor::PrefabSerializer::SaveSelectionAndConnect(source, {mesh.GetID()}, PrefabPath(), roots))
            return {};
        auto restored = scene::SceneSerializer::LoadFromText(
            scene::SceneSerializer::SaveToText(source), nullptr, PrefabPath());
        if (!restored) return {};
        auto* root = restored->GetRootGameObjects().front();
        /// @note 比較対象の形式を明示し、別テストの AssetManager ベースパスを引き継がない。
        root->GetComponent<scene::MeshRenderer>()->meshPath = currentReference;
        auto baseline = toml::parse(root->prefabSourceSnapshot);
        if (!baseline) return {};
        auto* objects = baseline.table()["gameobjects"].as_array();
        if (!objects || objects->empty()) return {};
        auto* object = objects->front().as_table();
        auto* renderer = object ? (*object)["MeshRenderer"].as_table() : nullptr;
        if (!renderer) return {};
        renderer->insert_or_assign("mesh", baselineReference);
        std::ostringstream stream;
        stream << baseline.table();
        root->prefabSourceSnapshot = stream.str();
        return restored;
    }

    bool HasMeshOverride(const editor::PrefabOverrideSet& overrides) const
    {
        for (const auto& entry : overrides.entries)
            if (entry.path == "MeshRenderer.mesh") return true;
        return false;
    }

    void CreateAsset(scene::Scene& source)
    {
        auto& rig = source.CreateGameObject("Rig");
        auto& target = source.CreateGameObject("Target");
        target.SetParent(rig);
        auto& pole = source.CreateGameObject("Pole");
        pole.SetParent(rig);
        auto& ik = rig.AddComponent<scene::IKSolverComponent>();
        ik.chains.emplace_back();
        ik.chains.front().targetEntity = target.GetID();
        ik.chains.front().poleEntity = pole.GetID();
        target.AddComponent<scene::BoneComponent>().skinnedMeshEntity = rig.GetID();
        rig.AddComponent<scene::SkinnedMeshRenderer>().skeletonRootEntity = rig.GetID();
        ASSERT_TRUE(editor::PrefabSerializer::SaveSelection(source, {rig.GetID()}, PrefabPath()));
    }
};

TEST_F(PrefabReferenceOverrideTest, UniqueNamesDoNotCreateInternalReferenceOverrides)
{
    scene::Scene source;
    CreateAsset(source);
    scene::Scene destination;
    destination.CreateGameObject("Rig");
    destination.CreateGameObject("Target");
    destination.CreateGameObject("Pole");
    std::vector<scene::EntityID> roots;
    ASSERT_TRUE(editor::PrefabSerializer::Instantiate(destination, PrefabPath(), roots));
    editor::PrefabOverrideSet overrides;

    ASSERT_TRUE(editor::ComputePrefabOverrides(destination, roots.front(),
                                               ProjectRoot().generic_string(), overrides));

    for (const auto& entry : overrides.entries) {
        EXPECT_NE(entry.path, "IKSolverComponent.chains");
        EXPECT_NE(entry.path, "BoneComponent.skinnedMeshOwner");
        EXPECT_NE(entry.path, "SkinnedMeshRenderer.skeletonRootName");
    }
    EXPECT_FALSE(overrides.hasStructuralOverrides);
}

TEST_F(PrefabReferenceOverrideTest, ActualReferenceChangeIsStillAnOverride)
{
    scene::Scene source;
    CreateAsset(source);
    scene::Scene destination;
    auto& external = destination.CreateGameObject("OtherTarget");
    std::vector<scene::EntityID> roots;
    ASSERT_TRUE(editor::PrefabSerializer::Instantiate(destination, PrefabPath(), roots));
    auto* rig = destination.GetGameObject(roots.front());
    auto* ik = rig->GetComponent<scene::IKSolverComponent>();
    ASSERT_NE(ik, nullptr);
    ASSERT_EQ(ik->chains.size(), 1u);
    ik->chains.front().targetEntity = external.GetID();
    editor::PrefabOverrideSet overrides;
    ASSERT_TRUE(editor::ComputePrefabOverrides(destination, rig->GetID(),
                                               ProjectRoot().generic_string(), overrides));

    bool changed = false;
    for (const auto& entry : overrides.entries)
        if (entry.path == "IKSolverComponent.chains") changed = true;
    EXPECT_TRUE(changed);
}

TEST_F(PrefabReferenceOverrideTest, LegacyAbsoluteModelPathAndGuidReferenceAreTheSameAsset)
{
    const std::string model = CreateRegisteredModel("Model.fbx");
    ASSERT_FALSE(model.empty());
    const std::string reference = asset::EncodeGuidRef(model);
    ASSERT_TRUE(asset::AssetDatabase::IsGuidRef(reference));
    auto scene = CreateMeshScene(reference, model);
    ASSERT_NE(scene, nullptr);
    auto* root = scene->GetRootGameObjects().front();
    EXPECT_EQ(root->GetComponent<scene::MeshRenderer>()->mesh, nullptr);
    editor::PrefabOverrideSet overrides;

    ASSERT_TRUE(editor::ComputePrefabOverrides(*scene, root->GetID(),
                                               ProjectRoot().generic_string(), overrides));

    EXPECT_FALSE(HasMeshOverride(overrides));
    EXPECT_FALSE(overrides.hasStructuralOverrides);
}

TEST_F(PrefabReferenceOverrideTest, RenamedAssetHintDoesNotCreateAnOverrideForTheSameGuid)
{
    const std::string oldPath = CreateRegisteredModel("OldModel.fbx");
    ASSERT_FALSE(oldPath.empty());
    const std::string oldReference = asset::EncodeGuidRef(oldPath);
    ASSERT_TRUE(asset::AssetDatabase::IsGuidRef(oldReference));
    const std::string newPath = File("Assets/RenamedModel.fbx").generic_string();
    ASSERT_TRUE(util::FileSystem::Rename(oldPath, newPath));
    ASSERT_TRUE(util::FileSystem::Rename(oldPath + ".meta", newPath + ".meta"));
    asset::AssetDatabase::OnAssetMoved(oldPath, newPath);
    const std::string newReference = asset::EncodeGuidRef(newPath);
    ASSERT_NE(oldReference, newReference);
    ASSERT_EQ(asset::AssetDatabase::GuidFromRef(oldReference), asset::AssetDatabase::GuidFromRef(newReference));
    auto scene = CreateMeshScene(newReference, oldReference);
    ASSERT_NE(scene, nullptr);
    auto* root = scene->GetRootGameObjects().front();
    editor::PrefabOverrideSet overrides;

    ASSERT_TRUE(editor::ComputePrefabOverrides(*scene, root->GetID(),
                                               ProjectRoot().generic_string(), overrides));

    EXPECT_FALSE(HasMeshOverride(overrides));
    EXPECT_FALSE(overrides.hasStructuralOverrides);
}

TEST_F(PrefabReferenceOverrideTest, DifferentSubmeshSuffixIsAnOverrideAndKeepsTheOriginalPatchValue)
{
    const std::string model = CreateRegisteredModel("Submeshes.fbx");
    ASSERT_FALSE(model.empty());
    const std::string baseline = asset::EncodeGuidRef(model + ":0");
    const std::string current = asset::EncodeGuidRef(model + ":1");
    ASSERT_TRUE(asset::AssetDatabase::IsGuidRef(current));
    ASSERT_EQ(asset::AssetDatabase::GuidFromRef(baseline), asset::AssetDatabase::GuidFromRef(current));
    auto scene = CreateMeshScene(current, baseline);
    ASSERT_NE(scene, nullptr);
    auto* root = scene->GetRootGameObjects().front();
    editor::PrefabOverrideSet overrides;

    ASSERT_TRUE(editor::ComputePrefabOverrides(*scene, root->GetID(),
                                               ProjectRoot().generic_string(), overrides));

    EXPECT_TRUE(HasMeshOverride(overrides));
    const auto snapshot = overrides.instanceTables.find(root->prefabSourceId);
    ASSERT_NE(snapshot, overrides.instanceTables.end());
    EXPECT_EQ(snapshot->second["MeshRenderer"]["mesh"].value_or(std::string{}), current);
    EXPECT_FALSE(overrides.hasStructuralOverrides);
}

} /// @note namespace fbzz::tests
