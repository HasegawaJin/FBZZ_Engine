/// @file    RenderGraphTests.cpp
/// @brief   RenderGraph の実行順・デッドパスカリング・依存エラーの契約を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-10
///
/// RenderGraph は GPU を触らない純粋な依存解決器 (ResourceHandle も ResourceManager も
/// include していない) なので、デバイス無しで契約を固定できる。
///
/// 実行順は «申告された依存» から導かれる。登録順が持つ役目は 2 つだけ:
///   1. 同じリソースへ複数のパスが書くときの世代の順序 (Sky は Geometry の上に描く)
///   2. 依存が何も言っていないときのタイブレーク
/// どちらも意図であって偶然ではないので、そこは登録順に従う。それ以外は依存が決めるため、
/// 生産者を消費者より後ろに登録しても正しく並ぶ。
///
/// 併せて «申告が正しい限り実行順は登録順に一致する» ことも固定する。既存のパイプラインが
/// スケジューラーの導入で動かないことの根拠がこれ。
#include <TestKit/TestKit.hpp>

#include <Engine/Renderer/RenderGraph.hpp>

#include <algorithm>
#include <string>
#include <vector>

namespace fbzz::tests {
namespace {

using RG = renderer::RenderGraph;

/// 実行順をパス名で受け取る。index のままだと失敗メッセージから何も読めない。
std::vector<std::string> OrderedNames(const RG& graph)
{
    std::vector<std::string> names;
    for (const size_t index : graph.GetLastReport().executionOrder)
        names.push_back(graph.GetPasses()[index].name);
    return names;
}

std::vector<std::string> CulledNames(const RG& graph)
{
    std::vector<std::string> names;
    for (const size_t index : graph.GetLastReport().culledPasses)
        names.push_back(graph.GetPasses()[index].name);
    return names;
}

void AddPass(RG& graph,
             std::string_view name,
             std::initializer_list<std::string_view> reads,
             std::initializer_list<std::string_view> writes,
             bool allowCulling = true)
{
    graph.AddPass(name, reads, writes, [] {}, allowCulling);
}

RG::ResourceDesc TransientRT(uint32_t width = 1920, uint32_t height = 1080)
{
    return RG::ResourceDesc{ RG::ResourceKind::RenderTarget, width, height,
                             renderer::Format::RGBA16F, 1, true, false, true };
}

/// external か !transient が «外から持ち込まれた» の印。producer が居なくても読める。
RG::ResourceDesc ImportedRT(uint32_t width = 1920, uint32_t height = 1080)
{
    return RG::ResourceDesc{ RG::ResourceKind::RenderTarget, width, height,
                             renderer::Format::RGBA16F, 1, true, true, false };
}

} // namespace

class RenderGraphTest : public testkit::Fixture {};

TEST_F(RenderGraphTest, CaptureHookRunsAfterProfilingAndBeforeTheNextOverwrite)
{
    RG graph;
    int imageValue = 0;
    bool gpuActive = false;
    std::vector<int> captured;
    graph.AddPass("Opaque", {}, { "HDR" }, [&] { imageValue = 7; });
    graph.AddPass("Unused", {}, { "UnusedImage" }, [&] { ADD_FAILURE(); });
    graph.AddPass("Transparent", { "HDR" }, { "HDR" }, [&] { imageValue = 42; });
    graph.AddPass("Composite", { "HDR" }, { "Output" }, [] {});
    graph.SetOutputs({ "Output" });
    graph.SetGpuProfilerHooks([&](std::string_view) { gpuActive = true; },
                              [&](std::string_view) { gpuActive = false; });
    graph.SetPassCompletedHook([&](size_t index) {
        EXPECT_FALSE(gpuActive);
        EXPECT_NE(index, 1u);
        EXPECT_EQ(graph.GetLastReport().profiles.size(), captured.size() + 1);
        captured.push_back(imageValue);
    });

    ASSERT_TRUE(graph.Execute());
    EXPECT_EQ(captured, (std::vector<int>{ 7, 42, 42 }));
    EXPECT_EQ(graph.GetLastReport().culledPasses, (std::vector<size_t>{ 1 }));
}

TEST_F(RenderGraphTest, FailedPlanDoesNotInvokeCaptureHook)
{
    RG graph;
    bool captured = false;
    graph.AddPass("A", { "B" }, { "A" }, [] {});
    graph.AddPass("B", { "A" }, { "B" }, [] {});
    graph.SetOutputs({ "A" });
    graph.SetPassCompletedHook([&](size_t) { captured = true; });
    EXPECT_FALSE(graph.Execute());
    EXPECT_FALSE(captured);
}

/// @name 実行順

TEST_F(RenderGraphTest, ExecutesLivePassesInRegistrationOrder)
{
    RG graph;
    graph.DeclareResource("HDR", TransientRT());
    graph.DeclareResource("Output", ImportedRT());
    AddPass(graph, "Geometry", {}, { "HDR" });
    AddPass(graph, "Bloom", { "HDR" }, { "Bloom" });
    AddPass(graph, "Composite", { "HDR", "Bloom" }, { "Output" });
    graph.SetOutputs({ "Output" });

    ASSERT_TRUE(graph.Plan());

    EXPECT_EQ(OrderedNames(graph),
              (std::vector<std::string>{ "Geometry", "Bloom", "Composite" }));
}

TEST_F(RenderGraphTest, KeepsRelativeOrderOfExistingPassesWhenAnotherIsInserted)
{
    /// @note 「追加順 = 優先度」の約束。ここが崩れると、パスを 1 本足しただけで
    ///       無関係なパスの相対順序が入れ替わり、絵が静かに変わる。
    const auto orderWith = [](bool withExtra) {
        RG graph;
        graph.DeclareResource("HDR", TransientRT());
        graph.DeclareResource("Output", ImportedRT());
        AddPass(graph, "Geometry", {}, { "HDR" });
        if (withExtra)
            AddPass(graph, "Decal", { "HDR" }, { "HDR" });
        AddPass(graph, "Particle", { "HDR" }, { "HDR" });
        AddPass(graph, "Composite", { "HDR" }, { "Output" });
        graph.SetOutputs({ "Output" });
        EXPECT_TRUE(graph.Plan());
        return OrderedNames(graph);
    };

    const std::vector<std::string> without = orderWith(false);
    const std::vector<std::string> with    = orderWith(true);

    EXPECT_EQ(without, (std::vector<std::string>{ "Geometry", "Particle", "Composite" }));
    EXPECT_EQ(with,
              (std::vector<std::string>{ "Geometry", "Decal", "Particle", "Composite" }));
}

TEST_F(RenderGraphTest, ProducesIdenticalOrderAcrossRepeatedPlans)
{
    /// @note producer / readersSinceLastWrite / byName が unordered_map を経由するので、
    ///       実装が «たまたま» 決まった順に依存していると、ここで揺れる。
    RG graph;
    graph.DeclareResource("A", TransientRT());
    graph.DeclareResource("B", TransientRT());
    graph.DeclareResource("Output", ImportedRT());
    AddPass(graph, "MakeA", {}, { "A" });
    AddPass(graph, "MakeB", {}, { "B" });
    AddPass(graph, "Combine", { "A", "B" }, { "Output" });
    graph.SetOutputs({ "Output" });

    ASSERT_TRUE(graph.Plan());
    const std::vector<std::string> first = OrderedNames(graph);
    ASSERT_TRUE(graph.Plan());
    const std::vector<std::string> second = OrderedNames(graph);

    EXPECT_EQ(first, second);
}

/// @name ハザード

TEST_F(RenderGraphTest, DoesNotDeadlockWhenAPassReadsAndWritesTheSameResource)
{
    /// @note ReadWrite は RAW と WAR の両方の辺を張るが、自分自身への辺は張ってはいけない。
    ///       張ると indegree が 0 に戻らず、そのパスが実行順から丸ごと落ちる。
    RG graph;
    graph.DeclareResource("HDR", TransientRT());
    graph.DeclareResource("Output", ImportedRT());
    AddPass(graph, "Geometry", {}, { "HDR" });
    graph.AddPass("Tonemap",
                  { RG::ResourceAccess{ "HDR", RG::ResourceUsage::ReadWrite } },
                  [] {});
    AddPass(graph, "Present", { "HDR" }, { "Output" });
    graph.SetOutputs({ "Output" });

    ASSERT_TRUE(graph.Plan());

    EXPECT_EQ(OrderedNames(graph),
              (std::vector<std::string>{ "Geometry", "Tonemap", "Present" }));
}

TEST_F(RenderGraphTest, OrdersOverwritingPassAfterEveryReaderOfThePreviousGeneration)
{
    /// @note WAR: HDR を読む Bloom / SSAO を追い越して HDR を上書きしてはいけない。
    RG graph;
    graph.DeclareResource("HDR", TransientRT());
    graph.DeclareResource("Output", ImportedRT());
    AddPass(graph, "Geometry", {}, { "HDR" });
    AddPass(graph, "Bloom", { "HDR" }, { "Bloom" });
    AddPass(graph, "SSAO", { "HDR" }, { "SSAO" });
    AddPass(graph, "Overwrite", { "Bloom", "SSAO" }, { "HDR" });
    AddPass(graph, "Present", { "HDR" }, { "Output" });
    graph.SetOutputs({ "Output" });

    ASSERT_TRUE(graph.Plan());

    const std::vector<std::string> order = OrderedNames(graph);
    const auto indexOf = [&order](std::string_view name) {
        return std::find(order.begin(), order.end(), name) - order.begin();
    };
    EXPECT_LT(indexOf("Bloom"), indexOf("Overwrite"));
    EXPECT_LT(indexOf("SSAO"), indexOf("Overwrite"));
}

/// @name カリング

TEST_F(RenderGraphTest, CullsPassesThatNoOutputDependsOn)
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

