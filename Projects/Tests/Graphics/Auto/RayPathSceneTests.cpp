/// @file    RayPathSceneTests.cpp
/// @brief   発光三角形の面積・選択分布・最新 CPU 版・Path 内容変更を固定入力で検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-01
#include <TestKit/TestKit.hpp>
#include <Graphics/RayTracing/RayPathScene.hpp>
#include <Graphics/Renderer/IBuffer.hpp>
#include <cmath>
#include <cstring>
#include <limits>

namespace fbzz::tests {
namespace {

class PathMemoryBuffer final : public renderer::IBuffer {
public:
    void Update(const void* data, size_t size) override
    {
        if (!data || size == 0 || size > m_bytes.size()) return;
        std::memcpy(m_bytes.data(), data, size);
        ++m_version;
    }
    size_t GetSize() const override { return m_capacity; }
    uint32_t GetStride() const override { return m_stride; }
    uint64_t GetContentVersion() const override { return m_version; }
    bool CopyData(size_t offset, size_t size, void* output) const override
    {
        ++m_copyCount;
        if (!m_readable || !output || offset > m_bytes.size() || size > m_bytes.size() - offset) return false;
        std::memcpy(output, m_bytes.data() + offset, size);
        return true;
    }
    template<typename T>
    void SetData(const std::vector<T>& values, uint32_t stride)
    {
        m_bytes.resize(values.size() * sizeof(T));
        std::memcpy(m_bytes.data(), values.data(), m_bytes.size());
        m_capacity = m_bytes.size();
        m_stride = stride;
        m_version = 1;
    }
    std::vector<uint8_t> m_bytes;
    size_t m_capacity = 0;
    uint32_t m_stride = 0;
    uint64_t m_version = 1;
    mutable uint32_t m_copyCount = 0;
    bool m_readable = true;
};

class RayPathSceneTest : public testkit::Fixture {
protected:
    renderer::RayScene m_scene;
    renderer::RayPathLighting m_lighting;
    renderer::RayPathSceneBuilder m_builder;
    PathMemoryBuffer m_vertices;
    PathMemoryBuffer m_indices;

    void SetUp() override
    {
        testkit::Fixture::SetUp();
        m_vertices.SetData(std::vector<std::array<float, 4>>{{0, 0, 0, 9}, {2, 0, 0, 9}, {0, 3, 0, 9}}, 16);
        m_indices.SetData(std::vector<uint32_t>{0, 1, 2}, 0);
        m_scene.sceneGeneration = 7;
        m_scene.snapshotSerial = 5;
        m_scene.frameStamp = 2;
        renderer::RayGeometryKey key;
        key.vertices = {11, 2};
        key.indices = {12, 3};
        key.vertexContentVersion = key.indexContentVersion = 1;
        key.vertexStride = 16;
        key.vertexCount = key.indexCount = 3;
        m_scene.geometries.push_back({key, {key.vertices, key.indices, 0, 3, 0, 0, 3, true}});
        renderer::RaySceneInstance instance;
        instance.objectId = {7, 91, 13};
        instance.surface.standardSurfaceSupported = true;
        instance.surface.issue = renderer::SurfaceMaterialIssue::NONE;
        instance.surface.emission = {2, 2, 2};
        m_scene.instances.push_back(instance);
    }

    renderer::RayPathScene Build(bool respectArtistShadows = false)
    {
        return m_builder.Build(m_scene, [this](renderer::ResourceHandle<renderer::BufferTag> handle) -> const renderer::IBuffer* {
            if (handle == renderer::ResourceHandle<renderer::BufferTag>{11, 2}) return &m_vertices;
            if (handle == renderer::ResourceHandle<renderer::BufferTag>{12, 3}) return &m_indices;
            return nullptr;
        }, m_lighting, respectArtistShadows);
    }

    void SyncGeometryInput()
    {
        auto& geometry = m_scene.geometries[0];
        const auto& key = geometry.key;
        geometry.triangles = {key.vertices, key.indices, key.firstVertex, key.vertexCount,
            key.positionOffset, key.firstIndex, key.indexCount, key.opaque};
    }

    bool HasIssue(const renderer::RayPathScene& scene, renderer::RayPathSceneIssue issue)
    {
        for (const auto& diagnostic : scene.diagnostics)
            if (diagnostic.issue == issue) return true;
        return false;
    }

    renderer::RayLightInput Light(renderer::RayLightType type, uint32_t owner = 40)
    {
        renderer::RayLightInput light;
        light.objectId = {m_scene.sceneGeneration, owner, 13};
        light.layerMask = 1;
        light.type = type;
        return light;
    }

    void SetThinCubeEmitter()
    {
        m_vertices.SetData(std::vector<std::array<float, 4>>{
            {-1, -1, 0.01f, 0}, {1, -1, 0.01f, 0}, {1, 1, 0.01f, 0}, {-1, 1, 0.01f, 0},
            {-1, -1, -0.01f, 0}, {1, -1, -0.01f, 0}, {1, 1, -0.01f, 0}, {-1, 1, -0.01f, 0}}, 16);
        m_indices.SetData(std::vector<uint32_t>{
            0, 1, 2, 0, 2, 3, 4, 7, 6, 4, 6, 5, 4, 5, 1, 4, 1, 0,
            7, 3, 2, 7, 2, 6, 4, 0, 3, 4, 3, 7, 5, 6, 2, 5, 2, 1}, 0);
        m_scene.geometries[0].key.vertexCount = 8;
        m_scene.geometries[0].key.indexCount = 36;
        SyncGeometryInput();
    }

