/// @file    PrefabSaveTests.cpp
/// @brief   選択範囲を .prefab として切り出す処理。
/// @author  Hasegawa Jin
/// @date    2026-09-10
/// @note 切り出しは «どこまでを含めるか» と «アセット側の id を安定させるか» の 2 点で事故る。
/// @note 含める範囲を間違えると子だけのプレハブができ、id が毎回変わると既存インスタンスの
/// @note prefabSourceId が行き先を失って override の対応が切れる。
/// @note 保存内容と、GPU 無しでの生成・伝播・差し替えを検証する。
#include <TestKit/TestKit.hpp>
#include <TestKit/Editor/EditorFixture.hpp>

#include <Editor/Util/PrefabSerializer.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/SceneSerializer.hpp>
#include <Engine/Scene/Components/LightComponent.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/Components/ConstraintComponents.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Util/FileSystem.hpp>

#include <toml++/toml.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace fbzz::tests {
namespace {

using editor::PrefabSerializer;

std::string ReadFile(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

/// @note 書き出した .prefab の gameobjects 配列から、名前の一覧を取り出す。
std::vector<std::string> ObjectNames(const std::string& text)
{
    std::vector<std::string> names;
    const auto parsed = toml::parse(text);
    if (!parsed) return names;
    const toml::array* objects = parsed.table()["gameobjects"].as_array();
    if (!objects) return names;
    for (const auto& item : *objects) {
        if (const toml::table* object = item.as_table())
            names.push_back((*object)["name"].value_or(std::string{}));
    }
    return names;
}

bool ContainsName(const std::vector<std::string>& names, const std::string& name)
{
    for (const std::string& candidate : names)
        if (candidate == name) return true;
    return false;
}

} /// @note namespace

/// @note SaveSelection は SceneIO::Serialize でシーンを直列化する。その一時ファイルの
/// @note 置き場所が要るため EditorFixture に載せる (無いと空文字が返り、保存が失敗する)。
class PrefabSaveTest : public testkit::EditorFixture {
protected:
    void TearDown() override
    {
        asset::AssetDatabase::Shutdown();
        testkit::EditorFixture::TearDown();
    }

    std::filesystem::path PrefabPath(const std::string& name = "Test.prefab") const
    {
        return File(name);
    }
    std::string PrefabPathUtf8(const std::string& name = "Test.prefab") const
    {
        return PrefabPath(name).generic_string();
    }

