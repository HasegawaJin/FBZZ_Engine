/// @file    RenderGraphLifetimeTests.cpp
/// @brief   リソース寿命の算出とエイリアスグループの割り当て規則を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-10
///
/// エイリアスグループは「ライフタイムが重ならない同型リソースは 1 つの物理 RT を
/// 共有してよい」という申告で、実際に VRAM を貸し回す判断の根拠になる。誤って
/// 重なる 2 つを同じグループへ入れると、片方の描画がもう片方を踏み潰す。絵を見て
/// 気付ける壊れ方ではないので、割り当て規則そのものを固定する。
///
/// 貸し回しの判定は «前のリソースの lastPass < 次のリソースの firstPass» という
/// 厳密な不等号。隣接するだけ (lastPass == firstPass) では共有しない。テストの
/// 構成に 1 段の «間» が要るのはこのため。
#include <TestKit/TestKit.hpp>

#include <Engine/Renderer/RenderGraph.hpp>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace fbzz::tests {
namespace {

using RG = renderer::RenderGraph;

void AddPass(RG& graph,
             std::string_view name,
             std::initializer_list<std::string_view> reads,
             std::initializer_list<std::string_view> writes)
{
    graph.AddPass(name, reads, writes, [] {});
}

RG::ResourceDesc TransientRT(uint32_t width = 1920, uint32_t height = 1080)
{
    return RG::ResourceDesc{ RG::ResourceKind::RenderTarget, width, height,
                             renderer::Format::RGBA16F, 1, true, false, true };
}

RG::ResourceDesc ImportedRT(uint32_t width = 1920, uint32_t height = 1080)
{
    return RG::ResourceDesc{ RG::ResourceKind::RenderTarget, width, height,
                             renderer::Format::RGBA16F, 1, true, true, false };
}

/// external ではないが transient でもない = «永続»。producer 無しで読めて、貸し回されない。
RG::ResourceDesc PersistentRT(uint32_t width = 1920, uint32_t height = 1080)
{
    return RG::ResourceDesc{ RG::ResourceKind::RenderTarget, width, height,
                             renderer::Format::RGBA16F, 1, true, false, false };
}

/// 深度専用 RT (colorCount = 0)。DecalDepth / cloudDepth がこれ。
RG::ResourceDesc TransientDepthOnlyRT(uint32_t width = 1920, uint32_t height = 1080)
{
    return RG::ResourceDesc{ RG::ResourceKind::RenderTarget, width, height,
                             renderer::Format::RGBA16F, 0, true, false, true };
}

/// 深度を持たないポストの中継 RT。
RG::ResourceDesc TransientColorOnlyRT(uint32_t width = 1920, uint32_t height = 1080)
{
    return RG::ResourceDesc{ RG::ResourceKind::RenderTarget, width, height,
                             renderer::Format::RGBA16F, 1, false, false, true };
}

const RG::ResourceLifetime* Find(const RG& graph, std::string_view name)
{
    const auto& lifetimes = graph.GetLastReport().lifetimes;
    const auto it = std::find_if(lifetimes.begin(), lifetimes.end(),
        [name](const RG::ResourceLifetime& lt) { return lt.name == name; });
    return it == lifetimes.end() ? nullptr : &*it;
}

} // namespace

class RenderGraphLifetimeTest : public testkit::Fixture {};

/// @name 寿命

TEST_F(RenderGraphLifetimeTest, ReportsLifetimeBoundsAsExecutionOrderIndices)
{
    /// @note firstPass / lastPass は «登録番号» ではなく «カリング後の実行順の番号»。
    ///       取り違えると、刈られたパスがある構成で寿命が実際より長く出る。
    RG graph;
    graph.DeclareResource("HDR", TransientRT());
    graph.DeclareResource("Unused", TransientRT());
    graph.DeclareResource("Output", ImportedRT());
    AddPass(graph, "Geometry", {}, { "HDR" });
    AddPass(graph, "Orphan", {}, { "Unused" });
    AddPass(graph, "Present", { "HDR" }, { "Output" });
    graph.SetOutputs({ "Output" });

    ASSERT_TRUE(graph.Plan());

    /// @note Orphan が刈られるので、Present の実行順番号は登録番号の 2 ではなく 1。
    const RG::ResourceLifetime* hdr = Find(graph, "HDR");
    ASSERT_NE(hdr, nullptr);
    EXPECT_EQ(hdr->firstPass, 0);
    EXPECT_EQ(hdr->lastPass, 1);
}

