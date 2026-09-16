/// @file    SpriteSlicerTests.cpp
/// @brief   スプライトのグリッド分割と、既存スライスへのマージ規則。
/// @author  Hasegawa Jin
/// @date    2026-09-10
///
/// 切り直しは «位置がずれた» を直す操作で、人が詰めた pivot / Border / ID を
/// 巻き添えにしてはいけない。ここが壊れると «直したはずが足元の当たりだけずれた»
/// という形で出て、原因がスライスだと気づけない。
#include <TestKit/TestKit.hpp>

#include <Editor/Util/SpriteSlicer.hpp>

#include <string>
#include <vector>

namespace fbzz::tests {
namespace {

namespace slicer = editor::spriteslice;

// 画素を読むのは keepEmptyRects=false のときだけ。既定は true にして、
// 画像ファイル無しで «並べ方» そのものを検証する。
slicer::GridParams ByCellSize(int cellW, int cellH)
{
    slicer::GridParams params;
    params.byCellCount = false;
    params.cellWidth   = cellW;
    params.cellHeight  = cellH;
    params.keepEmptyRects = true;
    params.baseName    = "sprite_";
    return params;
}

slicer::GridParams ByCellCount(int columns, int rows)
{
    slicer::GridParams params;
    params.byCellCount = true;
    params.columns     = columns;
    params.rows        = rows;
    params.keepEmptyRects = true;
    params.baseName    = "sprite_";
    return params;
}

asset::SpriteRect MakeRect(const char* name, uint32_t x, uint32_t y,
                           uint32_t w, uint32_t h)
{
    asset::SpriteRect rect;
    rect.id     = name;         // テスト内で追跡できる印として使う
    rect.name   = name;
    rect.x      = x;
    rect.y      = y;
    rect.width  = w;
    rect.height = h;
    return rect;
}

const asset::SpriteRect* FindByName(const std::vector<asset::SpriteRect>& list,
                                    const std::string& name)
{
    for (const auto& rect : list)
        if (rect.name == name) return &rect;
    return nullptr;
}

} // namespace

// --- グリッド生成 -----------------------------------------------------------

TEST(SpriteSlicerGrid, SplitsEvenlyByCellSize)
{
    const auto result = slicer::GenerateGrid("", 128, 64, ByCellSize(32, 32));

    ASSERT_TRUE(result.error.empty()) << result.error;
    EXPECT_EQ(result.sprites.size(), 8u);   // 4 列 x 2 行
    EXPECT_EQ(result.sprites[0].x, 0u);
    EXPECT_EQ(result.sprites[0].y, 0u);
    EXPECT_EQ(result.sprites[0].width, 32u);
    EXPECT_EQ(result.sprites[0].height, 32u);
}

TEST(SpriteSlicerGrid, SplitsEvenlyByCellCount)
{
    const auto result = slicer::GenerateGrid("", 100, 50, ByCellCount(4, 2));

    ASSERT_TRUE(result.error.empty()) << result.error;
    EXPECT_EQ(result.sprites.size(), 8u);
}

TEST(SpriteSlicerGrid, CellCountCoversTheWholeTextureWithoutGaps)
{
    // 割り切れない幅。切り捨てで «右端が欠ける» と、最後のコマだけ絵が切れる。
    const auto result = slicer::GenerateGrid("", 100, 30, ByCellCount(3, 1));

    ASSERT_EQ(result.sprites.size(), 3u);
    const auto& last = result.sprites.back();
    EXPECT_EQ(last.x + last.width, 100u);
}

TEST(SpriteSlicerGrid, NamesAreNumberedFromTheBaseName)
{
    const auto result = slicer::GenerateGrid("", 64, 32, ByCellSize(32, 32));

    ASSERT_EQ(result.sprites.size(), 2u);
    EXPECT_EQ(result.sprites[0].name, "sprite_0");
    EXPECT_EQ(result.sprites[1].name, "sprite_1");
}

TEST(SpriteSlicerGrid, EveryGeneratedSpriteGetsItsOwnId)
{
    const auto result = slicer::GenerateGrid("", 64, 64, ByCellSize(32, 32));

    ASSERT_EQ(result.sprites.size(), 4u);
    for (const auto& sprite : result.sprites) {
        EXPECT_FALSE(sprite.id.empty());
        for (const auto& other : result.sprites) {
            if (&sprite != &other) EXPECT_NE(sprite.id, other.id);
        }
    }
}

TEST(SpriteSlicerGrid, LaysCellsLeftToRightThenTopToBottom)
{
    const auto result = slicer::GenerateGrid("", 64, 64, ByCellSize(32, 32));

    ASSERT_EQ(result.sprites.size(), 4u);
    EXPECT_EQ(result.sprites[0].y, result.sprites[1].y);   // 同じ行
    EXPECT_LT(result.sprites[0].x, result.sprites[1].x);
    EXPECT_LT(result.sprites[1].y, result.sprites[2].y);   // 次の行へ
}

TEST(SpriteSlicerGrid, AppliesTheOffset)
{
    auto params = ByCellSize(32, 32);
    params.offsetX = 10;
    params.offsetY = 4;

    const auto result = slicer::GenerateGrid("", 128, 64, params);

    ASSERT_FALSE(result.sprites.empty());
    EXPECT_EQ(result.sprites[0].x, 10u);
    EXPECT_EQ(result.sprites[0].y, 4u);
}

TEST(SpriteSlicerGrid, PutsPaddingBetweenCellsNotInsideThem)
{
    auto params = ByCellSize(32, 32);
    params.paddingX = 8;

    const auto result = slicer::GenerateGrid("", 128, 32, params);

    ASSERT_GE(result.sprites.size(), 2u);
    EXPECT_EQ(result.sprites[0].width, 32u);              // セル自体は縮まない
    EXPECT_EQ(result.sprites[1].x, 32u + 8u);             // 間に隙間が入る
}

TEST(SpriteSlicerGrid, ClampsCellsThatWouldRunPastTheEdge)
{
    // 端数が出る割り方。テクスチャの外へはみ出す矩形を作ってはいけない。
    const auto result = slicer::GenerateGrid("", 100, 100, ByCellSize(30, 30));

    ASSERT_FALSE(result.sprites.empty());
    for (const auto& sprite : result.sprites) {
        EXPECT_LE(sprite.x + sprite.width, 100u);
        EXPECT_LE(sprite.y + sprite.height, 100u);
    }
}

TEST(SpriteSlicerGrid, CarriesThePivotOntoEverySprite)
{
    auto params = ByCellSize(32, 32);
    params.pivotX = 0.5f;
    params.pivotY = 0.0f;

    const auto result = slicer::GenerateGrid("", 64, 32, params);

    ASSERT_FALSE(result.sprites.empty());
    EXPECT_FLOAT_EQ(result.sprites[0].pivotX, 0.5f);
    EXPECT_FLOAT_EQ(result.sprites[0].pivotY, 0.0f);
}

TEST(SpriteSlicerGrid, ReportsInvalidTextureDimensions)
{
    EXPECT_FALSE(slicer::GenerateGrid("", 0, 64, ByCellSize(32, 32)).error.empty());
    EXPECT_FALSE(slicer::GenerateGrid("", 64, 0, ByCellSize(32, 32)).error.empty());
}

TEST(SpriteSlicerGrid, ReportsWhenOffsetAndPaddingLeaveNoRoom)
{
    auto params = ByCellCount(8, 8);
    params.offsetX  = 60;
    params.paddingX = 20;

    const auto result = slicer::GenerateGrid("", 64, 64, params);

    EXPECT_FALSE(result.error.empty());
    EXPECT_TRUE(result.sprites.empty());
}

TEST(SpriteSlicerGrid, ClampsDegenerateCellSizes)
{
    // 0 や負の指定で «幅 0 のスプライトが大量に出る» ことがあってはいけない。
    auto params = ByCellSize(0, -5);
    const auto result = slicer::GenerateGrid("", 8, 8, params);

    ASSERT_TRUE(result.error.empty()) << result.error;
    for (const auto& sprite : result.sprites) {
        EXPECT_GT(sprite.width, 0u);
        EXPECT_GT(sprite.height, 0u);
    }
}

// --- 既存スライスへのマージ -------------------------------------------------

TEST(SpriteSlicerMerge, DeleteExistingReplacesEverything)
{
    const std::vector<asset::SpriteRect> existing{ MakeRect("old", 0, 0, 10, 10) };
    std::vector<asset::SpriteRect> generated{ MakeRect("new", 0, 0, 32, 32) };

    const auto merged = slicer::MergeIntoExisting(existing, generated,
                                                  slicer::ExistingMode::DeleteExisting);

    ASSERT_EQ(merged.size(), 1u);
    EXPECT_EQ(merged[0].name, "new");
}

TEST(SpriteSlicerMerge, SmartUpdatesTheRectButKeepsTheIdentity)
{
    // 人が詰めた ID・名前・pivot は «位置がずれた» の修正で消してはいけない。
    asset::SpriteRect old = MakeRect("hand", 0, 0, 10, 10);
    old.pivotX = 0.25f;
    old.pivotY = 0.75f;

    std::vector<asset::SpriteRect> generated{ MakeRect("sprite_0", 2, 2, 32, 32) };

    int reused = 0;
    const auto merged = slicer::MergeIntoExisting({ old }, generated,
                                                  slicer::ExistingMode::Smart, &reused);

    ASSERT_EQ(merged.size(), 1u);
    EXPECT_EQ(reused, 1);
    EXPECT_EQ(merged[0].name, "hand");        // 名前は残る
    EXPECT_EQ(merged[0].id, "hand");          // ID も残る
    EXPECT_FLOAT_EQ(merged[0].pivotX, 0.25f); // pivot も残る
    EXPECT_EQ(merged[0].x, 2u);               // 矩形だけ合わせ直す
    EXPECT_EQ(merged[0].width, 32u);
}

TEST(SpriteSlicerMerge, SafeKeepsTheOverlappedSpriteUntouched)
{
    const std::vector<asset::SpriteRect> existing{ MakeRect("hand", 0, 0, 10, 10) };
    std::vector<asset::SpriteRect> generated{ MakeRect("sprite_0", 2, 2, 32, 32) };

    int reused = 0;
    const auto merged = slicer::MergeIntoExisting(existing, generated,
                                                  slicer::ExistingMode::Safe, &reused);

    ASSERT_EQ(merged.size(), 1u);
    EXPECT_EQ(reused, 1);
    EXPECT_EQ(merged[0].x, 0u);        // 矩形すら変えない
    EXPECT_EQ(merged[0].width, 10u);
}

TEST(SpriteSlicerMerge, AddsSpritesThatOverlapNothing)
{
    const std::vector<asset::SpriteRect> existing{ MakeRect("hand", 0, 0, 10, 10) };
    std::vector<asset::SpriteRect> generated{ MakeRect("sprite_0", 100, 100, 32, 32) };

    int reused = 0;
    const auto merged = slicer::MergeIntoExisting(existing, generated,
                                                  slicer::ExistingMode::Smart, &reused);

    EXPECT_EQ(merged.size(), 2u);
    EXPECT_EQ(reused, 0);
    EXPECT_NE(FindByName(merged, "sprite_0"), nullptr);
}

TEST(SpriteSlicerMerge, RenamesAnAddedSpriteThatWouldCollide)
{
    // 重ならないので «追加» されるが、名前は既存と衝突している。
    const std::vector<asset::SpriteRect> existing{ MakeRect("sprite_0", 0, 0, 10, 10) };
    std::vector<asset::SpriteRect> generated{ MakeRect("sprite_0", 100, 100, 32, 32) };

    const auto merged = slicer::MergeIntoExisting(existing, generated,
                                                  slicer::ExistingMode::Smart);

    ASSERT_EQ(merged.size(), 2u);
    EXPECT_NE(merged[0].name, merged[1].name);
}

TEST(SpriteSlicerMerge, PicksTheLargestOverlapWhenSeveralMatch)
{
    // かすっただけの相手に吸われると、別のコマの設定が書き換わる。
    const std::vector<asset::SpriteRect> existing{
        MakeRect("touching", 30, 0, 4, 4),    // わずかに重なる
        MakeRect("covering",  0, 0, 32, 32),  // ほぼ一致
    };
    std::vector<asset::SpriteRect> generated{ MakeRect("sprite_0", 0, 0, 32, 32) };

    const auto merged = slicer::MergeIntoExisting(existing, generated,
                                                  slicer::ExistingMode::Smart);

    ASSERT_EQ(merged.size(), 2u);
    const auto* covering = FindByName(merged, "covering");
    const auto* touching = FindByName(merged, "touching");
    ASSERT_NE(covering, nullptr);
    ASSERT_NE(touching, nullptr);
    EXPECT_EQ(covering->width, 32u);
    EXPECT_EQ(touching->width, 4u);   // かすった側は触られない
}

TEST(SpriteSlicerMerge, ReportsZeroReuseWhenThereIsNothingToMatch)
{
    std::vector<asset::SpriteRect> generated{ MakeRect("sprite_0", 0, 0, 32, 32) };

    int reused = -1;
    const auto merged = slicer::MergeIntoExisting({}, generated,
                                                  slicer::ExistingMode::Smart, &reused);

    EXPECT_EQ(reused, 0);
    EXPECT_EQ(merged.size(), 1u);
}

} // namespace fbzz::tests
