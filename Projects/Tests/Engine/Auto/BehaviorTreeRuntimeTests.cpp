/// @file    BehaviorTreeRuntimeTests.cpp
/// @brief   BT のネスト、個体状態、優先度中断と Action 終了通知を固定する。
/// @author  Hasegawa Jin
/// @date    2026-10-12
#include <TestKit/TestKit.hpp>
#include <TestKit/Fixture.hpp>
#include <Engine/AI/BehaviorTreeEvaluator.hpp>

#include <array>
#include <initializer_list>

namespace fbzz::tests {
namespace {

using namespace ai;

BTNodeDef Node(int id, int parent, BTNodeType type, int order = 0)
{
    BTNodeDef node;
    node.id = id;
    node.parentId = parent;
    node.type = type;
    node.order = order;
    return node;
}

BTNodeDef Guard(int id, int parent, AbortMode abort = AbortMode::None,
                BTNodeType type = BTNodeType::BlackboardCondition, int order = 0)
{
    auto node = Node(id, parent, type, order);
    node.keyName = "HasTarget";
    node.valueBool = true;
    node.abortMode = abort;
    return node;
}

class RecordingActions : public IBTActionHandler {
public:
    std::array<BTStatus, 32> results;
    std::array<int, 32> executed{};
    std::array<int, 32> aborted{};

    RecordingActions() { results.fill(BTStatus::Running); }
    BTStatus Execute(const BehaviorTreeRuntime& tree, std::uint16_t node,
                     BTInstanceState&, Blackboard&, BTTickContext&) override
    {
        const int id = tree.params[tree.nodes[node].paramIndex].authoringId;
        ++executed[id];
        return results[id];
    }
    void Abort(const BehaviorTreeRuntime& tree, std::uint16_t node,
               BTInstanceState&, BTTickContext&) override
    {
        ++aborted[tree.params[tree.nodes[node].paramIndex].authoringId];
    }
};

} /// @note namespace

class BehaviorTreeRuntimeTest : public testkit::Fixture {
protected:
    ai::BehaviorTreeRuntime tree;
    ai::BTInstanceState state;
    ai::Blackboard blackboard;
    RecordingActions actions;
    ai::BTTickContext ctx;

