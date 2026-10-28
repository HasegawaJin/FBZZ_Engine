/// @file    SpatialDebugVisualizationTests.cpp
/// @brief   Spatial diagnostics opt-in, independent contents, and public operator argument contracts.
/// @author  Hasegawa Jin
/// @date    2026-10-24
#include <TestKit/TestKit.hpp>
#include <TestKit/Editor/EditorFixture.hpp>
#include <Editor/Op/OperatorGroups.hpp>
#include <Editor/Util/SpatialDebugVisualization.hpp>
#include <Editor/Util/SpatialDebugOverlay.hpp>
#include <Engine/Scene/SpatialDebugSnapshot.hpp>
#include <Graphics/Renderer/RenderBvhDebug.hpp>
#include <limits>
#include <memory>

namespace fbzz::tests {

class SpatialDebugVisualizationTest : public testkit::EditorFixture {
protected:
    editor::OperatorRegistry m_registry;
    editor::UndoStack m_undo;

    void SetUp() override
    {
        EditorFixture::SetUp();
        editor::RegisterRenderOperators(m_registry);
        Context().operators = &m_registry;
        Context().undoStack = &m_undo;
    }

    editor::OpResult Configure(const editor::OpArgs& args)
    {
        return editor::InvokeOperator(Context(), "render.set_spatial_debug", args);
    }
};

TEST_F(SpatialDebugVisualizationTest, DefaultsRequestNoCaptureAndDisabledContentsNeverRequestCopies)
{
    auto& state = Context().spatialDebugVisualization;
    EXPECT_FALSE(state.render.AnyCapture());
    EXPECT_FALSE(state.physics.AnyCapture());
    EXPECT_EQ(state.target, editor::SpatialDebugTarget::PHYSICS);
    state.physics.proxyBounds = true;
    state.physics.candidatePairs = true;
    EXPECT_FALSE(state.physics.CapturesHierarchy());
    EXPECT_FALSE(state.physics.CapturesProxyBounds());
    EXPECT_FALSE(state.physics.CapturesCandidatePairs());
    EXPECT_FALSE(state.physics.AnyCapture());
    state.physics.enabled = true;
    EXPECT_TRUE(state.physics.CapturesHierarchy());
    EXPECT_TRUE(state.physics.CapturesProxyBounds());
    EXPECT_TRUE(state.physics.CapturesCandidatePairs());
    state.physics.hierarchy = false;
    state.physics.proxyBounds = false;
    state.physics.candidatePairs = false;
    EXPECT_FALSE(state.physics.AnyCapture());
}

TEST_F(SpatialDebugVisualizationTest, UiClampsDepthOrderAndHardDrawBudget)
{
    editor::SpatialDebugTargetSettings settings;
    settings.minDepth = 100;
    settings.maxDepth = -2;
    settings.maxDrawCommands = 50000;
    settings.Clamp();
    EXPECT_EQ(settings.minDepth, 64);
    EXPECT_EQ(settings.maxDepth, 64);
    EXPECT_EQ(settings.maxDrawCommands, 4096);
    settings.minDepth = -5;
    settings.maxDepth = 5;
    settings.maxDrawCommands = 0;
    settings.Clamp();
    EXPECT_EQ(settings.minDepth, 0);
    EXPECT_EQ(settings.maxDepth, 5);
    EXPECT_EQ(settings.maxDrawCommands, 1);
}

TEST_F(SpatialDebugVisualizationTest, PublicOperatorComposesPhysicsContentsAndKeepsRenderIndependentWithoutSceneOrUndo)
{
    editor::OpArgs physics;
    physics.Set("target", std::string("physics"));
    physics.Set("enabled", true);
    physics.Set("hierarchy", false);
    physics.Set("proxy_bounds", true);
    physics.Set("candidate_pairs", true);
    physics.Set("selected_only", true);
    physics.Set("min_depth", 1);
    physics.Set("max_depth", 6);
    physics.Set("max_draw_commands", 40);
    ASSERT_TRUE(Configure(physics).ok);
    editor::OpArgs render;
    render.Set("target", std::string("render"));
    render.Set("enabled", true);
    ASSERT_TRUE(Configure(render).ok);
    const auto& state = Context().spatialDebugVisualization;
    EXPECT_TRUE(state.render.CapturesHierarchy());
    EXPECT_FALSE(state.physics.CapturesHierarchy());
    EXPECT_TRUE(state.physics.CapturesProxyBounds());
    EXPECT_TRUE(state.physics.CapturesCandidatePairs());
    EXPECT_TRUE(state.physics.selectedOnly);
    EXPECT_EQ(state.physics.minDepth, 1);
    EXPECT_EQ(state.physics.maxDepth, 6);
    EXPECT_EQ(state.physics.maxDrawCommands, 40);
    EXPECT_EQ(state.target, editor::SpatialDebugTarget::RENDER);
    EXPECT_EQ(Context().activeScene, nullptr);
    EXPECT_EQ(m_undo.GetHistorySize(), 0u);
}

TEST_F(SpatialDebugVisualizationTest, InvalidArgumentsLeaveAllSettingsUntouched)
{
    auto& state = Context().spatialDebugVisualization;
    editor::OpArgs reversed;
    reversed.Set("target", std::string("physics"));
    reversed.Set("enabled", true);
    reversed.Set("min_depth", 12);
    reversed.Set("max_depth", 4);
    EXPECT_FALSE(Configure(reversed).ok);
    EXPECT_FALSE(state.physics.enabled);
    EXPECT_EQ(state.physics.minDepth, 0);
    editor::OpArgs unsupported;
    unsupported.Set("target", std::string("render"));
    unsupported.Set("enabled", true);
    unsupported.Set("candidate_pairs", true);
    EXPECT_FALSE(Configure(unsupported).ok);
    EXPECT_FALSE(state.render.enabled);
    EXPECT_FALSE(state.render.candidatePairs);
    editor::OpArgs overBudget;
    overBudget.Set("target", std::string("physics"));
    overBudget.Set("enabled", true);
    overBudget.Set("max_draw_commands", 4097);
    EXPECT_FALSE(Configure(overBudget).ok);
    EXPECT_FALSE(state.physics.enabled);
    EXPECT_EQ(state.physics.maxDrawCommands, 512);
}

TEST_F(SpatialDebugVisualizationTest, DebugMenuTogglesPreserveOtherTargetAndDisabledContents)
{
    editor::OpArgs on;
    on.Set("enabled", true);
    ASSERT_TRUE(editor::InvokeOperator(Context(), "render.show_render_bvh", on).ok);
    ASSERT_TRUE(editor::InvokeOperator(Context(), "render.show_physics_broadphase", on).ok);
    auto& state = Context().spatialDebugVisualization;
    state.physics.proxyBounds = true;
    state.physics.candidatePairs = true;
    editor::OpArgs off;
    off.Set("enabled", false);
    ASSERT_TRUE(editor::InvokeOperator(Context(), "render.show_physics_broadphase", off).ok);
    EXPECT_TRUE(state.render.AnyCapture());
    EXPECT_FALSE(state.physics.AnyCapture());
    EXPECT_TRUE(state.physics.proxyBounds);
    EXPECT_TRUE(state.physics.candidatePairs);
    ASSERT_TRUE(editor::InvokeOperator(Context(), "render.show_render_bvh", off).ok);
    EXPECT_FALSE(state.render.AnyCapture());
    EXPECT_FALSE(state.physics.AnyCapture());
}

TEST_F(SpatialDebugVisualizationTest, StatusQueryReportsSettingsAndActualCountsWithoutMutation)
{
    auto& state = Context().spatialDebugVisualization;
    state.renderStatus = "BVH unavailable: linear production mode";
    state.physicsDrawCommands = 9;
    const auto* op = m_registry.Find("render.get_spatial_debug");
    ASSERT_NE(op, nullptr);
    EXPECT_EQ(op->kind, editor::OpKind::Query);
    const auto result = editor::InvokeOperator(Context(), "render.get_spatial_debug");
    ASSERT_TRUE(result.ok);
    const auto* render = result.data.Find("render");
    const auto* physics = result.data.Find("physics");
    ASSERT_NE(render, nullptr);
    ASSERT_NE(physics, nullptr);
    ASSERT_NE(render->Find("status"), nullptr);
    EXPECT_EQ(render->Find("status")->AsString(), state.renderStatus);
    EXPECT_EQ(physics->Find("draw_commands")->AsInt(), 9);
    EXPECT_FALSE(state.render.enabled);
    EXPECT_EQ(state.physicsDrawCommands, 9u);
    EXPECT_EQ(m_undo.GetHistorySize(), 0u);
}

namespace {
renderer::RenderBvhDebugSnapshot RenderSnapshot()
{
    renderer::RenderBvhDebugSnapshot snapshot;
    snapshot.availability = renderer::RenderBvhDebugAvailability::READY;
    snapshot.sceneGeneration = 17;
    snapshot.snapshotSerial = 1;
    snapshot.contextFrameStamp = 100;
    snapshot.viewOutputId = 3;
    snapshot.viewOutputGeneration = 4;
    snapshot.shadowVolumeCount = 1;
    renderer::RenderBvhDebugNode node;
    node.low = {0, 0, 0};
    node.high = {2, 2, 2};
    snapshot.nodes.push_back(node);
    return snapshot;
}

editor::SpatialDebugOverlayContext OverlayContext()
{
    editor::SpatialDebugOverlayContext context;
    context.sceneGeneration = 17;
    context.currentResourceFrameStamp = 100;
    context.expectedPhysicsRevision = 8;
    context.outputId = 3;
    context.outputGeneration = 4;
    return context;
}
} /// @note namespace

TEST_F(SpatialDebugVisualizationTest, RenderBuilderRejectsWrongSceneViewFutureAndStaleSnapshots)
{
    auto snapshot = RenderSnapshot();
    auto context = OverlayContext();
    editor::SpatialDebugTargetSettings settings;
    settings.enabled = true;
    std::vector<scene::ScriptDebugDrawCommand> commands;
    snapshot.sceneGeneration = 16;
    EXPECT_FALSE(editor::BuildRenderBvhDebugCommands(snapshot, settings, context, commands).snapshotAccepted);
    snapshot.sceneGeneration = 17;
    snapshot.viewOutputGeneration = 5;
    EXPECT_FALSE(editor::BuildRenderBvhDebugCommands(snapshot, settings, context, commands).snapshotAccepted);
    snapshot.viewOutputGeneration = 4;
    context.currentResourceFrameStamp = 99;
    EXPECT_FALSE(editor::BuildRenderBvhDebugCommands(snapshot, settings, context, commands).snapshotAccepted);
    context.currentResourceFrameStamp = 103;
    EXPECT_FALSE(editor::BuildRenderBvhDebugCommands(snapshot, settings, context, commands).snapshotAccepted);
    EXPECT_TRUE(commands.empty());
    context.currentResourceFrameStamp = 102;
    EXPECT_TRUE(editor::BuildRenderBvhDebugCommands(snapshot, settings, context, commands).snapshotAccepted);
    ASSERT_EQ(commands.size(), 1u);
}

TEST_F(SpatialDebugVisualizationTest, RenderBuilderPreservesObservedColorsAndRejectsInvalidBoundsWithinBudget)
{
    auto snapshot = RenderSnapshot();
    snapshot.nodes.front().state = renderer::RenderBvhDebugState::REJECTED;
    auto invalid = snapshot.nodes.front();
    invalid.high.x = std::numeric_limits<float>::infinity();
    snapshot.nodes.push_back(invalid);
    auto unobserved = snapshot.nodes.front();
    unobserved.state = renderer::RenderBvhDebugState::UNOBSERVED;
    unobserved.partition = renderer::RenderBvhDebugPartition::DYNAMIC;
    snapshot.nodes.push_back(unobserved);
    snapshot.nodes.push_back(unobserved);
    editor::SpatialDebugTargetSettings settings;
    settings.enabled = true;
    settings.maxDrawCommands = 2;
    settings.depthTest = true;
    std::vector<scene::ScriptDebugDrawCommand> commands;
    const auto result = editor::BuildRenderBvhDebugCommands(snapshot, settings, OverlayContext(), commands);
    ASSERT_EQ(commands.size(), 2u);
    EXPECT_TRUE(result.traversalStatusAvailable);
    EXPECT_NE(result.status.find("DRAW BUDGET TRUNCATED"), std::string::npos);
    EXPECT_NE(result.status.find("invalid bounds skipped 1"), std::string::npos);
    EXPECT_EQ(commands[0].color, math::Vector4(1.0f, 0.3f, 0.3f, 1.0f));
    EXPECT_EQ(commands[1].color, math::Vector4(1.0f, 0.65f, 0.2f, 1.0f));
    EXPECT_TRUE(commands[0].depthTest);
    EXPECT_EQ(commands[0].type, scene::ScriptDebugDrawType::Box);
    EXPECT_VEC3_NEAR(commands[0].a, (math::Vector3{1, 1, 1}), 1e-6f);
}

TEST_F(SpatialDebugVisualizationTest, RenderBuilderReportsUnavailableAndSelectionLookupTruncationWithoutFabricatedGeometry)
{
    auto snapshot = RenderSnapshot();
    editor::SpatialDebugTargetSettings settings;
    settings.enabled = true;
    std::vector<scene::ScriptDebugDrawCommand> commands;
    snapshot.availability = renderer::RenderBvhDebugAvailability::LINEAR_MODE;
    const auto linear = editor::BuildRenderBvhDebugCommands(snapshot, settings, OverlayContext(), commands);
    EXPECT_FALSE(linear.snapshotAccepted);
    EXPECT_NE(linear.status.find("linear"), std::string::npos);
    EXPECT_TRUE(commands.empty());
    snapshot.availability = renderer::RenderBvhDebugAvailability::READY;
    snapshot.selectedLookupTruncated = true;
    settings.selectedOnly = true;
    const auto selected = editor::BuildRenderBvhDebugCommands(snapshot, settings, OverlayContext(), commands);
    EXPECT_NE(selected.status.find("lookup budget reached"), std::string::npos);
    EXPECT_TRUE(commands.empty());
}

TEST_F(SpatialDebugVisualizationTest, PhysicsBuilderCombinesContentsWithinOneBudgetAndChecksRequestLifetime)
{
    auto broadphase = std::make_shared<physics::BroadPhaseDebugSnapshot>();
    broadphase->worldEpoch = 2;
    broadphase->sequence = 3;
    broadphase->structure = physics::BroadPhaseDebugStructure::BVH;
    broadphase->request.enabled = true;
    broadphase->candidatePairsAvailable = true;
    physics::BroadPhaseDebugNode node;
    node.bounds = {{0, 0, 0}, {2, 2, 2}};
    broadphase->nodes.push_back(node);
    physics::BroadPhaseDebugProxy proxy;
    proxy.bounds = node.bounds;
    broadphase->proxies.push_back(proxy);
    physics::BroadPhaseDebugPair pair;
    pair.boundsA = node.bounds;
    pair.boundsB = {{2, 2, 2}, {4, 4, 4}};
    broadphase->pairs.push_back(pair);
    scene::PhysicsSpatialDebugSnapshot snapshot;
    snapshot.sceneGeneration = 17;
    snapshot.revision = 8;
    snapshot.physics = broadphase;
    editor::SpatialDebugTargetSettings settings;
    settings.enabled = true;
    settings.proxyBounds = true;
    settings.candidatePairs = true;
    settings.maxDrawCommands = 2;
    std::vector<scene::ScriptDebugDrawCommand> commands;
    auto result = editor::BuildPhysicsSpatialDebugCommands(snapshot, settings, OverlayContext(), commands);
    EXPECT_TRUE(result.snapshotAccepted);
    EXPECT_FALSE(result.traversalStatusAvailable);
    EXPECT_EQ(commands.size(), 2u);
    EXPECT_NE(result.status.find("DRAW BUDGET TRUNCATED"), std::string::npos);
    commands.clear();
    settings.hierarchy = false;
    settings.proxyBounds = false;
    result = editor::BuildPhysicsSpatialDebugCommands(snapshot, settings, OverlayContext(), commands);
    ASSERT_EQ(commands.size(), 1u);
    EXPECT_EQ(commands.front().type, scene::ScriptDebugDrawType::Line);
    EXPECT_VEC3_NEAR(commands.front().a, (math::Vector3{1, 1, 1}), 1e-6f);
    EXPECT_VEC3_NEAR(commands.front().b, (math::Vector3{3, 3, 3}), 1e-6f);
    commands.clear();
    snapshot.revision = 7;
    EXPECT_FALSE(editor::BuildPhysicsSpatialDebugCommands(snapshot, settings, OverlayContext(), commands).snapshotAccepted);
    EXPECT_TRUE(commands.empty());
    snapshot.revision = 8;
    broadphase->candidatePairsAvailable = false;
    result = editor::BuildPhysicsSpatialDebugCommands(snapshot, settings, OverlayContext(), commands);
    EXPECT_TRUE(commands.empty());
    EXPECT_NE(result.status.find("pairs unavailable"), std::string::npos);
    settings.selectedOnly = true;
    broadphase->selectionValid = false;
    broadphase->workLimitReached = true;
    result = editor::BuildPhysicsSpatialDebugCommands(snapshot, settings, OverlayContext(), commands);
    EXPECT_TRUE(commands.empty());
    EXPECT_NE(result.status.find("lookup budget reached"), std::string::npos);
}

TEST_F(SpatialDebugVisualizationTest, RenderBuilderEnforcesGlobalBudgetAndOffBuildAddsNothing)
{
    auto snapshot = RenderSnapshot();
    snapshot.nodes.push_back(snapshot.nodes.front());
    editor::SpatialDebugTargetSettings settings;
    std::vector<scene::ScriptDebugDrawCommand> commands;
    EXPECT_FALSE(editor::BuildRenderBvhDebugCommands(snapshot, settings, OverlayContext(), commands).snapshotAccepted);
    EXPECT_TRUE(commands.empty());
    settings.enabled = true;
    commands.resize(8191);
    const auto result = editor::BuildRenderBvhDebugCommands(snapshot, settings, OverlayContext(), commands);
    EXPECT_EQ(commands.size(), 8192u);
    EXPECT_EQ(result.drawCommands, 1u);
    EXPECT_NE(result.status.find("DRAW BUDGET TRUNCATED"), std::string::npos);
}

TEST_F(SpatialDebugVisualizationTest, DisabledShadowsRejectFreshRenderSnapshotAndPreservePhysicsCommands)
{
    const auto snapshot = RenderSnapshot();
    auto context = OverlayContext();
    context.shadowQueriesEnabled = false;
    editor::SpatialDebugTargetSettings settings;
    settings.enabled = true;
    scene::ScriptDebugDrawCommand physicsCommand;
    physicsCommand.type = scene::ScriptDebugDrawType::Line;
    physicsCommand.a = {1, 2, 3};
    physicsCommand.b = {4, 5, 6};
    std::vector<scene::ScriptDebugDrawCommand> commands{physicsCommand};
    const auto result = editor::BuildRenderBvhDebugCommands(snapshot, settings, context, commands);
    EXPECT_FALSE(result.snapshotAccepted);
    EXPECT_FALSE(result.traversalStatusAvailable);
    EXPECT_EQ(result.drawCommands, 0u);
    EXPECT_EQ(result.status, "No production shadow query: shadows disabled in Scene View");
    ASSERT_EQ(commands.size(), 1u);
    EXPECT_EQ(commands.front().type, physicsCommand.type);
    EXPECT_VEC3_NEAR(commands.front().a, physicsCommand.a, 1e-6f);
    EXPECT_VEC3_NEAR(commands.front().b, physicsCommand.b, 1e-6f);
}

} /// @note namespace fbzz::tests
