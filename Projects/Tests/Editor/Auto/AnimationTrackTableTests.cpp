/// @file    AnimationTrackTableTests.cpp
/// @brief   プレビューのトラック照合とアセット切替・再読み込みの回帰検査。
/// @author  Hasegawa Jin
/// @date    2026-10-10
#include <TestKit/TestKit.hpp>

#include <Editor/Util/AnimationTrackTable.hpp>
#include <Engine/Asset/AnimationClip.hpp>
#include <Engine/Asset/Skeleton.hpp>

#include <initializer_list>
#include <string>

namespace fbzz::tests {
namespace {

asset::Skeleton MakeSkeleton(std::initializer_list<const char*> names)
{
    asset::Skeleton skeleton;
    for (const char* name : names) {
        asset::SkeletonNode node;
        node.name = name;
        skeleton.nodes.push_back(std::move(node));
    }
    return skeleton;
}

asset::AnimationClip MakeClip(std::initializer_list<const char*> names)
{
    asset::AnimationClip clip;
    for (const char* name : names) {
        asset::NodeAnimationTrack track;
        track.nodeName = name;
        clip.tracks.push_back(std::move(track));
    }
    return clip;
}

}

class AnimationTrackTableTest : public testkit::Fixture {};

TEST_F(AnimationTrackTableTest, ExactMatchPrecedesCanonicalMatchAndKeepsFirstDuplicate)
{
    const auto skeleton = MakeSkeleton({ "Hips", "rig:Hips" });
    const auto clip = MakeClip({ "other:Hips", "Hips", "Hips", "rig:Hips" });
    editor::AnimationTrackTable tracks;

    ASSERT_TRUE(tracks.Update(skeleton, &clip, 0));

    EXPECT_EQ(tracks[0], &clip.tracks[1]);
    EXPECT_EQ(tracks[1], &clip.tracks[3]);
    EXPECT_FALSE(tracks.Update(skeleton, &clip, 0));
}

TEST_F(AnimationTrackTableTest, CanonicalMatchingPreservesFbxPathNamespaceAndHelperRules)
{
    const auto skeleton = MakeSkeleton({ "Hips", "model|rig:Arm_$AssimpFbx$_Rotation", "Missing" });
    const auto clip = MakeClip({ "model\\rig:Hips_$AssimpFbx$_Translation", "other:Hips", "Arm" });
    editor::AnimationTrackTable tracks;

    ASSERT_TRUE(tracks.Update(skeleton, &clip, 0));

    EXPECT_EQ(tracks[0], &clip.tracks[0]);
    EXPECT_EQ(tracks[1], &clip.tracks[2]);
    EXPECT_EQ(tracks[2], nullptr);
}

TEST_F(AnimationTrackTableTest, AssetGenerationRebindsNamesEditedWithoutMovingStorage)
{
    auto skeleton = MakeSkeleton({ "Hips" });
    auto clip = MakeClip({ "Hips", "Arm" });
    editor::AnimationTrackTable tracks;
    ASSERT_TRUE(tracks.Update(skeleton, &clip, 0));
    ASSERT_EQ(tracks[0], &clip.tracks[0]);

    clip.tracks[0].nodeName = "Arm";
    clip.tracks[1].nodeName = "Hips";
    ASSERT_TRUE(tracks.Update(skeleton, &clip, 1));

    EXPECT_EQ(tracks[0], &clip.tracks[1]);
    skeleton.nodes[0].name = "Arm";
    ASSERT_TRUE(tracks.Update(skeleton, &clip, 2));
    EXPECT_EQ(tracks[0], &clip.tracks[0]);
}

TEST_F(AnimationTrackTableTest, TransitionClipSwitchAndRemovalLeavePrimaryBindingIntact)
{
    const auto skeleton = MakeSkeleton({ "Hips", "Arm" });
    const auto clipA = MakeClip({ "Hips", "Arm" });
    const auto clipB = MakeClip({ "Arm", "Hips" });
    const auto clipC = MakeClip({ "Hips" });
    editor::AnimationTrackTable tracksA;
    editor::AnimationTrackTable tracksB;
    ASSERT_TRUE(tracksA.Update(skeleton, &clipA, 0));
    ASSERT_TRUE(tracksB.Update(skeleton, &clipB, 0));
    EXPECT_EQ(tracksB[0], &clipB.tracks[1]);

    ASSERT_TRUE(tracksB.Update(skeleton, &clipC, 0));

    EXPECT_EQ(tracksB[0], &clipC.tracks[0]);
    EXPECT_EQ(tracksB[1], nullptr);
    EXPECT_FALSE(tracksA.Update(skeleton, &clipA, 0));
    EXPECT_EQ(tracksA[0], &clipA.tracks[0]);
    ASSERT_TRUE(tracksB.Update(skeleton, nullptr, 0));
    EXPECT_EQ(tracksB[0], nullptr);
    EXPECT_EQ(tracksB[1], nullptr);
}

TEST_F(AnimationTrackTableTest, StorageReallocationAndSkeletonReplacementRebuildBindings)
{
    auto skeleton = MakeSkeleton({ "Hips" });
    auto clip = MakeClip({ "Hips" });
    editor::AnimationTrackTable tracks;
    ASSERT_TRUE(tracks.Update(skeleton, &clip, 0));

    clip.tracks.reserve(clip.tracks.capacity() + 1);
    ASSERT_TRUE(tracks.Update(skeleton, &clip, 0));
    EXPECT_EQ(tracks[0], &clip.tracks[0]);
    skeleton.nodes.reserve(skeleton.nodes.capacity() + 1);
    ASSERT_TRUE(tracks.Update(skeleton, &clip, 0));
    EXPECT_EQ(tracks[0], &clip.tracks[0]);

    const auto replacement = MakeSkeleton({ "Missing", "Hips" });
    ASSERT_TRUE(tracks.Update(replacement, &clip, 0));
    EXPECT_EQ(tracks[0], nullptr);
    EXPECT_EQ(tracks[1], &clip.tracks[0]);
}

TEST_F(AnimationTrackTableTest, EmptyAndMissingClipsClearEveryNodeBinding)
{
    const auto skeleton = MakeSkeleton({ "Hips", "Arm" });
    const auto populated = MakeClip({ "Hips", "Arm" });
    const asset::AnimationClip empty;
    editor::AnimationTrackTable tracks;
    ASSERT_TRUE(tracks.Update(skeleton, &populated, 0));

    ASSERT_TRUE(tracks.Update(skeleton, &empty, 0));

    EXPECT_EQ(tracks[0], nullptr);
    EXPECT_EQ(tracks[1], nullptr);
    ASSERT_TRUE(tracks.Update(skeleton, nullptr, 0));
    EXPECT_EQ(tracks[0], nullptr);
    EXPECT_EQ(tracks[1], nullptr);
    const asset::Skeleton noNodes;
    EXPECT_TRUE(tracks.Update(noNodes, nullptr, 0));
    EXPECT_FALSE(tracks.Update(noNodes, nullptr, 0));
}

}
