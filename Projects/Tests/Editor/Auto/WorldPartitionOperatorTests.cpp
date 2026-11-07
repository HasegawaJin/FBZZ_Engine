/// @file    WorldPartitionOperatorTests.cpp
/// @brief   World operation discovery, input limits and authoring permissions share the production registry.
/// @author  Hasegawa Jin
/// @date    2026-11-07
#include <TestKit/TestKit.hpp>
#include <TestKit/Editor/EditorFixture.hpp>
#include <Editor/Ai/OperatorBridge.hpp>
#include <Editor/Op/OperatorGroups.hpp>
#include <Editor/PlayModeController.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Components/WorldPartitionComponent.hpp>
#include <Engine/Scene/Components/WorldPcgAuthoringComponent.hpp>
#include <Engine/Scene/WorldPcg.hpp>
#include <filesystem>
#include <iterator>
#include <string>
#include <utility>

namespace fbzz::tests {

class WorldPartitionOperatorTest : public testkit::EditorFixture {
protected:
    void SetUp() override
    {
        EditorFixture::SetUp();
        (void)AttachScene();
        Context().projectRoot = ProjectRoot().generic_string();
        Context().operators = &m_registry;
        Context().undoStack = &m_undo;
        Context().markSceneDirty = [this] { ++m_dirtyCalls; };
        editor::RegisterWorldPartitionOperators(m_registry);
    }

    void TearDown() override
    {
        m_play.Cancel();
        Context().playMode = nullptr;
        m_undo.Clear();
        Context().operators = nullptr;
        Context().undoStack = nullptr;
        EditorFixture::TearDown();
    }

    editor::OpResult Invoke(const char* id, const editor::OpArgs& args = {})
    {
        editor::OpContext context{Context(), m_undo};
        return m_registry.Invoke(id, context, args);
    }

    bool Available(const char* id)
    {
        const editor::OpContext context{Context(), m_undo};
        return m_registry.CanInvoke(id, context);
    }

    editor::ai::JsonValue Request(const char* id)
    {
        auto request = editor::ai::JsonValue::MakeObject();
        request.Set("id", editor::ai::JsonValue(id));
        return request;
    }

    editor::OperatorRegistry m_registry;
    editor::UndoStack m_undo;
    editor::PlayModeController m_play;
    int m_dirtyCalls = 0;
};

TEST_F(WorldPartitionOperatorTest, RegistersAllWorldOperationsWithTheirPermissionKinds)
{
    struct ExpectedOperation {
        const char* id;
        editor::OpKind kind;
    };
    const ExpectedOperation expected[] = {
        {"worldcell.inspect", editor::OpKind::Query},
        {"worldcell.configure", editor::OpKind::Mutation},
        {"pcg.profile.inspect", editor::OpKind::Query},
        {"pcg.profile.save", editor::OpKind::Action},
        {"pcg.volume.configure", editor::OpKind::Mutation},
        {"pcg.preview", editor::OpKind::Query},
        {"pcg.generate", editor::OpKind::Mutation},
        {"pcg.detach", editor::OpKind::Mutation},
        {"worldcell.bake", editor::OpKind::Action},
        {"hlod.build", editor::OpKind::Action},
        {"worldcell.stream_status", editor::OpKind::Query},
        {"worldcell.stream_control", editor::OpKind::Action},
    };
    ASSERT_EQ(m_registry.All().size(), std::size(expected));
    for (const auto& entry : expected) {
        const auto* operation = m_registry.Find(entry.id);
        ASSERT_NE(operation, nullptr) << entry.id;
        EXPECT_EQ(operation->kind, entry.kind) << entry.id;
        EXPECT_FALSE(operation->desc.empty()) << entry.id;
        EXPECT_EQ(operation->category, "World") << entry.id;
        EXPECT_EQ(operation->undoLabel.empty(), entry.kind != editor::OpKind::Mutation) << entry.id;
        for (const auto& parameter : operation->params) EXPECT_FALSE(parameter.desc.empty()) << entry.id << '/' << parameter.name;
    }
}

TEST_F(WorldPartitionOperatorTest, ReadBridgeRejectsMutationsAndFileActionsWithoutChangingTheScene)
{
    for (const char* id : {"worldcell.configure", "pcg.generate", "pcg.profile.save", "worldcell.bake", "hlod.build", "worldcell.stream_control"}) {
        const auto result = editor::ai::QueryOperator(Context(), Request(id));
        EXPECT_FALSE(result.ok) << id;
        EXPECT_EQ(result.code, "NOT_A_QUERY") << id;
    }
    EXPECT_TRUE(AttachedScene()->GetRootGameObjects().empty());
    EXPECT_EQ(m_dirtyCalls, 0);
    EXPECT_FALSE(m_undo.CanUndo());
    EXPECT_FALSE(std::filesystem::exists(File("Assets/World")));
}

TEST_F(WorldPartitionOperatorTest, DryRunRejectsUndeclaredInputsAndInvalidNumericRangesBeforeServicesRun)
{
    for (const float cellSize : {0.5f, 65537.0f}) {
        auto request = Request("worldcell.configure");
        auto args = editor::ai::JsonValue::MakeObject();
        args.Set("cellSize", editor::ai::JsonValue(static_cast<double>(cellSize)));
        request.Set("args", std::move(args));
        const auto result = editor::ai::InvokeOperator(Context(), request, true);
        EXPECT_FALSE(result.ok);
        EXPECT_EQ(result.code, "BAD_ARG");
    }
    auto request = Request("worldcell.configure");
    auto args = editor::ai::JsonValue::MakeObject();
    args.Set("typoCellSize", editor::ai::JsonValue(256));
    request.Set("args", std::move(args));
    const auto result = editor::ai::InvokeOperator(Context(), request, true);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.code, "BAD_ARG");
    EXPECT_TRUE(AttachedScene()->GetRootGameObjects().empty());
    EXPECT_EQ(m_dirtyCalls, 0);
    EXPECT_FALSE(m_undo.CanUndo());
}