    bool Prepare(std::initializer_list<ai::BTNodeDef> nodes)
    {
        ai::BehaviorTreeAsset asset;
        asset.nodes = nodes;
        asset.blackboard.push_back({ .name = "Counter", .type = ai::BlackboardType::Int });
        if (!ai::CompileBehaviorTree(asset, tree)) return false;
        blackboard.Reset(tree.blackboard);
        ctx.actions = &actions;
        return true;
    }
    ai::BTStatus Tick(float dt = 0.1f)
    {
        ctx.dt = dt;
        return ai::TickBehaviorTree(tree, state, blackboard, ctx);
    }
};

using namespace ai;

TEST_F(BehaviorTreeRuntimeTest, TraversesDirectChildrenAfterNestedSubtrees)
{
    ASSERT_TRUE(Prepare({ Node(1, 0, BTNodeType::Sequence), Node(2, 1, BTNodeType::Sequence),
        Node(3, 2, BTNodeType::RunScript), Node(4, 1, BTNodeType::RunScript, 1) }));
    actions.results[3] = BTStatus::Success;

    EXPECT_EQ(Tick(), BTStatus::Running);
    EXPECT_EQ(Tick(), BTStatus::Running);

    EXPECT_EQ(actions.executed[3], 1);
    EXPECT_EQ(actions.executed[4], 2);
}

TEST_F(BehaviorTreeRuntimeTest, SelectsFallbackAfterNestedFailure)
{
    ASSERT_TRUE(Prepare({ Node(1, 0, BTNodeType::Selector), Node(2, 1, BTNodeType::Sequence),
        Node(3, 2, BTNodeType::AlwaysFail), Node(4, 1, BTNodeType::RunScript, 1) }));

    EXPECT_EQ(Tick(), BTStatus::Running);

    EXPECT_EQ(actions.executed[4], 1);
}

TEST_F(BehaviorTreeRuntimeTest, ChoosesWeightedDirectChildAfterNestedSubtree)
{
    auto root = Node(1, 0, BTNodeType::RandomSelector);
    root.childWeights = { 0.0f, 1.0f };
    ASSERT_TRUE(Prepare({ root, Node(2, 1, BTNodeType::Sequence),
        Node(3, 2, BTNodeType::RunScript), Node(4, 1, BTNodeType::RunScript, 1) }));

    EXPECT_EQ(Tick(), BTStatus::Running);
    EXPECT_EQ(Tick(), BTStatus::Running);

    EXPECT_EQ(actions.executed[3], 0);
    EXPECT_EQ(actions.executed[4], 2);
}

TEST_F(BehaviorTreeRuntimeTest, AbortsEveryRunningParallelActionExactlyOnce)
{
    ASSERT_TRUE(Prepare({ Node(1, 0, BTNodeType::Parallel), Node(2, 1, BTNodeType::Sequence),
        Node(3, 2, BTNodeType::RunScript), Node(4, 1, BTNodeType::RunScript, 1) }));
    ASSERT_EQ(Tick(), BTStatus::Running);

    AbortBehaviorTree(tree, state, ctx);
    AbortBehaviorTree(tree, state, ctx);

    EXPECT_EQ(actions.aborted[3], 1);
    EXPECT_EQ(actions.aborted[4], 1);
    EXPECT_EQ(state.runningLeaf, kInvalidNode);
}

TEST_F(BehaviorTreeRuntimeTest, AbortsUnfinishedParallelBranchesForEitherCompletionPolicy)
{
    for (const auto policy : { BTParallelPolicy::RequireAll, BTParallelPolicy::RequireOne }) {
        actions = RecordingActions{};
        auto root = Node(1, 0, BTNodeType::Parallel);
        root.successPolicy = policy;
        ASSERT_TRUE(Prepare({ root, Node(2, 1, BTNodeType::RunScript),
            Node(3, 1, BTNodeType::RunScript, 1) }));
        state.Clear();
        actions.results[3] = policy == BTParallelPolicy::RequireAll ? BTStatus::Failure : BTStatus::Success;

        EXPECT_EQ(Tick(), actions.results[3]);

        EXPECT_EQ(actions.aborted[2], 1);
        EXPECT_EQ(actions.aborted[3], 0);
    }
}

TEST_F(BehaviorTreeRuntimeTest, KeepsCompletedParallelChildrenInactiveAndCooldownAcrossAbort)
{
    ASSERT_TRUE(Prepare({ Node(1, 0, BTNodeType::Parallel), Node(2, 1, BTNodeType::Cooldown),
        Node(3, 2, BTNodeType::RunScript), Node(4, 1, BTNodeType::RunScript, 1) }));
    actions.results[3] = BTStatus::Success;

    EXPECT_EQ(Tick(), BTStatus::Running);
    EXPECT_EQ(Tick(), BTStatus::Running);

    EXPECT_EQ(actions.executed[3], 1);
    EXPECT_EQ(actions.executed[4], 2);
    AbortBehaviorTree(tree, state, ctx);
    EXPECT_EQ(Tick(), BTStatus::Failure);
    EXPECT_EQ(actions.executed[3], 1);
}

TEST_F(BehaviorTreeRuntimeTest, AbortsActionsWhenTimeLimitExpires)
{
    auto limit = Node(1, 0, BTNodeType::TimeLimit);
    limit.duration = 0.25f;
    ASSERT_TRUE(Prepare({ limit, Node(2, 1, BTNodeType::RunScript) }));
    ASSERT_EQ(Tick(0.2f), BTStatus::Running);

    EXPECT_EQ(Tick(0.1f), BTStatus::Failure);

    EXPECT_EQ(actions.executed[2], 1);
    EXPECT_EQ(actions.aborted[2], 1);
}

TEST_F(BehaviorTreeRuntimeTest, AbortsActionsWhenConditionClosesWithoutObserverMode)
{
    ASSERT_TRUE(Prepare({ Guard(1, 0), Node(2, 1, BTNodeType::RunScript) }));
    ASSERT_TRUE(blackboard.SetBool(bb::HasTarget, true));
    ASSERT_EQ(Tick(), BTStatus::Running);
    ASSERT_TRUE(blackboard.SetBool(bb::HasTarget, false));

    EXPECT_EQ(Tick(), BTStatus::Failure);

    EXPECT_EQ(actions.aborted[2], 1);
}

TEST_F(BehaviorTreeRuntimeTest, ObservesSelfLeafGuardWhileSequenceCursorSkipsIt)
{
    for (const auto mode : { AbortMode::Self, AbortMode::None }) {
        actions = RecordingActions{};
        ASSERT_TRUE(Prepare({ Node(1, 0, BTNodeType::Sequence),
            Guard(2, 1, mode, BTNodeType::BlackboardCompare), Node(3, 1, BTNodeType::RunScript, 1) }));
        state.Clear();
        ASSERT_TRUE(blackboard.SetBool(bb::HasTarget, true));
        ASSERT_EQ(Tick(), BTStatus::Running);
        ASSERT_TRUE(blackboard.SetBool(bb::HasTarget, false));

        EXPECT_EQ(Tick(), mode == AbortMode::Self ? BTStatus::Failure : BTStatus::Running);

        EXPECT_EQ(actions.executed[3], mode == AbortMode::Self ? 1 : 2);
        EXPECT_EQ(actions.aborted[3], mode == AbortMode::Self ? 1 : 0);
    }
}

TEST_F(BehaviorTreeRuntimeTest, PreemptsLowerPriorityThroughDecoratorAncestors)
{
    ASSERT_TRUE(Prepare({ Node(1, 0, BTNodeType::Selector), Node(2, 1, BTNodeType::TimeLimit),
        Guard(3, 2, AbortMode::Both), Node(4, 3, BTNodeType::RunScript),
        Node(5, 1, BTNodeType::RunScript, 1) }));
    ASSERT_EQ(Tick(), BTStatus::Running);
    ASSERT_TRUE(blackboard.SetBool(bb::HasTarget, true));

    EXPECT_EQ(Tick(), BTStatus::Running);

    EXPECT_EQ(actions.aborted[5], 1);
    EXPECT_EQ(actions.executed[4], 1);
    ASSERT_TRUE(blackboard.SetBool(bb::HasTarget, false));
    EXPECT_EQ(Tick(), BTStatus::Running);
    EXPECT_EQ(actions.aborted[4], 1);
    EXPECT_EQ(actions.executed[5], 2);
}

TEST_F(BehaviorTreeRuntimeTest, RequiresOtherSequenceGuardsBeforePreemptingLowerPriority)
{
    ASSERT_TRUE(Prepare({ Node(1, 0, BTNodeType::Selector), Node(2, 1, BTNodeType::Sequence),
        Guard(3, 2, AbortMode::LowerPriority, BTNodeType::BlackboardCompare), Node(4, 2, BTNodeType::AlwaysFail, 1),
        Node(5, 2, BTNodeType::RunScript, 2), Node(6, 1, BTNodeType::RunScript, 1) }));
    ASSERT_EQ(Tick(), BTStatus::Running);
    ASSERT_TRUE(blackboard.SetBool(bb::HasTarget, true));

    EXPECT_EQ(Tick(), BTStatus::Running);

    EXPECT_EQ(actions.aborted[6], 0);
    EXPECT_EQ(actions.executed[6], 2);
}

TEST_F(BehaviorTreeRuntimeTest, SharesTreeWhileKeepingWaitAndRestartStatePerInstance)
{
    auto wait = Node(1, 0, BTNodeType::Wait);
    wait.duration = 0.3f;
    ASSERT_TRUE(Prepare({ wait }));
    BTInstanceState other;
    other.rngState = 17;
    ASSERT_EQ(Tick(0.2f), BTStatus::Running);

    EXPECT_EQ(TickBehaviorTree(tree, other, blackboard, ctx), BTStatus::Running);
    AbortBehaviorTree(tree, state, ctx);
    state.Clear();
    EXPECT_EQ(Tick(0.2f), BTStatus::Running);
    EXPECT_EQ(TickBehaviorTree(tree, other, blackboard, ctx), BTStatus::Success);

    EXPECT_EQ(other.rngState, 17u);
}

TEST_F(BehaviorTreeRuntimeTest, ComparesAdjacentLargeIntegersWithoutFloatRounding)
{
    auto compare = Node(1, 0, BTNodeType::BlackboardCompare);
    compare.keyName = "Counter";
    compare.valueType = BlackboardType::Int;
    compare.valueInt = 16777216;
    compare.compareOp = BTCompareOp::Greater;
    ASSERT_TRUE(Prepare({ compare }));
    ASSERT_TRUE(blackboard.SetInt(tree.FindKey("Counter"), 16777217));

    EXPECT_EQ(Tick(), BTStatus::Success);
    EXPECT_FALSE(blackboard.SetFloat(tree.FindKey("Counter"), 1.0f));
    EXPECT_EQ(Tick(), BTStatus::Success);
}

TEST_F(BehaviorTreeRuntimeTest, RejectsUnsafeObserversAndExcessiveExecutionDepth)
{
    BehaviorTreeAsset asset;
    auto action = Node(1, 0, BTNodeType::RunScript);
    action.abortMode = AbortMode::Both;
    asset.nodes.push_back(action);
    EXPECT_FALSE(CompileBehaviorTree(asset, tree));
    asset.nodes.clear();
    for (int i = 1; i <= 257; ++i)
        asset.nodes.push_back(Node(i, i - 1, i == 257 ? BTNodeType::AlwaysRunning : BTNodeType::Inverter));

    EXPECT_FALSE(CompileBehaviorTree(asset, tree));
    asset.nodes.pop_back();
    asset.nodes.back().type = BTNodeType::AlwaysRunning;
    EXPECT_TRUE(CompileBehaviorTree(asset, tree));
}

} /// @note namespace fbzz::tests
