/// @file    RenderSceneExtractorTests.cpp
/// @brief   Scene から独立したメッシュ資源・姿勢・submesh 対応の抽出を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-21
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Engine/Scene/Systems/RenderSceneExtractor.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <Engine/Asset/Model.hpp>
#include <Engine/Renderer/Mesh.hpp>
#include <memory>

namespace fbzz::tests {
namespace {
class RenderSceneExtractorTest : public testkit::EngineFixture {
protected:
    scene::Scene m_scene;
    renderer::Mesh m_mesh;
    asset::Model m_model;

    scene::GameObject& StaticObject()
    {
        auto& go = m_scene.CreateGameObject("Mesh");
        scene::MeshRenderer component;
        component.mesh = &m_mesh;
        go.AddComponent<scene::MeshRenderer>(component);
        return go;
    }

    scene::GameObject& SkinnedObject()
    {
        for (uint32_t i = 0; i < 3; ++i) {
            auto mesh = std::make_unique<renderer::Mesh>();
            mesh->vertexBuffer = { 10 + i, 2 };
            mesh->indexBuffer = { 20 + i, 3 };
            mesh->indexCount = 30 + i;
            mesh->vertexCount = 40 + i;
            mesh->boundsRadius = 1.0f + static_cast<float>(i);
            m_model.meshes.push_back(std::move(mesh));
        }
        auto& go = m_scene.CreateGameObject("Skinned");
        scene::SkinnedMeshRenderer component;
        component.model = &m_model;
        component.submeshIndices = { 2, 0 };
        go.AddComponent<scene::SkinnedMeshRenderer>(component);
        return go;
    }
};

TEST_F(RenderSceneExtractorTest, KeepsOffscreenAndOtherLayerCandidatesButRejectsInactiveHierarchy)
{
    auto& go = StaticObject();
    go.layer = 7;
    go.transform.position = { 10000.0f, 0.0f, 0.0f };
    go.transform.worldPosition = go.transform.position;
    go.GetComponent<scene::MeshRenderer>()->lodVisible = false;
    auto snapshot = scene::ExtractRenderSceneGeometry(m_scene, 10);
    ASSERT_EQ(snapshot.objects.size(), 1u);
    EXPECT_EQ(snapshot.objects[0].layer, 7u);
    EXPECT_FALSE(snapshot.objects[0].lodVisible);
    auto& parent = m_scene.CreateGameObject("Parent");
    go.SetParent(parent);
    parent.SetActive(false);
    EXPECT_TRUE(scene::ExtractRenderSceneGeometry(m_scene, 10).objects.empty());
}

TEST_F(RenderSceneExtractorTest, SkinnedTextureQualityUsesAnimatedBoundsInsteadOfBindPose)
{
    auto& go = SkinnedObject();
    scene::AnimatorComponent animator;
    animator.skinnedBoundsCenter = {0.0f, 0.0f, 100.0f};
    animator.skinnedBoundsRadius = 7.0f;
    go.AddComponent<scene::AnimatorComponent>(animator);
    const auto snapshot = scene::ExtractRenderSceneGeometry(m_scene, 1);
    ASSERT_EQ(snapshot.objects.size(), 1u);
    const auto& object = snapshot.objects[0];
    const auto& item = snapshot.items[0];
    EXPECT_FLOAT_EQ(object.boundsRadius, 10.0f);
    EXPECT_FLOAT_EQ(scene::EstimateRenderTexturePixels(object, item, {}, 1.0f, 1000, false), 100.0f);
    EXPECT_FLOAT_EQ(scene::EstimateRenderTexturePixels(object, item, {0,0,90}, 1.0f, 1000, false), 1000.0f);
    EXPECT_FLOAT_EQ(scene::EstimateRenderTexturePixels(object, item, {}, 0.1f, 1000, true), 1000.0f);
    EXPECT_FLOAT_EQ(scene::EstimateRenderTexturePixels(object, item, {}, 0.0f, 1000, false), 0.0f);
}

TEST_F(RenderSceneExtractorTest, CopiesWorldBoundsAndHandlesWithoutOwningTheSourceMesh)
{
    auto& go = StaticObject();
    go.transform.worldPosition = { 10.0f, 2.0f, 3.0f };
    m_mesh.boundsCenter = { 1.0f, 0.0f, 0.0f };
    m_mesh.boundsRadius = 2.0f;
    m_mesh.vertexBuffer = { 11, 4 };
    m_mesh.indexBuffer = { 12, 5 };
    m_mesh.indexCount = 36;
    const auto snapshot = scene::ExtractRenderSceneGeometry(m_scene, 10);
    ASSERT_EQ(snapshot.items.size(), 1u);
    EXPECT_VEC3_NEAR(snapshot.objects[0].boundsCenter, (math::Vector3{ 11.0f, 2.0f, 3.0f }), testkit::kTolerance);
    EXPECT_FLOAT_EQ(snapshot.objects[0].boundsRadius, 2.0f);
    m_mesh.vertexBuffer = {};
    m_mesh.indexCount = 0;
    go.transform.worldPosition = {};
    EXPECT_EQ(snapshot.items[0].vertexBuffer, (renderer::ResourceHandle<renderer::BufferTag>{ 11, 4 }));
    EXPECT_EQ(snapshot.items[0].indexCount, 36u);
    EXPECT_FLOAT_EQ(snapshot.objects[0].world.m[0][3], 10.0f);
}

TEST_F(RenderSceneExtractorTest, LocalSlotsResolveReorderedSubmeshesAndMorphInputs)
{
    auto& go = SkinnedObject();
    auto* smr = go.GetComponent<scene::SkinnedMeshRenderer>();
    smr->morphVertexBuffers.resize(3);
    smr->morphVertexBuffers[2] = { 90, 7 };
    const auto snapshot = scene::ExtractRenderSceneGeometry(m_scene, 10);
    ASSERT_EQ(snapshot.items.size(), 2u);
    EXPECT_EQ(snapshot.items[0].sourceSubmesh, 2u);
    EXPECT_EQ(snapshot.items[0].materialSlot, 0u);
    EXPECT_EQ(snapshot.items[0].vertexBuffer.id, 12u);
    EXPECT_EQ(snapshot.items[0].skinningVertexBuffer.id, 90u);
    EXPECT_EQ(snapshot.items[1].sourceSubmesh, 0u);
    EXPECT_EQ(snapshot.items[1].materialSlot, 1u);
    EXPECT_EQ(snapshot.items[1].skinningVertexBuffer.id, 10u);
}

TEST_F(RenderSceneExtractorTest, DeformedOutputRequiresCurrentFrameAndMatchingModelAndNoMorph)
{
    auto& go = SkinnedObject();
    auto* smr = go.GetComponent<scene::SkinnedMeshRenderer>();
    smr->gpuSkinnedThisFrame = true;
    smr->gpuSkinningFrame = 10;
    smr->skinnedBufferModel = &m_model;
    smr->skinnedVertexBuffers = { { 60, 1 }, { 61, 1 }, { 62, 1 } };
    auto snapshot = scene::ExtractRenderSceneGeometry(m_scene, 10);
    EXPECT_EQ(snapshot.items[0].deformedVertexBuffer.id, 62u);
    EXPECT_EQ(snapshot.items[1].deformedVertexBuffer.id, 60u);
    EXPECT_FALSE(scene::ExtractRenderSceneGeometry(m_scene, 11).items[0].deformedVertexBuffer.IsValid());
    smr->skinnedBufferModel = nullptr;
    EXPECT_FALSE(scene::ExtractRenderSceneGeometry(m_scene, 10).items[0].deformedVertexBuffer.IsValid());
    smr->skinnedBufferModel = &m_model;
    smr->morphVertexBuffers.resize(3);
    smr->morphVertexBuffers[2] = { 80, 1 };
    EXPECT_FALSE(scene::ExtractRenderSceneGeometry(m_scene, 10).items[0].deformedVertexBuffer.IsValid());
}

TEST_F(RenderSceneExtractorTest, ParentAnimatorWinsOverReferencePoseAndPreservesPreviousPalette)
{
    auto& child = SkinnedObject();
    auto& parent = m_scene.CreateGameObject("AnimatorRoot");
    child.SetParent(parent);
    scene::AnimatorComponent animator;
    animator.skinningBuffer = { 71, 1 };
    animator.prevSkinningBuffer = { 72, 2 };
    animator.prevBoneMatricesValid = true;
    parent.AddComponent<scene::AnimatorComponent>(animator);
    m_model.referencePoseCB = { 81, 1 };
    const auto snapshot = scene::ExtractRenderSceneGeometry(m_scene, 10, { 91, 1 });
    ASSERT_EQ(snapshot.objects.size(), 1u);
    EXPECT_EQ(snapshot.objects[0].skinningPalette.id, 71u);
    EXPECT_EQ(snapshot.objects[0].previousSkinningPalette.id, 72u);
    EXPECT_TRUE(snapshot.objects[0].previousSkinningValid);
}

TEST_F(RenderSceneExtractorTest, ReferencePosePrecedesIdentityFallbackWithoutAnimator)
{
    SkinnedObject();
    m_model.referencePoseCB = { 81, 1 };
    EXPECT_EQ(scene::ExtractRenderSceneGeometry(m_scene, 10, { 91, 1 }).objects[0].skinningPalette.id, 81u);
    m_model.referencePoseCB = {};
    const auto snapshot = scene::ExtractRenderSceneGeometry(m_scene, 10, { 91, 1 });
    EXPECT_EQ(snapshot.objects[0].skinningPalette.id, 91u);
    EXPECT_FALSE(snapshot.objects[0].previousSkinningValid);
}

TEST_F(RenderSceneExtractorTest, PreviousWorldAdvancesOnceAcrossViewsAndResetsAfterGap)
{
    auto& go = StaticObject();
    go.transform.worldPosition.x = 1.0f;
    EXPECT_FLOAT_EQ(scene::ExtractRenderSceneGeometry(m_scene, 10).objects[0].previousWorld.m[0][3], 1.0f);
    go.transform.worldPosition.x = 2.0f;
    const auto firstView = scene::ExtractRenderSceneGeometry(m_scene, 11);
    const auto secondView = scene::ExtractRenderSceneGeometry(m_scene, 11);
    EXPECT_FLOAT_EQ(firstView.objects[0].previousWorld.m[0][3], 1.0f);
    EXPECT_FLOAT_EQ(secondView.objects[0].previousWorld.m[0][3], 1.0f);
    go.transform.worldPosition.x = 5.0f;
    EXPECT_FLOAT_EQ(scene::ExtractRenderSceneGeometry(m_scene, 15).objects[0].previousWorld.m[0][3], 5.0f);
    EXPECT_NE(firstView.snapshotSerial, secondView.snapshotSerial);
}

TEST_F(RenderSceneExtractorTest, InitialFrameDoesNotTreatUninitializedHistoryAsPreviousFrame)
{
    auto& go = StaticObject();
    go.transform.worldPosition.x = 3.0f;
    const auto snapshot = scene::ExtractRenderSceneGeometry(m_scene, 0);
    ASSERT_EQ(snapshot.objects.size(), 1u);
    EXPECT_FLOAT_EQ(snapshot.objects[0].previousWorld.m[0][3], 3.0f);
    go.transform.worldPosition.x = 4.0f;
    EXPECT_FLOAT_EQ(scene::ExtractRenderSceneGeometry(m_scene, 1).objects[0].previousWorld.m[0][3], 3.0f);
}

TEST_F(RenderSceneExtractorTest, MaterialSlotsStayIndependentAndHiddenSlotsRemainAddressable)
{
    auto& go = SkinnedObject();
    scene::MaterialComponent material;
    material.extraSlots.resize(1);
    material.SlotAt(0).hasBlendModeOverride = true;
    material.SlotAt(0).blendModeOverride = renderer::BlendMode::ALPHA_BLEND;
    material.RawSlotAt(1).visible = false;
    go.AddComponent<scene::MaterialComponent>(material);
    const auto snapshot = scene::ExtractRenderSceneGeometry(m_scene, 10);
    ASSERT_EQ(snapshot.items.size(), 2u);
    EXPECT_EQ(snapshot.items[0].material.capabilities.blend, renderer::BlendMode::ALPHA_BLEND);
    EXPECT_TRUE(snapshot.items[0].visible);
    EXPECT_FALSE(snapshot.items[1].visible);
}

} /// @note namespace
} /// @note namespace fbzz::tests