TEST_F(WorldPartitionOperatorTest, PreviewAndGenerationRequireBoundedSamplesAndAPlanFingerprint)
{
    editor::OpArgs args;
    args.Set("volume", std::string("11111111-1111-4111-8111-111111111111"));
    args.Set("maxPoints", 1025);
    const auto oversized = Invoke("pcg.preview", args);
    EXPECT_FALSE(oversized.ok);
    EXPECT_EQ(oversized.errorCode, "BAD_ARG");
    args.Set("maxPoints", 256);
    const auto missingFingerprint = Invoke("pcg.generate", args);
    EXPECT_FALSE(missingFingerprint.ok);
    EXPECT_EQ(missingFingerprint.errorCode, "BAD_ARG");
    EXPECT_TRUE(AttachedScene()->GetRootGameObjects().empty());
    EXPECT_EQ(m_dirtyCalls, 0);
    EXPECT_FALSE(m_undo.CanUndo());
}

TEST_F(WorldPartitionOperatorTest, AiPartialPartitionConfigurationPreservesOmittedFields)
{
    auto request = Request("worldcell.configure");
    auto args = editor::ai::JsonValue::MakeObject();
    args.Set("manifest", editor::ai::JsonValue("Assets/World/Custom.worldcells"));
    auto origin = editor::ai::JsonValue::MakeArray();
    origin.Push(editor::ai::JsonValue(128));
    origin.Push(editor::ai::JsonValue(16));
    origin.Push(editor::ai::JsonValue(-256));
    args.Set("origin", std::move(origin));
    args.Set("cellSize", editor::ai::JsonValue(64));
    args.Set("nearDistance", editor::ai::JsonValue(32));
    args.Set("hlodDistance", editor::ai::JsonValue(512));
    args.Set("hysteresis", editor::ai::JsonValue(16));
    args.Set("maxConcurrentReads", editor::ai::JsonValue(3));
    args.Set("maxCommitsPerUpdate", editor::ai::JsonValue(2));
    args.Set("streamInEditor", editor::ai::JsonValue(true));
    request.Set("args", std::move(args));
    const auto created = editor::ai::InvokeOperator(Context(), request, false);
    ASSERT_TRUE(created.ok) << created.code;
    const auto roots = AttachedScene()->GetRootGameObjects();
    ASSERT_EQ(roots.size(), 1u);
    auto* node = roots.front();
    ASSERT_NE(node, nullptr);
    const auto* partition = node->GetComponent<scene::WorldPartitionComponent>();
    ASSERT_NE(partition, nullptr);
    const auto before = *partition;

    request = Request("worldcell.configure");
    args = editor::ai::JsonValue::MakeObject();
    args.Set("partition", editor::ai::JsonValue(node->instanceId));
    args.Set("nearDistance", editor::ai::JsonValue(48));
    request.Set("args", std::move(args));
    const auto updated = editor::ai::InvokeOperator(Context(), request, false);
    ASSERT_TRUE(updated.ok) << updated.code;
    EXPECT_FLOAT_EQ(partition->nearDistance, 48.0f);
    EXPECT_EQ(partition->manifestPath, before.manifestPath);
    EXPECT_EQ(partition->gridOrigin, before.gridOrigin);
    EXPECT_FLOAT_EQ(partition->cellSize, before.cellSize);
    EXPECT_FLOAT_EQ(partition->hlodDistance, before.hlodDistance);
    EXPECT_FLOAT_EQ(partition->hysteresis, before.hysteresis);
    EXPECT_EQ(partition->maxConcurrentLoads, before.maxConcurrentLoads);
    EXPECT_EQ(partition->maxPublishesPerFrame, before.maxPublishesPerFrame);
    EXPECT_EQ(partition->streamInEditor, before.streamInEditor);
}

