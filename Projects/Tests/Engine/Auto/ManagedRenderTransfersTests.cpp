/// @file    ManagedRenderTransfersTests.cpp
/// @brief   scoped geometry page の原子性・全件転送・birth と caller fence を CPU kernel で検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-06
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Components/PresentationComponents.hpp>
#include <Engine/Scene/Components/ProceduralMeshComponent.hpp>
#include "../../../Engine/src/Scene/Managed/ManagedRenderTransfers.hpp"
#include "../../../Engine/src/Scene/Managed/ManagedServicesBridge.hpp"
#include "../../../Engine/src/Scene/Managed/ManagedObjectControls.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace fbzz::tests {
using RenderTransferStatus = scene::ManagedBridgeStatus;
using RenderTransferOp = scene::ManagedBridgeOp;

class ManagedRenderTransfersTest : public testkit::EngineFixture {
protected:
    scene::Scene m_scene;
    scene::GameObject* m_object = nullptr;
    scene::Script m_view;
    scene::managed_detail::ManagedRenderTransfers m_transfers;
    scene::managed_detail::ManagedServiceResult m_result;
    scene::ManagedBridgeRequest m_request;
    bool m_previousPlay = false;

    void SetUp() override
    {
        EngineFixture::SetUp();
        m_previousPlay = scene::Script::IsInPlayMode(); scene::Script::SetInPlayMode(true);
        m_object = &m_scene.CreateGameObject("Geometry transfer"); m_view.SetContext(&m_scene, m_object);
        auto& line = m_object->AddComponent<scene::LineRendererComponent>(); line.points = {{7, 8, 9}}; line.enabled = false;
        auto& mesh = m_object->AddComponent<scene::MeshRenderer>(); mesh.enabled = false; mesh.meshPath = "Pending.model"; mesh.meshPathDirty = true;
        m_request.ownerId = 11; m_request.instanceId = 12; m_request.generation = 13; m_request.callId = 14; m_request.integer = 15;
    }
    void TearDown() override
    {
        m_transfers.EndCall(m_request.callId); m_view.SetContext(nullptr, nullptr); m_scene.Clear();
        scene::Script::SetInPlayMode(m_previousPlay); EngineFixture::TearDown();
    }
    RenderTransferStatus Run(RenderTransferOp op, uint32_t selector, int64_t integer = 0,
        std::span<const float> values = {}, std::span<const std::string> text = {})
    {
        m_request.op = op; m_request.index = selector;
        return m_transfers.Run(m_scene, *m_object, m_view, m_request, integer, values, text, m_result);
    }
    int64_t BeginLine(uint32_t count, bool world = false)
    {
        const std::array<float, 1> mode{world ? 1.0f : 0.0f};
        EXPECT_EQ(Run(RenderTransferOp::SERVICE_LINE, 50, count, mode), RenderTransferStatus::OK);
        return m_result.integer;
    }
    int64_t BeginMesh(uint32_t vertices, uint32_t indices, bool update = false)
    {
        const std::array<float, 1> mode{update ? 1.0f : 0.0f};
        const int64_t counts = std::bit_cast<int64_t>((static_cast<uint64_t>(indices) << 32) | vertices);
        EXPECT_EQ(Run(RenderTransferOp::SERVICE_MESH, 50, counts, mode), RenderTransferStatus::OK);
        return m_result.integer;
    }
    static std::vector<float> VertexPage(uint32_t first, uint32_t count)
    {
        std::vector<float> values; values.reserve(static_cast<size_t>(count) * 15);
        for (uint32_t index = 0; index < count; ++index) {
            const std::array<float, 15> vertex{static_cast<float>(first + index), 2, 3, 0, 1, 0, 1, 0, 0, 0.25f, 0.75f, 0.1f, 0.2f, 0.3f, 0.4f};
            values.insert(values.end(), vertex.begin(), vertex.end());
        }
        return values;
    }
    void StageVertices(int64_t token, uint32_t count)
    {
        for (uint32_t offset = 0; offset < count;) {
            const uint32_t take = std::min<uint32_t>(8, count - offset);
            const auto values = VertexPage(offset, take); const std::array<std::string, 1> text{std::to_string(offset)};
            ASSERT_EQ(Run(RenderTransferOp::SERVICE_MESH, 51, token, values, text), RenderTransferStatus::OK);
            offset += take; EXPECT_EQ(m_result.integer, offset);
        }
    }
    void StageIndices(int64_t token, uint32_t count, uint32_t vertexCount)
    {
        for (uint32_t offset = 0; offset < count;) {
            const uint32_t take = std::min<uint32_t>(127, count - offset);
            std::vector<std::string> text; text.reserve(take + 1); text.push_back(std::to_string(offset));
            for (uint32_t index = 0; index < take; ++index) text.push_back(std::to_string((offset + index) % vertexCount));
            ASSERT_EQ(Run(RenderTransferOp::SERVICE_MESH, 52, token, {}, text), RenderTransferStatus::OK);
            offset += take; EXPECT_EQ(m_result.integer, offset);
        }
    }
    void ExpectOriginalLine()
    {
        const auto* line = m_object->GetComponent<scene::LineRendererComponent>();
        ASSERT_NE(line, nullptr); ASSERT_EQ(line->points.size(), 1U);
        EXPECT_VEC3_NEAR(line->points[0], (math::Vector3{7, 8, 9}), testkit::kTolerance);
        EXPECT_EQ(line->space, scene::LineSpace::Local);
    }
};

