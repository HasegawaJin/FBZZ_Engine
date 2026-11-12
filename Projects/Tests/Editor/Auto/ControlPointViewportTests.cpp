/// @file    ControlPointViewportTests.cpp
/// @brief   ControlPoint viewport hit testing, drag transactions, and startup gates.
/// @author  Hasegawa Jin
/// @date    2026-10-08
#include <TestKit/TestKit.hpp>
#include <TestKit/Editor/EditorFixture.hpp>

#include <Editor/PlayModeController.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <Editor/Op/OperatorGroups.hpp>
#include <Panels/Viewport/ViewportCommon.hpp>
#include <Editor/Tools/ControlPointEditing.hpp>
#include <Editor/Util/UndoStack.hpp>
#include <Engine/Renderer/Camera.hpp>
#include <Engine/Scene/Components/NavMeshOffMeshLinkComponent.hpp>
#include <Engine/Scene/Components/NavMeshPatrolComponent.hpp>
#include <Engine/Scene/Components/PresentationComponents.hpp>
#include <Engine/Scene/Components/SplineComponents.hpp>
#include <Engine/Scene/Components/TargetPointComponent.hpp>
#include <Engine/Scene/Components/WaterComponent.hpp>
#include <Engine/Scene/ControlPointAdapters.hpp>
#include <Engine/Scene/SceneManager.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Physics/World.hpp>
#include <ImGuizmo.h>
#include <imgui.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fbzz::tests {
namespace {

class ControlPointViewportTest : public testkit::EditorFixture {
protected:
    void SetUp() override
    {
        EditorFixture::SetUp();
        (void)AttachScene();
        Context().operators = &m_registry;
        Context().undoStack = &m_undo;
        Context().editorCamera = &m_camera;
        editor::RegisterControlPointOperators(m_registry);
        editor::RegisterTargetPointMoveOperator(m_registry);
        m_camera.m_position = {0.0f, 0.0f, -10.0f};
        m_camera.m_projection = renderer::ProjectionMode::Orthographic;
        m_camera.m_orthoHeight = 10.0f;
        m_camera.m_aspect = 4.0f / 3.0f;

        m_previous = ImGui::GetCurrentContext();
        m_imgui = ImGui::CreateContext();
        ImGui::SetCurrentContext(m_imgui);
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = {800.0f, 600.0f};
        io.DeltaTime = 1.0f / 60.0f;
        /// @note 一つのFrameで指定したmouse/keyの組を同時に適用し、別種イベントのtrickleによるframe遅延を除く。
        io.ConfigInputTrickleEventQueue = false;
        unsigned char* pixels = nullptr;
        int width = 0;
        int height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        ImGuizmo::Enable(true);
    }

    void TearDown() override
    {
        m_tool.Cancel();
        ImGuizmo::Enable(false);
        ImGuizmo::Enable(true);
        m_undo.Clear();
        Context().operators = nullptr;
        Context().undoStack = nullptr;
        Context().editorCamera = nullptr;
        ImGui::DestroyContext(m_imgui);
        ImGui::SetCurrentContext(m_previous);
        EditorFixture::TearDown();
    }

    scene::GameObject& MakeMarker(const char* name, const math::Vector3& worldPosition)
    {
        auto& object = AttachedScene()->CreateGameObject(name);
        object.transform.position = worldPosition;
        object.transform.worldPosition = worldPosition;
        object.AddComponent<scene::TargetPointComponent>();
        return object;
    }

    bool Frame(float mouseX, float mouseY, bool leftDown, bool editable = true,
               bool blocked = false, bool escape = false, bool includeOverlay = false,
               const char* activationScope = nullptr, const char* activationLabel = nullptr,
               bool* overlayCaptured = nullptr, int* pointDrawVertices = nullptr)
    {
        auto& io = ImGui::GetIO();
        io.AddMousePosEvent(mouseX, mouseY);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, leftDown);
        io.AddKeyEvent(ImGuiKey_Escape, escape);
        ImGui::NewFrame();
        ImGuizmo::BeginFrame();
        ImGui::SetNextWindowPos({0.0f, 0.0f});
        ImGui::SetNextWindowSize({800.0f, 600.0f});
        ImGui::Begin("Control-point viewport test", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize
                | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
        if (includeOverlay && activationScope && activationLabel) {
            ImGui::PushID(activationScope);
            const ImGuiID id = ImGui::GetID(activationLabel);
            ImGui::PopID();
            m_imgui->NavActivateId = id;
            m_imgui->NavActivateDownId = id;
            m_imgui->NavActivatePressedId = id;
            m_imgui->NavActivateFlags = ImGuiActivateFlags_None;
        }
        const bool overlayHovered = includeOverlay
            && editor::DrawControlPointOverlay(Context(), {0.0f, 0.0f}, {800.0f, 600.0f}, editable, blocked);
        if (overlayCaptured) *overlayCaptured = overlayHovered;
        bool transformGizmoDrawnBeforePoints = false;
        if (editable && !blocked && !overlayHovered && editor::HasTargetPointSelection(Context())
            && Context().gizmoMode != editor::EditorContext::GizmoMode::Translate) {
            editor::DrawGizmo(Context(), {0.0f, 0.0f}, {800.0f, 600.0f},
                m_lastGizmoOp, m_lastGizmoMode, m_prevGizmoOver, m_prevGizmoUsing);
            transformGizmoDrawnBeforePoints = true;
        }
        const int verticesBeforePoints = ImGui::GetWindowDrawList()->VtxBuffer.Size;
        const bool claimed = m_tool.Update(Context(), {0.0f, 0.0f}, {800.0f, 600.0f},
            editable, blocked || overlayHovered, true);
        if (pointDrawVertices)
            *pointDrawVertices = ImGui::GetWindowDrawList()->VtxBuffer.Size - verticesBeforePoints;
        if (editable && !blocked && !overlayHovered && !transformGizmoDrawnBeforePoints && !claimed
            && (!editor::HasTargetPointSelection(Context())
                || Context().gizmoMode != editor::EditorContext::GizmoMode::Translate
                || m_prevGizmoUsing))
            editor::DrawGizmo(Context(), {0.0f, 0.0f}, {800.0f, 600.0f},
                m_lastGizmoOp, m_lastGizmoMode, m_prevGizmoOver, m_prevGizmoUsing);
        ImGui::End();
        ImGui::Render();
        return claimed;
    }

