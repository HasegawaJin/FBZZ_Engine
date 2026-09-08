/// @file    LayerTests.cpp
/// @brief   レイヤーマスクのビット演算と、衝突行列が対称に保たれることを検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-08
///
/// 行列が片側しか更新されないと、A→B の判定は落とすのに B→A では衝突する、という
/// «ブロードフェーズがどちらの順で組を作ったか» に依存した挙動になる。再現しない不具合の温床。
#include <TestKit/TestKit.hpp>

#include <Physics/Layer.hpp>

namespace fbzz::tests {

class LayerTest : public testkit::Fixture {};

// --- マスク -----------------------------------------------------------------

TEST_F(LayerTest, MaskSetsExactlyTheBitForTheLayer)
{
    EXPECT_EQ(Layer::Mask(0), 1u);
    EXPECT_EQ(Layer::Mask(1), 2u);
    EXPECT_EQ(Layer::Mask(31), 1u << 31);
}

TEST_F(LayerTest, MaskWrapsLayerIndicesIntoThe32BitRange)
{
    // 添字は 0-31。範囲外で未定義シフトにせず、丸めて必ずビットを 1 本返す。
    EXPECT_EQ(Layer::Mask(32), Layer::Mask(0));
    EXPECT_EQ(Layer::Mask(33), Layer::Mask(1));
}

TEST_F(LayerTest, ContainsFindsOnlyTheLayersInTheMask)
{
    const LayerMask mask = Layer::Mask(Layer::Water) | Layer::Mask(Layer::UI);

    EXPECT_TRUE(Layer::Contains(mask, Layer::Water));
    EXPECT_TRUE(Layer::Contains(mask, Layer::UI));
    EXPECT_FALSE(Layer::Contains(mask, Layer::Default));
}

TEST_F(LayerTest, EverythingContainsAllLayersAndNothingContainsNone)
{
    for (int layer = 0; layer < 32; ++layer) {
        EXPECT_TRUE(Layer::Contains(Layer::Everything, layer)) << "layer " << layer;
        EXPECT_FALSE(Layer::Contains(Layer::Nothing, layer)) << "layer " << layer;
    }
}

TEST_F(LayerTest, BuiltInLayerIndicesAreDistinct)
{
    // 番号はシーンファイルに焼かれている。重複させると別レイヤーが混ざる。
    const LayerMask combined = Layer::Mask(Layer::Default) | Layer::Mask(Layer::TransparentFX)
                             | Layer::Mask(Layer::IgnoreRaycast) | Layer::Mask(Layer::Water)
                             | Layer::Mask(Layer::UI);

    EXPECT_TRUE(Layer::Contains(combined, Layer::Default));
    EXPECT_TRUE(Layer::Contains(combined, Layer::TransparentFX));
    EXPECT_TRUE(Layer::Contains(combined, Layer::IgnoreRaycast));
    EXPECT_TRUE(Layer::Contains(combined, Layer::Water));
    EXPECT_TRUE(Layer::Contains(combined, Layer::UI));
    EXPECT_FALSE(Layer::Contains(combined, 3));   // 未使用の番号は含まれない
}

// --- 衝突行列 ---------------------------------------------------------------

TEST_F(LayerTest, EveryPairCollidesByDefault)
{
    const LayerCollisionMatrix matrix;

    for (int a = 0; a < 32; ++a)
        for (int b = 0; b < 32; ++b)
            ASSERT_TRUE(matrix.CanCollide(a, b)) << a << " vs " << b;
}

TEST_F(LayerTest, DisablingAPairUpdatesBothDirections)
{
    LayerCollisionMatrix matrix;

    matrix.Set(Layer::Water, Layer::UI, false);

    EXPECT_FALSE(matrix.CanCollide(Layer::Water, Layer::UI));
    EXPECT_FALSE(matrix.CanCollide(Layer::UI, Layer::Water));
}

TEST_F(LayerTest, DisablingAPairLeavesOtherPairsAlone)
{
    LayerCollisionMatrix matrix;

    matrix.Set(Layer::Water, Layer::UI, false);

    EXPECT_TRUE(matrix.CanCollide(Layer::Water, Layer::Default));
    EXPECT_TRUE(matrix.CanCollide(Layer::UI, Layer::Default));
    EXPECT_TRUE(matrix.CanCollide(Layer::Water, Layer::Water));
}

TEST_F(LayerTest, ReEnablingAPairRestoresBothDirections)
{
    LayerCollisionMatrix matrix;
    matrix.Set(Layer::Water, Layer::UI, false);

    matrix.Set(Layer::UI, Layer::Water, true);

    EXPECT_TRUE(matrix.CanCollide(Layer::Water, Layer::UI));
    EXPECT_TRUE(matrix.CanCollide(Layer::UI, Layer::Water));
}

TEST_F(LayerTest, ALayerCanBeMadeToIgnoreItself)
{
    LayerCollisionMatrix matrix;

    matrix.Set(Layer::Water, Layer::Water, false);

    EXPECT_FALSE(matrix.CanCollide(Layer::Water, Layer::Water));
}

} // namespace fbzz::tests