TEST_F(ManagedRenderTransfersTest, LargeLinePagesPublishEveryPointOnlyAtCommitAndPreserveEnabled)
{
    constexpr uint32_t count = 5003;
    const int64_t token = BeginLine(count, true); ASSERT_GT(token, 0);
    for (uint32_t offset = 0; offset < count;) {
        const uint32_t take = std::min<uint32_t>(42, count - offset);
        std::vector<float> values; values.reserve(take * 3);
        for (uint32_t index = 0; index < take; ++index) { values.push_back(static_cast<float>(offset + index)); values.push_back(2); values.push_back(3); }
        const std::array<std::string, 1> text{std::to_string(offset)};
        ASSERT_EQ(Run(RenderTransferOp::SERVICE_LINE, 51, token, values, text), RenderTransferStatus::OK);
        offset += take; EXPECT_EQ(m_result.integer, offset); ExpectOriginalLine();
    }
    ASSERT_EQ(Run(RenderTransferOp::SERVICE_LINE, 52, token), RenderTransferStatus::OK);
    EXPECT_EQ(m_result.integer, 1);
    const auto& line = *m_object->GetComponent<scene::LineRendererComponent>();
    ASSERT_EQ(line.points.size(), count);
    EXPECT_VEC3_NEAR(line.points.back(), (math::Vector3{5002, 2, 3}), testkit::kTolerance);
    EXPECT_EQ(line.space, scene::LineSpace::World); EXPECT_FALSE(line.enabled);
    EXPECT_EQ(Run(RenderTransferOp::SERVICE_LINE, 52, token), RenderTransferStatus::NOT_FOUND);
}

TEST_F(ManagedRenderTransfersTest, EditVisualLineAndProceduralMeshCommitUnderLiveCallbackScope)
{
    scene::Script::SetInPlayMode(false);
    const int64_t token = BeginLine(1, true);
    const std::array<float, 3> point{1, 2, 3}; const std::array<std::string, 1> first{"0"};
    ASSERT_EQ(Run(RenderTransferOp::SERVICE_LINE, 51, token, point, first), RenderTransferStatus::OK);
    ASSERT_EQ(Run(RenderTransferOp::SERVICE_LINE, 52, token), RenderTransferStatus::OK);
    const auto& line = *m_object->GetComponent<scene::LineRendererComponent>();
    ASSERT_EQ(line.points.size(), 1U); EXPECT_VEC3_NEAR(line.points[0], (math::Vector3{1, 2, 3}), testkit::kTolerance);
    EXPECT_EQ(line.space, scene::LineSpace::World);
    const int64_t mesh = BeginMesh(3, 3); StageVertices(mesh, 3); StageIndices(mesh, 3, 3);
    ASSERT_EQ(Run(RenderTransferOp::SERVICE_MESH, 53, mesh), RenderTransferStatus::OK);
    ASSERT_NE(m_object->GetComponent<scene::ProceduralMeshComponent>(), nullptr);
    EXPECT_EQ(m_object->GetComponent<scene::ProceduralMeshComponent>()->builder.IndexCount(), 3U);
    const std::array<float, 1> mode{0};
    m_request.flags = 1;
    EXPECT_EQ(Run(RenderTransferOp::SERVICE_LINE, 50, 0, mode), RenderTransferStatus::INVALID_ARGUMENT);
    EXPECT_EQ(Run(RenderTransferOp::SERVICE_MESH, 50, 0, mode), RenderTransferStatus::INVALID_ARGUMENT);
}