    EXPECT_EQ(CulledNames(graph), (std::vector<std::string>{ "Orphan" }));
    EXPECT_EQ(OrderedNames(graph), (std::vector<std::string>{ "Geometry", "Present" }));
}

TEST_F(RenderGraphTest, KeepsPassesThatOptedOutOfCullingEvenWhenUnreachable)
{
    /// @note 外部副作用だけが目的のパス (IBL の BRDF LUT 焼き付けなど) の受け皿。
    RG graph;
    graph.DeclareResource("HDR", TransientRT());
    graph.DeclareResource("Output", ImportedRT());
    AddPass(graph, "Geometry", {}, { "HDR" });
    AddPass(graph, "BakeLut", {}, {}, /*allowCulling=*/false);
    AddPass(graph, "Present", { "HDR" }, { "Output" });
    graph.SetOutputs({ "Output" });

    ASSERT_TRUE(graph.Plan());

    EXPECT_TRUE(CulledNames(graph).empty());
    EXPECT_EQ(OrderedNames(graph),
              (std::vector<std::string>{ "Geometry", "BakeLut", "Present" }));
}

TEST_F(RenderGraphTest, KeepsEveryPassWhenNoOutputIsDeclared)
{
    RG graph;
    graph.DeclareResource("HDR", TransientRT());
    AddPass(graph, "Geometry", {}, { "HDR" });
    AddPass(graph, "Orphan", {}, { "Unused" });

    ASSERT_TRUE(graph.Plan());

    EXPECT_TRUE(CulledNames(graph).empty());
    EXPECT_EQ(OrderedNames(graph), (std::vector<std::string>{ "Geometry", "Orphan" }));
}

