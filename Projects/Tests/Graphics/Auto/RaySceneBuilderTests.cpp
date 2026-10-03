/// @file    RaySceneBuilderTests.cpp
/// @brief   静的レイ候補・内容版・完全 object ID・用途 mask と未対応診断を固定入力で検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-30
#include <TestKit/TestKit.hpp>
#include <Graphics/RayTracing/RayScene.hpp>
#include <Graphics/Renderer/RenderScene.hpp>
#include <limits>
#include <cmath>
#include <cstring>

namespace fbzz::tests {
namespace {

class RaySceneBuilderTest : public testkit::Fixture {
protected:
    renderer::RenderScene m_scene;

    void SetUp() override
    {
        testkit::Fixture::SetUp();
        m_scene.sceneGeneration = 17;
        m_scene.snapshotSerial = 42;
        m_scene.frameStamp = 9;
    }

    void AddObject(uint32_t index = 0xFEDCBA98, uint32_t generation = 0x87654321)
    {
        renderer::RenderObject object;
        object.sourceIndex = index;
        object.sourceGeneration = generation;
        object.firstItem = static_cast<uint32_t>(m_scene.items.size());
        object.itemCount = 1;
        renderer::RenderMeshItem item;
        item.objectIndex = static_cast<uint32_t>(m_scene.objects.size());
        item.vertexBuffer = {100, 3};
        item.indexBuffer = {101, 4};
        item.vertexContentVersion = 1;
        item.indexContentVersion = 1;
        item.vertexStride = 60;
        item.vertexCount = item.indexCount = 3;
        item.material.valid = true;
        item.material.rayCapabilities = {true, renderer::RayOpacity::OPAQUE_SURFACE};
        m_scene.objects.push_back(object);
        m_scene.items.push_back(item);
    }

    void SetSurfaceConstant(uint32_t offset, float value)
    {
        std::memcpy(m_scene.items[0].material.gbufferParams.data() + offset, &value, sizeof(value));
    }