    std::string SavedText(const std::string& name = "Test.prefab") const
    {
        return ReadFile(PrefabPath(name));
    }
};

TEST_F(PrefabSaveTest, RefusesAnEmptySelection)
{
    scene::Scene scene;

    EXPECT_FALSE(PrefabSerializer::SaveSelection(scene, {}, PrefabPathUtf8()));
    EXPECT_FALSE(std::filesystem::exists(PrefabPath()));
}

TEST_F(PrefabSaveTest, RefusesASelectionOfUnknownEntities)
{
    scene::Scene scene;
    const std::vector<scene::EntityID> bogus{ scene::EntityID::INVALID };

    EXPECT_FALSE(PrefabSerializer::SaveSelection(scene, bogus, PrefabPathUtf8()));
}

TEST_F(PrefabSaveTest, WritesAFileForASingleObject)
{
    scene::Scene scene;
    scene::GameObject& player = scene.CreateGameObject("Player");

    ASSERT_TRUE(PrefabSerializer::SaveSelection(scene, { player.GetID() }, PrefabPathUtf8()));

    ASSERT_TRUE(std::filesystem::exists(PrefabPath()));
    EXPECT_TRUE(ContainsName(ObjectNames(SavedText()), "Player"));
}

TEST_F(PrefabSaveTest, RecordsThePrefabFormatHeader)
{
    scene::Scene scene;
    scene::GameObject& player = scene.CreateGameObject("Player");
    ASSERT_TRUE(PrefabSerializer::SaveSelection(scene, { player.GetID() }, PrefabPathUtf8()));

    const auto parsed = toml::parse(SavedText());
    ASSERT_TRUE(parsed);
    EXPECT_EQ(parsed.table()["prefab"]["format_version"].value_or(0), 1);
    EXPECT_EQ(parsed.table()["prefab"]["root_count"].value_or(0), 1);
}

TEST_F(PrefabSaveTest, IncludesDescendantsOfTheSelectedRoot)
{
    /// @note 親だけ選んでも、子はプレハブの一部として付いてこなければならない。
    scene::Scene scene;
    scene::GameObject& parent = scene.CreateGameObject("Parent");
    scene::GameObject& child  = scene.CreateGameObject("Child");
    child.SetParent(parent);

    ASSERT_TRUE(PrefabSerializer::SaveSelection(scene, { parent.GetID() }, PrefabPathUtf8()));

    const auto names = ObjectNames(SavedText());
    EXPECT_TRUE(ContainsName(names, "Parent"));
    EXPECT_TRUE(ContainsName(names, "Child"));
}

TEST_F(PrefabSaveTest, ExcludesObjectsOutsideTheSelection)
{
    scene::Scene scene;
    scene::GameObject& player = scene.CreateGameObject("Player");
    scene.CreateGameObject("Unrelated");

    ASSERT_TRUE(PrefabSerializer::SaveSelection(scene, { player.GetID() }, PrefabPathUtf8()));

    EXPECT_FALSE(ContainsName(ObjectNames(SavedText()), "Unrelated"));
}

TEST_F(PrefabSaveTest, CountsOnlyTopLevelRootsWhenAWholeBranchIsSelected)
{
    /// @note 親と子を «両方» 選んでも、プレハブの根は親 1 つ。
    /// @note 子まで根に数えると、開いたときに同じ枝が二重に生える。
    scene::Scene scene;
    scene::GameObject& parent = scene.CreateGameObject("Parent");
    scene::GameObject& child  = scene.CreateGameObject("Child");
    child.SetParent(parent);

    ASSERT_TRUE(PrefabSerializer::SaveSelection(
        scene, { parent.GetID(), child.GetID() }, PrefabPathUtf8()));

    const auto parsed = toml::parse(SavedText());
    ASSERT_TRUE(parsed);
    EXPECT_EQ(parsed.table()["prefab"]["root_count"].value_or(0), 1);
}

TEST_F(PrefabSaveTest, CountsSeveralIndependentRoots)
{
    scene::Scene scene;
    scene::GameObject& a = scene.CreateGameObject("A");
    scene::GameObject& b = scene.CreateGameObject("B");

    ASSERT_TRUE(PrefabSerializer::SaveSelection(
        scene, { a.GetID(), b.GetID() }, PrefabPathUtf8()));

    const auto parsed = toml::parse(SavedText());
    ASSERT_TRUE(parsed);
    EXPECT_EQ(parsed.table()["prefab"]["root_count"].value_or(0), 2);
}

TEST_F(PrefabSaveTest, SavingOnlyAChildMakesThatChildTheRoot)
{
    /// @note 子だけを選んだら、その子が根のプレハブになる (親は含まれない)。
    scene::Scene scene;
    scene::GameObject& parent = scene.CreateGameObject("Parent");
    scene::GameObject& child  = scene.CreateGameObject("Child");
    child.SetParent(parent);

    ASSERT_TRUE(PrefabSerializer::SaveSelection(scene, { child.GetID() }, PrefabPathUtf8()));

    const auto names = ObjectNames(SavedText());
    EXPECT_TRUE(ContainsName(names, "Child"));
    EXPECT_FALSE(ContainsName(names, "Parent"));
}

TEST_F(PrefabSaveTest, KeepsTheAssetIdOfAnAlreadyConnectedObject)
{
    /// @note Apply を繰り返してもアセット側の id が入れ替わらないこと。ここが動くと、
    /// @note 他インスタンスの prefabSourceId が行き先を失い override の対応が切れる。
    scene::Scene scene;
    scene::GameObject& player = scene.CreateGameObject("Player");
    player.prefabSourceId = "stable-asset-id";

    ASSERT_TRUE(PrefabSerializer::SaveSelection(scene, { player.GetID() }, PrefabPathUtf8()));

    const auto parsed = toml::parse(SavedText());
    ASSERT_TRUE(parsed);
    const toml::array* objects = parsed.table()["gameobjects"].as_array();
    ASSERT_NE(objects, nullptr);
    ASSERT_FALSE(objects->empty());
    const toml::table* first = objects->front().as_table();
    ASSERT_NE(first, nullptr);
    EXPECT_EQ((*first)["instanceId"].value_or(std::string{}), "stable-asset-id");
}

TEST_F(PrefabSaveTest, DoesNotReuseTheSameAssetIdTwice)
{
    /// @note インスタンス内で子を複製すると prefabSourceId が重複する。
    /// @note 両方に同じ id を振ると、アセット側で 1 つに潰れて片方が消える。
    scene::Scene scene;
    scene::GameObject& first  = scene.CreateGameObject("First");
    scene::GameObject& second = scene.CreateGameObject("Second");
    first.prefabSourceId  = "shared-id";
    second.prefabSourceId = "shared-id";

    ASSERT_TRUE(PrefabSerializer::SaveSelection(
        scene, { first.GetID(), second.GetID() }, PrefabPathUtf8()));

    const auto parsed = toml::parse(SavedText());
    ASSERT_TRUE(parsed);
    const toml::array* objects = parsed.table()["gameobjects"].as_array();
    ASSERT_NE(objects, nullptr);
    ASSERT_EQ(objects->size(), 2u);

    const std::string idA = (*objects->front().as_table())["instanceId"].value_or(std::string{});
    const std::string idB = (*objects->back().as_table())["instanceId"].value_or(std::string{});
    EXPECT_FALSE(idA.empty());
    EXPECT_FALSE(idB.empty());
    EXPECT_NE(idA, idB);
}

TEST_F(PrefabSaveTest, ProducesTheSameFileForTheSameScene)
{
    /// @note 保存し直すたびに中身が揺れると、触っていないプレハブが git の差分に出る。
    scene::Scene scene;
    scene::GameObject& parent = scene.CreateGameObject("Parent");
    scene::GameObject& child  = scene.CreateGameObject("Child");
    child.SetParent(parent);

    ASSERT_TRUE(PrefabSerializer::SaveSelection(scene, { parent.GetID() },
                                                PrefabPathUtf8("A.prefab")));
    ASSERT_TRUE(PrefabSerializer::SaveSelection(scene, { parent.GetID() },
                                                PrefabPathUtf8("B.prefab")));

    EXPECT_EQ(SavedText("A.prefab"), SavedText("B.prefab"));
}

TEST_F(PrefabSaveTest, KeepsTheTransformOfTheSavedObject)
{
    scene::Scene scene;
    scene::GameObject& player = scene.CreateGameObject("Player");
    player.transform.position = { 1.5f, -2.5f, 3.25f };

    ASSERT_TRUE(PrefabSerializer::SaveSelection(scene, { player.GetID() }, PrefabPathUtf8()));

    /// @note 値そのものはシーンの直列化と同じ経路なので、ここでは «落ちていない» ことだけ見る。
    const std::string text = SavedText();
    EXPECT_NE(text.find("1.5"), std::string::npos) << text;
}

TEST_F(PrefabSaveTest, ApplyUpdatesUntouchedValuesAndKeepsIndividualOverrides)
{
    scene::Scene scene;
    auto& source = scene.CreateGameObject("Lamp");
    auto& light = source.AddComponent<scene::LightComponent>();
    light.intensity = 1.0f;
    light.range = 10.0f;
    std::vector<scene::EntityID> connected;
    ASSERT_TRUE(PrefabSerializer::SaveSelectionAndConnect(scene, {source.GetID()}, PrefabPathUtf8(), connected));
    std::vector<scene::EntityID> roots;
    ASSERT_TRUE(PrefabSerializer::Instantiate(scene, PrefabPathUtf8(), roots));
    const std::string untouchedGuid = scene.GetGameObject(roots.front())->instanceId;
    ASSERT_TRUE(PrefabSerializer::Instantiate(scene, PrefabPathUtf8(), roots));
    const std::string adjustedGuid = scene.GetGameObject(roots.front())->instanceId;
    scene.GetGameObject(roots.front())->GetComponent<scene::LightComponent>()->intensity = 4.0f;
    source.GetComponent<scene::LightComponent>()->intensity = 2.0f;
    source.GetComponent<scene::LightComponent>()->range = 20.0f;

    EXPECT_EQ(PrefabSerializer::ApplyAndPropagate(scene, source.GetID(), ProjectRoot().generic_string()), 2);

    const auto* untouched = scene.FindByGuid(untouchedGuid)->GetComponent<scene::LightComponent>();
    const auto* adjusted = scene.FindByGuid(adjustedGuid)->GetComponent<scene::LightComponent>();
    EXPECT_FLOAT_EQ(untouched->intensity, 2.0f);
    EXPECT_FLOAT_EQ(adjusted->intensity, 4.0f);
    EXPECT_FLOAT_EQ(untouched->range, 20.0f);
    EXPECT_FLOAT_EQ(adjusted->range, 20.0f);
}

TEST_F(PrefabSaveTest, DiskPropagationUsesTheSavedBaselineAfterSceneReload)
{
    scene::Scene scene;
    auto& root = scene.CreateGameObject("Lamp");
    root.AddComponent<scene::LightComponent>().intensity = 1.0f;
    std::vector<scene::EntityID> connected;
    ASSERT_TRUE(PrefabSerializer::SaveSelectionAndConnect(scene, {root.GetID()}, PrefabPathUtf8(), connected));
    auto restored = scene::SceneSerializer::LoadDataFromText(scene::SceneSerializer::SaveToText(scene), "test.scene");
    ASSERT_NE(restored, nullptr);
    root.GetComponent<scene::LightComponent>()->intensity = 2.0f;
    ASSERT_TRUE(PrefabSerializer::SaveSelection(scene, {root.GetID()}, PrefabPathUtf8()));

    EXPECT_EQ(PrefabSerializer::PropagateToInstances(*restored, PrefabPathUtf8(), {}, ProjectRoot().generic_string()), 1);
    EXPECT_FLOAT_EQ(restored->GetRootGameObjects().front()->GetComponent<scene::LightComponent>()->intensity, 2.0f);
}

TEST_F(PrefabSaveTest, RevertMissingFileLeavesHierarchyAndReferencesUnchanged)
{
    scene::Scene scene;
    auto& root = scene.CreateGameObject("Root");
    auto& child = scene.CreateGameObject("Child");
    child.SetParent(root);
    auto& follower = scene.CreateGameObject("Follower");
    follower.AddComponent<scene::SocketAttachmentComponent>().target.id = root.GetID();
    std::vector<scene::EntityID> connected;
    ASSERT_TRUE(PrefabSerializer::SaveSelectionAndConnect(scene, {root.GetID()}, PrefabPathUtf8(), connected));
    const auto rootId = root.GetID();
    const auto childId = child.GetID();
    const auto* rootPointer = &root;
    std::error_code error;
    ASSERT_TRUE(std::filesystem::remove(PrefabPath(), error));
    ASSERT_FALSE(error);
    std::vector<scene::EntityID> replaced;

    EXPECT_FALSE(PrefabSerializer::Revert(scene, rootId, replaced, ProjectRoot().generic_string()));

    EXPECT_EQ(scene.GetGameObject(rootId), rootPointer);
    EXPECT_NE(scene.GetGameObject(childId), nullptr);
    EXPECT_EQ(follower.GetComponent<scene::SocketAttachmentComponent>()->target.Resolve(scene), rootPointer);
    EXPECT_EQ(scene.GameObjectCount(), 3u);
    EXPECT_TRUE(replaced.empty());
}

TEST_F(PrefabSaveTest, RevertRejectsCyclicHierarchyBeforeTouchingExistingObjects)
{
    scene::Scene scene;
    auto& root = scene.CreateGameObject("Existing");
    const auto id = root.GetID();
    root.prefabAssetPath = PrefabPathUtf8();
    ASSERT_TRUE(util::FileSystem::WriteText(PrefabPathUtf8(),
        "[[gameobjects]]\ninstanceId='a'\nparentInstanceId='b'\nparent='B'\nname='A'\n"
        "[[gameobjects]]\ninstanceId='b'\nparentInstanceId='a'\nparent='A'\nname='B'\n"));
    std::vector<scene::EntityID> replaced;

    EXPECT_FALSE(PrefabSerializer::Revert(scene, id, replaced, ProjectRoot().generic_string()));
    EXPECT_EQ(scene.GetGameObject(id), &root);
    EXPECT_EQ(scene.GameObjectCount(), 1u);
}

TEST_F(PrefabSaveTest, RevertMovesExternalReferenceToTheReplacementEntity)
{
    scene::Scene source;
    auto& assetRoot = source.CreateGameObject("Character");
    auto& assetChild = source.CreateGameObject("Socket");
    assetChild.SetParent(assetRoot);
    assetChild.transform.position = {2.0f, 0.0f, 0.0f};
    ASSERT_TRUE(PrefabSerializer::SaveSelection(source, {assetRoot.GetID()}, PrefabPathUtf8()));
    scene::Scene scene;
    auto& unrelated = scene.CreateGameObject("Unrelated");
    scene.CreateGameObject("Other");
    std::vector<scene::EntityID> roots;
    ASSERT_TRUE(PrefabSerializer::Instantiate(scene, PrefabPathUtf8(), roots));
    const auto oldId = roots.front();
    unrelated.transform.position = {20.0f, 0.0f, 0.0f};
    scene.GetGameObject(oldId)->SetParent(unrelated);
    scene.GetGameObject(oldId)->transform.position = {10.0f, 0.0f, 0.0f};
    const std::string oldGuid = scene.GetGameObject(oldId)->instanceId;
    auto& follower = scene.CreateGameObject("Follower");
    follower.AddComponent<scene::SocketAttachmentComponent>().target.id = oldId;
    unrelated.AddComponent<scene::SocketAttachmentComponent>().target.id = follower.GetID();
    std::vector<scene::EntityID> replaced;

    ASSERT_TRUE(PrefabSerializer::Revert(scene, oldId, replaced, ProjectRoot().generic_string()));

    EXPECT_FALSE(scene.IsValid(oldId));
    EXPECT_EQ(scene.FindByGuid(oldGuid), scene.GetGameObject(replaced.front()));
    EXPECT_EQ(follower.GetComponent<scene::SocketAttachmentComponent>()->target.Resolve(scene), scene.GetGameObject(replaced.front()));
    EXPECT_EQ(unrelated.GetComponent<scene::SocketAttachmentComponent>()->target.Resolve(scene), &follower);
    const auto* replacement = scene.GetGameObject(replaced.front());
    EXPECT_FLOAT_EQ(replacement->transform.worldPosition.x, 30.0f);
    ASSERT_EQ(replacement->GetChildCount(), 1);
    EXPECT_FLOAT_EQ(replacement->GetChild(0)->transform.worldPosition.x, 32.0f);
}

TEST_F(PrefabSaveTest, RenamingAPrefabKeepsItsGuidSourceConnection)
{
    const auto oldPath = File("Assets/Lamp.prefab");
    const auto newPath = File("Assets/Renamed.prefab");
    asset::AssetDatabase::Init(File("Assets").generic_string());
    scene::Scene scene;
    auto& root = scene.CreateGameObject("Lamp");
    std::vector<scene::EntityID> connected;
    ASSERT_TRUE(PrefabSerializer::SaveSelectionAndConnect(scene, {root.GetID()}, oldPath.generic_string(), connected));
    EXPECT_TRUE(asset::AssetDatabase::IsGuidRef(root.prefabAssetPath));
    const std::string key = scene::PrefabAssetKey(root.prefabAssetPath);
    ASSERT_TRUE(util::FileSystem::Rename(oldPath, newPath));
    ASSERT_TRUE(util::FileSystem::Rename(oldPath.generic_string() + ".meta", newPath.generic_string() + ".meta"));
    asset::AssetDatabase::OnAssetMoved(oldPath.generic_string(), newPath.generic_string());
    std::vector<scene::EntityID> replaced;

    ASSERT_TRUE(PrefabSerializer::Revert(scene, root.GetID(), replaced, ProjectRoot().generic_string()));
    EXPECT_EQ(scene::PrefabAssetKey(scene.GetGameObject(replaced.front())->prefabAssetPath), key);
    EXPECT_EQ(PrefabSerializer::CountInstances(scene, newPath.generic_string()), 1);
}

TEST_F(PrefabSaveTest, ApplyingAConnectedPrefabSavesInternalReferencesWithAssetIds)
{
    scene::Scene scene;
    auto& root = scene.CreateGameObject("Root");
    auto& child = scene.CreateGameObject("Child");
    child.SetParent(root);
    child.AddComponent<scene::SocketAttachmentComponent>().target.id = root.GetID();
    root.prefabSourceId = "root-asset-id";
    child.prefabSourceId = "child-asset-id";
    ASSERT_TRUE(PrefabSerializer::SaveSelection(scene, {root.GetID()}, PrefabPathUtf8()));

    const auto parsed = toml::parse(SavedText());
    ASSERT_TRUE(parsed);
    const auto* objects = parsed.table()["gameobjects"].as_array();
    ASSERT_NE(objects, nullptr);
    ASSERT_EQ(objects->size(), 2u);
    const auto* childTable = objects->back().as_table();
    ASSERT_NE(childTable, nullptr);
    EXPECT_EQ((*childTable)["SocketAttachmentComponent"]["target"].value_or(std::string{}), "root-asset-id");
}

TEST_F(PrefabSaveTest, AutomaticPropagationKeepsLocallyAddedChildren)
{
    scene::Scene source;
    auto& sourceRoot = source.CreateGameObject("Root");
    sourceRoot.AddComponent<scene::LightComponent>().intensity = 1.0f;
    ASSERT_TRUE(PrefabSerializer::SaveSelection(source, {sourceRoot.GetID()}, PrefabPathUtf8()));
    scene::Scene scene;
    std::vector<scene::EntityID> roots;
    ASSERT_TRUE(PrefabSerializer::Instantiate(scene, PrefabPathUtf8(), roots));
    auto* root = scene.GetGameObject(roots.front());
    const auto rootId = root->GetID();
    auto& added = scene.CreateGameObject("LocalChild");
    added.SetParent(*root);
    const auto addedId = added.GetID();
    sourceRoot.GetComponent<scene::LightComponent>()->intensity = 2.0f;
    ASSERT_TRUE(PrefabSerializer::SaveSelection(source, {sourceRoot.GetID()}, PrefabPathUtf8()));

    EXPECT_EQ(PrefabSerializer::PropagateToInstances(scene, PrefabPathUtf8(), {}, ProjectRoot().generic_string()), 0);

    EXPECT_EQ(scene.GetGameObject(rootId), root);
    EXPECT_EQ(scene.GetGameObject(addedId)->GetParent(), root);
    EXPECT_FLOAT_EQ(root->GetComponent<scene::LightComponent>()->intensity, 1.0f);
    std::vector<scene::EntityID> explicitRevert;
    ASSERT_TRUE(PrefabSerializer::Revert(scene, rootId, explicitRevert, ProjectRoot().generic_string()));
    EXPECT_FALSE(scene.IsValid(addedId));
    EXPECT_EQ(scene.GetGameObject(explicitRevert.front())->GetChildCount(), 0);
}

TEST_F(PrefabSaveTest, AutomaticPropagationKeepsLocallyDeletedChildren)
{
    scene::Scene source;
    auto& sourceRoot = source.CreateGameObject("Root");
    sourceRoot.AddComponent<scene::LightComponent>().intensity = 1.0f;
    source.CreateGameObject("Child").SetParent(sourceRoot);
    ASSERT_TRUE(PrefabSerializer::SaveSelection(source, {sourceRoot.GetID()}, PrefabPathUtf8()));
    scene::Scene scene;
    std::vector<scene::EntityID> roots;
    ASSERT_TRUE(PrefabSerializer::Instantiate(scene, PrefabPathUtf8(), roots));
    auto* root = scene.GetGameObject(roots.front());
    const auto rootId = root->GetID();
    ASSERT_EQ(root->GetChildCount(), 1);
    scene.DestroyGameObject(root->GetChild(0)->GetID());
    sourceRoot.GetComponent<scene::LightComponent>()->intensity = 2.0f;
    ASSERT_TRUE(PrefabSerializer::SaveSelection(source, {sourceRoot.GetID()}, PrefabPathUtf8()));

    EXPECT_EQ(PrefabSerializer::PropagateToInstances(scene, PrefabPathUtf8(), {}, ProjectRoot().generic_string()), 0);

    EXPECT_EQ(scene.GetGameObject(rootId), root);
    EXPECT_EQ(root->GetChildCount(), 0);
    EXPECT_EQ(scene.GameObjectCount(), 1u);
    EXPECT_FLOAT_EQ(root->GetComponent<scene::LightComponent>()->intensity, 1.0f);
}

TEST_F(PrefabSaveTest, AutomaticPropagationKeepsLocallyReparentedChildren)
{
    scene::Scene source;
    auto& sourceRoot = source.CreateGameObject("Root");
    auto& branchA = source.CreateGameObject("BranchA");
    branchA.SetParent(sourceRoot);
    source.CreateGameObject("BranchB").SetParent(sourceRoot);
    source.CreateGameObject("Leaf").SetParent(branchA);
    ASSERT_TRUE(PrefabSerializer::SaveSelection(source, {sourceRoot.GetID()}, PrefabPathUtf8()));
    scene::Scene scene;
    std::vector<scene::EntityID> roots;
    ASSERT_TRUE(PrefabSerializer::Instantiate(scene, PrefabPathUtf8(), roots));
    auto* root = scene.GetGameObject(roots.front());
    auto* parentA = root->GetChild(0);
    auto* parentB = root->GetChild(1);
    auto* leaf = parentA->GetChild(0);
    ASSERT_NE(leaf, nullptr);
    leaf->SetParent(*parentB);
    const auto rootId = root->GetID();
    const auto leafId = leaf->GetID();

    EXPECT_EQ(PrefabSerializer::PropagateToInstances(scene, PrefabPathUtf8(), {}, ProjectRoot().generic_string()), 0);

    EXPECT_EQ(scene.GetGameObject(rootId), root);
    EXPECT_EQ(scene.GetGameObject(leafId), leaf);
    EXPECT_EQ(leaf->GetParent(), parentB);
    EXPECT_EQ(parentA->GetChildCount(), 0);
}

TEST_F(PrefabSaveTest, LegacyNameParentsReceiveStableSourceIdsAndKeepTheirHierarchy)
{
    ASSERT_TRUE(util::FileSystem::WriteText(PrefabPathUtf8(),
        "[[gameobjects]]\nname='Root'\nparent=''\n"
        "[[gameobjects]]\nname='Child'\nparent='Root'\n"));
    scene::Scene scene;
    std::vector<scene::EntityID> roots;

    ASSERT_TRUE(PrefabSerializer::Instantiate(scene, PrefabPathUtf8(), roots));

    ASSERT_EQ(roots.size(), 1u);
    auto* root = scene.GetGameObject(roots.front());
    ASSERT_EQ(root->GetChildCount(), 1);
    const std::string rootGuid = root->instanceId;
    const std::string childGuid = root->GetChild(0)->instanceId;
    EXPECT_FALSE(root->prefabSourceId.empty());
    EXPECT_FALSE(root->GetChild(0)->prefabSourceId.empty());
    ASSERT_TRUE(PrefabSerializer::Revert(scene, root->GetID(), roots, ProjectRoot().generic_string()));
    EXPECT_EQ(scene.GetGameObject(roots.front()), scene.FindByGuid(rootGuid));
    EXPECT_EQ(scene.FindByGuid(childGuid)->GetParent(), scene.GetGameObject(roots.front()));
}

TEST_F(PrefabSaveTest, LegacyNameCyclesAndAmbiguousParentsDoNotChangeTheScene)
{
    scene::Scene scene;
    auto& existing = scene.CreateGameObject("Existing");
    existing.prefabAssetPath = PrefabPathUtf8("Cycle.prefab");
    const auto id = existing.GetID();
    ASSERT_TRUE(util::FileSystem::WriteText(PrefabPathUtf8("Cycle.prefab"),
        "[[gameobjects]]\nname='Root'\n"
        "[[gameobjects]]\nname='A'\nparent='B'\n"
        "[[gameobjects]]\nname='B'\nparent='A'\n"));
    std::vector<scene::EntityID> roots;

    EXPECT_FALSE(PrefabSerializer::Revert(scene, id, roots, ProjectRoot().generic_string()));
    EXPECT_EQ(scene.GetGameObject(id), &existing);

    existing.prefabAssetPath = PrefabPathUtf8("Ambiguous.prefab");
    ASSERT_TRUE(util::FileSystem::WriteText(PrefabPathUtf8("Ambiguous.prefab"),
        "[[gameobjects]]\nname='Parent'\n"
        "[[gameobjects]]\nname='Parent'\n"
        "[[gameobjects]]\nname='Child'\nparent='Parent'\n"));
    EXPECT_FALSE(PrefabSerializer::Revert(scene, id, roots, ProjectRoot().generic_string()));
    EXPECT_EQ(scene.GetGameObject(id), &existing);
    EXPECT_EQ(scene.GameObjectCount(), 1u);
}

TEST_F(PrefabSaveTest, RendererOnlyPrefabKeepsItsExternalSharedSkeleton)
{
    scene::Scene scene;
    scene.CreateGameObject("Unrelated");
    auto& skeleton = scene.CreateGameObject("SharedArmature");
    auto& mesh = scene.CreateGameObject("Mesh");
    mesh.AddComponent<scene::SkinnedMeshRenderer>().skeletonRootEntity = skeleton.GetID();
    ASSERT_TRUE(PrefabSerializer::SaveSelection(scene, {mesh.GetID()}, PrefabPathUtf8()));
    std::vector<scene::EntityID> roots;

    ASSERT_TRUE(PrefabSerializer::Instantiate(scene, PrefabPathUtf8(), roots));

    auto* renderer = scene.GetGameObject(roots.front())->GetComponent<scene::SkinnedMeshRenderer>();
    ASSERT_NE(renderer, nullptr);
    EXPECT_EQ(renderer->skeletonRootEntity, skeleton.GetID());
    ASSERT_TRUE(PrefabSerializer::Revert(scene, roots.front(), roots, ProjectRoot().generic_string()));
    EXPECT_EQ(scene.GetGameObject(roots.front())->GetComponent<scene::SkinnedMeshRenderer>()->skeletonRootEntity,
              skeleton.GetID());
}

TEST_F(PrefabSaveTest, MultipleRootsConnectToTheExactSavedSourceIdsAndRefuseSingleRootReplacement)
{
    scene::Scene scene;
    auto& first = scene.CreateGameObject("First");
    auto& second = scene.CreateGameObject("Second");
    first.prefabSourceId = "shared-source";
    second.prefabSourceId = "shared-source";
    const auto firstId = first.GetID();
    const auto secondId = second.GetID();
    std::vector<scene::EntityID> connected;
    ASSERT_TRUE(PrefabSerializer::SaveSelectionAndConnect(
        scene, {secondId, firstId}, PrefabPathUtf8(), connected));
    const auto parsed = toml::parse(SavedText());
    ASSERT_TRUE(parsed);
    const auto* objects = parsed.table()["gameobjects"].as_array();
    ASSERT_NE(objects, nullptr);
    ASSERT_EQ(objects->size(), 2u);
    EXPECT_EQ((*objects->front().as_table())["instanceId"].value_or(std::string{}), first.prefabSourceId);
    EXPECT_EQ((*objects->back().as_table())["instanceId"].value_or(std::string{}), second.prefabSourceId);
    EXPECT_NE(first.prefabSourceId, second.prefabSourceId);
    std::vector<scene::EntityID> roots;

    EXPECT_FALSE(PrefabSerializer::Revert(scene, firstId, roots, ProjectRoot().generic_string()));

    EXPECT_EQ(scene.GetGameObject(firstId), &first);
    EXPECT_EQ(scene.GetGameObject(secondId), &second);
    EXPECT_EQ(scene.GameObjectCount(), 2u);
    EXPECT_TRUE(roots.empty());
    const std::string saved = SavedText();
    EXPECT_FALSE(PrefabSerializer::Apply(scene, firstId, ProjectRoot().generic_string()));
    EXPECT_EQ(SavedText(), saved);
}

TEST_F(PrefabSaveTest, EmptySaveAndMissingGuidApplyFailWithoutChangingTheInstance)
{
    scene::Scene scene;
    auto& root = scene.CreateGameObject("Root");
    root.prefabAssetPath = "guid:00000000-0000-4000-8000-000000000000";
    const auto id = root.GetID();
    const std::string reference = root.prefabAssetPath;

    EXPECT_FALSE(PrefabSerializer::SaveSelection(scene, {id}, ""));
    EXPECT_FALSE(PrefabSerializer::Apply(scene, id, ProjectRoot().generic_string()));

    EXPECT_EQ(scene.GetGameObject(id), &root);
    EXPECT_EQ(root.prefabAssetPath, reference);
    EXPECT_EQ(scene.GameObjectCount(), 1u);
}

} /// @note namespace fbzz::tests
