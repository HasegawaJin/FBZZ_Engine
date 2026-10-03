/// @file    PixEventStackTests.cpp
/// @brief   PIX logical scope pairing across command-list splits without a GPU or runtime.
/// @author  Hasegawa Jin
/// @date    2026-10-02
#include <TestKit/TestKit.hpp>
#include "../../../Graphics/src/Renderer/Platform/DX12/DX12PixEventStack.hpp"
#include <string>
#include <vector>

namespace fbzz::tests {
namespace {

struct PixEmission {
    bool begin = false;
    uint64_t color = 0;
    std::string name;
    bool operator==(const PixEmission&) const = default;
};

class PixEventStackTest : public testkit::Fixture {
protected:
    renderer::DX12PixEventStack m_stack;
    std::vector<PixEmission> m_emissions;

    auto Emit()
    {
        return [this](bool begin, uint64_t color, const std::string& name) {
            m_emissions.push_back({begin, color, name});
        };
    }

    uint64_t Begin(const std::string& name, uint64_t color = 1)
    {
        return m_stack.Begin(name, color, Emit());
    }

    bool End(uint64_t token) { return m_stack.End(token, Emit()); }
};

TEST_F(PixEventStackTest, NeverEmitsBeforeARecordingListSuccessfullyOpens)
{
    EXPECT_EQ(Begin("Rejected"), 0u);
    EXPECT_FALSE(End(0));
    EXPECT_FALSE(m_stack.IsRecording());
    EXPECT_EQ(m_stack.GetDepth(), 0u);
    EXPECT_TRUE(m_emissions.empty());
    m_stack.Resume(Emit());
    const auto token = Begin("Actual");
    EXPECT_NE(token, 0u);
    EXPECT_TRUE(End(token));
    EXPECT_EQ(m_emissions, (std::vector<PixEmission>{{true, 1, "Actual"}, {false, 1, "Actual"}}));
}

TEST_F(PixEventStackTest, SplitEndsInnerFirstAndReopensOuterFirstWithOwnedNames)
{
    m_stack.Resume(Emit());
    const auto frame = Begin("Frame", 11);
    const auto view = Begin("View=7", 12);
    std::string name = "RayReflection %s";
    const auto pass = Begin(name, 13);
    name = "changed caller memory";
    m_stack.Suspend(Emit());
    m_stack.Suspend(Emit());
    EXPECT_FALSE(m_stack.IsRecording());
    EXPECT_EQ(m_stack.GetDepth(), 3u);
    m_stack.Resume(Emit());
    m_stack.Resume(Emit());
    EXPECT_TRUE(End(pass));
    EXPECT_TRUE(End(view));
    EXPECT_TRUE(End(frame));
    EXPECT_EQ(m_emissions, (std::vector<PixEmission>{
        {true, 11, "Frame"}, {true, 12, "View=7"}, {true, 13, "RayReflection %s"},
        {false, 13, "RayReflection %s"}, {false, 12, "View=7"}, {false, 11, "Frame"},
        {true, 11, "Frame"}, {true, 12, "View=7"}, {true, 13, "RayReflection %s"},
        {false, 13, "RayReflection %s"}, {false, 12, "View=7"}, {false, 11, "Frame"}}));
    EXPECT_EQ(m_stack.GetDepth(), 0u);
}

TEST_F(PixEventStackTest, FailedCloseOrResetDiscardsWithoutReopeningRetiredScopes)
{
    m_stack.Resume(Emit());
    const auto old = Begin("Old");
    m_stack.Suspend(Emit());
    m_stack.Discard(Emit());
    EXPECT_EQ(m_emissions.size(), 2u);
    EXPECT_EQ(m_stack.GetDepth(), 0u);
    EXPECT_EQ(Begin("Failed list"), 0u);
    EXPECT_FALSE(End(old));
    m_stack.Resume(Emit());
    const auto next = Begin("Next");
    EXPECT_NE(old, next);
    EXPECT_FALSE(End(old));
    EXPECT_EQ(m_stack.GetDepth(), 1u);
    EXPECT_TRUE(End(next));
    EXPECT_EQ(m_emissions.back(), (PixEmission{false, 1, "Next"}));
}

TEST_F(PixEventStackTest, FrameEndAndShutdownBalanceEveryEmittedScopeOnce)
{
    m_stack.Resume(Emit());
    const auto outer = Begin("Outer", 2);
    const auto inner = Begin("Inner", 3);
    m_stack.Discard(Emit());
    m_stack.Discard(Emit());
    EXPECT_EQ(m_emissions, (std::vector<PixEmission>{
        {true, 2, "Outer"}, {true, 3, "Inner"}, {false, 3, "Inner"}, {false, 2, "Outer"}}));
    EXPECT_FALSE(m_stack.IsRecording());
    EXPECT_EQ(m_stack.GetDepth(), 0u);
    EXPECT_FALSE(End(inner));
    EXPECT_FALSE(End(outer));
}

TEST_F(PixEventStackTest, OutOfOrderOrUnmatchedEndsDoNotPopAnUnrelatedScope)
{
    m_stack.Resume(Emit());
    const auto outer = Begin("Outer");
    const auto inner = Begin("Inner");
    EXPECT_FALSE(End(outer));
    EXPECT_FALSE(End(inner + 1));
    EXPECT_EQ(m_emissions.size(), 2u);
    EXPECT_EQ(m_stack.GetDepth(), 2u);
    EXPECT_TRUE(End(inner));
    EXPECT_FALSE(End(inner));
    EXPECT_TRUE(End(outer));
    EXPECT_EQ(m_emissions.size(), 4u);
}

TEST_F(PixEventStackTest, SuspendedScopeCanRetireBeforeItsListReopens)
{
    m_stack.Resume(Emit());
    const auto outer = Begin("Outer");
    const auto inner = Begin("Inner");
    m_stack.Suspend(Emit());
    EXPECT_TRUE(End(inner));
    EXPECT_EQ(m_emissions.size(), 4u);
    m_stack.Resume(Emit());
    EXPECT_EQ(m_emissions.size(), 5u);
    EXPECT_EQ(m_emissions.back(), (PixEmission{true, 1, "Outer"}));
    EXPECT_TRUE(End(outer));
}

TEST_F(PixEventStackTest, EventRecordingHasNoTimestampQueryBudget)
{
    m_stack.Resume(Emit());
    const auto view = Begin("View");
    for (uint32_t i = 0; i < 256; ++i) {
        const auto pass = Begin("Pass=" + std::to_string(i));
        ASSERT_NE(pass, 0u);
        ASSERT_TRUE(End(pass));
        ASSERT_EQ(m_stack.GetDepth(), 1u);
    }
    EXPECT_TRUE(End(view));
    EXPECT_EQ(m_emissions.size(), 514u);
}

TEST_F(PixEventStackTest, ComputeListRetirementDoesNotAlterTheGraphicsListStack)
{
    renderer::DX12PixEventStack compute;
    std::vector<PixEmission> computeEmissions;
    const auto emitCompute = [&](bool begin, uint64_t color, const std::string& name) {
        computeEmissions.push_back({begin, color, name});
    };
    m_stack.Resume(Emit());
    const auto frame = Begin("GraphicsFrame");
    m_stack.Suspend(Emit());
    m_stack.Resume(Emit());
    compute.Resume(emitCompute);
    const auto dispatch = compute.Begin("LightProbeSH", 4, emitCompute);
    compute.Discard(emitCompute);
    EXPECT_FALSE(compute.End(dispatch, emitCompute));
    EXPECT_EQ(m_stack.GetDepth(), 1u);
    EXPECT_TRUE(m_stack.IsRecording());
    EXPECT_TRUE(End(frame));
    EXPECT_EQ(computeEmissions, (std::vector<PixEmission>{
        {true, 4, "LightProbeSH"}, {false, 4, "LightProbeSH"}}));
    EXPECT_EQ(m_emissions.size(), 4u);
}

} /// @note namespace
} /// @note namespace fbzz::tests
