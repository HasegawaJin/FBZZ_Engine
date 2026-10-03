/// @file    RayReflectionMediumCacheTests.cpp
/// @brief   Deterministic camera-air proofs, exact live buffer invalidation and conservative affine boundaries.
/// @author  Hasegawa Jin
/// @date    2026-10-02
#include <TestKit/TestKit.hpp>
#include <Graphics/RayTracing/RayReflectionMediumCache.hpp>
#include <Graphics/Renderer/IBuffer.hpp>
#include <Graphics/Renderer/Mesh.hpp>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace fbzz::tests {
namespace {

class MediumMemoryBuffer final : public renderer::IBuffer {
public:
    void Update(const void* data, size_t size) override
    {
        if (!data || size == 0 || size > bytes.size()) return;
        std::memcpy(bytes.data(), data, size);
        ++version;
    }
    size_t GetSize() const override { return capacity; }
    uint32_t GetStride() const override { return stride; }
    uint64_t GetContentVersion() const override { return version; }
    uint32_t GetBindlessUavIndex() const override { return uavIndex; }
    bool CopyData(size_t offset, size_t size, void* output) const override
    {
        ++copies;
        if (!readable || !output || offset > bytes.size() || size > bytes.size() - offset) return false;
        std::memcpy(output, bytes.data() + offset, size);
        return true;
    }
    template<typename T> void SetData(const std::vector<T>& data, uint32_t elementStride)
    {
        bytes.resize(data.size() * sizeof(T));
        std::memcpy(bytes.data(), data.data(), bytes.size());
        capacity = bytes.size(); stride = elementStride; ++version;
    }
    std::vector<uint8_t> bytes;
    size_t capacity = 0;
    uint32_t stride = 0;
    uint64_t version = 0;
    uint32_t uavIndex = renderer::INVALID_BINDLESS_INDEX;
    bool readable = true;
    mutable uint32_t copies = 0;
};

class RayReflectionMediumCacheTest : public testkit::Fixture {
protected:
    renderer::RayReflectionMediumCache m_cache;
    renderer::RayScene m_scene;
    MediumMemoryBuffer m_vertices, m_indices;
    const renderer::IBuffer* m_vertexReader = &m_vertices;
    renderer::ResourceHandle<renderer::BufferTag> m_liveVertexHandle{11, 2}, m_liveIndexHandle{12, 3};
    std::vector<renderer::Vertex> m_sourceVertices;
    std::vector<uint32_t> m_sourceIndices;

