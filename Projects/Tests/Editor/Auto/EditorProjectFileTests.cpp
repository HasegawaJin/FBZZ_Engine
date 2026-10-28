/// @file    EditorProjectFileTests.cpp
/// @brief   AI の新規保存先と projectRoot 外の拒否を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-04
#include <TestKit/TestKit.hpp>
#include <TestKit/Editor/EditorFixture.hpp>
#include <Editor/Ai/EditorBusDispatcher.hpp>
#include <Editor/Ai/EditorBusProtocol.hpp>
#include <Engine/Scene/SceneSerializer.hpp>
#include <Engine/Scene/ScriptComponent.hpp>
#include <Engine/Util/FileSystem.hpp>
#include "../../../Editor/src/Ai/Bus/BusInternal.hpp"
#include <filesystem>
#include <memory>
#include <string>

namespace fbzz::tests {

class EditorProjectFileTest : public testkit::EditorFixture {
protected:
    void SetUp() override
    {
        EditorFixture::SetUp();
        Context().projectRoot = ProjectRoot().generic_string();
        ASSERT_TRUE(util::FileSystem::EnsureDirectory(File("Assets/Scenes")));
    }

    bool Resolve(const std::string& requested, std::filesystem::path& path, std::string& relative)
    {
        return editor::ai::bus::ResolveProjectFile(Context(), requested, path, relative);
    }

    void ExpectRejected(const std::string& requested)
    {
        std::filesystem::path path = "unchanged-output";
        std::string relative = "unchanged-relative";
        EXPECT_FALSE(Resolve(requested, path, relative));
        EXPECT_EQ(path, std::filesystem::path("unchanged-output"));
        EXPECT_EQ(relative, "unchanged-relative");
    }
};

TEST_F(EditorProjectFileTest, ResolvesNewRelativeAndAbsoluteFilesWithoutCreatingThem)
{
    const std::string requested = "Assets/Scenes/New.scene";
    ASSERT_FALSE(std::filesystem::exists(File(requested)));
    std::filesystem::path path;
    std::string relative;
    ASSERT_TRUE(Resolve(requested, path, relative));
    EXPECT_EQ(path, File(requested));
    EXPECT_EQ(relative, requested);
    ASSERT_TRUE(Resolve(File(requested).generic_string(), path, relative));
    EXPECT_EQ(path, File(requested));
    EXPECT_EQ(relative, requested);
    EXPECT_FALSE(std::filesystem::exists(path));
}

TEST_F(EditorProjectFileTest, ResolvesMissingParentDirectoriesWithoutCreatingThem)
{
    const std::string requested = "Assets/Scenes/NewFolder/Nested/New.scene";
    ASSERT_FALSE(std::filesystem::exists(File("Assets/Scenes/NewFolder")));
    std::filesystem::path path;
    std::string relative;
    ASSERT_TRUE(Resolve(requested, path, relative));
    EXPECT_EQ(path, File(requested));
    EXPECT_EQ(relative, requested);
    EXPECT_FALSE(std::filesystem::exists(path.parent_path()));
}

TEST_F(EditorProjectFileTest, ResolvesExistingOutputAndNormalizesInternalDotDot)
{
    const std::string requested = "Assets/Scenes/Existing.scene";
    ASSERT_TRUE(util::FileSystem::WriteText(File(requested), "existing scene"));
    std::filesystem::path path;
    std::string relative;
    ASSERT_TRUE(Resolve(requested, path, relative));
    EXPECT_EQ(path, File(requested));
    ASSERT_TRUE(Resolve("Assets/Scenes/../Scenes/Existing.scene", path, relative));
    EXPECT_EQ(relative, requested);
    EXPECT_EQ(path, File(requested));
}

TEST_F(EditorProjectFileTest, RejectsParentTraversalAndAbsoluteExternalFiles)
{
    ExpectRejected("../Outside.scene");
    ExpectRejected("Assets/Scenes/../../../Outside.scene");
    ExpectRejected((ProjectRoot().parent_path() / "Outside.scene").generic_string());
    ExpectRejected(ProjectRoot().generic_string() + "-outside/New.scene");
}

TEST_F(EditorProjectFileTest, RejectsMissingInputsWithoutChangingOutputs)
{
    ExpectRejected("");
    Context().projectRoot.clear();
    ExpectRejected("Assets/Scenes/New.scene");
}

TEST_F(EditorProjectFileTest, AiSaveToNewFilePreservesNamedObjectsAndMissingScriptReferences)
{
    auto& scene = AttachScene();
    auto& target = scene.CreateGameObject("Player");
    auto& actor = scene.CreateGameObject("Enemy");
    auto& entry = actor.AddComponent<scene::ScriptComponent>().scripts.emplace_back();
    const std::string type = "cs:abababababababababababababababab";
    const std::string fields = "target = '" + target.instanceId + "'\ntargets = ['" + target.instanceId + "', '', 'missing-target']\n";
    entry.serialized = std::make_unique<scene::SerializedScriptData>(scene::SerializedScriptData{type, fields, true});
    Context().saveScenePathImmediate = [this](const std::string& path) {
        if (!scene::SceneSerializer::Save(*AttachedScene(), path)) return false;
        Context().currentScenePath = path;
        Context().sceneDirty = false;
        return true;
    };

    editor::ai::JsonValue payload = editor::ai::JsonValue::MakeObject();
    payload.Set("t", editor::ai::JsonValue("scene.save"));
    payload.Set("path", editor::ai::JsonValue("Assets/Scenes/NewSaved.scene"));
    editor::ai::JsonValue request = editor::ai::JsonValue::MakeObject();
    request.Set("protocol", editor::ai::JsonValue(editor::ai::kEditorProtocol));
    request.Set("id", editor::ai::JsonValue("new-scene-save"));
    request.Set("kind", editor::ai::JsonValue("command"));
    request.Set("dryRun", editor::ai::JsonValue(false));
    request.Set("payload", std::move(payload));
    editor::ai::EditorBusDispatcher dispatcher(Context());
    const std::string response = dispatcher.Handle(editor::ai::SerializeJson(request));
    const auto parsed = editor::ai::ParseJson(response);
    ASSERT_TRUE(parsed.has_value()) << response;
    ASSERT_NE(parsed->Find("ok"), nullptr);
    ASSERT_TRUE(parsed->Find("ok")->AsBool()) << response;
    ASSERT_TRUE(std::filesystem::is_regular_file(File("Assets/Scenes/NewSaved.scene")));

    auto restored = scene::SceneSerializer::LoadData(File("Assets/Scenes/NewSaved.scene").generic_string());
    ASSERT_NE(restored, nullptr);
    EXPECT_NE(restored->Find("Player"), nullptr);
    auto* restoredActor = restored->Find("Enemy");
    ASSERT_NE(restoredActor, nullptr);
    auto* scripts = restoredActor->GetComponent<scene::ScriptComponent>();
    ASSERT_NE(scripts, nullptr);
    ASSERT_EQ(scripts->scripts.size(), 1u);
    ASSERT_NE(scripts->scripts.front().serialized, nullptr);
    EXPECT_EQ(scripts->scripts.front().serialized->type, type);
    const std::string& restoredFields = scripts->scripts.front().serialized->fieldsToml;
    const std::size_t targetPosition = restoredFields.find(target.instanceId);
    ASSERT_NE(targetPosition, std::string::npos);
    EXPECT_NE(restoredFields.find(target.instanceId, targetPosition + target.instanceId.size()), std::string::npos);
    EXPECT_NE(restoredFields.find("missing-target"), std::string::npos);
}

} /// @note namespace fbzz::tests