/// @name エラー

TEST_F(RenderGraphTest, RejectsPlanWhenNoLivePassWritesARequiredOutput)
{
    /// @note 実際に起きた不具合の回帰。Composite の «出力先を LDR にするか Output にするか» の
    ///       判定が RenderSystem 側とパス側で食い違うと、Output を誰も書かないフレームになる。
    RG graph;
    graph.DeclareResource("HDR", TransientRT());
    graph.DeclareResource("LDR", TransientRT());
    graph.DeclareResource("Output", ImportedRT());
    AddPass(graph, "Geometry", {}, { "HDR" });
    AddPass(graph, "Composite", { "HDR" }, { "LDR" });
    graph.SetOutputs({ "Output" });

    std::string logged;
    graph.SetDebugLogHook([&logged](const char* message) { logged = message; });

    EXPECT_FALSE(graph.Plan());
    EXPECT_NE(logged.find("Output"), std::string::npos);
}

TEST_F(RenderGraphTest, RejectsPlanWhenAReadResourceHasNoProducerAndIsNotImported)
{
    RG graph;
    graph.DeclareResource("Missing", TransientRT());
    graph.DeclareResource("Output", ImportedRT());
    AddPass(graph, "Present", { "Missing" }, { "Output" });
    graph.SetOutputs({ "Output" });

    std::string logged;
    graph.SetDebugLogHook([&logged](const char* message) { logged = message; });

    EXPECT_FALSE(graph.Plan());
    EXPECT_NE(logged.find("Missing"), std::string::npos);
}

