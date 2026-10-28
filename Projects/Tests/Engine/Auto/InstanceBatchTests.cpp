/// @file    InstanceBatchTests.cpp
/// @brief   インスタンス束ねの «同じ描画とみなす» 条件を固定する。
/// @author  Hasegawa Jin
/// @date    2026-09-20
///
/// @note 束ね条件が緩むと別の材質が使われるため、全束縛の一致を検証する。
///
/// @see Docs/design/gpu-instancing.md
#include <TestKit/TestKit.hpp>

#include <Engine/Scene/Systems/RenderPasses/InstanceBatch.hpp>

namespace fbzz::tests {
namespace {

/// @brief 区別のつく値を入れた DrawCall。各テストはここから 1 枠だけ変えて比べる。
renderer::DrawCall MakeCall()
{
    renderer::DrawCall call;
    call.vertexBuffer.id     = 11;
    call.indexBuffer.id      = 12;
    call.shader.id           = 13;
    call.pipelineState.id    = 14;
    call.indexCount          = 300;
    call.vertexCount         = 100;
    call.startIndex          = 3;
    call.baseVertex          = 4;
    call.constantBuffers[0].id = 21;
    call.constantBuffers[1].id = 22;
    call.constantBuffers[2].id = 23;
    call.textures[0].id      = 31;
    call.textures[7].id      = 32;
    call.vsBuffers[0].id     = 41;
    call.psBuffers[1].id     = 42;
    return call;
}

class InstanceBatchTest : public testkit::Fixture {};

TEST_F(InstanceBatchTest, IdenticalCallsShareABatch)
{
    EXPECT_TRUE(scene::IsSameInstanceBatch(MakeCall(), MakeCall()));
}

TEST_F(InstanceBatchTest, DifferentCapturedPoseRowsDoNotShareABatch)
{
    auto first = MakeCall();
    first.deformationSource = {77, 2};
    auto second = first;
    second.preparedSkinningRow = 1;
    EXPECT_FALSE(scene::IsSameInstanceBatch(first, second));
}

/// @note b1 は per-object なので束ね条件に含めない。
TEST_F(InstanceBatchTest, ObjectConstantBufferIsIgnored)
{
    renderer::DrawCall other = MakeCall();
    other.constantBuffers[1].id = 999;
    EXPECT_TRUE(scene::IsSameInstanceBatch(MakeCall(), other));
}

/// @note インスタンス数とバッファは束ね側が決める。
TEST_F(InstanceBatchTest, InstanceFieldsAreIgnored)
{
    renderer::DrawCall other = MakeCall();
    other.instanceBuffer.id = 777;
    other.instanceCount     = 64;
    EXPECT_TRUE(scene::IsSameInstanceBatch(MakeCall(), other));
}

TEST_F(InstanceBatchTest, DifferentMeshBreaksTheBatch)
{
    renderer::DrawCall other = MakeCall();
    other.vertexBuffer.id = 99;
    EXPECT_FALSE(scene::IsSameInstanceBatch(MakeCall(), other));

    other = MakeCall();
    other.indexBuffer.id = 99;
    EXPECT_FALSE(scene::IsSameInstanceBatch(MakeCall(), other));
}

TEST_F(InstanceBatchTest, IndependentDeformedBuffersRequireTheSameSourceAndAllMaterialBindings)
{
    auto first = MakeCall();
    first.deformationSource = {77, 2};
    auto second = first;
    second.vertexBuffer = {91, 4};
    EXPECT_TRUE(scene::IsSameInstanceBatch(first, second));
    second.textures[0].id = 55;
    EXPECT_FALSE(scene::IsSameInstanceBatch(first, second));
    second = first;
    second.deformationSource.gen = 3;
    EXPECT_FALSE(scene::IsSameInstanceBatch(first, second));
    second = first;
    second.deformationSource = {};
    EXPECT_FALSE(scene::IsSameInstanceBatch(first, second));
    second = first;
    second.indirectReadBuffers.push_back({93, 1});
    EXPECT_FALSE(scene::IsSameInstanceBatch(first, second));
}

/// @note 同じメッシュでも submesh の範囲が違えば別の描画。
TEST_F(InstanceBatchTest, DifferentSubmeshRangeBreaksTheBatch)
{
    renderer::DrawCall other = MakeCall();
    other.startIndex = 99;
    EXPECT_FALSE(scene::IsSameInstanceBatch(MakeCall(), other));

    other = MakeCall();
    other.indexCount = 99;
    EXPECT_FALSE(scene::IsSameInstanceBatch(MakeCall(), other));

    other = MakeCall();
    other.baseVertex = 99;
    EXPECT_FALSE(scene::IsSameInstanceBatch(MakeCall(), other));
}

TEST_F(InstanceBatchTest, DifferentShaderOrPipelineStateBreaksTheBatch)
{
    renderer::DrawCall other = MakeCall();
    other.shader.id = 99;
    EXPECT_FALSE(scene::IsSameInstanceBatch(MakeCall(), other));

    other = MakeCall();
    other.pipelineState.id = 99;
    EXPECT_FALSE(scene::IsSameInstanceBatch(MakeCall(), other));
}

/// @note b1 以外の定数バッファは全枠で一致する必要がある。
TEST_F(InstanceBatchTest, AnyOtherConstantBufferBreaksTheBatch)
{
    for (std::size_t slot = 0; slot < renderer::DrawCall{}.constantBuffers.size(); ++slot) {
        if (slot == 1) continue;
        renderer::DrawCall other = MakeCall();
        other.constantBuffers[slot].id = 900 + static_cast<std::uint32_t>(slot);
        EXPECT_FALSE(scene::IsSameInstanceBatch(MakeCall(), other))
            << "constantBuffers[" << slot << "] の違いが見逃されている";
    }
}

/// @note テクスチャの全枠の一致を要求する。
TEST_F(InstanceBatchTest, AnyTextureSlotBreaksTheBatch)
{
    for (std::size_t slot = 0; slot < renderer::DrawCall{}.textures.size(); ++slot) {
        renderer::DrawCall other = MakeCall();
        other.textures[slot].id = 900 + static_cast<std::uint32_t>(slot);
        EXPECT_FALSE(scene::IsSameInstanceBatch(MakeCall(), other))
            << "textures[" << slot << "] の違いが見逃されている";
    }
}

TEST_F(InstanceBatchTest, StructuredBufferSlotsBreakTheBatch)
{
    renderer::DrawCall other = MakeCall();
    other.vsBuffers[1].id = 99;
    EXPECT_FALSE(scene::IsSameInstanceBatch(MakeCall(), other));

    other = MakeCall();
    other.psBuffers[0].id = 99;
    EXPECT_FALSE(scene::IsSameInstanceBatch(MakeCall(), other));
}

/// @note Velocity と ObjectMask の b1 の意味が異なっても束ね条件は共通にする。
TEST_F(InstanceBatchTest, PassSpecificObjectConstantMeaningDoesNotAffectTheCondition)
{
    renderer::DrawCall a = MakeCall();
    renderer::DrawCall b = MakeCall();
    /// @note 同じハンドルの共有 CB を指したまま、中身だけがパスごとに違う状況を表す。
    a.constantBuffers[1].id = 22;
    b.constantBuffers[1].id = 22;
    EXPECT_TRUE(scene::IsSameInstanceBatch(a, b));

    /// @note b2 は ObjectMask の申告ペイロード。ここが違えば別のシルエットなので束ねない。
    b.constantBuffers[2].id = 555;
    EXPECT_FALSE(scene::IsSameInstanceBatch(a, b));
}

TEST_F(InstanceBatchTest, LayerAndTopologyBreakTheBatch)
{
    renderer::DrawCall other = MakeCall();
    other.layer = renderer::RenderLayer::TRANSPARENT_LAYER;
    EXPECT_FALSE(scene::IsSameInstanceBatch(MakeCall(), other));

    other = MakeCall();
    other.topology = renderer::PrimitiveTopology::LINE_LIST;
    EXPECT_FALSE(scene::IsSameInstanceBatch(MakeCall(), other));
}

TEST_F(InstanceBatchTest, GpuResourceBindingsAndCommandsBreakTheBatch)
{
    const auto original = MakeCall();
    auto other = original;
    other.indirectReadTextures[0].id = 90;
    EXPECT_FALSE(scene::IsSameInstanceBatch(original, other));
    other = original; other.vsRawBuffers[1].id = 91;
    EXPECT_FALSE(scene::IsSameInstanceBatch(original, other));
    other = original; other.auxiliaryPsBuffers[1].id = 92;
    EXPECT_FALSE(scene::IsSameInstanceBatch(original, other));
    other = original; other.indirectArguments.id = 93;
    EXPECT_FALSE(scene::IsSameInstanceBatch(original, other));
    other = original; other.indirectArgumentOffset = 24;
    EXPECT_FALSE(scene::IsSameInstanceBatch(original, other));
    other = original; other.indirectCommandCount = 2;
    EXPECT_FALSE(scene::IsSameInstanceBatch(original, other));
    other = original; other.indirectInstanceOffset = true;
    EXPECT_FALSE(scene::IsSameInstanceBatch(original, other));
    other = original; other.meshGroupCountX = 1;
    EXPECT_FALSE(scene::IsSameInstanceBatch(original, other));
    other = original; other.meshGroupCountY = 1;
    EXPECT_FALSE(scene::IsSameInstanceBatch(original, other));
    other = original; other.meshGroupCountZ = 1;
    EXPECT_FALSE(scene::IsSameInstanceBatch(original, other));
}

} /// @note namespace
} /// @note namespace fbzz::tests
