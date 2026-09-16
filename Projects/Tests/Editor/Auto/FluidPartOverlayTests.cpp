/// @file    FluidPartOverlayTests.cpp
/// @brief   Fluid Editor のビューポートのハンドル (集め方・つかみ方・ドラッグの書き戻し) と部品のクリック選択の契約
/// @author  Hasegawa Jin
/// @date    2026-09-12
///
/// ドラッグの書き戻しは «手を離した位置に部品が残るか» でしか確かめられず、動きのキーや warmup が絡むと
/// 目で見てもずれに気づきにくい。ImDrawList を通らない関数だけを数値で縛る。
#include <TestKit/TestKit.hpp>

#include <Editor/Util/FluidPartOverlay.hpp>
#include <Engine/Asset/FluidOperatorEval.hpp>
#include <Engine/Asset/FluidRecipe.hpp>

#include <vector>

namespace fbzz::tests {
namespace {

using editor::FluidHandleKind;
using editor::FluidPartHandle;
using editor::FluidPartVisibility;
using editor::FluidSelection;
using editor::FluidSelectionKind;
using editor::FluidViewMapping;

constexpr float kDomainEps = 1.0e-4f;
constexpr float kScreenEps = 1.0e-3f;

FluidViewMapping MakeMapping()
{
    FluidViewMapping mapping;
    mapping.origin = { 100.0f, 50.0f };
    mapping.size = 400.0f;
    return mapping;
}

asset::FluidSource MakeSource(asset::FluidSourceShape shape, const math::Vector3& center, const math::Vector3& size)
{
    asset::FluidSource source;
    source.shape = shape;
    source.center = center;
    source.size = size;
    return source;
}

FluidSelection Select(FluidSelectionKind kind, int index)
{
    FluidSelection selection;
    selection.kind = kind;
    selection.index = index;
    return selection;
}

const FluidPartHandle* FindHandle(const std::vector<FluidPartHandle>& handles, FluidSelectionKind list, int index,
                                  FluidHandleKind kind, int keyIndex = -1)
{
    for (const FluidPartHandle& handle : handles) {
        if (handle.list != list || handle.index != index || handle.kind != kind) continue;
        if (kind == FluidHandleKind::MotionKey && handle.keyIndex != keyIndex) continue;
        return &handle;
    }
    return nullptr;
}

void ExpectScreenNear(const ImVec2& actual, const ImVec2& expected)
{
    EXPECT_NEAR(actual.x, expected.x, kScreenEps);
    EXPECT_NEAR(actual.y, expected.y, kScreenEps);
}

} // namespace

TEST(FluidPartOverlay, MappingRoundTripsAndPutsTheDomainCornersOnTheSquare)
{
    const FluidViewMapping mapping = MakeMapping();
    ExpectScreenNear(mapping.ToScreen({ -1.0f, 1.0f, 0.0f }), { 100.0f, 50.0f });
    ExpectScreenNear(mapping.ToScreen({ 1.0f, -1.0f, 0.0f }), { 500.0f, 450.0f });

    const math::Vector3 p{ 0.3f, -0.45f, 0.7f };
    const math::Vector3 back = mapping.ToDomain(mapping.ToScreen(p));
    EXPECT_NEAR(back.x, p.x, kDomainEps);
    EXPECT_NEAR(back.y, p.y, kDomainEps);
    EXPECT_FLOAT_EQ(back.z, 0.0f);
    EXPECT_NEAR(mapping.ToPixels(2.0f), 400.0f, kScreenEps);
}

TEST(FluidPartOverlay, SelectedSphereSourceHasCenterAndSizeButNoDirection)
{
    const FluidViewMapping mapping = MakeMapping();
    asset::FluidRecipe recipe;
    recipe.sources.push_back(MakeSource(asset::FluidSourceShape::Sphere, { 0.2f, -0.1f, 0.0f }, { 0.25f, 0.25f, 0.25f }));
    recipe.sources.push_back(MakeSource(asset::FluidSourceShape::Sphere, { -0.5f, 0.5f, 0.0f }, { 0.1f, 0.1f, 0.1f }));

    const std::vector<FluidPartHandle> handles = editor::CollectFluidPartHandles(
        mapping, recipe, 0.0f, Select(FluidSelectionKind::Source, 0), FluidPartVisibility{});
    ASSERT_FALSE(handles.empty());
    EXPECT_EQ(handles.front().index, 0);

    const FluidPartHandle* center = FindHandle(handles, FluidSelectionKind::Source, 0, FluidHandleKind::Center);
    const FluidPartHandle* size = FindHandle(handles, FluidSelectionKind::Source, 0, FluidHandleKind::Size);
    ASSERT_NE(center, nullptr);
    ASSERT_NE(size, nullptr);
    ExpectScreenNear(center->screen, mapping.ToScreen({ 0.2f, -0.1f, 0.0f }));
    ExpectScreenNear(size->screen, mapping.ToScreen({ 0.45f, -0.1f, 0.0f }));
    EXPECT_EQ(FindHandle(handles, FluidSelectionKind::Source, 0, FluidHandleKind::Direction), nullptr);

    // 選ばれていない部品は Center だけ (つかむと選択が移る)。
    EXPECT_NE(FindHandle(handles, FluidSelectionKind::Source, 1, FluidHandleKind::Center), nullptr);
    EXPECT_EQ(FindHandle(handles, FluidSelectionKind::Source, 1, FluidHandleKind::Size), nullptr);
}

TEST(FluidPartOverlay, PicksTheNearestHandleAndPrefersTheSelectedPartOnTies)
{
    const FluidViewMapping mapping = MakeMapping();
    asset::FluidRecipe recipe;
    recipe.sources.push_back(MakeSource(asset::FluidSourceShape::Sphere, { 0.0f, 0.0f, 0.0f }, { 0.3f, 0.3f, 0.3f }));
    // 選んでいない発生源の中心を、選んだ発生源の大きさのハンドルにちょうど重ねる。
    recipe.sources.push_back(MakeSource(asset::FluidSourceShape::Sphere, { 0.3f, 0.0f, 0.0f }, { 0.1f, 0.1f, 0.1f }));
    const std::vector<FluidPartHandle> handles = editor::CollectFluidPartHandles(
        mapping, recipe, 0.0f, Select(FluidSelectionKind::Source, 0), FluidPartVisibility{});

    const FluidPartHandle* onTie = editor::PickFluidPartHandle(handles, mapping.ToScreen({ 0.3f, 0.0f, 0.0f }));
    ASSERT_NE(onTie, nullptr);
    EXPECT_EQ(onTie->index, 0);
    EXPECT_EQ(onTie->kind, FluidHandleKind::Size);

    const ImVec2 nearCenter = mapping.ToScreen({ 0.0f, 0.0f, 0.0f });
    const FluidPartHandle* center = editor::PickFluidPartHandle(handles, { nearCenter.x + 2.0f, nearCenter.y - 1.0f });
    ASSERT_NE(center, nullptr);
    EXPECT_EQ(center->kind, FluidHandleKind::Center);

    EXPECT_EQ(editor::PickFluidPartHandle(handles, mapping.ToScreen({ -0.8f, 0.8f, 0.0f })), nullptr);
}

TEST(FluidPartOverlay, DraggingTheCenterPutsThePartUnderTheCursorAndKeepsDepth)
{
    const FluidViewMapping mapping = MakeMapping();
    asset::FluidRecipe recipe;
    recipe.sources.push_back(MakeSource(asset::FluidSourceShape::Sphere, { 0.1f, 0.2f, 0.35f }, { 0.2f, 0.2f, 0.2f }));
    const std::vector<FluidPartHandle> handles = editor::CollectFluidPartHandles(
        mapping, recipe, 0.0f, Select(FluidSelectionKind::Source, 0), FluidPartVisibility{});
    const FluidPartHandle* center = FindHandle(handles, FluidSelectionKind::Source, 0, FluidHandleKind::Center);
    ASSERT_NE(center, nullptr);

    editor::ApplyFluidHandleDrag(recipe, *center, { -0.4f, 0.6f, 0.35f }, 0.0f);
    EXPECT_NEAR(recipe.sources[0].center.x, -0.4f, kDomainEps);
    EXPECT_NEAR(recipe.sources[0].center.y, 0.6f, kDomainEps);
    EXPECT_NEAR(recipe.sources[0].center.z, 0.35f, kDomainEps);
}

TEST(FluidPartOverlay, DraggingTheCenterOfAMovingPartLandsItsCurrentPoseOnTheCursor)
{
    const FluidViewMapping mapping = MakeMapping();
    asset::FluidRecipe recipe;
    // time は warmup の後を 0 とする。動きは warmup + time (= 0.5) で評価する。
    recipe.output.warmup = 0.25f;
    asset::FluidSource source =
        MakeSource(asset::FluidSourceShape::Sphere, { 0.0f, -0.6f, 0.2f }, { 0.2f, 0.2f, 0.2f });
    source.motion.keys.push_back({ 0.0f, { 0.0f, 0.0f, 0.0f } });
    source.motion.keys.push_back({ 1.0f, { 0.4f, 0.2f, 0.0f } });
    recipe.sources.push_back(source);

    const float time = 0.25f;
    const std::vector<FluidPartHandle> handles = editor::CollectFluidPartHandles(
        mapping, recipe, time, Select(FluidSelectionKind::Source, 0), FluidPartVisibility{});
    const FluidPartHandle* center = FindHandle(handles, FluidSelectionKind::Source, 0, FluidHandleKind::Center);
    ASSERT_NE(center, nullptr);
    ExpectScreenNear(center->screen, mapping.ToScreen({ 0.2f, -0.5f, 0.2f }));

    editor::ApplyFluidHandleDrag(recipe, *center, { 0.1f, 0.3f, 0.2f }, time);
    const math::Vector3 posed = asset::PoseFluidSource(recipe.sources[0], 0.5f).center;
    EXPECT_NEAR(posed.x, 0.1f, kDomainEps);
    EXPECT_NEAR(posed.y, 0.3f, kDomainEps);
    EXPECT_NEAR(recipe.sources[0].center.z, 0.2f, kDomainEps);
    // 動きそのもの (キー) は触らない。
    EXPECT_NEAR(recipe.sources[0].motion.keys[1].offset.x, 0.4f, kDomainEps);
    EXPECT_NEAR(recipe.sources[0].motion.keys[1].offset.y, 0.2f, kDomainEps);
}

TEST(FluidPartOverlay, ResizesASphereFromTheDistanceAndClampsAtTheCenter)
{
    const FluidViewMapping mapping = MakeMapping();
    asset::FluidRecipe recipe;
    recipe.sources.push_back(MakeSource(asset::FluidSourceShape::Sphere, { 0.1f, 0.1f, 0.0f }, { 0.2f, 0.2f, 0.2f }));
    const std::vector<FluidPartHandle> handles = editor::CollectFluidPartHandles(
        mapping, recipe, 0.0f, Select(FluidSelectionKind::Source, 0), FluidPartVisibility{});
    const FluidPartHandle* size = FindHandle(handles, FluidSelectionKind::Source, 0, FluidHandleKind::Size);
    ASSERT_NE(size, nullptr);

    editor::ApplyFluidHandleDrag(recipe, *size, { 0.4f, 0.5f, 0.0f }, 0.0f);
    EXPECT_NEAR(recipe.sources[0].size.x, 0.5f, kDomainEps);

    editor::ApplyFluidHandleDrag(recipe, *size, { 0.1f, 0.1f, 0.0f }, 0.0f);
    EXPECT_NEAR(recipe.sources[0].size.x, 0.005f, kDomainEps);
}

TEST(FluidPartOverlay, ResizesABoxPerAxisAndKeepsDepth)
{
    const FluidViewMapping mapping = MakeMapping();
    asset::FluidRecipe recipe;
    recipe.sources.push_back(MakeSource(asset::FluidSourceShape::Box, { 0.1f, 0.1f, 0.0f }, { 0.2f, 0.2f, 0.3f }));
    const std::vector<FluidPartHandle> handles = editor::CollectFluidPartHandles(
        mapping, recipe, 0.0f, Select(FluidSelectionKind::Source, 0), FluidPartVisibility{});
    const FluidPartHandle* size = FindHandle(handles, FluidSelectionKind::Source, 0, FluidHandleKind::Size);
    ASSERT_NE(size, nullptr);
    ExpectScreenNear(size->screen, mapping.ToScreen({ 0.3f, 0.3f, 0.0f }));

    editor::ApplyFluidHandleDrag(recipe, *size, { -0.15f, 0.15f, 0.0f }, 0.0f);
    EXPECT_NEAR(recipe.sources[0].size.x, 0.25f, kDomainEps);
    EXPECT_NEAR(recipe.sources[0].size.y, 0.05f, kDomainEps);
    EXPECT_NEAR(recipe.sources[0].size.z, 0.3f, kDomainEps);
}

TEST(FluidPartOverlay, RotatesAConeFromTheHandleAtItsBase)
{
    const FluidViewMapping mapping = MakeMapping();
    asset::FluidRecipe recipe;
    asset::FluidSource cone = MakeSource(asset::FluidSourceShape::Cone, { 0.0f, 0.0f, 0.0f }, { 0.1f, 0.5f, 0.0f });
    cone.direction = { 0.0f, 1.0f, 0.0f };
    recipe.sources.push_back(cone);
    const std::vector<FluidPartHandle> handles = editor::CollectFluidPartHandles(
        mapping, recipe, 0.0f, Select(FluidSelectionKind::Source, 0), FluidPartVisibility{});
    const FluidPartHandle* direction = FindHandle(handles, FluidSelectionKind::Source, 0, FluidHandleKind::Direction);
    ASSERT_NE(direction, nullptr);
    ExpectScreenNear(direction->screen, mapping.ToScreen({ 0.0f, 0.5f, 0.0f }));

    editor::ApplyFluidHandleDrag(recipe, *direction, { 0.3f, 0.0f, 0.0f }, 0.0f);
    const math::Vector3& d = recipe.sources[0].direction;
    EXPECT_NEAR(d.x, 1.0f, kDomainEps);
    EXPECT_NEAR(d.y, 0.0f, kDomainEps);
    EXPECT_NEAR(d.z, 0.0f, kDomainEps);
    // 底がカーソルに残るよう、長さもカーソルまでの距離になる。底の半径は変えない。
    EXPECT_NEAR(recipe.sources[0].size.y, 0.3f, kDomainEps);
    EXPECT_NEAR(recipe.sources[0].size.x, 0.1f, kDomainEps);
}

TEST(FluidPartOverlay, DirectionDragKeepsTheDepthComponentUnlessItFacesTheView)
{
    asset::FluidRecipe recipe;
    asset::FluidSource tilted = MakeSource(asset::FluidSourceShape::Cone, { 0.0f, 0.0f, 0.0f }, { 0.1f, 0.5f, 0.0f });
    tilted.direction = { 0.0f, 0.6f, 0.8f };
    recipe.sources.push_back(tilted);
    asset::FluidSource facing = tilted;
    facing.direction = { 0.0f, 0.0f, 2.0f };
    recipe.sources.push_back(facing);

    FluidPartHandle handle;
    handle.list = FluidSelectionKind::Source;
    handle.kind = FluidHandleKind::Direction;
    handle.index = 0;
    editor::ApplyFluidHandleDrag(recipe, handle, { 0.4f, 0.0f, 0.0f }, 0.0f);
    EXPECT_NEAR(recipe.sources[0].direction.x, 0.6f, kDomainEps);
    EXPECT_NEAR(recipe.sources[0].direction.y, 0.0f, kDomainEps);
    EXPECT_NEAR(recipe.sources[0].direction.z, 0.8f, kDomainEps);

    // 真正面を向いた向きは z を捨てて画面内へ倒す (長さは保つ)。
    handle.index = 1;
    editor::ApplyFluidHandleDrag(recipe, handle, { -0.4f, 0.0f, 0.0f }, 0.0f);
    EXPECT_NEAR(recipe.sources[1].direction.x, -2.0f, kDomainEps);
    EXPECT_NEAR(recipe.sources[1].direction.y, 0.0f, kDomainEps);
    EXPECT_NEAR(recipe.sources[1].direction.z, 0.0f, kDomainEps);
}

TEST(FluidPartOverlay, MovesAMotionKeyRelativeToTheBaseCenter)
{
    const FluidViewMapping mapping = MakeMapping();
    asset::FluidRecipe recipe;
    asset::FluidSource source =
        MakeSource(asset::FluidSourceShape::Sphere, { 0.0f, -0.6f, 0.2f }, { 0.2f, 0.2f, 0.2f });
    source.motion.keys.push_back({ 0.0f, { 0.0f, 0.0f, 0.1f } });
    source.motion.keys.push_back({ 1.0f, { 0.4f, 0.0f, 0.1f } });
    recipe.sources.push_back(source);

    const std::vector<FluidPartHandle> handles = editor::CollectFluidPartHandles(
        mapping, recipe, 0.0f, Select(FluidSelectionKind::Source, 0), FluidPartVisibility{});
    const FluidPartHandle* key = FindHandle(handles, FluidSelectionKind::Source, 0, FluidHandleKind::MotionKey, 1);
    ASSERT_NE(key, nullptr);
    ExpectScreenNear(key->screen, mapping.ToScreen({ 0.4f, -0.6f, 0.0f }));

    editor::ApplyFluidHandleDrag(recipe, *key, { 0.5f, 0.5f, 0.0f }, 0.0f);
    const asset::FluidMotion& motion = recipe.sources[0].motion;
    EXPECT_NEAR(motion.keys[1].offset.x, 0.5f, kDomainEps);
    EXPECT_NEAR(motion.keys[1].offset.y, 1.1f, kDomainEps);
    EXPECT_NEAR(motion.keys[1].offset.z, 0.1f, kDomainEps);
    EXPECT_NEAR(motion.keys[0].offset.x, 0.0f, kDomainEps);
    EXPECT_NEAR(recipe.sources[0].center.y, -0.6f, kDomainEps);

    const math::Vector3 posed = asset::PoseFluidSource(recipe.sources[0], 1.0f).center;
    EXPECT_NEAR(posed.x, 0.5f, kDomainEps);
    EXPECT_NEAR(posed.y, 0.5f, kDomainEps);
}

TEST(FluidPartOverlay, PicksTheSourceDrawnAboveAColliderAndNothingOutside)
{
    const FluidViewMapping mapping = MakeMapping();
    asset::FluidRecipe recipe;
    recipe.sources.push_back(MakeSource(asset::FluidSourceShape::Sphere, { 0.0f, 0.0f, 0.0f }, { 0.2f, 0.2f, 0.2f }));
    asset::FluidCollider box;
    box.shape = asset::FluidColliderShape::Box;
    box.center = { 0.0f, 0.0f, 0.0f };
    box.size = { 0.5f, 0.5f, 0.5f };
    recipe.colliders.push_back(box);

    const FluidSelection onSphere =
        editor::PickFluidPart(mapping, recipe, 0.0f, mapping.ToScreen({ 0.05f, 0.05f, 0.0f }), FluidPartVisibility{});
    EXPECT_EQ(onSphere.kind, FluidSelectionKind::Source);
    EXPECT_EQ(onSphere.index, 0);

    const FluidSelection onBox =
        editor::PickFluidPart(mapping, recipe, 0.0f, mapping.ToScreen({ 0.4f, 0.4f, 0.0f }), FluidPartVisibility{});
    EXPECT_EQ(onBox.kind, FluidSelectionKind::Collider);
    EXPECT_EQ(onBox.index, 0);

    const FluidSelection outside =
        editor::PickFluidPart(mapping, recipe, 0.0f, mapping.ToScreen({ 0.8f, 0.8f, 0.0f }), FluidPartVisibility{});
    EXPECT_EQ(outside.kind, FluidSelectionKind::None);
}

TEST(FluidPartOverlay, HiddenPartsCannotBePickedOrGrabbed)
{
    const FluidViewMapping mapping = MakeMapping();
    asset::FluidRecipe recipe;
    recipe.sources.push_back(MakeSource(asset::FluidSourceShape::Sphere, { 0.0f, 0.0f, 0.0f }, { 0.2f, 0.2f, 0.2f }));
    const FluidPartVisibility visible = [](FluidSelectionKind list, int index) {
        return !(list == FluidSelectionKind::Source && index == 0);
    };

    const FluidSelection picked =
        editor::PickFluidPart(mapping, recipe, 0.0f, mapping.ToScreen({ 0.05f, 0.05f, 0.0f }), visible);
    EXPECT_EQ(picked.kind, FluidSelectionKind::None);

    const std::vector<FluidPartHandle> handles =
        editor::CollectFluidPartHandles(mapping, recipe, 0.0f, Select(FluidSelectionKind::Source, 0), visible);
    EXPECT_TRUE(handles.empty());
}

} // namespace fbzz::tests