TEST_F(RenderGraphTest, IgnoresUnresolvedReadsOfPassesThatGetCulled)
{
    /// @note 申告を契約にするのは «実際に実行されるパス» だけ。刈られるパスの申告漏れで
    ///       Plan 全体を落とすと、設定を 1 つ切った途端に画面が出なくなる。
    RG graph;
    graph.DeclareResource("HDR", TransientRT());
    graph.DeclareResource("Missing", TransientRT());
    graph.DeclareResource("Output", ImportedRT());
    AddPass(graph, "Geometry", {}, { "HDR" });
    AddPass(graph, "Orphan", { "Missing" }, { "Unused" });
    AddPass(graph, "Present", { "HDR" }, { "Output" });
    graph.SetOutputs({ "Output" });

    ASSERT_TRUE(graph.Plan());

    EXPECT_EQ(CulledNames(graph), (std::vector<std::string>{ "Orphan" }));
}

TEST_F(RenderGraphTest, AllowsReadingAnImportedResourceWithoutAnyProducer)
{
    RG graph;
    graph.DeclareResource("History", ImportedRT());
    graph.DeclareResource("Output", ImportedRT());
    AddPass(graph, "Resolve", { "History" }, { "Output" });
    graph.SetOutputs({ "Output" });

    EXPECT_TRUE(graph.Plan());
}

TEST_F(RenderGraphTest, SchedulesProducerBeforeConsumerEvenWhenRegisteredLater)
{
    /// @note 登録順は正しいトポロジカル順でなくてよい。依存が順序を決める。
    RG graph;
    graph.DeclareResource("Shadow", TransientRT());
    graph.DeclareResource("Output", ImportedRT());
    AddPass(graph, "Present", { "Shadow" }, { "Output" });
    AddPass(graph, "ShadowMap", {}, { "Shadow" });
    graph.SetOutputs({ "Output" });

    ASSERT_TRUE(graph.Plan());

    EXPECT_EQ(OrderedNames(graph), (std::vector<std::string>{ "ShadowMap", "Present" }));
}

TEST_F(RenderGraphTest, CullsIndependentlyOfRegistrationOrder)
{
    /// @note 生存判定も «後ろから前へ 1 回» の走査ではなく、出力から辿れるかで決まる。
    ///       消費者より後ろに登録された生産者が刈られてはいけない。
    RG graph;
    graph.DeclareResource("HDR", TransientRT());
    graph.DeclareResource("Unused", TransientRT());
    graph.DeclareResource("Output", ImportedRT());
    AddPass(graph, "Present", { "HDR" }, { "Output" });
    AddPass(graph, "Orphan", {}, { "Unused" });
    AddPass(graph, "Geometry", {}, { "HDR" });
    graph.SetOutputs({ "Output" });

    ASSERT_TRUE(graph.Plan());

    EXPECT_EQ(CulledNames(graph), (std::vector<std::string>{ "Orphan" }));
    EXPECT_EQ(OrderedNames(graph), (std::vector<std::string>{ "Geometry", "Present" }));
}

