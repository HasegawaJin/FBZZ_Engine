/// @file    StaticMeshRenderReuseTests.cpp
/// @brief   静的Meshの再利用・通知・公開フィールド照合とsnapshotの独立性を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-23
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include "../Fixtures/StaticMeshRenderReuseFixture.hpp"
#include "../../../Engine/src/Scene/Systems/RenderShadowPreparation.hpp"
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <cmath>

namespace fbzz::tests {
TEST_F(StaticMeshRenderReuseTest, ReusesUnchangedGeometryAndRebuildsOnlyChangedInputs)
{
    auto& first = Object(m_scene);
    auto& second = Object(m_scene);
    const auto original = Extract(100);
    EXPECT_EQ(Rebuilds(), 2u);

    const auto unchanged = Extract(101);
    EXPECT_EQ(Rebuilds(), 0u);
    ASSERT_EQ(unchanged.items.size(), 2u);
    EXPECT_VEC3_NEAR(unchanged.items[0].boundsCenter, (math::Vector3{1, 0, 0}), testkit::kTolerance);
    first.transform.worldPosition.x = 2;
    scene::NotifyMeshRendererChanged(m_scene, first.GetID());
    const auto notified = Extract(102);
    EXPECT_EQ(Rebuilds(), 1u);
    EXPECT_FLOAT_EQ(notified.Objects()[0].world.m[0][3], 2);
    EXPECT_EQ(notified.items[1].vertexBuffer, m_mesh.vertexBuffer);

    second.transform.worldPosition = {7, 0, 0};
    const auto changed = Extract(103);
    EXPECT_EQ(Rebuilds(), 1u);
    EXPECT_VEC3_NEAR(changed.items[1].boundsCenter, (math::Vector3{8, 0, 0}), testkit::kTolerance);
    EXPECT_VEC3_NEAR(original.items[1].boundsCenter, (math::Vector3{1, 0, 0}), testkit::kTolerance);
}

TEST_F(StaticMeshRenderReuseTest, SceneBoundsObserveTypeMembershipAndDirectTerrainHeightWrites)
{
    const auto bounds = [&] { return scene::shadow_preparation_detail::ComputeSceneBounds(m_scene, Layer::Everything); };
    EXPECT_FALSE(bounds().valid);
    m_mesh.boundsCenter = {0.5f, 0, 0.5f}; m_mesh.boundsRadius = 1;
    auto& object = Object(m_scene);
    object.transform.worldPosition = {-0.5f, 0, -0.5f};
    const auto staticOnly = bounds();
    ASSERT_TRUE(staticOnly.valid);
    EXPECT_VEC3_NEAR(staticOnly.center, math::Vector3::ZERO, testkit::kTolerance);
    EXPECT_FLOAT_EQ(staticOnly.radius, 1);
    auto& terrain = object.AddComponent<scene::TerrainComponent>();
    terrain.columns = terrain.rows = 2; terrain.cellSize = 1; terrain.maxHeight = 2;
    terrain.heightData = {0, 0, 0, 0};
    EXPECT_FLOAT_EQ(bounds().radius, 1);
    const auto revision = terrain.surfaceRevision;
    terrain.heightData[0] = -1; terrain.heightData[1] = 1;
    const auto edited = bounds();
    EXPECT_EQ(terrain.surfaceRevision, revision);
    ASSERT_TRUE(edited.valid);
    EXPECT_VEC3_NEAR(edited.center, math::Vector3::ZERO, testkit::kTolerance);
    EXPECT_NEAR(edited.radius, std::sqrt(4.5f), testkit::kTolerance);
    object.RemoveComponent<scene::TerrainComponent>();
    EXPECT_FLOAT_EQ(bounds().radius, 1);
    object.RemoveComponent<scene::MeshRenderer>();
    EXPECT_FALSE(bounds().valid);
    object.AddComponent<scene::MeshRenderer>().mesh = &m_mesh;
    EXPECT_FLOAT_EQ(bounds().radius, 1);
    const auto retired = object.GetID();
    ASSERT_TRUE(m_scene.DestroyGameObject(retired));
    EXPECT_FALSE(bounds().valid);
    Object(m_scene).transform.worldPosition = {9.5f, 0, -0.5f};
    const auto replacement = bounds();
    ASSERT_TRUE(replacement.valid);
    EXPECT_VEC3_NEAR(replacement.center, (math::Vector3{10, 0, 0}), testkit::kTolerance);
    EXPECT_FLOAT_EQ(replacement.radius, 1);
}

TEST_F(StaticMeshRenderReuseTest, SceneBoundsRespectCurrentLayerActivationAndRendererEnabledState)
{
    auto& object = Object(m_scene);
    object.layer = 3;
    const auto bounds = [&](LayerMask mask) { return scene::shadow_preparation_detail::ComputeSceneBounds(m_scene, mask); };
    const LayerMask selected = uint32_t{1} << 3;
    EXPECT_FALSE(bounds(uint32_t{1} << 4).valid);
    ASSERT_TRUE(bounds(selected).valid);
    auto& mesh = *object.GetComponent<scene::MeshRenderer>();
    mesh.enabled = false;
    EXPECT_FALSE(bounds(selected).valid);
    mesh.enabled = true;
    auto& parent = m_scene.CreateGameObject("Bounds parent");
    object.SetParent(parent);
    parent.SetActive(false);
    EXPECT_FALSE(bounds(selected).valid);
    parent.SetActive(true);
    ASSERT_TRUE(bounds(selected).valid);
    object.layer = 4;
    EXPECT_FALSE(bounds(selected).valid);
    EXPECT_TRUE(bounds(uint32_t{1} << 4).valid);
}

TEST_F(StaticMeshRenderReuseTest, SceneBoundsKeepMixedRendererSceneOrder)
{
    m_mesh.boundsCenter = math::Vector3::ZERO; m_mesh.boundsRadius = 1;
    Object(m_scene);
    auto& terrainObject = m_scene.CreateGameObject("Middle terrain");
    auto& terrain = terrainObject.AddComponent<scene::TerrainComponent>();
    terrain.columns = terrain.rows = 2; terrain.cellSize = std::sqrt(2.0f); terrain.maxHeight = 1;
    terrain.heightData = {0, 0, 0, 0};
    terrainObject.transform.worldPosition = {4 - terrain.cellSize * 0.5f, 0, -terrain.cellSize * 0.5f};
    Object(m_scene).transform.worldPosition = {0, 4, 0};
    const auto bounds = scene::shadow_preparation_detail::ComputeSceneBounds(m_scene, Layer::Everything);
    ASSERT_TRUE(bounds.valid);
    EXPECT_VEC3_NEAR(bounds.center, (math::Vector3{1.4472136f, 1.1055728f, 0}), testkit::kTolerance);
    EXPECT_NEAR(bounds.radius, 4.236068f, testkit::kTolerance);
}

TEST_F(StaticMeshRenderReuseTest, ObservesRawPoseMeshAndPresentationWritesWithoutNotifications)
{
    auto& object = Object(m_scene);
    const auto original = Extract(10);
    object.transform.position = {90, 0, 0};
    object.transform.worldPosition = {3, 4, 5};
    object.transform.worldScale = {-2, 3, 1};
    object.layer = 7;
    auto& mesh = *object.GetComponent<scene::MeshRenderer>();
    mesh.castShadows = false; mesh.lodVisible = false; mesh.lodDither = 0.25f;
    const auto moved = Extract(11);
    ASSERT_EQ(moved.Objects().size(), 1u);
    const auto& value = moved.Objects()[0];
    EXPECT_EQ(Rebuilds(), 1u);
    EXPECT_VEC3_NEAR(value.boundsCenter, (math::Vector3{1, 4, 5}), testkit::kTolerance);
    EXPECT_FLOAT_EQ(value.boundsRadius, 6);
    EXPECT_FLOAT_EQ(value.worldInvTranspose.m[0][0], -0.5f);
    EXPECT_NEAR(value.worldInvTranspose.m[1][1], 1.0f / 3, testkit::kTolerance);
    EXPECT_VEC3_NEAR(value.sortPosition, (math::Vector3{90, 0, 0}), testkit::kTolerance);
    EXPECT_EQ(value.layer, 7u); EXPECT_FALSE(value.castShadows); EXPECT_FALSE(value.lodVisible);
    EXPECT_FLOAT_EQ(value.lodDither, 0.25f);
    EXPECT_VEC3_NEAR(original.items[0].boundsCenter, (math::Vector3{1, 0, 0}), testkit::kTolerance);

    m_mesh.vertexBuffer.gen = 9; m_mesh.indexBuffer = {52, 7};
    m_mesh.vertexCount = 10; m_mesh.indexCount = 12;
    m_mesh.boundsCenter = {0, 1, 0}; m_mesh.boundsRadius = 4; m_mesh.boundsExtents = {0.01f, 4, 4};
    const auto edited = Extract(12);
    EXPECT_EQ(Rebuilds(), 1u);
    EXPECT_EQ(edited.items[0].vertexBuffer, m_mesh.vertexBuffer);
    EXPECT_EQ(edited.items[0].indexBuffer, m_mesh.indexBuffer);
    EXPECT_EQ(edited.items[0].vertexCount, 10u); EXPECT_EQ(edited.items[0].indexCount, 12u);
    EXPECT_VEC3_NEAR(edited.items[0].boundsCenter, (math::Vector3{3, 7, 5}), testkit::kTolerance);
    EXPECT_FLOAT_EQ(edited.items[0].boundsRadius, 12); EXPECT_FALSE(edited.items[0].reliableOccluder);
    EXPECT_TRUE(original.items[0].reliableOccluder);
}

TEST_F(StaticMeshRenderReuseTest, RebuildsRotatedBoundsAndSeparatesSkinnedMeshInputs)
{
    auto& object = Object(m_scene);
    (void)Extract(1);
    object.transform.worldRotation = math::Quaternion::FromAxisAngle(math::Vector3::UP, math::PI);
    const auto rotated = Extract(2);
    EXPECT_EQ(Rebuilds(), 1u);
    EXPECT_VEC3_NEAR(rotated.items[0].boundsCenter, (math::Vector3{-1, 0, 0}), testkit::kTolerance);
    (void)Extract(3);
    EXPECT_EQ(Rebuilds(), 0u);
    m_mesh.isSkinned = true;
    EXPECT_TRUE(Extract(4).Objects().empty());
    m_mesh.isSkinned = false;
    const auto restored = Extract(5);
    EXPECT_EQ(Rebuilds(), 1u);
    EXPECT_VEC3_NEAR(restored.items[0].boundsCenter, (math::Vector3{-1, 0, 0}), testkit::kTolerance);
}

TEST_F(StaticMeshRenderReuseTest, AdvancesPreviousWorldOnReuseAndResetsAfterMissingFrames)
{
    auto& object = Object(m_scene);
    (void)Extract(20);
    object.transform.worldPosition.x = 5;
    const auto moved = Extract(21);
    EXPECT_FLOAT_EQ(moved.Objects()[0].previousWorld.m[0][3], 0);
    const auto stable = Extract(22);
    EXPECT_EQ(Rebuilds(), 0u);
    EXPECT_FLOAT_EQ(stable.Objects()[0].previousWorld.m[0][3], 5);
    object.transform.worldPosition.x = 9;
    const auto sameFrame = Extract(22);
    EXPECT_FLOAT_EQ(sameFrame.Objects()[0].world.m[0][3], 9);
    EXPECT_FLOAT_EQ(sameFrame.Objects()[0].previousWorld.m[0][3], 5);
    const auto gap = Extract(30);
    EXPECT_EQ(Rebuilds(), 0u);
    EXPECT_FLOAT_EQ(gap.Objects()[0].previousWorld.m[0][3], 9);
}

TEST_F(StaticMeshRenderReuseTest, SeparatesComponentEntityAndSceneLifetimes)
{
    auto& object = Object(m_scene);
    const auto id = object.GetID();
    const auto retained = Extract(1);
    object.RemoveComponent<scene::MeshRenderer>();
    object.AddComponent<scene::MeshRenderer>().mesh = &m_mesh;
    (void)Extract(2);
    EXPECT_EQ(Rebuilds(), 1u);
    ASSERT_TRUE(m_scene.DestroyGameObject(id));
    auto& replacement = Object(m_scene);
    replacement.transform.worldPosition = {-8, 0, 0};
    const auto replaced = Extract(3);
    ASSERT_EQ(replaced.Objects().size(), 1u);
    EXPECT_EQ(replaced.Objects()[0].sourceGeneration, replacement.GetID().generation);
    EXPECT_VEC3_NEAR(replaced.items[0].boundsCenter, (math::Vector3{-7, 0, 0}), testkit::kTolerance);
    m_scene.Clear();
    Object(m_scene).transform.worldPosition = {20, 0, 0};
    const auto cleared = Extract(4);
    EXPECT_VEC3_NEAR(cleared.items[0].boundsCenter, (math::Vector3{21, 0, 0}), testkit::kTolerance);
    EXPECT_VEC3_NEAR(retained.items[0].boundsCenter, (math::Vector3{1, 0, 0}), testkit::kTolerance);
}

TEST_F(StaticMeshRenderReuseTest, ObservesParentMotionReparentingAndHierarchyActivation)
{
    auto& parent = m_scene.CreateGameObject("Parent");
    auto& child = Object(m_scene);
    child.SetParent(parent);
    child.transform.position = {2, 0, 0};
    scene::FlushWorldTransforms(m_scene);
    (void)Extract(1);
    parent.transform.position = {10, 0, 0};
    scene::FlushWorldTransforms(m_scene);
    const auto moved = Extract(2);
    EXPECT_EQ(Rebuilds(), 1u);
    EXPECT_VEC3_NEAR(moved.items[0].boundsCenter, (math::Vector3{13, 0, 0}), testkit::kTolerance);
    parent.SetActive(false);
    EXPECT_TRUE(Extract(3).Objects().empty());
    parent.SetActive(true);
    ASSERT_TRUE(child.ClearParent());
    child.transform.position = {-3, 0, 0};
    scene::FlushWorldTransforms(m_scene);
    const auto detached = Extract(4);
    ASSERT_EQ(detached.items.size(), 1u);
    EXPECT_VEC3_NEAR(detached.items[0].boundsCenter, (math::Vector3{-2, 0, 0}), testkit::kTolerance);
    EXPECT_VEC3_NEAR(moved.items[0].boundsCenter, (math::Vector3{13, 0, 0}), testkit::kTolerance);
}

TEST_F(StaticMeshRenderReuseTest, KeepsUnchangedGeometryWhenAnEarlyRendererLeavesAndRejoins)
{
    auto& parent = m_scene.CreateGameObject("Parent");
    auto& child = Object(m_scene);
    child.SetParent(parent);
    for (int i = 0; i < 80; ++i) Object(m_scene);
    const auto full = Extract(1);
    ASSERT_EQ(full.Objects().size(), 81u);
    ASSERT_EQ(full.Objects().front().sourceIndex, child.GetID().index);

    parent.SetActive(false);
    const auto hidden = Extract(2);
    ASSERT_EQ(hidden.Objects().size(), 80u);
    EXPECT_EQ(hidden.Objects().front().sourceIndex, full.Objects()[1].sourceIndex);
    EXPECT_EQ(Rebuilds(), 0u);

    parent.SetActive(true);
    const auto visible = Extract(3);
    ASSERT_EQ(visible.Objects().size(), 81u);
    EXPECT_EQ(visible.Objects().front().sourceIndex, child.GetID().index);
    EXPECT_EQ(Rebuilds(), 0u);

    parent.SetActive(false);
    ASSERT_TRUE(child.ClearParent());
    child.transform.position.x = 5.0f;
    scene::FlushWorldTransforms(m_scene);
    const auto detached = Extract(4);
    ASSERT_EQ(detached.Objects().size(), 81u);
    EXPECT_EQ(detached.Objects().front().sourceIndex, child.GetID().index);
    EXPECT_FLOAT_EQ(detached.Objects().front().world.m[0][3], 5.0f);
    EXPECT_EQ(Rebuilds(), 1u);

    (void)Extract(5);
    EXPECT_EQ(Rebuilds(), 0u);
    EXPECT_EQ(SampleCount("RenderSceneExtractor::DenseGeometryFallback"), 0u);
}

TEST_F(StaticMeshRenderReuseTest, RevalidatesAfterBoundedSceneEvictionAndRendererReordering)
{
    const auto firstId = Object(m_scene).GetID();
    auto& second = Object(m_scene);
    second.transform.worldPosition = {10, 0, 0};
    (void)Extract(1);
    std::array<std::unique_ptr<scene::Scene>, 5> others;
    for (auto& owner : others) {
        owner = std::make_unique<scene::Scene>(); Object(*owner);
        (void)scene::ExtractRenderSceneGeometry(*owner, 1);
    }
    (void)Extract(2);
    EXPECT_EQ(Rebuilds(), 0u);
    EXPECT_EQ(SampleCount("RenderSceneExtractor::ColdGeometry"), 1u);
    ASSERT_TRUE(m_scene.DestroyGameObject(firstId));
    auto& added = Object(m_scene);
    added.transform.worldPosition = {-2, 0, 0};
    const auto result = Extract(3);
    ASSERT_EQ(result.Objects().size(), 2u);
    scene::NotifyMeshRendererChanged(m_scene, added.GetID());
    (void)Extract(4);
    EXPECT_EQ(Rebuilds(), 0u);
    scene::NotifyMeshRendererChanged(m_scene, second.GetID());
    (void)Extract(5);
    EXPECT_EQ(Rebuilds(), 0u);
}

}