TEST_F(WorldPartitionOperatorTest, UnknownStreamingActionsAreRejectedBeforeAnyWorkIsQueued)
{
    editor::OpArgs args;
    args.Set("action", std::string("load_everything"));
    const auto result = Invoke("worldcell.stream_control", args);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.errorCode, "BAD_ARG");
    EXPECT_EQ(m_dirtyCalls, 0);
    EXPECT_TRUE(AttachedScene()->GetRootGameObjects().empty());
}

TEST_F(WorldPartitionOperatorTest, AiPartialVolumeConfigurationPreservesOmittedOverrides)
{
    scene::WorldPcgProfile profile;
    scene::WorldPcgRule rule;
    rule.key = "trees";
    rule.parts = {{"primitive:cube", "guid:test-material", 0}};
    rule.lodHeights = {0.0f};
    profile.rules.push_back(std::move(rule));
    const std::string path = File("Assets/World/Forest.pcg").generic_string();
    std::string diagnostic;
    ASSERT_TRUE(scene::SaveWorldPcgProfile(path, profile, diagnostic)) << diagnostic;
    Context().editScene = AttachedScene();
    auto& node = AttachedScene()->CreateGameObject("Existing PCG Volume");
    auto& volume = node.AddComponent<scene::WorldPcgVolumeComponent>();
    volume.profilePath = path;
    volume.enabled = false;
    volume.protectManual = false;
    volume.overrideSeed = true;
    volume.seed = 17;
    volume.overrideBounds = true;
    volume.boundsMin = {16, 0, 32};
    volume.boundsMax = {64, 0, 128};
    const auto before = volume;

    auto request = Request("pcg.volume.configure");
    auto args = editor::ai::JsonValue::MakeObject();
    args.Set("volume", editor::ai::JsonValue(node.instanceId));
    args.Set("seed", editor::ai::JsonValue(35));
    request.Set("args", std::move(args));
    const auto result = editor::ai::InvokeOperator(Context(), request, false);
    ASSERT_TRUE(result.ok) << result.code;
    EXPECT_TRUE(volume.overrideSeed);
    EXPECT_EQ(volume.seed, 35);
    EXPECT_EQ(volume.enabled, before.enabled);
    EXPECT_EQ(volume.protectManual, before.protectManual);
    EXPECT_EQ(volume.overrideBounds, before.overrideBounds);
    EXPECT_EQ(volume.boundsMin, before.boundsMin);
    EXPECT_EQ(volume.boundsMax, before.boundsMax);
}

TEST_F(WorldPartitionOperatorTest, DisabledUndoPreventsMutationsWithoutBlockingInspectionAndFileActions)
{
    m_undo.SetRecordingEnabled(false);
    for (const char* id : {"worldcell.configure", "pcg.volume.configure", "pcg.generate", "pcg.detach"}) EXPECT_FALSE(Available(id)) << id;
    for (const char* id : {"worldcell.inspect", "pcg.preview", "pcg.profile.inspect", "worldcell.bake", "hlod.build", "worldcell.stream_status"}) EXPECT_TRUE(Available(id)) << id;
    const auto result = Invoke("worldcell.configure");
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.errorCode, "NOT_AVAILABLE");
    EXPECT_EQ(m_dirtyCalls, 0);
    EXPECT_TRUE(AttachedScene()->GetRootGameObjects().empty());
}