    void SkipViewportFrame(float mouseX, float mouseY)
    {
        ImGui::GetIO().AddMousePosEvent(mouseX, mouseY);
        ImGui::NewFrame();
        ImGuizmo::BeginFrame();
        ImGui::Render();
    }

    bool SelectCenterMarker(scene::GameObject& object, bool includeOverlay = false)
    {
        Context().selectedEntities.clear();
        const bool claimed = Frame(400.0f, 300.0f, true, true, false, false, includeOverlay);
        Frame(400.0f, 300.0f, false, true, false, false, includeOverlay);
        return claimed && Context().selectedEntities == std::vector<scene::EntityID>{object.GetID()};
    }

    bool ClickMarkerAt(scene::GameObject& object, float mouseX, float mouseY, bool clearSelection = true)
    {
        if (clearSelection) Context().selectedEntities.clear();
        const bool claimed = Frame(mouseX, mouseY, true);
        Frame(mouseX, mouseY, false);
        return claimed && Context().selectedEntities == std::vector<scene::EntityID>{object.GetID()};
    }

    int BeginGizmoDrag(scene::GameObject& object, bool includeOverlay = false)
    {
        if (!SelectCenterMarker(object, includeOverlay)) return -1;
        for (int offset = 13; offset <= 64; ++offset) {
            Frame(400.0f + static_cast<float>(offset), 300.0f, true, true, false, false, includeOverlay);
            if (Context().controlPointMouseClaimFrame == ImGui::GetFrameCount()) return offset;
            Frame(400.0f + static_cast<float>(offset), 300.0f, false, true, false, false, includeOverlay);
        }
        return -1;
    }

    ImVec2 FindTransformGizmoHotspot()
    {
        constexpr float kPi = 3.14159265358979323846f;
        for (int radius = 16; radius <= 120; radius += 4) {
            for (int step = 0; step < 32; ++step) {
                const float angle = 2.0f * kPi * static_cast<float>(step) / 32.0f;
                const ImVec2 point{400.0f + static_cast<float>(radius) * std::cos(angle),
                                   300.0f + static_cast<float>(radius) * std::sin(angle)};
                Frame(point.x, point.y, false);
                if (m_prevGizmoOver) return point;
            }
        }
        return {-1.0f, -1.0f};
    }

    static void MakeStaleSplineBake(scene::Scene& world, scene::EntityID ownerId,
                                    scene::SplineComponent& spline)
    {
        spline.points = {{0.0f, 0.0f, 0.0f}, {4.0f, 0.0f, 0.0f}};
        spline.pointRuntimeMode = scene::ControlPointRuntimeMode::LIVE;
        const auto bake = scene::ResolveSplinePointAuthoring(world, ownerId, spline,
            scene::ControlPointRuntimeMode::BAKED_STATIC);
        spline.bakedPoints = bake.points;
        spline.bakedInputSignature = bake.inputSignature;
        spline.bakedSourceMode = spline.pointSourceMode;
        spline.bakedAdapterVersion = scene::CONTROL_POINT_ADAPTER_VERSION;
        spline.bakedClosed = spline.closed;
        spline.bakeValid = true;
        spline.points[1].x = 5.0f;
        spline.pointRuntimeMode = scene::ControlPointRuntimeMode::BAKED_STATIC;
    }

