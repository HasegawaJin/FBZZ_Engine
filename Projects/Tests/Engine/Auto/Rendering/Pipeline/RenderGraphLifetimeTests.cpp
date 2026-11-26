/// @file    RenderGraphLifetimeTests.cpp
/// @brief   リソース寿命の算出とエイリアスグループの割り当て規則を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-10
/// @note 寿命が重ならない同型資源だけを同じ alias group に入れる。
/// @note lastPass == firstPass の資源は同じパスで生存しているため共有できない。
#include <TestKit/TestKit.hpp>

#include <Graphics/RenderGraph/RenderGraph.hpp>

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

/// @note 永続資源は書き手なしで読めるが alias しない。
RG::ResourceDesc PersistentRT(uint32_t width = 1920, uint32_t height = 1080)
{
    return RG::ResourceDesc{ RG::ResourceKind::RenderTarget, width, height,
                             renderer::Format::RGBA16F, 1, true, false, false };
}

/// @note colorCount == 0 は深度専用 RT。
RG::ResourceDesc TransientDepthOnlyRT(uint32_t width = 1920, uint32_t height = 1080)
{
    return RG::ResourceDesc{ RG::ResourceKind::RenderTarget, width, height,
                             renderer::Format::RGBA16F, 0, true, false, true };
}

/// @note 深度を持たないカラー RT。
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

} /// @note namespace

class RenderGraphLifetimeTest : public testkit::Fixture {};

/// @name 寿命

/// @brief Reports lifetime bounds as execution order indices.
TEST_F(RenderGraphLifetimeTest, ReportsLifetimeBoundsAsExecutionOrderIndices)
{
    // firstPass / lastPass はカリング後の実行順の番号。
    RG graph;
    graph.DeclareResource("HDR", TransientRT());
    graph.DeclareResource("Unused", TransientRT());
    graph.DeclareResource("Output", ImportedRT());
    AddPass(graph, "Geometry", {}, { "HDR" });
    AddPass(graph, "Orphan", {}, { "Unused" });
    AddPass(graph, "Present", { "HDR" }, { "Output" });
    graph.SetOutputs({ "Output" });

    ASSERT_TRUE(graph.Plan());

    // Orphan が刈られるので、Present の実行順番号は登録番号の 2 ではなく 1。
    const RG::ResourceLifetime* hdr = Find(graph, "HDR");
    ASSERT_NE(hdr, nullptr);
    EXPECT_EQ(hdr->firstPass, 0);
    EXPECT_EQ(hdr->lastPass, 1);
}

/// @brief Omits resources touched only by culled passes.
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

