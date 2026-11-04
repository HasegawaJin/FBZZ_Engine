/// @file    ModelImportOperatorTests.cpp
/// @brief   モデル変換Operatorの設定保全・原本境界・非同期生成結果を検証する。
/// @author  Hasegawa Jin
/// @date    2026-11-03
#include <TestKit/TestKit.hpp>
#include <TestKit/Editor/EditorFixture.hpp>
#include <Editor/Import/FbxMetaSerializer.hpp>
#include <Editor/Op/OperatorGroups.hpp>
#include <Editor/Util/UndoStack.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Core/Concurrency/TaskSystem.hpp>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fbzz::tests {

class ModelImportOperatorTest : public testkit::EditorFixture {
protected:
    void SetUp() override
    {
        EditorFixture::SetUp();
        Context().projectRoot = ProjectRoot().generic_string();
        std::ofstream(File("Assets/Triangle.obj")) << "o Triangle\nv 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
        asset::AssetDatabase::Init(File("Assets").generic_string());
        editor::RegisterModelImportOperators(m_registry);
    }

    void TearDown() override
    {
        TaskSystem::Shutdown();
        m_registry.Clear();
        asset::AssetDatabase::Shutdown();
        EditorFixture::TearDown();
    }

    editor::OpArgs Args() const
    {
        editor::OpArgs args;
        args.Set("path", std::string("Assets/Triangle.obj"));
        return args;
    }

    editor::OpResult Invoke(const char* id, const editor::OpArgs& args)
    {
        editor::OpContext context{Context(), m_undo};
        return m_registry.Invoke(id, context, args);
    }

    std::string Read(const std::filesystem::path& path) const
    {
        std::ifstream input(path, std::ios::binary);
        std::ostringstream result;
        result << input.rdbuf();
        return result.str();
    }

    editor::OperatorRegistry m_registry;
    editor::UndoStack m_undo;
};

TEST_F(ModelImportOperatorTest, InspectReportsSourceContentsWithoutCreatingBakedOutputs)
{
    const auto result = Invoke("asset.model.inspect", Args());

    ASSERT_TRUE(result.ok) << result.message;
    const auto* meshes = result.data.Find("meshNames");
    ASSERT_NE(meshes, nullptr);
    ASSERT_EQ(meshes->AsArray().size(), 1u);
    EXPECT_EQ(meshes->AsArray()[0].AsString(), "Triangle");
    EXPECT_FALSE(std::filesystem::exists(File("Library/Baked")));
}

TEST_F(ModelImportOperatorTest, ConfigurePreservesUnspecifiedOptionsAndTheSourceGuid)
{
    editor::FbxImportOptions original;
    original.generateTangents = false;
    original.unitScaleMultiplier = 2.0f;
    ASSERT_TRUE(editor::FbxMetaSerializer::SaveOptions(File("Assets/Triangle.obj").generic_string(), original));
    const std::string guid = asset::AssetDatabase::GuidFromPath(File("Assets/Triangle.obj").generic_string());
    auto args = Args();
    auto options = editor::OpData::MakeObject();
    options.Set("generateNormals", editor::OpData(false));
    args.Set("options", options);

    const auto result = Invoke("asset.model.configure", args);

    ASSERT_TRUE(result.ok) << result.message;
    editor::FbxImportOptions actual;
    ASSERT_TRUE(editor::FbxMetaSerializer::LoadOptions(File("Assets/Triangle.obj").generic_string(), actual));
    EXPECT_FALSE(actual.generateNormals);
    EXPECT_FALSE(actual.generateTangents);
    EXPECT_NEAR(actual.unitScaleMultiplier, 2.0f, 0.0001f);
    EXPECT_EQ(asset::AssetDatabase::GuidFromPath(File("Assets/Triangle.obj").generic_string()), guid);
}

TEST_F(ModelImportOperatorTest, InvalidPartialSettingsLeaveTheWholeSidecarUnchanged)
{
    const auto meta = File("Assets/Triangle.obj.meta");
    const std::string before = Read(meta);
    auto args = Args();
    auto options = editor::OpData::MakeObject();
    options.Set("unitScaleMultiplier", editor::OpData(4.0f));
    options.Set("sourceDcc", editor::OpData("Unknown"));
    args.Set("options", options);

    const auto result = Invoke("asset.model.configure", args);

    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.errorCode, "BAD_ARG");
    EXPECT_EQ(Read(meta), before);
}

TEST_F(ModelImportOperatorTest, RejectsSourcesOutsideAssetsWithoutCreatingMetadata)
{
    std::ofstream(File("Outside.obj")) << Read(File("Assets/Triangle.obj"));
    auto args = Args();
    args.Set("path", std::string("Assets/../Outside.obj"));

    const auto result = Invoke("asset.model.import", args);

    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.errorCode, "MODEL_SOURCE_NOT_FOUND");
    EXPECT_FALSE(std::filesystem::exists(File("Outside.obj.meta")));
}

TEST_F(ModelImportOperatorTest, RejectsUnknownClipSelectionBeforeChangingSettingsOrOutputs)
{
    const std::string before = Read(File("Assets/Triangle.obj.meta"));
    auto args = Args();
    auto options = editor::OpData::MakeObject();
    auto names = editor::OpData::MakeArray(); names.Push(editor::OpData("Missing"));
    options.Set("selectedAnimNames", names); args.Set("options", options);

    const auto result = Invoke("asset.model.import", args);

    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.errorCode, "BAD_ARG");
    EXPECT_EQ(Read(File("Assets/Triangle.obj.meta")), before);
    EXPECT_FALSE(std::filesystem::exists(File("Library/Baked")));
}

TEST_F(ModelImportOperatorTest, ImportCompletesWithoutOpeningAPanelAndPublishesOutputs)
{
    TaskSystem::Init(1);
    const auto started = Invoke("asset.model.import", Args());
    ASSERT_TRUE(started.ok) << started.message;
    const auto* job = started.data.Find("job");
    ASSERT_NE(job, nullptr);
    editor::OpArgs args;
    args.Set("job", job->AsInt());
    /// @note 通常のShutdownが投入済み処理をjoinするため、sleepや時計で完了を推測しない。
    TaskSystem::Shutdown();

    const auto completed = Invoke("asset.model.status", args);

    ASSERT_TRUE(completed.ok) << completed.message;
    ASSERT_NE(completed.data.Find("state"), nullptr);
    EXPECT_EQ(completed.data.Find("state")->AsString(), "done");
    ASSERT_NE(completed.data.Find("outputs"), nullptr);
    EXPECT_FALSE(completed.data.Find("outputs")->AsArray().empty());
    const auto guid = asset::AssetDatabase::GuidFromPath(File("Assets/Triangle.obj").generic_string());
    EXPECT_TRUE(std::filesystem::exists(File("Library/Baked/" + guid + "/Triangle.fzasset")));
    EXPECT_FALSE(editor::FbxMetaSerializer::LoadCacheInfo(File("Assets/Triangle.obj").generic_string()).contentHash.empty());
}

} /// @note namespace fbzz::tests
