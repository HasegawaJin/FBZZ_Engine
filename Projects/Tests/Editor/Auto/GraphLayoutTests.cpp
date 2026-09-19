/// @file    GraphLayoutTests.cpp
/// @brief   ノードグラフの自動整列 (深さベースの列配置)。
/// @author  Hasegawa Jin
/// @date    2026-09-10
///
/// AnimatorGraph / BehaviorTree / VFX が同じ整列を使う。壊れると «並べ直したら
/// 矢印が逆流した» «整列のたびに位置が入れ替わる» という形で出る。
/// 循環グラフで止まらなくなる類の事故も、ここで踏み止める。
#include <TestKit/TestKit.hpp>

#include <Editor/GraphEditor/GraphLayoutAlgo.hpp>

#include <vector>

namespace fbzz::tests {
namespace {

using editor::ComputeGraphLayout;
using editor::GraphLayoutEdge;
using editor::GraphLayoutOptions;

/// 既定の options は originX=80 / columnStep=300。列番号は x から逆算できる。
int ColumnOf(float x, const GraphLayoutOptions& options = {})
{
    return static_cast<int>((x - options.originX) / options.columnStep + 0.5f);
}

} // namespace

TEST(GraphLayout, ReturnsNothingForAnEmptyGraph)
{
    /// @note 空の中括弧だと «根の一覧» と «options» のどちらにも見えて解決できない。
    ///       型を書いて、根を渡す 3 引数版であることを固定する。
    const std::vector<int>             noNodes;
    const std::vector<GraphLayoutEdge> noEdges;
    const std::vector<int>             noRoots;

    const auto positions = ComputeGraphLayout(noNodes, noEdges, noRoots);

    EXPECT_TRUE(positions.empty());
}

TEST(GraphLayout, PlacesEveryNodeExactlyOnce)
{
    const std::vector<int> nodes{ 1, 2, 3 };
    const std::vector<GraphLayoutEdge> edges{ { 1, 2 }, { 2, 3 } };

    const auto positions = ComputeGraphLayout(nodes, edges, std::vector<int>{ 1 });

    EXPECT_EQ(positions.size(), 3u);
    for (const int id : nodes)
        EXPECT_NE(positions.find(id), positions.end()) << "missing node " << id;
}

TEST(GraphLayout, LaysChildrenToTheRightOfTheirParent)
{
    const std::vector<int> nodes{ 1, 2, 3 };
    const std::vector<GraphLayoutEdge> edges{ { 1, 2 }, { 2, 3 } };

    const auto positions = ComputeGraphLayout(nodes, edges, std::vector<int>{ 1 });

    EXPECT_LT(positions.at(1).x, positions.at(2).x);
    EXPECT_LT(positions.at(2).x, positions.at(3).x);
}

TEST(GraphLayout, UsesTheLongestPathSoArrowsNeverPointBackwards)
{
    /// @note 1->2->3 と 1->3 の両方がある。最短で置くと 3 が 2 と同じ列に来て矢印が逆流する。
    const std::vector<int> nodes{ 1, 2, 3 };
    const std::vector<GraphLayoutEdge> edges{ { 1, 2 }, { 2, 3 }, { 1, 3 } };

    const auto positions = ComputeGraphLayout(nodes, edges, std::vector<int>{ 1 });

    EXPECT_EQ(ColumnOf(positions.at(1).x), 0);
    EXPECT_EQ(ColumnOf(positions.at(2).x), 1);
    EXPECT_EQ(ColumnOf(positions.at(3).x), 2);
}

TEST(GraphLayout, SiblingsShareAColumnAndStackVertically)
{
    const std::vector<int> nodes{ 1, 2, 3 };
    const std::vector<GraphLayoutEdge> edges{ { 1, 2 }, { 1, 3 } };

    const auto positions = ComputeGraphLayout(nodes, edges, std::vector<int>{ 1 });

    EXPECT_FLOAT_EQ(positions.at(2).x, positions.at(3).x);
    EXPECT_NE(positions.at(2).y, positions.at(3).y);
}

TEST(GraphLayout, KeepsTheGivenOrderWithinAColumn)
{
    /// @note 整列のたびに上下が入れ替わると、覚えた位置関係が壊れる。
    const std::vector<int> nodes{ 1, 2, 3 };
    const std::vector<GraphLayoutEdge> edges{ { 1, 2 }, { 1, 3 } };

    const auto positions = ComputeGraphLayout(nodes, edges, std::vector<int>{ 1 });

    EXPECT_LT(positions.at(2).y, positions.at(3).y);
}

TEST(GraphLayout, IsStableAcrossRepeatedRuns)
{
    const std::vector<int> nodes{ 1, 2, 3, 4 };
    const std::vector<GraphLayoutEdge> edges{ { 1, 2 }, { 1, 3 }, { 2, 4 } };

    const auto first  = ComputeGraphLayout(nodes, edges, std::vector<int>{ 1 });
    const auto second = ComputeGraphLayout(nodes, edges, std::vector<int>{ 1 });

    for (const int id : nodes) {
        EXPECT_FLOAT_EQ(first.at(id).x, second.at(id).x) << "node " << id;
        EXPECT_FLOAT_EQ(first.at(id).y, second.at(id).y) << "node " << id;
    }
}

TEST(GraphLayout, PutsUnreachableNodesPastTheDeepestColumn)
{
    /// @note どこからも辿れないノードを根と同じ列に置くと、根と見分けが付かなくなる。
    const std::vector<int> nodes{ 1, 2, 99 };
    const std::vector<GraphLayoutEdge> edges{ { 1, 2 } };

    const auto positions = ComputeGraphLayout(nodes, edges, std::vector<int>{ 1 });

    EXPECT_GT(positions.at(99).x, positions.at(2).x);
}

TEST(GraphLayout, TerminatesOnACycle)
{
    /// @note 循環があっても «深さの更新» が止まらなくなってはいけない。
    ///       返ってくること自体が、このテストの検証内容 (止まればタイムアウトで落ちる)。
    const std::vector<int> nodes{ 1, 2, 3 };
    const std::vector<GraphLayoutEdge> edges{ { 1, 2 }, { 2, 3 }, { 3, 1 } };

    const auto positions = ComputeGraphLayout(nodes, edges, std::vector<int>{ 1 });

    EXPECT_EQ(positions.size(), 3u);
}

TEST(GraphLayout, TerminatesOnASelfLoop)
{
    const std::vector<int> nodes{ 1 };
    const std::vector<GraphLayoutEdge> edges{ { 1, 1 } };

    const auto positions = ComputeGraphLayout(nodes, edges, std::vector<int>{ 1 });

    EXPECT_EQ(positions.size(), 1u);
}

TEST(GraphLayout, IgnoresEdgesPointingAtUnknownNodes)
{
    /// @note 削除済みノードを指す辺が残っていても、整列ごと壊れてはいけない。
    const std::vector<int> nodes{ 1, 2 };
    const std::vector<GraphLayoutEdge> edges{ { 1, 2 }, { 2, 404 }, { 404, 1 } };

    const auto positions = ComputeGraphLayout(nodes, edges, std::vector<int>{ 1 });

    EXPECT_EQ(positions.size(), 2u);
    EXPECT_EQ(positions.find(404), positions.end());
}

TEST(GraphLayout, IgnoresRootsThatAreNotInTheGraph)
{
    const std::vector<int> nodes{ 1, 2 };
    const std::vector<GraphLayoutEdge> edges{ { 1, 2 } };

    const auto positions = ComputeGraphLayout(nodes, edges, std::vector<int>{ 404 });

    /// @note 根が 1 つも無いので全ノードが到達不能。落ちずに全部置ければよい。
    EXPECT_EQ(positions.size(), 2u);
}

TEST(GraphLayout, DerivesRootsFromNodesWithNoIncomingEdge)
{
    /// @note 根を渡さない版。入次数 0 を根とみなす。
    const std::vector<int> nodes{ 1, 2, 3 };
    const std::vector<GraphLayoutEdge> edges{ { 1, 2 }, { 2, 3 } };

    const auto positions = ComputeGraphLayout(nodes, edges);

    EXPECT_EQ(ColumnOf(positions.at(1).x), 0);
    EXPECT_LT(positions.at(1).x, positions.at(2).x);
    EXPECT_LT(positions.at(2).x, positions.at(3).x);
}

TEST(GraphLayout, HonoursTheSpacingOptions)
{
    GraphLayoutOptions options;
    options.originX    = 10.0f;
    options.originY    = 20.0f;
    options.columnStep = 100.0f;
    options.rowStep    = 50.0f;
    options.centerColumns = false;

    const std::vector<int> nodes{ 1, 2 };
    const std::vector<GraphLayoutEdge> edges{ { 1, 2 } };

    const auto positions = ComputeGraphLayout(nodes, edges, std::vector<int>{ 1 }, options);

    EXPECT_FLOAT_EQ(positions.at(1).x, 10.0f);
    EXPECT_FLOAT_EQ(positions.at(1).y, 20.0f);
    EXPECT_FLOAT_EQ(positions.at(2).x, 110.0f);
}

TEST(GraphLayout, CenteringOnlyShiftsTheShorterColumn)
{
    GraphLayoutOptions centered;
    centered.centerColumns = true;
    GraphLayoutOptions flat;
    flat.centerColumns = false;

    /// @note 列 0 に 1 個、列 1 に 2 個。センタリングすると短い列 0 だけが下がる。
    const std::vector<int> nodes{ 1, 2, 3 };
    const std::vector<GraphLayoutEdge> edges{ { 1, 2 }, { 1, 3 } };

    const auto withCenter = ComputeGraphLayout(nodes, edges, std::vector<int>{ 1 }, centered);
    const auto withoutCenter = ComputeGraphLayout(nodes, edges, std::vector<int>{ 1 }, flat);

    EXPECT_GT(withCenter.at(1).y, withoutCenter.at(1).y);
    EXPECT_FLOAT_EQ(withCenter.at(2).y, withoutCenter.at(2).y);
}

} // namespace fbzz::tests