TEST_F(RenderGraphTest, RejectsPlanWhenPassesFormACycle)
{
    /// @note 並べ替えができるようになった以上、循環は本当に作れる。
    ///       無限ループにならず false を返すこと。
    RG graph;
    graph.DeclareResource("Encoded", TransientRT());
    graph.DeclareResource("Decoded", TransientRT());
    graph.DeclareResource("Output", ImportedRT());
    AddPass(graph, "Encode", { "Decoded" }, { "Encoded" });
    AddPass(graph, "Decode", { "Encoded" }, { "Decoded" });
    AddPass(graph, "Present", { "Decoded" }, { "Output" });
    graph.SetOutputs({ "Output" });

    std::string logged;
    graph.SetDebugLogHook([&logged](const char* message) { logged = message; });

    EXPECT_FALSE(graph.Plan());
    EXPECT_NE(logged.find("cycle"), std::string::npos);
}

TEST_F(RenderGraphTest, LinearisesMutuallyDependentPassesByWriteGeneration)
{
    /// @note 相互参照«に見える»構成でも循環にはならない。依存辺は必ず自分より前に登録された
    ///       パスへ向かうので、A→B と B→A が同時に張られることが構造的に起きないため。
    ///       「循環でグラフが止まる」を心配しなくてよい根拠をここに固定する。
    RG graph;
    graph.DeclareResource("A", TransientRT());
    graph.DeclareResource("B", TransientRT());
    graph.DeclareResource("Output", ImportedRT());
    AddPass(graph, "Seed", {}, { "A", "B" });
    AddPass(graph, "First", { "A" }, { "B" });
    AddPass(graph, "Second", { "B" }, { "A" });
    AddPass(graph, "Present", { "A" }, { "Output" });
    graph.SetOutputs({ "Output" });

    ASSERT_TRUE(graph.Plan());

    EXPECT_EQ(OrderedNames(graph),
              (std::vector<std::string>{ "Seed", "First", "Second", "Present" }));
}

/// @name 並べ替えの方針

namespace {

/// 独立した 2 本の鎖が最後に合流する形。依存は «MakeX より後に UseX» としか
/// 言っていないので、2 本をどう混ぜるかはスケジューラーの自由になる。
void BuildTwoIndependentChains(RG& graph)
{
    graph.DeclareResource("A", TransientRT());
    graph.DeclareResource("B", TransientRT());
    graph.DeclareResource("Out1", TransientRT());
    graph.DeclareResource("Out2", TransientRT());
    graph.DeclareResource("Output", ImportedRT());
    AddPass(graph, "MakeA", {}, { "A" });
    AddPass(graph, "MakeB", {}, { "B" });
    AddPass(graph, "UseA", { "A" }, { "Out1" });
    AddPass(graph, "UseB", { "B" }, { "Out2" });
    AddPass(graph, "Combine", { "Out1", "Out2" }, { "Output" });
    graph.SetOutputs({ "Output" });
}

} // namespace

TEST_F(RenderGraphTest, FallsBackToRegistrationOrderWhenDependenciesLeaveTheChoiceOpen)
{
    RG graph;
    BuildTwoIndependentChains(graph);

    ASSERT_TRUE(graph.Plan());

    EXPECT_EQ(graph.GetSchedulePolicy(), RG::SchedulePolicy::RegistrationOrder);
    EXPECT_EQ(OrderedNames(graph),
              (std::vector<std::string>{ "MakeA", "MakeB", "UseA", "UseB", "Combine" }));
}

TEST_F(RenderGraphTest, InterleavesChainsToShortenLifetimesUnderMinimizeLifetimes)
{
    /// @note A は UseA で死ぬ。先に UseA まで走らせてしまえば、B を作る時点で A は既に居ない。
    ///       登録順のままだと A と B が同時に生きる区間ができる。
    RG graph;
    BuildTwoIndependentChains(graph);
    graph.SetSchedulePolicy(RG::SchedulePolicy::MinimizeLifetimes);

    ASSERT_TRUE(graph.Plan());

    EXPECT_EQ(OrderedNames(graph),
              (std::vector<std::string>{ "MakeA", "UseA", "MakeB", "UseB", "Combine" }));
}