TEST_F(ManagedRenderTransfersTest, IncompleteOutOfOrderOversizedAndNonfiniteLinePagesLeaveLiveGeometryUnchanged)
{
    const std::array<float, 3> point{1, 2, 3};
    const std::array<std::string, 1> first{"0"}, second{"1"};
    int64_t token = BeginLine(2);
    ASSERT_EQ(Run(RenderTransferOp::SERVICE_LINE, 51, token, point, first), RenderTransferStatus::OK);
    EXPECT_EQ(Run(RenderTransferOp::SERVICE_LINE, 52, token), RenderTransferStatus::INVALID_ARGUMENT); ExpectOriginalLine();
    token = BeginLine(2);
    EXPECT_EQ(Run(RenderTransferOp::SERVICE_LINE, 51, token, point, second), RenderTransferStatus::INVALID_ARGUMENT); ExpectOriginalLine();
    token = BeginLine(43);
    const std::array<float, 129> oversized{};
    EXPECT_EQ(Run(RenderTransferOp::SERVICE_LINE, 51, token, oversized, first), RenderTransferStatus::INVALID_ARGUMENT); ExpectOriginalLine();
    token = BeginLine(1);
    const std::array<float, 3> nonfinite{1, 2, std::numeric_limits<float>::infinity()};
    EXPECT_EQ(Run(RenderTransferOp::SERVICE_LINE, 51, token, nonfinite, first), RenderTransferStatus::INVALID_ARGUMENT); ExpectOriginalLine();
    const std::array<float, 1> mode{0}; m_result.integer = 987;
    EXPECT_EQ(Run(RenderTransferOp::SERVICE_LINE, 50, INT64_MAX, mode), RenderTransferStatus::INVALID_ARGUMENT);
    EXPECT_EQ(m_result.integer, 987); ExpectOriginalLine();
}

TEST_F(ManagedRenderTransfersTest, AllScopeFieldsAndTargetBirthFenceOneReceptionWithoutMutatingOutput)
{
    const int64_t token = BeginLine(1); const auto original = m_request;
    const std::array<float, 3> point{1, 2, 3}; const std::array<std::string, 1> offset{"0"};
    for (auto field : std::array{&scene::ManagedBridgeRequest::ownerId, &scene::ManagedBridgeRequest::instanceId,
        &scene::ManagedBridgeRequest::generation, &scene::ManagedBridgeRequest::callId}) {
        m_request = original; ++(m_request.*field); m_result.integer = 789;
        EXPECT_EQ(Run(RenderTransferOp::SERVICE_LINE, 51, token, point, offset), RenderTransferStatus::INVALID_SCOPE);
        EXPECT_EQ(m_result.integer, 789); ExpectOriginalLine();
    }
    m_request = original; ++m_request.integer;
    EXPECT_EQ(Run(RenderTransferOp::SERVICE_LINE, 51, token, point, offset), RenderTransferStatus::INVALID_SCOPE);
    m_request = original;
    auto* previous = m_object; m_object = &m_scene.CreateGameObject("Other target");
    m_object->AddComponent<scene::LineRendererComponent>();
    EXPECT_EQ(Run(RenderTransferOp::SERVICE_LINE, 51, token, point, offset), RenderTransferStatus::INVALID_SCOPE);
    m_object = previous;
    m_object->RemoveComponent<scene::LineRendererComponent>();
    m_object->AddComponent<scene::LineRendererComponent>().points = {{7, 8, 9}};
    EXPECT_EQ(Run(RenderTransferOp::SERVICE_LINE, 51, token, point, offset), RenderTransferStatus::NOT_FOUND);
    ExpectOriginalLine();
}

