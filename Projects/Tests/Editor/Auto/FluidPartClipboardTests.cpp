/// @file    FluidPartClipboardTests.cpp
/// @brief   Fluid Editor の部品の控え (コピー & ペースト) と、添字で覚えている hide / solo の付いて回り方。
/// @author  Hasegawa Jin
/// @date    2026-09-13
///
/// 控えは «別の .fluid を開いた後でも貼れる» ことが値打ちなので、置き場は文書の外に要る。
/// ここで縛るのは «貼った先で名前が衝突しない»・«種別違いと上限を黙って通さない»・
/// «並べ替えや複製でプレビューの hide / solo が別の部品へ移らない» の 3 つ。
/// どれも画面では «なぜかこの渦だけ出ない» という形でしか現れず、目で追えない。
#include <TestKit/TestKit.hpp>

#include <Panels/FluidEditor/FluidEditorExtras.hpp>

#include <Editor/Util/FluidDocument.hpp>
#include <Engine/Asset/FluidRecipe.hpp>

#include <cstddef>
#include <string>
#include <vector>

namespace fbzz::tests {

// fbzz::editor::fluidui (レシピの編集欄) と紛れないよう、パネル側は fluidpanel と呼ぶ。
namespace fluidpanel = editor::fluideditor;

namespace {

using editor::FluidDocument;
using editor::FluidSelectionKind;

asset::FluidRecipe RecipeWithNamedSources(const std::vector<std::string>& names)
{
    asset::FluidRecipe recipe = asset::MakeFluidPreset(asset::FluidPreset::Smoke);
    recipe.sources.clear();
    recipe.forces.clear();
    recipe.colliders.clear();
    recipe.sources.resize(names.size());
    for (std::size_t i = 0; i < names.size(); ++i) {
        recipe.sources[i].name = names[i];
        recipe.sources[i].startTime = static_cast<float>(i);
    }
    return recipe;
}

} // namespace

TEST(FluidPartClipboardTest, UniqueNameAddsSerialWithinTheSameList)
{
    const asset::FluidRecipe recipe = RecipeWithNamedSources({ "Vortex", "Vortex (1)", "Jet" });

    // 衝突しなければそのまま。
    EXPECT_EQ(fluidpanel::MakeUniqueFluidPartName(recipe, FluidSelectionKind::Source, "Puff", -1), "Puff");
    // 連番は «埋まっていない最初の番号»。"Vortex (1) (1)" のようには伸ばさない。
    EXPECT_EQ(fluidpanel::MakeUniqueFluidPartName(recipe, FluidSelectionKind::Source, "Vortex", -1),
              "Vortex (2)");
    EXPECT_EQ(fluidpanel::MakeUniqueFluidPartName(recipe, FluidSelectionKind::Source, "Vortex (1)", -1),
              "Vortex (2)");
    // 自分自身の名前は衝突と数えない (リネームで «変えていないのに連番が付く» を防ぐ)。
    EXPECT_EQ(fluidpanel::MakeUniqueFluidPartName(recipe, FluidSelectionKind::Source, "Vortex", 0), "Vortex");
    // 名前が空の部品は添字から表示名を作るので、空のままでも衝突しない。
    EXPECT_EQ(fluidpanel::MakeUniqueFluidPartName(recipe, FluidSelectionKind::Source, "", -1), "");
    // 別のリストの名前は見ない。
    EXPECT_EQ(fluidpanel::MakeUniqueFluidPartName(recipe, FluidSelectionKind::Force, "Vortex", -1), "Vortex");
}

TEST(FluidPartClipboardTest, PasteCarriesThePartIntoAnotherRecipe)
{
    const asset::FluidRecipe from = RecipeWithNamedSources({ "Bystander", "Vortex" });
    fluidpanel::SetFluidPartClipboard(from, FluidSelectionKind::Source, 1);

    EXPECT_TRUE(fluidpanel::HasFluidPartClipboard());
    EXPECT_TRUE(fluidpanel::FluidPartClipboardMatches(FluidSelectionKind::Source));
    EXPECT_EQ(fluidpanel::FluidPartClipboardKind(), FluidSelectionKind::Source);
    EXPECT_FALSE(fluidpanel::FluidPartClipboardLabel().empty());

    // 別の .fluid を開いたのと同じこと — 控えは元のレシピを参照しない。
    asset::FluidRecipe into = RecipeWithNamedSources({ "Vortex" });
    ASSERT_TRUE(fluidpanel::PasteFluidPartClipboard(into, FluidSelectionKind::Source, 1));

    ASSERT_EQ(into.sources.size(), 2u);
    EXPECT_FLOAT_EQ(into.sources[1].startTime, 1.0f);
    // 貼る先で名前が衝突したら連番を振り直す。
    EXPECT_EQ(into.sources[1].name, "Vortex (1)");
    EXPECT_EQ(into.sources[0].name, "Vortex");

    // 範囲外の位置は末尾へ寄せる。
    ASSERT_TRUE(fluidpanel::PasteFluidPartClipboard(into, FluidSelectionKind::Source, 99));
    ASSERT_EQ(into.sources.size(), 3u);
    EXPECT_EQ(into.sources[2].name, "Vortex (2)");
}

TEST(FluidPartClipboardTest, PasteRejectsOtherKindsAndFullLists)
{
    const asset::FluidRecipe from = RecipeWithNamedSources({ "Vortex" });
    fluidpanel::SetFluidPartClipboard(from, FluidSelectionKind::Source, 0);

    asset::FluidRecipe into = RecipeWithNamedSources({});
    EXPECT_FALSE(fluidpanel::FluidPartClipboardMatches(FluidSelectionKind::Force));
    EXPECT_FALSE(fluidpanel::PasteFluidPartClipboard(into, FluidSelectionKind::Force, 0));
    EXPECT_FALSE(fluidpanel::PasteFluidPartClipboard(into, FluidSelectionKind::Collider, 0));
    EXPECT_TRUE(into.forces.empty());
    EXPECT_TRUE(into.colliders.empty());

    into.sources.resize(static_cast<std::size_t>(asset::kMaxFluidSources));
    EXPECT_FALSE(fluidpanel::PasteFluidPartClipboard(into, FluidSelectionKind::Source, 0));
    EXPECT_EQ(into.sources.size(), static_cast<std::size_t>(asset::kMaxFluidSources));
}

TEST(FluidVisibilityRemapTest, HiddenAndSoloFollowTheirPart)
{
    FluidDocument document;
    document.SetHidden(FluidSelectionKind::Source, 1, true);
    document.SetHidden(FluidSelectionKind::Force, 1, true);
    document.ToggleSolo(FluidSelectionKind::Source, 1);

    // 前へ 1 つ挿し込んだら、その後ろの印は 1 つ後ろへ。挿し込んだ部品自身は印なしで始まる。
    document.RemapVisibilityAfterInsert(FluidSelectionKind::Source, 0);
    EXPECT_FALSE(document.IsHidden(FluidSelectionKind::Source, 0));
    EXPECT_FALSE(document.IsHidden(FluidSelectionKind::Source, 1));
    EXPECT_TRUE(document.IsHidden(FluidSelectionKind::Source, 2));
    EXPECT_TRUE(document.IsSolo(FluidSelectionKind::Source, 2));
    // 別のリストの印は動かさない。
    EXPECT_TRUE(document.IsHidden(FluidSelectionKind::Force, 1));

    // 並べ替え (2 → 0)。間に挟まれた部品は 1 つずつずれる。
    document.RemapVisibilityAfterMove(FluidSelectionKind::Source, 2, 0);
    EXPECT_TRUE(document.IsHidden(FluidSelectionKind::Source, 0));
    EXPECT_TRUE(document.IsSolo(FluidSelectionKind::Source, 0));
    EXPECT_FALSE(document.IsHidden(FluidSelectionKind::Source, 2));

    // 消えた部品の印は落とす (残すと «隣の部品が勝手に隠れる»)。
    document.RemapVisibilityAfterRemove(FluidSelectionKind::Source, 0);
    EXPECT_FALSE(document.IsHidden(FluidSelectionKind::Source, 0));
    EXPECT_FALSE(document.IsSolo(FluidSelectionKind::Source, 0));
    EXPECT_TRUE(document.IsHidden(FluidSelectionKind::Force, 1));
}

} // namespace fbzz::tests