TEST_F(RenderGraphTest, KeepsWriteGenerationOrderRegardlessOfPolicy)
{
    /// @note 同じリソースへの書き込み順は «意図» なので、どの方針でも動かしてはいけない。
    ///       Sky は Geometry の上に描く。入れ替わったら絵が変わる。
    const auto orderUnder = [](RG::SchedulePolicy policy) {
        RG graph;
        graph.DeclareResource("HDR", TransientRT());
        graph.DeclareResource("Side", TransientRT());
        graph.DeclareResource("Output", ImportedRT());
        AddPass(graph, "Geometry", {}, { "HDR" });
        AddPass(graph, "MakeSide", {}, { "Side" });
        AddPass(graph, "Sky", { "HDR" }, { "HDR" });
        AddPass(graph, "Composite", { "HDR", "Side" }, { "Output" });
        graph.SetOutputs({ "Output" });
        graph.SetSchedulePolicy(policy);
        EXPECT_TRUE(graph.Plan());
        return OrderedNames(graph);
    };

    for (const RG::SchedulePolicy policy : { RG::SchedulePolicy::RegistrationOrder,
                                             RG::SchedulePolicy::MinimizeLifetimes }) {
        const std::vector<std::string> order = orderUnder(policy);
        const auto indexOf = [&order](std::string_view name) {
            return std::find(order.begin(), order.end(), name) - order.begin();
        };
        EXPECT_LT(indexOf("Geometry"), indexOf("Sky"));
    }
}

TEST_F(RenderGraphTest, AgreesBetweenPoliciesWhenTheOrderIsFullyConstrained)
{
    /// @note 依存が一本道なら方針は結果に影響しない。
    const auto orderUnder = [](RG::SchedulePolicy policy) {
        RG graph;
        graph.DeclareResource("A", TransientRT());
        graph.DeclareResource("B", TransientRT());
        graph.DeclareResource("Output", ImportedRT());
        AddPass(graph, "First", {}, { "A" });
        AddPass(graph, "Second", { "A" }, { "B" });
        AddPass(graph, "Third", { "B" }, { "Output" });
        graph.SetOutputs({ "Output" });
        graph.SetSchedulePolicy(policy);
        EXPECT_TRUE(graph.Plan());
        return OrderedNames(graph);
    };

    EXPECT_EQ(orderUnder(RG::SchedulePolicy::RegistrationOrder),
              orderUnder(RG::SchedulePolicy::MinimizeLifetimes));
}

/// @name 構成テキスト

TEST_F(RenderGraphTest, DescribesExecutionOrderCulledPassesAndLifetimes)
{
    RG graph;
    graph.DeclareResource("HDR", TransientRT(1920, 1080));
    graph.DeclareResource("Unused", TransientRT());
    graph.DeclareResource("Output", ImportedRT());
    AddPass(graph, "Geometry", {}, { "HDR" });
    AddPass(graph, "Orphan", {}, { "Unused" });
    AddPass(graph, "Present", { "HDR" }, { "Output" });
    graph.SetOutputs({ "Output" });

    ASSERT_TRUE(graph.Plan());
    const std::string described = graph.DescribeLastPlan();

    EXPECT_NE(described.find("policy RegistrationOrder"), std::string::npos);
    EXPECT_NE(described.find("pass 0 Geometry"), std::string::npos);
    EXPECT_NE(described.find("pass 1 Present"), std::string::npos);
    EXPECT_NE(described.find("culled Orphan"), std::string::npos);
    EXPECT_NE(described.find("res HDR first=0 last=1 alias=0 kind=RenderTarget size=1920x1080"),
              std::string::npos);
    EXPECT_NE(described.find("res Output first=1 last=1 alias=-1"), std::string::npos);
}