    renderer::SurfaceMaterialData ResolveSurface(bool standardPbr = true, bool textures = false, bool lobes = false,
        const renderer::SolidDielectricSettings& dielectric = {})
    {
        return renderer::ResolveConstantSurfaceMaterial(m_scene.items[0].material.gbufferParams,
            standardPbr, textures, lobes, dielectric);
    }
};

TEST_F(RaySceneBuilderTest, KeepsOffscreenAndRasterInvisibleCandidatesButFiltersLayers)
{
    AddObject();
    m_scene.objects[0].layer = 7;
    m_scene.objects[0].lodVisible = false;
    m_scene.objects[0].world = math::Matrix4::Translate({10000, 0, 0});
    m_scene.items[0].visible = false;
    AddObject(1, 2);
    const auto result = renderer::BuildRayScene(m_scene, {1u << 7});

    ASSERT_EQ(result.instances.size(), 1u);
    EXPECT_EQ(result.layerMask, 1u << 7);
    EXPECT_EQ(result.instances[0].sourceItem, 0u);
    EXPECT_FLOAT_EQ(result.instances[0].world.m[0][3], 10000);
    EXPECT_TRUE(result.diagnostics.empty());
    m_scene.objects[0].layer = 32;
    EXPECT_TRUE(renderer::BuildRayScene(m_scene, {1u << 7}).instances.empty());
}

TEST_F(RaySceneBuilderTest, EffectSourcesDiagnoseFullOwnersWithoutRasterDrawsAndRespectLayers)
{
    m_scene.rayUnsupportedEffects.push_back({0xFEDCBA98, 0x87654321, 1u << 7,
        renderer::RenderRayUnsupportedEffect::PARTICLE});
    ASSERT_TRUE(m_scene.particles.empty());
    const auto result = renderer::BuildRayScene(m_scene, {1u << 7});
    ASSERT_EQ(result.diagnostics.size(), 1u);
    EXPECT_EQ(result.diagnostics[0].objectId, (renderer::RayObjectId{17, 0xFEDCBA98, 0x87654321}));
    EXPECT_EQ(result.diagnostics[0].issue, renderer::RaySceneIssue::NONMESH_GEOMETRY_UNSUPPORTED);
    EXPECT_TRUE(renderer::BuildRayScene(m_scene, {1u << 1}).diagnostics.empty());

    m_scene.particles.push_back({});
    m_scene.particles[0].layer = 7;
    EXPECT_EQ(renderer::BuildRayScene(m_scene, {1u << 7}).diagnostics.size(), 2u);
    m_scene.rayUnsupportedEffects.clear();
    EXPECT_EQ(renderer::BuildRayScene(m_scene, {1u << 7}).diagnostics.size(), 1u);
}

TEST_F(RaySceneBuilderTest, PartialParticleMetadataCannotSuppressAnotherOwnerOrLayerDrawList)
{
    m_scene.rayUnsupportedEffects.push_back({11, 3, 1u << 7,
        renderer::RenderRayUnsupportedEffect::PARTICLE});
    m_scene.particles.push_back({});
    m_scene.particles[0].layer = 1;
    EXPECT_EQ(renderer::BuildRayScene(m_scene, {1u << 1}).diagnostics.size(), 1u);
    const auto mesh = renderer::BuildRayScene(m_scene, {1u << 7});
    ASSERT_EQ(mesh.diagnostics.size(), 1u);
    EXPECT_EQ(mesh.diagnostics[0].objectId, (renderer::RayObjectId{17, 11, 3}));
    EXPECT_EQ(renderer::BuildRayScene(m_scene).diagnostics.size(), 2u);
    EXPECT_TRUE(renderer::BuildRayScene(m_scene, {1u << 2}).diagnostics.empty());
}

TEST_F(RaySceneBuilderTest, MeshFiberMetadataCannotSuppressAnotherLayerTerrainLegacySource)
{
    m_scene.rayUnsupportedEffects.push_back({11, 3, 1u << 7,
        renderer::RenderRayUnsupportedEffect::FIBER});
    m_scene.fibers.push_back({});
    m_scene.fibers[0].layer = 1;
    m_scene.fibers[0].vertices = {21, 3};
    m_scene.fibers[0].indices = {22, 3};
    m_scene.fibers[0].indexCount = 3;
    const auto terrain = renderer::BuildRayScene(m_scene, {1u << 1});
    ASSERT_EQ(terrain.diagnostics.size(), 1u);
    EXPECT_EQ(terrain.diagnostics[0].issue, renderer::RaySceneIssue::NONMESH_GEOMETRY_UNSUPPORTED);
    const auto mesh = renderer::BuildRayScene(m_scene, {1u << 7});
    ASSERT_EQ(mesh.diagnostics.size(), 1u);
    EXPECT_EQ(mesh.diagnostics[0].objectId, (renderer::RayObjectId{17, 11, 3}));
    EXPECT_EQ(renderer::BuildRayScene(m_scene).diagnostics.size(), 2u);
    EXPECT_TRUE(renderer::BuildRayScene(m_scene, {1u << 2}).diagnostics.empty());
}

TEST_F(RaySceneBuilderTest, ResolvedFiberCountsProveEmptinessWithoutUsingCameraOrLod)
{
    auto& fiber = m_scene.fibers.emplace_back();
    EXPECT_TRUE(renderer::BuildRayScene(m_scene).diagnostics.empty());
    fiber.vertices = {11, 2}; fiber.indices = {12, 2}; fiber.indexCount = 3;
    fiber.frame.m_lod = {0, 0, 0, 0};
    fiber.boundsRadius = 0;
    fiber.lodCenter = {10000, 0, 0};
    EXPECT_EQ(renderer::BuildRayScene(m_scene).diagnostics.size(), 1u);
    fiber.indexCount = 0;
    EXPECT_TRUE(renderer::BuildRayScene(m_scene).diagnostics.empty());
    fiber.settings.m_mode = renderer::FiberRenderMode::FIN;
    fiber.fins.m_vertices = {13, 2}; fiber.fins.m_indices = {14, 2}; fiber.fins.m_indexCount = 6;
    EXPECT_EQ(renderer::BuildRayScene(m_scene).diagnostics.size(), 1u);
    fiber.fins.m_indexCount = 0;
    EXPECT_TRUE(renderer::BuildRayScene(m_scene).diagnostics.empty());
    fiber.settings.m_mode = renderer::FiberRenderMode::BLADE;
    fiber.blades.m_blades = {15, 2}; fiber.blades.m_bladeCount = 1;
    EXPECT_EQ(renderer::BuildRayScene(m_scene).diagnostics.size(), 1u);
    fiber.skinned = true;
    EXPECT_TRUE(renderer::BuildRayScene(m_scene).diagnostics.empty());
    fiber.skinned = false;
    fiber.blades.m_bladeCount = 0;
    EXPECT_TRUE(renderer::BuildRayScene(m_scene).diagnostics.empty());
    fiber.settings.m_mode = renderer::FiberRenderMode::HYBRID;
    fiber.fins.m_indexCount = 6;
    EXPECT_EQ(renderer::BuildRayScene(m_scene).diagnostics.size(), 1u);
    fiber.fins.m_indexCount = 0;
    fiber.indexCount = 3;
    EXPECT_EQ(renderer::BuildRayScene(m_scene).diagnostics.size(), 1u);
    fiber.material.m_density = 0;
    EXPECT_TRUE(renderer::BuildRayScene(m_scene).diagnostics.empty());
    fiber.material.m_density = 1;
    fiber.material.m_length = 0;
    EXPECT_TRUE(renderer::BuildRayScene(m_scene).diagnostics.empty());
    fiber.material.m_length = 1;
    fiber.material.m_density = std::numeric_limits<float>::quiet_NaN();
    EXPECT_EQ(renderer::BuildRayScene(m_scene).diagnostics.size(), 1u);
    fiber.material.m_density = 1;
    fiber.indexCount = 0;
    fiber.settings.m_mode = static_cast<renderer::FiberRenderMode>(99);
    EXPECT_EQ(renderer::BuildRayScene(m_scene).diagnostics.size(), 1u);
}

TEST_F(RaySceneBuilderTest, PreservesCompleteObjectIdsAcrossDenseSubmeshInstances)
{
    AddObject();
    m_scene.objects[0].itemCount = 2;
    m_scene.items.push_back(m_scene.items[0]);
    m_scene.items[1].sourceSubmesh = 11;
    m_scene.items[1].materialSlot = 3;
    m_scene.objects[0].previousWorld = math::Matrix4::Translate({1, 2, 3});
    const auto result = renderer::BuildRayScene(m_scene);

    ASSERT_EQ(result.instances.size(), 2u);
    EXPECT_EQ(result.geometries.size(), 1u);
    EXPECT_EQ(result.instances[0].denseInstanceId, 0u);
    EXPECT_EQ(result.instances[1].denseInstanceId, 1u);
    EXPECT_EQ(result.instances[1].sourceSubmesh, 11u);
    EXPECT_EQ(result.instances[1].materialSlot, 3u);
    EXPECT_EQ(result.instances[1].objectId, (renderer::RayObjectId{17, 0xFEDCBA98, 0x87654321}));
    EXPECT_FLOAT_EQ(result.instances[1].previousWorld.m[2][3], 3);
    ++m_scene.snapshotSerial;
    EXPECT_EQ(renderer::BuildRayScene(m_scene).instances[0].objectId, result.instances[0].objectId);
    ++m_scene.sceneGeneration;
    EXPECT_NE(renderer::BuildRayScene(m_scene).instances[0].objectId, result.instances[0].objectId);
}

TEST_F(RaySceneBuilderTest, DisablesOnlyShadowPurposeAndHonorsExplicitSlotVisibility)
{
    AddObject();
    m_scene.objects[0].castShadows = false;
    const auto result = renderer::BuildRayScene(m_scene);

    ASSERT_EQ(result.instances.size(), 1u);
    EXPECT_EQ(result.instances[0].mask, renderer::RAY_PRIMARY_MASK | renderer::RAY_SPECULAR_MASK | renderer::RAY_DIFFUSE_MASK);
    m_scene.items[0].slotVisible = false;
    const auto hidden = renderer::BuildRayScene(m_scene);
    EXPECT_TRUE(hidden.instances.empty());
    EXPECT_TRUE(hidden.diagnostics.empty());
}

TEST_F(RaySceneBuilderTest, ReusesOnlyMatchingBufferGenerationsContentVersionsAndOpacityLayout)
{
    AddObject();
    AddObject(1, 1);
    EXPECT_EQ(renderer::BuildRayScene(m_scene).geometries.size(), 1u);
    ++m_scene.snapshotSerial;
    ++m_scene.frameStamp;
    EXPECT_EQ(renderer::BuildRayScene(m_scene).geometries.size(), 1u);
    ++m_scene.items[1].vertexContentVersion;
    EXPECT_EQ(renderer::BuildRayScene(m_scene).geometries.size(), 2u);
    m_scene.items[1] = m_scene.items[0];
    m_scene.items[1].objectIndex = 1;
    ++m_scene.items[1].indexBuffer.gen;
    EXPECT_EQ(renderer::BuildRayScene(m_scene).geometries.size(), 2u);
    m_scene.items[1].indexBuffer = m_scene.items[0].indexBuffer;
    m_scene.items[1].material.doubleSided = true;
    EXPECT_EQ(renderer::BuildRayScene(m_scene).geometries.size(), 2u);
    m_scene.items[1].material.doubleSided = false;
    m_scene.items[1].vertexPositionOffset = 4;
    EXPECT_EQ(renderer::BuildRayScene(m_scene).geometries.size(), 2u);
}

TEST_F(RaySceneBuilderTest, UsesOnlyVersionedCurrentDeformationAndCanonicalLod)
{
    AddObject();
    auto& object = m_scene.objects[0];
    auto& item = m_scene.items[0];
    object.skinned = true;
    object.lodVisible = false;
    object.lodDither = 0.4f;
    item.deformedVertexBuffer = {200, 5};
    EXPECT_TRUE(renderer::BuildRayScene(m_scene).instances.empty());
    item.deformedContentVersion = 7;
    const auto current = renderer::BuildRayScene(m_scene);
    ASSERT_EQ(current.instances.size(), 1u);
    EXPECT_EQ(current.geometries[0].key.vertices, item.deformedVertexBuffer);
    EXPECT_EQ(current.geometries[0].key.vertexContentVersion, 7u);
    EXPECT_EQ(current.geometries[0].key.vertexStride, 60u);
    object.rayVisible = false;
    EXPECT_TRUE(renderer::BuildRayScene(m_scene).instances.empty());
    EXPECT_TRUE(renderer::BuildRayScene(m_scene).diagnostics.empty());
    object.rayLodSelectionRequired = true;
    const auto ambiguous = renderer::BuildRayScene(m_scene);
    ASSERT_EQ(ambiguous.diagnostics.size(), 1u);
    EXPECT_EQ(ambiguous.diagnostics[0].issue, renderer::RaySceneIssue::LOD_SELECTION_UNRESOLVED);
}

TEST_F(RaySceneBuilderTest, CandidateAlphaRequiresValidatedSurfaceAndNonOpaqueBlas)
{
    AddObject();
    auto& item = m_scene.items[0];
    item.material.rayCapabilities.opacity = renderer::RayOpacity::ALPHA_TEST;
    EXPECT_TRUE(renderer::BuildRayScene(m_scene).instances.empty());
    item.material.surface.standardSurfaceSupported = true;
    const auto clipped = renderer::BuildRayScene(m_scene);
    ASSERT_EQ(clipped.instances.size(), 1u);
    EXPECT_FALSE(clipped.geometries[0].key.opaque);
    EXPECT_FALSE(clipped.geometries[0].triangles.opaque);
}

TEST_F(RaySceneBuilderTest, DiagnosesUnsupportedGeometryWithoutUsingRasterRouteAsProof)
{
    AddObject();
    m_scene.objects[0].skinned = true;
    AddObject(1, 1);
    m_scene.items[1].material.rayCapabilities.staticGeometry = false;
    m_scene.items[1].material.capabilities.gbufferEquivalentShader = true;
    AddObject(2, 1);
    m_scene.items[2].material.rayCapabilities.opacity = renderer::RayOpacity::ALPHA_TEST;
    AddObject(3, 1);
    m_scene.items[3].material.capabilities.blend = renderer::BlendMode::ALPHA_BLEND;
    AddObject(4, 1);
    const auto result = renderer::BuildRayScene(m_scene);

    ASSERT_EQ(result.instances.size(), 1u);
    EXPECT_EQ(result.instances[0].denseInstanceId, 0u);
    EXPECT_EQ(result.instances[0].objectId.index, 4u);
    ASSERT_EQ(result.diagnostics.size(), 4u);
    EXPECT_EQ(result.diagnostics[0].issue, renderer::RaySceneIssue::SKINNED_UNSUPPORTED);
    EXPECT_EQ(result.diagnostics[1].issue, renderer::RaySceneIssue::CUSTOM_GEOMETRY_UNSUPPORTED);
    EXPECT_EQ(result.diagnostics[2].issue, renderer::RaySceneIssue::ALPHA_TEST_UNSUPPORTED);
    EXPECT_EQ(result.diagnostics[3].issue, renderer::RaySceneIssue::TRANSPARENT_UNSUPPORTED);
    EXPECT_EQ(result.diagnostics[2].objectId.index, 2u);
}

TEST_F(RaySceneBuilderTest, RejectsMissingIdentityMalformedRangesAndUnavailableContent)
{
    AddObject();
    m_scene.sceneGeneration = 0;
    auto result = renderer::BuildRayScene(m_scene);
    ASSERT_EQ(result.diagnostics.size(), 1u);
    EXPECT_EQ(result.diagnostics[0].issue, renderer::RaySceneIssue::SCENE_ID_UNAVAILABLE);
    m_scene.sceneGeneration = 17;
    m_scene.objects[0].firstItem = UINT32_MAX;
    result = renderer::BuildRayScene(m_scene);
    ASSERT_EQ(result.diagnostics.size(), 1u);
    EXPECT_EQ(result.diagnostics[0].issue, renderer::RaySceneIssue::INVALID_OBJECT_RANGE);
    m_scene.objects[0].firstItem = 0;
    m_scene.items[0].vertexContentVersion = 0;
    result = renderer::BuildRayScene(m_scene);
    ASSERT_EQ(result.diagnostics.size(), 1u);
    EXPECT_EQ(result.diagnostics[0].issue, renderer::RaySceneIssue::CONTENT_VERSION_UNAVAILABLE);
    m_scene.items[0].vertexContentVersion = 1;
    m_scene.objects[0].world = math::Matrix4::Scale({1, 0, 1});
    result = renderer::BuildRayScene(m_scene);
    ASSERT_EQ(result.diagnostics.size(), 1u);
    EXPECT_EQ(result.diagnostics[0].issue, renderer::RaySceneIssue::INVALID_TRANSFORM);
}

TEST_F(RaySceneBuilderTest, DiagnosesOnlyInLayerUnsupportedEffects)
{
    m_scene.water.emplace_back().layer = 7;
    m_scene.terrains.emplace_back().layer = 1;
    const auto result = renderer::BuildRayScene(m_scene, {1u << 7});
    ASSERT_EQ(result.diagnostics.size(), 1u);
    EXPECT_EQ(result.diagnostics[0].issue, renderer::RaySceneIssue::NONMESH_GEOMETRY_UNSUPPORTED);
}

TEST_F(RaySceneBuilderTest, CollectsNonindexedTrianglesWithNoIndexContentVersion)
{
    AddObject();
    m_scene.items[0].indexBuffer = {};
    m_scene.items[0].indexCount = 0;
    m_scene.items[0].indexContentVersion = 0;
    const auto result = renderer::BuildRayScene(m_scene);
    ASSERT_EQ(result.geometries.size(), 1u);
    EXPECT_FALSE(result.geometries[0].triangles.indices.IsValid());
    EXPECT_EQ(result.geometries[0].triangles.vertexCount, 3u);
    EXPECT_TRUE(result.diagnostics.empty());
}

TEST_F(RaySceneBuilderTest, ProvesOpaqueOnlyForKnownConstantAlphaAndKeepsEmptyCoverageEmpty)
{
    const auto resolve = [](bool standard, renderer::BlendMode blend, float alpha, float cutoff, bool texture) {
        return renderer::ResolveStaticRayMaterialCapabilities(standard, blend, alpha, cutoff, texture);
    };
    const auto opaque = renderer::BlendMode::OPAQUE_BLEND;
    EXPECT_EQ(resolve(true, opaque, 1, 0.5f, false).opacity, renderer::RayOpacity::OPAQUE_SURFACE);
    EXPECT_EQ(resolve(true, opaque, 0.25f, 0.5f, false).opacity, renderer::RayOpacity::EMPTY);
    EXPECT_EQ(resolve(true, opaque, 1, 0.5f, true).opacity, renderer::RayOpacity::ALPHA_TEST);
    EXPECT_EQ(resolve(true, opaque, std::numeric_limits<float>::quiet_NaN(), 0, false).opacity, renderer::RayOpacity::UNSUPPORTED);
    EXPECT_FALSE(resolve(false, opaque, 1, 0, false).staticGeometry);
    EXPECT_EQ(resolve(true, renderer::BlendMode::ALPHA_BLEND, 1, 0, false).opacity, renderer::RayOpacity::UNSUPPORTED);
    AddObject();
    m_scene.items[0].material.rayCapabilities.opacity = renderer::RayOpacity::EMPTY;
    const auto result = renderer::BuildRayScene(m_scene);
    EXPECT_TRUE(result.instances.empty());
    EXPECT_TRUE(result.diagnostics.empty());
}

TEST_F(RaySceneBuilderTest, ResolvesLinearConstantPbrIntoDenseInstanceSurfaceRecords)
{
    AddObject();
    SetSurfaceConstant(0, 0.25f); SetSurfaceConstant(4, 0.5f); SetSurfaceConstant(8, 0.75f);
    SetSurfaceConstant(12, 1); SetSurfaceConstant(16, 0.7f); SetSurfaceConstant(20, 0);
    SetSurfaceConstant(32, 2); SetSurfaceConstant(36, 3); SetSurfaceConstant(40, 4); SetSurfaceConstant(44, 2);
    m_scene.items[0].material.surface = ResolveSurface();
    const auto result = renderer::BuildRayScene(m_scene);
    ASSERT_EQ(result.instances.size(), 1u);
    EXPECT_TRUE(result.surfaceDiagnostics.empty());
    EXPECT_VEC3_NEAR(result.instances[0].surface.emission, (math::Vector3{4, 6, 8}), 1e-6f);
    const auto record = renderer::MakeRaySurfaceRecord(result.instances[0].surface);
    EXPECT_FLOAT_EQ(record.baseColor[0], 0.25f);
    EXPECT_FLOAT_EQ(record.metallic, 0.7f);
    EXPECT_FLOAT_EQ(record.roughness, 0.045f);
    EXPECT_EQ(record.supported, 1u);
    EXPECT_FLOAT_EQ(record.transmission, 0);
    EXPECT_FLOAT_EQ(record.ior, 1.5f);
    EXPECT_EQ(record.attenuationColor, (std::array<float, 3>{1, 1, 1}));
    EXPECT_FLOAT_EQ(record.attenuationDistance, 1);
}

TEST_F(RaySceneBuilderTest, SeparatesUnsupportedHitShadingFromIntersectionCoverage)
{
    AddObject();
    const std::array<renderer::SurfaceMaterialData, 3> unsupported = {
        ResolveSurface(false), ResolveSurface(true, true), ResolveSurface(true, false, true)};
    const std::array<renderer::SurfaceMaterialIssue, 3> expected = {
        renderer::SurfaceMaterialIssue::UNSUPPORTED_SHADER,
        renderer::SurfaceMaterialIssue::TEXTURE_UNSUPPORTED,
        renderer::SurfaceMaterialIssue::ADVANCED_LOBES_UNSUPPORTED};
    for (size_t i = 0; i < unsupported.size(); ++i) {
        m_scene.items[0].material.surface = unsupported[i];
        const auto result = renderer::BuildRayScene(m_scene);
        ASSERT_EQ(result.instances.size(), 1u);
        EXPECT_TRUE(result.diagnostics.empty());
        ASSERT_EQ(result.surfaceDiagnostics.size(), 1u);
        EXPECT_EQ(result.surfaceDiagnostics[0].issue, expected[i]);
        EXPECT_EQ(result.surfaceDiagnostics[0].sourceItem, 0u);
        EXPECT_EQ(renderer::MakeRaySurfaceRecord(result.instances[0].surface).supported, 0u);
    }
    for (uint32_t bit = 0; bit < 8; ++bit) {
        const uint32_t mask = 1u << bit;
        std::memcpy(m_scene.items[0].material.gbufferParams.data() + 80, &mask, sizeof(mask));
        EXPECT_EQ(ResolveSurface().issue, renderer::SurfaceMaterialIssue::TEXTURE_UNSUPPORTED);
    }
}

TEST_F(RaySceneBuilderTest, DetectsExactSurfaceChangesWithoutUsingFrameOrSnapshotSerial)
{
    AddObject();
    SetSurfaceConstant(0, 1);
    m_scene.items[0].material.surface = ResolveSurface();
    const auto initial = renderer::BuildRayScene(m_scene);
    ++m_scene.snapshotSerial; ++m_scene.frameStamp;
    EXPECT_EQ(renderer::BuildRayScene(m_scene).instances[0].surface, initial.instances[0].surface);
    SetSurfaceConstant(0, std::nextafter(1.0f, 2.0f));
    m_scene.items[0].material.surface = ResolveSurface();
    const auto changed = renderer::BuildRayScene(m_scene);
    EXPECT_NE(changed.instances[0].surface, initial.instances[0].surface);
    EXPECT_EQ(changed.geometries[0].key, initial.geometries[0].key);
    EXPECT_NE(renderer::MakeRaySurfaceRecord(changed.instances[0].surface),
        renderer::MakeRaySurfaceRecord(initial.instances[0].surface));
}

TEST_F(RaySceneBuilderTest, RejectsNonfiniteAndOverflowedEmissionBeforeGpuPacking)
{
    AddObject();
    SetSurfaceConstant(0, std::numeric_limits<float>::quiet_NaN());
    const auto invalid = ResolveSurface();
    EXPECT_EQ(invalid.issue, renderer::SurfaceMaterialIssue::INVALID_CONSTANTS);
    EXPECT_EQ(invalid, ResolveSurface());
    const auto safe = renderer::MakeRaySurfaceRecord(invalid);
    EXPECT_EQ(safe.supported, 0u);
    EXPECT_TRUE(std::isfinite(safe.baseColor[0]));
    SetSurfaceConstant(0, 1);
    SetSurfaceConstant(32, std::numeric_limits<float>::max()); SetSurfaceConstant(44, 2);
    EXPECT_EQ(ResolveSurface().issue, renderer::SurfaceMaterialIssue::INVALID_CONSTANTS);
    EXPECT_TRUE(std::isfinite(renderer::MakeRaySurfaceRecord(ResolveSurface()).emission[0]));
    SetSurfaceConstant(44, -1);
    EXPECT_FALSE(ResolveSurface().standardSurfaceSupported);
}

TEST_F(RaySceneBuilderTest, CollectsSmoothSolidDielectricOnlyWithPathOptIn)
{
    AddObject();
    SetSurfaceConstant(0, 1); SetSurfaceConstant(4, 1); SetSurfaceConstant(8, 1); SetSurfaceConstant(12, 1);
    renderer::SolidDielectricSettings glass;
    glass.transmission = 1;
    glass.ior = 1.6f;
    glass.attenuationColor = {0, 0.5f, 1};
    glass.attenuationDistance = 2;
    m_scene.items[0].material.surface = ResolveSurface(true, false, false, glass);
    EXPECT_FALSE(m_scene.items[0].material.surface.standardSurfaceSupported);
    EXPECT_TRUE(m_scene.items[0].material.surface.solidDielectricSupported);
    const auto reflection = renderer::BuildRayScene(m_scene);
    EXPECT_TRUE(reflection.instances.empty());
    ASSERT_EQ(reflection.diagnostics.size(), 1u);
    EXPECT_EQ(reflection.diagnostics[0].issue, renderer::RaySceneIssue::SOLID_DIELECTRIC_UNSUPPORTED);
    const auto path = renderer::BuildRayScene(m_scene, {UINT32_MAX, true});
    ASSERT_EQ(path.instances.size(), 1u);
    EXPECT_TRUE(path.diagnostics.empty());
    EXPECT_TRUE(path.surfaceDiagnostics.empty());
    EXPECT_TRUE(path.geometries[0].key.opaque);
    EXPECT_TRUE(path.geometries[0].triangles.opaque);
    const auto record = renderer::MakeRaySurfaceRecord(path.instances[0].surface);
    EXPECT_EQ(record.supported, 2u);
    EXPECT_FLOAT_EQ(record.transmission, 1);
    EXPECT_FLOAT_EQ(record.ior, 1.6f);
    EXPECT_EQ(record.attenuationColor, (std::array<float, 3>{0, 0.5f, 1}));
    EXPECT_FLOAT_EQ(record.attenuationDistance, 2);
    m_scene.items[0].material.capabilities.blend = renderer::BlendMode::ALPHA_BLEND;
    const auto alphaBlend = renderer::BuildRayScene(m_scene, {UINT32_MAX, true});
    EXPECT_TRUE(alphaBlend.instances.empty());
    EXPECT_EQ(alphaBlend.diagnostics[0].issue, renderer::RaySceneIssue::TRANSPARENT_UNSUPPORTED);
}

TEST_F(RaySceneBuilderTest, HybridCandidatesSeparateGlassFromOpaqueAndAlphaGeometry)
{
    AddObject(10, 1);
    SetSurfaceConstant(12, 1);
    const auto opaque = ResolveSurface();
    renderer::SolidDielectricSettings glass;
    glass.transmission = 1;
    m_scene.items[0].material.surface = opaque;
    AddObject(20, 2);
    m_scene.items[1].material.surface = ResolveSurface(true, false, false, glass);
    m_scene.objects[1].castShadows = false;
    AddObject(30, 3);
    m_scene.items[2].material.surface = opaque;
    m_scene.items[2].material.rayCapabilities.opacity = renderer::RayOpacity::ALPHA_TEST;
    m_scene.items[2].material.doubleSided = true;
    AddObject(40, 4);
    m_scene.items[3].material.surface = opaque;
    m_scene.items[3].material.doubleSided = true;

    const auto hybrid = renderer::BuildRayScene(m_scene, {UINT32_MAX, true, true});
    ASSERT_EQ(hybrid.instances.size(), 4u);
    ASSERT_EQ(hybrid.geometries.size(), 3u);
    EXPECT_TRUE(hybrid.hybridCandidatePolicy);
    EXPECT_TRUE(hybrid.diagnostics.empty());
    EXPECT_TRUE(hybrid.surfaceDiagnostics.empty());
    for (uint32_t i = 0; i < hybrid.instances.size(); ++i) {
        const auto& instance = hybrid.instances[i];
        const auto& geometry = hybrid.geometries[instance.geometryIndex];
        EXPECT_EQ(geometry.key.opaque, i == 0u || i == 3u);
        EXPECT_EQ(geometry.triangles.opaque, geometry.key.opaque);
        EXPECT_EQ(geometry.key.doubleSided, i == 1u);
        EXPECT_EQ(instance.doubleSided, i == 1u);
        EXPECT_EQ(instance.denseInstanceId, i);
        EXPECT_EQ(instance.objectId.index, (i + 1u) * 10u);
        EXPECT_EQ(instance.mask, i == 1u ? 13u : 15u);
    }
    EXPECT_NE(hybrid.instances[0].geometryIndex, hybrid.instances[1].geometryIndex);
    EXPECT_NE(hybrid.instances[1].geometryIndex, hybrid.instances[2].geometryIndex);
    EXPECT_EQ(hybrid.instances[0].geometryIndex, hybrid.instances[3].geometryIndex);
    EXPECT_TRUE(m_scene.items[2].material.doubleSided);
    EXPECT_TRUE(m_scene.items[3].material.doubleSided);
    EXPECT_FALSE(m_scene.items[1].material.doubleSided);
}

TEST_F(RaySceneBuilderTest, HybridPolicySwitchPreservesReferenceFlagsAndCreatesDistinctKeys)
{
    AddObject(10, 1);
    SetSurfaceConstant(12, 1);
    renderer::SolidDielectricSettings glass;
    glass.transmission = 1;
    m_scene.items[0].material.surface = ResolveSurface(true, false, false, glass);
    AddObject(20, 2);
    m_scene.items[1].material.surface = ResolveSurface();
    m_scene.items[1].material.doubleSided = true;

    const auto before = renderer::BuildRayScene(m_scene, {UINT32_MAX, true});
    const auto hybrid = renderer::BuildRayScene(m_scene, {UINT32_MAX, true, true});
    const auto after = renderer::BuildRayScene(m_scene, {UINT32_MAX, true});
    ASSERT_EQ(before.instances.size(), 2u);
    ASSERT_EQ(hybrid.instances.size(), 2u);
    ASSERT_EQ(after.instances.size(), 2u);
    EXPECT_FALSE(renderer::RaySceneBuildOptions{}.hybridCandidatePolicy);
    EXPECT_FALSE(before.hybridCandidatePolicy);
    EXPECT_TRUE(hybrid.hybridCandidatePolicy);
    EXPECT_FALSE(after.hybridCandidatePolicy);
    for (uint32_t i = 0; i < before.instances.size(); ++i) {
        const auto& beforeKey = before.geometries[before.instances[i].geometryIndex].key;
        const auto& hybridKey = hybrid.geometries[hybrid.instances[i].geometryIndex].key;
        const auto& afterKey = after.geometries[after.instances[i].geometryIndex].key;
        EXPECT_TRUE(beforeKey.opaque);
        EXPECT_EQ(beforeKey.doubleSided, i == 1u);
        EXPECT_EQ(before.instances[i].doubleSided, i == 1u);
        EXPECT_EQ(afterKey, beforeKey);
        EXPECT_EQ(after.instances[i].doubleSided, before.instances[i].doubleSided);
        EXPECT_NE(hybridKey, beforeKey);
        EXPECT_EQ(hybrid.instances[i].doubleSided, i == 0u);
        EXPECT_EQ(hybrid.instances[i].surface, before.instances[i].surface);
        EXPECT_EQ(hybrid.instances[i].objectId, before.instances[i].objectId);
    }
}

TEST_F(RaySceneBuilderTest, HybridCandidatePolicyRetainsDielectricOptInAndUnsupportedDiagnostics)
{
    AddObject();
    SetSurfaceConstant(12, 1);
    renderer::SolidDielectricSettings glass;
    glass.transmission = 1;
    m_scene.items[0].material.surface = ResolveSurface(true, false, false, glass);
    const auto denied = renderer::BuildRayScene(m_scene, {UINT32_MAX, false, true});
    EXPECT_TRUE(denied.instances.empty());
    ASSERT_EQ(denied.diagnostics.size(), 1u);
    EXPECT_EQ(denied.diagnostics[0].issue, renderer::RaySceneIssue::SOLID_DIELECTRIC_UNSUPPORTED);

    for (uint32_t kind = 0; kind < 3u; ++kind) {
        SCOPED_TRACE(kind);
        glass.transmission = kind == 0u ? 0.5f : 1.0f;
        glass.ior = kind == 1u ? std::numeric_limits<float>::quiet_NaN() : 1.5f;
        glass.thinWalled = kind == 2u;
        SetSurfaceConstant(20, kind == 2u ? 0.1f : 0.0f);
        m_scene.items[0].material.surface = ResolveSurface(true, false, false, glass);
        const auto rejected = renderer::BuildRayScene(m_scene, {UINT32_MAX, true, true});
        EXPECT_TRUE(rejected.instances.empty());
        ASSERT_EQ(rejected.diagnostics.size(), 1u);
        EXPECT_EQ(rejected.diagnostics[0].issue, renderer::RaySceneIssue::SOLID_DIELECTRIC_UNSUPPORTED);
        ASSERT_EQ(rejected.surfaceDiagnostics.size(), 1u);
        EXPECT_EQ(rejected.surfaceDiagnostics[0].issue, m_scene.items[0].material.surface.issue);
    }
}

TEST_F(RaySceneBuilderTest, AcceptsRoughSolidAndSmoothThinButRejectsPartialMetalEmissiveAndAlphaDielectrics)
{
    AddObject();
    SetSurfaceConstant(12, 1);
    renderer::SolidDielectricSettings glass;
    glass.transmission = 1;
    ASSERT_TRUE(ResolveSurface(true, false, false, glass).solidDielectricSupported);
    for (const uint32_t offset : {12u, 16u, 32u}) {
        const float original = offset == 12 ? 1.0f : 0.0f;
        SetSurfaceConstant(offset, offset == 12 ? 0.5f : 0.1f);
        if (offset == 32) SetSurfaceConstant(44, 1);
        const auto invalid = ResolveSurface(true, false, false, glass);
        EXPECT_EQ(invalid.issue, renderer::SurfaceMaterialIssue::SOLID_DIELECTRIC_UNSUPPORTED);
        EXPECT_FALSE(invalid.solidDielectricSupported);
        EXPECT_FALSE(invalid.standardSurfaceSupported);
        EXPECT_EQ(renderer::MakeRaySurfaceRecord(invalid).supported, 0u);
        EXPECT_FLOAT_EQ(invalid.dielectric.transmission, 1);
        SetSurfaceConstant(offset, original);
        SetSurfaceConstant(44, 0);
    }
    glass.transmission = 0.5f;
    EXPECT_EQ(ResolveSurface(true, false, false, glass).issue, renderer::SurfaceMaterialIssue::SOLID_DIELECTRIC_UNSUPPORTED);
    glass.transmission = 1;
    SetSurfaceConstant(20, 0.1f);
    const auto rough = ResolveSurface(true, false, false, glass);
    EXPECT_TRUE(rough.solidDielectricSupported);
    EXPECT_FALSE(rough.standardSurfaceSupported);
    EXPECT_EQ(renderer::MakeRaySurfaceRecord(rough).supported, 2u);
    EXPECT_FLOAT_EQ(rough.roughness, 0.1f);
    glass.thinWalled = true;
    EXPECT_EQ(ResolveSurface(true, false, false, glass).issue, renderer::SurfaceMaterialIssue::SOLID_DIELECTRIC_UNSUPPORTED);
    SetSurfaceConstant(20, 0);
    const auto thin = ResolveSurface(true, false, false, glass);
    EXPECT_TRUE(thin.solidDielectricSupported);
    EXPECT_FALSE(thin.standardSurfaceSupported);
    EXPECT_EQ(renderer::MakeRaySurfaceRecord(thin).supported, 2u);
    EXPECT_EQ(renderer::MakeRaySurfaceRecord(thin).dielectricFlags & 1u, 1u);
    glass.attenuationColor.x = 0.5f;
    EXPECT_EQ(ResolveSurface(true, false, false, glass).issue, renderer::SurfaceMaterialIssue::SOLID_DIELECTRIC_UNSUPPORTED);
    glass.attenuationColor.x = 1;
    glass.thinWalled = false;
    EXPECT_EQ(ResolveSurface(true, true, false, glass).issue, renderer::SurfaceMaterialIssue::TEXTURE_UNSUPPORTED);
    EXPECT_EQ(ResolveSurface(true, false, true, glass).issue, renderer::SurfaceMaterialIssue::ADVANCED_LOBES_UNSUPPORTED);
    glass.ior = std::numeric_limits<float>::infinity();
    EXPECT_EQ(ResolveSurface(true, false, false, glass).issue, renderer::SurfaceMaterialIssue::INVALID_CONSTANTS);
    glass.ior = 1.5f;
    glass.attenuationDistance = 0;
    EXPECT_EQ(ResolveSurface(true, false, false, glass).issue, renderer::SurfaceMaterialIssue::INVALID_CONSTANTS);
    glass.attenuationDistance = 1;
    glass.attenuationColor.x = -0.1f;
    EXPECT_EQ(ResolveSurface(true, false, false, glass).issue, renderer::SurfaceMaterialIssue::INVALID_CONSTANTS);
    glass.attenuationColor.x = 1.1f;
    EXPECT_EQ(ResolveSurface(true, false, false, glass).issue, renderer::SurfaceMaterialIssue::INVALID_CONSTANTS);
}

TEST_F(RaySceneBuilderTest, HybridOptInAcceptsRoughSolidsAndMarksEveryAuthoredDielectric)
{
    AddObject();
    SetSurfaceConstant(12, 1);
    renderer::SolidDielectricSettings glass;
    glass.transmission = 1;
    const auto opaque = ResolveSurface();
    EXPECT_FALSE(renderer::IsHybridRayDielectricSupported(opaque));
    EXPECT_FLOAT_EQ(renderer::ResolveHybridRaySurfaceMarker(opaque), 0);
    for (float roughness : {0.0f, 0.031f, 0.032f, 0.5f, 1.0f}) {
        SCOPED_TRACE(roughness);
        SetSurfaceConstant(20, roughness);
        const auto surface = ResolveSurface(true, false, false, glass);
        ASSERT_TRUE(surface.solidDielectricSupported);
        EXPECT_TRUE(renderer::IsHybridRayDielectricSupported(surface));
        EXPECT_FLOAT_EQ(renderer::ResolveHybridRaySurfaceMarker(surface), 1.0f);
    }
    glass.thinWalled = true;
    SetSurfaceConstant(20, 0);
    EXPECT_TRUE(renderer::IsHybridRayDielectricSupported(ResolveSurface(true, false, false, glass)));
    SetSurfaceConstant(20, 0.1f);
    EXPECT_FLOAT_EQ(renderer::ResolveHybridRaySurfaceMarker(ResolveSurface(true, false, false, glass)), 2);
    SetSurfaceConstant(20, 0);
    glass.attenuationColor.x = 0.5f;
    EXPECT_FLOAT_EQ(renderer::ResolveHybridRaySurfaceMarker(ResolveSurface(true, false, false, glass)), 2);
    glass.thinWalled = false;
    glass.attenuationColor.x = 1;
    for (uint32_t unsupported = 0; unsupported < 6; ++unsupported) {
        SCOPED_TRACE(unsupported);
        auto candidate = ResolveSurface(true, false, false, glass);
        switch (unsupported) {
        case 0: candidate.dielectric.transmission = 0.5f; break;
        case 1: candidate.metallic = 0.1f; break;
        case 2: candidate.baseColor.w = 0.5f; break;
        case 3: candidate.emission.x = 1; break;
        case 4: candidate.textureMask = 1; break;
        default: candidate.dielectric.ior = std::numeric_limits<float>::quiet_NaN(); break;
        }
        EXPECT_FALSE(renderer::IsHybridRayDielectricSupported(candidate));
        EXPECT_FLOAT_EQ(renderer::ResolveHybridRaySurfaceMarker(candidate), 2);
    }
}

TEST_F(RaySceneBuilderTest, CanonicalDrawMarkerDoesNotMutateMaterialBytesOrLeakBetweenSurfaces)
{
    AddObject();
    SetSurfaceConstant(12, 1);
    SetSurfaceConstant(84, 19);
    const auto original = m_scene.items[0].material.gbufferParams;
    renderer::SolidDielectricSettings glass;
    glass.transmission = 1;
    for (uint32_t kind = 0; kind < 3; ++kind) {
        auto surface = ResolveSurface(true, false, false, kind ? glass : renderer::SolidDielectricSettings{});
        if (kind == 2) surface.dielectric.transmission = 0.5f;
        const auto packed = renderer::MakeHybridGBufferParams(original, surface);
        float marker = -1;
        std::memcpy(&marker, packed.data() + 84, sizeof(marker));
        EXPECT_FLOAT_EQ(marker, static_cast<float>(kind));
        for (uint32_t byte = 0; byte < original.size(); ++byte)
            if (byte < 84 || byte >= 88) EXPECT_EQ(packed[byte], original[byte]);
        EXPECT_EQ(m_scene.items[0].material.gbufferParams, original);
    }
}

} /// @note namespace
} /// @note namespace fbzz::tests
