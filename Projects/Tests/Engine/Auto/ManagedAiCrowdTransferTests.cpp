/// @file    ManagedAiCrowdTransferTests.cpp
/// @brief   Real kernel packet parity and atomic rejection at the managed crowd transfer boundary.
/// @author  Hasegawa Jin
/// @date    2026-10-11
#include <TestKit/Fixture.hpp>
#include "../../../Engine/src/Scene/Managed/ManagedAiCrowdTransfer.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <vector>

namespace fbzz::tests {
class ManagedAiCrowdTransferTest : public testkit::Fixture {
protected:
    scene::managed_detail::ManagedAiCrowdTransfer transfer;
    ai::CrowdSettings settings;
    std::vector<ai::CrowdProfile> profiles{ai::CrowdProfile{}};
    std::vector<ai::CrowdAgent> agents;
    std::vector<ai::CrowdThreat> threats;
    std::vector<ai::CrowdObstacle> obstacles;
    std::vector<char> input, output;
    scene::ManagedBridgeRequest request;
    scene::ManagedBridgeReply reply;

    template<typename T> void Append(const T& value) {
        const std::size_t offset = input.size(); input.resize(offset + sizeof(T));
        std::memcpy(input.data() + offset, &value, sizeof(T));
    }
    void Pack(std::size_t count = 3) {
        settings.minimum = {-100, -100, -100}; settings.maximum = {100, 100, 100}; settings.deltaSeconds = 1.0f / 60;
        agents.resize(count);
        for (std::size_t i = 0; i < count; ++i) {
            agents[i] = ai::CrowdAgent{};
            agents[i].position = {static_cast<float>(i % 10), -4, static_cast<float>((i / 10) % 100)};
            agents[i].goal = {15, -4, 15}; agents[i].floor = -100; agents[i].ceiling = 100; agents[i].seed = static_cast<std::uint32_t>(i + 1);
        }
        input.clear();
        const std::array<std::uint32_t, 12> header{1, 96, 64, 164, 28, 16, 44,
            static_cast<std::uint32_t>(agents.size()), static_cast<std::uint32_t>(profiles.size()),
            static_cast<std::uint32_t>(threats.size()), static_cast<std::uint32_t>(obstacles.size()), 0};
        Append(header); Append(settings);
        for (const auto& p : profiles) Append(p);
        for (const auto& a : agents) Append(a);
        for (const auto& t : threats) Append(t);
        for (const auto& o : obstacles) Append(o);
        output.assign(sizeof(ai::CrowdStats) + agents.size() * sizeof(ai::CrowdAgent), '\x5a');
        request = {}; request.op = static_cast<scene::ManagedBridgeOp>(340);
        request.input = input.data(); request.inputBytes = static_cast<std::uint32_t>(input.size());
        request.output = output.data(); request.outputCapacity = static_cast<std::uint32_t>(output.size()); reply = {};
    }
    void Header(std::size_t index, std::uint32_t value) { std::memcpy(input.data() + index * 4, &value, 4); }
    void ExpectInvalid() {
        const auto before = output;
        EXPECT_EQ(transfer.Run(request, reply), scene::ManagedBridgeStatus::INVALID_ARGUMENT);
        EXPECT_EQ(output, before); EXPECT_EQ(reply.index, 0U); EXPECT_EQ(reply.flags, 0U); EXPECT_EQ(reply.outputBytes, 0U);
    }
};

TEST_F(ManagedAiCrowdTransferTest, PacketOutputMatchesDirectKernelExactly) {
    Pack(1000);
    std::vector<ai::CrowdAgent> expected(agents.size()); ai::CrowdWorkspace workspace; ai::CrowdStats stats;
    ASSERT_TRUE(ai::StepCrowd(settings, profiles, agents, threats, obstacles, expected, stats, workspace));
    ASSERT_EQ(transfer.Run(request, reply), scene::ManagedBridgeStatus::OK);
    EXPECT_EQ(reply.index, 1000U); EXPECT_EQ(reply.outputBytes, output.size());
    EXPECT_EQ(std::memcmp(output.data(), &stats, sizeof(stats)), 0);
    EXPECT_EQ(std::memcmp(output.data() + sizeof(stats), expected.data(), expected.size() * sizeof(ai::CrowdAgent)), 0);
    const auto first = output;
    ASSERT_EQ(transfer.Run(request, reply), scene::ManagedBridgeStatus::OK);
    EXPECT_EQ(first, output);
}
TEST_F(ManagedAiCrowdTransferTest, TooSmallOutputReportsExactSizeWithoutComputingOrWriting) {
    Pack(); const auto before = output; request.outputCapacity--;
    ASSERT_EQ(transfer.Run(request, reply), scene::ManagedBridgeStatus::BUFFER_TOO_SMALL);
    EXPECT_EQ(reply.outputBytes, output.size()); EXPECT_EQ(reply.index, 0U); EXPECT_EQ(output, before);
    request.outputCapacity = 0; request.output = nullptr;
    ASSERT_EQ(transfer.Run(request, reply), scene::ManagedBridgeStatus::BUFFER_TOO_SMALL);
    EXPECT_EQ(reply.outputBytes, output.size());
    request.output = output.data(); request.outputCapacity = static_cast<std::uint32_t>(output.size());
    EXPECT_EQ(transfer.Run(request, reply), scene::ManagedBridgeStatus::OK);
}
TEST_F(ManagedAiCrowdTransferTest, VersionSizesCountsAndReservedHeaderAreValidated) {
    for (std::size_t slot = 0; slot < 7; ++slot) { Pack(); Header(slot, 999); ExpectInvalid(); }
    Pack(); Header(7, UINT32_MAX); ExpectInvalid();
    Pack(); Header(8, 0); ExpectInvalid();
    Pack(); Header(8, 65); ExpectInvalid();
    Pack(); Header(9, 65); ExpectInvalid();
    Pack(); Header(10, 257); ExpectInvalid();
    Pack(); Header(11, 1); ExpectInvalid();
}
TEST_F(ManagedAiCrowdTransferTest, ExactLengthAndPointersAreValidated) {
    Pack(); request.inputBytes--; ExpectInvalid();
    Pack(); request.inputBytes++; ExpectInvalid();
    Pack(); request.inputBytes = 47; ExpectInvalid();
    Pack(); request.inputBytes = 1024 * 1024 + 1; ExpectInvalid();
    Pack(); request.input = nullptr; ExpectInvalid();
    Pack(); request.output = nullptr; ExpectInvalid();
    Pack(); request.outputCapacity = 1024 * 1024 + 1; ExpectInvalid();
}
TEST_F(ManagedAiCrowdTransferTest, UnsupportedRequestFieldsAndNonfiniteSettingsAreRejected) {
    Pack(); request.flags = 1; ExpectInvalid();
    Pack(); request.index = 1; ExpectInvalid();
    Pack(); request.integer = 1; ExpectInvalid();
    Pack(); request.reserved0 = 1; ExpectInvalid();
    Pack(); request.reserved1 = 1; ExpectInvalid();
    Pack(); request.x = std::numeric_limits<float>::quiet_NaN(); ExpectInvalid();
    Pack(); settings.deltaSeconds = std::numeric_limits<float>::quiet_NaN(); std::memcpy(input.data() + 48, &settings, sizeof(settings)); ExpectInvalid();
    Pack(); settings.reserved = 1; std::memcpy(input.data() + 48, &settings, sizeof(settings)); ExpectInvalid();
}
TEST_F(ManagedAiCrowdTransferTest, InvalidLaterAgentDoesNotPublishEarlierSlots) {
    Pack(); agents.back().profile = 64;
    const std::size_t offset = 48 + sizeof(settings) + profiles.size() * sizeof(ai::CrowdProfile);
    std::memcpy(input.data() + offset, agents.data(), agents.size() * sizeof(ai::CrowdAgent));
    ExpectInvalid();
    Pack(); EXPECT_EQ(transfer.Run(request, reply), scene::ManagedBridgeStatus::OK);
}
TEST_F(ManagedAiCrowdTransferTest, OverlappingInputAndOutputAreRejectedWithoutChangingInput) {
    Pack(); const auto before = input; request.output = input.data();
    EXPECT_EQ(transfer.Run(request, reply), scene::ManagedBridgeStatus::INVALID_ARGUMENT);
    EXPECT_EQ(input, before); EXPECT_EQ(reply.outputBytes, 0U);
}
TEST_F(ManagedAiCrowdTransferTest, EmptyAndMaximumPacketsSucceedWithinOneMiB) {
    Pack(0); ASSERT_EQ(transfer.Run(request, reply), scene::ManagedBridgeStatus::OK);
    EXPECT_EQ(reply.index, 0U); EXPECT_EQ(reply.outputBytes, 44U);
    profiles.resize(64); threats.resize(64); obstacles.resize(256); Pack(4096);
    EXPECT_EQ(input.size(), 681872U); EXPECT_EQ(output.size(), 671788U);
    ASSERT_EQ(transfer.Run(request, reply), scene::ManagedBridgeStatus::OK);
    EXPECT_EQ(reply.index, 4096U); EXPECT_EQ(reply.outputBytes, output.size());
}
}