TEST_F(RenderGraphLifetimeTest, OmitsResourcesTouchedOnlyByCulledPasses)
{
    RG graph;
    graph.DeclareResource("HDR", TransientRT());
    graph.DeclareResource("Unused", TransientRT());
    graph.DeclareResource("Output", ImportedRT());
    AddPass(graph, "Geometry", {}, { "HDR" });
    AddPass(graph, "Orphan", {}, { "Unused" });
    AddPass(graph, "Present", { "HDR" }, { "Output" });
    graph.SetOutputs({ "Output" });

    ASSERT_TRUE(graph.Plan());

    EXPECT_EQ(Find(graph, "Unused"), nullptr);
}

TEST_F(RenderGraphLifetimeTest, SortsLifetimesByFirstUseThenName)
{
    /// @note RenderPipeline はこの並び順でプールを組み直す。並びが変われば、同じ構成でも
    ///       別の物理 RT が配られる。
    RG graph;
    graph.DeclareResource("Zebra", TransientRT());
    graph.DeclareResource("Alpha", TransientRT());
    graph.DeclareResource("Output", ImportedRT());
    AddPass(graph, "MakeBoth", {}, { "Zebra", "Alpha" });
    AddPass(graph, "Present", { "Zebra", "Alpha" }, { "Output" });
    graph.SetOutputs({ "Output" });

    ASSERT_TRUE(graph.Plan());

    const auto& lifetimes = graph.GetLastReport().lifetimes;
    ASSERT_GE(lifetimes.size(), 2u);
    EXPECT_EQ(lifetimes[0].name, "Alpha");
    EXPECT_EQ(lifetimes[1].name, "Zebra");
}

/// @name エイリアス

TEST_F(RenderGraphLifetimeTest, SharesOneGroupBetweenResourcesWhoseLifetimesDoNotOverlap)
{
    /// @note Early は Middle で死に、Late は MakeLate で生まれる。間が 1 段空くので貸し回せる。
    RG graph;
    graph.DeclareResource("Early", TransientRT());
    graph.DeclareResource("Mid", TransientRT());
    graph.DeclareResource("Late", TransientRT());
    graph.DeclareResource("Output", ImportedRT());
    AddPass(graph, "MakeEarly", {}, { "Early" });
    AddPass(graph, "Middle", { "Early" }, { "Mid" });
    AddPass(graph, "MakeLate", { "Mid" }, { "Late" });
    AddPass(graph, "Present", { "Late" }, { "Output" });
    graph.SetOutputs({ "Output" });

    ASSERT_TRUE(graph.Plan());

    const RG::ResourceLifetime* early = Find(graph, "Early");
    const RG::ResourceLifetime* late  = Find(graph, "Late");
    ASSERT_NE(early, nullptr);
    ASSERT_NE(late, nullptr);
    EXPECT_LT(early->lastPass, late->firstPass);
    EXPECT_EQ(early->aliasGroup, late->aliasGroup);
}

TEST_F(RenderGraphLifetimeTest, SeparatesGroupsForResourcesAliveAtTheSameTime)
{
    RG graph;
    graph.DeclareResource("A", TransientRT());
    graph.DeclareResource("B", TransientRT());
    graph.DeclareResource("Output", ImportedRT());
    AddPass(graph, "MakeA", {}, { "A" });
    AddPass(graph, "MakeB", {}, { "B" });
    AddPass(graph, "Combine", { "A", "B" }, { "Output" });
    graph.SetOutputs({ "Output" });

    ASSERT_TRUE(graph.Plan());

    const RG::ResourceLifetime* a = Find(graph, "A");
    const RG::ResourceLifetime* b = Find(graph, "B");
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_NE(a->aliasGroup, b->aliasGroup);
}

