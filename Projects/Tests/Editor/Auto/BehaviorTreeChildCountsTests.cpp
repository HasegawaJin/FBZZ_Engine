/// @file    BehaviorTreeChildCountsTests.cpp
/// @brief   Behavior Tree の子数集計と構造編集後の描画スナップショットの回帰検査。
/// @author  Hasegawa Jin
/// @date    2026-10-10
#include <TestKit/TestKit.hpp>

#include <Editor/GraphEditor/BehaviorTreeOps.hpp>

namespace fbzz::tests {

class BehaviorTreeChildCountsTest : public testkit::Fixture {
protected:
    static void AddDefinition(ai::BehaviorTreeAsset& asset, int id, int parentId, int order = 0)
    {
        ai::BTNodeDef node;
        node.id = id;
        node.parentId = parentId;
        node.order = order;
        asset.nodes.push_back(std::move(node));
    }
};

TEST_F(BehaviorTreeChildCountsTest, CountsSparseAndInvalidParentsWithoutSortingOrFiltering)
{
    ai::BehaviorTreeAsset asset;
    AddDefinition(asset, 100, 0);
    AddDefinition(asset, 300, 100, 9);
    AddDefinition(asset, 200, 100, -2);
    AddDefinition(asset, 400, 999);
    AddDefinition(asset, 500, 0);

    const auto counts = editor::btops::BuildChildCounts(asset);

    EXPECT_EQ(counts.at(0), 2);
    EXPECT_EQ(counts.at(100), 2);
    EXPECT_EQ(counts.at(999), 1);
    EXPECT_FALSE(counts.contains(300));
    EXPECT_EQ(asset.nodes[1].id, 300);
    EXPECT_EQ(asset.nodes[2].id, 200);
}

TEST_F(BehaviorTreeChildCountsTest, ReparentAndSubtreeRemovalAppearInTheNextSnapshot)
{
    ai::BehaviorTreeAsset asset;
    AddDefinition(asset, 1, 0);
    AddDefinition(asset, 2, 1);
    AddDefinition(asset, 3, 1);
    AddDefinition(asset, 4, 2);
    const auto previousCounts = editor::btops::BuildChildCounts(asset);
    ASSERT_EQ(previousCounts.at(1), 2);

    ASSERT_TRUE(editor::btops::TryReparentNode(asset, 3, 2).empty());
    const auto reparentedCounts = editor::btops::BuildChildCounts(asset);

    EXPECT_EQ(previousCounts.at(1), 2);
    EXPECT_EQ(reparentedCounts.at(1), 1);
    EXPECT_EQ(reparentedCounts.at(2), 2);
    ASSERT_EQ(editor::btops::RemoveSubtree(asset, 2), 3);
    const auto removedCounts = editor::btops::BuildChildCounts(asset);
    EXPECT_EQ(removedCounts.at(0), 1);
    EXPECT_FALSE(removedCounts.contains(1));
    EXPECT_FALSE(removedCounts.contains(2));
}

TEST_F(BehaviorTreeChildCountsTest, DuplicateAndUndoReplacementCannotReuseStaleCounts)
{
    ai::BehaviorTreeAsset asset;
    AddDefinition(asset, 1, 0);
    AddDefinition(asset, 2, 1);
    AddDefinition(asset, 3, 2);
    asset.nextNodeId = 4;
    const auto before = asset;

    const auto duplicate = editor::btops::DuplicateSubtree(asset, 2, 0.0f, 0.0f);
    ASSERT_NE(duplicate.newRootId, 0);
    ASSERT_TRUE(duplicate.rejectReason.empty());
    const auto duplicatedCounts = editor::btops::BuildChildCounts(asset);

    EXPECT_EQ(duplicatedCounts.at(1), 2);
    EXPECT_EQ(duplicatedCounts.at(duplicate.newRootId), 1);
    asset = before;
    const auto undoCounts = editor::btops::BuildChildCounts(asset);
    EXPECT_EQ(undoCounts.at(1), 1);
    EXPECT_FALSE(undoCounts.contains(duplicate.newRootId));
    EXPECT_TRUE(editor::btops::BuildChildCounts(ai::BehaviorTreeAsset{}).empty());
}

}
