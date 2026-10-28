/// @file    MeshletGeometryBuilderTests.cpp
/// @brief   CPU meshlet packing bounds, ordering, and input validation.
/// @author  Hasegawa Jin
/// @date    2026-10-10
#include <TestKit/TestKit.hpp>
#include <Graphics/Renderer/MeshletGeometry.hpp>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace fbzz::tests {
namespace {

class MeshletGeometryBuilderTest : public testkit::Fixture {};

constexpr uint32_t kVertexStride = 16;

void WritePosition(std::vector<uint8_t>& vertices, uint32_t index,
                   float x, float y, float z)
{
    const std::array<float, 3> position{ x, y, z };
    std::memcpy(vertices.data() + static_cast<size_t>(index) * kVertexStride,
                position.data(), sizeof(position));
}

std::vector<uint8_t> MakeIndexBytes(const std::vector<uint32_t>& indices)
{
    std::vector<uint8_t> bytes(indices.size() * sizeof(uint32_t));
    std::memcpy(bytes.data(), indices.data(), bytes.size());
    return bytes;
}

renderer::MeshletDescriptor ReadDescriptor(const std::vector<uint8_t>& blob,
                                           uint32_t index)
{
    renderer::MeshletDescriptor descriptor{};
    std::memcpy(&descriptor,
                blob.data() + static_cast<size_t>(index) * sizeof(descriptor),
                sizeof(descriptor));
    return descriptor;
}

uint32_t ReadUint32(const std::vector<uint8_t>& blob, size_t offset)
{
    uint32_t value = 0;
    std::memcpy(&value, blob.data() + offset, sizeof(value));
    return value;
}

TEST_F(MeshletGeometryBuilderTest, KeepsTriangleOrderDegeneratesAndBoundsAcrossMeshletSplits)
{
    std::vector<uint8_t> vertices(3u * kVertexStride);
    WritePosition(vertices, 0, -2.0f, 1.0f, 4.0f);
    WritePosition(vertices, 1, 3.0f, -5.0f, 2.0f);
    WritePosition(vertices, 2, 1.0f, 7.0f, -6.0f);
    std::vector<uint32_t> indices;
    indices.reserve(127u * 3u);
    for (uint32_t triangle = 0; triangle < 127; ++triangle) {
        if (triangle == 1)
            indices.insert(indices.end(), { 2, 2, 1 });
        else
            indices.insert(indices.end(), { 0, 1, 2 });
    }
    const std::vector<uint8_t> indexBytes = MakeIndexBytes(indices);
    std::vector<uint8_t> blob;
    uint32_t meshletCount = 0;
    ASSERT_TRUE(renderer::MeshletGeometryBuilder::Build(
        vertices.data(), vertices.size(), 3, kVertexStride,
        indexBytes.data(), indexBytes.size(), static_cast<uint32_t>(indices.size()),
        blob, meshletCount));
    ASSERT_EQ(meshletCount, 2u);

    const renderer::MeshletDescriptor first = ReadDescriptor(blob, 0);
    const renderer::MeshletDescriptor second = ReadDescriptor(blob, 1);
    EXPECT_EQ(first.vertexCount, 3u);
    EXPECT_EQ(first.primitiveCount, 126u);
    EXPECT_EQ(second.vertexCount, 3u);
    EXPECT_EQ(second.primitiveCount, 1u);
    EXPECT_EQ(first.vertexOffsetBytes, 2u * sizeof(renderer::MeshletDescriptor));
    EXPECT_EQ(first.primitiveOffsetBytes, first.vertexOffsetBytes + 6u * sizeof(uint32_t));
    EXPECT_EQ(second.vertexOffsetBytes, first.vertexOffsetBytes + 3u * sizeof(uint32_t));
    EXPECT_EQ(second.primitiveOffsetBytes,
              first.primitiveOffsetBytes + 126u * 3u * sizeof(uint32_t));
    EXPECT_FLOAT_EQ(first.boundsMin[0], -2.0f);
    EXPECT_FLOAT_EQ(first.boundsMin[1], -5.0f);
    EXPECT_FLOAT_EQ(first.boundsMin[2], -6.0f);
    EXPECT_FLOAT_EQ(first.boundsMax[0], 3.0f);
    EXPECT_FLOAT_EQ(first.boundsMax[1], 7.0f);
    EXPECT_FLOAT_EQ(first.boundsMax[2], 4.0f);
    EXPECT_FLOAT_EQ(first.boundsMin[3], 0.0f);
    EXPECT_FLOAT_EQ(first.boundsMax[3], 0.0f);

    for (uint32_t triangle = 0; triangle < 127; ++triangle) {
        const renderer::MeshletDescriptor& descriptor = triangle < 126 ? first : second;
        const uint32_t primitive = triangle < 126 ? triangle : 0u;
        const size_t primitiveAddress = descriptor.primitiveOffsetBytes +
            static_cast<size_t>(primitive) * 3u * sizeof(uint32_t);
        const std::array<uint32_t, 3> expected = triangle == 1
            ? std::array<uint32_t, 3>{ 2, 2, 1 }
            : std::array<uint32_t, 3>{ 0, 1, 2 };
        for (uint32_t corner = 0; corner < 3; ++corner) {
            const uint32_t localIndex = ReadUint32(blob,
                primitiveAddress + static_cast<size_t>(corner) * sizeof(uint32_t));
            ASSERT_LT(localIndex, descriptor.vertexCount);
            const uint32_t globalIndex = ReadUint32(blob,
                descriptor.vertexOffsetBytes + static_cast<size_t>(localIndex) * sizeof(uint32_t));
            EXPECT_EQ(globalIndex, expected[corner]);
        }
    }
}

TEST_F(MeshletGeometryBuilderTest, RejectsMisalignedStrideShortRangesBadIndicesAndNonFinitePositions)
{
    std::vector<uint8_t> vertices(4u * 16u);
    WritePosition(vertices, 0, 0.0f, 0.0f, 0.0f);
    WritePosition(vertices, 1, 1.0f, 0.0f, 0.0f);
    WritePosition(vertices, 2, 0.0f, 1.0f, 0.0f);
    WritePosition(vertices, 3, 1.0f, 1.0f, 0.0f);
    const std::vector<uint32_t> validIndices{ 0, 1, 2 };
    const std::vector<uint8_t> validIndexBytes = MakeIndexBytes(validIndices);
    std::vector<uint8_t> output{ 11, 22, 33 };
    uint32_t meshletCount = 77;
    auto expectPreservedFailure = [&](const uint8_t* vertexData, size_t vertexDataBytes,
                                      uint32_t vertexCount, uint32_t stride,
                                      const uint8_t* indexData, size_t indexDataBytes,
                                      uint32_t indexCount) {
        EXPECT_FALSE(renderer::MeshletGeometryBuilder::Build(
            vertexData, vertexDataBytes, vertexCount, stride,
            indexData, indexDataBytes, indexCount, output, meshletCount));
        EXPECT_EQ(output, (std::vector<uint8_t>{ 11, 22, 33 }));
        EXPECT_EQ(meshletCount, 77u);
    };

    expectPreservedFailure(vertices.data(), vertices.size(), 4, 13,
                           validIndexBytes.data(), validIndexBytes.size(), 3);
    expectPreservedFailure(vertices.data(), vertices.size() - 1, 4, 16,
                           validIndexBytes.data(), validIndexBytes.size(), 3);
    expectPreservedFailure(vertices.data(), vertices.size(), 4, 16,
                           validIndexBytes.data(), validIndexBytes.size() - 1, 3);
    const std::vector<uint32_t> outOfRangeIndices{ 0, 1, 4 };
    const std::vector<uint8_t> outOfRangeBytes = MakeIndexBytes(outOfRangeIndices);
    expectPreservedFailure(vertices.data(), vertices.size(), 4, 16,
                           outOfRangeBytes.data(), outOfRangeBytes.size(), 3);

    WritePosition(vertices, 1, std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f);
    expectPreservedFailure(vertices.data(), vertices.size(), 4, 16,
                           validIndexBytes.data(), validIndexBytes.size(), 3);
}

TEST_F(MeshletGeometryBuilderTest, AllowsTriangleDegeneracyAndRejectsIncompleteTriangleLists)
{
    std::vector<uint8_t> vertices(1u * kVertexStride);
    WritePosition(vertices, 0, 2.0f, 3.0f, 4.0f);
    const std::vector<uint32_t> degenerateIndices{ 0, 0, 0 };
    const std::vector<uint8_t> indexBytes = MakeIndexBytes(degenerateIndices);
    std::vector<uint8_t> blob;
    uint32_t meshletCount = 0;
    ASSERT_TRUE(renderer::MeshletGeometryBuilder::Build(
        vertices.data(), vertices.size(), 1, kVertexStride,
        indexBytes.data(), indexBytes.size(), 3, blob, meshletCount));
    ASSERT_EQ(meshletCount, 1u);
    const renderer::MeshletDescriptor descriptor = ReadDescriptor(blob, 0);
    EXPECT_EQ(descriptor.vertexCount, 1u);
    EXPECT_EQ(descriptor.primitiveCount, 1u);
    const size_t primitiveAddress = descriptor.primitiveOffsetBytes;
    EXPECT_EQ(ReadUint32(blob, primitiveAddress), 0u);
    EXPECT_EQ(ReadUint32(blob, primitiveAddress + sizeof(uint32_t)), 0u);
    EXPECT_EQ(ReadUint32(blob, primitiveAddress + 2u * sizeof(uint32_t)), 0u);

    const std::vector<uint32_t> incompleteIndices{ 0, 0 };
    const std::vector<uint8_t> incompleteBytes = MakeIndexBytes(incompleteIndices);
    const std::vector<uint8_t> previous = blob;
    EXPECT_FALSE(renderer::MeshletGeometryBuilder::Build(
        vertices.data(), vertices.size(), 1, kVertexStride,
        incompleteBytes.data(), incompleteBytes.size(), 2, blob, meshletCount));
    EXPECT_EQ(blob, previous);
    EXPECT_EQ(meshletCount, 1u);
}

TEST_F(MeshletGeometryBuilderTest, StartsANewMeshletBeforeExceedingTheUniqueVertexLimit)
{
    std::vector<uint8_t> vertices(66u * kVertexStride);
    std::vector<uint32_t> indices;
    indices.reserve(66u);
    for (uint32_t vertex = 0; vertex < 66; ++vertex) {
        WritePosition(vertices, vertex, static_cast<float>(vertex), 0.0f, 0.0f);
        indices.push_back(vertex);
    }
    const std::vector<uint8_t> indexBytes = MakeIndexBytes(indices);
    std::vector<uint8_t> blob;
    uint32_t meshletCount = 0;
    ASSERT_TRUE(renderer::MeshletGeometryBuilder::Build(
        vertices.data(), vertices.size(), 66, kVertexStride,
        indexBytes.data(), indexBytes.size(), static_cast<uint32_t>(indices.size()),
        blob, meshletCount));
    ASSERT_EQ(meshletCount, 2u);
    const renderer::MeshletDescriptor first = ReadDescriptor(blob, 0);
    const renderer::MeshletDescriptor second = ReadDescriptor(blob, 1);
    EXPECT_EQ(first.vertexCount, 63u);
    EXPECT_EQ(first.primitiveCount, 21u);
    EXPECT_EQ(second.vertexCount, 3u);
    EXPECT_EQ(second.primitiveCount, 1u);
}

}
}