TEST_F(RenderGraphLifetimeTest, RefusesToShareAGroupWhenDescriptorsDiffer)
{
    /// @note Half は寿命の上では Full の枠に収まるが、寸法が違うので貸し回してはいけない。
    ///       上の SharesOneGroup... と同じ形で desc だけを変えてある。
    RG graph;
    graph.DeclareResource("Full", TransientRT(1920, 1080));
    graph.DeclareResource("Mid", TransientRT(1920, 1080));
    graph.DeclareResource("Half", TransientRT(960, 540));
    graph.DeclareResource("Output", ImportedRT());
    AddPass(graph, "MakeFull", {}, { "Full" });
    AddPass(graph, "Middle", { "Full" }, { "Mid" });
    AddPass(graph, "Downsample", { "Mid" }, { "Half" });
    AddPass(graph, "Present", { "Half" }, { "Output" });
    graph.SetOutputs({ "Output" });

    ASSERT_TRUE(graph.Plan());

    const RG::ResourceLifetime* full = Find(graph, "Full");
    const RG::ResourceLifetime* half = Find(graph, "Half");
    ASSERT_NE(full, nullptr);
    ASSERT_NE(half, nullptr);
    /// @note 枠は空いている
    EXPECT_LT(full->lastPass, half->firstPass);
    /// @note それでも共有しない
    EXPECT_NE(full->aliasGroup, half->aliasGroup);
}

TEST_F(RenderGraphLifetimeTest, RefusesToShareAGroupBetweenDepthOnlyAndColourTargets)
{
    /// @note 寸法も形式も同じだが colorCount が違う。実体の作られ方が別物なので貸し回せない。
    ///       以前は寸法と形式しか見ておらず、深度専用の DecalDepth とカラーマスクが
    ///       同じグループに入り得た。
    RG graph;
    graph.DeclareResource("Colour", TransientRT(1920, 1080));
    graph.DeclareResource("Mid",    TransientRT(1920, 1080));
    graph.DeclareResource("Depth",  TransientDepthOnlyRT(1920, 1080));
    graph.DeclareResource("Output", ImportedRT());
    AddPass(graph, "MakeColour", {}, { "Colour" });
    AddPass(graph, "Middle", { "Colour" }, { "Mid" });
    AddPass(graph, "MakeDepth", { "Mid" }, { "Depth" });
    AddPass(graph, "Present", { "Depth" }, { "Output" });
    graph.SetOutputs({ "Output" });

    ASSERT_TRUE(graph.Plan());

    const RG::ResourceLifetime* colour = Find(graph, "Colour");
    const RG::ResourceLifetime* depth  = Find(graph, "Depth");
    ASSERT_NE(colour, nullptr);
    ASSERT_NE(depth, nullptr);
    /// @note 枠は空いている
    EXPECT_LT(colour->lastPass, depth->firstPass);
    /// @note それでも共有しない
    EXPECT_NE(colour->aliasGroup, depth->aliasGroup);
}

TEST_F(RenderGraphLifetimeTest, RefusesToShareAGroupWhenOnlyTheDepthFlagDiffers)
{
    /// @note colorCount も形式も同じで、深度を持つかだけが違う。
    RG graph;
    graph.DeclareResource("WithDepth", TransientRT(1920, 1080));
    graph.DeclareResource("Mid",       TransientRT(1920, 1080));
    graph.DeclareResource("NoDepth",   TransientColorOnlyRT(1920, 1080));
    graph.DeclareResource("Output",    ImportedRT());
    AddPass(graph, "MakeWithDepth", {}, { "WithDepth" });
    AddPass(graph, "Middle", { "WithDepth" }, { "Mid" });
    AddPass(graph, "MakeNoDepth", { "Mid" }, { "NoDepth" });
    AddPass(graph, "Present", { "NoDepth" }, { "Output" });
    graph.SetOutputs({ "Output" });

    ASSERT_TRUE(graph.Plan());

    const RG::ResourceLifetime* withDepth = Find(graph, "WithDepth");
    const RG::ResourceLifetime* noDepth   = Find(graph, "NoDepth");
    ASSERT_NE(withDepth, nullptr);
    ASSERT_NE(noDepth, nullptr);
    EXPECT_LT(withDepth->lastPass, noDepth->firstPass);
    EXPECT_NE(withDepth->aliasGroup, noDepth->aliasGroup);
}