TEST_F(ManagedRenderTransfersTest, LargeMeshPagesCommitFullVertexRepresentationAndTopologyAndClearPendingPath)
{
    constexpr uint32_t vertices = 257, indices = 771;
    const int64_t token = BeginMesh(vertices, indices); ASSERT_GT(token, 0);
    StageVertices(token, vertices); StageIndices(token, indices, vertices);
    EXPECT_EQ(m_object->GetComponent<scene::ProceduralMeshComponent>(), nullptr);
    const auto* renderer = m_object->GetComponent<scene::MeshRenderer>();
    EXPECT_EQ(renderer->meshPath, "Pending.model"); EXPECT_TRUE(renderer->meshPathDirty); EXPECT_FALSE(renderer->enabled);
    ASSERT_EQ(Run(RenderTransferOp::SERVICE_MESH, 53, token), RenderTransferStatus::OK);
    const auto& procedural = *m_object->GetComponent<scene::ProceduralMeshComponent>();
    ASSERT_EQ(procedural.builder.VertexCount(), vertices); ASSERT_EQ(procedural.builder.IndexCount(), indices);
    const auto& vertex = procedural.builder.Vertices().back();
    EXPECT_VEC3_NEAR(vertex.position, (math::Vector3{256, 2, 3}), testkit::kTolerance);
    EXPECT_VEC3_NEAR(vertex.normal, math::Vector3::UP, testkit::kTolerance);
    EXPECT_VEC3_NEAR(vertex.tangent, math::Vector3::RIGHT, testkit::kTolerance);
    EXPECT_FLOAT_EQ(vertex.uv.x, 0.25f); EXPECT_FLOAT_EQ(vertex.uv.y, 0.75f);
    EXPECT_FLOAT_EQ(vertex.color.x, 0.1f); EXPECT_FLOAT_EQ(vertex.color.w, 0.4f);
    EXPECT_EQ(procedural.builder.Indices().back(), 256U); EXPECT_EQ(procedural.dirty, scene::MeshDirty::All);
    EXPECT_TRUE(renderer->enabled); EXPECT_TRUE(procedural.enabled); EXPECT_TRUE(renderer->meshPath.empty()); EXPECT_FALSE(renderer->meshPathDirty);
}

TEST_F(ManagedRenderTransfersTest, MalformedMeshIndicesAndVerticesNeverCreateProceduralGeometry)
{
    const std::array<float, 1> mode{0};
    const int64_t nonTriangles = std::bit_cast<int64_t>((2ULL << 32) | 3);
    EXPECT_EQ(Run(RenderTransferOp::SERVICE_MESH, 50, nonTriangles, mode), RenderTransferStatus::INVALID_ARGUMENT);
    int64_t token = BeginMesh(3, 3); StageVertices(token, 3);
    const std::array<std::string, 4> outOfBounds{"0", "0", "1", "3"};
    EXPECT_EQ(Run(RenderTransferOp::SERVICE_MESH, 52, token, {}, outOfBounds), RenderTransferStatus::INVALID_ARGUMENT);
    EXPECT_EQ(m_object->GetComponent<scene::ProceduralMeshComponent>(), nullptr);
    token = BeginMesh(3, 3); StageVertices(token, 3);
    const std::array<std::string, 4> signedIndex{"0", "0", "1", "-1"};
    EXPECT_EQ(Run(RenderTransferOp::SERVICE_MESH, 52, token, {}, signedIndex), RenderTransferStatus::INVALID_ARGUMENT);
    token = BeginMesh(3, 3); const std::array<std::string, 1> offset{"0"};
    auto badVertex = VertexPage(0, 1); badVertex[14] = std::numeric_limits<float>::quiet_NaN();
    EXPECT_EQ(Run(RenderTransferOp::SERVICE_MESH, 51, token, badVertex, offset), RenderTransferStatus::INVALID_ARGUMENT);
    EXPECT_EQ(m_object->GetComponent<scene::ProceduralMeshComponent>(), nullptr);
    token = BeginMesh(3, 3); StageVertices(token, 3);
    EXPECT_EQ(Run(RenderTransferOp::SERVICE_MESH, 53, token), RenderTransferStatus::INVALID_ARGUMENT);
    EXPECT_EQ(m_object->GetComponent<scene::ProceduralMeshComponent>(), nullptr);
    EXPECT_EQ(m_object->GetComponent<scene::MeshRenderer>()->meshPath, "Pending.model");
}

