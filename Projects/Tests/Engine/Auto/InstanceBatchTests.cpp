/// @file    InstanceBatchTests.cpp
/// @brief   インスタンス束ねの «同じ描画とみなす» 条件を固定する。
/// @author  Hasegawa Jin
/// @date    2026-09-20
///
/// 束ね条件が緩むと、別のテクスチャ・別の定数バッファで描かれた絵が出る。絵を見ても
/// «なぜこの物体だけ色が違うのか» に辿りつけない壊れ方なので、条件そのものを表明しておく。
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

/// b1 は物体ごとに中身が違って当然なので、束ね条件から外れていること。
/// これが効かないと «同じメッシュを並べただけ» の物体が 1 つも束ねられない。
TEST_F(InstanceBatchTest, ObjectConstantBufferIsIgnored)
{
    renderer::DrawCall other = MakeCall();
    other.constantBuffers[1].id = 999;
    EXPECT_TRUE(scene::IsSameInstanceBatch(MakeCall(), other));
}

/// 束ね側が決める枠なので、積む時点の値は条件に入らない。
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

/// 同じメッシュでも submesh の範囲が違えば別の描画。
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

/// b1 以外の定数バッファが違えば材質が違う。全枠を見ていることを端と中で確かめる。
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

/// テクスチャ 32 枠はすべて見ること。1 枠でも抜けると、その枠だけ前の物体の絵が残る。
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

/// Velocity と ObjectMask は b1 の 2 枠目を別の意味で使う (prevWorld / 未使用) が、束ね条件は
/// b1 を見ない 1 本のままであること。パスごとに «どこを見るか» が分かれると、片方だけ条件が
/// 古くなったときに気づけない。
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

} // namespace
} // namespace fbzz::tests