/// @brief Sorts lifetimes by first use then name.
TEST_F(RenderGraphLifetimeTest, SortsLifetimesByFirstUseThenName)
{
    // 構成が同じときに異なる資源へ alias group が配られないよう順序を固定する。
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

/// @brief Shares one group between resources whose lifetimes do not overlap.
TEST_F(RenderGraphLifetimeTest, SharesOneGroupBetweenResourcesWhoseLifetimesDoNotOverlap)
{
    // Early は Middle で死に、Late は MakeLate で生まれる。間が 1 段空くので貸し回せる。
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

/// @brief Separates groups for resources alive at the same time.
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

/// @brief Refuses to share a group when descriptors differ.
TEST_F(RenderGraphLifetimeTest, RefusesToShareAGroupWhenDescriptorsDiffer)
{
    // 寿命が重ならなくても寸法が違う資源は共有できない。
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
    // 枠は空いている
    EXPECT_LT(full->lastPass, half->firstPass);
    // それでも共有しない
    EXPECT_NE(full->aliasGroup, half->aliasGroup);
}

/// @brief Refuses to share a group between depth only and colour targets.
TEST_F(RenderGraphLifetimeTest, RefusesToShareAGroupBetweenDepthOnlyAndColourTargets)
{
    // 寸法と形式が一致しても colorCount の違う資源は共有できない。
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
    // 枠は空いている
    EXPECT_LT(colour->lastPass, depth->firstPass);
    // それでも共有しない
    EXPECT_NE(colour->aliasGroup, depth->aliasGroup);
}

/// @brief Refuses to share a group when only the depth flag differs.
TEST_F(RenderGraphLifetimeTest, RefusesToShareAGroupWhenOnlyTheDepthFlagDiffers)
{
    // colorCount も形式も同じで、深度を持つかだけが違う。
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

/// @brief Excludes imported and persistent resources from aliasing.
TEST_F(RenderGraphLifetimeTest, ExcludesImportedAndPersistentResourcesFromAliasing)
{
    // 外部資源と履歴の aliasGroup は -1。
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

/// @brief Shares a group between undeclared resources because they get the default desc.
TEST_F(RenderGraphLifetimeTest, SharesAGroupBetweenUndeclaredResourcesBecauseTheyGetTheDefaultDesc)
{
    // 未宣言資源は既定の Unknown 記述となり、現状では互いに alias できてしまう。
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

/// @brief Buffer aliasing requires the same byte size and stride.
TEST_F(RenderGraphLifetimeTest, BufferAliasingRequiresTheSameByteSizeAndStride)
{
    RG::ResourceDesc first;
    first.kind = RG::ResourceKind::Buffer;
    first.byteSize = 4096;
    first.stride = 32;
    for (const auto [byteSize, stride] : {
            std::pair<uint64_t, uint32_t>{ 4096, 32 },
            std::pair<uint64_t, uint32_t>{ 8192, 32 },
            std::pair<uint64_t, uint32_t>{ 4096, 64 } }) {
        RG graph;
        auto second = first;
        second.byteSize = byteSize;
        second.stride = stride;
        graph.DeclareResource("Early", first);
        graph.DeclareResource("Mid", TransientRT());
        graph.DeclareResource("Late", second);
        graph.DeclareResource("Output", ImportedRT());
        AddPass(graph, "MakeEarly", {}, { "Early" });
        AddPass(graph, "Middle", { "Early" }, { "Mid" });
        AddPass(graph, "MakeLate", { "Mid" }, { "Late" });
        AddPass(graph, "Present", { "Late" }, { "Output" });
        graph.SetOutputs({ "Output" });

        ASSERT_TRUE(graph.Plan());
        const auto* early = Find(graph, "Early");
        const auto* late = Find(graph, "Late");
        ASSERT_NE(early, nullptr);
        ASSERT_NE(late, nullptr);
        EXPECT_LT(early->lastPass, late->firstPass);
        if (byteSize == first.byteSize && stride == first.stride)
            EXPECT_EQ(early->aliasGroup, late->aliasGroup);
        else
            EXPECT_NE(early->aliasGroup, late->aliasGroup);
    }
}

/// @brief Explicit no alias resources have no alias group.
TEST_F(RenderGraphLifetimeTest, ExplicitNoAliasResourcesHaveNoAliasGroup)
{
    RG graph;
    auto noAlias = TransientRT();
    noAlias.allowAliasing = false;
    graph.DeclareResource("History", noAlias);
    graph.DeclareResource("Output", ImportedRT());
    AddPass(graph, "MakeHistory", {}, { "History" });
    AddPass(graph, "Present", { "History" }, { "Output" });
    graph.SetOutputs({ "Output" });

    ASSERT_TRUE(graph.Plan());
    const auto* history = Find(graph, "History");
    ASSERT_NE(history, nullptr);
    EXPECT_EQ(history->aliasGroup, -1);
    EXPECT_FALSE(history->desc.allowAliasing);
}

/// @brief Acceleration structures cannot alias even when requested.
TEST_F(RenderGraphLifetimeTest, AccelerationStructuresCannotAliasEvenWhenRequested)
{
    RG graph;
    RG::ResourceDesc accelerationStructure;
    accelerationStructure.kind = RG::ResourceKind::AccelerationStructure;
    accelerationStructure.byteSize = 4096;
    accelerationStructure.allowAliasing = true;
    graph.DeclareResource("BLAS", accelerationStructure);
    graph.DeclareResource("TLAS", accelerationStructure);
    graph.DeclareResource("Output", ImportedRT());
    AddPass(graph, "BuildBLAS", {}, { "BLAS" });
    AddPass(graph, "BuildTLAS", { "BLAS" }, { "TLAS" });
    AddPass(graph, "Trace", { "TLAS" }, { "Output" });
    graph.SetOutputs({ "Output" });

    ASSERT_TRUE(graph.Plan());
    for (const auto name : { "BLAS", "TLAS" }) {
        const auto* resource = Find(graph, name);
        ASSERT_NE(resource, nullptr);
        EXPECT_EQ(resource->aliasGroup, -1);
        EXPECT_FALSE(resource->desc.allowAliasing);
    }
}

/// @brief Ray preparation and trace follow declared dependencies.
TEST_F(RenderGraphLifetimeTest, RayPreparationAndTraceFollowDeclaredDependencies)
{
    RG graph;
    RG::ResourceDesc vertices;
    vertices.kind = RG::ResourceKind::Buffer;
    vertices.byteSize = 4096;
    RG::ResourceDesc accelerationStructure;
    accelerationStructure.kind = RG::ResourceKind::AccelerationStructure;
    graph.DeclareResource("Vertices", vertices);
    graph.DeclareResource("BLAS", accelerationStructure);
    graph.DeclareResource("TLAS", accelerationStructure);
    graph.DeclareResource("Output", ImportedRT());
    graph.AddPass("Trace", {
        { "TLAS", RG::ResourceUsage::Read, RG::ResourceAccessPurpose::TRACE_READ },
        { "Vertices", RG::ResourceUsage::Read, RG::ResourceAccessPurpose::SHADER_READ },
        { "Output", RG::ResourceUsage::Write, RG::ResourceAccessPurpose::UAV }
    }, [] {});
    graph.AddPass("BuildTLAS", {
        { "BLAS", RG::ResourceUsage::Read, RG::ResourceAccessPurpose::BUILD_INPUT },
        { "TLAS", RG::ResourceUsage::Write, RG::ResourceAccessPurpose::AS_WRITE }
    }, [] {});
    graph.AddPass("BuildBLAS", {
        { "Vertices", RG::ResourceUsage::Read, RG::ResourceAccessPurpose::BUILD_INPUT },
        { "BLAS", RG::ResourceUsage::Write, RG::ResourceAccessPurpose::AS_WRITE }
    }, [] {});
    graph.AddPass("Skinning", {
        { "Vertices", RG::ResourceUsage::Write, RG::ResourceAccessPurpose::UAV }
    }, [] {});
    graph.SetOutputs({ "Output" });

    ASSERT_TRUE(graph.Plan());
    EXPECT_EQ(graph.GetLastReport().executionOrder, (std::vector<size_t>{ 3, 2, 1, 0 }));
}

/// @brief Preparation output keeps its producers without a view graph.
TEST_F(RenderGraphLifetimeTest, PreparationOutputKeepsItsProducersWithoutAViewGraph)
{
    RG graph;
    RG::ResourceDesc accelerationStructure;
    accelerationStructure.kind = RG::ResourceKind::AccelerationStructure;
    graph.DeclareResource("BLAS", accelerationStructure);
    graph.DeclareResource("TLAS", accelerationStructure);
    AddPass(graph, "BuildTLAS", { "BLAS" }, { "TLAS" });
    AddPass(graph, "BuildBLAS", {}, { "BLAS" });
    AddPass(graph, "Unused", {}, { "UnusedTable" });
    graph.SetOutputs({ "TLAS" });

    ASSERT_TRUE(graph.Plan());
    EXPECT_EQ(graph.GetLastReport().executionOrder, (std::vector<size_t>{ 1, 0 }));
    EXPECT_EQ(graph.GetLastReport().culledPasses, (std::vector<size_t>{ 2 }));
}

/// @brief Assigns identical alias groups across repeated plans.
TEST_F(RenderGraphLifetimeTest, AssignsIdenticalAliasGroupsAcrossRepeatedPlans)
{
    // unordered_map の走査順が alias group の割り当てを変えてはならない。
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

} /// @note namespace fbzz::tests
