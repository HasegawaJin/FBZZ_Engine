/// @file    FiberPlacementTests.cpp
/// @brief   大量の草、UV 独立ペイント、差分更新と保存互換を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-11
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Engine/Asset/FiberPaintAsset.hpp>
#include <Engine/Renderer/Mesh.hpp>
#include <Engine/Scene/Components/FiberComponent.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/SceneSerializer.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Graphics/Renderer/FiberPlacement.hpp>
#include <limits>
#include <set>

namespace fbzz::tests {
class FiberPlacementTest : public testkit::EngineFixture {};
namespace {
renderer::Mesh Ground(float size = 4)
{
    renderer::Mesh mesh; mesh.cpuVertices.resize(4);
    mesh.cpuVertices[0].position = {0,0,0}; mesh.cpuVertices[1].position = {size,0,0};
    mesh.cpuVertices[2].position = {0,0,size}; mesh.cpuVertices[3].position = {size,0,size};
    for (auto& vertex : mesh.cpuVertices) vertex.normal = {0,1,0};
    mesh.cpuIndices = {0,2,1,1,2,3}; mesh.ComputeBounds();
    return mesh;
}
std::set<std::array<float,3>> Positions(const renderer::FiberPlacementCache& cache)
{
    std::set<std::array<float,3>> result;
    for (const auto& chunk : cache.chunks) for (const auto& root : chunk.roots)
        result.insert({root.m_position.x,root.m_position.y,root.m_position.z});
    return result;
}
} /// @note namespace

TEST_F(FiberPlacementTest, PlacesMoreThanLegacyMeshLimitAcrossChunks)
{
    const auto mesh = Ground(20);
    renderer::FiberPaintData paint;
    ASSERT_TRUE(renderer::CreateFiberPaintData(mesh,0.5f,1,paint));
    renderer::FiberPlacementOptions options; options.density = 250;
    renderer::FiberPlacementCache cache;
    ASSERT_TRUE(renderer::UpdateFiberPlacement(mesh,paint,options,cache));
    EXPECT_GT(cache.bladeCount,renderer::FIBER_MAX_BLADES);
    EXPECT_NEAR(cache.bladeCount,100000u,200u);
    EXPECT_GT(cache.chunks.size(),1u);
    for (const auto& chunk : cache.chunks) EXPECT_LE(chunk.roots.size(),renderer::FIBER_MAX_BLADES);
}
TEST_F(FiberPlacementTest, EmptyPaintDoesNotAllocateRootsAndBrushIsIdempotent)
{
    const auto mesh = Ground();
    renderer::FiberPaintData paint;
    ASSERT_TRUE(renderer::CreateFiberPaintData(mesh,0.5f,0,paint));
    renderer::FiberPlacementCache cache; renderer::FiberPlacementOptions options;
    ASSERT_TRUE(renderer::UpdateFiberPlacement(mesh,paint,options,cache));
    EXPECT_EQ(cache.bladeCount,0u);
    renderer::FiberPaintStroke stroke; stroke.from = stroke.to = {1,0,1}; stroke.radius = 0.9f;
    uint32_t changed = 0;
    ASSERT_TRUE(renderer::PaintFiberSurface(mesh,options.world,stroke,paint,changed));
    EXPECT_GT(changed,0u);
    const auto first = paint;
    ASSERT_TRUE(renderer::PaintFiberSurface(mesh,options.world,stroke,paint,changed));
    EXPECT_EQ(changed,0u); EXPECT_TRUE(paint == first);
    ASSERT_TRUE(renderer::UpdateFiberPlacement(mesh,paint,options,cache));
    EXPECT_GT(cache.bladeCount,0u); EXPECT_LT(cache.bladeCount,500u);
    stroke.target = 0;
    ASSERT_TRUE(renderer::PaintFiberSurface(mesh,options.world,stroke,paint,changed));
    const auto erased = paint;
    ASSERT_TRUE(renderer::PaintFiberSurface(mesh,options.world,stroke,paint,changed));
    EXPECT_TRUE(paint == erased); EXPECT_EQ(changed,0u);
}
TEST_F(FiberPlacementTest, BuildsApproximatelyOneMillionBladesWithinAnExplicitBudget)
{
    const auto mesh = Ground(50);
    renderer::FiberPaintData paint;
    ASSERT_TRUE(renderer::CreateFiberPaintData(mesh,0.5f,1,paint));
    renderer::FiberPlacementOptions options; options.density = 400; options.maxBlades = 1100000;
    renderer::FiberPlacementCache cache;
    ASSERT_TRUE(renderer::UpdateFiberPlacement(mesh,paint,options,cache));
    EXPECT_NEAR(cache.bladeCount,1000000u,500u);
    EXPECT_GT(cache.chunks.size(),100u);
    EXPECT_LE(cache.bladeCount,options.maxBlades);
}
TEST_F(FiberPlacementTest, UpdatesOnlyTouchedChunksAndKeepsOtherRootPositions)
{
    const auto mesh = Ground(12);
    renderer::FiberPaintData paint;
    ASSERT_TRUE(renderer::CreateFiberPaintData(mesh,0.5f,1,paint));
    renderer::FiberPlacementOptions options; options.density = 40;
    renderer::FiberPlacementCache cache;
    ASSERT_TRUE(renderer::UpdateFiberPlacement(mesh,paint,options,cache));
    std::vector<uint64_t> revisions; for (const auto& chunk : cache.chunks) revisions.push_back(chunk.revision);
    const auto before = Positions(cache);
    renderer::FiberPaintStroke stroke; stroke.from = stroke.to = {1,0,1}; stroke.radius = 1; stroke.target = 0;
    uint32_t changed = 0;
    ASSERT_TRUE(renderer::PaintFiberSurface(mesh,options.world,stroke,paint,changed));
    ASSERT_TRUE(renderer::UpdateFiberPlacement(mesh,paint,options,cache));
    size_t same = 0, updated = 0;
    for (size_t i = 0; i < cache.chunks.size(); ++i) { if (cache.chunks[i].revision == revisions[i]) ++same; else ++updated; }
    EXPECT_GT(same,0u); EXPECT_GT(updated,0u);
    for (const auto& position : Positions(cache)) EXPECT_TRUE(before.contains(position));
    const auto revision = cache.revision;
    ASSERT_TRUE(renderer::UpdateFiberPlacement(mesh,paint,options,cache));
    EXPECT_EQ(cache.revision,revision);
}
TEST_F(FiberPlacementTest, WorldDensityAndWidthRespectNonUniformScale)
{
    const auto mesh = Ground(1);
    renderer::FiberPaintData paint;
    ASSERT_TRUE(renderer::CreateFiberPaintData(mesh,0.5f,1,paint));
    renderer::FiberPlacementOptions options; options.density = 100;
    options.world.m[0][0] = 4; options.world.m[2][2] = 2;
    renderer::FiberPlacementCache cache;
    ASSERT_TRUE(renderer::UpdateFiberPlacement(mesh,paint,options,cache));
    EXPECT_NEAR(cache.bladeCount,800u,20u);
    for (const auto& chunk : cache.chunks) for (const auto& root : chunk.roots) {
        const auto width = options.world*math::Vector4{root.m_side0.x,root.m_side0.y,root.m_side0.z,0};
        EXPECT_NEAR(math::Vector3(width.x,width.y,width.z).Length(),options.width*0.5f,1.0e-6f);
    }
}
TEST_F(FiberPlacementTest, OverlappingUvSurfacesHaveIndependentPaint)
{
    auto mesh = Ground(2);
    const auto top = mesh.cpuVertices;
    for (auto vertex : top) { vertex.position.y = -0.2f; mesh.cpuVertices.push_back(vertex); }
    mesh.cpuIndices.insert(mesh.cpuIndices.end(),{4,6,5,5,6,7});
    renderer::FiberPaintData paint;
    ASSERT_TRUE(renderer::CreateFiberPaintData(mesh,0.25f,0,paint));
    renderer::FiberPaintStroke stroke; stroke.from = stroke.to = {1,0,1}; stroke.radius = 2;
    uint32_t changed = 0;
    ASSERT_TRUE(renderer::PaintFiberSurface(mesh,math::Matrix4::Identity(),stroke,paint,changed,
        [](const auto&,uint32_t triangle) { return triangle < 2; }));
    renderer::FiberPlacementCache cache;
    ASSERT_TRUE(renderer::UpdateFiberPlacement(mesh,paint,{},cache));
    EXPECT_GT(cache.bladeCount,0u);
    for (const auto& chunk : cache.chunks) for (const auto& root : chunk.roots) EXPECT_NEAR(root.m_position.y,0.0f,1.0e-6f);
}
TEST_F(FiberPlacementTest, RejectsBudgetTopologyAndNonFiniteInputWithoutReplacingCache)
{
    auto mesh = Ground(); renderer::FiberPaintData paint;
    ASSERT_TRUE(renderer::CreateFiberPaintData(mesh,0.5f,1,paint));
    renderer::FiberPlacementCache cache; renderer::FiberPlacementOptions options;
    ASSERT_TRUE(renderer::UpdateFiberPlacement(mesh,paint,options,cache));
    const auto positions = Positions(cache); const auto revision = cache.revision;
    options.maxBlades = 1;
    EXPECT_FALSE(renderer::UpdateFiberPlacement(mesh,paint,options,cache));
    EXPECT_EQ(cache.revision,revision); EXPECT_EQ(Positions(cache),positions);
    options.maxBlades = 1000000; options.width = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(renderer::UpdateFiberPlacement(mesh,paint,options,cache));
    options.width = 0.015f; mesh.cpuVertices[0].position.x += 0.1f;
    EXPECT_FALSE(renderer::UpdateFiberPlacement(mesh,paint,options,cache));
    EXPECT_EQ(Positions(cache),positions);
}
TEST_F(FiberPlacementTest, SavesPaintAndRejectsMalformedDataWithoutOverwritingOutput)
{
    testkit::TempDir temp{"fiber-paint"}; const auto file = temp.File("Ground.fiberpaint").generic_string();
    asset::FiberPaintAsset source;
    ASSERT_TRUE(renderer::CreateFiberPaintData(Ground(),0.5f,0,source.data));
    source.data.cells.emplace(0,renderer::FiberPaintValue{0.7f,0.4f});
    ASSERT_TRUE(asset::SaveFiberPaintAssetToFile(file,source));
    asset::FiberPaintAsset restored;
    ASSERT_TRUE(asset::LoadFiberPaintAssetFromFile(file,restored));
    EXPECT_TRUE(source.data == restored.data); EXPECT_NE(restored.revision,0u);
    ASSERT_TRUE(util::FileSystem::WriteTextAtomic(file,"version=99\n"));
    const auto before = restored;
    EXPECT_FALSE(asset::LoadFiberPaintAssetFromFile(file,restored));
    EXPECT_TRUE(restored.data == before.data); EXPECT_EQ(restored.revision,before.revision);
}
TEST_F(FiberPlacementTest, SavesNewComponentSettingsAndPreservesLegacyDefaults)
{
    testkit::TempDir temp{"fiber-component"}; scene::Scene scene;
    auto& go = scene.CreateGameObject("Ground");
    auto& fiber = go.AddComponent<scene::FiberComponent>();
    fiber.m_chunkedPlacement = true; fiber.m_chunkSize = 6; fiber.m_maxSlope = 30; fiber.m_maxBlades = 500000;
    fiber.m_paintPath = "Assets/Fiber/Ground.fiberpaint";
    const auto file = temp.File("Fiber.scene").generic_string();
    ASSERT_TRUE(scene::SceneSerializer::Save(scene,file));
    const auto loaded = scene::SceneSerializer::LoadData(file);
    ASSERT_TRUE(loaded); auto* target = loaded->FindByGuid(go.instanceId);
    ASSERT_TRUE(target); const auto* result = target->GetComponent<scene::FiberComponent>(); ASSERT_TRUE(result);
    EXPECT_TRUE(result->m_chunkedPlacement); EXPECT_FLOAT_EQ(result->m_chunkSize,6); EXPECT_FLOAT_EQ(result->m_maxSlope,30);
    EXPECT_EQ(result->m_maxBlades,500000); EXPECT_EQ(result->m_paintPath,fiber.m_paintPath);
    EXPECT_FALSE(scene::FiberComponent{}.m_chunkedPlacement);
}
} /// @note namespace fbzz::tests