TEST_F(ManagedRenderTransfersTest, VertexOnlyUpdatesPreserveTopologyEnabledAndPendingFullUpload)
{
    int64_t token = BeginMesh(3, 3); StageVertices(token, 3); StageIndices(token, 3, 3);
    ASSERT_EQ(Run(RenderTransferOp::SERVICE_MESH, 53, token), RenderTransferStatus::OK);
    auto& procedural = *m_object->GetComponent<scene::ProceduralMeshComponent>();
    auto& renderer = *m_object->GetComponent<scene::MeshRenderer>();
    procedural.enabled = false; renderer.enabled = false;
    const auto topology = procedural.builder.Indices();
    token = BeginMesh(3, 0, true);
    const auto moved = VertexPage(10, 3); const std::array<std::string, 1> first{"0"};
    ASSERT_EQ(Run(RenderTransferOp::SERVICE_MESH, 51, token, moved, first), RenderTransferStatus::OK);
    ASSERT_EQ(Run(RenderTransferOp::SERVICE_MESH, 53, token), RenderTransferStatus::OK);
    EXPECT_EQ(procedural.builder.Indices(), topology); EXPECT_EQ(procedural.dirty, scene::MeshDirty::All);
    EXPECT_FALSE(procedural.enabled); EXPECT_FALSE(renderer.enabled);
    EXPECT_FLOAT_EQ(procedural.builder.Vertices().front().position.x, 10);
    procedural.dirty = scene::MeshDirty::None;
    token = BeginMesh(3, 0, true); StageVertices(token, 3);
    ASSERT_EQ(Run(RenderTransferOp::SERVICE_MESH, 53, token), RenderTransferStatus::OK);
    EXPECT_EQ(procedural.dirty, scene::MeshDirty::Vertices);
    token = BeginMesh(3, 0, true); StageVertices(token, 3);
    m_object->RemoveComponent<scene::ProceduralMeshComponent>();
    auto& replacement = m_object->AddComponent<scene::ProceduralMeshComponent>();
    replacement.builder.Vertices().resize(3); replacement.builder.Indices() = {0, 1, 2};
    EXPECT_EQ(Run(RenderTransferOp::SERVICE_MESH, 53, token), RenderTransferStatus::INVALID_ARGUMENT);
    EXPECT_EQ(replacement.dirty, scene::MeshDirty::None);
}

TEST_F(ManagedRenderTransfersTest, AbortCallOwnerAndGenerationCleanupReclaimEveryCandidateAndResourceLimit)
{
    std::vector<int64_t> tokens;
    for (uint32_t index = 0; index < 32; ++index) tokens.push_back(BeginLine(0));
    const std::array<float, 1> mode{0};
    EXPECT_EQ(Run(RenderTransferOp::SERVICE_LINE, 50, 0, mode), RenderTransferStatus::PENDING_LIMIT);
    ASSERT_EQ(Run(RenderTransferOp::SERVICE_LINE, 53, tokens.front()), RenderTransferStatus::OK);
    EXPECT_EQ(Run(RenderTransferOp::SERVICE_LINE, 52, tokens.front()), RenderTransferStatus::NOT_FOUND);
    const int64_t replacement = BeginLine(0); ASSERT_GT(replacement, 0);
    m_transfers.EndCall(m_request.callId);
    for (int64_t token : tokens) EXPECT_EQ(Run(RenderTransferOp::SERVICE_LINE, 52, token), RenderTransferStatus::NOT_FOUND);
    EXPECT_EQ(Run(RenderTransferOp::SERVICE_LINE, 52, replacement), RenderTransferStatus::NOT_FOUND);
    int64_t token = BeginLine(0); m_transfers.CancelOwner(m_request.ownerId);
    EXPECT_EQ(Run(RenderTransferOp::SERVICE_LINE, 52, token), RenderTransferStatus::NOT_FOUND);
    token = BeginLine(0); m_transfers.RetireGeneration(m_request.generation);
    EXPECT_EQ(Run(RenderTransferOp::SERVICE_LINE, 52, token), RenderTransferStatus::NOT_FOUND);
    ExpectOriginalLine();
}

} /// @note namespace fbzz::tests
