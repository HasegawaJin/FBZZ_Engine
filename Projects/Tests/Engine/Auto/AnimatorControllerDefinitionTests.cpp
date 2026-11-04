/// @file    AnimatorControllerDefinitionTests.cpp
/// @brief   Controller 定義の独立性と保存・適用時のランタイム分離を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-31
#include <Engine/Asset/AnimatorControllerAsset.hpp>
namespace fbzz::tests {
template<class T> concept CompleteControllerType = requires { sizeof(T); };
template<class T> concept HasControllerRuntime = requires(T value) { value.runtime; };
template<class T> concept HasMaskCache = requires(T value) { value.loaded; };
template<class T> concept HasBlendCache = requires(T value) { value.dampedValue; };
template<class T> concept HasSlotRequest = requires(T value) { value.active; value.sourcePath; };
static_assert(!CompleteControllerType<scene::AnimatorComponent>);
static_assert(!HasControllerRuntime<asset::AnimationLayerDefinition>);
static_assert(!HasMaskCache<asset::AnimationMaskDefinition>);
static_assert(!HasBlendCache<asset::BlendTree1DDefinition>);
static_assert(!HasSlotRequest<asset::AnimationSlotDefinition>);
} /// @note namespace fbzz::tests
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <TestKit/TempDir.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>

