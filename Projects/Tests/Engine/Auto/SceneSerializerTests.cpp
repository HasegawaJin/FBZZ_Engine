/// @file    SceneSerializerTests.cpp
/// @brief   シーンの保存・復元が往復し、GPU リソース無しでもデータが揃うことを検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-08
///
/// 「保存して開いたら同じ」はシーンで最も事故が多い契約。項目を足したときに
/// 片側だけ書き忘れると、保存はできるのに開くと既定値へ戻る ── エディタで
/// 作業した内容が «静かに» 消え、履歴からも追えない。
///
/// ここでは LoadData (GPU リソースを作らない復元) を使う。デバイスを要求しないので、
/// 往復の検証そのものがテストとして成立する。
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>

#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/SceneSerializer.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Util/FileSystem.hpp>

#include <Math/MathUtils.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>

#include <memory>
#include <string>

namespace fbzz::tests {

class SceneSerializerTest : public testkit::EngineFixture {
protected:
    void SetUp() override
    {
        testkit::EngineFixture::SetUp();
        ASSERT_TRUE(m_temp.IsValid());
    }

    std::string ScenePath(const std::string& name = "test.scene") const
    {
        return m_temp.File(name).generic_string();
    }

    /// scene を保存して、GPU リソース抜きで読み直す。
    std::unique_ptr<scene::Scene> RoundTrip(scene::Scene& source)
    {
        const std::string path = ScenePath();
        if (!scene::SceneSerializer::Save(source, path)) return nullptr;
        return scene::SceneSerializer::LoadData(path);
    }

private:
    testkit::TempDir m_temp{"scene"};
};

/// @name 骨格

TEST_F(SceneSerializerTest, SavesAndReloadsAnEmptyScene)
{
    scene::Scene source;

    const std::unique_ptr<scene::Scene> restored = RoundTrip(source);

    ASSERT_NE(restored, nullptr);
    EXPECT_TRUE(restored->GetRootGameObjects().empty());
}

TEST_F(SceneSerializerTest, KeepsEveryGameObject)
{
    scene::Scene source;
    source.CreateGameObject("Alpha");
    source.CreateGameObject("Beta");
    source.CreateGameObject("Gamma");

    const std::unique_ptr<scene::Scene> restored = RoundTrip(source);

    ASSERT_NE(restored, nullptr);
    EXPECT_NE(restored->Find("Alpha"), nullptr);
    EXPECT_NE(restored->Find("Beta"), nullptr);
    EXPECT_NE(restored->Find("Gamma"), nullptr);
    EXPECT_EQ(restored->GetRootGameObjects().size(), 3u);
}

TEST_F(SceneSerializerTest, KeepsNameTagAndLayer)
{
    scene::Scene source;
    scene::GameObject& object = source.CreateGameObject("Player");
    object.tag   = "Enemy";
    object.layer = 4;

    const std::unique_ptr<scene::Scene> restored = RoundTrip(source);

    ASSERT_NE(restored, nullptr);
    scene::GameObject* reloaded = restored->Find("Player");
    ASSERT_NE(reloaded, nullptr);
    EXPECT_EQ(reloaded->tag, "Enemy");
    EXPECT_EQ(reloaded->layer, 4);
}

TEST_F(SceneSerializerTest, KeepsThePersistentIdentifier)
{
    /// @note instanceId はクロスオブジェクト参照 (IK の Pole/Target 等) の永続キー。
    ///       保存で振り直すと、開き直した瞬間に参照が全部切れる。
    scene::Scene source;
    const std::string id = source.CreateGameObject("Player").instanceId;
    ASSERT_FALSE(id.empty());

    const std::unique_ptr<scene::Scene> restored = RoundTrip(source);

    ASSERT_NE(restored, nullptr);
    EXPECT_NE(restored->FindByGuid(id), nullptr);
}

TEST_F(SceneSerializerTest, KeepsTheActiveFlag)
{
    scene::Scene source;
    source.CreateGameObject("Hidden").SetActive(false);

    const std::unique_ptr<scene::Scene> restored = RoundTrip(source);

    ASSERT_NE(restored, nullptr);
    scene::GameObject* reloaded = restored->Find("Hidden");
    ASSERT_NE(reloaded, nullptr);
    EXPECT_FALSE(reloaded->activeSelf());
}

/// @name Transform

TEST_F(SceneSerializerTest, KeepsTheLocalTransform)
{
    scene::Scene source;
    scene::GameObject& object = source.CreateGameObject("Moved");
    object.transform.position = { 1.5f, -2.5f, 3.25f };
    object.transform.rotation =
        math::Quaternion::FromAxisAngle(math::Vector3::UP, math::ToRad(30.0f));
    object.transform.scale = { 2.0f, 0.5f, 1.0f };

    const std::unique_ptr<scene::Scene> restored = RoundTrip(source);

    ASSERT_NE(restored, nullptr);
    scene::GameObject* reloaded = restored->Find("Moved");
    ASSERT_NE(reloaded, nullptr);
    EXPECT_VEC3_NEAR(reloaded->transform.position, math::Vector3(1.5f, -2.5f, 3.25f),
                     testkit::kLooseTolerance);
    EXPECT_VEC3_NEAR(reloaded->transform.scale, math::Vector3(2.0f, 0.5f, 1.0f),
                     testkit::kLooseTolerance);
    EXPECT_QUAT_NEAR(reloaded->transform.rotation, object.transform.rotation,
                     testkit::kLooseTolerance);
}

/// @name 階層

TEST_F(SceneSerializerTest, KeepsTheParentChildHierarchy)
{
    scene::Scene source;
    scene::GameObject& parent = source.CreateGameObject("Parent");
    scene::GameObject& child  = source.CreateGameObject("Child");
    child.SetParent(parent);

    const std::unique_ptr<scene::Scene> restored = RoundTrip(source);

    ASSERT_NE(restored, nullptr);
    /// @note 親子が復元されていれば、ルートは親だけになる。
    EXPECT_EQ(restored->GetRootGameObjects().size(), 1u);
    EXPECT_NE(restored->Find("Child"), nullptr);
}

TEST_F(SceneSerializerTest, KeepsDeepHierarchies)
{
    scene::Scene source;
    scene::GameObject& a = source.CreateGameObject("A");
    scene::GameObject& b = source.CreateGameObject("B");
    scene::GameObject& c = source.CreateGameObject("C");
    b.SetParent(a);
    c.SetParent(b);

    const std::unique_ptr<scene::Scene> restored = RoundTrip(source);

    ASSERT_NE(restored, nullptr);
    EXPECT_EQ(restored->GetRootGameObjects().size(), 1u);
    EXPECT_NE(restored->Find("C"), nullptr);
}

/// @name 実行時生成物

TEST_F(SceneSerializerTest, DoesNotSaveRuntimeGeneratedObjects)
{
    /// @note VFX のノード実体や Water の飛沫。保存すると、開くたびにゾンビ GO が増える。
    scene::Scene source;
    source.CreateGameObject("Authored");
    source.CreateGameObject("Spawned").runtimeGenerated = true;

    const std::unique_ptr<scene::Scene> restored = RoundTrip(source);

    ASSERT_NE(restored, nullptr);
    EXPECT_NE(restored->Find("Authored"), nullptr);
    EXPECT_EQ(restored->Find("Spawned"), nullptr);
}

/// @name GPU リソースを作らない復元

TEST_F(SceneSerializerTest, KeepsTheMeshPathWithoutBuildingGpuResources)
{
    /// @note これが «データ復元と GPU リソース生成を分けた» ことの中身。
    ///       デバイス無しでも参照 (パス) は完全に残り、実体だけが後回しになる。
    scene::Scene source;
    scene::GameObject& object = source.CreateGameObject("Prop");
    scene::MeshRenderer renderer{};
    renderer.meshPath    = "Assets/Models/Prop.fbx";
    renderer.castShadows = false;
    object.AddComponent<scene::MeshRenderer>(std::move(renderer));

    const std::unique_ptr<scene::Scene> restored = RoundTrip(source);

    ASSERT_NE(restored, nullptr);
    scene::GameObject* reloaded = restored->Find("Prop");
    ASSERT_NE(reloaded, nullptr);
    auto* mr = reloaded->GetComponent<scene::MeshRenderer>();
    ASSERT_NE(mr, nullptr);
    EXPECT_EQ(mr->meshPath, "Assets/Models/Prop.fbx");
    EXPECT_FALSE(mr->castShadows);
    /// @note GPU リソースは作らない
    EXPECT_EQ(mr->mesh, nullptr);
}

TEST_F(SceneSerializerTest, KeepsSeveralMeshPathsIndependently)
{
    /// @note 後から ResolveMeshes() で結び直すのはこの meshPath。1 つでも落ちると
    ///       «そのオブジェクトだけ描かれない» になる。
    scene::Scene source;
    for (int i = 0; i < 3; ++i) {
        scene::GameObject& object = source.CreateGameObject("Prop" + std::to_string(i));
        scene::MeshRenderer renderer{};
        renderer.meshPath = "Assets/Models/Prop" + std::to_string(i) + ".fbx";
        object.AddComponent<scene::MeshRenderer>(std::move(renderer));
    }

    const std::unique_ptr<scene::Scene> restored = RoundTrip(source);

    ASSERT_NE(restored, nullptr);
    for (int i = 0; i < 3; ++i) {
        scene::GameObject* reloaded = restored->Find("Prop" + std::to_string(i));
        ASSERT_NE(reloaded, nullptr) << i;
        auto* mr = reloaded->GetComponent<scene::MeshRenderer>();
        ASSERT_NE(mr, nullptr) << i;
        EXPECT_EQ(mr->meshPath, "Assets/Models/Prop" + std::to_string(i) + ".fbx");
    }
}

/// @name 壊れた入力

TEST_F(SceneSerializerTest, RejectsTextThatIsNotToml)
{
    EXPECT_EQ(scene::SceneSerializer::LoadDataFromText("{{{ not toml ]]]", "broken.scene"),
              nullptr);
}

TEST_F(SceneSerializerTest, RejectsAMissingFile)
{
    EXPECT_EQ(scene::SceneSerializer::LoadData(ScenePath("does_not_exist.scene")), nullptr);
}

TEST_F(SceneSerializerTest, AcceptsAnEmptyDocument)
{
    /// @note 空のシーンファイルは «壊れている» ではなく «何も無い»。
    const std::unique_ptr<scene::Scene> restored =
        scene::SceneSerializer::LoadDataFromText("", "empty.scene");

    ASSERT_NE(restored, nullptr);
    EXPECT_TRUE(restored->GetRootGameObjects().empty());
}

/// @name 安定性

TEST_F(SceneSerializerTest, SavingTwiceProducesTheSameFile)
{
    /// @note 触っていないのに差分が出ると、シーンのコミットに毎回ノイズが混ざる。
    scene::Scene source;
    scene::GameObject& object = source.CreateGameObject("Stable");
    object.transform.position = { 1.0f, 2.0f, 3.0f };

    ASSERT_TRUE(scene::SceneSerializer::Save(source, ScenePath("a.scene")));
    ASSERT_TRUE(scene::SceneSerializer::Save(source, ScenePath("b.scene")));

    std::string first;
    std::string second;
    ASSERT_TRUE(util::FileSystem::ReadText(ScenePath("a.scene"), first));
    ASSERT_TRUE(util::FileSystem::ReadText(ScenePath("b.scene"), second));
    EXPECT_EQ(first, second);
}

TEST_F(SceneSerializerTest, ASecondRoundTripChangesNothing)
{
    scene::Scene source;
    scene::GameObject& object = source.CreateGameObject("Stable");
    object.tag                = "Player";
    object.transform.position = { 1.0f, 2.0f, 3.0f };

    const std::unique_ptr<scene::Scene> once = RoundTrip(source);
    ASSERT_NE(once, nullptr);
    const std::unique_ptr<scene::Scene> twice = RoundTrip(*once);

    ASSERT_NE(twice, nullptr);
    scene::GameObject* reloaded = twice->Find("Stable");
    ASSERT_NE(reloaded, nullptr);
    EXPECT_EQ(reloaded->tag, "Player");
    EXPECT_VEC3_NEAR(reloaded->transform.position, math::Vector3(1.0f, 2.0f, 3.0f),
                     testkit::kLooseTolerance);
}

} // namespace fbzz::tests
