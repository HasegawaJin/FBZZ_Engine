/// @file    DX12UploadArenaSizingTests.cpp
/// @brief   Exact bounded DX12 upload sizing tokens and safe fallback behavior.
/// @author  Hasegawa Jin
/// @date    2026-10-23
#include <TestKit/TestKit.hpp>
#include "../../../Graphics/src/Renderer/Platform/DX12/DX12UploadArenaSizing.hpp"
#include <array>
#include <string_view>

namespace fbzz::tests {
namespace {
using namespace renderer::upload_arena_detail;

class DX12UploadArenaSizingTest : public testkit::Fixture {};
} /// @note namespace

TEST_F(DX12UploadArenaSizingTest, UnsetPreservesDefaultWithoutWarning)
{
    constexpr auto sizing = ParseUploadArenaMiB({}, false);
    static_assert(sizing.bytesPerFrame == 16u * MIB && sizing.valid && !sizing.specified);
    EXPECT_EQ(sizing.bytesPerFrame, 16u * MIB);
    EXPECT_TRUE(sizing.valid);
    EXPECT_FALSE(sizing.specified);
}

TEST_F(DX12UploadArenaSizingTest, AcceptsOnlyFourBoundedCapacities)
{
    constexpr std::array<std::string_view, 4> tokens{"16", "32", "64", "128"};
    constexpr std::array<size_t, 4> capacities{16u, 32u, 64u, 128u};
    for (size_t index = 0; index < tokens.size(); ++index) {
        SCOPED_TRACE(tokens[index]);
        const auto sizing = ParseUploadArenaMiB(tokens[index], true);
        EXPECT_EQ(sizing.bytesPerFrame, capacities[index] * MIB);
        EXPECT_TRUE(sizing.valid);
        EXPECT_TRUE(sizing.specified);
    }
}

TEST_F(DX12UploadArenaSizingTest, InvalidExplicitTokensFallBackTo16MiB)
{
    constexpr std::array<std::string_view, 20> tokens{
        "", "0", "1", "15", "17", "256", "999999999999999999999999999999999999",
        "18446744073709551616", "+64", "-64", " 64", "64 ", "64\n", "\t64",
        "64MiB", "64MB", "67108864", "064", "64.0", "0x40"};
    for (const auto token : tokens) {
        SCOPED_TRACE(token);
        const auto sizing = ParseUploadArenaMiB(token, true);
        EXPECT_EQ(sizing.bytesPerFrame, 16u * MIB);
        EXPECT_FALSE(sizing.valid);
        EXPECT_TRUE(sizing.specified);
    }
}

} /// @note namespace fbzz::tests