TEST_F(RenderGraphTest, OmitsTimingFromTheDescriptionSoDiffsStayStable)
{
    /// @note 計測値が混ざるとフレームごとに差分が出て、順序の変化が埋もれる。
    RG graph;
    graph.DeclareResource("HDR", TransientRT());
    graph.DeclareResource("Output", ImportedRT());
    AddPass(graph, "Geometry", {}, { "HDR" });
    AddPass(graph, "Present", { "HDR" }, { "Output" });
    graph.SetOutputs({ "Output" });

    ASSERT_TRUE(graph.Execute());
    ASSERT_FALSE(graph.GetLastReport().profiles.empty());
    const std::string first = graph.DescribeLastPlan();
    ASSERT_TRUE(graph.Execute());
    const std::string second = graph.DescribeLastPlan();

    /// @note 2 回実行すれば計測値は必ず違う。それでもテキストが一致することが «入っていない» 証拠。
    EXPECT_EQ(first, second);
}

/// @name Execute

TEST_F(RenderGraphTest, InvokesCallbacksOfLivePassesOnlyAndInExecutionOrder)
{
    RG graph;
    graph.DeclareResource("HDR", TransientRT());
    graph.DeclareResource("Unused", TransientRT());
    graph.DeclareResource("Output", ImportedRT());

    std::vector<std::string> invoked;
    graph.AddPass("Geometry", {}, { "HDR" }, [&invoked] { invoked.push_back("Geometry"); });
    graph.AddPass("Orphan", {}, { "Unused" }, [&invoked] { invoked.push_back("Orphan"); });
    graph.AddPass("Present", { "HDR" }, { "Output" }, [&invoked] { invoked.push_back("Present"); });
    graph.SetOutputs({ "Output" });

    ASSERT_TRUE(graph.Execute());

    EXPECT_EQ(invoked, (std::vector<std::string>{ "Geometry", "Present" }));
}

TEST_F(RenderGraphTest, ReusesAnInjectedPlanInsteadOfReplanning)
{
    /// @note RenderPipeline がトポロジ不変フレームで Plan をスキップする経路。
    ///       注入したプランがそのまま実行順として使われること。
    RG source;
    source.DeclareResource("HDR", TransientRT());
    source.DeclareResource("Output", ImportedRT());
    AddPass(source, "Geometry", {}, { "HDR" });
    AddPass(source, "Present", { "HDR" }, { "Output" });
    source.SetOutputs({ "Output" });
    ASSERT_TRUE(source.Plan());

    RG graph;
    graph.DeclareResource("HDR", TransientRT());
    graph.DeclareResource("Output", ImportedRT());
    std::vector<std::string> invoked;
    graph.AddPass("Geometry", {}, { "HDR" }, [&invoked] { invoked.push_back("Geometry"); });
    graph.AddPass("Present", { "HDR" }, { "Output" }, [&invoked] { invoked.push_back("Present"); });
    graph.InjectPlan(source.GetLastReport());

    ASSERT_TRUE(graph.Execute());

    EXPECT_EQ(invoked, (std::vector<std::string>{ "Geometry", "Present" }));
}

TEST_F(RenderGraphTest, ClearsProfilesWhenAPlanIsInjected)
{
    /// @note profiles は Execute が毎回書き直す。注入元の計測値が混ざると
    ///       AnalysisPanel が «実行していないフレームの時間» を表示する。
    RG graph;
    graph.DeclareResource("HDR", TransientRT());
    graph.DeclareResource("Output", ImportedRT());
    AddPass(graph, "Geometry", {}, { "HDR" });
    AddPass(graph, "Present", { "HDR" }, { "Output" });
    graph.SetOutputs({ "Output" });
    ASSERT_TRUE(graph.Execute());
    ASSERT_FALSE(graph.GetLastReport().profiles.empty());

    const RG::ExecutionReport planned = graph.GetLastReport();
    graph.InjectPlan(planned);

    EXPECT_TRUE(graph.GetLastReport().profiles.empty());
}

} // namespace fbzz::tests