TEST_F(WorldPartitionOperatorTest, ProfileAuthoringDoesNotRequireAnOpenScene)
{
    DetachScene();
    EXPECT_TRUE(Available("pcg.profile.inspect"));
    EXPECT_TRUE(Available("pcg.profile.save"));
    EXPECT_FALSE(Available("worldcell.inspect"));
    EXPECT_FALSE(Available("worldcell.configure"));
    EXPECT_FALSE(Available("worldcell.stream_status"));
    EXPECT_FALSE(Available("worldcell.stream_control"));

    scene::WorldPcgProfile profile;
    scene::WorldPcgRule rule;
    rule.key = "trees";
    rule.parts = {{"primitive:cube", "guid:test-material", 0}};
    rule.lodHeights = {0.0f};
    profile.rules.push_back(std::move(rule));
    std::string text, diagnostic;
    ASSERT_TRUE(scene::SerializeWorldPcgProfile(profile, text, diagnostic)) << diagnostic;
    auto request = Request("pcg.profile.save");
    auto args = editor::ai::JsonValue::MakeObject();
    args.Set("path", editor::ai::JsonValue("Assets/World/NoScene.pcg"));
    args.Set("text", editor::ai::JsonValue(text));
    request.Set("args", std::move(args));
    const auto saved = editor::ai::InvokeOperator(Context(), request, false);
    ASSERT_TRUE(saved.ok) << saved.code;
    scene::WorldPcgProfile loaded;
    EXPECT_TRUE(scene::LoadWorldPcgProfile(File("Assets/World/NoScene.pcg").generic_string(), loaded, diagnostic)) << diagnostic;
    ASSERT_EQ(loaded.rules.size(), 1u);
    EXPECT_EQ(loaded.rules.front().key, "trees");
    EXPECT_FALSE(m_undo.CanUndo());
}

TEST_F(WorldPartitionOperatorTest, ManagedBuildBlocksAuthoringWhileStreamingRemainsInspectable)
{
    Context().managedBuildBusy = true;
    for (const char* id : {"worldcell.configure", "worldcell.inspect", "pcg.profile.inspect", "pcg.profile.save",
                          "pcg.volume.configure", "pcg.preview", "pcg.generate", "pcg.detach", "worldcell.bake", "hlod.build"})
        EXPECT_FALSE(Available(id)) << id;
    EXPECT_TRUE(Available("worldcell.stream_status"));
    EXPECT_TRUE(Available("worldcell.stream_control"));
    const auto result = editor::ai::InvokeOperator(Context(), Request("worldcell.configure"), false);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.code, "NOT_AVAILABLE");
    EXPECT_TRUE(AttachedScene()->GetRootGameObjects().empty());
    EXPECT_FALSE(m_undo.CanUndo());
}

TEST_F(WorldPartitionOperatorTest, PlayAllowsStreamingButBlocksAuthoringAndPendingRestoreBlocksBoth)
{
    Context().playMode = &m_play;
    m_play.Play(*AttachedScene());
    ASSERT_TRUE(m_play.IsPlaying()) << m_play.Diagnostic();
    EXPECT_FALSE(Available("worldcell.configure"));
    EXPECT_FALSE(Available("pcg.preview"));
    EXPECT_FALSE(Available("pcg.profile.save"));
    EXPECT_FALSE(Available("worldcell.bake"));
    EXPECT_FALSE(Available("hlod.build"));
    EXPECT_TRUE(Available("worldcell.stream_status"));
    EXPECT_TRUE(Available("worldcell.stream_control"));

    m_play.Stop(*AttachedScene());
    ASSERT_TRUE(m_play.HasPendingRestore());
    EXPECT_FALSE(Available("worldcell.configure"));
    EXPECT_FALSE(Available("pcg.profile.inspect"));
    EXPECT_FALSE(Available("worldcell.stream_status"));
    EXPECT_FALSE(Available("worldcell.stream_control"));
}

} /// @note namespace fbzz::tests
