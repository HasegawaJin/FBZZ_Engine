/// @file    PrefabSaveTests.cpp
/// @brief   選択範囲を .prefab として切り出す処理。
/// @author  Hasegawa Jin
/// @date    2026-09-10
///
/// 切り出しは «どこまでを含めるか» と «アセット側の id を安定させるか» の 2 点で事故る。
/// 含める範囲を間違えると子だけのプレハブができ、id が毎回変わると既存インスタンスの
/// prefabSourceId が行き先を失って override の対応が切れる。
///
/// @note 読み戻し (Instantiate) は ResourceManager::Active() を要求するため GPU 無しのテストからは呼べない。ここでは書き出したアセットの中身を直接読む。
#include <TestKit/TestKit.hpp>
#include <TestKit/Editor/EditorFixture.hpp>

#include <Editor/Util/PrefabSerializer.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>

#include <toml++/toml.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
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

/// 書き出した .prefab の gameobjects 配列から、名前の一覧を取り出す。
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

} // namespace

/// SaveSelection は SceneIO::Serialize でシーンを直列化する。その一時ファイルの
/// 置き場所が要るため EditorFixture に載せる (無いと空文字が返り、保存が失敗する)。
class PrefabSaveTest : public testkit::EditorFixture {
protected:
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
    ///       子まで根に数えると、開いたときに同じ枝が二重に生える。
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
    ///       他インスタンスの prefabSourceId が行き先を失い override の対応が切れる。
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
    ///       両方に同じ id を振ると、アセット側で 1 つに潰れて片方が消える。
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

} // namespace fbzz::tests
