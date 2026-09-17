/// @file    FzAssetFormatTests.cpp
/// @brief   焼き済みアセットのヘッダー配置と版番号を、ディスク上の実体と同じ形で固定する。
/// @author  Hasegawa Jin
/// @date    2026-09-08
///
/// Library/Baked の既存ファイルはこの構造体の並びを前提に読む。サイズは static_assert が守るが、
/// 同じサイズのまま並びを入れ替えてもコンパイルは通り、焼き直せば直るため手元では再現しない。
/// @note 構造体を書き写すようなテストに見えるが、ここでの «実装» はディスク上のバイト列そのもので、
///       構造体はその写しを留めているだけ。
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>

#include <Engine/Format/FzAssetFormat.hpp>

#include <cstddef>
#include <cstring>
#include <type_traits>

namespace fbzz::tests {

class FzAssetFormatTest : public testkit::EngineFixture {};

/// @name メッシュ

TEST_F(FzAssetFormatTest, MeshHeaderFieldsSitAtTheirOnDiskOffsets)
{
    EXPECT_EQ(offsetof(asset::FzMeshHeader, magic), 0u);
    EXPECT_EQ(offsetof(asset::FzMeshHeader, version), 4u);
    EXPECT_EQ(offsetof(asset::FzMeshHeader, flags), 8u);
    EXPECT_EQ(offsetof(asset::FzMeshHeader, vertexCount), 12u);
    EXPECT_EQ(offsetof(asset::FzMeshHeader, indexCount), 16u);
    EXPECT_EQ(offsetof(asset::FzMeshHeader, boundsCenter), 20u);
    EXPECT_EQ(offsetof(asset::FzMeshHeader, boundsRadius), 32u);
    EXPECT_EQ(sizeof(asset::FzMeshHeader), 36u);
}

TEST_F(FzAssetFormatTest, KeepsThePreColourVertexLayoutForOldFiles)
{
    /// @note v1 の頂点は «色が付く前» の renderer::Vertex と同じ 44 バイト。
    ///       ここが変わると、焼き直していない .mesh の読み込みがずれる。
    EXPECT_EQ(sizeof(asset::FzVertexV1), 44u);
    EXPECT_EQ(offsetof(asset::FzVertexV1, position), 0u);
    EXPECT_EQ(offsetof(asset::FzVertexV1, normal), 12u);
    EXPECT_EQ(offsetof(asset::FzVertexV1, tangent), 24u);
    EXPECT_EQ(offsetof(asset::FzVertexV1, uv), 36u);
}

TEST_F(FzAssetFormatTest, MeshFlagsAreDistinctBits)
{
    EXPECT_NE(asset::FZMESH_FLAG_SKINNED, 0u);
    EXPECT_EQ(asset::FZMESH_FLAG_SKINNED & (asset::FZMESH_FLAG_SKINNED - 1u), 0u);
}

/// @name アニメーション

TEST_F(FzAssetFormatTest, AnimHeaderFieldsSitAtTheirOnDiskOffsets)
{
    /// @note 名前は 128 バイト固定。ここを縮めると尺と ticksPerSecond の位置がずれ、
    ///       «再生速度だけおかしいアニメ» になる。
    EXPECT_EQ(offsetof(asset::FzAnimHeader, magic), 0u);
    EXPECT_EQ(offsetof(asset::FzAnimHeader, version), 4u);
    EXPECT_EQ(offsetof(asset::FzAnimHeader, name), 8u);
    EXPECT_EQ(offsetof(asset::FzAnimHeader, durationTicks), 136u);
    EXPECT_EQ(offsetof(asset::FzAnimHeader, ticksPerSecond), 144u);
    EXPECT_EQ(offsetof(asset::FzAnimHeader, trackCount), 152u);
    EXPECT_EQ(sizeof(asset::FzAnimHeader), 160u);
}

TEST_F(FzAssetFormatTest, AnimTrackHeaderKeepsItsCountsAfterTheName)
{
    EXPECT_EQ(offsetof(asset::FzAnimTrackHeader, nodeName), 0u);
    EXPECT_EQ(offsetof(asset::FzAnimTrackHeader, positionCount), 128u);
    EXPECT_EQ(offsetof(asset::FzAnimTrackHeader, rotationCount), 132u);
    EXPECT_EQ(offsetof(asset::FzAnimTrackHeader, scaleCount), 136u);
}

/// @name 版番号

TEST_F(FzAssetFormatTest, VersionsAreTheOnesAlreadyBakedOnDisk)
{
    /// @note 版を上げたらこのテストも一緒に直す。«直すべきものが 2 つある» ことを
    ///       気づかせるのが目的で、上げること自体を止めるものではない。
    EXPECT_EQ(asset::FZMESH_VERSION, 2u);
    EXPECT_EQ(asset::FZANIM_VERSION, 3u);
    EXPECT_EQ(asset::FZSKEL_VERSION, 1u);
}

TEST_F(FzAssetFormatTest, HeadersAreTriviallyCopyable)
{
    /// @note ヘッダーは fread / fwrite でそのまま往復する。仮想関数や
    ///       std::string を足した瞬間に、書けはするが読めないファイルが生まれる。
    EXPECT_TRUE(std::is_trivially_copyable_v<asset::FzMeshHeader>);
    EXPECT_TRUE(std::is_trivially_copyable_v<asset::FzAnimHeader>);
    EXPECT_TRUE(std::is_trivially_copyable_v<asset::FzAnimTrackHeader>);
    EXPECT_TRUE(std::is_trivially_copyable_v<asset::FzVertexV1>);
}

} // namespace fbzz::tests