TEST_F(RenderGraphLifetimeTest, ExcludesImportedAndPersistentResourcesFromAliasing)
{
    /// @note 外から持ち込んだ RT と、フレームを跨いで内容を保つ RT は貸し回してはいけない。
    ///       aliasGroup = -1 がその印。
    RG graph;
    graph.DeclareResource("HDR", TransientRT());
    graph.DeclareResource("History", PersistentRT());
    graph.DeclareResource("Output", ImportedRT());
    AddPass(graph, "Geometry", {}, { "HDR" });
    AddPass(graph, "Resolve", { "HDR", "History" }, { "Output" });
    graph.SetOutputs({ "Output" });

    ASSERT_TRUE(graph.Plan());

    const RG::ResourceLifetime* hdr     = Find(graph, "HDR");
    const RG::ResourceLifetime* history = Find(graph, "History");
    const RG::ResourceLifetime* output  = Find(graph, "Output");
    ASSERT_NE(hdr, nullptr);
    ASSERT_NE(history, nullptr);
    ASSERT_NE(output, nullptr);
    EXPECT_EQ(history->aliasGroup, -1);
    EXPECT_EQ(output->aliasGroup, -1);
    EXPECT_GE(hdr->aliasGroup, 0);
}

TEST_F(RenderGraphLifetimeTest, SharesAGroupBetweenUndeclaredResourcesBecauseTheyGetTheDefaultDesc)
{
    /// @note DeclareResource を書き忘れた名前は desc が既定値 {Unknown, 0, 0} になる。
    ///       寸法も kind も揃ってしまうので、本来まったく別物のリソース同士が «同型» と
    ///       判定されて同じ物理実体を貸し回される。
    ///       現状これを検出する仕組みは無い。所有権をグラフへ移す前に必ず塞ぐこと。
    RG graph;
    graph.DeclareResource("Output", ImportedRT());
    AddPass(graph, "MakeA", {}, { "UndeclaredA" });
    AddPass(graph, "Middle", { "UndeclaredA" }, { "UndeclaredMid" });
    AddPass(graph, "MakeB", { "UndeclaredMid" }, { "UndeclaredB" });
    AddPass(graph, "Present", { "UndeclaredB" }, { "Output" });
    graph.SetOutputs({ "Output" });

    ASSERT_TRUE(graph.Plan());

    const RG::ResourceLifetime* a = Find(graph, "UndeclaredA");
    const RG::ResourceLifetime* b = Find(graph, "UndeclaredB");
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(a->desc.kind, RG::ResourceKind::Unknown);
    EXPECT_EQ(a->desc.width, 0u);
    EXPECT_EQ(a->aliasGroup, b->aliasGroup);
}

TEST_F(RenderGraphLifetimeTest, AssignsIdenticalAliasGroupsAcrossRepeatedPlans)
{
    /// @note グループ番号は物理 RT の貸出先そのもの。unordered_map の走査順に依存して
    ///       揺れると、フレームごとに違う RT へ描くことになる。
    RG graph;
    graph.DeclareResource("A", TransientRT());
    graph.DeclareResource("B", TransientRT());
    graph.DeclareResource("C", TransientRT());
    graph.DeclareResource("Output", ImportedRT());
    AddPass(graph, "MakeA", {}, { "A" });
    AddPass(graph, "MakeB", { "A" }, { "B" });
    AddPass(graph, "MakeC", { "B" }, { "C" });
    AddPass(graph, "Present", { "C" }, { "Output" });
    graph.SetOutputs({ "Output" });

    const auto groupsOf = [&graph] {
        std::vector<std::pair<std::string, int>> result;
        for (const auto& lifetime : graph.GetLastReport().lifetimes)
            result.emplace_back(lifetime.name, lifetime.aliasGroup);
        return result;
    };

    ASSERT_TRUE(graph.Plan());
    const std::vector<std::pair<std::string, int>> first = groupsOf();
    ASSERT_TRUE(graph.Plan());
    const std::vector<std::pair<std::string, int>> second = groupsOf();

    EXPECT_EQ(first, second);
}

} // namespace fbzz::tests
