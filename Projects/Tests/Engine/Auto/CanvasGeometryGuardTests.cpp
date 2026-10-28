/// @file    CanvasGeometryGuardTests.cpp
/// @brief   Preserve Canvas geometry safety and visit overlapping subtree nodes once per call.
/// @author  Hasegawa Jin
/// @date    2026-10-24
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include "../../../Engine/src/Scene/Systems/CanvasGeometryGuard.hpp"
#include <Engine/Scene/SceneSerializer.hpp>
#include <cstddef>
#include <utility>
#include <vector>

namespace fbzz::tests {

class CanvasGeometryGuardTest : public testkit::EngineFixture {
protected:
    scene::Scene m_scene;

    bool CanShare()
    {
        return scene::canvas_geometry_detail::CanShareCanvasFrameGeometry(m_scene);
    }

    bool CountEmptyProbes(std::size_t& probes)
    {
        probes = 0;
        return scene::canvas_geometry_detail::CanShareCanvasFrameGeometry(m_scene,
            [&](scene::GameObject&) { ++probes; return false; });
    }
};

TEST_F(CanvasGeometryGuardTest, NoCanvasDoesNotVisitUnrelatedGeometry)
{
    for (int index = 0; index < 128; ++index)
        m_scene.CreateGameObject("Unrelated").AddComponent<scene::MeshRenderer>();
    std::size_t probes = 0;
    EXPECT_TRUE(CountEmptyProbes(probes));
    EXPECT_EQ(probes, 0u);
    EXPECT_TRUE(CanShare());
}

TEST_F(CanvasGeometryGuardTest, DisabledAndInactiveCanvasRootsAreExcluded)
{
    auto& disabled = m_scene.CreateGameObject("Disabled");
    disabled.AddComponent<scene::UICanvas>().enabled = false;
    disabled.AddComponent<scene::MeshRenderer>();
    auto& inactive = m_scene.CreateGameObject("Inactive");
    inactive.SetActive(false);
    auto& child = m_scene.CreateGameObject("CanvasBelowInactiveParent");
    child.SetParent(inactive);
    child.AddComponent<scene::UICanvas>();
    child.AddComponent<scene::SkinnedMeshRenderer>();
    EXPECT_TRUE(CanShare());
    std::size_t probes = 0;
    EXPECT_TRUE(CountEmptyProbes(probes));
    EXPECT_EQ(probes, 0u);
    inactive.SetActive(true);
    EXPECT_FALSE(CanShare());
    inactive.SetActive(false);
    disabled.GetComponent<scene::UICanvas>()->enabled = true;
    EXPECT_FALSE(CanShare());
}

TEST_F(CanvasGeometryGuardTest, RootRendererPresenceIncludesDisabledEmptyComponents)
{
    auto& root = m_scene.CreateGameObject("Root");
    root.AddComponent<scene::UICanvas>();
    root.AddComponent<scene::MeshRenderer>().enabled = false;
    EXPECT_FALSE(CanShare());
    root.RemoveComponent<scene::MeshRenderer>();
    EXPECT_TRUE(CanShare());
    root.AddComponent<scene::SkinnedMeshRenderer>().enabled = false;
    EXPECT_FALSE(CanShare());
}

TEST_F(CanvasGeometryGuardTest, InactiveChildrenAndDisabledNestedCanvasesAreNotPruned)
{
    auto& root = m_scene.CreateGameObject("Root");
    root.AddComponent<scene::UICanvas>();
    auto& nested = m_scene.CreateGameObject("Nested");
    nested.SetParent(root);
    nested.SetActive(false);
    nested.AddComponent<scene::UICanvas>().enabled = false;
    auto& mesh = m_scene.CreateGameObject("Mesh");
    mesh.SetParent(nested);
    mesh.AddComponent<scene::SkinnedMeshRenderer>().enabled = false;
    EXPECT_FALSE(CanShare());
}

TEST_F(CanvasGeometryGuardTest, NestedCanvasesProbeEveryReachableNodeOnce)
{
    constexpr std::size_t NODE_COUNT = 128;
    std::vector<scene::GameObject*> nodes;
    for (std::size_t index = 0; index < NODE_COUNT; ++index) {
        auto& object = m_scene.CreateGameObject("Nested");
        if (!nodes.empty()) object.SetParent(*nodes.back());
        nodes.push_back(&object);
    }
    for (auto it = nodes.rbegin(); it != nodes.rend(); ++it)
        (*it)->AddComponent<scene::UICanvas>();
    for (int index = 0; index < 128; ++index)
        m_scene.CreateGameObject("OutsideUnion");
    std::size_t probes = 0;
    EXPECT_TRUE(CountEmptyProbes(probes));
    EXPECT_EQ(probes, NODE_COUNT);
    EXPECT_TRUE(CountEmptyProbes(probes));
    EXPECT_EQ(probes, NODE_COUNT);
    nodes.back()->AddComponent<scene::MeshRenderer>();
    EXPECT_FALSE(CanShare());
}

TEST_F(CanvasGeometryGuardTest, ProvenChildCannotHideGeometryInALaterAncestorRoot)
{
    auto& root = m_scene.CreateGameObject("LaterRoot");
    auto& geometry = m_scene.CreateGameObject("UnknownSibling");
    geometry.SetParent(root);
    geometry.AddComponent<scene::MeshRenderer>();
    auto& child = m_scene.CreateGameObject("ProvenChild");
    child.SetParent(root);
    child.AddComponent<scene::UICanvas>();
    root.AddComponent<scene::UICanvas>();
    std::size_t probes = 0;
    const bool shared = scene::canvas_geometry_detail::CanShareCanvasFrameGeometry(m_scene,
        [&](scene::GameObject& object) {
            ++probes;
            return object.GetComponent<scene::MeshRenderer>() || object.GetComponent<scene::SkinnedMeshRenderer>();
        });
    EXPECT_FALSE(shared);
    EXPECT_EQ(probes, 3u);
}

TEST_F(CanvasGeometryGuardTest, DisjointAndWideSubtreesVisitOnlyTheirUnion)
{
    for (int tree = 0; tree < 3; ++tree) {
        auto& root = m_scene.CreateGameObject("Root");
        root.AddComponent<scene::UICanvas>();
        for (int index = 0; index < 64; ++index) {
            auto& child = m_scene.CreateGameObject("Child");
            child.SetParent(root);
            child.AddComponent<scene::UICanvas>();
        }
    }
    std::size_t probes = 0;
    EXPECT_TRUE(CountEmptyProbes(probes));
    EXPECT_EQ(probes, 3u * 65u);
}

TEST_F(CanvasGeometryGuardTest, SameCountRendererReplacementIsObservedOnNextCall)
{
    auto& root = m_scene.CreateGameObject("Root");
    root.AddComponent<scene::UICanvas>();
    auto& child = m_scene.CreateGameObject("Child");
    child.SetParent(root);
    auto& outside = m_scene.CreateGameObject("Outside");
    outside.AddComponent<scene::MeshRenderer>();
    EXPECT_TRUE(CanShare());
    outside.RemoveComponent<scene::MeshRenderer>();
    child.AddComponent<scene::MeshRenderer>();
    EXPECT_FALSE(CanShare());
    child.RemoveComponent<scene::MeshRenderer>();
    outside.AddComponent<scene::MeshRenderer>();
    EXPECT_TRUE(CanShare());
}

TEST_F(CanvasGeometryGuardTest, ReparentingIntoAndOutOfCanvasIsObservedOnNextCall)
{
    auto& root = m_scene.CreateGameObject("Root");
    root.AddComponent<scene::UICanvas>();
    auto& mesh = m_scene.CreateGameObject("Mesh");
    mesh.AddComponent<scene::MeshRenderer>();
    EXPECT_TRUE(CanShare());
    mesh.SetParent(root);
    EXPECT_FALSE(CanShare());
    ASSERT_TRUE(mesh.ClearParent());
    EXPECT_TRUE(CanShare());
}

TEST_F(CanvasGeometryGuardTest, DeletedEntitySlotReplacementCannotInheritEarlierVisit)
{
    auto& root = m_scene.CreateGameObject("Root");
    root.AddComponent<scene::UICanvas>();
    auto& child = m_scene.CreateGameObject("Child");
    child.SetParent(root);
    const auto retired = child.GetID();
    EXPECT_TRUE(CanShare());
    ASSERT_TRUE(m_scene.DestroyGameObject(retired));
    auto& replacement = m_scene.CreateGameObject("Replacement");
    EXPECT_EQ(replacement.GetID().index, retired.index);
    EXPECT_NE(replacement.GetID().generation, retired.generation);
    replacement.SetParent(root);
    replacement.AddComponent<scene::MeshRenderer>();
    EXPECT_FALSE(CanShare());
}

TEST_F(CanvasGeometryGuardTest, ClearAndOtherScenesCannotReuseVisitedState)
{
    auto& original = m_scene.CreateGameObject("Original");
    original.AddComponent<scene::UICanvas>();
    const auto originalId = original.GetID();
    const auto generation = m_scene.GetRenderSceneGeneration();
    EXPECT_TRUE(CanShare());
    m_scene.Clear();
    auto& replacement = m_scene.CreateGameObject("Replacement");
    EXPECT_EQ(replacement.GetID(), originalId);
    EXPECT_NE(m_scene.GetRenderSceneGeneration(), generation);
    replacement.AddComponent<scene::UICanvas>();
    replacement.AddComponent<scene::SkinnedMeshRenderer>();
    EXPECT_FALSE(CanShare());
    scene::Scene other;
    auto& otherRoot = other.CreateGameObject("OtherRoot");
    otherRoot.AddComponent<scene::UICanvas>();
    EXPECT_TRUE(scene::canvas_geometry_detail::CanShareCanvasFrameGeometry(other));
    EXPECT_FALSE(CanShare());
}

TEST_F(CanvasGeometryGuardTest, SerializedSceneReplacementObservesRestoredHierarchy)
{
    auto& root = m_scene.CreateGameObject("Root");
    root.AddComponent<scene::UICanvas>();
    auto& child = m_scene.CreateGameObject("Child");
    child.SetParent(root);
    const auto snapshot = scene::SceneSerializer::SaveToText(m_scene);
    ASSERT_FALSE(snapshot.empty());
    EXPECT_TRUE(CanShare());
    child.AddComponent<scene::MeshRenderer>();
    EXPECT_FALSE(CanShare());
    auto restored = scene::SceneSerializer::LoadDataFromText(snapshot, {});
    ASSERT_NE(restored, nullptr);
    m_scene = std::move(*restored);
    EXPECT_TRUE(CanShare());
    ASSERT_EQ(m_scene.GetEntities<scene::UICanvas>().size(), 1u);
    auto* restoredRoot = m_scene.GetGameObject(m_scene.GetEntities<scene::UICanvas>().front());
    ASSERT_NE(restoredRoot, nullptr);
    ASSERT_EQ(restoredRoot->GetChildCount(), 1);
    restoredRoot->GetChild(0)->AddComponent<scene::MeshRenderer>();
    EXPECT_FALSE(CanShare());
}

TEST_F(CanvasGeometryGuardTest, WorldCanvasDepthAndSurfaceOffsetSurviveSceneRoundTrip)
{
    auto& root = m_scene.CreateGameObject("MountedDisplay");
    auto& canvas = root.AddComponent<scene::UICanvas>();
    canvas.renderMode = scene::UIRenderMode::WorldSpace;
    canvas.depthMode = scene::UICanvasDepthMode::ALWAYS_ON_TOP;
    canvas.surfaceOffset = 0.003f;
    const auto text = scene::SceneSerializer::SaveToText(m_scene);
    auto restored = scene::SceneSerializer::LoadDataFromText(text, {});
    ASSERT_NE(restored, nullptr);
    ASSERT_EQ(restored->GetEntities<scene::UICanvas>().size(), 1u);
    const auto* saved = restored->GetComponent<scene::UICanvas>(restored->GetEntities<scene::UICanvas>().front());
    ASSERT_NE(saved, nullptr);
    EXPECT_EQ(saved->depthMode, scene::UICanvasDepthMode::ALWAYS_ON_TOP);
    EXPECT_FLOAT_EQ(saved->surfaceOffset, 0.003f);
    EXPECT_EQ(saved->renderMode, scene::UIRenderMode::WorldSpace);
}

TEST_F(CanvasGeometryGuardTest, LegacyWorldCanvasUsesSceneDepthWithoutMovingItsSurface)
{
    auto& canvas = m_scene.CreateGameObject("LegacyDisplay").AddComponent<scene::UICanvas>();
    canvas.renderMode = scene::UIRenderMode::WorldSpace;
    std::string text = scene::SceneSerializer::SaveToText(m_scene);
    for (const char* field : {"depthMode", "surfaceOffset"}) {
        const auto position = text.find(field);
        ASSERT_NE(position, std::string::npos);
        const auto lineStart = text.rfind('\n', position);
        const auto lineEnd = text.find('\n', position);
        text.erase(lineStart == std::string::npos ? 0 : lineStart + 1,
            lineEnd == std::string::npos ? std::string::npos : lineEnd - (lineStart == std::string::npos ? 0 : lineStart + 1) + 1);
    }
    auto restored = scene::SceneSerializer::LoadDataFromText(text, {});
    ASSERT_NE(restored, nullptr);
    ASSERT_EQ(restored->GetEntities<scene::UICanvas>().size(), 1u);
    const auto* saved = restored->GetComponent<scene::UICanvas>(restored->GetEntities<scene::UICanvas>().front());
    ASSERT_NE(saved, nullptr);
    EXPECT_EQ(saved->depthMode, scene::UICanvasDepthMode::SCENE_DEPTH);
    EXPECT_FLOAT_EQ(saved->surfaceOffset, 0.0f);
}

} /// @note namespace fbzz::tests