    editor::UndoStack m_undo;
    editor::OperatorRegistry m_registry;
    renderer::Camera m_camera;
    editor::ControlPointViewportTool m_tool;
    int m_lastGizmoOp = -1;
    int m_lastGizmoMode = -1;
    bool m_prevGizmoOver = false;
    bool m_prevGizmoUsing = false;
    ImGuiContext* m_previous = nullptr;
    ImGuiContext* m_imgui = nullptr;
};

TEST_F(ControlPointViewportTest, MarkerClickClaimsSelectionAndOverlayOrReadOnlyFramesDoNotSelect)
{
    auto& marker = MakeMarker("Target Point", {0.0f, 0.0f, 0.0f});
    EXPECT_TRUE(SelectCenterMarker(marker));

    Context().selectedEntities.clear();
    EXPECT_FALSE(Frame(400.0f, 300.0f, true, true, true));
    EXPECT_TRUE(Context().selectedEntities.empty());
    Frame(400.0f, 300.0f, false);

    EXPECT_FALSE(Frame(400.0f, 300.0f, true, false));
    EXPECT_TRUE(Context().selectedEntities.empty());
    Frame(400.0f, 300.0f, false);

    EXPECT_FALSE(Frame(850.0f, 300.0f, true));
    EXPECT_TRUE(Context().selectedEntities.empty());
    Frame(850.0f, 300.0f, false);
}

TEST_F(ControlPointViewportTest, BehindNearFarOutsideAndNonfiniteMarkersAreNotPicked)
{
    const std::array<math::Vector3, 5> invalidPositions{{
        {0.0f, 0.0f, -20.0f},
        {0.0f, 0.0f, -9.95f},
        {0.0f, 0.0f, 1005.0f},
        {30.0f, 0.0f, 0.0f},
        {(std::numeric_limits<float>::infinity)(), 0.0f, 0.0f},
    }};
    m_camera.m_far = 1000.0f;
    int index = 0;
    for (const auto& position : invalidPositions)
        MakeMarker(("Invalid " + std::to_string(index++)).c_str(), position);

    EXPECT_FALSE(Frame(400.0f, 300.0f, true));
    EXPECT_TRUE(Context().selectedEntities.empty());
    Frame(400.0f, 300.0f, false);
}

TEST_F(ControlPointViewportTest, GizmoDragPreviewsWithoutMutationThenCommitsOneUndo)
{
    auto& marker = MakeMarker("Target Point", {0.0f, 0.0f, 0.0f});
    const int offset = BeginGizmoDrag(marker);
    ASSERT_GE(offset, 0);

    const math::Vector3 initial = marker.transform.position;
    Frame(400.0f + static_cast<float>(offset + 20), 300.0f, true);
    EXPECT_VEC3_NEAR(marker.transform.position, initial, testkit::kTolerance);
    Frame(400.0f + static_cast<float>(offset + 20), 300.0f, false);
    EXPECT_GT((marker.transform.position - initial).LengthSq(), 1e-8f);
    ASSERT_EQ(m_undo.GetHistorySize(), 1u);
    EXPECT_TRUE(m_undo.Undo());
    EXPECT_VEC3_NEAR(marker.transform.position, initial, testkit::kTolerance);
}

TEST_F(ControlPointViewportTest, RotateAndScaleModesUseTheTransformGizmoForTargetPoints)
{
    auto& marker = MakeMarker("Target Point", {0.0f, 0.0f, 0.0f});
    marker.transform.scale = {-1.25f, 0.8f, 1.5f};
    scene::FlushWorldTransforms(*AttachedScene());
    ASSERT_TRUE(SelectCenterMarker(marker));

    const std::array modes{editor::EditorContext::GizmoMode::Rotate,
                           editor::EditorContext::GizmoMode::Scale};
    for (const auto mode : modes) {
        m_undo.Clear();
        Context().gizmoMode = mode;
        const ImVec2 start = FindTransformGizmoHotspot();
        ASSERT_GE(start.x, 0.0f);
        const scene::Transform before = marker.transform;
        EXPECT_FALSE(Frame(start.x, start.y, true));
        EXPECT_TRUE(m_prevGizmoUsing);
        const float dx = start.x - 400.0f;
        const float dy = start.y - 300.0f;
        float endX = start.x + 24.0f;
        float endY = start.y + 12.0f;
        if (mode == editor::EditorContext::GizmoMode::Rotate) {
            constexpr float kTurn = 0.45f;
            const float cosine = std::cos(kTurn);
            const float sine = std::sin(kTurn);
            endX = 400.0f + dx * cosine - dy * sine;
            endY = 300.0f + dx * sine + dy * cosine;
        }
        Frame(endX, endY, true);
        Frame(endX, endY, false);

        if (mode == editor::EditorContext::GizmoMode::Rotate) {
            const float difference = std::fabs(marker.transform.rotation.x - before.rotation.x)
                + std::fabs(marker.transform.rotation.y - before.rotation.y)
                + std::fabs(marker.transform.rotation.z - before.rotation.z)
                + std::fabs(marker.transform.rotation.w - before.rotation.w);
            EXPECT_GT(difference, 1e-4f);
            EXPECT_VEC3_NEAR(marker.transform.position, before.position, testkit::kTolerance);
            EXPECT_VEC3_NEAR(marker.transform.scale, before.scale, testkit::kTolerance);
        } else {
            EXPECT_GT((marker.transform.scale - before.scale).LengthSq(), 1e-8f);
            EXPECT_VEC3_NEAR(marker.transform.position, before.position, testkit::kTolerance);
            EXPECT_NEAR(marker.transform.rotation.x, before.rotation.x, testkit::kTolerance);
            EXPECT_NEAR(marker.transform.rotation.y, before.rotation.y, testkit::kTolerance);
            EXPECT_NEAR(marker.transform.rotation.z, before.rotation.z, testkit::kTolerance);
            EXPECT_NEAR(marker.transform.rotation.w, before.rotation.w, testkit::kTolerance);
        }
        ASSERT_EQ(m_undo.GetHistorySize(), 1u);
        EXPECT_TRUE(m_undo.Undo());
        EXPECT_VEC3_NEAR(marker.transform.position, before.position, testkit::kTolerance);
        EXPECT_VEC3_NEAR(marker.transform.scale, before.scale, testkit::kTolerance);
        EXPECT_NEAR(marker.transform.rotation.x, before.rotation.x, testkit::kTolerance);
        EXPECT_NEAR(marker.transform.rotation.y, before.rotation.y, testkit::kTolerance);
        EXPECT_NEAR(marker.transform.rotation.z, before.rotation.z, testkit::kTolerance);
        EXPECT_NEAR(marker.transform.rotation.w, before.rotation.w, testkit::kTolerance);
        Context().gizmoMode = editor::EditorContext::GizmoMode::Translate;
        Frame(400.0f, 300.0f, false);
    }
}

TEST_F(ControlPointViewportTest, ChangingModeCancelsAnInFlightTargetPointTransformGizmo)
{
    auto& marker = MakeMarker("Target Point", {0.0f, 0.0f, 0.0f});
    ASSERT_TRUE(SelectCenterMarker(marker));
    Context().gizmoMode = editor::EditorContext::GizmoMode::Rotate;
    const ImVec2 start = FindTransformGizmoHotspot();
    ASSERT_GE(start.x, 0.0f);
    const scene::Transform before = marker.transform;

    EXPECT_FALSE(Frame(start.x, start.y, true));
    EXPECT_TRUE(m_prevGizmoUsing);
    Frame(start.x - 20.0f, start.y + 20.0f, true);
    const float previewDifference = std::fabs(marker.transform.rotation.x - before.rotation.x)
        + std::fabs(marker.transform.rotation.y - before.rotation.y)
        + std::fabs(marker.transform.rotation.z - before.rotation.z)
        + std::fabs(marker.transform.rotation.w - before.rotation.w);
    Context().gizmoMode = editor::EditorContext::GizmoMode::Translate;
    Frame(start.x - 20.0f, start.y + 20.0f, true);

    EXPECT_GT(previewDifference, 1e-4f);
    EXPECT_NEAR(marker.transform.rotation.x, before.rotation.x, testkit::kTolerance);
    EXPECT_NEAR(marker.transform.rotation.y, before.rotation.y, testkit::kTolerance);
    EXPECT_NEAR(marker.transform.rotation.z, before.rotation.z, testkit::kTolerance);
    EXPECT_NEAR(marker.transform.rotation.w, before.rotation.w, testkit::kTolerance);
    EXPECT_EQ(m_undo.GetHistorySize(), 0u);
    Frame(start.x - 20.0f, start.y + 20.0f, false);
}

TEST_F(ControlPointViewportTest, LockedTargetPointCannotStartOrContinueTransformGizmo)
{
    auto& marker = MakeMarker("Target Point", {0.0f, 0.0f, 0.0f});
    ASSERT_TRUE(SelectCenterMarker(marker));
    Context().gizmoMode = editor::EditorContext::GizmoMode::Rotate;
    ImVec2 start = FindTransformGizmoHotspot();
    ASSERT_GE(start.x, 0.0f);
    const scene::Transform before = marker.transform;

    Context().lockedEntities.push_back(marker.GetID());
    EXPECT_FALSE(Frame(start.x, start.y, true));
    EXPECT_FALSE(m_prevGizmoUsing);
    EXPECT_FALSE(m_prevGizmoOver);
    Frame(start.x, start.y, false);
    EXPECT_VEC3_NEAR(marker.transform.position, before.position, testkit::kTolerance);
    EXPECT_NEAR(marker.transform.rotation.w, before.rotation.w, testkit::kTolerance);
    EXPECT_EQ(m_undo.GetHistorySize(), 0u);

    Context().lockedEntities.clear();
    start = FindTransformGizmoHotspot();
    ASSERT_GE(start.x, 0.0f);
    EXPECT_FALSE(Frame(start.x, start.y, true));
    ASSERT_TRUE(m_prevGizmoUsing);
    const float dx = start.x - 400.0f;
    const float dy = start.y - 300.0f;
    constexpr float kTurn = 0.45f;
    const float endX = 400.0f + dx * std::cos(kTurn) - dy * std::sin(kTurn);
    const float endY = 300.0f + dx * std::sin(kTurn) + dy * std::cos(kTurn);
    Frame(endX, endY, true);
    const scene::Transform preview = marker.transform;
    EXPECT_GT(std::fabs(preview.rotation.x - before.rotation.x)
        + std::fabs(preview.rotation.y - before.rotation.y)
        + std::fabs(preview.rotation.z - before.rotation.z)
        + std::fabs(preview.rotation.w - before.rotation.w), 1e-4f);

    Context().lockedEntities.push_back(marker.GetID());
    Frame(endX, endY, true);
    EXPECT_FALSE(m_prevGizmoUsing);
    EXPECT_FALSE(m_prevGizmoOver);
    EXPECT_NEAR(marker.transform.rotation.x, before.rotation.x, testkit::kTolerance);
    EXPECT_NEAR(marker.transform.rotation.y, before.rotation.y, testkit::kTolerance);
    EXPECT_NEAR(marker.transform.rotation.z, before.rotation.z, testkit::kTolerance);
    EXPECT_NEAR(marker.transform.rotation.w, before.rotation.w, testkit::kTolerance);
    EXPECT_EQ(m_undo.GetHistorySize(), 0u);
    Frame(endX, endY, false);
}

TEST_F(ControlPointViewportTest, CenterScaleHandleWinsOverTheCoincidentMarkerClick)
{
    auto& marker = MakeMarker("Target Point", {0.0f, 0.0f, 0.0f});
    marker.transform.scale = {-1.25f, 0.8f, 1.5f};
    scene::FlushWorldTransforms(*AttachedScene());
    ASSERT_TRUE(SelectCenterMarker(marker));
    Context().gizmoMode = editor::EditorContext::GizmoMode::Scale;
    Frame(400.0f, 300.0f, false);
    ASSERT_TRUE(m_prevGizmoOver);
    const scene::Transform before = marker.transform;

    EXPECT_FALSE(Frame(400.0f, 300.0f, true));
    EXPECT_TRUE(m_prevGizmoUsing);
    Frame(420.0f, 300.0f, true);
    Frame(420.0f, 300.0f, false);

    EXPECT_GT((marker.transform.scale - before.scale).LengthSq(), 1e-8f);
    EXPECT_VEC3_NEAR(marker.transform.position, before.position, testkit::kTolerance);
    EXPECT_NEAR(marker.transform.rotation.x, before.rotation.x, testkit::kTolerance);
    EXPECT_NEAR(marker.transform.rotation.y, before.rotation.y, testkit::kTolerance);
    EXPECT_NEAR(marker.transform.rotation.z, before.rotation.z, testkit::kTolerance);
    EXPECT_NEAR(marker.transform.rotation.w, before.rotation.w, testkit::kTolerance);
    EXPECT_LT(marker.transform.scale.x, 0.0f);
    ASSERT_EQ(m_undo.GetHistorySize(), 1u);
    EXPECT_TRUE(m_undo.Undo());
    EXPECT_VEC3_NEAR(marker.transform.scale, before.scale, testkit::kTolerance);
}

TEST_F(ControlPointViewportTest, FrameGapCancelsAnUncommittedGizmoCandidate)
{
    auto& marker = MakeMarker("Target Point", {0.0f, 0.0f, 0.0f});
    const int offset = BeginGizmoDrag(marker);
    ASSERT_GE(offset, 0);
    const math::Vector3 initial = marker.transform.position;

    SkipViewportFrame(400.0f + static_cast<float>(offset), 300.0f);
    EXPECT_TRUE(Frame(400.0f + static_cast<float>(offset + 12), 300.0f, true));
    EXPECT_VEC3_NEAR(marker.transform.position, initial, testkit::kTolerance);
    EXPECT_EQ(m_undo.GetHistorySize(), 0u);
    Frame(400.0f + static_cast<float>(offset + 12), 300.0f, false);
}

TEST_F(ControlPointViewportTest, EscapeAndSceneReplacementCancelAnUncommittedGizmoCandidate)
{
    auto& marker = MakeMarker("Target Point", {0.0f, 0.0f, 0.0f});
    const int offset = BeginGizmoDrag(marker);
    ASSERT_GE(offset, 0);
    const math::Vector3 initial = marker.transform.position;
    EXPECT_TRUE(Frame(400.0f + static_cast<float>(offset + 8), 300.0f, true, true, false, true));
    EXPECT_VEC3_NEAR(marker.transform.position, initial, testkit::kTolerance);
    EXPECT_EQ(m_undo.GetHistorySize(), 0u);
    Frame(400.0f + static_cast<float>(offset + 8), 300.0f, false);

    const int nextOffset = BeginGizmoDrag(marker);
    ASSERT_GE(nextOffset, 0);
    scene::Scene replacement;
    scene::Scene* original = Context().activeScene;
    Context().activeScene = &replacement;
    EXPECT_TRUE(Frame(400.0f + static_cast<float>(nextOffset + 8), 300.0f, true));
    EXPECT_VEC3_NEAR(marker.transform.position, initial, testkit::kTolerance);
    EXPECT_EQ(m_undo.GetHistorySize(), 0u);
    Context().activeScene = original;
    Frame(400.0f + static_cast<float>(nextOffset + 8), 300.0f, false);
}

TEST_F(ControlPointViewportTest, ReleasingOutsideViewportCancelsTheGizmoCandidate)
{
    auto& marker = MakeMarker("Target Point", {0.0f, 0.0f, 0.0f});
    const int offset = BeginGizmoDrag(marker);
    ASSERT_GE(offset, 0);
    const math::Vector3 initial = marker.transform.position;

    EXPECT_TRUE(Frame(850.0f, 300.0f, false));
    EXPECT_VEC3_NEAR(marker.transform.position, initial, testkit::kTolerance);
    EXPECT_EQ(m_undo.GetHistorySize(), 0u);
}

TEST_F(ControlPointViewportTest, ClickingInlineRiverPointSelectsTheCorrespondingGizmoTarget)
{
    auto& owner = AttachedScene()->CreateGameObject("River");
    auto& water = owner.AddComponent<scene::WaterComponent>();
    water.surfaceShape = scene::WaterSurfaceShape::SPLINE_RIVER;
    water.splinePoints = {{41, {-2.0f, 0.0f, 0.0f}, 2.0f, 1.0f, 1.0f},
        {17, {2.0f, 0.0f, 2.0f}, 3.0f, 1.0f, 1.0f}};
    Context().selectedEntities = {owner.GetID()};
    Context().gizmoMode = editor::EditorContext::GizmoMode::Rotate;
    Frame(520.0f, 300.0f, false);
    EXPECT_EQ(water.pointSourceMode, scene::ControlPointSourceMode::INLINE);
    EXPECT_EQ(m_undo.GetHistorySize(), 0u);
    ASSERT_TRUE(Frame(520.0f, 300.0f, true));
    ASSERT_EQ(water.pointSourceMode, scene::ControlPointSourceMode::POINT_OBJECTS);
    ASSERT_EQ(water.pointBindings.size(), 2u);
    const auto* selected = Context().GetSelectedGO();
    ASSERT_NE(selected, nullptr);
    EXPECT_EQ(selected->instanceId, water.pointBindings[1].target.guid);
    EXPECT_EQ(water.pointBindings[1].id, 17u);
    EXPECT_EQ(Context().gizmoMode, editor::EditorContext::GizmoMode::Translate);
    EXPECT_EQ(Context().controlPointOwnerGuid, owner.instanceId);
    EXPECT_EQ(m_undo.GetHistorySize(), 1u);
    ASSERT_TRUE(m_undo.Undo());
    EXPECT_EQ(water.pointSourceMode, scene::ControlPointSourceMode::INLINE);
}

TEST_F(ControlPointViewportTest, SceneOverlayConvertsInlineOwnersStartsEditingAndSupportsPointGizmoUndo)
{
    auto runConversion = [this](scene::GameObject& owner, const char* kind) {
        const std::size_t originalCount = AttachedScene()->GameObjectCount();
        m_undo.Clear();
        Context().controlPointOwnerGuid.clear();
        Context().selectedEntities = {owner.GetID()};
        bool overlayCaptured = false;
        Frame(20.0f, 50.0f, false, true, false, false, true,
            kind, "TargetPointへ変換", &overlayCaptured);

        EXPECT_TRUE(overlayCaptured);
        EXPECT_EQ(Context().controlPointOwnerGuid, owner.instanceId);
        std::string targetGuid;
        if (auto* water = owner.GetComponent<scene::WaterComponent>()) {
            EXPECT_EQ(water->pointSourceMode, scene::ControlPointSourceMode::POINT_OBJECTS);
            ASSERT_FALSE(water->pointBindings.empty());
            targetGuid = water->pointBindings.front().target.guid;
        } else if (auto* spline = owner.GetComponent<scene::SplineComponent>()) {
            EXPECT_EQ(spline->pointSourceMode, scene::ControlPointSourceMode::POINT_OBJECTS);
            ASSERT_FALSE(spline->pointBindings.empty());
            targetGuid = spline->pointBindings.front().target.guid;
        } else if (auto* line = owner.GetComponent<scene::LineRendererComponent>()) {
            EXPECT_EQ(line->pointSourceMode, scene::ControlPointSourceMode::POINT_OBJECTS);
            ASSERT_FALSE(line->pointBindings.empty());
            targetGuid = line->pointBindings.front().target.guid;
        } else if (auto* patrol = owner.GetComponent<scene::NavMeshPatrolComponent>()) {
            EXPECT_EQ(patrol->pointSourceMode, scene::ControlPointSourceMode::POINT_OBJECTS);
            ASSERT_FALSE(patrol->waypointBindings.empty());
            targetGuid = patrol->waypointBindings.front().target.guid;
        } else if (auto* link = owner.GetComponent<scene::NavMeshOffMeshLinkComponent>()) {
            EXPECT_EQ(link->pointSourceMode, scene::ControlPointSourceMode::POINT_OBJECTS);
            targetGuid = link->startTarget.guid;
        }
        ASSERT_FALSE(targetGuid.empty());
        auto* target = AttachedScene()->FindByGuid(targetGuid);
        ASSERT_NE(target, nullptr);
        ASSERT_NE(target->GetComponent<scene::TargetPointComponent>(), nullptr);
        ASSERT_EQ(m_undo.GetHistorySize(), 1u);

        ASSERT_TRUE(SelectCenterMarker(*target, true));
        EXPECT_TRUE(editor::HasTargetPointSelection(Context()));
        const math::Vector3 initialPosition = target->transform.position;
        const int offset = BeginGizmoDrag(*target, true);
        ASSERT_GE(offset, 0);
        Frame(400.0f + static_cast<float>(offset + 20), 300.0f, true,
            true, false, false, true);
        Frame(400.0f + static_cast<float>(offset + 20), 300.0f, false,
            true, false, false, true);
        EXPECT_GT((target->transform.position - initialPosition).LengthSq(), 1e-8f);
        ASSERT_EQ(m_undo.GetHistorySize(), 2u);
        ASSERT_TRUE(m_undo.Undo()) << m_undo.LastDiagnostic();
        EXPECT_VEC3_NEAR(target->transform.position, initialPosition, testkit::kTolerance);
        ASSERT_TRUE(m_undo.Undo()) << m_undo.LastDiagnostic();
        EXPECT_EQ(AttachedScene()->FindByGuid(targetGuid), nullptr);
        EXPECT_EQ(AttachedScene()->GameObjectCount(), originalCount);
        if (auto* water = owner.GetComponent<scene::WaterComponent>())
            EXPECT_EQ(water->pointSourceMode, scene::ControlPointSourceMode::INLINE);
        else if (auto* spline = owner.GetComponent<scene::SplineComponent>())
            EXPECT_EQ(spline->pointSourceMode, scene::ControlPointSourceMode::INLINE);
        else if (auto* line = owner.GetComponent<scene::LineRendererComponent>())
            EXPECT_EQ(line->pointSourceMode, scene::ControlPointSourceMode::INLINE);
        else if (auto* patrol = owner.GetComponent<scene::NavMeshPatrolComponent>())
            EXPECT_EQ(patrol->pointSourceMode, scene::ControlPointSourceMode::INLINE);
        else if (auto* link = owner.GetComponent<scene::NavMeshOffMeshLinkComponent>())
            EXPECT_EQ(link->pointSourceMode, scene::ControlPointSourceMode::INLINE);
    };

    auto& waterOwner = AttachedScene()->CreateGameObject("Water River");
    auto& water = waterOwner.AddComponent<scene::WaterComponent>();
    water.surfaceShape = scene::WaterSurfaceShape::SPLINE_RIVER;
    water.splinePoints = {{1, {0.0f, 0.0f, 0.0f}, 2.0f, 1.0f, 0.0f},
                          {2, {2.0f, 0.0f, 0.0f}, 2.0f, 1.0f, 0.0f}};
    runConversion(waterOwner, "water");

    auto& splineOwner = AttachedScene()->CreateGameObject("Spline");
    auto& spline = splineOwner.AddComponent<scene::SplineComponent>();
    spline.points = {{0.0f, 0.0f, 0.0f}, {2.0f, 0.0f, 0.0f}};
    runConversion(splineOwner, "spline");

    auto& lineOwner = AttachedScene()->CreateGameObject("Line");
    auto& line = lineOwner.AddComponent<scene::LineRendererComponent>();
    line.points = {{0.0f, 0.0f, 0.0f}, {2.0f, 0.0f, 0.0f}};
    runConversion(lineOwner, "line");

    auto& patrolOwner = AttachedScene()->CreateGameObject("Patrol");
    auto& patrol = patrolOwner.AddComponent<scene::NavMeshPatrolComponent>();
    patrol.waypoints = {{0.0f, 0.0f, 0.0f}, {2.0f, 0.0f, 0.0f}};
    runConversion(patrolOwner, "patrol");

    auto& linkOwner = AttachedScene()->CreateGameObject("Off-Mesh Link");
    auto& link = linkOwner.AddComponent<scene::NavMeshOffMeshLinkComponent>();
    link.startPoint = {0.0f, 0.0f, 0.0f};
    link.endPoint = {2.0f, 0.0f, 0.0f};
    runConversion(linkOwner, "nav_link");
}

TEST_F(ControlPointViewportTest, BlockedOverlayKeepsPointDrawingAndCancelsInFlightCandidate)
{
    auto& owner = AttachedScene()->CreateGameObject("Editable Water");
    auto& water = owner.AddComponent<scene::WaterComponent>();
    water.surfaceShape = scene::WaterSurfaceShape::SPLINE_RIVER;
    water.pointSourceMode = scene::ControlPointSourceMode::POINT_OBJECTS;
    auto& first = MakeMarker("River Point A", {0.0f, 0.0f, 0.0f});
    auto& second = MakeMarker("River Point B", {2.0f, 0.0f, 0.0f});
    for (auto* target : {&first, &second}) {
        scene::WaterControlPointBinding binding;
        binding.id = target == &first ? 1u : 2u;
        binding.target.id = target->GetID();
        binding.target.guid = target->instanceId;
        binding.target.sceneGeneration = AttachedScene()->GetRenderSceneGeneration();
        binding.width = 2.0f;
        binding.depth = 1.0f;
        water.pointBindings.push_back(binding);
    }
    scene::FlushWorldTransforms(*AttachedScene());
    Context().controlPointOwnerGuid = owner.instanceId;
    Context().selectedEntities = {owner.GetID()};

    bool overlayCaptured = false;
    int pointDrawVertices = 0;
    EXPECT_FALSE(Frame(20.0f, 50.0f, true, true, false, false, true,
        nullptr, nullptr, &overlayCaptured, &pointDrawVertices));
    EXPECT_TRUE(overlayCaptured);
    EXPECT_GT(pointDrawVertices, 0);
    EXPECT_EQ(Context().selectedEntities, (std::vector<scene::EntityID>{owner.GetID()}));
    Frame(20.0f, 50.0f, false, true, false, false, true);

    const int offset = BeginGizmoDrag(first, true);
    ASSERT_GE(offset, 0);
    const math::Vector3 before = first.transform.position;
    Frame(400.0f + static_cast<float>(offset + 16), 300.0f, true,
        true, false, false, true);
    EXPECT_VEC3_NEAR(first.transform.position, before, testkit::kTolerance);
    EXPECT_EQ(m_undo.GetHistorySize(), 0u);

    EXPECT_TRUE(Frame(20.0f, 50.0f, true, true, false, false, true,
        nullptr, nullptr, &overlayCaptured, &pointDrawVertices));
    EXPECT_TRUE(overlayCaptured);
    EXPECT_GT(pointDrawVertices, 0);
    EXPECT_VEC3_NEAR(first.transform.position, before, testkit::kTolerance);
    EXPECT_EQ(m_undo.GetHistorySize(), 0u);
    Frame(20.0f, 50.0f, false, true, false, false, true);

    int hiddenPointVertices = 0;
    Frame(20.0f, 50.0f, true, false, false, false, true,
        nullptr, nullptr, &overlayCaptured, &hiddenPointVertices);
    EXPECT_EQ(hiddenPointVertices, 0);
    EXPECT_EQ(Context().selectedEntities, (std::vector<scene::EntityID>{first.GetID()}));
    Frame(20.0f, 50.0f, false, false, false, false, true);
}

TEST_F(ControlPointViewportTest, OverlayCaptureEndsWhenMouseReleasedWhileCardIsNotDrawn)
{
    auto& owner = AttachedScene()->CreateGameObject("Spline Owner");
    owner.AddComponent<scene::SplineComponent>().points = {{0.0f, 0.0f, 0.0f}, {2.0f, 0.0f, 0.0f}};
    Context().selectedEntities = {owner.GetID()};
    bool overlayCaptured = false;
    Frame(20.0f, 50.0f, true, true, false, false, true,
        nullptr, nullptr, &overlayCaptured);
    ASSERT_TRUE(overlayCaptured);
    Frame(750.0f, 550.0f, false, true, false, false, false);
    Frame(750.0f, 550.0f, false, true, false, false, true,
        nullptr, nullptr, &overlayCaptured);
    EXPECT_FALSE(overlayCaptured);
    EXPECT_EQ(m_undo.GetHistorySize(), 0u);
}

TEST_F(ControlPointViewportTest, InvalidInlinePointsCannotBeConvertedFromTheSceneOverlay)
{
    auto& owner = AttachedScene()->CreateGameObject("Invalid River");
    auto& water = owner.AddComponent<scene::WaterComponent>();
    water.surfaceShape = scene::WaterSurfaceShape::SPLINE_RIVER;
    water.splinePoints = {{1, {0.0f, 0.0f, 0.0f}, 2.0f, 1.0f, 0.0f}};
    Context().selectedEntities = {owner.GetID()};
    const std::size_t originalCount = AttachedScene()->GameObjectCount();
    bool overlayCaptured = false;

    Frame(20.0f, 50.0f, false, true, false, false, true,
        "water", "TargetPointへ変換", &overlayCaptured);

    EXPECT_TRUE(overlayCaptured);
    EXPECT_EQ(water.pointSourceMode, scene::ControlPointSourceMode::INLINE);
    EXPECT_TRUE(water.pointBindings.empty());
    EXPECT_TRUE(Context().controlPointOwnerGuid.empty());
    EXPECT_EQ(AttachedScene()->GameObjectCount(), originalCount);
    EXPECT_EQ(m_undo.GetHistorySize(), 0u);
}

TEST_F(ControlPointViewportTest, WorldLineAndPatrolPointClicksKeepIndependentOwnerRoutesVisible)
{
    const auto convertAndSelect = [this](scene::GameObject& owner, const char* kind,
                                         const char* componentKind, float firstScreenX) {
        editor::OpArgs args;
        args.Set("node", owner.instanceId);
        args.Set("kind", std::string(kind));
        args.Set("source", std::string("POINT_OBJECTS"));
        const auto converted = editor::InvokeOperator(Context(), "control_points.convert", args);
        ASSERT_TRUE(converted.ok) << converted.errorCode << ": " << converted.message;

        std::string firstGuid;
        if (std::string_view(componentKind) == "line") {
            auto* line = owner.GetComponent<scene::LineRendererComponent>();
            ASSERT_NE(line, nullptr);
            ASSERT_EQ(line->pointSourceMode, scene::ControlPointSourceMode::POINT_OBJECTS);
            ASSERT_EQ(line->pointBindings.size(), 2u);
            firstGuid = line->pointBindings.front().target.guid;
        } else {
            auto* patrol = owner.GetComponent<scene::NavMeshPatrolComponent>();
            ASSERT_NE(patrol, nullptr);
            ASSERT_EQ(patrol->pointSourceMode, scene::ControlPointSourceMode::POINT_OBJECTS);
            ASSERT_EQ(patrol->waypointBindings.size(), 2u);
            firstGuid = patrol->waypointBindings.front().target.guid;
        }
        ASSERT_FALSE(firstGuid.empty());
        auto* first = AttachedScene()->FindByGuid(firstGuid);
        ASSERT_NE(first, nullptr);
        ASSERT_NE(first->GetParent(), nullptr);
        EXPECT_EQ(first->GetParent()->GetParent(), nullptr);
        EXPECT_NE(first->GetParent(), &owner);
        const auto containerId = first->GetParent()->GetID();
        ASSERT_NE(first->GetComponent<scene::TargetPointComponent>(), nullptr);

        Context().controlPointOwnerGuid.clear();
        Context().selectedEntities = {owner.GetID()};
        int withRouteVertices = 0;
        Frame(750.0f, 550.0f, false, true, false, false, false,
            nullptr, nullptr, nullptr, &withRouteVertices);
        Context().selectedEntities.clear();
        int markerOnlyVertices = 0;
        Frame(750.0f, 550.0f, false, true, false, false, false,
            nullptr, nullptr, nullptr, &markerOnlyVertices);
        EXPECT_GT(withRouteVertices, markerOnlyVertices);

        Context().selectedEntities = {owner.GetID()};
        ASSERT_TRUE(ClickMarkerAt(*first, firstScreenX, 300.0f, false));
        EXPECT_EQ(Context().controlPointOwnerGuid, owner.instanceId);
        ASSERT_NE(first->GetParent(), nullptr);
        EXPECT_EQ(first->GetParent()->GetID(), containerId);
        EXPECT_EQ(first->GetParent()->GetParent(), nullptr);

        int afterClickVertices = 0;
        Frame(750.0f, 550.0f, false, true, false, false, false,
            nullptr, nullptr, nullptr, &afterClickVertices);
        EXPECT_GT(afterClickVertices, markerOnlyVertices);
    };

    auto& lineOwner = AttachedScene()->CreateGameObject("World Line");
    auto& line = lineOwner.AddComponent<scene::LineRendererComponent>();
    line.space = scene::LineSpace::World;
    line.points = {{0.0f, 0.0f, 0.0f}, {2.0f, 0.0f, 0.0f}};
    convertAndSelect(lineOwner, "line", "line", 400.0f);

    Context().controlPointOwnerGuid.clear();
    Context().selectedEntities.clear();
    auto& patrolOwner = AttachedScene()->CreateGameObject("World Patrol");
    auto& patrol = patrolOwner.AddComponent<scene::NavMeshPatrolComponent>();
    patrol.waypoints = {{4.0f, 0.0f, 0.0f}, {6.0f, 0.0f, 0.0f}};
    convertAndSelect(patrolOwner, "patrol", "patrol", 640.0f);
}

TEST_F(ControlPointViewportTest, OwnerLocalWaterAndSplinePointsRemainDistinctAtLargeWorldOrigins)
{
    constexpr float kOriginX = 100000000.0f;
    constexpr float kWorldScaleX = 4.0f;
    constexpr float kFirstScreenX = 430.0f;
    constexpr float kSecondScreenX = 460.0f;

    m_camera.m_position = {kOriginX, 0.0f, -10.0f};

    const auto createChildMarker = [this](scene::GameObject& owner, const char* name,
                                          const math::Vector3& localPosition) -> scene::GameObject& {
        auto& marker = AttachedScene()->CreateGameObject(name);
        EXPECT_TRUE(marker.SetParent(&owner));
        marker.transform.position = localPosition;
        marker.AddComponent<scene::TargetPointComponent>();
        return marker;
    };

    const auto assignWaterRow = [this](scene::WaterComponent& water,
                                       scene::GameObject& marker, std::uint32_t stableId) {
        scene::WaterControlPointBinding binding;
        binding.id = stableId;
        binding.target.id = marker.GetID();
        binding.target.guid = marker.instanceId;
        binding.target.sceneGeneration = AttachedScene()->GetRenderSceneGeneration();
        binding.width = 1.0f;
        binding.depth = 1.0f;
        water.pointBindings.push_back(binding);
    };

    auto& waterOwner = AttachedScene()->CreateGameObject("Large-Origin Water");
    waterOwner.transform.position = {kOriginX, 0.0f, 0.0f};
    waterOwner.transform.scale = {kWorldScaleX, 1.0f, 1.0f};
    auto& water = waterOwner.AddComponent<scene::WaterComponent>();
    water.surfaceShape = scene::WaterSurfaceShape::SPLINE_RIVER;
    water.pointSourceMode = scene::ControlPointSourceMode::POINT_OBJECTS;
    auto& waterFirst = createChildMarker(waterOwner, "Water Point 0.125", {0.125f, 0.0f, 0.0f});
    auto& waterSecond = createChildMarker(waterOwner, "Water Point 0.25", {0.25f, 0.0f, 0.0f});
    assignWaterRow(water, waterFirst, 1u);
    assignWaterRow(water, waterSecond, 2u);

    auto& splineOwner = AttachedScene()->CreateGameObject("Large-Origin Spline");
    splineOwner.transform.position = {kOriginX, 3.0f, 0.0f};
    splineOwner.transform.scale = {kWorldScaleX, 1.0f, 1.0f};
    auto& spline = splineOwner.AddComponent<scene::SplineComponent>();
    spline.pointSourceMode = scene::ControlPointSourceMode::POINT_OBJECTS;
    auto& splineFirst = createChildMarker(splineOwner, "Spline Point 0.125", {0.125f, 0.0f, 0.0f});
    auto& splineSecond = createChildMarker(splineOwner, "Spline Point 0.25", {0.25f, 0.0f, 0.0f});
    for (const auto& [marker, stableId] : std::array<std::pair<scene::GameObject*, std::uint32_t>, 2>{
             std::pair{&splineFirst, 1u}, std::pair{&splineSecond, 2u}}) {
        scene::SplineControlPointBinding binding;
        binding.id = stableId;
        binding.target.id = marker->GetID();
        binding.target.guid = marker->instanceId;
        binding.target.sceneGeneration = AttachedScene()->GetRenderSceneGeneration();
        spline.pointBindings.push_back(binding);
    }
    scene::FlushWorldTransforms(*AttachedScene());

    const auto selectPair = [this, kFirstScreenX, kSecondScreenX](scene::GameObject& owner, scene::GameObject& first,
                                   scene::GameObject& second, float y) {
        Context().controlPointOwnerGuid.clear();
        Context().selectedEntities = {owner.GetID()};
        ASSERT_TRUE(ClickMarkerAt(first, kFirstScreenX, y, false));
        EXPECT_EQ(Context().controlPointOwnerGuid, owner.instanceId);
        ASSERT_TRUE(ClickMarkerAt(second, kSecondScreenX, y));
        EXPECT_EQ(Context().selectedEntities, (std::vector<scene::EntityID>{second.GetID()}));
        EXPECT_EQ(Context().controlPointOwnerGuid, owner.instanceId);
    };

    selectPair(waterOwner, waterFirst, waterSecond, 300.0f);
    selectPair(splineOwner, splineFirst, splineSecond, 120.0f);
}

TEST_F(ControlPointViewportTest, PlayRejectsStaleStaticBakeWithoutParkingOrCloningEditScene)
{
    auto& owner = AttachedScene()->CreateGameObject("Stale Spline");
    auto& spline = owner.AddComponent<scene::SplineComponent>();
    MakeStaleSplineBake(*AttachedScene(), owner.GetID(), spline);
    const auto ownerId = owner.GetID();
    const std::string ownerGuid = owner.instanceId;

    editor::PlayModeController play;
    play.Play(*AttachedScene());

    EXPECT_TRUE(play.IsInEditor());
    EXPECT_EQ(play.PlayScene(), nullptr);
    EXPECT_FALSE(AttachedScene()->IsExecutionParked());
    EXPECT_EQ(AttachedScene()->GetGameObject(ownerId), &owner);
    EXPECT_EQ(AttachedScene()->FindByGuid(ownerGuid), &owner);
    EXPECT_FALSE(play.Diagnostic().empty());
}

TEST_F(ControlPointViewportTest, ValidPlayUsesAnIndependentSceneClone)
{
    auto& owner = AttachedScene()->CreateGameObject("Clone Boundary");
    owner.transform.position = {3.0f, 4.0f, 5.0f};
    const scene::EntityID ownerId = owner.GetID();
    const std::string ownerGuid = owner.instanceId;
    scene::FlushWorldTransforms(*AttachedScene());

    editor::PlayModeController play;
    play.Play(*AttachedScene());

    ASSERT_TRUE(play.IsPlaying()) << play.Diagnostic();
    ASSERT_NE(play.PlayScene(), AttachedScene());
    ASSERT_NE(play.PlayScene()->FindByGuid(ownerGuid), nullptr);
    EXPECT_EQ(AttachedScene()->GetGameObject(ownerId), &owner);
    EXPECT_VEC3_NEAR(play.PlayScene()->FindByGuid(ownerGuid)->transform.position,
        (math::Vector3{3.0f, 4.0f, 5.0f}), testkit::kTolerance);
    play.Cancel();
    EXPECT_FALSE(AttachedScene()->IsExecutionParked());
}

TEST_F(ControlPointViewportTest, SceneManagerStartupGateSkipsRuntimeSystemsForStaleBake)
{
    auto& owner = AttachedScene()->CreateGameObject("Stale Spline");
    auto& spline = owner.AddComponent<scene::SplineComponent>();
    MakeStaleSplineBake(*AttachedScene(), owner.GetID(), spline);
    auto& waterOwner = AttachedScene()->CreateGameObject("Live River");
    auto& water = waterOwner.AddComponent<scene::WaterComponent>();
    water.surfaceShape = scene::WaterSurfaceShape::SPLINE_RIVER;
    water.splinePoints = {
        {1, {0.0f, 0.0f, 0.0f}, 2.0f, 1.0f, 0.0f},
        {2, {10.0f, 0.0f, 0.0f}, 2.0f, 1.0f, 0.0f},
    };
    ASSERT_FALSE(scene::ValidateStaticControlPointBakes(*AttachedScene()));
    ASSERT_EQ(water.splineSnapshot, nullptr);

    scene::SceneManager manager;
    manager.SetScene(AttachedScene());
    manager.SetPlaying(true);
    manager.SetSimulating(true);
    physics::World physicsWorld;
    manager.Update(1.0f / 60.0f, physicsWorld);

    EXPECT_EQ(water.splineSnapshot, nullptr);
}

} /// @note namespace
} /// @note namespace fbzz::tests