namespace fbzz::tests {
class AnimatorControllerDefinitionTest : public testkit::EngineFixture {};
TEST_F(AnimatorControllerDefinitionTest, SavingLiveAnimatorRetainsAuthoredFieldsAndDropsTransientState)
{
    testkit::TempDir temp("controller-definition");
    ASSERT_TRUE(temp.IsValid());
    scene::AnimatorComponent live;
    live.defaultStateName = "Walk";
    scene::AnimationState state;
    state.name = "Walk";
    state.mode = scene::AnimationStateMode::BlendTree1D;
    state.sourcePath = "guid:motion";
    state.clipName = "Walk";
    state.speed = 1.5f;
    state.ikWeight = 0.4f;
    state.blendTree1D.paramName = "Speed";
    state.blendTree1D.dampTime = 0.3f;
    state.blendTree1D.syncNormalizedTime = true;
    state.blendTree1D.motions.push_back({});
    state.blendTree1D.motions.front().sourcePath = "guid:walk";
    state.blendTree1D.dampedValue = 9.0f;
    state.blendTree1D.dampedValueInitialized = true;
    state.blendTree1D.normalizedPhase = 0.8f;
    state.blendTree1D.normalizedPhaseInitialized = true;
    state.blendTree2D.paramX = "X";
    state.blendTree2D.paramY = "Y";
    state.blendTree2D.type = scene::BlendTree2DType::FreeformCartesian;
    state.blendTree2D.dampTime = 0.2f;
    live.states.push_back(state);
    scene::AnimationLayer layer;
    layer.name = "Upper";
    layer.weight = 0.6f;
    layer.mode = scene::AnimationLayerMode::Additive;
    layer.mask.path = "guid:mask";
    layer.mask.loaded = true;
    layer.mask.loadedPath = "cached.mask";
    layer.slot.sourcePath = "guid:oneshot";
    layer.slot.active = true;
    layer.slot.time = 2.0f;
    layer.slot.fadeInDuration = 0.1f;
    layer.slot.fadeOutDuration = 0.4f;
    layer.runtime.currentStateName = "Walk";
    layer.runtime.stateTime = 7.0f;
    layer.states.push_back(state);
    layer.additiveReference.sourcePath = "guid:reference";
    layer.additiveReference.time = 0.7f;
    layer.retargetMappings.push_back({"Hip", "Pelvis", math::Quaternion::Identity(), 1.2f});
    live.layers.push_back(layer);
    const auto definition = asset::MakeAnimatorControllerAsset(live);
    const auto path = temp.File("Walk.animcontroller").generic_string();
    ASSERT_TRUE(asset::SaveAnimatorControllerAsset(path, definition));
    asset::AnimatorControllerAsset loaded;
    ASSERT_TRUE(asset::LoadAnimatorControllerAsset(path, loaded));
    scene::AnimatorComponent restored;
    asset::ApplyAnimatorControllerAsset(loaded, restored);
    ASSERT_EQ(restored.states.size(), 1u);
    ASSERT_EQ(restored.layers.size(), 1u);
    const auto& restoredState = restored.states.front();
    EXPECT_EQ(restoredState.sourcePath, "guid:motion");
    EXPECT_NEAR(restoredState.speed, 1.5f, testkit::kTolerance);
    EXPECT_NEAR(restoredState.ikWeight, 0.4f, testkit::kTolerance);
    EXPECT_EQ(restoredState.blendTree1D.paramName, "Speed");
    EXPECT_NEAR(restoredState.blendTree1D.dampTime, 0.3f, testkit::kTolerance);
    EXPECT_TRUE(restoredState.blendTree1D.syncNormalizedTime);
    ASSERT_EQ(restoredState.blendTree1D.motions.size(), 1u);
    EXPECT_EQ(restoredState.blendTree1D.motions.front().sourcePath, "guid:walk");
    EXPECT_FALSE(restoredState.blendTree1D.dampedValueInitialized);
    EXPECT_FALSE(restoredState.blendTree1D.normalizedPhaseInitialized);
    EXPECT_EQ(restoredState.blendTree2D.type, scene::BlendTree2DType::FreeformCartesian);
    EXPECT_NEAR(restoredState.blendTree2D.dampTime, 0.2f, testkit::kTolerance);
    const auto& restoredLayer = restored.layers.front();
    EXPECT_EQ(restoredLayer.mask.path, "guid:mask");
    EXPECT_FALSE(restoredLayer.mask.loaded);
    EXPECT_TRUE(restoredLayer.mask.loadedPath.empty());
    EXPECT_FALSE(restoredLayer.slot.active);
    EXPECT_TRUE(restoredLayer.slot.sourcePath.empty());
    EXPECT_NEAR(restoredLayer.slot.fadeInDuration, 0.1f, testkit::kTolerance);
    EXPECT_NEAR(restoredLayer.slot.fadeOutDuration, 0.4f, testkit::kTolerance);
    EXPECT_TRUE(restoredLayer.runtime.currentStateName.empty());
    EXPECT_EQ(restoredLayer.additiveReference.sourcePath, "guid:reference");
    ASSERT_EQ(restoredLayer.retargetMappings.size(), 1u);
    EXPECT_NEAR(restoredLayer.retargetMappings.front().translationScale, 1.2f, testkit::kTolerance);
}
TEST_F(AnimatorControllerDefinitionTest, ReapplyingSameControllerKeepsPlaybackForExistingStates)
{
    asset::AnimatorControllerAsset definition;
    asset::AnimationStateDefinition state;
    state.name = "Walk";
    definition.states.push_back(state);
    asset::AnimationLayerDefinition layer;
    layer.name = "Upper";
    layer.states.push_back(state);
    definition.layers.push_back(layer);
    scene::AnimatorComponent live;
    live.controllerPath = live.loadedControllerPath = "guid:controller";
    asset::ApplyAnimatorControllerAsset(definition, live);
    live.currentStateName = "Walk";
    live.stateTime = 2.0f;
    live.layers.front().runtime.currentStateName = "Walk";
    live.layers.front().runtime.stateTime = 3.0f;
    definition.states.front().speed = 1.8f;
    asset::ApplyAnimatorControllerAsset(definition, live);
    EXPECT_EQ(live.currentStateName, "Walk");
    EXPECT_NEAR(live.stateTime, 2.0f, testkit::kTolerance);
    EXPECT_NEAR(live.layers.front().runtime.stateTime, 3.0f, testkit::kTolerance);
    EXPECT_NEAR(live.states.front().speed, 1.8f, testkit::kTolerance);
}
} /// @note namespace fbzz::tests
