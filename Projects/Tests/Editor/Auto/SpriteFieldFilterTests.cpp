/// @file    SpriteFieldFilterTests.cpp
/// @brief   どのアセット欄が Sprite サブアセット参照を受けるかを固定する。
/// @author  Hasegawa Jin
/// @date    2026-09-14
///
/// 受けられない欄がドロップだけ受理すると、LoadTexture が "::sprite::" を落として読むので
/// エラーにならずアトラス全面が出る。宣言はフィルター文字列の ".sprite" 1 か所きり
/// (規約は Docs/design/sprite-reference.md)。
#include <TestKit/TestKit.hpp>

#include <Editor/Util/ImGuiWidgets.hpp>
#include <Engine/Scene/Components/UIButton.hpp>

#include <string_view>

namespace fbzz::tests {
namespace {

/// ".sprite" を除いた集合が一致するか。片方だけ画像形式が増えると、欄によって候補が変わる。
bool SameImageExtensions(std::string_view withSprites, std::string_view plain)
{
    constexpr std::string_view kMarker = ".sprite,";
    if (!withSprites.starts_with(kMarker)) return false;
    return withSprites.substr(kMarker.size()) == plain;
}

} // namespace

TEST(SpriteFieldFilterTest, OnlyDeclaredFieldsAcceptSpriteReferences)
{
    // 矩形を読む経路を持つ欄。
    EXPECT_TRUE(editor::widgets::FilterAcceptsSprites(editor::widgets::kSpriteAssetFilter));
    EXPECT_TRUE(editor::widgets::FilterAcceptsSprites(scene::kSpriteFieldExtensions));
    // 流体のテクスチャ発生源 (FluidRecipe.cpp / FluidRecipeWidgets.cpp と同じ値)。
    EXPECT_TRUE(editor::widgets::FilterAcceptsSprites(".sprite,.png,.tga,.jpg,.jpeg"));

    // 画像は受けるが切り抜きは効かない欄 (Light の Cookie・Decal・歪みマップ・Flipbook の 1 枚目)。
    EXPECT_FALSE(editor::widgets::FilterAcceptsSprites(editor::widgets::kTextureAssetFilter));
    EXPECT_FALSE(editor::widgets::FilterAcceptsSprites(".png,.jpg,.dds,.tga"));
    // そもそも画像ではない欄。
    EXPECT_FALSE(editor::widgets::FilterAcceptsSprites(".mat"));

    // フィルター無し = 何でも受ける欄 (スキーマ駆動の汎用欄) はそのまま通す。
    EXPECT_TRUE(editor::widgets::FilterAcceptsSprites(""));
    EXPECT_TRUE(editor::widgets::FilterAcceptsSprites(nullptr));
}

TEST(SpriteFieldFilterTest, SpriteFilterCarriesTheSameImageExtensions)
{
    EXPECT_TRUE(SameImageExtensions(editor::widgets::kSpriteAssetFilter,
                                    editor::widgets::kTextureAssetFilter))
        << editor::widgets::kSpriteAssetFilter;
    // UI の状態別スプライト欄も «.sprite + 画像» の形で書く。
    EXPECT_TRUE(std::string_view(scene::kSpriteFieldExtensions).starts_with(".sprite,"));
}

} // namespace fbzz::tests