    void SetRectangleEmitter(float dimension)
    {
        const float half = dimension * 0.5f;
        m_vertices.SetData(std::vector<std::array<float, 4>>{
            {-half, -half, 0, 0}, {half, -half, 0, 0}, {half, half, 0, 0}, {-half, half, 0, 0}}, 16);
        m_indices.SetData(std::vector<uint32_t>{0, 1, 2, 0, 2, 3}, 0);
        m_scene.geometries[0].key.vertexCount = 4;
        m_scene.geometries[0].key.indexCount = 6;
        SyncGeometryInput();
    }
};

TEST_F(RayPathSceneTest, BuildsWorldAreaNormalAndHitIdentityForNegativeNonuniformScale)
{
    m_scene.instances[0].world = math::Matrix4::TRS({1, 2, 4}, {}, {-2, 3, 1});
    const auto path = Build();
    ASSERT_TRUE(path.coverageComplete);
    ASSERT_EQ(path.emitters.size(), 1u);
    const auto& emitter = path.emitters[0];
    EXPECT_EQ(emitter.v0, (std::array<float, 3>{1, 2, 4}));
    EXPECT_EQ(emitter.edge1, (std::array<float, 3>{-4, 0, 0}));
    EXPECT_EQ(emitter.edge2, (std::array<float, 3>{0, 9, 0}));
    EXPECT_EQ(emitter.geometricNormal, (std::array<float, 3>{0, 0, 1}));
    EXPECT_FLOAT_EQ(emitter.area, 18);
    EXPECT_EQ(emitter.instanceId, 0u);
    EXPECT_EQ(emitter.primitiveId, 0u);
    EXPECT_FLOAT_EQ(emitter.selectionPdf, 1);
    EXPECT_FLOAT_EQ(emitter.selectionCdf, 1);
}

TEST_F(RayPathSceneTest, SamplesPowerAndStoresActualCdfMassForEmissionMis)
{
    auto second = m_scene.instances[0];
    second.denseInstanceId = 1;
    second.surface.emission = {6, 6, 6};
    second.world = math::Matrix4::Translate({0, 0, 2});
    m_scene.instances.push_back(second);
    const auto path = Build();
    ASSERT_TRUE(path.coverageComplete);
    ASSERT_EQ(path.emitters.size(), 2u);
    EXPECT_FLOAT_EQ(path.emitters[0].selectionPdf, 0.25f);
    EXPECT_FLOAT_EQ(path.emitters[1].selectionPdf, 0.75f);
    EXPECT_FLOAT_EQ(path.emitters[1].selectionCdf, 1);
    EXPECT_EQ(path.emitters[1].instanceId, 1u);
    float previous = 0;
    for (const auto& emitter : path.emitters) {
        EXPECT_FLOAT_EQ(emitter.selectionPdf, emitter.selectionCdf - previous);
        const float areaPdf = emitter.selectionPdf / emitter.area;
        const float distance = 2;
        const float cosTheta = 0.5f;
        EXPECT_FLOAT_EQ(areaPdf * distance * distance / cosTheta, emitter.selectionPdf * (8.0f / emitter.area));
        previous = emitter.selectionCdf;
    }
}

TEST_F(RayPathSceneTest, UsesSubmeshVertexOffsetPositionOffsetAndRelativeIndices)
{
    m_vertices.SetData(std::vector<std::array<float, 4>>{{9, 9, 9, 9}, {8, 0, 0, 0}, {8, 2, 0, 0}, {8, 0, 3, 0}}, 16);
    m_indices.SetData(std::vector<uint32_t>{99, 2, 0, 1}, 0);
    auto& key = m_scene.geometries[0].key;
    key.firstVertex = 1;
    key.positionOffset = 4;
    key.firstIndex = 1;
    SyncGeometryInput();
    const auto path = Build();
    ASSERT_TRUE(path.coverageComplete);
    ASSERT_EQ(path.emitters.size(), 1u);
    EXPECT_FLOAT_EQ(path.emitters[0].area, 3);
    EXPECT_EQ(path.emitters[0].v0, (std::array<float, 3>{0, 3, 0}));
    EXPECT_EQ(path.emitters[0].geometricNormal, (std::array<float, 3>{0, 0, 1}));
}

TEST_F(RayPathSceneTest, NonIndexedPrimitiveIdsRemainLocalToGeometry)
{
    m_vertices.SetData(std::vector<std::array<float, 4>>{
        {0, 0, 0, 9}, {2, 0, 0, 9}, {0, 3, 0, 9},
        {0, 0, 2, 9}, {2, 0, 2, 9}, {0, 3, 2, 9}}, 16);
    auto& key = m_scene.geometries[0].key;
    key.indices = {};
    key.indexContentVersion = 0;
    key.indexCount = 0;
    key.vertexCount = 6;
    SyncGeometryInput();
    const auto path = Build();
    ASSERT_TRUE(path.coverageComplete);
    ASSERT_EQ(path.emitters.size(), 2u);
    EXPECT_EQ(path.emitters[0].primitiveId, 0u);
    EXPECT_EQ(path.emitters[1].primitiveId, 1u);
    EXPECT_FLOAT_EQ(path.emitters[1].selectionPdf, 0.5f);
}

TEST_F(RayPathSceneTest, CachesCpuTrianglesByContentVersionAndUsesLatestUpdate)
{
    const auto first = Build();
    const auto copies = m_vertices.m_copyCount + m_indices.m_copyCount;
    const auto same = Build();
    EXPECT_EQ(first.contentRevision, same.contentRevision);
    EXPECT_EQ(m_vertices.m_copyCount + m_indices.m_copyCount, copies);
    const std::array<std::array<float, 4>, 3> updated{{{0, 0, 0, 9}, {4, 0, 0, 9}, {0, 3, 0, 9}}};
    m_vertices.Update(updated.data(), sizeof(updated));
    const auto mismatched = Build();
    EXPECT_FALSE(mismatched.coverageComplete);
    EXPECT_TRUE(HasIssue(mismatched, renderer::RayPathSceneIssue::BUFFER_CONTENT_MISMATCH));
    m_scene.geometries[0].key.vertexContentVersion = 2;
    const auto changed = Build();
    ASSERT_TRUE(changed.coverageComplete);
    EXPECT_GT(changed.contentRevision, same.contentRevision);
    EXPECT_GT(m_vertices.m_copyCount + m_indices.m_copyCount, copies);
    EXPECT_FLOAT_EQ(changed.emitters[0].area, 6);
}

TEST_F(RayPathSceneTest, IgnoresExtractionSerialFrameStampPreviousWorldAndSourceItem)
{
    const auto first = Build();
    ++m_scene.snapshotSerial;
    m_scene.frameStamp = 0;
    m_scene.instances[0].previousWorld = math::Matrix4::Translate({100, 200, 300});
    m_scene.instances[0].sourceItem = 77;
    EXPECT_EQ(Build().contentRevision, first.contentRevision);
    m_builder.Reset();
    EXPECT_GT(Build().contentRevision, first.contentRevision);
}

TEST_F(RayPathSceneTest, ExactContentChangesResetRevisionWithoutRelyingOnFloatTolerance)
{
    auto previous = Build().contentRevision;
    const auto changed = [&] {
        const auto revision = Build().contentRevision;
        EXPECT_GT(revision, previous);
        previous = revision;
    };
    m_scene.instances[0].surface.roughness = std::nextafter(0.5f, 1.0f); changed();
    m_scene.instances[0].surface.baseColor.x = std::nextafter(1.0f, 2.0f); changed();
    m_scene.instances[0].world.m[0][3] = std::nextafter(0.0f, 1.0f); changed();
    m_scene.instances[0].mask &= static_cast<uint8_t>(~renderer::RAY_PRIMARY_MASK); changed();
    m_scene.layerMask = 1; changed();
    m_scene.sceneGeneration = 8; changed();
    m_lighting.directionalRadiance = {1, 2, 3}; changed();
    m_lighting.environmentRadiance = {0.1f, 0.2f, 0.3f}; changed();
    m_lighting.direction.x = std::nextafter(0.0f, 1.0f); changed();
    ++m_lighting.environmentContentVersion; changed();
}

TEST_F(RayPathSceneTest, UnsupportedReadersPartialDataInvalidIndicesAndRangesFailClosed)
{
    m_vertices.m_readable = false;
    auto path = Build();
    EXPECT_FALSE(path.coverageComplete);
    EXPECT_TRUE(path.emitters.empty());
    EXPECT_TRUE(HasIssue(path, renderer::RayPathSceneIssue::BUFFER_READER_UNSUPPORTED));
    m_vertices.m_readable = true;
    m_vertices.m_bytes.resize(32);
    EXPECT_FALSE(Build().coverageComplete);
    m_vertices.SetData(std::vector<std::array<float, 4>>{{0, 0, 0, 9}, {2, 0, 0, 9}, {0, 3, 0, 9}}, 16);
    m_indices.SetData(std::vector<uint32_t>{0, 1, 3}, 0);
    path = Build();
    EXPECT_FALSE(path.coverageComplete);
    EXPECT_TRUE(HasIssue(path, renderer::RayPathSceneIssue::INVALID_EMITTER_GEOMETRY));
    auto& key = m_scene.geometries[0].key;
    key.firstVertex = UINT32_MAX;
    key.vertexCount = 131073;
    key.vertexStride = 4294836228u;
    m_vertices.m_stride = key.vertexStride;
    m_vertices.m_capacity = 524288;
    SyncGeometryInput();
    EXPECT_FALSE(Build().coverageComplete);
}

TEST_F(RayPathSceneTest, UnrepresentableCdfMassDegenerateAndDoubleSidedEmittersFailClosed)
{
    auto second = m_scene.instances[0];
    second.denseInstanceId = 1;
    second.surface.emission = {std::numeric_limits<float>::min(), 0, 0};
    m_scene.instances.push_back(second);
    auto path = Build();
    EXPECT_FALSE(path.coverageComplete);
    EXPECT_TRUE(HasIssue(path, renderer::RayPathSceneIssue::UNREPRESENTABLE_SELECTION_PDF));
    m_scene.instances.resize(1);
    m_scene.instances[0].doubleSided = true;
    path = Build();
    EXPECT_FALSE(path.coverageComplete);
    EXPECT_TRUE(HasIssue(path, renderer::RayPathSceneIssue::EMITTER_DOUBLE_SIDED_UNSUPPORTED));
    m_scene.instances[0].doubleSided = false;
    m_vertices.SetData(std::vector<std::array<float, 4>>{{0, 0, 0, 9}, {2, 0, 0, 9}, {4, 0, 0, 9}}, 16);
    path = Build();
    EXPECT_FALSE(path.coverageComplete);
    EXPECT_TRUE(HasIssue(path, renderer::RayPathSceneIssue::INVALID_EMITTER_GEOMETRY));
}

TEST_F(RayPathSceneTest, UnsupportedSurfaceLightingAndInvalidConstantsFailClosed)
{
    m_lighting.direction = {};
    EXPECT_TRUE(Build().coverageComplete);
    m_lighting.directionalRadiance = {1, 0, 0};
    EXPECT_FALSE(Build().coverageComplete);
    m_lighting.directionalRadiance = {};
    m_scene.instances[0].surface.standardSurfaceSupported = false;
    EXPECT_FALSE(Build().coverageComplete);
    m_scene.instances[0].surface.standardSurfaceSupported = true;
    m_lighting.supported = false;
    EXPECT_FALSE(Build().coverageComplete);
    m_lighting.supported = true;
    m_lighting.environmentRadiance.x = std::numeric_limits<float>::infinity();
    auto path = Build();
    EXPECT_FALSE(path.coverageComplete);
    EXPECT_TRUE(HasIssue(path, renderer::RayPathSceneIssue::INVALID_LIGHTING));
    m_lighting.environmentRadiance = {};
    m_scene.instances[0].surface.emission = {-1, 0, 0};
    EXPECT_FALSE(Build().coverageComplete);
}

TEST_F(RayPathSceneTest, SupportsSolidDielectricAndTracksEveryOpticalParameterWithoutFrameRevision)
{
    auto& surface = m_scene.instances[0].surface;
    surface.standardSurfaceSupported = false;
    surface.solidDielectricSupported = true;
    surface.dielectric.transmission = 1;
    surface.roughness = 0;
    surface.emission = {};
    const auto initial = Build();
    ASSERT_TRUE(initial.coverageComplete);
    EXPECT_TRUE(initial.emitters.empty());
    ++m_scene.snapshotSerial; ++m_scene.frameStamp;
    EXPECT_EQ(Build().contentRevision, initial.contentRevision);
    auto previous = initial.contentRevision;
    const auto changed = [&] {
        const auto path = Build();
        EXPECT_GT(path.contentRevision, previous);
        previous = path.contentRevision;
        return path;
    };
    surface.dielectric.ior = std::nextafter(1.5f, 2.0f);
    EXPECT_TRUE(changed().coverageComplete);
    surface.dielectric.attenuationColor.x = 0.75f;
    EXPECT_TRUE(changed().coverageComplete);
    surface.dielectric.attenuationColor.y = 0.5f;
    EXPECT_TRUE(changed().coverageComplete);
    surface.dielectric.attenuationColor.z = 0;
    EXPECT_TRUE(changed().coverageComplete);
    surface.dielectric.attenuationDistance = 2;
    EXPECT_TRUE(changed().coverageComplete);
    surface.dielectric.thinWalled = true;
    EXPECT_FALSE(changed().coverageComplete);
    surface.dielectric.thinWalled = false;
    EXPECT_TRUE(changed().coverageComplete);
    surface.dielectric.transmission = 0.5f;
    EXPECT_FALSE(changed().coverageComplete);
    surface.dielectric.transmission = 1;
    EXPECT_TRUE(changed().coverageComplete);
    surface.solidDielectricSupported = false;
    EXPECT_FALSE(changed().coverageComplete);
}

TEST_F(RayPathSceneTest, CollectsEveryDirectionalPointAndSpotWithTypeSpecificUnits)
{
    m_scene.instances[0].surface.emission = {};
    m_lighting.useSceneLights = true;
    m_lighting.directionalRadiance.x = std::numeric_limits<float>::infinity();
    auto point = Light(renderer::RayLightType::POINT);
    point.position = {4, 2, -1}; point.color = {0.5f, 1, 2}; point.intensity = 2; point.range = 8;
    auto spot = Light(renderer::RayLightType::SPOT, 41);
    spot.direction = {0, 0, 4}; spot.innerCone = 10; spot.outerCone = 30;
    auto directional = Light(renderer::RayLightType::DIRECTIONAL, 42);
    directional.direction = {0, -2, 0};
    auto otherDirectional = Light(renderer::RayLightType::DIRECTIONAL, 43);
    m_lighting.lights = {point, spot, directional, otherDirectional};
    auto path = Build();
    ASSERT_TRUE(path.coverageComplete);
    ASSERT_EQ(path.deltaLights.size(), 4u);
    EXPECT_TRUE(path.emitters.empty());
    EXPECT_EQ(path.deltaLights[0].type, 0u);
    EXPECT_EQ(path.deltaLights[0].position, (std::array<float, 3>{4, 2, -1}));
    EXPECT_FLOAT_EQ(path.deltaLights[0].range, 8);
    EXPECT_NEAR(path.deltaLights[0].radiance[0], 3.14159265f, testkit::kTolerance);
    EXPECT_NEAR(path.deltaLights[0].radiance[2], 4 * 3.14159265f, testkit::kTolerance);
    EXPECT_EQ(path.deltaLights[1].type, 1u);
    EXPECT_EQ(path.deltaLights[1].direction, (std::array<float, 3>{0, 0, 1}));
    EXPECT_NEAR(path.deltaLights[1].outerCos, std::sqrt(3.0f) * 0.5f, testkit::kTolerance);
    EXPECT_EQ(path.deltaLights[2].type, 2u);
    EXPECT_EQ(path.deltaLights[3].type, 2u);
    EXPECT_EQ(path.deltaLights[2].direction, (std::array<float, 3>{0, -1, 0}));
    const auto revision = path.contentRevision;
    m_lighting.lights[0].intensity = std::nextafter(2.0f, 3.0f);
    EXPECT_GT(Build().contentRevision, revision);
}

TEST_F(RayPathSceneTest, HybridKeepsIndependentShadowStrengthAcrossEveryLightTable)
{
    m_lighting.useSceneLights = true;
    auto directional = Light(renderer::RayLightType::DIRECTIONAL);
    directional.shadowStrength = 0.25f;
    auto point = Light(renderer::RayLightType::POINT, 41);
    point.castShadows = false;
    auto spot = Light(renderer::RayLightType::SPOT, 42);
    spot.shadowStrength = 0.5f;
    auto area = Light(renderer::RayLightType::AREA, 43);
    area.shadowStrength = 0.75f;
    auto sphere = Light(renderer::RayLightType::SPHERE, 44);
    sphere.sourceRadius = 1; sphere.shadowStrength = 0.125f;
    auto tube = Light(renderer::RayLightType::TUBE, 45);
    tube.sourceRadius = 1; tube.shadowStrength = 0.375f;
    m_lighting.lights = {directional, point, spot, area, sphere, tube};
    auto path = Build(true);
    ASSERT_TRUE(path.coverageComplete);
    ASSERT_EQ(path.deltaLights.size(), 3u);
    ASSERT_EQ(path.emitters.size(), 3u);
    ASSERT_EQ(path.shapes.size(), 2u);
    EXPECT_FLOAT_EQ(path.deltaLights[0].shadowStrength, 0.25f);
    EXPECT_FLOAT_EQ(path.deltaLights[1].shadowStrength, 0);
    EXPECT_FLOAT_EQ(path.deltaLights[2].shadowStrength, 0.5f);
    EXPECT_FLOAT_EQ(path.emitters[0].shadowStrength, 1);
    EXPECT_FLOAT_EQ(path.emitters[1].shadowStrength, 0.75f);
    EXPECT_FLOAT_EQ(path.emitters[2].shadowStrength, 0.75f);
    EXPECT_FLOAT_EQ(path.shapes[0].shadowStrength, 0.125f);
    EXPECT_FLOAT_EQ(path.shapes[1].shadowStrength, 0.375f);
    const auto revision = path.contentRevision;
    m_lighting.lights[1].castShadows = true;
    EXPECT_GT(Build(true).contentRevision, revision);
    const auto changed = Build(true).contentRevision;
    m_lighting.lights[0].shadowStrength = std::nextafter(0.25f, 1.0f);
    EXPECT_GT(Build(true).contentRevision, changed);
}

TEST_F(RayPathSceneTest, HybridClampsFiniteShadowStrengthButFailsClosedOnNonfiniteInput)
{
    m_lighting.useSceneLights = true;
    auto light = Light(renderer::RayLightType::POINT);
    light.shadowStrength = 2;
    m_lighting.lights = {light};
    auto path = Build(true);
    ASSERT_TRUE(path.coverageComplete);
    ASSERT_EQ(path.deltaLights.size(), 1u);
    EXPECT_FLOAT_EQ(path.deltaLights[0].shadowStrength, 1);
    m_lighting.lights[0].shadowStrength = -2;
    path = Build(true);
    ASSERT_TRUE(path.coverageComplete);
    ASSERT_EQ(path.deltaLights.size(), 1u);
    EXPECT_FLOAT_EQ(path.deltaLights[0].shadowStrength, 0);
    for (float invalid : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
        m_lighting.lights[0].shadowStrength = invalid;
        path = Build(true);
        EXPECT_FALSE(path.coverageComplete);
        EXPECT_TRUE(HasIssue(path, renderer::RayPathSceneIssue::INVALID_LIGHT_SOURCE));
        EXPECT_TRUE(path.deltaLights.empty());
    }
}

TEST_F(RayPathSceneTest, ReferenceIgnoresArtistShadowValuesWithoutResettingPhysicalLighting)
{
    m_lighting.useSceneLights = true;
    auto point = Light(renderer::RayLightType::POINT);
    auto area = Light(renderer::RayLightType::AREA, 41);
    auto sphere = Light(renderer::RayLightType::SPHERE, 42);
    sphere.sourceRadius = 1;
    m_lighting.lights = {point, area, sphere};
    const auto baseline = Build();
    ASSERT_TRUE(baseline.coverageComplete);
    for (auto& light : m_lighting.lights) {
        light.castShadows = false;
        light.shadowStrength = std::numeric_limits<float>::quiet_NaN();
    }
    const auto unchanged = Build();
    EXPECT_TRUE(unchanged.coverageComplete);
    EXPECT_EQ(unchanged.contentRevision, baseline.contentRevision);
    EXPECT_EQ(unchanged.emitters, baseline.emitters);
    EXPECT_EQ(unchanged.deltaLights, baseline.deltaLights);
    EXPECT_EQ(unchanged.shapes, baseline.shapes);
    for (const auto& emitter : unchanged.emitters) EXPECT_FLOAT_EQ(emitter.shadowStrength, 1);
    for (const auto& delta : unchanged.deltaLights) EXPECT_FLOAT_EQ(delta.shadowStrength, 1);
    for (const auto& shape : unchanged.shapes) EXPECT_FLOAT_EQ(shape.shadowStrength, 1);
}

TEST_F(RayPathSceneTest, ReferenceIgnoresHybridMeshMasksButTracksPrimaryVisibility)
{
    auto& instance = m_scene.instances[0];
    const uint8_t originalMask = instance.mask;
    const auto reference = Build();
    ASSERT_TRUE(reference.coverageComplete);
    instance.mask &= static_cast<uint8_t>(~renderer::RAY_SHADOW_MASK);
    const auto withoutShadow = Build();
    ASSERT_TRUE(withoutShadow.coverageComplete);
    EXPECT_EQ(withoutShadow.contentRevision, reference.contentRevision);
    EXPECT_EQ(withoutShadow.emitters, reference.emitters);
    instance.mask &= static_cast<uint8_t>(~renderer::RAY_SPECULAR_MASK);
    const auto withoutSpecular = Build();
    ASSERT_TRUE(withoutSpecular.coverageComplete);
    EXPECT_EQ(withoutSpecular.contentRevision, reference.contentRevision);
    instance.mask &= static_cast<uint8_t>(~renderer::RAY_PRIMARY_MASK);
    const auto withoutPrimary = Build();
    ASSERT_TRUE(withoutPrimary.coverageComplete);
    EXPECT_GT(withoutPrimary.contentRevision, reference.contentRevision);

    instance.mask = originalMask;
    const auto hybrid = Build(true);
    ASSERT_TRUE(hybrid.coverageComplete);
    instance.mask &= static_cast<uint8_t>(~renderer::RAY_SHADOW_MASK);
    const auto hybridWithoutShadow = Build(true);
    ASSERT_TRUE(hybridWithoutShadow.coverageComplete);
    EXPECT_GT(hybridWithoutShadow.contentRevision, hybrid.contentRevision);
}

TEST_F(RayPathSceneTest, AreaProxyShadowSettingDoesNotChangeAuthoritativeRadiance)
{
    SetThinCubeEmitter();
    m_lighting.useSceneLights = true;
    auto area = Light(renderer::RayLightType::AREA, m_scene.instances[0].objectId.index);
    area.areaWidth = area.areaHeight = 2; area.intensity = 3; area.shadowStrength = 0.375f;
    m_lighting.lights = {area};
    const auto fractional = Build(true);
    ASSERT_TRUE(fractional.coverageComplete);
    ASSERT_EQ(fractional.emitters.size(), 2u);
    m_lighting.lights[0].castShadows = false;
    const auto disabled = Build(true);
    ASSERT_TRUE(disabled.coverageComplete);
    ASSERT_EQ(disabled.emitters.size(), 2u);
    const auto physical = Build();
    ASSERT_TRUE(physical.coverageComplete);
    ASSERT_EQ(physical.emitters.size(), 2u);
    for (size_t i = 0; i < 2; ++i) {
        EXPECT_FLOAT_EQ(fractional.emitters[i].shadowStrength, 0.375f);
        EXPECT_FLOAT_EQ(disabled.emitters[i].shadowStrength, 0);
        EXPECT_FLOAT_EQ(physical.emitters[i].shadowStrength, 1);
        EXPECT_EQ(fractional.emitters[i].emission, (std::array<float, 3>{3, 3, 3}));
        EXPECT_EQ(disabled.emitters[i].emission, fractional.emitters[i].emission);
        EXPECT_EQ(physical.emitters[i].emission, fractional.emitters[i].emission);
    }
}

TEST_F(RayPathSceneTest, VirtualAreaUsesTwoTrianglesOwnerRangeAndRadianceWithoutPi)
{
    m_scene.instances[0].surface.emission = {};
    m_lighting.useSceneLights = true;
    auto area = Light(renderer::RayLightType::AREA);
    area.areaWidth = 2; area.areaHeight = 4; area.intensity = 3; area.range = 9;
    area.color = {1, 0.5f, 0.25f}; area.twoSided = true;
    m_lighting.lights.push_back(area);
    const auto path = Build();
    ASSERT_TRUE(path.coverageComplete);
    ASSERT_EQ(path.emitters.size(), 2u);
    EXPECT_TRUE(path.deltaLights.empty());
    for (const auto& emitter : path.emitters) {
        EXPECT_EQ(emitter.instanceId, UINT32_MAX);
        EXPECT_EQ(emitter.flags, renderer::RAY_PATH_EMITTER_VIRTUAL | renderer::RAY_PATH_EMITTER_TWO_SIDED);
        EXPECT_EQ(emitter.objectIndex, area.objectId.index);
        EXPECT_EQ(emitter.objectGeneration, area.objectId.generation);
        EXPECT_EQ(emitter.emission, (std::array<float, 3>{3, 1.5f, 0.75f}));
        EXPECT_EQ(emitter.geometricNormal, (std::array<float, 3>{0, 0, 1}));
        EXPECT_FLOAT_EQ(emitter.area, 4);
        EXPECT_FLOAT_EQ(emitter.range, 9);
        EXPECT_FLOAT_EQ(emitter.selectionPdf, 0.5f);
    }
    EXPECT_EQ(path.emitters[0].primitiveId, 0u);
    EXPECT_EQ(path.emitters[1].primitiveId, 1u);
}

TEST_F(RayPathSceneTest, ProvenThinCubeAreaProxyReplacesOnlyForwardFaceAndRetainsZeroEmissionControl)
{
    SetThinCubeEmitter();
    m_lighting.useSceneLights = true;
    auto area = Light(renderer::RayLightType::AREA, m_scene.instances[0].objectId.index);
    area.areaWidth = area.areaHeight = 2; area.intensity = 3; area.range = 7;
    m_lighting.lights = {area};
    auto path = Build();
    ASSERT_TRUE(path.coverageComplete);
    ASSERT_EQ(path.emitters.size(), 2u);
    for (const auto& emitter : path.emitters) {
        EXPECT_EQ(emitter.flags, renderer::RAY_PATH_EMITTER_MESH_LIGHT_PROXY);
        EXPECT_EQ(emitter.instanceId, 0u);
        EXPECT_LE(emitter.primitiveId, 1u);
        EXPECT_EQ(emitter.emission, (std::array<float, 3>{3, 3, 3}));
        EXPECT_FLOAT_EQ(emitter.v0[2], 0.01f);
        EXPECT_FLOAT_EQ(emitter.range, 7);
    }
    m_lighting.lights[0].intensity = 0;
    path = Build();
    ASSERT_TRUE(path.coverageComplete);
    ASSERT_EQ(path.emitters.size(), 2u);
    for (const auto& emitter : path.emitters) {
        EXPECT_EQ(emitter.flags, renderer::RAY_PATH_EMITTER_MESH_LIGHT_PROXY);
        EXPECT_EQ(emitter.emission, (std::array<float, 3>{0, 0, 0}));
        EXPECT_FLOAT_EQ(emitter.selectionPdf, 0);
        EXPECT_FLOAT_EQ(emitter.selectionCdf, 0);
    }
}

TEST_F(RayPathSceneTest, AmbiguousAreaProxyDimensionsDuplicateCornersAndTwoSidedSolidFailClosed)
{
    SetThinCubeEmitter();
    m_lighting.useSceneLights = true;
    auto area = Light(renderer::RayLightType::AREA, m_scene.instances[0].objectId.index);
    area.areaWidth = 2.1f; area.areaHeight = 2;
    m_lighting.lights = {area};
    auto path = Build();
    EXPECT_FALSE(path.coverageComplete);
    EXPECT_TRUE(HasIssue(path, renderer::RayPathSceneIssue::AREA_MESH_BINDING_UNSUPPORTED));
    m_lighting.lights[0].areaWidth = 2;
    m_lighting.lights[0].twoSided = true;
    EXPECT_FALSE(Build().coverageComplete);
    m_lighting.lights[0].twoSided = false;
    m_indices.SetData(std::vector<uint32_t>{0, 1, 2, 0, 1, 2}, 0);
    m_scene.geometries[0].key.indexCount = 6;
    SyncGeometryInput(); m_builder.Reset();
    path = Build();
    EXPECT_FALSE(path.coverageComplete);
    EXPECT_TRUE(HasIssue(path, renderer::RayPathSceneIssue::AREA_MESH_BINDING_UNSUPPORTED));
}

TEST_F(RayPathSceneTest, NonemissiveOwnerMeshIsAProvenLuminaireRatherThanAnEmbeddedVirtualLight)
{
    SetThinCubeEmitter();
    m_scene.instances[0].surface.emission = {};
    m_lighting.useSceneLights = true;
    auto area = Light(renderer::RayLightType::AREA, m_scene.instances[0].objectId.index);
    area.areaWidth = area.areaHeight = 2; area.intensity = 4;
    m_lighting.lights = {area};
    const auto path = Build();
    ASSERT_TRUE(path.coverageComplete);
    ASSERT_EQ(path.emitters.size(), 2u);
    for (const auto& emitter : path.emitters) {
        EXPECT_EQ(emitter.instanceId, 0u);
        EXPECT_EQ(emitter.flags, renderer::RAY_PATH_EMITTER_MESH_LIGHT_PROXY);
        EXPECT_EQ(emitter.emission, (std::array<float, 3>{4, 4, 4}));
        EXPECT_FLOAT_EQ(emitter.v0[2], 0.01f);
    }
}

TEST_F(RayPathSceneTest, NonemissiveSiblingGeometryCannotHideAnUnprovenForwardBoundary)
{
    SetThinCubeEmitter();
    auto key = m_scene.geometries[0].key;
    key.firstIndex = 30; key.indexCount = 3;
    m_scene.geometries.push_back({key, {key.vertices, key.indices, 0, 8, 0, 30, 3, true}});
    auto blocker = m_scene.instances[0];
    blocker.denseInstanceId = 1; blocker.geometryIndex = 1;
    blocker.surface.emission = {};
    blocker.world = math::Matrix4::Translate({0, 0, 0.02f});
    m_scene.instances.push_back(blocker);
    m_lighting.useSceneLights = true;
    auto area = Light(renderer::RayLightType::AREA, blocker.objectId.index);
    area.areaWidth = area.areaHeight = 2;
    m_lighting.lights = {area};
    const auto path = Build();
    EXPECT_FALSE(path.coverageComplete);
    EXPECT_TRUE(path.emitters.empty());
    EXPECT_TRUE(HasIssue(path, renderer::RayPathSceneIssue::AREA_MESH_BINDING_UNSUPPORTED));
}

TEST_F(RayPathSceneTest, TinyAreaRejectsMuchLargerCubeWithoutAnAbsoluteToleranceFloor)
{
    SetThinCubeEmitter();
    m_scene.instances[0].world = math::Matrix4::Scale({5e-7f, 5e-7f, 5e-5f});
    m_lighting.useSceneLights = true;
    auto area = Light(renderer::RayLightType::AREA, m_scene.instances[0].objectId.index);
    area.areaWidth = area.areaHeight = 1e-8f;
    m_lighting.lights = {area};
    const auto path = Build();
    EXPECT_FALSE(path.coverageComplete);
    EXPECT_TRUE(path.emitters.empty());
    EXPECT_TRUE(HasIssue(path, renderer::RayPathSceneIssue::AREA_MESH_BINDING_UNSUPPORTED));
}

TEST_F(RayPathSceneTest, TinyMatchingRectangleIsProvenWithoutAnArbitraryMinimumSize)
{
    SetRectangleEmitter(1e-8f);
    m_lighting.useSceneLights = true;
    auto area = Light(renderer::RayLightType::AREA, m_scene.instances[0].objectId.index);
    area.areaWidth = area.areaHeight = 1e-8f;
    m_lighting.lights = {area};
    const auto path = Build();
    ASSERT_TRUE(path.coverageComplete);
    ASSERT_EQ(path.emitters.size(), 2u);
    for (const auto& emitter : path.emitters) {
        EXPECT_EQ(emitter.flags, renderer::RAY_PATH_EMITTER_MESH_LIGHT_PROXY);
        EXPECT_FLOAT_EQ(emitter.selectionPdf, 0.5f);
        EXPECT_NEAR(static_cast<double>(emitter.area) / 5e-17, 1, 1e-6);
    }
}

TEST_F(RayPathSceneTest, AreaProofPreservesLargeTranslationAndNegativeScaleButRejectsLostCorners)
{
    SetRectangleEmitter(2);
    m_scene.instances[0].world = math::Matrix4::TRS({100000, -100000, 100000}, {}, {-1, 1, 1});
    m_lighting.useSceneLights = true;
    auto area = Light(renderer::RayLightType::AREA, m_scene.instances[0].objectId.index);
    area.position = {100000, -100000, 100000};
    area.areaWidth = area.areaHeight = 2;
    m_lighting.lights = {area};
    const auto path = Build();
    ASSERT_TRUE(path.coverageComplete);
    ASSERT_EQ(path.emitters.size(), 2u);
    EXPECT_EQ(path.emitters[0].geometricNormal, (std::array<float, 3>{0, 0, 1}));
    m_scene.instances[0].world = math::Matrix4::TRS(area.position, {}, {5e-9f, 5e-9f, 1});
    m_lighting.lights[0].areaWidth = m_lighting.lights[0].areaHeight = 1e-8f;
    EXPECT_FALSE(Build().coverageComplete);
}

TEST_F(RayPathSceneTest, LayerFilteringPrecedesUnsupportedSourcesAndTracksOnlyIncludedLightContent)
{
    m_lighting.useSceneLights = true;
    m_scene.layerMask = 1;
    auto excluded = Light(renderer::RayLightType::SPHERE);
    excluded.layerMask = 2; excluded.intensity = -1;
    excluded.unsupportedFlags = renderer::RAY_LIGHT_UNSUPPORTED_COOKIE;
    m_lighting.lights = {excluded};
    const auto first = Build();
    ASSERT_TRUE(first.coverageComplete);
    m_lighting.lights[0].intensity = 20;
    EXPECT_EQ(Build().contentRevision, first.contentRevision);
    m_lighting.lights[0].layerMask = 1;
    const auto included = Build();
    EXPECT_FALSE(included.coverageComplete);
    EXPECT_GT(included.contentRevision, first.contentRevision);
    EXPECT_TRUE(included.emitters.empty());
    EXPECT_TRUE(HasIssue(included, renderer::RayPathSceneIssue::UNSUPPORTED_LIGHT_SOURCE));
    ASSERT_FALSE(included.diagnostics.empty());
    EXPECT_EQ(included.diagnostics[0].objectId, excluded.objectId);
}

TEST_F(RayPathSceneTest, InvalidSourceDomainOwnerAndCookieParticleCoverageFailClosed)
{
    m_lighting.useSceneLights = true;
    auto light = Light(renderer::RayLightType::POINT);
    m_lighting.lights = {light};
    for (uint32_t flags : {renderer::RAY_LIGHT_UNSUPPORTED_COOKIE, renderer::RAY_LIGHT_UNSUPPORTED_PARTICLE,
                          renderer::RAY_LIGHT_UNSUPPORTED_DAY_NIGHT}) {
        m_lighting.lights[0].unsupportedFlags = flags;
        EXPECT_TRUE(HasIssue(Build(), renderer::RayPathSceneIssue::UNSUPPORTED_LIGHT_SOURCE));
    }
    m_lighting.lights[0] = light;
    m_lighting.lights[0].range = -1;
    EXPECT_TRUE(HasIssue(Build(), renderer::RayPathSceneIssue::INVALID_LIGHT_SOURCE));
    m_lighting.lights[0] = light;
    m_lighting.lights[0].color.x = std::numeric_limits<float>::infinity();
    EXPECT_FALSE(Build().coverageComplete);
    m_lighting.lights[0] = light;
    ++m_lighting.lights[0].objectId.sceneGeneration;
    EXPECT_FALSE(Build().coverageComplete);
    m_lighting.lights = {light, light};
    EXPECT_FALSE(Build().coverageComplete);
    m_lighting.lights = {Light(renderer::RayLightType::AREA)};
    m_lighting.lights[0].areaWidth = 0;
    EXPECT_FALSE(Build().coverageComplete);
    m_lighting.lights[0].areaWidth = 1;
    m_lighting.lights[0].tangent = m_lighting.lights[0].direction;
    EXPECT_FALSE(Build().coverageComplete);
    m_lighting.lights = {Light(renderer::RayLightType::SPOT)};
    m_lighting.lights[0].innerCone = 40; m_lighting.lights[0].outerCone = 30;
    EXPECT_FALSE(Build().coverageComplete);
}

TEST_F(RayPathSceneTest, BuildsSphereAndCapsuleWithPointFluxUnitsAndActualRoundedSelectionMass)
{
    m_lighting.useSceneLights = true;
    auto sphere = Light(renderer::RayLightType::SPHERE);
    sphere.sourceRadius = 2; sphere.intensity = 8;
    auto tube = Light(renderer::RayLightType::TUBE, 41);
    tube.sourceRadius = 1; tube.sourceLength = 2; tube.intensity = 8;
    tube.tangent = {0, 5, 0}; tube.range = 30;
    m_lighting.lights = {sphere, tube};
    const auto path = Build();
    ASSERT_TRUE(path.coverageComplete);
    ASSERT_EQ(path.shapes.size(), 2u);
    EXPECT_EQ(path.deltaLights.size(), 0u);
    EXPECT_EQ(path.shapes[0].type, 0u);
    EXPECT_EQ(path.shapes[1].type, 1u);
    EXPECT_NEAR(path.shapes[0].area, 16 * 3.14159265f, 1e-5f);
    EXPECT_NEAR(path.shapes[1].area, 8 * 3.14159265f, 1e-5f);
    EXPECT_FLOAT_EQ(path.shapes[0].emission[0], 2);
    EXPECT_FLOAT_EQ(path.shapes[1].emission[0], 4);
    EXPECT_FLOAT_EQ(path.shapes[0].selectionPdf, 0.5f);
    EXPECT_FLOAT_EQ(path.shapes[1].selectionCdf - path.shapes[0].selectionCdf, path.shapes[1].selectionPdf);
    EXPECT_EQ(path.shapes[1].axis, (std::array<float, 3>{0, 1, 0}));
    EXPECT_FLOAT_EQ(path.shapes[1].halfLength, 1);
    EXPECT_FLOAT_EQ(path.shapes[1].range, 30);
    EXPECT_EQ(path.shapes[0].objectIndex, sphere.objectId.index);
}

TEST_F(RayPathSceneTest, ShapeZeroEnergyRemainsSolidAndShapeDomainAndMeshBindingFailClosed)
{
    m_lighting.useSceneLights = true;
    auto light = Light(renderer::RayLightType::TUBE);
    light.sourceRadius = 1; light.sourceLength = 0; light.intensity = 0;
    m_lighting.lights = {light};
    auto path = Build();
    ASSERT_TRUE(path.coverageComplete);
    ASSERT_EQ(path.shapes.size(), 1u);
    EXPECT_FLOAT_EQ(path.shapes[0].selectionCdf, 0);
    EXPECT_FLOAT_EQ(path.shapes[0].selectionPdf, 0);
    m_lighting.lights[0].sourceRadius = 0;
    EXPECT_TRUE(HasIssue(Build(), renderer::RayPathSceneIssue::INVALID_LIGHT_SOURCE));
    m_lighting.lights[0] = light;
    m_lighting.lights[0].sourceLength = -1;
    EXPECT_FALSE(Build().coverageComplete);
    m_lighting.lights[0] = light;
    m_lighting.lights[0].tangent = {};
    EXPECT_FALSE(Build().coverageComplete);
    m_lighting.lights[0] = light;
    m_lighting.lights[0].objectId = m_scene.instances[0].objectId;
    path = Build();
    EXPECT_TRUE(HasIssue(path, renderer::RayPathSceneIssue::SHAPE_MESH_BINDING_UNSUPPORTED));
    EXPECT_TRUE(path.shapes.empty());
}

TEST_F(RayPathSceneTest, ShapeTextureAndEnvironmentSamplingInputsAdvanceContentRevisionWithoutSnapshotReset)
{
    m_lighting.useSceneLights = true;
    auto sphere = Light(renderer::RayLightType::SPHERE);
    sphere.sourceRadius = 1;
    m_lighting.lights = {sphere};
    auto revision = Build().contentRevision;
    ++m_scene.snapshotSerial;
    EXPECT_EQ(Build().contentRevision, revision);
    m_lighting.lights[0].sourceRadius = 2;
    auto next = Build().contentRevision;
    EXPECT_GT(next, revision); revision = next;
    m_scene.instances[0].surface.textures[0].texture = {27, 3};
    m_scene.instances[0].surface.textures[0].contentVersion = 9;
    next = Build().contentRevision; EXPECT_GT(next, revision); revision = next;
    ++m_scene.instances[0].surface.textures[0].contentVersion;
    next = Build().contentRevision; EXPECT_GT(next, revision); revision = next;
    m_scene.instances[0].surface.uvOffset[0] = 0.25f;
    next = Build().contentRevision; EXPECT_GT(next, revision); revision = next;
    m_lighting.environmentRotation = 0.5f;
    next = Build().contentRevision; EXPECT_GT(next, revision); revision = next;
    m_lighting.environmentIntensity = 2;
    EXPECT_GT(Build().contentRevision, revision);
    m_lighting.environmentIntensity = -1;
    EXPECT_FALSE(Build().coverageComplete);
}

TEST_F(RayPathSceneTest, MaskedAreaOwnerIsNotProvenAsAFullCoverageRectangle)
{
    SetRectangleEmitter(2);
    m_scene.geometries[0].key.opaque = m_scene.geometries[0].triangles.opaque = false;
    m_lighting.useSceneLights = true;
    auto area = Light(renderer::RayLightType::AREA, m_scene.instances[0].objectId.index);
    area.areaWidth = area.areaHeight = 2;
    m_lighting.lights = {area};
    EXPECT_TRUE(HasIssue(Build(), renderer::RayPathSceneIssue::AREA_MESH_BINDING_UNSUPPORTED));
}

TEST_F(RayPathSceneTest, TexturedMeshEmissionKeepsFullTriangleBaseEstimateButAreaProxyRejectsTheTexture)
{
    auto& surface = m_scene.instances[0].surface;
    surface.textureMask = 8;
    surface.textures[3].texture = {28, 3};
    surface.textures[3].contentVersion = 1;
    const auto ordinary = Build();
    ASSERT_TRUE(ordinary.coverageComplete);
    ASSERT_EQ(ordinary.emitters.size(), 1u);
    EXPECT_FLOAT_EQ(ordinary.emitters[0].selectionPdf, 1);
    EXPECT_EQ(ordinary.emitters[0].emission, (std::array<float, 3>{2, 2, 2}));
    const auto revision = ordinary.contentRevision;
    ++surface.textures[3].contentVersion;
    const auto updated = Build();
    ASSERT_TRUE(updated.coverageComplete);
    EXPECT_EQ(updated.emitters, ordinary.emitters);
    EXPECT_GT(updated.contentRevision, revision);
    SetRectangleEmitter(2);
    m_lighting.useSceneLights = true;
    auto area = Light(renderer::RayLightType::AREA, m_scene.instances[0].objectId.index);
    area.areaWidth = area.areaHeight = 2;
    m_lighting.lights = {area};
    EXPECT_TRUE(HasIssue(Build(), renderer::RayPathSceneIssue::AREA_MESH_BINDING_UNSUPPORTED));
}

} /// @note namespace
} /// @note namespace fbzz::tests