    void AddFace(std::array<math::Vector3, 4> points, const math::Vector3& normal)
    {
        const auto first = static_cast<uint32_t>(m_sourceVertices.size());
        for (auto point : points) m_sourceVertices.push_back({.position = point, .normal = normal});
        for (uint32_t index : {0u, 1u, 2u, 0u, 2u, 3u}) m_sourceIndices.push_back(first + index);
    }
    void Upload()
    {
        m_vertices.SetData(m_sourceVertices, sizeof(renderer::Vertex));
        m_indices.SetData(m_sourceIndices, 0);
        auto& key = m_scene.geometries[0].key;
        key.vertexContentVersion = m_vertices.version; key.indexContentVersion = m_indices.version;
        key.vertexCount = static_cast<uint32_t>(m_sourceVertices.size());
        key.indexCount = static_cast<uint32_t>(m_sourceIndices.size());
        SyncInput();
    }
    void SyncInput()
    {
        auto& geometry = m_scene.geometries[0];
        const auto& key = geometry.key;
        geometry.triangles = {key.vertices, key.indices, key.firstVertex, key.vertexCount,
            key.positionOffset, key.firstIndex, key.indexCount, key.opaque};
    }
    bool Air(math::Vector3 origin = {3, 0, 0})
    {
        return m_cache.IsOriginProvenAir(m_scene, [this](renderer::ResourceHandle<renderer::BufferTag> handle) -> const renderer::IBuffer* {
            if (handle == m_liveVertexHandle) return m_vertexReader;
            if (handle == m_liveIndexHandle) return &m_indices;
            return nullptr;
        }, origin);
    }
    void SetUp() override
    {
        testkit::Fixture::SetUp();
        m_scene.sceneGeneration = 7;
        renderer::RayGeometryKey key;
        key.vertices = m_liveVertexHandle; key.indices = m_liveIndexHandle; key.vertexStride = sizeof(renderer::Vertex);
        m_scene.geometries.push_back({key, {}});
        renderer::RaySceneInstance instance;
        instance.objectId = {7, 91, 13}; instance.surface.issue = renderer::SurfaceMaterialIssue::NONE;
        instance.surface.solidDielectricSupported = true; instance.surface.roughness = 0;
        instance.surface.dielectric.transmission = 1;
        m_scene.instances.push_back(instance);
        AddFace({math::Vector3{1, -1, -1}, {1, 1, -1}, {1, 1, 1}, {1, -1, 1}}, {1, 0, 0});
        AddFace({math::Vector3{-1, -1, 1}, {-1, 1, 1}, {-1, 1, -1}, {-1, -1, -1}}, {-1, 0, 0});
        AddFace({math::Vector3{-1, 1, -1}, {-1, 1, 1}, {1, 1, 1}, {1, 1, -1}}, {0, 1, 0});
        AddFace({math::Vector3{-1, -1, 1}, {-1, -1, -1}, {1, -1, -1}, {1, -1, 1}}, {0, -1, 0});
        AddFace({math::Vector3{-1, -1, 1}, {1, -1, 1}, {1, 1, 1}, {-1, 1, 1}}, {0, 0, 1});
        AddFace({math::Vector3{1, -1, -1}, {-1, -1, -1}, {-1, 1, -1}, {1, 1, -1}}, {0, 0, -1});
        Upload();
    }
};

TEST_F(RayReflectionMediumCacheTest, ProvesOnlyStrictlyOutsideOriginsAndKeepsBoundaryPaddingUnknown)
{
    EXPECT_TRUE(Air());
    EXPECT_FALSE(Air({0, 0, 0}));
    EXPECT_FALSE(Air({1, 0, 0}));
    EXPECT_FALSE(Air({std::nextafter(1.0f, 2.0f), 0, 0}));
    EXPECT_TRUE(Air({1.001f, 0, 0}));
}

TEST_F(RayReflectionMediumCacheTest, ReusesExactLocalSnapshotButRecomputesCurrentRigidWorld)
{
    ASSERT_TRUE(Air());
    const auto vertexCopies = m_vertices.copies, indexCopies = m_indices.copies;
    m_scene.instances[0].world = math::Matrix4::Translate({3, 0, 0});
    EXPECT_FALSE(Air());
    EXPECT_EQ(m_vertices.copies, vertexCopies + 2);
    EXPECT_EQ(m_indices.copies, indexCopies + 2);
    EXPECT_TRUE(Air({6, 0, 0}));
}

TEST_F(RayReflectionMediumCacheTest, UnionsEverySubmeshOfOneOwnerInsteadOfAcceptingDisjointPartBounds)
{
    auto sibling = m_scene.instances[0];
    sibling.denseInstanceId = 1; sibling.world = math::Matrix4::Translate({6, 0, 0});
    m_scene.instances.push_back(sibling);
    EXPECT_FALSE(Air());
    EXPECT_TRUE(Air({9, 0, 0}));
}

TEST_F(RayReflectionMediumCacheTest, RequiresOutsideEverySolidOwnerAndIndependentOriginsForTwoViews)
{
    auto other = m_scene.instances[0];
    other.objectId.index = 92; other.denseInstanceId = 1; other.world = math::Matrix4::Translate({6, 0, 0});
    m_scene.instances.push_back(other);
    EXPECT_TRUE(Air());
    EXPECT_FALSE(Air({6, 0, 0}));
    EXPECT_TRUE(Air({9, 0, 0}));
    EXPECT_FALSE(Air({0, 0, 0}));
}

TEST_F(RayReflectionMediumCacheTest, PreservesConservativeBoundsForShearAndNegativeNonuniformScale)
{
    auto world = math::Matrix4::Scale({-2, 3, 0.5f});
    world.m[0][1] = 2; world.m[0][3] = 4;
    m_scene.instances[0].world = world;
    EXPECT_FALSE(Air({7.5f, 0, 0}));
    EXPECT_FALSE(Air({8, 0, 0}));
    EXPECT_TRUE(Air({8.01f, 0, 0}));
    EXPECT_TRUE(Air({4, 3.01f, 0}));
}

TEST_F(RayReflectionMediumCacheTest, IncludesAbsoluteTranslationCancellationErrorInOutwardPadding)
{
    for (auto& vertex : m_sourceVertices) vertex.position.x += 10000;
    Upload();
    m_scene.instances[0].world = math::Matrix4::Translate({-10000, 0, 0});
    EXPECT_FALSE(Air({1.005f, 0, 0}));
    EXPECT_TRUE(Air({8, 0, 0}));
}

TEST_F(RayReflectionMediumCacheTest, KeepsLargeTranslationUlpNeighborhoodUnknown)
{
    m_scene.instances[0].world = math::Matrix4::Translate({1000000, 0, 0});
    EXPECT_FALSE(Air({1000001.25f, 0, 0}));
    EXPECT_TRUE(Air({1000008, 0, 0}));
}

TEST_F(RayReflectionMediumCacheTest, RejectsNonfiniteProjectiveSingularAndIllConditionedTransforms)
{
    ASSERT_TRUE(Air());
    m_scene.instances[0].world.m[0][3] = std::numeric_limits<float>::infinity(); EXPECT_FALSE(Air());
    m_scene.instances[0].world = math::Matrix4::Identity();
    m_scene.instances[0].world.m[3][0] = 1; EXPECT_FALSE(Air());
    m_scene.instances[0].world = math::Matrix4::Scale({0, 1, 1}); EXPECT_FALSE(Air());
    m_scene.instances[0].world = math::Matrix4::Scale({1e-8f, 1, 1}); EXPECT_FALSE(Air());
}

TEST_F(RayReflectionMediumCacheTest, RejectsUnrepresentableOutwardWorldBoundsAndSubnormalInputs)
{
    const float maximum = std::numeric_limits<float>::max();
    m_scene.instances[0].world = math::Matrix4::Scale({maximum, maximum, maximum});
    EXPECT_FALSE(Air({maximum, 0, 0}));
    m_scene.instances[0].world = math::Matrix4::Identity();
    EXPECT_FALSE(Air({std::numeric_limits<float>::denorm_min(), 3, 0}));
    m_scene.instances[0].world.m[0][3] = std::numeric_limits<float>::denorm_min(); EXPECT_FALSE(Air());
}

TEST_F(RayReflectionMediumCacheTest, RejectsStaleVersionsAndRebuildsChangedSameHandleGeometry)
{
    ASSERT_TRUE(Air());
    for (auto& vertex : m_sourceVertices) vertex.position.x += 3;
    m_vertices.Update(m_sourceVertices.data(), m_sourceVertices.size() * sizeof(renderer::Vertex));
    EXPECT_FALSE(Air());
    m_scene.geometries[0].key.vertexContentVersion = m_vertices.version;
    EXPECT_FALSE(Air());
    EXPECT_TRUE(Air({6, 0, 0}));
    m_indices.Update(m_sourceIndices.data(), m_sourceIndices.size() * sizeof(uint32_t));
    EXPECT_FALSE(Air({6, 0, 0}));
    m_scene.geometries[0].key.indexContentVersion = m_indices.version;
    EXPECT_TRUE(Air({6, 0, 0}));
}

TEST_F(RayReflectionMediumCacheTest, RevalidatesReaderIdentityEvenWithSameHandlesAndVersions)
{
    ASSERT_TRUE(Air());
    MediumMemoryBuffer replacement;
    for (auto& vertex : m_sourceVertices) vertex.position.x += 3;
    replacement.SetData(m_sourceVertices, sizeof(renderer::Vertex));
    ASSERT_EQ(replacement.version, m_vertices.version);
    m_vertexReader = &replacement;
    EXPECT_FALSE(Air());
    EXPECT_GT(replacement.copies, 2u);
}

TEST_F(RayReflectionMediumCacheTest, RejectsMissingReusedHandlesAndUntrackedVersions)
{
    ASSERT_TRUE(Air());
    auto& key = m_scene.geometries[0].key;
    ++key.vertices.gen; SyncInput(); EXPECT_FALSE(Air());
    m_liveVertexHandle = key.vertices; EXPECT_TRUE(Air());
    key.vertexContentVersion = 0; EXPECT_FALSE(Air());
    key.vertexContentVersion = m_vertices.version;
    key.indexContentVersion = 0; EXPECT_FALSE(Air());
}

TEST_F(RayReflectionMediumCacheTest, CachedCpuReadabilityAndNonUavOwnershipMustRemainLive)
{
    ASSERT_TRUE(Air());
    m_vertices.readable = false; EXPECT_FALSE(Air());
    m_vertices.readable = true; m_vertices.uavIndex = 5; EXPECT_FALSE(Air());
    m_vertices.uavIndex = renderer::INVALID_BINDLESS_INDEX;
    m_indices.readable = false; EXPECT_FALSE(Air());
    m_indices.readable = true; m_indices.uavIndex = 7; EXPECT_FALSE(Air());
    m_indices.uavIndex = renderer::INVALID_BINDLESS_INDEX; EXPECT_TRUE(Air());
}

TEST_F(RayReflectionMediumCacheTest, RejectsCapacityOnlyProofAndMismatchedTypedTriangleInput)
{
    ASSERT_TRUE(Air());
    const auto last = m_vertices.bytes.back(); m_vertices.bytes.pop_back(); EXPECT_FALSE(Air());
    m_vertices.bytes.push_back(last);
    ++m_scene.geometries[0].triangles.firstVertex; EXPECT_FALSE(Air());
    SyncInput(); EXPECT_TRUE(Air());
    m_vertices.stride = 16; EXPECT_FALSE(Air());
}

TEST_F(RayReflectionMediumCacheTest, ValidatesVertexIndexOffsetsAndRejectsOutOfRangeIndices)
{
    m_sourceVertices.insert(m_sourceVertices.begin(), renderer::Vertex{});
    m_sourceIndices.insert(m_sourceIndices.begin(), UINT32_MAX);
    Upload();
    auto& key = m_scene.geometries[0].key;
    key.firstVertex = 1; --key.vertexCount; key.firstIndex = 1; --key.indexCount; SyncInput();
    EXPECT_TRUE(Air());
    m_sourceIndices[1] = key.vertexCount;
    m_indices.Update(m_sourceIndices.data(), m_sourceIndices.size() * sizeof(uint32_t));
    key.indexContentVersion = m_indices.version;
    EXPECT_FALSE(Air());
}

TEST_F(RayReflectionMediumCacheTest, SupportsNonindexedClosedGeometry)
{
    std::vector<renderer::Vertex> expanded;
    for (auto index : m_sourceIndices) expanded.push_back(m_sourceVertices[index]);
    m_sourceVertices = std::move(expanded); m_sourceIndices.clear(); Upload();
    auto& key = m_scene.geometries[0].key;
    key.indices = {}; key.indexContentVersion = 0; SyncInput();
    EXPECT_TRUE(Air());
    EXPECT_FALSE(Air({0, 0, 0}));
}

TEST_F(RayReflectionMediumCacheTest, RejectsInvalidPositionsNormalsAndNonduplicateCollinearGeometry)
{
    m_sourceVertices[0].normal = {}; Upload(); EXPECT_FALSE(Air());
    m_sourceVertices[0].normal = {1, 0, 0};
    m_sourceVertices[0].position.x = std::numeric_limits<float>::quiet_NaN(); Upload(); EXPECT_FALSE(Air());
    m_sourceVertices[0].position = {1, -1, -1};
    m_sourceVertices[1].position = {1, 0, 0}; m_sourceVertices[2].position = {1, 1, 1}; Upload(); EXPECT_FALSE(Air());
}

TEST_F(RayReflectionMediumCacheTest, KeepsNearDegenerateNormalUncertaintyUnknownWithoutSceneUnitEpsilon)
{
    m_sourceVertices[0].position = {1, 0, 0}; m_sourceVertices[1].position = {1, 1, 1};
    m_sourceVertices[2].position = {1, 2, std::nextafter(2.0f, 3.0f)};
    Upload(); EXPECT_FALSE(Air());
}

TEST_F(RayReflectionMediumCacheTest, SupportsSmallWellConditionedClosedSolidsWithoutAbsoluteGeometryFloor)
{
    for (auto& vertex : m_sourceVertices) vertex.position = vertex.position * 1e-6f;
    Upload();
    EXPECT_TRUE(Air({1e-4f, 0, 0}));
    EXPECT_FALSE(Air({0, 0, 0}));
}

TEST_F(RayReflectionMediumCacheTest, RejectsNormalizedVertexNormalsWhoseInterpolatedLengthUnderflows)
{
    constexpr float tinyProjection = 2e-38f;
    for (uint32_t vertex = 16; vertex < 20; ++vertex)
        m_sourceVertices[vertex].normal = {vertex == 16 || vertex == 19 ? 1.0f : -1.0f, 0, tinyProjection};
    Upload();
    const auto middle = (m_sourceVertices[16].normal + m_sourceVertices[18].normal) * 0.5f;
    EXPECT_FLOAT_EQ(middle.x, 0);
    EXPECT_FLOAT_EQ(math::Vector3::Dot(middle, middle), 0);
    EXPECT_FALSE(Air());
}

TEST_F(RayReflectionMediumCacheTest, MeasuresNormalizedSeparationInsteadOfRawNormalAmplitude)
{
    constexpr float tinyProjection = 2e-38f;
    for (uint32_t vertex = 16; vertex < 20; ++vertex) {
        const bool positive = vertex == 16 || vertex == 19;
        const float amplitude = positive ? 1.0f : 1e10f;
        m_sourceVertices[vertex].normal = {positive ? amplitude : -amplitude, 0, tinyProjection * amplitude};
    }
    Upload();
    EXPECT_FALSE(Air());
}

TEST_F(RayReflectionMediumCacheTest, KeepsWorldNormalizedInterpolationUnknownWhenInverseTransformAmplifiesUncertainty)
{
    for (uint32_t vertex = 16; vertex < 20; ++vertex)
        m_sourceVertices[vertex].normal = {vertex == 16 || vertex == 19 ? 1.0f : -1.0f, 0, 0.01f};
    Upload();
    ASSERT_TRUE(Air());
    m_scene.instances[0].world = math::Matrix4::Scale({2, 1, 1});
    EXPECT_TRUE(Air({4, 0, 0}));
    m_scene.instances[0].world = math::Matrix4::Scale({0.001f, 1, 1});
    EXPECT_FALSE(Air());
}

TEST_F(RayReflectionMediumCacheTest, AcceptsSpherePoleDuplicateTrianglesButNotAnEntireDegenerateMesh)
{
    m_sourceVertices.clear(); m_sourceIndices.clear();
    constexpr uint32_t segments = 32, rings = segments / 2;
    constexpr float pi = 3.14159265358979323846f;
    for (uint32_t ring = 0; ring <= rings; ++ring) for (uint32_t segment = 0; segment <= segments; ++segment) {
        const float phi = pi * static_cast<float>(ring) / static_cast<float>(rings);
        const float theta = segment == segments ? 0 : 2 * pi * static_cast<float>(segment) / static_cast<float>(segments);
        const math::Vector3 normal = ring == 0 || ring == rings ? math::Vector3{0, ring == 0 ? 1.0f : -1.0f, 0}
            : math::Vector3{std::sin(phi) * std::cos(theta), std::cos(phi), std::sin(phi) * std::sin(theta)};
        m_sourceVertices.push_back({.position = normal * 0.5f, .normal = normal});
    }
    for (uint32_t ring = 0; ring < rings; ++ring) for (uint32_t segment = 0; segment < segments; ++segment) {
        const uint32_t a = ring * (segments + 1) + segment, b = (ring + 1) * (segments + 1) + segment;
        for (uint32_t index : {a, a + 1, b, b, a + 1, b + 1}) m_sourceIndices.push_back(index);
    }
    Upload(); EXPECT_TRUE(Air()); EXPECT_FALSE(Air({0, 0, 0}));
    m_sourceIndices = {0, 1, 2}; Upload(); EXPECT_FALSE(Air());
}

TEST_F(RayReflectionMediumCacheTest, RejectsMixedOpaqueThinAndMismatchedOpticsWithinOneFullOwner)
{
    auto sibling = m_scene.instances[0]; sibling.denseInstanceId = 1;
    m_scene.instances.push_back(sibling);
    ASSERT_TRUE(Air());
    auto& surface = m_scene.instances[1].surface;
    surface.dielectric.ior = 1.6f; EXPECT_FALSE(Air());
    surface = m_scene.instances[0].surface; surface.dielectric.attenuationColor.x = 0.9f; EXPECT_FALSE(Air());
    surface = m_scene.instances[0].surface; surface.dielectric.attenuationDistance = 2; EXPECT_FALSE(Air());
    surface = m_scene.instances[0].surface; surface.dielectric.thinWalled = true; EXPECT_FALSE(Air());
    surface = m_scene.instances[0].surface; surface.dielectric.transmission = 0;
    surface.solidDielectricSupported = false; surface.standardSurfaceSupported = true; EXPECT_FALSE(Air());
}

TEST_F(RayReflectionMediumCacheTest, DistinguishesOwnerGenerationsAndSceneResetBeforeReusingLocalBounds)
{
    auto opaque = m_scene.instances[0]; opaque.denseInstanceId = 1; ++opaque.objectId.generation;
    opaque.surface.dielectric.transmission = 0; opaque.surface.solidDielectricSupported = false;
    opaque.surface.standardSurfaceSupported = true; m_scene.instances.push_back(opaque);
    ASSERT_TRUE(Air());
    const auto copies = m_vertices.copies;
    ++m_scene.sceneGeneration;
    for (auto& instance : m_scene.instances) instance.objectId.sceneGeneration = m_scene.sceneGeneration;
    EXPECT_TRUE(Air()); EXPECT_GT(m_vertices.copies, copies + 2);
    m_cache.Reset();
    const auto afterGeneration = m_vertices.copies;
    EXPECT_TRUE(Air()); EXPECT_GT(m_vertices.copies, afterGeneration + 2);
}

TEST_F(RayReflectionMediumCacheTest, RejectsInvalidSolidSupportAndIncompleteSceneInsteadOfPublishingOldAir)
{
    ASSERT_TRUE(Air());
    m_scene.instances[0].surface.dielectric.ior = std::numeric_limits<float>::quiet_NaN(); EXPECT_FALSE(Air());
    m_scene.instances[0].surface.dielectric.ior = 1.5f;
    m_scene.diagnostics.push_back({}); EXPECT_FALSE(Air()); m_scene.diagnostics.clear();
    m_scene.surfaceDiagnostics.push_back({}); EXPECT_FALSE(Air()); m_scene.surfaceDiagnostics.clear();
    m_scene.instances[0].mask &= static_cast<uint8_t>(~renderer::RAY_PRIMARY_MASK); EXPECT_FALSE(Air());
    m_scene.instances[0].mask |= renderer::RAY_PRIMARY_MASK;
    ++m_scene.instances[0].denseInstanceId; EXPECT_FALSE(Air());
}

TEST_F(RayReflectionMediumCacheTest, EmptyOrThinOnlySceneProvesAirWithoutInventingSolidInterior)
{
    m_scene.instances[0].surface.dielectric.thinWalled = true;
    EXPECT_TRUE(Air({0, 0, 0}));
    m_scene.instances.clear(); EXPECT_TRUE(Air({0, 0, 0}));
    m_scene.sceneGeneration = 0; EXPECT_FALSE(Air());
}

} /// @note namespace
} /// @note namespace fbzz::tests
